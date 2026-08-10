#include "serial_io.h"
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

void serial_io_init() {
    // initialize FIFO
    bfifo_create(&fifo, buf, SERIAL_IO_FIFO_LEN);

    // initialize flags and thread
    flags = osEventFlagsNew(NULL);

    osThreadAttr_t attr;
    memset(&attr, 0, sizeof(osThreadAttr_t));
    attr.name = "serialio";
    attr.stack_size = 2048;
    th = osThreadNew(serial_io_thread, NULL, &attr);

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

void GPDMA1_Channel0_IRQHandler() {
    HAL_DMA_IRQHandler(&dma);
}

void USART3_IRQHandler() {
    HAL_UART_IRQHandler(&uart);
}

void serial_io_write(const char *str, uint32_t len) {
    bfifo_push(&fifo, (const uint8_t *)str, len);
}

/* Last-resort output for the paths where the normal console cannot work: fault handlers,
 * FreeRTOS hooks and configASSERT all run with interrupts disabled or the scheduler stopped,
 * so serial_io_write() -- which pushes into a FIFO drained by a task over DMA -- would queue
 * the message and never emit it. That is why F53 and the stack-overflow path both presented
 * as a board that simply went quiet.
 *
 * This touches no FIFO, no DMA and no RTOS object: it disables the UART's DMA request so it
 * cannot race an in-flight transfer, then polls TXE and writes TDR directly. Safe from any
 * context, at any interrupt priority. */
void serial_io_panic_write(const char *str) {
    if ((str == NULL) || ((USART3->CR1 & USART_CR1_UE) == 0U)) {
        return; // never initialised -- polling TXE would hang instead of reporting
    }

    CLEAR_BIT(USART3->CR3, USART_CR3_DMAT);

    while (*str != '\0') {
        if (*str == '\n') { // the terminal needs CRLF; callers should not have to remember
            while ((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) {
            }
            USART3->TDR = (uint8_t)'\r';
        }

        while ((USART3->ISR & USART_ISR_TXE_TXFNF) == 0U) {
        }
        USART3->TDR = (uint8_t)*str++;
    }

    while ((USART3->ISR & USART_ISR_TC) == 0U) { // drain before the caller halts
    }
}

//  ----------

#define SERIAL_IO_LINEBUF_LEN (256)
static uint8_t lineBuf[SERIAL_IO_LINEBUF_LEN];

static void serial_io_thread(void * arg) {
    while (true) {
        bfifo_wait_avail(&fifo);
        uint32_t size = bfifo_read(&fifo, lineBuf, SERIAL_IO_LINEBUF_LEN);
        bfifo_pop(&fifo, size, 0);
        HAL_UART_Transmit_DMA(&uart, lineBuf, size);
        osEventFlagsWait(flags, SERIAL_IO_TX_CPLT_FLAG, 0, osWaitForever);
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