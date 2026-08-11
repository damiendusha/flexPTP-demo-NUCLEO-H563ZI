#include "mac_drv.h"
#include "standard_output/standard_output.h"
#include "stm32h5xx.h"

#include <cmsis_compiler.h> // __get_PRIMASK, __disable_irq, __enable_irq

#include <memory.h>

#include <stm32h5xx_hal.h>

/* Every hardware handshake in this driver is "set a bit, wait for the hardware to clear it".
 * None of them may spin forever. An unbounded wait turns any clock, supply or PHY fault into
 * a silent hang of the calling thread -- which is exactly how F53 presented: the flexPTP task
 * stopped, the PTP CLI went dead, and nothing said why. Bounded, and loud on timeout.
 *
 * The bound is a spin count rather than a time: these handshakes resolve in nanoseconds, this
 * runs before any timebase is guaranteed, and it must not depend on a timebase. At 250 MHz the
 * loop below is a few milliseconds -- six orders of magnitude of headroom.
 *
 * NOTE on the report: every caller of this helper runs in thread context (initialisation and
 * the MDIO accessors), which is what makes the MSG() legitimate. An earlier version of this
 * comment claimed the reports were ISR-callable; they are not, and neither is MSG(). Console
 * output from an interrupt is refused and counted -- see the contract note on
 * serial_io_write() and finding N6. Interrupt paths latch and let a thread report. */
#define ETHHW_HW_TIMEOUT_SPINS (100000UL)

static bool ETHHW_WaitBitsClear(volatile uint32_t *reg, uint32_t mask, const char *what) {
    for (uint32_t i = 0; i < ETHHW_HW_TIMEOUT_SPINS; i++) {
        if ((*reg & mask) == 0U) {
            return true;
        }
    }

    MSG("ETH: timed out waiting for %s to clear\n", what);
    return false;
}

__weak uint32_t ETHHW_setupPHY(ETH_TypeDef *eth) {
    (void)eth;
    return MODEINIT_FULL_DUPLEX | MODEINIT_SPEED_100MBPS;
}

// ----------------

static void ETHHW_InitClocks() {
    GPIO_InitTypeDef GPIO_InitStructure;

    /* Ethernett MSP init: RMII Mode */

    /* Enable GPIOs clocks */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* Ethernet pins configuration ************************************************/
    /*
     RMII_REF_CLK ----------------------> PA1
     RMII_MDIO -------------------------> PA2
     RMII_MDC --------------------------> PC1
     RMII_MII_CRS_DV -------------------> PA7
     RMII_MII_RXD0 ---------------------> PC4
     RMII_MII_RXD1 ---------------------> PC5
     RMII_MII_TX_EN --------------------> PG11
     RMII_MII_TXD0 ---------------------> PG13
     RMII_MII_TXD1 ---------------------> PB15   (PB13 on the H743 board)

     Not configured here:
       RMII_MII_RXER  - PG2 on the H743 board; dropped, see docs/design_doc.md
       PPS_OUT        - PG8; owned by the flexPTP hardware port, not this driver
     */

    /* Configure PA1, PA2 and PA7 */
    GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStructure.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStructure.Pull = GPIO_NOPULL;
    GPIO_InitStructure.Alternate = GPIO_AF11_ETH;
    GPIO_InitStructure.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* Configure PB15 (ETH_TXD1; this is PB13 on the NUCLEO-H743ZI) */
    GPIO_InitStructure.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStructure);

    /* Configure PC1, PC4 and PC5 */
    GPIO_InitStructure.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* Configure PG11 and PG13 (PG2/RXER is not brought out on this board) */
    GPIO_InitStructure.Pin = GPIO_PIN_11 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStructure);

    /* Enable the Ethernet global Interrupt */
    HAL_NVIC_SetPriority(ETH_IRQn, 0x7, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);

    /* Enable Ethernet clocks (H5 drops the "1" from the H7 names) */
    __HAL_RCC_ETH_CLK_ENABLE();
    __HAL_RCC_ETHTX_CLK_ENABLE();
    __HAL_RCC_ETHRX_CLK_ENABLE();

}

#ifndef CEIL_TO_4
#define CEIL_TO_4(x) ((((x) >> 2) + (((x) & 0b11) ? 1 : 0)) << 2)
#endif

static void ETHHW_InitPeripheral(ETH_TypeDef *eth, ETHHW_InitOpts *init) {
    /* SYSCFG is called SBS on the STM32H5 */
    __HAL_RCC_SBS_CLK_ENABLE();
    HAL_SBS_ETHInterfaceSelect(SBS_ETH_RMII);

    // reset all MAC internal registers and logic
    SET_BIT(eth->DMAMR, ETH_DMAMR_SWR);

    // wait for reset completion
    ETHHW_WaitBitsClear(&eth->DMAMR, ETH_DMAMR_SWR, "DMA software reset");

    /* ---- MAC initialization ---- */

    // Initialize the MDIO clock. ETH_MACMDIOAR_CR_DIV102 is already a positioned mask
    // (bit 10), so it must NOT be shifted by ETH_MACMDIOAR_CR_Pos again -- doing so lands
    // in the RDA field and leaves CR at 0 (divide by 42). At HCLK 250 MHz that would give
    // ~6 MHz MDC against a 2.5 MHz limit. 250/102 = 2.45 MHz.
    WRITE_REG(eth->MACMDIOAR, ETH_MACMDIOAR_CR_DIV102);

    // setup PHY
    uint32_t initMode = ETHHW_setupPHY(eth);

    // fill-in MAC address
    uint32_t hwaTmp = 0;
    memcpy(&hwaTmp, init->mac, 4);
    WRITE_REG(eth->MACA0LR, hwaTmp);
    hwaTmp = 0;
    memcpy(&hwaTmp, init->mac + 4, 2);
    WRITE_REG(eth->MACA0HR, hwaTmp);

    // receive using perfect matching and multicast matching
    SET_BIT(eth->MACPFR, ETH_MACPFR_PM | ETH_MACPFR_HPF);

    // create a mode bit subfield based on the ETHHW_setup() return value
    uint32_t mode = 0;
    if (initMode & MODEINIT_FULL_DUPLEX) { // duplex mode
        mode |= ETH_MACCR_DM;
    }
    if (initMode & MODEINIT_SPEED_100MBPS) { // Fast Ethernet
        mode |= ETH_MACCR_FES;
    }

    // set speed and duplex mode AND turn on checksum validation
    WRITE_REG(eth->MACCR, mode | ETH_MACCR_IPC);

    /* ---- MTL initialization ---- */
    WRITE_REG(eth->MTLTQOMR, (0b111 << 16) | ETH_MTLTQOMR_TSF | (0b10 << 2)); // transmit store and forward ON and enable transmit queue
    WRITE_REG(eth->MTLRQOMR, ETH_MTLRQOMR_RSF);                               // receive store and forward ON

    /* ---- DMA configuration ---- */

    // configure DMA system bus mode TODO
    // calculate aligned buffer size
    uint16_t alignedBufSize = CEIL_TO_4(init->blockSize);

    // create RX descriptors; use Extended Descriptors
    uint16_t byteSkip = sizeof(ETHHW_DescExt);                // calculate skip size in bytes
    uint16_t dwordSkip = byteSkip / 4;                        // calculate skip size in dwords (32-bit units), used by the DMA
    ETHHW_DescFull *ring = (ETHHW_DescFull *)init->rxRingPtr; // acquire RX ring begin pointer
    uint8_t *rxBuf = init->bufPtr;

    // fill RX descriptor area
    memset(init->rxRingPtr, 0, sizeof(ETHHW_DescFull) * init->rxRingLen); // clear descriptors
    for (uint16_t i = 0; i < init->rxRingLen; i++) {
        ETHHW_DescFull *bd = ring + i;                                                            // acquire descriptor at the beginning of the ring item
        bd->ext.bufAddr = ((uint32_t)(rxBuf)) + alignedBufSize * i;                               // compute buffer start and store it for subsequent use when the
                                                                                                  // descriptor field get overwritten by the DMA
        bd->desc.DES0 = bd->ext.bufAddr;                                                          // store Buffer 1 address
        bd->desc.DES3 = 0 | ETH_DMARXNDESCRF_OWN | ETH_DMARXNDESCRF_IOC | ETH_DMARXNDESCRF_BUF1V; // set flags: OWN, IOC, BUF1V
    }

    // write receive-related registers
    WRITE_REG(eth->DMACRDRLR, init->rxRingLen - 1);       // write ring length
    WRITE_REG(eth->DMACRDLAR, (uint32_t)init->rxRingPtr); // ring start address
    // WRITE_REG(eth->DMACRDTPR, ((uint32_t ) init->rxRingPtr) + (init->rxRingLen)
    // * byteSkip); // tail pointer
    WRITE_REG(eth->DMACRDTPR, 0); // tail pointer WON'T STOP

    // transmit descriptor initialization
    memset(init->txRingPtr, 0, sizeof(ETHHW_DescFull) * init->txRingLen); // clear everything
    ring = (ETHHW_DescFull *)init->txRingPtr;
    uint8_t *txBuf = init->bufPtr + alignedBufSize * init->rxRingLen; // fill in later used buffer addresses
    for (uint16_t i = 0; i < init->txRingLen; i++) {
        ETHHW_DescFull *bd = ring + i;
        bd->ext.bufAddr = ((uint32_t)txBuf) + i * alignedBufSize;
    }

    // write transmit-related registers
    WRITE_REG(eth->DMACTDRLR, init->txRingLen - 1);       // write ring length
    WRITE_REG(eth->DMACTDLAR, (uint32_t)init->txRingPtr); // ring start address
    WRITE_REG(eth->DMACTDTPR, 0);                         // tail pointer WON'T STOP

    // set common DMA-related options
    WRITE_REG(eth->DMACCR, ((dwordSkip & 0b111) << ETH_DMACCR_DSL_Pos));                         // set skip to the size of the extension
    WRITE_REG(eth->DMACTCR, ETH_DMACTCR_TPBL_32PBL);                                             // 32 beats per DMA transfer (TX) [DO NOT START!]
    WRITE_REG(eth->DMACRCR, ETH_DMACRCR_RPBL_32PBL | (init->blockSize << ETH_DMACRCR_RBSZ_Pos)); // set beats per DMA transfer to 32 (RX) and write receive buffer size [DO NOT START!]
}

static ETHHW_State *ETHHW_GetState(ETH_TypeDef *eth) {
    return ((ETHHW_State *)eth->DMACRDLAR) - 1;
}

static void ETHHW_InitState(ETH_TypeDef *eth) {
    ETHHW_State *state = ETHHW_GetState(eth);
    state->nextTxDescIdx = 0;
    state->txCntSent = 0;
    state->txCntAcked = 0;
}

void ETHHW_Init(ETH_TypeDef *eth, ETHHW_InitOpts *init) {
    ETHHW_InitClocks();
    ETHHW_InitPeripheral(eth, init);
    ETHHW_InitState(eth);
}

void ETHHW_Start(ETH_TypeDef *eth) {
    SET_BIT(eth->DMACIER, ETH_DMACIER_NIE); // normal interrupt enable
    SET_BIT(eth->DMACIER, ETH_DMACIER_RIE); // receive interrupt enable
    SET_BIT(eth->DMACIER, ETH_DMACIER_TIE); // transmit interrupt enable

    /* Abnormal interrupts. Without AIE and these per-condition enables, DMACSR.AIS never
     * asserts -- the summary bit is the OR of the *unmasked* conditions only -- so the error
     * branch in ETHHW_ISR() was unreachable from the day it was written. That is why a ring
     * exhaustion could stop reception permanently and nothing ever said a word: the driver
     * could not observe the condition, let alone recover from it (finding N3).
     *
     * TBUE is deliberately NOT enabled. Transmit-buffer-unavailable is raised every time the
     * TX DMA runs out of work, which is the normal idle transition on a lightly loaded link;
     * enabling it would report a fault several times a second. The genuinely abnormal
     * conditions are here. */
    SET_BIT(eth->DMACIER, ETH_DMACIER_AIE |   // abnormal interrupt summary
                              ETH_DMACIER_RBUE | // receive buffer unavailable
                              ETH_DMACIER_FBEE | // fatal bus error
                              ETH_DMACIER_RSE |  // receive process stopped
                              ETH_DMACIER_TXSE | // transmit process stopped
                              ETH_DMACIER_CDEE | // context descriptor error
                              ETH_DMACIER_RWTE); // receive watchdog timeout

    // start DMA reception and transmission
    SET_BIT(eth->DMACRCR, ETH_DMACRCR_SR);
    SET_BIT(eth->DMACTCR, ETH_DMACTCR_ST);

    // ----------------------------------

    // start MAC transceiver
    SET_BIT(eth->MACCR, ETH_MACCR_RE);
    SET_BIT(eth->MACCR, ETH_MACCR_TE);
}

__weak int ETHHW_EventCallback(ETHHW_EventDesc *evt) {
    (void)evt;
    return 0;
}

__weak int ETHHW_ReadCallback(ETHHW_EventDesc *evt) {
    (void)evt;
    return ETHHW_RET_RX_PROCESSED;
}

ETHHW_DescFull *ETHHW_AdvanceDesc(ETHHW_DescFull *start, uint16_t n, ETHHW_DescFull *bd, int delta) {
    int16_t index = (((uint32_t)(bd)) - ((uint32_t)(start))) / sizeof(ETHHW_DescFull);
    // int16_t startIndex = index;
    index = ((int)index + delta) % n;
    index = (index < 0) ? (index + n) : index;
    // MSG("%d +(%d) = %d mod %u\n", startIndex, delta, index, n);
    return start + index;
}

#define ETHHW_DESC_PREV(s, n, p) ETHHW_AdvanceDesc((s), (n), (p), -1)
#define ETHHW_DESC_NEXT(s, n, p) ETHHW_AdvanceDesc((s), (n), (p), 1)

void ETHHW_RestoreRXDesc(ETHHW_DescFull *bd) {
    if (bd == NULL) {
        return;
    }

    bd->desc.DES0 = bd->ext.bufAddr; // store Buffer 1 address

    // The buffer address must be visible to the DMA before the descriptor is handed over.
    // `volatile` orders these two stores as far as the compiler is concerned; it says nothing
    // about the store buffer or the fabric, which is what the barrier is for (finding N9).
    __DMB();

    bd->desc.DES3 = 0 | ETH_DMARXNDESCRF_OWN | ETH_DMARXNDESCRF_IOC | ETH_DMARXNDESCRF_BUF1V; // set flags: OWN, IOC, BUF1V

    // Clear the in-flight marker only AFTER ownership has gone back to the DMA. The reverse
    // order leaves a window in which an RX interrupt sees the descriptor as free and fresh
    // and re-delivers a stale frame; this order makes it see OWN set, and skip it.
    bd->ext.inFlight = 0;

    /* Receive poll demand.
     *
     * Returning ownership is not enough to restart a receive engine that has already given up.
     * When the DMA reaches a descriptor the application still owns it raises RBU and suspends,
     * and it does not re-examine the ring on its own -- the tail pointer has to be written
     * again. Nothing did that: DMACRDTPR was written once at init and never touched, so a
     * single momentary exhaustion of the ring killed reception permanently. Measured: with
     * rx_thread stalled for 500 ms under load the link stayed up, the console stayed alive,
     * and not one further frame was ever received (finding N3).
     *
     * ETH rather than a parameter: this is called from the receive thread as well as from
     * ETHHW_ProcessRx(), the part has exactly one Ethernet peripheral, and threading the
     * handle through the callback boundary buys nothing. One 32-bit store per restored
     * descriptor, unconditional -- cheaper than testing whether it is needed, and correct
     * even for an RBU that arrived while flags were being cleared. */
    __DSB(); // ownership visible before the DMA is asked to look again
    WRITE_REG(ETH->DMACRDTPR, 0);
}

typedef enum { ETHHW_RINGBUF_RX,
               ETHHW_RINGBUF_TX } ETHHW_RingBufId;

// Diagnostic dumper. Call sites below are commented out by default; kept because it is
// the quickest way to inspect descriptor ownership when the rings misbehave.
__attribute__((unused)) static void ETHHW_PrintRingBufStatus(ETH_TypeDef *eth,
                                     ETHHW_RingBufId ringBufId) {
    ETHHW_DescFull *ring = NULL;
    ETHHW_DescFull *currentPtr = NULL;
    uint16_t n = 0;

    // fetch pointers based on ringbuffer ID
    if (ringBufId == ETHHW_RINGBUF_RX) {
        ring = (ETHHW_DescFull *)eth->DMACRDLAR;
        n = eth->DMACRDRLR + 1;
        currentPtr = (ETHHW_DescFull *)eth->DMACCARDR;
        MSG("RX: ");
    } else if (ringBufId == ETHHW_RINGBUF_TX) {
        ring = (ETHHW_DescFull *)eth->DMACTDLAR;
        n = eth->DMACTDRLR + 1;
        currentPtr = (ETHHW_DescFull *)eth->DMACCATDR;
        MSG("TX: ");
    }

    uint16_t current = n + 1;              // set current to something non-possible
    uint32_t DES3 = ring[n - 1].desc.DES3; // extract DES3

#define IS_DESC_EMPTY(DES3)                                            \
    ((ringBufId == ETHHW_RINGBUF_RX) ? ((DES3) & ETH_DMARXNDESCRF_OWN) \
                                     : !((DES3) & ETH_DMARXNDESCRF_OWN))

    // packet spanning over multiple descriptors; examine last descriptor first to
    // check if first descriptor is an intermediate one
    bool multiDescPkt = !(IS_DESC_EMPTY(DES3)) && !(DES3 & ETH_DMARXNDESCWBF_LD);

    MSG("|");
    for (uint16_t i = 0; i < n; i++) {
        if ((ring + i) == currentPtr) { // search current descriptor (next descriptor to safe in)
            current = i;
        }

        DES3 = ring[i].desc.DES3;             // fetch DES3 dword
        bool descEmpty = IS_DESC_EMPTY(DES3); // determine if descriptor is empty

        if (descEmpty) { // descriptor empty (rx: "armed", tx: not yet passed to the DMA)
            if ((ringBufId == ETHHW_RINGBUF_TX) && (ring[i].ext.tsCbPtr != 0) && (DES3 & ETH_DMATXNDESCWBF_TTSS)) {
                MSG("t");
            } else {
                MSG("-");
            }
        } else { // descriptor non-empty (rx: packet stored, tx: packet passed to
                 // the DMA)
            // multidescriptor packets
            bool packetBoundary = false;

            // detect first descriptor
            if ((DES3 & ETH_DMARXNDESCWBF_FD) && !((DES3 & ETH_DMARXNDESCWBF_LD))) {
                multiDescPkt = true;
                packetBoundary = true;
            }

            // detect last descriptor
            if (!(DES3 & ETH_DMARXNDESCWBF_FD) && ((DES3 & ETH_DMARXNDESCWBF_LD))) {
                multiDescPkt = false;
                packetBoundary = true;
            }

            // print mark accordingly...
            if (DES3 & ETH_DMARXNDESCWBF_CTXT) { // context descriptor
                MSG("C");
            } else {                                  // normal descriptor
                if (packetBoundary && multiDescPkt) { // first descriptor
                    MSG(">");
                } else if (packetBoundary && !multiDescPkt) { // last descriptor
                    MSG("<");
                } else { // intermediate or non-multi descriptor
                    if ((ringBufId == ETHHW_RINGBUF_TX) && (ring[i].ext.tsCbPtr != 0) &&
                        (ring[i].desc.DES2 & ETH_DMATXNDESCRF_TTSE)) {
                        MSG("T");
                    } else {
                        MSG("x");
                    }
                }
            }
        }

        // print connection between descriptors
        if (multiDescPkt) { // multi-descriptor packets
            MSG("=");
        } else { // single descriptor packets
            MSG(" ");
        }
    }
    MSG("|\n");

    // mark current descriptor position
    MSG("%*c", (uint32_t)2 * current + 5, ' ');
    MSG("^\n");
}

#define ETHHW_DESC_OWNED_BY_APPLICATION(bd) \
    (!(((bd)->desc.DES3) & ETH_DMARXNDESCRF_OWN))

// A descriptor needs processing only if the application owns it AND it is not already queued
// with the receive thread. See ETHHW_DescExt.inFlight and F61.
#define ETHHW_DESC_NEEDS_PROCESSING(bd) \
    (ETHHW_DESC_OWNED_BY_APPLICATION(bd) && !((bd)->ext.inFlight))

// process incoming packet
void ETHHW_ProcessRx(ETH_TypeDef *eth) {
    // ETHHW_PrintRingBufStatus(eth, ETHHW_RINGBUF_RX);

    ETHHW_DescFull *ring = (ETHHW_DescFull *)eth->DMACRDLAR;
    uint16_t ringLen = eth->DMACRDRLR + 1;

    // get current descriptor
    ETHHW_DescFull *bd = (ETHHW_DescFull *)eth->DMACCARDR;
    ETHHW_DescFull *bd_prev = ETHHW_DESC_PREV(ring, ringLen, bd);

    // get the first unprocessed descriptor (oldest one). An in-flight descriptor is a hard
    // boundary: it is older than anything fresh, so stopping there is the correct start.
    while (ETHHW_DESC_NEEDS_PROCESSING(bd_prev)) {
        bd = bd_prev;
        bd_prev = ETHHW_DESC_PREV(ring, ringLen, bd);
    }

    // iterate over unprocessed descriptors
    while (ETHHW_DESC_NEEDS_PROCESSING(bd)) {
        ETHHW_DescFull *bd_next = ETHHW_DESC_NEXT(ring, ringLen, bd);

        /* Zeroed because ts_s/ts_ns are only assigned when a context descriptor carried a
         * timestamp, but rx_thread copies them into pbuf->time_s/time_ns unconditionally.
         * Left uninitialised, a frame the MAC did not timestamp handed flexPTP whatever was
         * on the stack -- a plausible-looking wrong time rather than an obvious zero, which
         * is the same class of trap as F59's NaN printing as "0.0000". */
        ETHHW_EventDesc evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = ETHHW_EVT_RX_READ;
        evt.data.rx.size = (bd->desc.DES3) & 0x3FFF;
        evt.data.rx.payload = (void *)bd->ext.bufAddr;
        evt.bd = bd;

        // check if a timestamp had been captured for the packet as well
        bool tsFound = bd->desc.DES1 & ETH_DMARXNDESCWBF_TSA;
        ETHHW_DescFull *ctx_bd = NULL; // context descriptor holding the timestamp
        if (tsFound) {
            // fetch timestamp
            ctx_bd = bd_next;
            evt.data.rx.ts_s = ctx_bd->desc.DES1;
            evt.data.rx.ts_ns = ctx_bd->desc.DES0;

            // step next bd further
            bd_next = ETHHW_DESC_NEXT(ring, ringLen, bd_next);
        }

        evt.bd_ctx = ctx_bd;

        // Mark before dispatching, not after: the receive thread may already have consumed
        // and restored the descriptor by the time the callback returns, and setting the flag
        // then would strand the descriptor for good.
        bd->ext.inFlight = 1;
        if (ctx_bd != NULL) {
            ctx_bd->ext.inFlight = 1;
        }

        int ret = ETHHW_ReadCallback(&evt);
        if (ret == ETHHW_RET_RX_PROCESSED) {
            ETHHW_RestoreRXDesc(bd);     // release buffer descriptor (also clears inFlight)
            ETHHW_RestoreRXDesc(ctx_bd); // and context descriptor also
        }

        bd = bd_next;
    }

    // ETHHW_print_rx_ringbuf_status(eth, ETHHW_RINGBUF_RX);

    // (*(bd-1)).desc.DES3
    // MSG("TAIL: %p SIZE: %u\n", bd, size);
}

void ETHHW_ProcessTx(ETH_TypeDef *eth) {
    uint16_t ringLen = eth->DMACTDRLR + 1;

    ETHHW_State *state = ETHHW_GetState(eth);
    ETHHW_DescFull *ring = (ETHHW_DescFull *)eth->DMACTDLAR;

    /* One descriptor is retired per pass, oldest first.
     *
     * FIXED: the counter of timestamp-carrying descriptors used to be declared outside
     * this loop and was never reset per pass, so each re-scan ADDED to the previous
     * count instead of replacing it. The count therefore stayed positive after the ring
     * had been drained, and the loop ran again with `firstIteration` still true and
     * `minIdx` left over from the previous pass -- re-dispatching a descriptor that had
     * already been processed and cleared. That produced duplicate transmit-timestamp
     * callbacks for the same uid, which flexPTP's slave.c then mismatched against its
     * Delay_Req, discarding the Delay_Resp ("no response received"). See finding F42.
     *
     * The descriptor is also cleared BEFORE the callback is invoked, so it cannot be
     * observed a second time even if this function re-enters. */
    for (;;) {
        uint16_t found = 0;
        uint16_t minIdx = 0;
        int16_t minDelta = 0; // same width as delta, so the comparison stays modular
        bool first = true;

        // search for the oldest timestamp-carrying descriptor
        for (uint16_t i = 0; i < ringLen; i++) {
            uint16_t txCntr = ring[i].ext.txCntr;
            uint32_t DES3 = ring[i].desc.DES3;

            if ((DES3 & ETH_DMATXNDESCWBF_TTSS) && !(DES3 & ETH_DMATXNDESCWBF_OWN)) {
                found++;

                /* Modular difference in 16 bits, NOT a widened subtraction.
                 *
                 * "(int32_t)txCntr - (int32_t)state->txCntAcked" breaks across the uint16_t
                 * wrap: with txCntAcked = 65530 and descriptors 65531, 65532 and 2 pending,
                 * the deltas are +1, +2 and -65528, so the "delta < minDelta" rule picks the
                 * newest descriptor as the oldest and dispatches its timestamp out of order.
                 *
                 * The wrap is not a rare event. txCntr is incremented on EVERY transmit in
                 * ETHHW_Transmit(), not only on timestamped ones, so on a busy link it comes
                 * round in minutes rather than the ~18 hours a 1 Hz Delay_Req rate suggests. */
                int16_t delta = (int16_t)(txCntr - state->txCntAcked);

                if (first) {
                    first = false;
                    minDelta = delta;
                    minIdx = i;
                } else if (delta == 1) { // consecutive packet, stop searching further
                    minIdx = i;
                    minDelta = 1;
                } else if (delta < minDelta) {
                    minIdx = i;
                    minDelta = delta;
                }
            }
        }

        if (found == 0) {
            break; // ring drained
        }

        ETHHW_DescFull *bd = ring + minIdx;

        // snapshot everything needed before the descriptor is released
        uint32_t ts_s = bd->desc.DES1;
        uint32_t ts_ns = bd->desc.DES0;
        uint32_t cbPtr = bd->ext.tsCbPtr;
        uint32_t cbArg = bd->ext.tsCbArg;

        // release the descriptor first, so it can never be dispatched twice
        memset((void *)bd, 0, sizeof(ETHHW_Desc));
        bd->ext.tsCbPtr = 0;
        bd->ext.tsCbArg = 0;
        state->txCntAcked = bd->ext.txCntr;

        if (cbPtr != 0) {
            // Signature must match lwipopts.h's tx_cb and flexPTP's ptp_transmit_cb:
            // the tag is a void *, carried through the descriptor extension as a uint32_t.
            ((void (*)(uint32_t, uint32_t, void *))cbPtr)(ts_s, ts_ns, (void *)cbArg);
        }
    }
}

/* DMACSR is write-1-to-clear, so it must be written with ONLY the bits being acknowledged.
 *
 * This used to use SET_BIT, i.e. "DMACSR |= bit" -- a read-modify-write. The read returns
 * every flag currently asserted, the OR changes nothing, and the write-back then clears all
 * of them. Acknowledging RI silently acknowledged TI, AIS, FBE, RBU and anything else that
 * happened to be set.
 *
 * Within one invocation that was mostly survivable, because both branches below test the
 * `csr` snapshot rather than the live register. The real damage is at the edges: a flag that
 * asserts between the snapshot and the write-back is destroyed without ever being seen, and
 * the error flags -- AIS, FBE, RPS, TPS -- are wiped every interrupt, which is why nothing in
 * this driver has ever been able to report a DMA fault.
 *
 * ST's own __HAL_ETH_DMA_CLEAR_IT (stm32h5xx_hal_eth.h:1623) is a direct assignment for
 * exactly this reason. */
/* DMA error state, latched in the ISR and drained by a thread. See ETHHW_ISR()'s abnormal
 * branch for why it is not printed where it happens. */
static volatile uint32_t sDmaErrFlags = 0;
static volatile uint32_t sDmaErrCount = 0;

bool ETHHW_TakeDmaError(uint32_t *pFlags, uint32_t *pCount) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    const uint32_t flags = sDmaErrFlags;
    const uint32_t count = sDmaErrCount;
    sDmaErrFlags = 0;
    sDmaErrCount = 0;

    if (primask == 0U) {
        __enable_irq();
    }

    if (count == 0U) {
        return false;
    }

    *pFlags = flags;
    *pCount = count;
    return true;
}

void ETHHW_ReportDmaError(void) {
    uint32_t flags = 0;
    uint32_t count = 0;

    if (!ETHHW_TakeDmaError(&flags, &count)) {
        return;
    }

    MSG("ETH: %u DMA error interrupt(s), DMACSR flags 0x%08X:", count, flags);
    if (flags & ETH_DMACSR_RBU) {
        MSG(" RBU(rx ring exhausted)");
    }
    if (flags & ETH_DMACSR_TBU) {
        MSG(" TBU(tx ring exhausted)");
    }
    if (flags & ETH_DMACSR_FBE) {
        MSG(" FBE(fatal bus error)");
    }
    if (flags & ETH_DMACSR_RPS) {
        MSG(" RPS(rx process stopped)");
    }
    if (flags & ETH_DMACSR_TPS) {
        MSG(" TPS(tx process stopped)");
    }
    if (flags & ETH_DMACSR_CDE) {
        MSG(" CDE(context descriptor error)");
    }
    if (flags & ETH_DMACSR_RWT) {
        MSG(" RWT(receive watchdog timeout)");
    }
    MSG("\n");
}

void ETHHW_ISR(ETH_TypeDef *eth) {
    uint32_t csr = READ_REG(eth->DMACSR);

    if (csr & ETH_DMACSR_NIS) {    // Normal Interrupt Summary
        if (csr & ETH_DMACSR_RI) {  // Receive Interrupt
            WRITE_REG(eth->DMACSR, ETH_DMACSR_RI | ETH_DMACSR_NIS);

            ETHHW_EventDesc evt;
            evt.type = ETHHW_EVT_RX_NOTFY;
            ETHHW_EventCallback(&evt);
        }

        // Independent of the RX branch: with "else if", a combined RI+TI interrupt serviced
        // only RX. Measured NOT to be the cause of F42 (the TX path delivers 100% of
        // timestamps either way), but the exclusive form is latently wrong under load.
        if (csr & ETH_DMACSR_TI) {  // Transmit Interrupt
            WRITE_REG(eth->DMACSR, ETH_DMACSR_TI | ETH_DMACSR_NIS);

            ETHHW_ProcessTx(eth);
        }
    }

    /* Abnormal Interrupt Summary: previously wiped by the read-modify-writes above without
     * anyone looking at it. The comment here used to promise a report and the code only
     * acknowledged the flags, so a DMA bus error or a stopped process was still absorbed in
     * silence -- the one event that explains a dead receive path.
     *
     * Latched rather than printed. This runs in interrupt context, where console output is
     * refused (F68): MSG() cannot format there and the FIFO cannot be waited on. A thread
     * picks these up and prints them; see ETHHW_TakeDmaError(). */
    if (csr & ETH_DMACSR_AIS) {
        const uint32_t errBits = csr & (ETH_DMACSR_FBE | ETH_DMACSR_RBU | ETH_DMACSR_TBU |
                                        ETH_DMACSR_RPS | ETH_DMACSR_TPS | ETH_DMACSR_CDE |
                                        ETH_DMACSR_ETI | ETH_DMACSR_RWT);

        sDmaErrFlags |= errBits;
        sDmaErrCount++;

        WRITE_REG(eth->DMACSR, (csr & (ETH_DMACSR_AIS | ETH_DMACSR_FBE | ETH_DMACSR_RBU |
                                       ETH_DMACSR_TBU | ETH_DMACSR_RPS | ETH_DMACSR_TPS |
                                       ETH_DMACSR_CDE | ETH_DMACSR_ETI | ETH_DMACSR_RWT)));
    }

    WRITE_REG(eth->DMACSR, ETH_DMACSR_NIS);
}

void ETHHW_Transmit(ETH_TypeDef *eth, const uint8_t *buf, uint16_t len, uint8_t txOpts, void *txOptArgs) {
    ETHHW_State *state = ETHHW_GetState(eth);                                // fetch state
    uint16_t nextTxDescIdx = state->nextTxDescIdx;                           // fetch index of descriptor to fill
    ETHHW_DescFull *bd = ((ETHHW_DescFull *)eth->DMACTDLAR) + nextTxDescIdx; // get descriptor being filled

    // MSG("%u\n", nextTxDesc);

    // ETHHW_PrintRingBufStatus(eth, ETHHW_RINGBUF_TX);

    /* Bounded, like every other handshake in this driver. This one was left unbounded by the
     * F54 sweep, and it is the worst place to spin: low_level_output() calls it on
     * tcpip_thread at priority 40 *while holding the lwIP core lock*, so a stalled TX DMA
     * would not merely burn the CPU -- it would deadlock every lwIP user behind the lock,
     * with no watchdog configured to recover. Dropping the frame is the right failure: the
     * stack is designed for lossy links. */
    bool descFree = false;
    for (uint32_t i = 0; i < ETHHW_HW_TIMEOUT_SPINS; i++) {
        if (ETHHW_DESC_OWNED_BY_APPLICATION(bd)) {
            descFree = true;
            break;
        }
    }

    if (!descFree) {
        MSG("ETH: TX descriptor %u still owned by the DMA; frame dropped\n", nextTxDescIdx);
        return;
    }

    // erase possible old descriptor data
    memset(bd, 0, sizeof(ETHHW_Desc)); // DON'T erase extension

    uint32_t opts = 0;
    if (txOpts & ETHHW_TXOPT_INTERRUPT_ON_COMPLETION) {
        opts |= ETH_DMATXNDESCRF_IOC;
    }

    if ((txOpts == ETHHW_TXOPT_CAPTURE_TS) &&
        (txOptArgs != NULL)) { // arguments are mandatory
        opts |= ETH_DMATXNDESCRF_TTSE;
        ETHHW_OptArg_TxTsCap *arg = (ETHHW_OptArg_TxTsCap *)txOptArgs; // retrieve args
        bd->ext.tsCbPtr = arg->txTsCbPtr;                              // fill-in extension fields
        bd->ext.tsCbArg = arg->tag;
    }

    // fill-in identification field
    bd->ext.txCntr = ++state->txCntSent; // copy AFTER increase

    // copy payload to TX buffer
    memcpy((void *)bd->ext.bufAddr, buf, len);

    // fill in-descriptor fields
    bd->desc.DES0 = bd->ext.bufAddr;
    bd->desc.DES2 = opts | (len & 0x3FFF); // {IOC|TTSE} and buffer length truncated to 14-bits

    // payload and descriptor body visible before ownership changes hands (N9)
    __DMB();

    bd->desc.DES3 = ETH_DMATXNDESCRF_OWN | ETH_DMATXNDESCRF_FD | ETH_DMATXNDESCRF_LD | ETH_DMATXNDESCRF_CIC_IPHDR_PAYLOAD_INSERT_PHDR_CALC; // pass desciptor to the DMA, set First Desc. and Last Desc. flags

    //ETHHW_PrintRingBufStatus(eth, ETHHW_RINGBUF_TX);

    state->nextTxDescIdx = (state->nextTxDescIdx + 1) % (eth->DMACTDRLR + 1); // advance index to next descriptor

    // and the descriptor complete before the DMA is told to go and look at it. ST's HAL does
    // the same before its tail-pointer write in HAL_ETH_Transmit().
    __DSB();

    WRITE_REG(eth->DMACTDTPR, 0); // tail pointer WON'T STOP
}

// -----------------

void ETHHW_SetLinkProperties(ETH_TypeDef *eth, bool fastEthernet, bool fullDuplex) {
    // fetch register content
    uint32_t reg = eth->MACCR;

    // set Fast Ethernet state
    if (fastEthernet) {
        SET_BIT(reg, ETH_MACCR_FES);
    } else {
        CLEAR_BIT(reg, ETH_MACCR_FES);
    }

    // set duplex state
    if (fullDuplex) {
        SET_BIT(reg, ETH_MACCR_DM);
    } else {
        CLEAR_BIT(reg, ETH_MACCR_DM);
    }

    // write modified register value back
    eth->MACCR = reg;
}

uint32_t ETHHW_ReadPHYRegister(ETH_TypeDef *eth, uint32_t PHYAddr, uint32_t PHYReg, uint32_t *pRegValue) {
    uint32_t tmpreg;

    /* Check for the Busy flag */
    if (READ_BIT(eth->MACMDIOAR, ETH_MACMDIOAR_MB) != 0U) {
        return 1;
    }

    /* Get the  MACMDIOAR value */
    WRITE_REG(tmpreg, eth->MACMDIOAR);

    /* Prepare the MDIO Address Register value
     - Set the PHY device address
     - Set the PHY register address
     - Set the read mode
     - Set the MII Busy bit */

    MODIFY_REG(tmpreg, ETH_MACMDIOAR_PA, (PHYAddr << 21));
    MODIFY_REG(tmpreg, ETH_MACMDIOAR_RDA, (PHYReg << 16));
    MODIFY_REG(tmpreg, ETH_MACMDIOAR_MOC, ETH_MACMDIOAR_MOC_RD);
    SET_BIT(tmpreg, ETH_MACMDIOAR_MB);

    /* Write the result value into the MDII Address register */
    WRITE_REG(eth->MACMDIOAR, tmpreg);

    /* Wait for the Busy flag */
    if (!ETHHW_WaitBitsClear(&eth->MACMDIOAR, ETH_MACMDIOAR_MB, "MDIO read")) {
        return 1;
    }

    /* Get MACMIIDR value */
    WRITE_REG(*pRegValue, (uint16_t)eth->MACMDIODR);

    return 0;
}

uint32_t ETHHW_WritePHYRegister(ETH_TypeDef *eth, uint32_t PHYAddr,
                                uint32_t PHYReg, uint32_t RegValue) {
    uint32_t tmpreg;

    /* Check for the Busy flag */
    if (READ_BIT(eth->MACMDIOAR, ETH_MACMDIOAR_MB) != 0U) {
        return 1;
    }

    /* Get the  MACMDIOAR value */
    WRITE_REG(tmpreg, eth->MACMDIOAR);

    /* Prepare the MDIO Address Register value
     - Set the PHY device address
     - Set the PHY register address
     - Set the write mode
     - Set the MII Busy bit */

    MODIFY_REG(tmpreg, ETH_MACMDIOAR_PA, (PHYAddr << 21));
    MODIFY_REG(tmpreg, ETH_MACMDIOAR_RDA, (PHYReg << 16));
    MODIFY_REG(tmpreg, ETH_MACMDIOAR_MOC, ETH_MACMDIOAR_MOC_WR);
    SET_BIT(tmpreg, ETH_MACMDIOAR_MB);

    /* Give the value to the MII data register */
    WRITE_REG(eth->MACMDIODR, (uint16_t)RegValue);

    /* Write the result value into the MII Address register */
    WRITE_REG(eth->MACMDIOAR, tmpreg);

    /* Wait for the Busy flag */
    if (!ETHHW_WaitBitsClear(&eth->MACMDIOAR, ETH_MACMDIOAR_MB, "MDIO write")) {
        return 1;
    }

    return 0;
}

/* ---- PTP CAPABILITIES ---- */

// #define ETH_PTP_FLAG_TSARFE ((uint32_t)(1 << 8)) // enable timestamping for
// every received frame #define ETH_PTP_FLAG_TSSSR ((uint32_t)(1 << 9)) //
// subsecond rollover control (1 = rollover on 10^9-1 nsec) #define
// ETH_PTP_FLAG_TSE ((uint32_t)(1 << 0)) // global timestamp enable flag

/* Snapshot selection, spelled out because the register alone does not say what it selects and
 * getting it wrong costs timestamps on one delay mechanism only -- a failure that looks like
 * a protocol bug rather than a driver setting.
 *
 * The combination written here is SNAPTYPSEL = 0b01 with TSEVNTENA = 0 and TSMSTRENA = 0,
 * which is the widest selection this MAC offers: every PTP message type, event and general,
 * INCLUDING the peer-delay ones (Pdelay_Req / Pdelay_Resp / Pdelay_Resp_Follow_Up).
 *
 * That breadth is required, not incidental. The delay mechanism is switchable at runtime --
 * "ptp delmech {e2e|p2p}" -- and the profiles use different ones: the default IEEE 1588
 * preset is E2E (Delay_Req/Delay_Resp) while the gPTP preset this project boots into is P2P
 * (Pdelay_*). A narrower selection would silently time-stamp only one of them.
 *
 * TSENALL is deliberately NOT set: that would timestamp every received frame, not just PTP.
 *
 * Confirmed by the F42 investigation, which rebuilt with SNAPTYPSEL = 0b00 and measured no
 * change in what the MAC *received* -- these bits select what is timestamped, not what is
 * accepted. The exact message-type table is RM0481's; it is not restated here because this
 * project has no copy to cite against. */
void ETHHW_EnablePTPTimeStamping(ETH_TypeDef *eth) {
    __IO uint32_t tmpreg = eth->MACTSCR;

    tmpreg = 0b01 << ETH_MACTSCR_SNAPTYPSEL_Pos;
    tmpreg |= ETH_MACTSCR_TSIPV4ENA | ETH_MACTSCR_TSIPENA |
              ETH_MACTSCR_TSVER2ENA | ETH_MACTSCR_TSENA |
              ETH_MACTSCR_TSCTRLSSR; // turn on relevant flags

    eth->MACTSCR = tmpreg;
}

void ETHHW_DisablePTPTimeStamping(ETH_TypeDef *eth) {
    __IO uint32_t tmpreg = eth->MACTSCR;

    tmpreg &= ~ETH_MACTSCR_TSENA;

    eth->MACTSCR = tmpreg;
}

// #define ETH_PTP_FLAG_TSSTI ((uint32_t)(1 << 2)) // initialize PTP time with
// the values stored in Timestamp high and low registers

void ETHHW_InitPTPTime(ETH_TypeDef *eth, uint32_t sec, uint32_t nsec) {
    // fill registers with time components
    eth->MACSTSUR = sec;
    eth->MACSTNUR = nsec;

    // wait for TSINIT to clear
    ETHHW_WaitBitsClear(&eth->MACTSCR, ETH_MACTSCR_TSINIT, "TSINIT");

    // perform time initialization
    __IO uint32_t tmpreg = eth->MACTSCR;

    tmpreg |= ETH_MACTSCR_TSINIT;

    eth->MACTSCR = tmpreg;
}

// #define ETH_PTP_FLAG_TSFCU ((uint32_t)(1 << 1)) // flag controlling
// fine/coarse update methods

void ETHHW_EnablePTPFineCorr(ETH_TypeDef *eth, bool enFineCorr) {
    __IO uint32_t tmpreg = eth->MACTSCR;

    if (enFineCorr) {
        tmpreg |= ETH_MACTSCR_TSCFUPDT;
    } else {
        tmpreg &= ~ETH_MACTSCR_TSCFUPDT;
    }

    eth->MACTSCR = tmpreg;
}

// NOTE: there is no relative time-update helper here on purpose. TSUPDT-based adjustment
// (MACSTSUR/MACSTNUR with the ADDSUB bit) is a documented feature of this MAC, but nothing in
// this project uses it -- the flexPTP core does coarse correction through PTP_SET_CLOCK, i.e.
// ETHHW_InitPTPTime(). A relative version inherited from the H743 driver was removed as dead
// code with a wrong subtract path; see F41. If you need one, write it against RM0481 rather
// than resurrecting that one.

// #define ETH_PTP_FLAG_TSARU ((uint32_t)(1 << 5)) // flag initiating addend
// register update

bool ETHHW_SetPTPAddend(ETH_TypeDef *eth, uint32_t addend) {
    eth->MACTSAR = addend; // set addend

    // wait for any previous update to retire
    if (!ETHHW_WaitBitsClear(&eth->MACTSCR, ETH_MACTSCR_TSADDREG, "TSADDREG")) {
        return false;
    }

    // update PTP block internal register
    __IO uint32_t tmpreg = eth->MACTSCR;

    tmpreg |= ETH_MACTSCR_TSADDREG;

    eth->MACTSCR = tmpreg;

    return true;
}

uint32_t ETHHW_GetPTPAddend(ETH_TypeDef *eth) {
    return eth->MACTSAR;
}

void ETHHW_SetPTPSubsecondIncrement(ETH_TypeDef *eth, uint8_t increment) {
    eth->MACSSIR = increment << 16;
}

uint32_t ETHHW_GetPTPSubsecondIncrement(ETH_TypeDef *eth) {
    return eth->MACSSIR >> 16;
}

#define ETH_PTP_PPS_NO_INTERRUPT (0b11 << 5)
#define ETH_PTP_PPS_OUTPUT_MODE_SELECT (1 << 4)

void ETHHW_SetPTPPPSFreq(ETH_TypeDef *eth, uint32_t freqCode) {
    __IO uint32_t tmpreg = eth->MACPPSCR;

    tmpreg = ETH_PTP_PPS_NO_INTERRUPT | (freqCode & 0x0F);

    eth->MACPPSCR = tmpreg;
}

/* Read-modify-write, not assign: "tmpreg = en << (ch + 4)" discarded the register and
 * disabled every other auxiliary channel as a side effect of touching one. Unused by this
 * project, but wrong for whoever enables auxiliary snapshots first. */
void ETHHW_AuxTimestampCh(ETH_TypeDef *eth, uint8_t ch, bool en) {
    const uint32_t bit = 1UL << ((ch & 0b11) + 4);
    uint32_t tmpreg = eth->MACACR;

    if (en) {
        tmpreg |= bit;
    } else {
        tmpreg &= ~bit;
    }

    eth->MACACR = tmpreg;
}

void ETHHW_ReadLastAuxTimestamp(ETH_TypeDef *eth, uint32_t *ps, uint32_t *pns) {
    *ps = eth->MACATSSR;
    *pns = eth->MACATSNR;
}

void ETHHW_ClearAuxTimestampFIFO(ETH_TypeDef *eth) {
    eth->MACACR |= 1;
}

uint8_t ETHHW_GetAuxTimestampCnt(ETH_TypeDef *eth) {
    return ((eth->MACTSSR >> 25) & 0b11111);
}

#define ETH_PTP_PPS_PULSE_TRAIN_START (0b0010)
#define ETH_PTP_PPS_PULSE_TRAIN_STOP_IMM (0b0101)
#define ETH_PTP_PPSCMD_MASK (0x0F)

void ETHHW_StartPTPPPSPulseTrain(ETH_TypeDef *eth, uint32_t high_len, uint32_t period) {
    ETHHW_StopPTPPPSPulseTrain(eth);

    ETHHW_WaitBitsClear(&eth->MACPPSCR, ETH_PTP_PPSCMD_MASK, "PPSCMD");

    // delayed start of pulse train with (at least) 1s to ensure,
    // target timestamps point always in the future on return from this function
    eth->MACPPSTTSR = eth->MACSTSR + 2;

    // emit pulsetrain on nanoseconds rollover
    eth->MACPPSTTNR = 0;

    // get increment value
    uint32_t increment = ETHHW_GetPTPSubsecondIncrement(eth);

    // compute positive pulse width
    eth->MACPPSWR = (high_len / increment) - 1;

    // compute repeat period
    eth->MACPPSIR = (period / increment) - 1;

    __IO uint32_t tmpreg = eth->MACPPSCR;
    tmpreg |= ETH_PTP_PPS_NO_INTERRUPT | ETH_PTP_PPS_PULSE_TRAIN_START;
    eth->MACPPSCR = tmpreg;
}

void ETHHW_StopPTPPPSPulseTrain(ETH_TypeDef *eth) {
    if (!(eth->MACPPSCR & ETH_PTP_PPS_OUTPUT_MODE_SELECT)) {
        eth->MACPPSCR |= ETH_PTP_PPS_OUTPUT_MODE_SELECT; // switch to pulse-train mode
    } else {
        ETHHW_WaitBitsClear(&eth->MACPPSCR, ETH_PTP_PPSCMD_MASK, "PPSCMD");
    }

    eth->MACPPSCR |= ETH_PTP_PPS_PULSE_TRAIN_STOP_IMM;
}
