/* Recovery regression: execute the production functions with a wedged AQ. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IFF_RUNNING 1
#define IFF_UP 2
#define LINK_STATE_DOWN 0
#define ENA_REGS_RESET_GENERIC 0
#define ETHER_ADDR_LEN 6
#define ISSET(v, f) ((v) & (f))
#define ENA_DEVNAME(sc) "ena-test"
#define NET_LOCK() ((void)0)
#define NET_UNLOCK() ((void)0)
#define membar_producer() ((void)0)
#define membar_consumer() ((void)0)
#define membar_sync() ((void)0)

struct ifnet { int if_flags, if_link_state; unsigned int if_mtu; };
struct ena_com_dev { bool running, polling; };
struct ena_queue { unsigned int eq_kst_resets; };
struct ena_softc {
	struct { struct ifnet ac_if; unsigned char ac_enaddr[6]; } sc_arpcom;
	struct ena_com_dev *sc_ena_dev;
	struct ena_queue *sc_queues;
	unsigned int sc_nqueues, sc_tx_ring_size, sc_rx_ring_size;
	unsigned int sc_tx_offload_cap;
	unsigned int sc_admin_up;
	int sc_admin_initialized, sc_rss_ready, sc_up;
	void *sc_admin_ih;
};
struct ena_com_dev_get_features_ctx {
	struct { unsigned char mac_addr[6]; unsigned int max_mtu; } dev_attr;
	struct { unsigned int tx; } offload;
};

int resets, frees, admin_inits, interrupts, reset_error, init_error;
int feature_changed;
int quiesced, dma_stopped, missing_admin, io_live, admin_live;
struct ena_softc *current;

int ena_reset_device(struct ena_softc *);
int splnet(void) { return 0; }
void splx(int s) { (void)s; }
unsigned int atomic_load_int(unsigned int *p) { return *p; }
void atomic_store_int(unsigned int *p, unsigned int v) { *p = v; }
void intr_barrier(void *p) { (void)p; }
void if_link_state_change(struct ifnet *p) { (void)p; }
void ena_com_set_admin_running_state(struct ena_com_dev *d, bool v)
{ d->running = v; }
void ena_com_set_admin_polling_mode(struct ena_com_dev *d, bool v)
{ d->polling = v; }
void ena_com_admin_aenq_enable(struct ena_com_dev *d)
{ assert(d->running); }
void ena_com_admin_q_comp_intr_handler(struct ena_com_dev *d)
{ (void)d; assert(admin_live); interrupts++; }
void ena_com_aenq_intr_handler(struct ena_com_dev *d, void *p)
{ (void)d; (void)p; assert(admin_live); }
void ena_quiesce(struct ena_softc *sc)
{
	sc->sc_up = 0;
	sc->sc_arpcom.ac_if.if_flags &= ~IFF_RUNNING;
	quiesced = 1;
}
int ena_com_dev_reset(struct ena_com_dev *d, int reason)
{
	(void)reason;
	assert(quiesced && !current->sc_admin_up && !d->running);
	resets++;
	if (reset_error)
		return reset_error;
	dma_stopped = 1;
	missing_admin = 0;
	return 0;
}
void ena_destroy_io_queues(struct ena_softc *sc)
{
	(void)sc;
	assert(dma_stopped);
	io_live = 0;
	frees++;
}
void ena_com_rss_destroy(struct ena_com_dev *d)
{ (void)d; assert(dma_stopped); }
void ena_com_delete_host_info(struct ena_com_dev *d)
{ (void)d; assert(dma_stopped); }
void ena_com_admin_destroy(struct ena_com_dev *d)
{
	(void)d;
	assert(dma_stopped && !current->sc_admin_up);
	admin_live = 0;
}
int ena_admin_init(struct ena_softc *sc,
    struct ena_com_dev_get_features_ctx *feat)
{
	assert(!sc->sc_admin_up && !admin_live);
	admin_inits++;
	admin_live = 1;
	sc->sc_admin_initialized = 1;
	sc->sc_ena_dev->running = true;
	sc->sc_ena_dev->polling = true;
	memset(feat, 0, sizeof(*feat));
	feat->dev_attr.max_mtu = 9000;
	feat->offload.tx = sc->sc_tx_offload_cap;
	if (feature_changed)
		feat->dev_attr.mac_addr[0] = 1;
	return init_error;
}
unsigned int ena_calc_max_io_queues(struct ena_softc *sc,
    struct ena_com_dev_get_features_ctx *feat)
{ (void)feat; return sc->sc_nqueues; }
int ena_calc_io_queue_size(struct ena_softc *sc,
    struct ena_com_dev_get_features_ctx *feat)
{ (void)sc; (void)feat; return 0; }
/* The old recovery submits DESTROY_SQ to a nonresponsive admin queue. */
void ena_stop(struct ena_softc *sc) __attribute__((unused));
void ena_stop(struct ena_softc *sc)
{
	ena_quiesce(sc);
	if (missing_admin)
		sc->sc_ena_dev->running = false;
	io_live = 0;
	frees++;
}
int ena_init(struct ena_softc *sc)
{
	if (!sc->sc_ena_dev->running)
		return ENODEV;
	io_live = 1;
	sc->sc_up = 1;
	sc->sc_arpcom.ac_if.if_flags |= IFF_RUNNING;
	return 0;
}

#include "driver.c"

void setup(struct ena_softc *sc, struct ena_com_dev *dev,
    struct ena_queue *queues)
{
	memset(sc, 0, sizeof(*sc));
	memset(dev, 0, sizeof(*dev));
	memset(queues, 0, 2 * sizeof(*queues));
	current = sc;
	sc->sc_ena_dev = dev;
	sc->sc_queues = queues;
	sc->sc_nqueues = 2;
	sc->sc_admin_up = sc->sc_admin_initialized = sc->sc_up = 1;
	sc->sc_rss_ready = 1;
	sc->sc_arpcom.ac_if.if_flags = IFF_RUNNING | IFF_UP;
	sc->sc_arpcom.ac_if.if_mtu = 1500;
	dev->running = true;
	resets = frees = admin_inits = interrupts = reset_error = init_error = 0;
	feature_changed = 0;
	quiesced = dma_stopped = 0;
	missing_admin = io_live = admin_live = 1;
}

int main(void)
{
	struct ena_softc sc;
	struct ena_com_dev dev;
	struct ena_queue queues[2];

	setup(&sc, &dev, queues);
	ena_reset_task(&sc);
	if (!(sc.sc_arpcom.ac_if.if_flags & IFF_RUNNING)) {
		fprintf(stderr, "FAIL: watchdog left the interface down after AQ timeout\n");
		return 1;
	}
	assert(resets == 1 && admin_inits == 1 && frees == 1);
	assert(io_live && sc.sc_admin_up && !dev.polling);
	assert(queues[0].eq_kst_resets == 1 && queues[1].eq_kst_resets == 1);
	puts("PASS: timed-out admin queue is rebuilt before I/O restart");

	missing_admin = 1;
	dma_stopped = 0;
	ena_reset_task(&sc);
	assert(resets == 2 && admin_inits == 2 && frees == 2);
	assert(sc.sc_up && io_live && queues[0].eq_kst_resets == 2);
	puts("PASS: repeated watchdog recovery");

	setup(&sc, &dev, queues);
	dev.running = false;
	ena_reset_task(&sc);
	assert(sc.sc_up && resets == 1 && admin_inits == 1);
	puts("PASS: admin queue already marked dead");

	setup(&sc, &dev, queues);
	reset_error = ETIMEDOUT;
	ena_reset_task(&sc);
	assert(!sc.sc_up && !sc.sc_admin_up && !dev.running);
	assert(frees == 0 && io_live && admin_live && admin_inits == 0);
	assert(sc.sc_arpcom.ac_if.if_link_state == LINK_STATE_DOWN);
	assert(ena_intr_admin(&sc) == 0 && interrupts == 0);
	reset_error = 0;
	assert(ena_reset_device(&sc) == 0 && ena_init(&sc) == 0);
	assert(sc.sc_up && resets == 2 && frees == 1);
	puts("PASS: failed hardware reset retains DMA memory and permits retry");

	setup(&sc, &dev, queues);
	init_error = EIO;
	ena_reset_task(&sc);
	assert(!sc.sc_up && !sc.sc_admin_up && !dev.running);
	assert(!io_live && admin_live && frees == 1);
	assert(ena_intr_admin(&sc) == 0 && interrupts == 0);
	init_error = 0;
	assert(ena_reset_device(&sc) == 0 && ena_init(&sc) == 0);
	assert(sc.sc_up && resets == 2 && admin_inits == 2);
	puts("PASS: failed admin initialization remains interrupt-safe until retry");

	setup(&sc, &dev, queues);
	feature_changed = 1;
	ena_reset_task(&sc);
	assert(!sc.sc_up && !sc.sc_admin_up && !dev.running);
	assert(admin_live && !io_live);
	puts("PASS: changed device identity is rejected");

	setup(&sc, &dev, queues);
	sc.sc_arpcom.ac_if.if_flags &= ~IFF_RUNNING;
	ena_reset_task(&sc);
	assert(resets == 0 && frees == 0 && admin_inits == 0);
	puts("PASS: stale watchdog task leaves a stopped interface alone");

	setup(&sc, &dev, queues);
	assert(ena_intr_admin(&sc) == 1 && interrupts == 1);
	sc.sc_admin_up = 0;
	admin_live = 0;
	assert(ena_intr_admin(&sc) == 0 && interrupts == 1);
	puts("PASS: masked admin handler never touches destroyed rings");
	return 0;
}
