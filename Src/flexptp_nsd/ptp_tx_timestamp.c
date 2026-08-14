/* Moves flexPTP's transmit-timestamp callback out of interrupt context.
 *
 * THIS FILE EXISTS BECAUSE OF ONE DEFECT, AND IT IS THE SHARPEST IN THE WHOLE PORT.
 *
 * flexPTP's CMSIS-RTOS2 binding does this, unconditionally, in ptp_transmit_timestamp_cb():
 *
 *     osMessageQueuePut(sTxCbFIFO, &ts, 0, osWaitForever);
 *     osMessageQueuePut(sNotificationFIFO, &notif, 0, osWaitForever);
 *
 * On this hardware that function is reached from interrupt context: the MAC captures the
 * transmit timestamp, ETHHW_ProcessTx() walks the ring inside ETH_IRQHandler, and invokes the
 * per-descriptor callback there. CMSIS-RTOS2 forbids a non-zero timeout from an ISR, and ST's
 * ThreadX wrapper enforces it:
 *
 *     if ((IS_IRQ_MODE() && (timeout != 0)) || ...) { status = osErrorParameter; }
 *
 * flexPTP does not check the return value. So every transmit timestamp would be discarded in
 * silence -- t3 would never reach the servo, mean path delay would never be computed, and the
 * clock would converge on a lie while every visible indicator stayed green. That is the same
 * shape as F59, and it is why tools/hil/ checks ptp_ts BEFORE ptp_sync.
 *
 * flexPTP's FreeRTOS binding gets this right (it branches on xPortIsInsideInterrupt() and uses
 * xQueueSendFromISR), so the defect is specific to the binding this port must use. Fixing it
 * upstream would be better; doing it here keeps the submodule unpatched and is the reason the
 * flexPTP pin needs no fourth patch.
 *
 * The deferral is one queue and one thread. The ISR posts with timeout 0 -- legal -- and the
 * thread makes the call flexPTP expects from thread context.
 */

#include "ptp_tx_timestamp.h"

#include <string.h>

#include <cmsis_os2.h>

#include "flexptp/task_ptp.h"
#include "standard_output/standard_output.h"

/* One entry per outstanding timestamped transmit. flexPTP's own TX_CALLBACK_FIFO_LENGTH is
   10 and it never has more than a couple of Delay_Reqs in flight, so 8 is generous. */
#define PTP_TX_TS_QUEUE_LEN (8)

typedef struct {
    uint32_t uid;
    uint32_t sec;
    uint32_t nanosec;
} PtpTxTs;

static osMessageQueueId_t s_queue;
static osThreadId_t s_thread;

/* Diagnostics. Counted rather than printed: this is an ISR path, and printing from one is
   what finding F68 was about. Surfaced by `ptp diag ts`. */
static volatile uint32_t s_posted;
static volatile uint32_t s_dropped;
static volatile uint32_t s_delivered;

static void ptp_tx_ts_thread(void *arg) {
    (void)arg;

    for (;;) {
        PtpTxTs ts;
        if (osMessageQueueGet(s_queue, &ts, NULL, osWaitForever) != osOK) {
            continue;
        }

        s_delivered++;

        /* Thread context, so flexPTP's osWaitForever enqueues are legal here. */
        ptp_transmit_timestamp_cb(ts.uid, ts.sec, ts.nanosec);
    }
}

bool ptp_tx_timestamp_init(void) {
    if (s_queue != NULL) {
        return true;
    }

    s_queue = osMessageQueueNew(PTP_TX_TS_QUEUE_LEN, sizeof(PtpTxTs), NULL);
    if (s_queue == NULL) {
        MSG("PTP: could not create the transmit-timestamp queue\n");
        return false;
    }

    osThreadAttr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.name = "ptpts";
    attr.stack_size = 1024;
    /* Above the PTP thread it feeds, so a timestamp is handed over promptly rather than
       waiting behind a servo iteration. Accuracy does not depend on this -- the timestamp was
       captured in hardware long before -- but latency to the servo does. */
    attr.priority = osPriorityAboveNormal;

    s_thread = osThreadNew(ptp_tx_ts_thread, NULL, &attr);
    if (s_thread == NULL) {
        MSG("PTP: could not create the transmit-timestamp thread\n");
        return false;
    }

    return true;
}

void ptp_tx_timestamp_from_isr(uint32_t ts_s, uint32_t ts_ns, void *tag) {
    if (s_queue == NULL) {
        return;
    }

    PtpTxTs ts = {.uid = (uint32_t)(uintptr_t)tag, .sec = ts_s, .nanosec = ts_ns};

    s_posted++;

    /* Timeout 0. The whole point of this file: any other value is rejected outright from
       interrupt context, which is the defect described at the top. */
    if (osMessageQueuePut(s_queue, &ts, 0, 0) != osOK) {
        s_dropped++;
    }
}

void ptp_tx_timestamp_stats(uint32_t *posted, uint32_t *delivered, uint32_t *dropped) {
    if (posted != NULL) {
        *posted = s_posted;
    }
    if (delivered != NULL) {
        *delivered = s_delivered;
    }
    if (dropped != NULL) {
        *dropped = s_dropped;
    }
}
