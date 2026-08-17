#include "standard_output/serial_io.h"
#include "blocking_io/blocking_fifo.h"
#include "cmsis_os2.h"

#include <stdint.h>
#include <stm32h5xx_hal.h>
#include <string.h>

static void serial_io_tx_cplt(UART_HandleTypeDef *huart);
static void serial_io_rx_cplt(UART_HandleTypeDef *huart);
static void serial_io_thread(void * arg);

#define SERIAL_IO_FIFO_LEN (16384)

static UART_HandleTypeDef uart;
static DMA_HandleTypeDef dma;
static osEventFlagsId_t flags;
static osThreadId_t th;
static BFifo fifo;
static uint8_t buf[SERIAL_IO_FIFO_LEN];
static char c;

#define SERIAL_IO_TX_CPLT_FLAG (0x01)

/* Split into hardware and RTOS halves in S3 of the ThreadX port.
 *
 * Under FreeRTOS this was one function that created its FIFO, event flags and thread before
 * osKernelInitialize() -- outside the CMSIS-RTOS2 contract, but harmless, because the shim
 * forwarded straight to xTaskCreate() which is documented as callable before the scheduler
 * starts. main.c carried a long comment explaining why that was safe and left alone.
 *
 * It is not safe under ThreadX. ST's CMSIS-RTOS2 wrapper creates the two ThreadX byte pools
 * that back every object inside osKernelInitialize(), so anything allocated before that call
 * has nowhere to come from and fails.
 *
 * The split keeps the useful part of the old ordering: USART3 is configured first, so the
 * polled panic writer works before any RTOS object exists and a failure in the second half
 * can still be reported. */
void serial_io_init(void) {
    // Enable peripheral clocks
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_GPDMA1_CLK_ENABLE();

    // Initialize pins as USART3 Tx and Rx
    GPIO_InitTypeDef gpio;

    // TX
    gpio.Pin = GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Alternate = GPIO_AF7_USART3;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOD, &gpio);

    // RX
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_OD;
    HAL_GPIO_Init(GPIOD, &gpio);

    // Initialize USART3
    uart.Instance = USART3;
    uart.Init.BaudRate = 115200;
    uart.Init.Mode = UART_MODE_TX_RX;
    uart.Init.Parity = UART_PARITY_NONE;
    uart.Init.WordLength = UART_WORDLENGTH_8B;
    uart.Init.StopBits = UART_STOPBITS_1;
    uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart.Init.OverSampling = UART_OVERSAMPLING_16;
    uart.Init.ClockPrescaler = UART_PRESCALER_DIV16;
    // huart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    HAL_UART_Init(&uart);

    HAL_UART_RegisterCallback(&uart, HAL_UART_TX_COMPLETE_CB_ID, serial_io_tx_cplt);
    HAL_UART_RegisterCallback(&uart, HAL_UART_RX_COMPLETE_CB_ID, serial_io_rx_cplt);

    HAL_UART_Receive_IT(&uart, (uint8_t *)&c, 1);

    HAL_NVIC_SetPriority(USART3_IRQn, 12, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    // Configure GPDMA1 channel 0 for USART3 TX.
    //
    // The STM32H5 GPDMA is not a drop-in for the H7's stream-based DMA: the peripheral
    // is channel-based with a different DMA_InitTypeDef. Field mapping from the H7:
    //   PeriphInc/MemInc            -> DestInc/SrcInc          (direction-dependent)
    //   PeriphDataAlignment/MemData -> DestDataWidth/SrcDataWidth
    //   MemBurst/PeriphBurst        -> Src/DestBurstLength (a beat count, 1..64)
    //   FIFOMode/FIFOThreshold      -> no equivalent; dropped
    // and BlkHWRequest, TransferAllocatedPort and TransferEventMode are new.
    dma.Instance = GPDMA1_Channel0;
    dma.Init.Request = GPDMA1_REQUEST_USART3_TX;
    dma.Init.BlkHWRequest = DMA_BREQ_SINGLE_BURST;
    dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
    dma.Init.SrcInc = DMA_SINC_INCREMENTED; // walk the source buffer
    dma.Init.DestInc = DMA_DINC_FIXED;      // USART3->TDR is a fixed address
    dma.Init.SrcDataWidth = DMA_SRC_DATAWIDTH_BYTE;
    dma.Init.DestDataWidth = DMA_DEST_DATAWIDTH_BYTE;
    dma.Init.Priority = DMA_HIGH_PRIORITY;
    dma.Init.SrcBurstLength = 1;
    dma.Init.DestBurstLength = 1;
    dma.Init.TransferAllocatedPort = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0;
    dma.Init.TransferEventMode = DMA_TCEM_BLOCK_TRANSFER;
    dma.Init.Mode = DMA_NORMAL;

    HAL_DMA_Init(&dma);

    __HAL_LINKDMA(&uart, hdmatx, dma);

    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 12, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);

}

/* The RTOS half. Must be called AFTER osKernelInitialize() and before osKernelStart():
   the objects below come out of the wrapper's byte pools, which do not exist until then. */
void serial_io_start(void) {
    bool fifoOK = bfifo_create(&fifo, buf, SERIAL_IO_FIFO_LEN);

    flags = osEventFlagsNew(NULL);

    osThreadAttr_t attr;
    memset(&attr, 0, sizeof(osThreadAttr_t));
    attr.name = "serialio";
    attr.stack_size = 2048;
    /* Priority stated explicitly. Under FreeRTOS this was left at zero and the shim
       substituted osPriorityNormal; ST's ThreadX wrapper instead REJECTS osPriorityNone (0)
       because it is below osPriorityIdle (1), and osThreadNew() returns NULL. See
       docs/threadx_porting_plan.md, S3. */
    attr.priority = osPriorityNormal;
    th = osThreadNew(serial_io_thread, NULL, &attr);

    /* USART3 was configured by serial_io_init(), so the polled writer works and these
       failures can be reported rather than presenting as a silently dead console. */
    if (!fifoOK) {
        serial_io_panic_write("\n\n*** FATAL: console FIFO could not be created\n"
                              "Halted. Raise RTOS2_BYTE_POOL_HEAP_SIZE (Middlewares/threadx.cmake).\n");
        for (;;) {
        }
    }
    if ((flags == NULL) || (th == NULL)) {
        serial_io_panic_write("\n\n*** FATAL: console thread or event flags could not be created\n"
                              "Halted. Raise RTOS2_BYTE_POOL_STACK_SIZE (Middlewares/threadx.cmake).\n");
        for (;;) {
        }
    }
}

void GPDMA1_Channel0_IRQHandler() {
    HAL_DMA_IRQHandler(&dma);
}

void USART3_IRQHandler() {
    HAL_UART_IRQHandler(&uart);
}

/* Polled, length-counted writer, used by the panic paths.
 *
 * Every caller halts immediately afterwards, which is what makes it safe to stop the UART
 * pulling bytes from the GPDMA and then poke TDR by hand: nothing is left waiting on the
 * completion this steals, and nothing needs the DMA path afterwards.
 *
 * That property does NOT extend to a caller that intends to carry on -- see the contract note
 * on serial_io_write() for why interrupt context is refused rather than served here. */
static void serial_io_polled_write(const char *str, uint32_t len) {
    if ((str == NULL) || ((USART3->CR1 & USART_CR1_UE) == 0U)) {
        return; // never initialised -- polling TXE would hang instead of reporting
    }

    CLEAR_BIT(USART3->CR3, USART_CR3_DMAT);

    for (uint32_t i = 0U; i < len; i++) {
        if (str[i] == '\n') { // the terminal needs CRLF; callers should not have to remember
            while ((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) {
            }
            USART3->TDR = (uint8_t)'\r';
        }

        while ((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) {
        }
        USART3->TDR = (uint8_t)str[i];
    }

    while ((USART3->ISR & USART_ISR_TC) == 0U) { // drain before the caller halts
    }
}

/* Messages discarded because they were issued from interrupt context. Reported by `osinfo`
 * rather than printed when it happens -- printing is precisely what cannot be done there. */
static volatile uint32_t sIsrDropCount = 0;

uint32_t serial_io_isr_drop_count(void) {
    return sIsrDropCount;
}

void serial_io_note_isr_drop(void) {
    sIsrDropCount++;
}

void serial_io_write(const char *str, uint32_t len) {
    /* THE CONTRACT: this is thread-only. Interrupt context is refused, not served.
     *
     * Three comments in three files used to disagree about that (finding N6), and the code
     * did the worst of the options: the CMSIS-RTOS2 shim refuses a blocking acquire in
     * interrupt context and returns immediately, so bfifo_push() ran unserialised and, with
     * the FIFO full, spun forever. Measured -- the board hung hard, 100% ping loss and a dead
     * console, until this refusal existed.
     *
     * Serving the ISR from the polled writer was tried and rejected on measurement. It cannot
     * be made safe against HAL's DMA transmit state machine: poking TDR by hand consumes the
     * TC event HAL's completion path waits for, and gating on gState or DMAT still leaves a
     * window inside HAL_UART_Transmit_DMA() where the ISR clears the DMA request the thread
     * has just armed. Both variants left the console permanently silent after a burst while
     * the board otherwise ran normally -- trading a loud failure for a quiet one.
     *
     * So interrupt-context diagnostics latch and report from a thread instead. See
     * ETHHW_ISR()'s DMA error path for the pattern: record what happened in a volatile, let a
     * thread notice and print it. The count of anything that ignored this is available to
     * `osinfo` so the loss is visible rather than assumed to be zero. */
    if (__get_IPSR() != 0U) {
        sIsrDropCount++;
        return;
    }

    bfifo_push_all(&fifo, (const uint8_t *)str, len);
}

/* Last-resort output for the paths where the normal console cannot work: fault handlers,
 * FreeRTOS hooks and configASSERT all run with interrupts disabled or the scheduler stopped,
 * so serial_io_write() -- which pushes into a FIFO drained by a task over DMA -- would queue
 * the message and never emit it. That is why F53 and the stack-overflow path both presented
 * as a board that simply went quiet.
 *
 * Safe from any context, at any interrupt priority. Leaves the UART's DMA request disabled,
 * because every caller halts immediately afterwards. */
void serial_io_panic_write(const char *str) {
    if (str == NULL) {
        return;
    }
    serial_io_polled_write(str, strlen(str));
}

//  ----------

#define SERIAL_IO_LINEBUF_LEN (256)
static uint8_t lineBuf[SERIAL_IO_LINEBUF_LEN];

static void serial_io_thread(void * arg) {
    while (true) {
        bfifo_wait_avail(&fifo);
        uint32_t size = bfifo_read(&fifo, lineBuf, SERIAL_IO_LINEBUF_LEN);
        bfifo_pop(&fifo, size, 0);

        /* Only wait for the completion flag if the transfer actually started. The return was
         * discarded before, and the wait below is osWaitForever on a flag that ONLY the
         * success path's callback ever sets -- so a single rejected transfer killed the
         * console permanently, for the rest of the session. That is precisely the symptom
         * F51 presented as: output stopping mid-banner with the board otherwise alive.
         *
         * On failure the data is already popped and is lost; that is the right trade. The
         * alternative is to stop draining the FIFO, which loses everything after it too. */
        if (HAL_UART_Transmit_DMA(&uart, lineBuf, size) == HAL_OK) {
            /* Bounded, not osWaitForever. The flag is set by exactly one callback, so any
             * event that costs us that callback -- a UART error, or anything that consumes
             * the TC the HAL completion path waits on -- used to park this thread for the
             * rest of the session, which is the same silent-console failure F51 was about.
             * 256 bytes at 115200 baud is ~22 ms; 500 ms is two decades of headroom, and on
             * expiry the transfer is aborted so the peripheral is left in a known state. */
            if (osEventFlagsWait(flags, SERIAL_IO_TX_CPLT_FLAG, 0, 500U) == (uint32_t)osFlagsErrorTimeout) {
                HAL_UART_AbortTransmit(&uart);
            }
        } else {
            osDelay(1); // let the peripheral settle rather than spinning on a failing DMA
        }
    }
}

//  ----------

static void serial_io_tx_cplt(UART_HandleTypeDef *huart) {
    osEventFlagsSet(flags, SERIAL_IO_TX_CPLT_FLAG);
}

__weak void serial_io_rx_callback(const uint8_t *data, uint32_t size) {
    (void)data;
    (void)size;
}

static void serial_io_rx_cplt(UART_HandleTypeDef *huart) {
    char k = c;
    HAL_UART_Receive_IT(&uart, (uint8_t *)&c, 1);
    serial_io_rx_callback((const uint8_t *)&k, 1);
}