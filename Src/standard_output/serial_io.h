#ifndef STANDARD_OUTPUT_SERIAL_OUTPUT
#define STANDARD_OUTPUT_SERIAL_OUTPUT

#include <stdint.h>

void serial_io_init();

// THREAD CONTEXT ONLY. A call from an ISR is counted and discarded -- see the contract note at
// the definition. Interrupt-context diagnostics must latch state and let a thread report it.
void serial_io_write(const char * str, uint32_t len);

// Polled, DMA-free, FIFO-free output for fault and assert paths. Callable with interrupts
// disabled and the scheduler stopped; converts '\n' to CRLF. Blocks until fully sent.
// Leaves the UART's DMA request disabled, so only call it if you are about to halt.
void serial_io_panic_write(const char * str);

// Console messages discarded for having been issued from interrupt context, and a way for
// the layer above to record one. Surfaced by `osinfo` so the loss is visible.
uint32_t serial_io_isr_drop_count(void);
void serial_io_note_isr_drop(void);

#endif /* STANDARD_OUTPUT_SERIAL_OUTPUT */
