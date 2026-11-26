/* A fake hardware port and network-stack driver for running flexPTP's real engine on the host.
 *
 * flexPTP reaches its hardware only through the PTP_HW_* / PTP_SET_* macros in
 * flexptp_options.h, and the network only through network_stack_driver.h. This file supplies
 * both: a virtual clock the test advances by hand, and a driver that records what flexPTP
 * transmits and hands each frame's TX timestamp straight back, as a MAC would. Everything a
 * test needs to observe -- user events, clock steps, transmitted message types, NSD
 * re-initialisations -- is counted here.
 */
#ifndef FLEXPTP_TEST_FAKE_PORT_H
#define FLEXPTP_TEST_FAKE_PORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct TimestampU_;

/* ---- the virtual clock: nanoseconds since an arbitrary epoch ---- */
int64_t fake_now_ns(void);
void fake_advance_ns(int64_t ns);
void fake_ptphw_init(uint32_t increment, uint32_t addend);
void fake_set_clock(int64_t s, int64_t ns);   /* PTP_SET_CLOCK: a coarse step of the clock */
void fake_set_addend(uint32_t addend);
void fake_get_time(void *pTimestampU);        /* PTP_HW_GET_TIME */

/* ---- counters ---- */
typedef struct {
    uint32_t events[32];      /* per PtpUserEventCode */
    uint32_t clock_steps;     /* PTP_SET_CLOCK calls */
    uint32_t tx_by_type[16];  /* transmitted messages per messageType */
    uint32_t nsd_inits;       /* ptp_nsd_init() calls */
    int last_nsd_tp;          /* last ptp_nsd_init() transport, as int */
    uint16_t last_tx_seq[16]; /* sequenceId of the last message sent, per messageType */
} FakeCounters;

extern FakeCounters g_fake;
void fake_reset(void);

/* The user event callback named by PTP_USER_EVENT_CALLBACK. */
void fake_user_event(int uev);

/* Called from ptp_nsd_init(); a test may set it to act at that moment (e.g. during teardown). */
extern void (*g_fake_nsd_init_hook)(int tp);

/* The OS-less FIFO lock (FLEXPTP_OSLESS_LOCK). Single-threaded, so it only counts. */
void fake_osless_lock(bool lock);

#ifdef __cplusplus
}
#endif

#endif
