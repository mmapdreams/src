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
 * Copyright (c) 2024 mmap <mmapdreams@users.noreply.github.com>
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

/*
 * Driver for the Amazon Elastic Network Adapter (ENA), the paravirtual NIC
 * found on AWS EC2 "Nitro" instances.  The device-independent ena-com HAL
 * (sys/dev/pci/ena-com/) is imported verbatim from upstream; this file is the
 * OpenBSD-native glue (autoconf, PCI/MSI-X, bus_dma, ifnet datapath) over it.
 *
 * v1 scope: single IO queue, host-memory TX placement (no LLQ).  RX csum
 * results from the device are honoured, and TX IPv4/TCP/UDP checksum
 * offload is advertised when the device reports the capability (the L3/L4
 * metadata descriptor is built per packet in ena_tx_csum()).  Multiqueue/
 * RSS, TSO, LLQ and KSTAT are intentionally staged for follow-up work.
 */

#include "bpfilter.h"
#include "vlan.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/socket.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/atomic.h>
#include <sys/intrmap.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/toeplitz.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>

#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/pci/if_enavar.h>

/*
 * ena-com expects the OS log level as a global; ENA_ERR (0) keeps the device
 * quiet except for real errors.
 */
int ena_log_level = ENA_ERR;

const char *
ena_dev_name(void *arg)
{
	struct ena_softc *sc = arg;

	return (ENA_DEVNAME(sc));
}

/* autoconf */
int	ena_match(struct device *, void *, void *);
void	ena_attach(struct device *, struct device *, void *);

/* setup */
int	ena_map_pci(struct ena_softc *, struct pci_attach_args *);
int	ena_device_init(struct ena_softc *,
	    struct ena_com_dev_get_features_ctx *);
int	ena_setup_interrupts(struct ena_softc *, struct pci_attach_args *);
void	ena_setup_ifp(struct ena_softc *, uint8_t *);

/* ena-com platform callbacks (referenced from ena_plat.h). */
/* ena_dma_alloc / ena_dma_free / ena_rss_key_fill are below. */

/* ifnet */
int	ena_ioctl(struct ifnet *, u_long, caddr_t);
int	ena_init(struct ena_softc *);
void	ena_stop(struct ena_softc *);
void	ena_start(struct ifqueue *);
void	ena_watchdog(struct ifnet *);
int	ena_media_change(struct ifnet *);
void	ena_media_status(struct ifnet *, struct ifmediareq *);
int	ena_rxrinfo(struct ena_softc *, struct if_rxrinfo *);

/* IO queues */
int	ena_create_io_queues(struct ena_softc *);
void	ena_destroy_io_queues(struct ena_softc *);
int	ena_queue_alloc(struct ena_softc *, struct ena_queue *);
void	ena_queue_free(struct ena_softc *, struct ena_queue *);

/* datapath */
void	ena_rx_fill(struct ena_queue *);
void	ena_rx_refill(void *);
int	ena_rxeof(struct ena_queue *);
int	ena_txeof(struct ena_queue *);
int	ena_encap(struct ena_queue *, struct mbuf *);
void	ena_tx_csum(struct ena_com_tx_ctx *, struct mbuf *);
int	ena_intr_queue(void *);
int	ena_intr_admin(void *);

/* control */
void	ena_tick(void *);
void	ena_reset_task(void *);
void	ena_link_task(void *);

/* dmamem helper for glue-owned rings */
int	ena_dmamem_alloc(struct ena_softc *, struct ena_dmamem *,
	    bus_size_t, u_int);
void	ena_dmamem_free(struct ena_softc *, struct ena_dmamem *);

const struct pci_matchid ena_devices[] = {
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_PF },
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_PF_RSERV0 },
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_VF },
	{ PCI_VENDOR_AMAZON, PCI_PRODUCT_AMAZON_ENA_VF_RSERV0 },
};

const struct cfattach ena_ca = {
	sizeof(struct ena_softc), ena_match, ena_attach
};

struct cfdriver ena_cd = {
	NULL, "ena", DV_IFNET
};

/* ------------------------------------------------------------------------ *
 * ena-com platform callbacks
 * ------------------------------------------------------------------------ */

/*
 * Allocate coherent DMA memory for an ena-com object and fill its mem handle.
 * OpenBSD has no per-allocation bus_dma_tag_create; the parent tag is used and
 * the constraints are passed to bus_dmamap_create.
 */
int
ena_dma_alloc(void *dmadev, bus_size_t size, ena_mem_handle_t *dma,
    int mapflags, bus_size_t alignment, int domain)
{
	struct ena_softc *sc = dmadev;
	int nseg;

	dma->tag = sc->sc_dmat;

	if (bus_dmamap_create(dma->tag, size, 1, size, 0,
	    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW | BUS_DMA_64BIT, &dma->map) != 0)
		return (ENOMEM);
	if (bus_dmamem_alloc(dma->tag, size, alignment, 0, &dma->seg, 1,
	    &nseg, BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_64BIT) != 0)
		goto destroy;
	dma->nseg = nseg;
	if (bus_dmamem_map(dma->tag, &dma->seg, nseg, size,
	    &dma->vaddr, BUS_DMA_WAITOK | BUS_DMA_COHERENT) != 0)
		goto free;
	if (bus_dmamap_load(dma->tag, dma->map, dma->vaddr, size, NULL,
	    BUS_DMA_WAITOK) != 0)
		goto unmap;

	dma->paddr = dma->map->dm_segs[0].ds_addr;
	return (0);

unmap:
	bus_dmamem_unmap(dma->tag, dma->vaddr, size);
free:
	bus_dmamem_free(dma->tag, &dma->seg, 1);
destroy:
	bus_dmamap_destroy(dma->tag, dma->map);
	dma->tag = NULL;
	dma->vaddr = NULL;
	return (ENOMEM);
}

void
ena_dma_free(void *dmadev, bus_size_t size, ena_mem_handle_t *dma)
{
	if (dma->tag == NULL)
		return;
	bus_dmamap_unload(dma->tag, dma->map);
	bus_dmamem_unmap(dma->tag, dma->vaddr, size);
	bus_dmamem_free(dma->tag, &dma->seg, 1);
	bus_dmamap_destroy(dma->tag, dma->map);
	dma->tag = NULL;
	dma->vaddr = NULL;
}

void
ena_rss_key_fill(void *key, size_t size)
{
	stoeplitz_to_key(key, size);
}

/* ------------------------------------------------------------------------ *
 * glue-owned coherent ring allocation
 * ------------------------------------------------------------------------ */

int
ena_dmamem_alloc(struct ena_softc *sc, struct ena_dmamem *edm, bus_size_t size,
    u_int align)
{
	int nseg;

	edm->edm_size = size;

	if (bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW | BUS_DMA_64BIT,
	    &edm->edm_map) != 0)
		return (1);
	if (bus_dmamem_alloc(sc->sc_dmat, size, align, 0, &edm->edm_seg, 1,
	    &nseg, BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_64BIT) != 0)
		goto destroy;
	if (bus_dmamem_map(sc->sc_dmat, &edm->edm_seg, nseg, size,
	    &edm->edm_kva, BUS_DMA_WAITOK | BUS_DMA_COHERENT) != 0)
		goto free;
	if (bus_dmamap_load(sc->sc_dmat, edm->edm_map, edm->edm_kva, size,
	    NULL, BUS_DMA_WAITOK) != 0)
		goto unmap;

	return (0);

unmap:
	bus_dmamem_unmap(sc->sc_dmat, edm->edm_kva, size);
free:
	bus_dmamem_free(sc->sc_dmat, &edm->edm_seg, 1);
destroy:
	bus_dmamap_destroy(sc->sc_dmat, edm->edm_map);
	return (1);
}

void
ena_dmamem_free(struct ena_softc *sc, struct ena_dmamem *edm)
{
	bus_dmamap_unload(sc->sc_dmat, edm->edm_map);
	bus_dmamem_unmap(sc->sc_dmat, edm->edm_kva, edm->edm_size);
	bus_dmamem_free(sc->sc_dmat, &edm->edm_seg, 1);
	bus_dmamap_destroy(sc->sc_dmat, edm->edm_map);
}

/* ------------------------------------------------------------------------ *
 * AENQ (async event) handlers
 * ------------------------------------------------------------------------ */

static void
ena_aenq_link_change(void *data, struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_softc *sc = data;
	struct ena_admin_aenq_link_change_desc *desc;

	desc = (struct ena_admin_aenq_link_change_desc *)aenq_e;
	sc->sc_link_pending =
	    (desc->flags & ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK) ?
	    LINK_STATE_FULL_DUPLEX : LINK_STATE_DOWN;

	/*
	 * AENQ callbacks run in the admin interrupt at IPL_NET without the
	 * KERNEL_LOCK; if_link_state_change() must run from a task instead.
	 */
	task_add(systq, &sc->sc_link_task);
}

static void
ena_aenq_keep_alive(void *data, struct ena_admin_aenq_entry *aenq_e)
{
	struct ena_softc *sc = data;

	sc->sc_keep_alive_ts = getnsecuptime();
}

static void
ena_aenq_default(void *data, struct ena_admin_aenq_entry *aenq_e)
{
	/* Notifications / warnings: nothing actionable in v1. */
}

static struct ena_aenq_handlers ena_aenq_handlers = {
	.handlers = {
		[ENA_ADMIN_LINK_CHANGE] = ena_aenq_link_change,
		[ENA_ADMIN_KEEP_ALIVE] = ena_aenq_keep_alive,
	},
	.unimplemented_handler = ena_aenq_default,
};

/* ------------------------------------------------------------------------ *
 * autoconf
 * ------------------------------------------------------------------------ */

int
ena_match(struct device *parent, void *match, void *aux)
{
	return (pci_matchbyid(aux, ena_devices, nitems(ena_devices)));
}

void
ena_attach(struct device *parent, struct device *self, void *aux)
{
	struct ena_softc *sc = (struct ena_softc *)self;
	struct pci_attach_args *pa = aux;
	struct ena_com_dev_get_features_ctx feat;
	struct ena_com_dev *ena_dev;

	sc->sc_pc = pa->pa_pc;
	sc->sc_tag = pa->pa_tag;
	sc->sc_dmat = pa->pa_dmat;
	sc->sc_link_state = LINK_STATE_UNKNOWN;

	if (ena_map_pci(sc, pa) != 0)
		return;

	ena_dev = malloc(sizeof(*ena_dev), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_ena_dev = ena_dev;
	ena_dev->dmadev = sc;
	ena_dev->bus = &sc->sc_bus;

	if (ena_device_init(sc, &feat) != 0) {
		printf(": device init failed\n");
		goto free_dev;
	}

	/*
	 * Queue sizing.  v1 uses a single IO queue.  Clamp ring depth to the
	 * device's advertised maximum and a power of two.
	 */
	sc->sc_tx_ring_size = ENA_DEFAULT_TX_DESC;
	sc->sc_rx_ring_size = ENA_DEFAULT_RX_DESC;
	sc->sc_max_mtu = feat.dev_attr.max_mtu;
	sc->sc_tx_offload_cap = feat.offload.tx;

	if (ena_setup_interrupts(sc, pa) != 0) {
		printf(": interrupt setup failed\n");
		goto destroy_dev;
	}

	ena_com_admin_aenq_enable(ena_dev);

	timeout_set(&sc->sc_tick, ena_tick, sc);
	sc->sc_reset_tq = taskq_create(ENA_DEVNAME(sc), 1, IPL_NET, 0);
	if (sc->sc_reset_tq != NULL)
		task_set(&sc->sc_reset_task, ena_reset_task, sc);
	task_set(&sc->sc_link_task, ena_link_task, sc);

	ena_setup_ifp(sc, feat.dev_attr.mac_addr);

	printf(", address %s\n",
	    ether_sprintf(sc->sc_arpcom.ac_enaddr));
	return;

destroy_dev:
	ena_com_admin_destroy(ena_dev);
	ena_com_mmio_reg_read_request_destroy(ena_dev);
free_dev:
	free(ena_dev, M_DEVBUF, sizeof(*ena_dev));
	sc->sc_ena_dev = NULL;
	bus_space_unmap(sc->sc_bus.reg_bar_t, sc->sc_bus.reg_bar_h,
	    sc->sc_reg_ios);
}

int
ena_map_pci(struct ena_softc *sc, struct pci_attach_args *pa)
{
	pcireg_t memtype;

	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, ENA_REG_BAR);
	if (pci_mapreg_map(pa, ENA_REG_BAR, memtype, 0,
	    &sc->sc_bus.reg_bar_t, &sc->sc_bus.reg_bar_h, NULL,
	    &sc->sc_reg_ios, 0) != 0) {
		printf(": can't map registers\n");
		return (1);
	}

	return (0);
}

/*
 * Bring up the ena-com device: mirror FreeBSD's ena_device_init.  Admin runs
 * in polling mode through this path; the caller switches to interrupt-driven
 * admin after the MSI-X handlers are established.
 */
int
ena_device_init(struct ena_softc *sc, struct ena_com_dev_get_features_ctx *feat)
{
	struct ena_com_dev *ena_dev = sc->sc_ena_dev;
	uint32_t aenq_groups;
	int rc, dma_width;
	bool readless = true;

	rc = ena_com_mmio_reg_read_request_init(ena_dev);
	if (rc != 0)
		return (rc);

	ena_com_set_mmio_read_mode(ena_dev, readless);

	rc = ena_com_dev_reset(ena_dev, ENA_REGS_RESET_NORMAL);
	if (rc != 0)
		goto err_mmio;

	rc = ena_com_validate_version(ena_dev);
	if (rc != 0)
		goto err_mmio;

	dma_width = ena_com_get_dma_width(ena_dev);
	if (dma_width < 0) {
		rc = dma_width;
		goto err_mmio;
	}

	rc = ena_com_admin_init(ena_dev, &ena_aenq_handlers);
	if (rc != 0)
		goto err_mmio;

	/* Polled admin keeps the bring-up path free of wait-events. */
	ena_com_set_admin_polling_mode(ena_dev, true);

	rc = ena_com_get_dev_attr_feat(ena_dev, feat);
	if (rc != 0)
		goto err_admin;

	aenq_groups = BIT(ENA_ADMIN_LINK_CHANGE) | BIT(ENA_ADMIN_KEEP_ALIVE) |
	    BIT(ENA_ADMIN_FATAL_ERROR) | BIT(ENA_ADMIN_WARNING);
	aenq_groups &= feat->aenq.supported_groups;
	rc = ena_com_set_aenq_config(ena_dev, aenq_groups);
	if (rc != 0)
		goto err_admin;

	return (0);

err_admin:
	ena_com_admin_destroy(ena_dev);
err_mmio:
	ena_com_mmio_reg_read_request_destroy(ena_dev);
	return (rc);
}

/*
 * MSI-X: vector 0 == admin/AENQ, vectors 1..N == IO queues distributed over
 * CPUs by intrmap.  v1 binds a single IO queue.
 */
int
ena_setup_interrupts(struct ena_softc *sc, struct pci_attach_args *pa)
{
	struct ena_com_dev *ena_dev = sc->sc_ena_dev;
	pci_intr_handle_t ih;
	const char *intrstr;
	int msix, nvec, i;

	msix = pci_intr_msix_count(pa);
	if (msix < 2) {
		printf(": not enough MSI-X vectors");
		return (1);
	}

	/* Admin/AENQ on vector 0. */
	if (pci_intr_map_msix(pa, 0, &ih) != 0) {
		printf(": can't map admin interrupt");
		return (1);
	}
	intrstr = pci_intr_string(sc->sc_pc, ih);
	sc->sc_admin_ih = pci_intr_establish(sc->sc_pc, ih, IPL_NET | IPL_MPSAFE,
	    ena_intr_admin, sc, ENA_DEVNAME(sc));
	if (sc->sc_admin_ih == NULL) {
		printf(": can't establish admin interrupt");
		return (1);
	}
	if (intrstr != NULL)
		printf(": %s", intrstr);

	/* IO queue vectors over the remaining MSI-X. */
	nvec = msix - 1;
	sc->sc_intrmap = intrmap_create(&sc->sc_dev, nvec, 1,
	    INTRMAP_POWEROF2);
	sc->sc_nqueues = intrmap_count(sc->sc_intrmap);

	sc->sc_queues = mallocarray(sc->sc_nqueues, sizeof(*sc->sc_queues),
	    M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];
		int vec = i + 1;

		eq->eq_sc = sc;
		eq->eq_idx = i;
		eq->eq_intr = vec;
		eq->eq_tx_ring_size = sc->sc_tx_ring_size;
		eq->eq_rx_ring_size = sc->sc_rx_ring_size;
		mtx_init(&eq->eq_rx_mtx, IPL_NET);
		timeout_set(&eq->eq_rx_refill, ena_rx_refill, eq);

		if (pci_intr_map_msix(pa, vec, &ih) != 0) {
			printf(": can't map queue interrupt %d\n", vec);
			return (1);
		}
		snprintf(eq->eq_intrname, sizeof(eq->eq_intrname), "%s:%d",
		    ENA_DEVNAME(sc), i);
		eq->eq_ih = pci_intr_establish_cpu(sc->sc_pc, ih,
		    IPL_NET | IPL_MPSAFE, intrmap_cpu(sc->sc_intrmap, i),
		    ena_intr_queue, eq, eq->eq_intrname);
		if (eq->eq_ih == NULL) {
			printf(": can't establish queue interrupt %d\n", vec);
			return (1);
		}
	}

	/* Admin completions are interrupt-driven from here on. */
	ena_com_set_admin_polling_mode(ena_dev, false);

	return (0);
}

void
ena_setup_ifp(struct ena_softc *sc, uint8_t *mac)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	uint32_t cap = sc->sc_tx_offload_cap;

	memcpy(sc->sc_arpcom.ac_enaddr, mac, ETHER_ADDR_LEN);

	strlcpy(ifp->if_xname, ENA_DEVNAME(sc), IFNAMSIZ);
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_xflags = IFXF_MPSAFE;
	ifp->if_ioctl = ena_ioctl;
	ifp->if_qstart = ena_start;
	ifp->if_watchdog = ena_watchdog;
	ifp->if_hardmtu = sc->sc_max_mtu;
	/*
	 * TX checksum offload.  The device places an offloaded checksum only
	 * when ena_encap() emits a metadata descriptor describing the L3/L4
	 * protocol and the L3 header offset/length (see ena_tx_csum()); a
	 * stale or missing descriptor silently corrupts the packet.  OpenBSD
	 * pre-seeds the L4 pseudo-header checksum (in_proto_cksum_out()), so
	 * we drive the device in its "partial" L4 mode -- advertise the
	 * *_CSUM_PART device capabilities, not *_CSUM_FULL.
	 */
	ifp->if_capabilities = IFCAP_VLAN_MTU;
	if (ISSET(cap, ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L3_CSUM_IPV4_MASK))
		ifp->if_capabilities |= IFCAP_CSUM_IPv4;
	if (ISSET(cap, ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV4_CSUM_PART_MASK))
		ifp->if_capabilities |= IFCAP_CSUM_TCPv4 | IFCAP_CSUM_UDPv4;
	if (ISSET(cap, ENA_ADMIN_FEATURE_OFFLOAD_DESC_TX_L4_IPV6_CSUM_PART_MASK))
		ifp->if_capabilities |= IFCAP_CSUM_TCPv6 | IFCAP_CSUM_UDPv6;

	ifq_init_maxlen(&ifp->if_snd, sc->sc_tx_ring_size);

	ifmedia_init(&sc->sc_media, IFM_IMASK, ena_media_change,
	    ena_media_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);

	if_attach(ifp);
	ether_ifattach(ifp);

	if_attach_iqueues(ifp, sc->sc_nqueues);
	if_attach_queues(ifp, sc->sc_nqueues);
}

/* ------------------------------------------------------------------------ *
 * ifnet control
 * ------------------------------------------------------------------------ */

int
ena_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct ena_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0, s;

	s = splnet();

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (!ISSET(ifp->if_flags, IFF_RUNNING))
			error = ena_init(sc);
		break;
	case SIOCSIFFLAGS:
		if (ISSET(ifp->if_flags, IFF_UP)) {
			if (ISSET(ifp->if_flags, IFF_RUNNING))
				error = ENETRESET;
			else
				error = ena_init(sc);
		} else {
			if (ISSET(ifp->if_flags, IFF_RUNNING))
				ena_stop(sc);
		}
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;
	case SIOCGIFRXR:
		error = ena_rxrinfo(sc, (struct if_rxrinfo *)ifr->ifr_data);
		break;
	default:
		error = ether_ioctl(ifp, &sc->sc_arpcom, cmd, data);
	}

	if (error == ENETRESET) {
		/* Multicast/rx filter refresh: ENA falls back to allmulti. */
		error = 0;
	}

	splx(s);
	return (error);
}

int
ena_rxrinfo(struct ena_softc *sc, struct if_rxrinfo *ifri)
{
	struct if_rxring_info *ifrs;
	unsigned int i;
	int error;

	ifrs = mallocarray(sc->sc_nqueues, sizeof(*ifrs), M_TEMP,
	    M_WAITOK | M_ZERO | M_CANFAIL);
	if (ifrs == NULL)
		return (ENOMEM);

	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];

		ifrs[i].ifr_size = MCLBYTES;
		snprintf(ifrs[i].ifr_name, sizeof(ifrs[i].ifr_name), "%u", i);
		ifrs[i].ifr_info = eq->eq_rx_ring;
	}

	error = if_rxr_info_ioctl(ifri, sc->sc_nqueues, ifrs);
	free(ifrs, M_TEMP, sc->sc_nqueues * sizeof(*ifrs));
	return (error);
}

int
ena_media_change(struct ifnet *ifp)
{
	return (0);
}

void
ena_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct ena_softc *sc = ifp->if_softc;

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER | IFM_AUTO;

	if (sc->sc_link_state == LINK_STATE_FULL_DUPLEX ||
	    sc->sc_link_state == LINK_STATE_UP)
		ifmr->ifm_status |= IFM_ACTIVE;
}

void
ena_link_task(void *arg)
{
	struct ena_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	int link = sc->sc_link_pending;

	NET_LOCK();
	sc->sc_link_state = link;
	if (ifp->if_link_state != link) {
		ifp->if_link_state = link;
		if_link_state_change(ifp);
	}
	NET_UNLOCK();
}

/* ------------------------------------------------------------------------ *
 * init / stop
 * ------------------------------------------------------------------------ */

int
ena_init(struct ena_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	unsigned int i;
	int rc;

	if (ISSET(ifp->if_flags, IFF_RUNNING))
		ena_stop(sc);

	rc = ena_create_io_queues(sc);
	if (rc != 0) {
		printf("%s: failed to create IO queues\n", ENA_DEVNAME(sc));
		return (rc);
	}

	/* Prime the RX rings before enabling the datapath. */
	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];

		if_rxr_init(&eq->eq_rx_ring, ENA_RX_RING_LWM,
		    eq->eq_rx_ring_size - 1);
		mtx_enter(&eq->eq_rx_mtx);
		ena_rx_fill(eq);
		mtx_leave(&eq->eq_rx_mtx);
	}

	SET(ifp->if_flags, IFF_RUNNING);
	sc->sc_up = 1;

	for (i = 0; i < sc->sc_nqueues; i++) {
		ifq_clr_oactive(ifp->if_ifqs[i]);
		ifq_restart(ifp->if_ifqs[i]);
	}

	sc->sc_keep_alive_ts = getnsecuptime();
	timeout_add_sec(&sc->sc_tick, 1);

	return (0);
}

void
ena_stop(struct ena_softc *sc)
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	unsigned int i;

	CLR(ifp->if_flags, IFF_RUNNING);
	sc->sc_up = 0;

	timeout_del(&sc->sc_tick);

	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];

		ifq_clr_oactive(ifp->if_ifqs[i]);
		ifq_barrier(ifp->if_ifqs[i]);
		timeout_del(&eq->eq_rx_refill);
	}

	intr_barrier(sc->sc_admin_ih);
	for (i = 0; i < sc->sc_nqueues; i++)
		intr_barrier(sc->sc_queues[i].eq_ih);

	ena_destroy_io_queues(sc);
}

/* ------------------------------------------------------------------------ *
 * IO queue lifecycle
 * ------------------------------------------------------------------------ */

int
ena_create_io_queues(struct ena_softc *sc)
{
	struct ena_com_dev *ena_dev = sc->sc_ena_dev;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct ena_com_create_io_ctx ctx;
	unsigned int i;
	int rc;

	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];
		uint16_t qid;

		if (ena_queue_alloc(sc, eq) != 0)
			goto destroy;

		eq->eq_ifq = ifp->if_ifqs[i];
		ifp->if_ifqs[i]->ifq_softc = eq;
		eq->eq_ifiq = ifp->if_iqs[i];

		/* TX queue. */
		memset(&ctx, 0, sizeof(ctx));
		qid = ENA_IO_TXQ_IDX(i);
		ctx.mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
		ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_TX;
		ctx.qid = qid;
		ctx.msix_vector = eq->eq_intr;
		ctx.queue_size = eq->eq_tx_ring_size;
		ctx.numa_node = -1;
		rc = ena_com_create_io_queue(ena_dev, &ctx);
		if (rc != 0)
			goto destroy;
		rc = ena_com_get_io_handlers(ena_dev, qid, &eq->eq_tx_sq,
		    &eq->eq_tx_cq);
		if (rc != 0) {
			ena_com_destroy_io_queue(ena_dev, qid);
			goto destroy;
		}

		/* RX queue. */
		memset(&ctx, 0, sizeof(ctx));
		qid = ENA_IO_RXQ_IDX(i);
		ctx.mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
		ctx.direction = ENA_COM_IO_QUEUE_DIRECTION_RX;
		ctx.qid = qid;
		ctx.msix_vector = eq->eq_intr;
		ctx.queue_size = eq->eq_rx_ring_size;
		ctx.numa_node = -1;
		rc = ena_com_create_io_queue(ena_dev, &ctx);
		if (rc != 0)
			goto destroy;
		rc = ena_com_get_io_handlers(ena_dev, qid, &eq->eq_rx_sq,
		    &eq->eq_rx_cq);
		if (rc != 0) {
			ena_com_destroy_io_queue(ena_dev, qid);
			goto destroy;
		}
	}

	return (0);

destroy:
	ena_destroy_io_queues(sc);
	return (ENXIO);
}

void
ena_destroy_io_queues(struct ena_softc *sc)
{
	struct ena_com_dev *ena_dev = sc->sc_ena_dev;
	unsigned int i;

	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];

		if (eq->eq_tx_sq != NULL) {
			ena_com_destroy_io_queue(ena_dev, ENA_IO_TXQ_IDX(i));
			eq->eq_tx_sq = NULL;
		}
		if (eq->eq_rx_sq != NULL) {
			ena_com_destroy_io_queue(ena_dev, ENA_IO_RXQ_IDX(i));
			eq->eq_rx_sq = NULL;
		}
		ena_queue_free(sc, eq);
	}
}

int
ena_queue_alloc(struct ena_softc *sc, struct ena_queue *eq)
{
	unsigned int i;

	eq->eq_tx_buf = mallocarray(eq->eq_tx_ring_size,
	    sizeof(struct ena_tx_buf), M_DEVBUF, M_WAITOK | M_ZERO);
	eq->eq_tx_free_ids = mallocarray(eq->eq_tx_ring_size,
	    sizeof(uint16_t), M_DEVBUF, M_WAITOK | M_ZERO);
	eq->eq_rx_buf = mallocarray(eq->eq_rx_ring_size,
	    sizeof(struct ena_rx_buf), M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < eq->eq_tx_ring_size; i++) {
		eq->eq_tx_free_ids[i] = i;
		if (bus_dmamap_create(sc->sc_dmat, ENA_MAX_FRAME_LEN,
		    ENA_PKT_MAX_BUFS, ENA_MAX_FRAME_LEN, 0,
		    BUS_DMA_WAITOK | BUS_DMA_64BIT,
		    &eq->eq_tx_buf[i].etx_map) != 0)
			goto fail;
	}
	for (i = 0; i < eq->eq_rx_ring_size; i++) {
		if (bus_dmamap_create(sc->sc_dmat, MCLBYTES, 1, MCLBYTES, 0,
		    BUS_DMA_WAITOK | BUS_DMA_64BIT,
		    &eq->eq_rx_buf[i].erx_map) != 0)
			goto fail;
	}

	eq->eq_tx_prod = 0;
	eq->eq_tx_cons = 0;
	eq->eq_tx_completions = 0;
	eq->eq_tx_stall_last = 0;
	eq->eq_tx_stall_ticks = 0;
	eq->eq_rx_prod = 0;
	eq->eq_rx_cons = 0;

	return (0);

fail:
	ena_queue_free(sc, eq);
	return (ENOMEM);
}

void
ena_queue_free(struct ena_softc *sc, struct ena_queue *eq)
{
	unsigned int i;

	if (eq->eq_rx_buf != NULL) {
		for (i = 0; i < eq->eq_rx_ring_size; i++) {
			struct ena_rx_buf *rb = &eq->eq_rx_buf[i];

			if (rb->erx_map != NULL) {
				if (rb->erx_mbuf != NULL) {
					bus_dmamap_unload(sc->sc_dmat,
					    rb->erx_map);
					m_freem(rb->erx_mbuf);
					rb->erx_mbuf = NULL;
				}
				bus_dmamap_destroy(sc->sc_dmat, rb->erx_map);
				rb->erx_map = NULL;
			}
		}
		free(eq->eq_rx_buf, M_DEVBUF,
		    eq->eq_rx_ring_size * sizeof(struct ena_rx_buf));
		eq->eq_rx_buf = NULL;
	}

	if (eq->eq_tx_buf != NULL) {
		for (i = 0; i < eq->eq_tx_ring_size; i++) {
			struct ena_tx_buf *tb = &eq->eq_tx_buf[i];

			if (tb->etx_map != NULL) {
				if (tb->etx_mbuf != NULL) {
					bus_dmamap_unload(sc->sc_dmat,
					    tb->etx_map);
					m_freem(tb->etx_mbuf);
					tb->etx_mbuf = NULL;
				}
				bus_dmamap_destroy(sc->sc_dmat, tb->etx_map);
				tb->etx_map = NULL;
			}
		}
		free(eq->eq_tx_buf, M_DEVBUF,
		    eq->eq_tx_ring_size * sizeof(struct ena_tx_buf));
		eq->eq_tx_buf = NULL;
	}

	if (eq->eq_tx_free_ids != NULL) {
		free(eq->eq_tx_free_ids, M_DEVBUF,
		    eq->eq_tx_ring_size * sizeof(uint16_t));
		eq->eq_tx_free_ids = NULL;
	}
}

/* ------------------------------------------------------------------------ *
 * RX datapath
 * ------------------------------------------------------------------------ */

void
ena_rx_fill(struct ena_queue *eq)
{
	struct ena_softc *sc = eq->eq_sc;
	struct ena_com_buf ebuf;
	struct mbuf *m;
	u_int slots;
	uint16_t req_id;
	int rc;

	MUTEX_ASSERT_LOCKED(&eq->eq_rx_mtx);

	slots = if_rxr_get(&eq->eq_rx_ring, eq->eq_rx_ring_size);
	if (slots == 0)
		return;

	while (slots > 0) {
		struct ena_rx_buf *rb;

		req_id = eq->eq_rx_prod;
		rb = &eq->eq_rx_buf[req_id];

		m = MCLGETL(NULL, M_DONTWAIT, MCLBYTES);
		if (m == NULL)
			break;
		m->m_len = m->m_pkthdr.len = MCLBYTES;

		if (bus_dmamap_load_mbuf(sc->sc_dmat, rb->erx_map, m,
		    BUS_DMA_NOWAIT) != 0) {
			m_freem(m);
			break;
		}
		bus_dmamap_sync(sc->sc_dmat, rb->erx_map, 0,
		    rb->erx_map->dm_mapsize, BUS_DMASYNC_PREREAD);

		rb->erx_mbuf = m;
		ebuf.paddr = rb->erx_map->dm_segs[0].ds_addr;
		ebuf.len = rb->erx_map->dm_segs[0].ds_len;

		rc = ena_com_add_single_rx_desc(eq->eq_rx_sq, &ebuf, req_id);
		if (rc != 0) {
			bus_dmamap_unload(sc->sc_dmat, rb->erx_map);
			m_freem(m);
			rb->erx_mbuf = NULL;
			break;
		}

		eq->eq_rx_prod = ENA_RX_RING_IDX_NEXT(req_id,
		    eq->eq_rx_ring_size);
		slots--;
	}

	if_rxr_put(&eq->eq_rx_ring, slots);

	if (if_rxr_inuse(&eq->eq_rx_ring) == 0) {
		timeout_add(&eq->eq_rx_refill, 1);
		return;
	}

	ena_com_write_sq_doorbell(eq->eq_rx_sq);
}

void
ena_rx_refill(void *arg)
{
	struct ena_queue *eq = arg;

	mtx_enter(&eq->eq_rx_mtx);
	ena_rx_fill(eq);
	mtx_leave(&eq->eq_rx_mtx);
}

int
ena_rxeof(struct ena_queue *eq)
{
	struct ena_softc *sc = eq->eq_sc;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	struct ena_com_rx_ctx rx_ctx;
	struct ena_com_rx_buf_info ena_bufs[ENA_PKT_MAX_BUFS];
	struct mbuf *m, *mh, *mt;
	unsigned int done = 0;
	int rc, budget = ENA_RX_CLEANUP_BUDGET;

	for (;;) {
		uint16_t req_id;
		unsigned int i;

		if (budget-- <= 0)
			break;

		memset(&rx_ctx, 0, sizeof(rx_ctx));
		rx_ctx.ena_bufs = ena_bufs;
		rx_ctx.max_bufs = ENA_PKT_MAX_BUFS;

		rc = ena_com_rx_pkt(eq->eq_rx_cq, eq->eq_rx_sq, &rx_ctx);
		if (rc != 0)
			break;
		if (rx_ctx.descs == 0)
			break;

		mh = mt = NULL;
		for (i = 0; i < rx_ctx.descs; i++) {
			struct ena_rx_buf *rb;

			req_id = ena_bufs[i].req_id;
			rb = &eq->eq_rx_buf[req_id];
			m = rb->erx_mbuf;
			rb->erx_mbuf = NULL;

			bus_dmamap_sync(sc->sc_dmat, rb->erx_map, 0,
			    rb->erx_map->dm_mapsize, BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->sc_dmat, rb->erx_map);

			m->m_len = ena_bufs[i].len;

			if (mh == NULL) {
				mh = mt = m;
				m->m_pkthdr.len = m->m_len;
			} else {
				m->m_flags &= ~M_PKTHDR;
				mt->m_next = m;
				mt = m;
				mh->m_pkthdr.len += m->m_len;
			}

			eq->eq_rx_cons = ENA_RX_RING_IDX_NEXT(eq->eq_rx_cons,
			    eq->eq_rx_ring_size);
			done++;
		}

		if (mh == NULL)
			break;

		/* RX checksum results. */
		if (rx_ctx.l3_proto == ENA_ETH_IO_L3_PROTO_IPV4 &&
		    !rx_ctx.l3_csum_err)
			SET(mh->m_pkthdr.csum_flags, M_IPV4_CSUM_IN_OK);
		if (rx_ctx.l4_csum_checked && !rx_ctx.l4_csum_err) {
			SET(mh->m_pkthdr.csum_flags,
			    M_TCP_CSUM_IN_OK | M_UDP_CSUM_IN_OK);
		}

		ml_enqueue(&ml, mh);
	}

	if (ifiq_input(eq->eq_ifiq, &ml))
		if_rxr_livelocked(&eq->eq_rx_ring);

	mtx_enter(&eq->eq_rx_mtx);
	if_rxr_put(&eq->eq_rx_ring, done);
	ena_rx_fill(eq);
	mtx_leave(&eq->eq_rx_mtx);

	return (done);
}

/* ------------------------------------------------------------------------ *
 * TX datapath
 * ------------------------------------------------------------------------ */

/*
 * Build the TX checksum-offload metadata for one packet.  The stack only
 * leaves M_*_CSUM_OUT set when this interface advertised the matching
 * IFCAP (in_proto_cksum_out()/in6_proto_cksum_out()/in_hdr_cksum_out()),
 * the L4 header directly follows the IP header (no IPv4 options, no IPv6
 * extension headers) and the frame is not bridged; in that case the L4
 * pseudo-header checksum has already been seeded into the packet, so the
 * device only completes it ("partial" L4 mode, l4_csum_partial = 1).  When
 * the headers cannot be parsed we leave meta_valid clear so the packet is
 * sent without offload rather than corrupted.
 */
void
ena_tx_csum(struct ena_com_tx_ctx *tx_ctx, struct mbuf *m)
{
	struct ether_extracted ext;
	int csum_flags = m->m_pkthdr.csum_flags;

	if (!ISSET(csum_flags, M_IPV4_CSUM_OUT | M_TCP_CSUM_OUT | M_UDP_CSUM_OUT))
		return;

	ether_extract_headers(m, &ext);

	if (ext.ip4 != NULL) {
		tx_ctx->l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
		tx_ctx->l3_csum_enable =
		    ISSET(csum_flags, M_IPV4_CSUM_OUT) ? 1 : 0;
		if (ISSET(ext.ip4->ip_off, htons(IP_DF)))
			tx_ctx->df = 1;
	} else if (ext.ip6 != NULL) {
		tx_ctx->l3_proto = ENA_ETH_IO_L3_PROTO_IPV6;
		tx_ctx->df = 1;
	} else {
		/* Not an IP packet we can describe; send without offload. */
		return;
	}

	if (ext.tcp != NULL && ISSET(csum_flags, M_TCP_CSUM_OUT)) {
		tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_TCP;
		tx_ctx->l4_csum_enable = 1;
		tx_ctx->l4_csum_partial = 1;
	} else if (ext.udp != NULL && ISSET(csum_flags, M_UDP_CSUM_OUT)) {
		tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_UDP;
		tx_ctx->l4_csum_enable = 1;
		tx_ctx->l4_csum_partial = 1;
	} else {
		/*
		 * L3-only offload (IPv4 header checksum), or the L4 header was
		 * not parsed: the device still needs the L3 metadata to place
		 * the IPv4 header checksum.
		 */
		tx_ctx->l4_proto = ENA_ETH_IO_L4_PROTO_UNKNOWN;
		tx_ctx->l4_csum_enable = 0;
	}

	tx_ctx->ena_meta.l3_hdr_offset = ext.evh != NULL ?
	    ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN : ETHER_HDR_LEN;
	tx_ctx->ena_meta.l3_hdr_len = ext.iphlen;
	tx_ctx->meta_valid = 1;
}

int
ena_encap(struct ena_queue *eq, struct mbuf *m)
{
	struct ena_softc *sc = eq->eq_sc;
	struct ena_com_tx_ctx tx_ctx;
	struct ena_tx_buf *tb;
	bus_dmamap_t map;
	uint16_t req_id;
	int nb_hw_desc, rc, i;

	req_id = eq->eq_tx_free_ids[eq->eq_tx_prod];
	tb = &eq->eq_tx_buf[req_id];
	map = tb->etx_map;

	rc = bus_dmamap_load_mbuf(sc->sc_dmat, map, m,
	    BUS_DMA_NOWAIT | BUS_DMA_STREAMING);
	if (rc == EFBIG) {
		if (m_defrag(m, M_DONTWAIT) != 0)
			return (ENOBUFS);
		rc = bus_dmamap_load_mbuf(sc->sc_dmat, map, m,
		    BUS_DMA_NOWAIT | BUS_DMA_STREAMING);
	}
	if (rc != 0)
		return (ENOBUFS);

	bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	for (i = 0; i < map->dm_nsegs; i++) {
		tb->etx_bufs[i].paddr = map->dm_segs[i].ds_addr;
		tb->etx_bufs[i].len = map->dm_segs[i].ds_len;
	}

	memset(&tx_ctx, 0, sizeof(tx_ctx));
	tx_ctx.ena_bufs = tb->etx_bufs;
	tx_ctx.num_bufs = map->dm_nsegs;
	tx_ctx.req_id = req_id;

	ena_tx_csum(&tx_ctx, m);

	if (!ena_com_sq_have_enough_space(eq->eq_tx_sq,
	    map->dm_nsegs + 1)) {
		bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, map);
		return (ENOBUFS);
	}

	rc = ena_com_prepare_tx(eq->eq_tx_sq, &tx_ctx, &nb_hw_desc);
	if (rc != 0) {
		bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, map);
		return (ENOBUFS);
	}

	tb->etx_mbuf = m;
	eq->eq_tx_prod = ENA_TX_RING_IDX_NEXT(eq->eq_tx_prod,
	    eq->eq_tx_ring_size);

	return (0);
}

void
ena_start(struct ifqueue *ifq)
{
	struct ena_queue *eq = ifq->ifq_softc;
	struct mbuf *m;
	int post = 0;

	if (!eq->eq_sc->sc_up) {
		ifq_purge(ifq);
		return;
	}

	for (;;) {
		if (ena_com_free_q_entries(eq->eq_tx_sq) < ENA_PKT_MAX_BUFS + 2) {
			ifq_set_oactive(ifq);
			break;
		}

		m = ifq_dequeue(ifq);
		if (m == NULL)
			break;

		if (ena_encap(eq, m) != 0) {
			m_freem(m);
			ifq->ifq_errors++;
			continue;
		}

#if NBPFILTER > 0
		if (ifq->ifq_if->if_bpf != NULL)
			bpf_mtap_ether(ifq->ifq_if->if_bpf, m, BPF_DIRECTION_OUT);
#endif
		post = 1;
	}

	if (post)
		ena_com_write_sq_doorbell(eq->eq_tx_sq);
}

int
ena_txeof(struct ena_queue *eq)
{
	struct ena_softc *sc = eq->eq_sc;
	struct ifqueue *ifq = eq->eq_ifq;
	unsigned int done = 0;
	int budget = ENA_TX_CLEANUP_BUDGET;

	for (;;) {
		struct ena_tx_buf *tb;
		uint16_t req_id;
		int rc;

		if (budget-- <= 0)
			break;

		rc = ena_com_tx_comp_req_id_get(eq->eq_tx_cq, &req_id);
		if (rc != 0)
			break;

		tb = &eq->eq_tx_buf[req_id];

		bus_dmamap_sync(sc->sc_dmat, tb->etx_map, 0,
		    tb->etx_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, tb->etx_map);
		m_freem(tb->etx_mbuf);
		tb->etx_mbuf = NULL;

		eq->eq_tx_free_ids[eq->eq_tx_cons] = req_id;
		eq->eq_tx_cons = ENA_TX_RING_IDX_NEXT(eq->eq_tx_cons,
		    eq->eq_tx_ring_size);

		ena_com_comp_ack(eq->eq_tx_sq, 1);
		done++;
	}

	if (done > 0) {
		/* Feed the IO-progress watchdog (ena_tick). */
		eq->eq_tx_completions += done;
		if (ifq_is_oactive(ifq))
			ifq_restart(ifq);
	}

	return (done);
}

/* ------------------------------------------------------------------------ *
 * interrupts
 * ------------------------------------------------------------------------ */

int
ena_intr_queue(void *arg)
{
	struct ena_queue *eq = arg;
	struct ena_softc *sc = eq->eq_sc;
	struct ena_eth_io_intr_reg intr_reg;
	int work = 0;

	if (!sc->sc_up)
		return (0);

	work |= ena_rxeof(eq);
	work |= ena_txeof(eq);

	/* Rearm the vector. */
	ena_com_update_intr_reg(&intr_reg, 64, 64, true, false);
	ena_com_unmask_intr(eq->eq_rx_cq, &intr_reg);

	return (work != 0);
}

int
ena_intr_admin(void *arg)
{
	struct ena_softc *sc = arg;
	struct ena_com_dev *ena_dev = sc->sc_ena_dev;

	ena_com_admin_q_comp_intr_handler(ena_dev);
	ena_com_aenq_intr_handler(ena_dev, sc);

	return (1);
}

/* ------------------------------------------------------------------------ *
 * watchdog / keep-alive / reset
 * ------------------------------------------------------------------------ */

void
ena_tick(void *arg)
{
	struct ena_softc *sc = arg;
	uint64_t now, timeout;
	unsigned int i;

	if (!sc->sc_up)
		return;

	/* Device promises a keep-alive AENQ every second; allow 6s slack. */
	now = getnsecuptime();
	timeout = sc->sc_keep_alive_ts + SEC_TO_NSEC(6);
	if ((int64_t)(now - timeout) > 0) {
		printf("%s: keep-alive watchdog timeout, resetting\n",
		    ENA_DEVNAME(sc));
		if (sc->sc_reset_tq != NULL)
			task_add(sc->sc_reset_tq, &sc->sc_reset_task);
		return;
	}

	/*
	 * IO-progress watchdog.  The keep-alive above rides the admin vector,
	 * which keeps answering while an IO queue is wedged (see
	 * ena_io_stall_analysis.md), so it cannot see a stalled ring.  Watch
	 * TX completions instead: TX and RX share one IO MSI-X vector, so a
	 * wedged queue stops reaping TX completions while descriptors are
	 * still posted.  RX is not a usable signal here -- an idle RX ring
	 * legitimately reaps nothing, which would false-trip a reset.  Reads
	 * are unlocked like the keep-alive check above; a one-tick skew
	 * against the IO handler is harmless against the multi-tick threshold.
	 */
	for (i = 0; i < sc->sc_nqueues; i++) {
		struct ena_queue *eq = &sc->sc_queues[i];
		int outstanding = (eq->eq_tx_prod != eq->eq_tx_cons);
		int progressed = (eq->eq_tx_completions != eq->eq_tx_stall_last);

		if (!outstanding || progressed) {
			eq->eq_tx_stall_last = eq->eq_tx_completions;
			eq->eq_tx_stall_ticks = 0;
			continue;
		}

		/* TX in flight but no completion reaped since last tick. */
		if (++eq->eq_tx_stall_ticks >= ENA_TX_STALL_TICKS) {
			printf("%s: TX queue %u stalled for %us, resetting\n",
			    ENA_DEVNAME(sc), i, eq->eq_tx_stall_ticks);
			if (sc->sc_reset_tq != NULL)
				task_add(sc->sc_reset_tq, &sc->sc_reset_task);
			return;
		}
	}

	timeout_add_sec(&sc->sc_tick, 1);
}

void
ena_watchdog(struct ifnet *ifp)
{
	/*
	 * Unused: if_timer is never armed.  IO-queue liveness (TX-progress)
	 * and the keep-alive AENQ are both checked from ena_tick() at 1Hz.
	 */
}

void
ena_reset_task(void *arg)
{
	struct ena_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	int s;

	s = splnet();
	if (ISSET(ifp->if_flags, IFF_RUNNING)) {
		ena_stop(sc);
		ena_init(sc);
	}
	splx(s);
}
