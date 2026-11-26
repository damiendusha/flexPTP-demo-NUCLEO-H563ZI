#include "fake_port.h"

#include <string.h>

#include <flexptp/event.h>
#include <flexptp/network_stack_driver.h>
#include <flexptp/task_ptp.h>
#include <flexptp/timeutils.h>

FakeCounters g_fake;
void (*g_fake_nsd_init_hook)(int tp);

/* Starts well away from zero, so a time computed by subtraction never goes negative. Reset by
   fake_reset(), so no test inherits a clock another test stepped. */
#define FAKE_EPOCH_NS (1700000000LL * 1000000000LL)
static int64_t s_now_ns = FAKE_EPOCH_NS;

int64_t fake_now_ns(void) { return s_now_ns; }
void fake_advance_ns(int64_t ns) { s_now_ns += ns; }

void fake_ptphw_init(uint32_t increment, uint32_t addend) {
    (void)increment;
    (void)addend;
}

void fake_set_clock(int64_t s, int64_t ns) {
    g_fake.clock_steps++;
    s_now_ns = s * 1000000000LL + ns;
}

void fake_set_addend(uint32_t addend) { (void)addend; }

void fake_get_time(void *p) {
    TimestampU *t = (TimestampU *)p;
    t->sec = (uint64_t)(s_now_ns / 1000000000LL);
    t->nanosec = (uint32_t)(s_now_ns % 1000000000LL);
}

void fake_reset(void) {
    memset(&g_fake, 0, sizeof(g_fake));
    s_now_ns = FAKE_EPOCH_NS;
    g_fake.last_nsd_tp = -2;
    g_fake_nsd_init_hook = NULL;
}

void fake_user_event(int uev) {
    if (uev >= 0 && uev < 32) {
        g_fake.events[uev]++;
    }
}

void fake_user_event_cb(PtpUserEventCode uev) { fake_user_event((int)uev); }

void fake_osless_lock(bool lock) { (void)lock; }

/* ---- network stack driver ---- */

static const uint8_t kOwnMac[6] = {0x56, 0x00, 0x1e, 0x00, 0x04, 0x51};

void ptp_nsd_init(const NsdInitSettings *init) {
    g_fake.nsd_inits++;
    g_fake.last_nsd_tp = (int)(int8_t)init->tp;
    if (g_fake_nsd_init_hook) {
        g_fake_nsd_init_hook((int)(int8_t)init->tp);
    }
}

void ptp_nsd_get_interface_address(uint8_t *hwa) { memcpy(hwa, kOwnMac, 6); }

void ptp_nsd_igmp_join_leave(bool join) { (void)join; }

/* Record the message and complete it the way a MAC does: every frame flexPTP sends is
   timestamped (nsd_lwip.c requests a timestamp for all of them), and the timestamp comes back
   through ptp_transmit_timestamp_cb() with the frame's uid. */
void ptp_nsd_transmit_msg(RawPtpMessage *pMsg, uint32_t uid) {
    const unsigned mt = pMsg->data[0] & 0x0F;
    g_fake.tx_by_type[mt]++;
    g_fake.last_tx_seq[mt] = (uint16_t)((pMsg->data[30] << 8) | pMsg->data[31]);
    TimestampU t;
    fake_get_time(&t);
    ptp_transmit_timestamp_cb(uid, (uint32_t)t.sec, t.nanosec);
}
