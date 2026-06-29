/*	$OpenBSD$	*/

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015-2024 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2026 mmap <mmapdreams@users.noreply.github.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#ifndef _IF_ENAVAR_H_
#define _IF_ENAVAR_H_

#include "ena-com/ena_com.h"
#include "ena-com/ena_eth_com.h"

/* PCI BARs (see ena.h in the FreeBSD reference). */
#define ENA_REG_BAR		PCI_MAPREG_START	/* BAR0: registers */
#define ENA_MEM_BAR		(PCI_MAPREG_START + 8)	/* BAR2: LLQ memory */

/* Ring sizing.  Powers of two; the device requires po2 queue depth. */
#define ENA_DEFAULT_TX_DESC	1024
#define ENA_DEFAULT_RX_DESC	1024
#define ENA_RX_RING_LWM		64	/* seed a usable RX buffer pool */
#define ENA_MIN_RING_SIZE	256

/* Up to this many segments per packet that the device accepts. */
#define ENA_PKT_MAX_BUFS	19
#define ENA_TX_CLEANUP_BUDGET	128
#define ENA_RX_CLEANUP_BUDGET	256

/*
 * IO-progress watchdog: consecutive 1Hz ena_tick() passes a TX ring may
 * have descriptors posted but reap zero completions before the queue is
 * declared wedged and reset.  Mirrors the keep-alive AENQ's 6s slack; the
 * keep-alive (admin vector) stays live during an IO-vector stall, so this
 * is the only liveness check that can see a wedged IO ring.
 */
#define ENA_TX_STALL_TICKS	6

#define ENA_RX_REFILL_THRESH_DIVIDER	8

#define ENA_ADMIN_MSIX_VEC	1	/* vector 0 == admin + AENQ */

#define ENA_MAX_FRAME_LEN	9216
#define ENA_MIN_MTU		128

#define ENA_TX_RING_IDX_NEXT(idx, size) (((idx) + 1) & ((size) - 1))
#define ENA_RX_RING_IDX_NEXT(idx, size) (((idx) + 1) & ((size) - 1))

/* HAL maps queue id 2*q (tx) and 2*q+1 (rx). */
#define ENA_IO_TXQ_IDX(q)	(2 * (q))
#define ENA_IO_RXQ_IDX(q)	(2 * (q) + 1)

/* RSS indirection table size, log2 (128 entries, matches the FreeBSD ena). */
#define ENA_RX_RSS_TABLE_LOG_SIZE	7

#define ENA_DEVNAME(sc)		((sc)->sc_dev.dv_xname)

/*
 * Coherent DMA chunk used for admin/AENQ/IO descriptor rings.  Mirrors the
 * mcx/vmx <drv>_dmamem helper.  ena-com fills its own ena_mem_handle_t via
 * ena_dma_alloc(); this struct is for rings the glue owns directly.
 */
struct ena_dmamem {
	bus_dmamap_t		edm_map;
	bus_dma_segment_t	edm_seg;
	size_t			edm_size;
	caddr_t			edm_kva;
};
#define ENA_DMA_MAP(_edm)	((_edm)->edm_map)
#define ENA_DMA_DVA(_edm)	((_edm)->edm_map->dm_segs[0].ds_addr)
#define ENA_DMA_KVA(_edm)	((void *)(_edm)->edm_kva)
#define ENA_DMA_LEN(_edm)	((_edm)->edm_size)

/* Per-slot software state for a TX descriptor. */
struct ena_tx_buf {
	struct mbuf		*etx_mbuf;
	bus_dmamap_t		 etx_map;
	struct ena_com_buf	 etx_bufs[ENA_PKT_MAX_BUFS];
	uint16_t		 etx_nb_hw_desc;	/* SQ descriptors posted for this packet */
};

/* Per-slot software state for an RX descriptor. */
struct ena_rx_buf {
	struct mbuf		*erx_mbuf;
	bus_dmamap_t		 erx_map;
	struct ena_com_buf	 erx_buf;
};

struct ena_softc;

/*
 * A queue == one TX ring + one RX ring sharing one MSI-X vector, mirroring
 * the device's IO queue-pair model.  v1 uses a single queue.
 */
struct ena_queue {
	struct ena_softc	*eq_sc;
	unsigned int		 eq_idx;	/* queue index (0..n-1) */
	void			*eq_ih;		/* MSI-X cookie */
	unsigned int		 eq_intr;	/* MSI-X vector number */
	char			 eq_intrname[16];

	/* TX */
	struct ifqueue		*eq_ifq;
	struct ena_com_io_sq	*eq_tx_sq;
	struct ena_com_io_cq	*eq_tx_cq;
	struct ena_tx_buf	*eq_tx_buf;	/* [tx_ring_size] */
	uint16_t		*eq_tx_free_ids;
	unsigned int		 eq_tx_ring_size;
	unsigned int		 eq_tx_prod;	/* next req_id to use */
	unsigned int		 eq_tx_cons;	/* next completion to reap */

	/* IO-progress watchdog state (ena_tick); see ENA_TX_STALL_TICKS. */
	uint64_t		 eq_tx_completions; /* reaped TX descs, monotonic */
	uint64_t		 eq_tx_stall_last;  /* eq_tx_completions at last tick */
	unsigned int		 eq_tx_stall_ticks; /* ticks stalled w/ TX in flight */

	/* RX */
	struct ifiqueue		*eq_ifiq;
	struct ena_com_io_sq	*eq_rx_sq;
	struct ena_com_io_cq	*eq_rx_cq;
	struct ena_rx_buf	*eq_rx_buf;	/* [rx_ring_size] */
	struct if_rxring	 eq_rx_ring;
	struct timeout		 eq_rx_refill;
	unsigned int		 eq_rx_ring_size;
	unsigned int		 eq_rx_prod;	/* next slot to refill */
	unsigned int		 eq_rx_cons;	/* next slot to consume */
	struct mutex		 eq_rx_mtx;
};

struct ena_softc {
	struct device		 sc_dev;
	struct arpcom		 sc_arpcom;
	struct ifmedia		 sc_media;

	pci_chipset_tag_t	 sc_pc;
	pcitag_t		 sc_tag;
	pci_intr_handle_t	 sc_ih_handle;

	/* BAR0 register window, wrapped for the HAL. */
	struct ena_bus		 sc_bus;
	bus_size_t		 sc_reg_ios;
	bus_dma_tag_t		 sc_dmat;

	/* ena-com device handle. */
	struct ena_com_dev	*sc_ena_dev;

	void			*sc_admin_ih;	/* vector 0 cookie */

	struct intrmap		*sc_intrmap;
	unsigned int		 sc_nqueues;
	unsigned int		 sc_max_io_queues;	/* device limit */
	int			 sc_rss_ready;		/* RSS host state allocated */
	struct ena_queue	*sc_queues;	/* [sc_nqueues] */

	unsigned int		 sc_tx_ring_size;
	unsigned int		 sc_rx_ring_size;
	uint32_t		 sc_max_mtu;
	uint32_t		 sc_tx_offload_cap;

	/* AENQ-driven liveness + link state. */
	uint64_t		 sc_keep_alive_ts;	/* nsec uptime */
	int			 sc_link_state;
	int			 sc_link_pending;
	struct task		 sc_link_task;

	struct timeout		 sc_tick;	/* 1Hz keep-alive watchdog */
	struct task		 sc_reset_task;
	struct taskq		*sc_reset_tq;

	int			 sc_up;		/* IFF_RUNNING shadow */
};

/* if_ena.c */
int	ena_dma_alloc(void *, bus_size_t, ena_mem_handle_t *, int,
	    bus_size_t, int);
void	ena_dma_free(void *, bus_size_t, ena_mem_handle_t *);

#endif /* _IF_ENAVAR_H_ */
