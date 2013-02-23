/*
 * Copyright (C) 2012 Luigi Rizzo. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * $Id: mlx4_netmap_linux.h $
 *
 * netmap support for mlx4 (LINUX version)
 *
 */


#include <bsd_glue.h>
#include <net/netmap.h>
#include <netmap/netmap_kern.h>
#define SOFTC_T	mlx4_en_priv

/*
 * This driver is split in multiple small files.
 * The main device descriptor has type struct mlx4_en_priv *priv;
 * and we attach to the device in mlx4_en_init_netdev()
 * (do port numbers start from 1 ?)
 *
 * The reconfig routine is in mlx4_en_start_port() (also here)
 * which is called on a mlx4_en_restart() (watchdog), open and set-mtu.
 *
 *      priv->num_frags                         ??
 *      DS_SIZE                                 ??
 *              apparently each rx desc is followed by frag.descriptors
 *              and the rx desc is rounded up to a power of 2.
 *
 *   Receive code is in en_rx.c
 *      priv->rx_ring_num                       number of rx rings
 *      rxr = prov->rx_ring[ring_ind]           rx ring descriptor
 *      rxr->size                               number of slots
 *      rxr->prod                               producer
 *         probably written into a mmio reg at *rxr->wqres.db.db
 *         trimmed to 16 bits.
 *
 *      Rx init routine:
 *              mlx4_en_activate_rx_rings()
 *                mlx4_en_init_rx_desc()
 *   Transmit code is in en_tx.c
 */

int mlx4_netmap_rx_config(struct SOFTC_T *priv, int ring_nr);
int mlx4_netmap_tx_config(struct SOFTC_T *priv, int ring_nr);

int mlx4_tx_desc_dump(struct mlx4_en_tx_desc *tx_desc);

#ifdef NETMAP_MLX4_MAIN
static inline void
nm_pkt_dump(int i, char *buf, int len)
{
    uint8_t *s = buf+6, *d = buf;
    RD(10, "%d len %4d %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x",
		i,
		len,
		s[0], s[1], s[2], s[3], s[4], s[5],
		d[0], d[1], d[2], d[3], d[4], d[5]);
}

/* show the content of the descriptor. Only the first block is printed
 * to make sure we do not fail on wraparounds (otherwise we would need
 * base, index and ring size).
 */
int
mlx4_tx_desc_dump(struct mlx4_en_tx_desc *tx_desc)
{
	struct mlx4_wqe_ctrl_seg *ctrl = &tx_desc->ctrl;
	uint32_t *p = (uint32_t *)tx_desc;
	int i, l = ctrl->fence_size;

	RD(5,"------- txdesc %p size 0x%x", tx_desc, ctrl->fence_size);
	if (l > 4)
		l = 4;
	for (i = 0; i < l; i++) {
		RD(20, "[%2d]: 0x%08x 0x%08x 0x%08x 0x%08x", i,
			ntohl(p[0]), ntohl(p[1]), ntohl(p[2]), ntohl(p[3]));
		p += 4;
	} 
	return 0;
}


/*
 * Register/unregister. We are already under (netmap) core lock.
 * Only called on the first register or the last unregister.
 */
static int
mlx4_netmap_reg(struct ifnet *ifp, int onoff)
{
	struct SOFTC_T *priv = netdev_priv(ifp);
	struct netmap_adapter *na = NA(ifp);
	int error = 0, need_load = 0;
	struct mlx4_en_dev *mdev = priv->mdev;

	if (na == NULL)
		return EINVAL;	/* no netmap support here */
	/*
	 * On enable, flush pending ops, set flag and reinit rings.
	 * On disable, flush again, and restart the interface.
	 */
	D("setting netmap mode for %s to %s", ifp->if_xname, onoff ? "ON" : "OFF");
	// rtnl_lock(); // ???
	if (netif_running(ifp)) {
		D("unloading %s", ifp->if_xname);
		//double_mutex_state_lock(mdev);
		mutex_lock(&mdev->state_lock);
		if (onoff == 0) {
			int i;
			/* coming from netmap mode, clean up the ring pointers
			 * so we do not crash in mlx4_en_free_tx_buf()
			 * XXX should STAMP the txdesc value to pretend the hw got there
			 * 0x7fffffff plus the bit set to
			 *	!!(ring->cons & ring->size)
			 */
			for (i = 0; i < na->num_tx_rings; i++) {
				struct mlx4_en_tx_ring *txr = &priv->tx_ring[i];
				ND("txr %d : cons %d prod %d txbb %d", i, txr->cons, txr->prod, txr->last_nr_txbb);
				txr->cons += txr->last_nr_txbb; // XXX should be 1
				for (;txr->cons != txr->prod; txr->cons++) {
					uint16_t j = txr->cons & txr->size_mask;
					uint32_t new_val, *ptr = (uint32_t *)(txr->buf + j * TXBB_SIZE);
					new_val = cpu_to_be32(STAMP_VAL | (!!(txr->cons & txr->size) << STAMP_SHIFT));
					ND(10, "old 0x%08x new 0x%08x", *ptr,  new_val);
					*ptr = new_val;
				}
			}
		}
		mlx4_en_stop_port(ifp);
		need_load = 1;
	}

retry:
	if (onoff) { /* enable netmap mode */
		ifp->if_capenable |= IFCAP_NETMAP;
		/* save if_transmit and replace with our routine */
		na->if_transmit = (void *)ifp->netdev_ops;
		ifp->netdev_ops = &na->nm_ndo;
	} else { /* reset normal mode */
		ifp->netdev_ops = (void *)na->if_transmit;
		ifp->if_capenable &= ~IFCAP_NETMAP;
	}
	if (need_load) {
		D("loading %s", ifp->if_xname);
		error = mlx4_en_start_port(ifp);
		D("start_port returns %d", error);
		if (error && onoff) {
			onoff = 0;
			goto retry;
		}
		mutex_unlock(&mdev->state_lock);
		//double_mutex_state_unlock(mdev);
	}
	// rtnl_unlock();
	return (error);
}


/*
 * Reconcile kernel and user view of the transmit ring.
 * This routine might be called frequently so it must be efficient.
 *
 * Userspace has filled tx slots up to ring->cur (excluded).
 * The last unused slot previously known to the kernel was kring->nkr_hwcur,
 * and the last interrupt reported kring->nr_hwavail slots available.
 *
 * This function runs under lock (acquired from the caller or internally).
 * It must first update ring->avail to what the kernel knows,
 * subtract the newly used slots (ring->cur - kring->nkr_hwcur)
 * from both avail and nr_hwavail, and set ring->nkr_hwcur = ring->cur
 * issuing a dmamap_sync on all slots.
 *
 * Since ring comes from userspace, its content must be read only once,
 * and validated before being used to update the kernel's structures.
 * (this is also true for every use of ring in the kernel).
 *
 * ring->avail is never used, only checked for bogus values.
 *
 * do_lock is set iff the function is called from the ioctl handler.
 * In this case, grab a lock around the body, and also reclaim transmitted
 * buffers irrespective of interrupt mitigation.

OUTGOING (txr->prod)
Tx packets need to fill a 64-byte block with one control block and
one descriptor (both 16-byte). Probably we need to fill the other
two data entries in the block with NULL entries as done in rx_config().
One can request completion reports (intr) on all entries or only
on selected ones. The std. driver reports every 16 packets.

txr->prod points to the first available slot to send.

COMPLETION (txr->cons)
TX events are reported through a Completion Queue (CQ) whose entries
can be 32 or 64 bytes. In case of 64 bytes, the interesting part is
at odd indexes. The "factor" variable does the addressing.

txr->cons points to the last completed block (XXX note so it is 1 behind)

There is no link back from the txring to the completion
queue so we need to track it ourselves. HOWEVER mlx4_en_alloc_resources()
uses the same index for cq and ring so tx_cq and tx_ring correspond,
same for rx_cq and rx_ring.

 */
static int
mlx4_netmap_txsync(struct ifnet *ifp, u_int ring_nr, int do_lock)
{
	struct SOFTC_T *priv = netdev_priv(ifp);
	struct mlx4_en_tx_ring *txr = &priv->tx_ring[ring_nr];
	struct netmap_adapter *na = NA(ifp);
	struct netmap_kring *kring = &na->tx_rings[ring_nr];
	struct netmap_ring *ring = kring->ring;
	u_int j, k = ring->cur, n = 0, lim = kring->nkr_num_slots - 1;
	int error = 0;

	/* if cur is invalid reinitialize the ring. */
	if (k > lim)
		return netmap_ring_reinit(kring);
	if (do_lock)
		mtx_lock(&na->core_lock); // XXX exp
		// mtx_lock(&kring->q_lock);

	// XXX debugging, only print if sending something
	n = (txr->prod - txr->cons - 1) & 0xffffff; // should be modulo 2^24 ?
	if (n >= txr->size) {
		RD(5, "XXXXXXXXXXX txr %d overflow: cons %u prod %u size %d delta %d",
		    ring_nr, txr->cons, txr->prod, txr->size, n);
	}
	/*
	 * Process new packets to send. j is the current index in the
	 * netmap ring, txr->prod is the entry in the NIC ring.
	 */
	j = kring->nr_hwcur;
	if (j > lim) {
		D("XXXXXXXXXXXXX ERROR q %d nwcur overflow %d", j, lim);
		error = EINVAL;
		goto err;
	}
	// XXX debugging, assuming lim is 2^x-1
	n = 0; // XXX debugging
	if (j != k) {	/* we have new packets to send */
		ND(5,"START: txr %d cons %u prod %u hwcur %u cur %u avail %d send %d",
			ring_nr, txr->cons, txr->prod, kring->nr_hwcur, ring->cur, kring->nr_hwavail,
			(k - j) & lim);

		// XXX see en_tx.c :: mlx4_en_xmit()
		/*
		 * In netmap the descriptor has one control segment
		 * and one data segment. The control segment is 16 bytes,
		 * the data segment is another 16 bytes mlx4_wqe_data_seg.
		 * The alignment is TXBB_SIZE (64 bytes) though, so we are
		 * forced to use 64 bytes each.
		 */

		ND(10,"=======>========== send from %d to %d at bd %d", j, k, txr->prod);
		for (n = 0; j != k; n++) {
			struct netmap_slot *slot = &ring->slot[j];
			uint64_t paddr;
			void *addr = PNMB(slot, &paddr);
			uint16_t len = slot->len;
			uint32_t l = txr->prod & txr->size_mask;
			struct mlx4_en_tx_desc *tx_desc = txr->buf + l * TXBB_SIZE;
			struct mlx4_wqe_ctrl_seg *ctrl = &tx_desc->ctrl;

			/* Quick check for valid addr and len. */
			if (addr == netmap_buffer_base || len > NETMAP_BUF_SIZE) {
				D("ring %d error, resetting", ring_nr);
				error = EINVAL;
				goto err;
			}

			slot->flags &= ~NS_REPORT;
			if (slot->flags & NS_BUF_CHANGED) {
				/* buffer has changed, unload and reload map */
				// netmap_reload_map(pdev, DMA_TO_DEVICE, old_addr, addr);
				slot->flags &= ~NS_BUF_CHANGED;
			}
			/*
			 * Fill the slot in the NIC ring.
			 */
			ctrl->vlan_tag = 0;	// not used
			ctrl->ins_vlan = 0;	// NO
			ctrl->fence_size = 2;	// used descriptor size in 16byte blocks
			// request notification. XXX later report only if NS_REPORT or not too often.
			ctrl->srcrb_flags = cpu_to_be32(MLX4_WQE_CTRL_CQ_UPDATE |
					MLX4_WQE_CTRL_SOLICITED);

			if (1) { // XXX do we need this ?
				uint64_t mac = mlx4_en_mac_to_u64(addr);
				uint32_t mac_h = (u32) ((mac & 0xffff00000000ULL) >> 16);
				uint32_t mac_l = (u32) (mac & 0xffffffff);

				ctrl->srcrb_flags |= cpu_to_be32(mac_h);
				ctrl->imm = cpu_to_be32(mac_l);
			}

			tx_desc->data.addr = cpu_to_be64(paddr);
			tx_desc->data.lkey = cpu_to_be32(priv->mdev->mr.key);
			wmb();		// XXX why here ?
			tx_desc->data.byte_count = cpu_to_be32(len); // XXX crc corrupt ?
			wmb();
			ctrl->owner_opcode = cpu_to_be32(
				MLX4_OPCODE_SEND |
				((txr->prod & txr->size) ? MLX4_EN_BIT_DESC_OWN : 0) );
			txr->prod++;
			j = (j == lim) ? 0 : j + 1;
		}
		kring->nr_hwcur = k; /* the saved ring->cur */
		/* decrease avail by number of packets  sent */
		kring->nr_hwavail -= n;

		/* XXX Check how to deal with nkr_hwofs */
		/* these two are always in sync. */
		wmb();	/* synchronize writes to the NIC ring */
		/* (re)start the transmitter up to slot l (excluded) */
		ND(5, "doorbell cid %d data 0x%x", txdata->cid, txdata->tx_db.raw);
		// XXX is this doorbell correct ?
		iowrite32be(txr->doorbell_qpn, txr->bf.uar->map + MLX4_SEND_DOORBELL);
	}
	// XXX debugging, only print if sent something
	if (n)
	    ND(5, "SENT: txr %d cons %u prod %u hwcur %u cur %u avail %d sent %d",
		ring_nr, txr->cons, txr->prod, kring->nr_hwcur, ring->cur, kring->nr_hwavail, n);

    /* XXX now recover completed transmissions. */
    {
	struct mlx4_en_cq *cq = &priv->tx_cq[ring_nr];
	struct mlx4_cq *mcq = &cq->mcq;

	int size = cq->size;			// number of entries
	struct mlx4_cqe *buf = cq->buf;		// base of cq entries
	uint32_t size_mask = txr->size_mask;	// same in txq and cq ?.......
	uint16_t new_index, ring_index;
	int factor = priv->cqe_factor;	// 1 for 64 bytes, 0 for 32 bytes

	/*
	 * Reclaim buffers for completed transmissions. The CQE tells us
	 * where the consumer (NIC) is. Bit 7 of the owner_sr_opcode
	 * is the ownership bit. It toggles up and down so the
	 * non-bitwise XNOR trick lets us detect toggles as the ring
	 * wraps around. On even rounds, the second operand is 0 so
	 * we exit when the MLX4_CQE_OWNER_MASK bit is 1, viceversa
	 * on odd rounds.
	 */
	new_index = ring_index = txr->cons & size_mask;

	for (n = 0; n < 2*lim; n++) {
		uint16_t index = mcq->cons_index & size_mask;
		struct mlx4_cqe *cqe = &buf[(index << factor) + factor];

		if (!XNOR(cqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK,
				mcq->cons_index & size))
			break;
                /*
                 * make sure we read the CQE after we read the
                 * ownership bit
                 */
                rmb();

                /* Skip over last polled CQE */
                new_index = be16_to_cpu(cqe->wqe_index) & size_mask;
		ND(5, "txq %d new_index %d", ring_nr, new_index);
		mcq->cons_index++;
	}
	if (n > lim) {
		D("XXXXXXXXXXX too many notifications %d", n);
	}
	/* now we have updated cons-index, notify the card. */
	/* XXX can we make it conditional ?  */
	wmb();
	mlx4_cq_set_ci(mcq);
	// XXX the following enables interrupts... */
	// mlx4_en_arm_cq(priv, cq); // XXX always ?
	wmb();
	/* XXX unsigned arithmetic below */
	n = (new_index - ring_index) & size_mask;
	if (n) {
		ND(5, "txr %d completed %d packets", ring_nr, n);
		/* some tx completed, increment hwavail. */
		txr->cons += n;
		kring->nr_hwavail += n;
		if (kring->nr_hwavail > lim) {
			D("XXXXXXXXXXXX ERROR ring %d hwavail %d > lim", ring_nr, kring->nr_hwavail);
			error = EINVAL;
			goto err;
		}
		ND(10, "RECOVER: txr %d cons %u prod %u hwcur %u cur %u avail %d n was %d",
			ring_nr, txr->cons, txr->prod, kring->nr_hwcur, ring->cur, kring->nr_hwavail, n);
	}
	if (kring->nr_hwavail == 0) {
		mlx4_en_arm_cq(priv, cq);
	}
    }
	ring->avail = kring->nr_hwavail;

err:
	if (do_lock)
		mtx_unlock(&na->core_lock);
		// mtx_unlock(&kring->q_lock);
	if (error)
		return netmap_ring_reinit(kring);
	return 0;
}


/*
 * Reconcile kernel and user view of the receive ring.
 * Same as for the txsync, this routine must be efficient and
 * avoid races in accessing the shared regions.
 *
 * When called, userspace has read data from slots kring->nr_hwcur
 * up to ring->cur (excluded).
 *
 * The last interrupt reported kring->nr_hwavail slots available
 * after kring->nr_hwcur.
 * We must subtract the newly consumed slots (cur - nr_hwcur)
 * from nr_hwavail, make the descriptors available for the next reads,
 * and set kring->nr_hwcur = ring->cur and ring->avail = kring->nr_hwavail.
 *
 * do_lock has a special meaning: please refer to txsync.

MELLANOX:

the ring has prod and cons indexes, the size is a power of 2,
size and actual_size indicate how many entries can be allocated,
stride is the size of each entry.

mlx4_en_update_rx_prod_db() tells the NIC where it can go
(to be used when new buffers are freed).
 
 */
static int
mlx4_netmap_rxsync(struct ifnet *ifp, u_int ring_nr, int do_lock)
{
	struct SOFTC_T *priv = netdev_priv(ifp);
	struct mlx4_en_rx_ring *rxr = &priv->rx_ring[ring_nr];
	struct netmap_adapter *na = NA(ifp);
	struct netmap_kring *kring = &na->rx_rings[ring_nr];
	struct netmap_ring *ring = kring->ring;
	u_int j, l, n, lim = kring->nkr_num_slots - 1;
	int force_update = do_lock || kring->nr_kflags & NKR_PENDINTR;
	u_int k = ring->cur, resvd = ring->reserved;

        if (!priv->port_up)	// XXX as in mlx4_en_process_rx_cq()
                return 0;

	if (k > lim) /* userspace is cheating */
		return netmap_ring_reinit(kring);
	ND(5, "START rxr %d cons %d prod %d kcur %d kavail %d cur %d avail %d",
		ring_nr, rxr->cons, rxr->prod, kring->nr_hwcur, kring->nr_hwavail, ring->cur, ring->avail);

	if (do_lock)
		mtx_lock(&kring->q_lock);
	/*
	 * First part, import newly received packets into the netmap ring.
	 *
	 * j is the index of the next free slot in the netmap ring,
	 * and l is the index of the next received packet in the NIC ring,
	 * and they may differ in case if_init() has been called while
	 * in netmap mode. For the receive ring we have
	 *
	 *	j = (kring->nr_hwcur + kring->nr_hwavail) % ring_size
	 *	l = consumer index in NIC ring (the NIC is the consumer)
	 * and
	 *	j == (l + kring->nkr_hwofs) % ring_size
	 */

	/* scan the completion queue to see what is going on.
	 * The mapping is 1:1. The hardware toggles the OWNER bit in the
	 * descriptor at mcq->cons_index & size_mask, which is mapped 1:1
	 * to an entry in the RXR.
	 * XXX there are two notifications sent to the hw:
	 *	mlx4_cq_set_ci(struct mlx4_cq *cq);
	 *		*cq->set_ci_db = cpu_to_be32(cq->cons_index & 0xffffff);
	 *	mlx4_en_update_rx_prod_db(rxr);
	 *		*ring->wqres.db.db = cpu_to_be32(ring->prod & 0xffff);
	 *	apparently they point to the same memory word
	 *	(see mlx4_en_activate_cq() ) and are initialized to 0
	 *	    DB is the doorbell page (sec.15.1.2 ?)
	 *		wqres is set in mlx4_alloc_hwq_res()
	 *		and in turn mlx4_alloc_hwq_res()
	 */
	if (1 || netmap_no_pendintr || force_update) {
		struct mlx4_en_cq *cq = &priv->rx_cq[ring_nr];
		struct mlx4_cq *mcq = &cq->mcq;
		int factor = priv->cqe_factor;
		uint32_t size_mask = rxr->size_mask;
		int size = cq->size;
		struct mlx4_cqe *buf = cq->buf;
		uint16_t slot_flags = kring->nkr_slot_flags;

		j = (kring->nr_hwcur + kring->nr_hwavail) % kring->nkr_num_slots;

		/* Process all completed CQEs, use same logic as in TX */
		for (n = 0; n <= 2*lim ; n++) {
			int index = mcq->cons_index & size_mask;
			struct mlx4_cqe *cqe = &buf[(index << factor) + factor];
			prefetch(cqe+1);
			if (!XNOR(cqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK, mcq->cons_index & size))
				break;

			rmb();	/* make sure data is up to date */
			ring->slot[j].len = be32_to_cpu(cqe->byte_cnt) - rxr->fcs_del;
			ring->slot[j].flags = slot_flags;
			mcq->cons_index++;
			j = (j == lim) ? 0 : j + 1;
		}
		if (n) { /* update the state variables */
			if (n >= 2*lim)
				D("XXXXXXXXXXXXX   too many received packets %d", n);
			ND(5, "received %d packets", n);
			kring->nr_hwavail += n;
			rxr->cons += n;
			ND(5, "RECVD %d rxr %d cons %d prod %d kcur %d kavail %d cur %d avail %d",
				n,
				ring_nr, rxr->cons, rxr->prod, kring->nr_hwcur, kring->nr_hwavail, ring->cur, ring->avail);

			/* XXX ack completion queue */
			mlx4_cq_set_ci(mcq);
		}
		kring->nr_kflags &= ~NKR_PENDINTR;
	}

	/*
	 * Skip past packets that userspace has already released
	 * (from kring->nr_hwcur to ring->cur-ring->reserved excluded),
	 * and make the buffers available for reception.
	 * As usual j is the index in the netmap ring, l is the index
	 * in the NIC ring, and j == (l + kring->nkr_hwofs) % ring_size
	 */
	j = kring->nr_hwcur; /* netmap ring index */
	if (resvd > 0) {
		if (resvd + ring->avail >= lim + 1) {
			D("XXX invalid reserve/avail %d %d", resvd, ring->avail);
			ring->reserved = resvd = 0; // XXX panic...
		}
		k = (k >= resvd) ? k - resvd : k + lim + 1 - resvd;
	}
	if (j != k) { /* userspace has released some packets. */
		l = netmap_idx_k2n(kring, j); // XXX NIC index
		for (n = 0; j != k; n++) {
			/* collect per-slot info, with similar validations
			 * and flag handling as in the txsync code.
			 *
			 * NOTE curr and rxbuf are indexed by l.
			 * Also, this driver needs to update the physical
			 * address in the NIC ring, but other drivers
			 * may not have this requirement.
			 */
			struct netmap_slot *slot = &ring->slot[j];
			uint64_t paddr;
			void *addr = PNMB(slot, &paddr);
			struct mlx4_en_rx_desc *rx_desc = rxr->buf + (l * rxr->stride);

			if (addr == netmap_buffer_base) /* bad buf */
				goto ring_reset;

			if (slot->flags & NS_BUF_CHANGED) {
				// netmap_reload_map(pdev, DMA_TO_DEVICE, old_addr, addr);
				slot->flags &= ~NS_BUF_CHANGED;
			}

			/* XXX
			 * The rx descriptor only contains buffer descriptors,
			 * probably only the length is changed or not even that one.
			 */
			// see mlx4_en_prepare_rx_desc() and mlx4_en_alloc_frag()
			rx_desc->data[0].addr = cpu_to_be64(paddr);
			rx_desc->data[0].byte_count = cpu_to_be32(NETMAP_BUF_SIZE);
			rx_desc->data[0].lkey = cpu_to_be32(priv->mdev->mr.key);

#if 0
			int jj, possible_frags;
			/* we only use one fragment, so the rest is padding */
			possible_frags = (rxr->stride - sizeof(struct mlx4_en_rx_desc)) / DS_SIZE;
			for (jj = 1; jj < possible_frags; jj++) {
				rx_desc->data[jj].byte_count = 0;
				rx_desc->data[jj].lkey = cpu_to_be32(MLX4_EN_MEMTYPE_PAD);
				rx_desc->data[jj].addr = 0;
			}
#endif

			j = (j == lim) ? 0 : j + 1;
			l = (l == lim) ? 0 : l + 1;
		}

		/* XXX note that mcq->cons_index and ring->cons are not in sync */
		wmb();
		rxr->prod += n;
		kring->nr_hwavail -= n;
		kring->nr_hwcur = k;

		/* and now tell the system that there are more buffers available.
		 * should use mlx4_en_update_rx_prod_db(rxr) but it is static in
		 * en_rx.c so we do not see it here
		 */
		*rxr->wqres.db.db = cpu_to_be32(rxr->prod & 0xffff);

		ring->avail = kring->nr_hwavail - resvd;	// XXX dup here for debugging
		ND(5, "FREED rxr %d cons %d prod %d kcur %d kavail %d cur %d avail %d",
			ring_nr, rxr->cons, rxr->prod, kring->nr_hwcur, kring->nr_hwavail, ring->cur, ring->avail);

	}

	/* tell userspace that there are new packets */
	ring->avail = kring->nr_hwavail - resvd;

	if (do_lock)
		mtx_unlock(&kring->q_lock);
	return 0;

ring_reset:
	if (do_lock)
		mtx_unlock(&kring->q_lock);
	return netmap_ring_reinit(kring);
}


/*
 * If in netmap mode, attach the netmap buffers to the ring and return true.
 * Otherwise return false.
 * Called at the end of mlx4_en_start_port().
 * XXX TODO: still incomplete.
 */
int
mlx4_netmap_tx_config(struct SOFTC_T *priv, int ring_nr)
{
	struct netmap_adapter *na = NA(priv->dev);
	struct netmap_slot *slot;
	struct mlx4_en_cq *cq;

	ND(5, "priv %p ring_nr %d", priv, ring_nr);

/*
 CONFIGURE TX RINGS IN NETMAP MODE
 little if anything to do
 The main code does
	mlx4_en_activate_cq()
	mlx4_en_activate_tx_ring()
	<Set initial ownership of all Tx TXBBs to SW (1)>

 */
	slot = netmap_reset(na, NR_TX, ring_nr, 0);
	if (!slot)
		return 0;			// not in netmap mode;
	ND(5, "init tx ring %d with %d slots (driver %d)", ring_nr,
		na->num_tx_desc,
		priv->tx_ring[ring_nr].size);
	/* enable interrupts on the netmap queues */
	cq = &priv->tx_cq[ring_nr];	// derive from the txring

	return 1;
}

int
mlx4_netmap_rx_config(struct SOFTC_T *priv, int ring_nr)
{
	struct netmap_adapter *na = NA(priv->dev);
        struct netmap_slot *slot;
        struct mlx4_en_rx_ring *rxr;
	struct netmap_kring *kring;
        int i, j, possible_frags;

	/*
	 * on the receive ring, must set buf addresses into the slots.

	The ring is activated by mlx4_en_activate_rx_rings(), near the end
	the rx ring is also 'started' with mlx4_en_update_rx_prod_db()
	so we patch into that routine.

	 */
	slot = netmap_reset(na, NR_RX, ring_nr, 0);
	if (!slot) // XXX should not happen
		return 0;
	kring = &na->rx_rings[ring_nr];
	rxr = &priv->rx_ring[ring_nr];
	ND(20, "ring %d slots %d (driver says %d) frags %d stride %d", ring_nr,
		kring->nkr_num_slots, rxr->actual_size, priv->num_frags, rxr->stride);
	rxr->prod--;	// XXX avoid wraparounds ?
	if (kring->nkr_num_slots != rxr->actual_size) {
		D("mismatch between slots and actual size, %d vs %d",
			kring->nkr_num_slots, rxr->actual_size);
		return 1; // XXX error
	}
	possible_frags = (rxr->stride - sizeof(struct mlx4_en_rx_desc)) / DS_SIZE;
	RD(1, "stride %d possible frags %d descsize %d DS_SIZE %d", rxr->stride, possible_frags, (int)sizeof(struct mlx4_en_rx_desc), (int)DS_SIZE );
	/* then fill the slots with our entries */
	for (i = 0; i < kring->nkr_num_slots; i++) {
		uint64_t paddr;
		struct mlx4_en_rx_desc *rx_desc = rxr->buf + (i * rxr->stride);

		PNMB(slot + i, &paddr);
		// see mlx4_en_prepare_rx_desc() and mlx4_en_alloc_frag()
		rx_desc->data[0].addr = cpu_to_be64(paddr);
		rx_desc->data[0].byte_count = cpu_to_be32(NETMAP_BUF_SIZE);
		rx_desc->data[0].lkey = cpu_to_be32(priv->mdev->mr.key);

		/* we only use one fragment, so the rest is padding */
		for (j = 1; j < possible_frags; j++) {
			rx_desc->data[j].byte_count = 0;
			rx_desc->data[j].lkey = cpu_to_be32(MLX4_EN_MEMTYPE_PAD);
			rx_desc->data[j].addr = 0;
		}
	}
	RD(5, "ring %d done", ring_nr);
	return 1;
}


/*
 * The attach routine, called near the end of mlx4_en_init_netdev(),
 * fills the parameters for netmap_attach() and calls it.
 * It cannot fail, in the worst case (such as no memory)
 * netmap mode will be disabled and the driver will only
 * operate in standard mode.
 *
 * XXX TODO:
 *   at the moment use a single lock, and only init a max of 4 queues.
 */
static void
mlx4_netmap_attach(struct SOFTC_T *priv)
{
	struct netmap_adapter na;
	struct net_device *dev = priv->dev;
	int rxq, txq, nq;

	bzero(&na, sizeof(na));

	na.ifp = dev;
	na.separate_locks = 1;
	rxq = priv->rx_ring_num;
	txq = priv->tx_ring_num;
	/* this card has 1k tx queues, so better limit the number */
	nq = rxq;
	if (rxq < nq)
		nq = rxq;
	if (txq < nq)
		nq = txq;

	D("hw configured for %d/%d tx/rx rings, use %d", txq, rxq, nq);
	if (txq < 1 && rxq < 1)
		txq = rxq = 1;
	/* this card has separate rx/tx locks */
	na.num_tx_desc = priv->tx_ring[0].size;
	na.num_rx_desc = priv->rx_ring[0].size;
	na.nm_txsync = mlx4_netmap_txsync;
	na.nm_rxsync = mlx4_netmap_rxsync;
	na.nm_register = mlx4_netmap_reg;
	netmap_attach(&na, nq);
	D("%d queues, tx: %d rx %d slots", na.num_rx_rings,
			na.num_tx_desc, na.num_rx_desc);
}
#endif /* NETMAP_BNX2X_MAIN */
/* end of file */
