#ifndef STANDARD_OUTPUT_SERIAL_OUTPUT
#define STANDARD_OUTPUT_SERIAL_OUTPUT

#include <stdint.h>

void serial_io_init();
void serial_io_write(const char * str, uint32_t len);

// Polled, DMA-free, FIFO-free output for fault and assert paths. Callable with interrupts
// disabled and the scheduler stopped; converts '\n' to CRLF. Blocks until fully sent.
void serial_io_panic_write(const char * str);

#endif /* STANDARD_OUTPUT_SERIAL_OUTPUT */
