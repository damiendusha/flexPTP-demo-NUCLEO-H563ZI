#ifndef BLOCKING_IO_BLOCKING_FIFO
#define BLOCKING_IO_BLOCKING_FIFO

#include <stdint.h>
#include <stdbool.h>

#include <cmsis_os2.h>

/**
 * Blocking FIFO structure.
 *
 * Occupancy is NOT derived from the two indices. With only writeIdx and readIdx,
 * writeIdx == readIdx has to mean both "empty" and "full", and a push that exactly filled
 * the buffer therefore read back as empty -- the whole backlog abandoned, the drain thread
 * spinning on a zero-length transfer, and producers told the buffer was entirely free
 * (finding N1). The running totals below are unambiguous by construction: `full` is
 * pushedTotal - poppedTotal == size, `empty` is == 0, and those are different states.
 *
 * The totals free-run and are allowed to wrap at 2^32; only their difference is ever used,
 * and modular subtraction gives the right answer as long as the FIFO holds less than 4 GB.
 * Each has exactly ONE writer -- pushedTotal the producer (serialised by writeMtx),
 * poppedTotal the consumer -- so a 32-bit aligned load/store is all the atomicity needed,
 * and no critical section is required. A concurrent update can only make an observer's view
 * stale, and it is stale in the safe direction for both sides: the producer can under-read
 * the free space, the consumer can under-read the used space, and neither can over-read.
 *
 * This scheme deliberately imposes no power-of-two constraint on `size`, unlike the
 * mask-on-access alternative -- bfifo_create() takes an arbitrary runtime size, so a
 * constraint here would be an unenforced precondition on every future caller.
*/
typedef struct {
    uint32_t writeIdx;       ///< Next byte to write (owned by the producer)
    uint32_t readIdx;        ///< Next byte to read (owned by the consumer)
    uint32_t pushedTotal;    ///< Free-running count of bytes ever pushed (producer writes)
    uint32_t poppedTotal;    ///< Free-running count of bytes ever popped (consumer writes)
    uint32_t size;   ///< Size of the circular buffer
    osSemaphoreId_t readSem; ///< Signals "data became available"
    osSemaphoreId_t writeSem; ///< Signals "space became available"
    osMutexId_t writeMtx;    ///< Writer exclusion, held only inside bfifo_push()
    uint8_t *mem;      ///< FIFO memory
} BFifo;

/**
 * Create blocking FIFO.
 * @param fifo pointer to existing, but uninitialized BFifo object
 * @param mem pointer to memory space used for FIFO data storage
 * @param size size of the FIFO in bytes (NOT in element count)
 * @return indicates if BFifo creation was successful
*/
bool bfifo_create(BFifo * fifo, uint8_t * mem, uint32_t size);

/**
 * Destroy a blocking FIFO.
 * @param fifo pointer to a BFifo object initialized using bfifo_create()
*/
void bfifo_destroy(BFifo * fifo);

/**
 * Get free size in a FIFO.
 * @param fifo pointer to BFifo object
 * @return number of free bytes
*/
uint32_t bfifo_get_free(const BFifo * fifo);

/**
 * Get used area size in the FIFO.
 * @param fifo pointer to BFifo object
 * @return number of used bytes
*/
uint32_t bfifo_get_used(const BFifo * fifo);

/**
 * Push data into the FIFO.
 * @param fifo pointer to BFifo object
 * @param data pointer to data getting pushed into the FIFO
 * @param size size attempted to be pushed
 * @return number of bytes successfully pushed
*/
uint32_t bfifo_push(BFifo * fifo, const uint8_t * data, uint32_t size);

/**
 * Push ALL the data into the FIFO.
 * @param fifo pointer to BFifo object
 * @param data pointer to data getting pushed into the FIFO
 * @param size size to be pushed
 * @return number of bytes successfully pushed
*/
uint32_t bfifo_push_all(BFifo * fifo, const uint8_t * data, uint32_t size);

/**
 * Read from the FIFO.
 * @param fifo pointer to BFifo object
 * @param dest pointer to destination
 * @param maxSize maximum read size
 * @return number of bytes read
*/
uint32_t bfifo_read(BFifo * fifo, uint8_t * dest, uint32_t maxSize);

/**
 * Pop size bytes of data.
 * @param fifo pointer to BFifo object
 * @param size number of bytes popped
 * @param timeout pop timeout
 * @return number of bytes popped
*/
uint32_t bfifo_pop(BFifo * fifo, uint32_t size, uint32_t timeout);

/**
 * Wait for available data.
 * @param fifo pointer to BFifo object
*/
void bfifo_wait_avail(BFifo * fifo);

#endif /* BLOCKING_IO_BLOCKING_FIFO */
