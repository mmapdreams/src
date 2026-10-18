/* Watchdog regression: execute ENA negotiation and tick with fake time. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BIT(n) (1U << (n))
#define SEC_TO_NSEC(n) ((uint64_t)(n) * 1000000000ULL)
#define ENA_DEVNAME(sc) "ena-test"
#define ENA_TX_STALL_TICKS 6
#define MSEC_TO_NSEC(n) ((uint64_t)(n) * 1000000ULL)
#define ENA_HW_HINTS_NO_TIMEOUT 0xFFFF
enum {
	ENA_ADMIN_LINK_CHANGE = 0,
	ENA_ADMIN_FATAL_ERROR = 1,
	ENA_ADMIN_WARNING = 2,
	ENA_ADMIN_KEEP_ALIVE = 4
};
struct ena_com_dev {
	int unused;
};
struct ena_queue {
	unsigned int eq_tx_prod, eq_tx_cons, eq_tx_stall_ticks;
	uint64_t eq_tx_completions, eq_tx_stall_last;
};
struct ena_softc {
	struct ena_com_dev *sc_ena_dev;
	struct ena_queue *sc_queues;
	unsigned int sc_nqueues;
	int sc_admin_initialized, sc_up, sc_reset_task, sc_tick;
	void *sc_reset_tq;
	uint64_t sc_keep_alive_ts, sc_keep_alive_timeout;
};
struct ena_com_dev_get_features_ctx {
	struct {
		uint32_t supported_groups;
	} aenq;
	struct {
		uint16_t driver_watchdog_timeout;
	} hw_hints;
};
static struct ena_com_dev_get_features_ctx offered;
static uint32_t enabled_groups;
static uint64_t fake_time;
static unsigned int resets, timer_arms;
static int ena_aenq_handlers;

static int
ena_com_validate_version(struct ena_com_dev *d)
{
	(void)d;
	return 0;
}

static int
ena_com_get_dma_width(struct ena_com_dev *d)
{
	(void)d;
	return 48;
}

static int
ena_com_admin_init(struct ena_com_dev *d, int *h)
{
	(void)d;
	(void)h;
	return 0;
}

static void
ena_com_set_admin_polling_mode(struct ena_com_dev *d, bool on)
{
	(void)d;
	(void)on;
}

static void
ena_config_host_info(struct ena_softc *sc)
{
	(void)sc;
}

static int
ena_com_get_dev_attr_feat(
    struct ena_com_dev *d, struct ena_com_dev_get_features_ctx *feat)
{
	(void)d;
	*feat = offered;
	return 0;
}

static int
ena_com_set_aenq_config(struct ena_com_dev *d, uint32_t groups)
{
	(void)d;
	enabled_groups = groups;
	return 0;
}

static void
ena_config_llq(struct ena_softc *sc, struct ena_com_dev_get_features_ctx *feat)
{
	(void)sc;
	(void)feat;
}

static uint64_t
getnsecuptime(void)
{
	return fake_time;
}

static void
task_add(void *tq, int *task)
{
	(void)tq;
	(void)task;
	resets++;
}

static void
timeout_add_sec(int *t, int sec)
{
	(void)t;
	(void)sec;
	timer_arms++;
}

#include "driver.c"

static struct ena_com_dev dev;
static struct ena_queue queue;
static struct ena_softc sc;

static void
offer(bool keep_alive, uint16_t hint)
{
	memset(&offered, 0, sizeof(offered));
	offered.aenq.supported_groups = BIT(ENA_ADMIN_LINK_CHANGE);
	if (keep_alive)
		offered.aenq.supported_groups |= BIT(ENA_ADMIN_KEEP_ALIVE);
	offered.hw_hints.driver_watchdog_timeout = hint;
}

static int
run_ns(const char *name, bool keep_alive, uint16_t hint, uint64_t ns,
    bool tx_stalled, unsigned int expected)
{
	struct ena_com_dev_get_features_ctx feat;
	unsigned int i, ticks = tx_stalled ? ENA_TX_STALL_TICKS : 1;
	int bad;

	memset(&dev, 0, sizeof(dev));
	memset(&queue, 0, sizeof(queue));
	memset(&sc, 0, sizeof(sc));

	offer(keep_alive, hint);
	sc.sc_ena_dev = &dev;
	sc.sc_queues = &queue;
	sc.sc_nqueues = 1;
	sc.sc_up = 1;
	sc.sc_reset_tq = &dev;
	queue.eq_tx_prod = tx_stalled ? 1 : 0;
	resets = timer_arms = 0;
	fake_time = 0;
	if (ena_admin_init(&sc, &feat) != 0)
		return 1;
	fake_time = ns;
	for (i = 0; i < ticks; i++)
		ena_tick(&sc);
	bad = resets != expected;
	if ((enabled_groups & BIT(ENA_ADMIN_KEEP_ALIVE)) !=
	    (keep_alive ? BIT(ENA_ADMIN_KEEP_ALIVE) : 0))
		bad = 1;
	if (expected == 0 && timer_arms != ticks)
		bad = 1;
	printf("%s: %s groups=0x%x hint=%u now_ns=%llu resets=%u expected=%u "
	       "arms=%u\n",
	    name, bad ? "FAIL" : "PASS", enabled_groups, hint,
	    (unsigned long long)ns, resets, expected, timer_arms);
	return bad;
}

static int
run_case(const char *name, bool keep_alive, uint16_t hint, unsigned int ms,
    bool tx_stalled, unsigned int expected)
{
	return run_ns(
	    name, keep_alive, hint, MSEC_TO_NSEC(ms), tx_stalled, expected);
}

/* Reset path: ena_admin_init again, then ena_up refreshes the timestamp. */
static int
run_reinit(void)
{
	struct ena_com_dev_get_features_ctx feat;
	unsigned int r[4];
	int bad;

	memset(&sc, 0, sizeof(sc));
	sc.sc_ena_dev = &dev;
	sc.sc_queues = &queue;
	sc.sc_nqueues = 1;
	sc.sc_up = 1;
	sc.sc_reset_tq = &dev;
	memset(&queue, 0, sizeof(queue));
	resets = timer_arms = 0;

	offer(true, 2000);
	fake_time = 0;
	ena_admin_init(&sc, &feat);
	fake_time = MSEC_TO_NSEC(2001);
	ena_tick(&sc);
	r[0] = resets;
	offer(false, 2000);
	ena_admin_init(&sc, &feat);
	sc.sc_keep_alive_ts = fake_time;
	fake_time += MSEC_TO_NSEC(60000);
	ena_tick(&sc);
	r[1] = resets;
	offer(true, 0);
	ena_admin_init(&sc, &feat);
	sc.sc_keep_alive_ts = fake_time;
	fake_time += SEC_TO_NSEC(6);
	ena_tick(&sc);
	r[2] = resets;
	fake_time += 1;
	ena_tick(&sc);
	r[3] = resets;
	bad = !(r[0] == 1 && r[1] == 1 && r[2] == 1 && r[3] == 2);
	printf("reset reinit KA 2s -> absent -> default: %s resets=%u,%u,%u,%u "
	       "expected=1,1,1,2\n",
	    bad ? "FAIL" : "PASS", r[0], r[1], r[2], r[3]);
	return bad;
}

int
main(void)
{
	int failed = 0;
	failed += run_case("keep-alive group absent", false, 0, 7000, false, 0);
	failed += run_case("no-timeout hint", true, 0xffff, 7000, false, 0);
	failed += run_case(
	    "15-second hint before deadline", true, 15000, 7000, false, 0);
	failed += run_case(
	    "2-second hint after deadline", true, 2000, 3000, false, 1);
	failed += run_ns("2-second hint at deadline", true, 2000,
	    MSEC_TO_NSEC(2000), false, 0);
	failed += run_ns("2-second hint 1ns past deadline", true, 2000,
	    MSEC_TO_NSEC(2000) + 1, false, 1);
	failed += run_ns(
	    "zero hint at 6s deadline", true, 0, SEC_TO_NSEC(6), false, 0);
	failed += run_ns(
	    "zero hint 1ns past 6s", true, 0, SEC_TO_NSEC(6) + 1, false, 1);
	failed += run_ns("max finite hint 65534ms at deadline", true, 0xfffe,
	    MSEC_TO_NSEC(0xfffe), false, 0);
	failed += run_ns("max finite hint 65534ms 1ns past", true, 0xfffe,
	    MSEC_TO_NSEC(0xfffe) + 1, false, 1);
	failed += run_case("1ms hint after deadline", true, 1, 2, false, 1);
	failed += run_case(
	    "no-timeout hint long idle", true, 0xffff, 3600000, false, 0);
	failed += run_case(
	    "KA absent, TX stall still resets", false, 0, 1000, true, 1);
	failed += run_case("KA no-timeout, TX stall still resets", true, 0xffff,
	    1000, true, 1);
	failed += run_reinit();
	failed +=
	    run_case("zero hint retains default", true, 0, 7000, false, 1);
	failed += run_case(
	    "TX watchdog with keep-alive group absent", false, 0, 100, true, 1);
	printf("failed=%d cases=17\n", failed);
	return failed != 0;
}
