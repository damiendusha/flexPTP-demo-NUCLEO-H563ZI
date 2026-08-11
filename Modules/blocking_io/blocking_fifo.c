#include "blocking_fifo.h"

#include <cmsis_compiler.h> // __get_IPSR, __get_PRIMASK, __disable_irq, __enable_irq
#include <memory.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Writer exclusion is a critical section, not a mutex, because one producer is an interrupt.
 *
 * The CLI's input FIFO is filled by serial_io_rx_callback(), which runs in the USART3
 * receive ISR, while MSG() and the CLI echo push to the console FIFO from threads. A mutex
 * cannot be taken from an ISR at all, so the original code's osSemaphoreAcquire() there
 * failed silently and the push proceeded with no exclusion whatsoever -- an ISR landing in
 * the middle of a thread's push corrupts writeIdx for both. Masking interrupts is the only
 * primitive that covers both contexts.
 *
 * The window is short: the longest push in this project is one MSG() line, and a 2 KB word
 * copy at 250 MHz is a couple of microseconds. Nothing here is on a timestamping path --
 * PTP timestamps are captured in hardware. */
static inline uint32_t bfifo_crit_enter(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void bfifo_crit_exit(uint32_t primask) {
    if (primask == 0U) {
        __enable_irq();
    }
}

bool bfifo_create(BFifo *fifo, uint8_t *mem, uint32_t size) {
    memset(fifo, 0, sizeof(BFifo));
    fifo->size = size; // store size and memory pointer
    fifo->mem = mem;
    fifo->readIdx = 0; // store read and write indices
    fifo->writeIdx = 0;
    fifo->pushedTotal = 0; // and the running totals occupancy is derived from
    fifo->poppedTotal = 0;

    /* Two signals, and NOT a permit between them:
     *   readSem   data became available   (level-triggered; every waiter re-checks)
     *   writeSem  space became available  (likewise)
     *
     * writeSem used to do double duty as the writer token, and bfifo_pop() released it
     * unconditionally from the consumer thread -- including while a producer was still inside
     * bfifo_push(). That handed the token to a second writer mid-push, and both then computed
     * their block sizes from the same writeIdx (finding N7). Writer exclusion is now the
     * critical section above, which is the only form that also covers the ISR producer. */
    fifo->readSem = osSemaphoreNew(1, 0, NULL);
    fifo->writeSem = osSemaphoreNew(1, 1, NULL);

    return (fifo->readSem != NULL) && (fifo->writeSem != NULL);
}

void bfifo_destroy(BFifo *fifo) {
    osSemaphoreDelete(fifo->readSem);
    osSemaphoreDelete(fifo->writeSem);
    memset(fifo, 0, sizeof(BFifo));
}

uint32_t bfifo_get_used(const BFifo *fifo) {
    // Modular difference of two free-running totals: correct across the 2^32 wrap, and
    // unambiguous at both ends of the range. See the note on BFifo in the header.
    return fifo->pushedTotal - fifo->poppedTotal;
}

uint32_t bfifo_get_free(const BFifo *fifo) {
    return fifo->size - bfifo_get_used(fifo);
}

// Copy as much as fits, right now, without waiting for anything. Safe from any context.
static uint32_t bfifo_push_nonblocking(BFifo *fifo, const uint8_t *data, uint32_t size) {
    uint32_t primask = bfifo_crit_enter();

    uint32_t pushSize = MIN(bfifo_get_free(fifo), size); // push size cannot exceed free size

    if (pushSize > 0U) {
        // do the actual pushing, if pushing involves an index rollover, then
        // pushing can be performed in two consecutive blocks
        uint32_t bytesToMemEnd = fifo->size - fifo->writeIdx; // calculate the number of bytes to end of the memory block counted from the current write index
        uint32_t firstBlockSize = MIN(pushSize, bytesToMemEnd); // determine the first copy block size
        uint32_t secondBlockSize = pushSize - firstBlockSize; // determine the second copy block size (might by zero)

        // copy data
        memcpy(fifo->mem + fifo->writeIdx, data, firstBlockSize); // first block
        if (secondBlockSize > 0) {
            memcpy(fifo->mem, data + firstBlockSize, secondBlockSize); // second block
        }

        // advance write index
        uint32_t newWriteIndex = fifo->writeIdx + pushSize;
        if (newWriteIndex >= fifo->size) {
            newWriteIndex = newWriteIndex - fifo->size;
        }
        fifo->writeIdx = newWriteIndex;

        // publish the data: the total is what the consumer's occupancy check reads, so it is
        // updated only after the bytes are actually in the buffer
        fifo->pushedTotal += pushSize;
    }

    bfifo_crit_exit(primask);
    return pushSize;
}

uint32_t bfifo_push(BFifo * fifo, const uint8_t * data, uint32_t size) {
    // shortcut zero length pushes
    if (size == 0) {
        return 0;
    }

    /* Blocking is a thread-only privilege.
     *
     * From an ISR this returns what fits and drops the rest, because there is no way to wait:
     * the CMSIS-RTOS2 shim refuses a blocking acquire in interrupt context and returns
     * immediately, so a wait loop there does not wait -- it spins with interrupts disabled,
     * forever, once the FIFO is full. Measured before this existed: the board hung hard, 100%
     * ping loss and a dead console (finding N6).
     *
     * Dropping is the right answer for the one ISR producer this FIFO has. The CLI's input
     * FIFO is filled a byte at a time from the USART3 receive interrupt, and a keystroke
     * arriving with 512 bytes of unread input already queued is worth less than the CPU. */
    const bool inIsr = (__get_IPSR() != 0U);

    for (;;) {
        uint32_t pushSize = bfifo_push_nonblocking(fifo, data, size);

        if (pushSize > 0U) {
            // osSemaphoreRelease is ISR-safe in the CMSIS-RTOS2 shim, which is what lets a
            // keystroke pushed from the UART interrupt wake the CLI thread
            osSemaphoreRelease(fifo->readSem);
            return pushSize;
        }

        if (inIsr) {
            return 0; // full, and nothing here may wait
        }

        /* Wait for space. The semaphore is a signal, not a permit: it says "space became
         * available", and the loop re-checks the real occupancy afterwards. A stale or
         * spurious signal therefore costs one extra iteration instead of corrupting
         * anything, which is what makes releasing it unconditionally from bfifo_pop() safe. */
        if (osSemaphoreAcquire(fifo->writeSem, osWaitForever) != osOK) {
            return 0; // cannot wait after all -- give up rather than spin
        }
    }
}

uint32_t bfifo_push_all(BFifo *fifo, const uint8_t *data, uint32_t size) {
    uint32_t pushLeft = size;
    uint32_t pushedSize = 0;
    while (pushLeft > 0) {
        uint32_t currentPush = bfifo_push(fifo, data + pushedSize, pushLeft);

        /* bfifo_push() blocks until it can place at least one byte, so from a thread it
         * cannot return 0. From an ISR it can: the CMSIS-RTOS2 shim refuses a blocking
         * acquire in interrupt context and returns immediately, at which point looping here
         * would spin forever inside the ISR (finding N6). bfifo_push() now refuses to wait
         * in interrupt context, and this bail-out is the second half of that: a full FIFO
         * costs the message, not the CPU. */
        if (currentPush == 0) {
            break;
        }

        pushedSize += currentPush;
        pushLeft -= currentPush;
    }
    return pushedSize;
}

uint32_t bfifo_read(BFifo *fifo, uint8_t *dest, uint32_t maxSize) {
    // shortcut zero-length reads
    if (maxSize == 0) {
        return 0;
    }

    // determine read size
    uint32_t usedSize = bfifo_get_used(fifo);
    uint32_t readSize = MIN(usedSize, maxSize);

    // read data in two blocks (similar to how pushing worked)
    uint32_t bytesToEnd = fifo->size - fifo->readIdx;
    uint32_t firstBlockSize = MIN(bytesToEnd, readSize);
    uint32_t secondBlockSize = readSize - firstBlockSize;

    // copy data
    memcpy(dest, fifo->mem + fifo->readIdx, firstBlockSize); // first block
    if (secondBlockSize > 0) { // second block
        memcpy(dest + firstBlockSize, fifo->mem, secondBlockSize);
    }

    return readSize;
}

uint32_t bfifo_pop(BFifo * fifo, uint32_t size, uint32_t timeout) {
    // shortcut zero-length pops
    if (size == 0) {
        return 0;
    }

    // wait for data if there is none, then re-check -- same level-triggered signalling as
    // the write side, so a stale readSem token cannot make this report data that is not there
    if (bfifo_get_used(fifo) == 0) {
        if (osSemaphoreAcquire(fifo->readSem, timeout) != osOK) {
            return 0;
        }
    }

    // limit pop count to actual data content size
    uint32_t usedSize = bfifo_get_used(fifo);
    uint32_t popSize = MIN(usedSize, size);

    // advance read index
    uint32_t newReadIdx = fifo->readIdx + popSize;
    if (newReadIdx >= fifo->size) {
        newReadIdx -= fifo->size;
    }
    fifo->readIdx = newReadIdx;

    // release the space: written after readIdx has moved, for the same reason pushedTotal
    // is written after the data lands
    fifo->poppedTotal += popSize;

    osSemaphoreRelease(fifo->writeSem); // it's ceratin, that the FIFO can be written

    return popSize; // return with the number of bytes popped
}

void bfifo_wait_avail(BFifo *fifo) {
    // Block until there is something to read. The semaphore only wakes us; the loop decides,
    // so a signal left over from an earlier push costs an iteration and nothing else.
    while (bfifo_get_used(fifo) == 0) {
        osSemaphoreAcquire(fifo->readSem, osWaitForever);
    }
}
