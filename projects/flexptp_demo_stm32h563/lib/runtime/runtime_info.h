/**
 * @file      runtime_info.h
 * @brief     What the runtime can be asked about itself, at run time, on the board.
 *
 * Everything declared here is answered by //projects/flexptp_demo_stm32h563/lib:runtime and is independent of the RTOS and of
 * the network stack -- it is the C runtime and the linked image talking about themselves. That
 * is what makes it shareable between the two firmwares, and it is why the declarations live in
 * a header instead of being repeated as `extern` lines inside each project's cmds.c, which is
 * what they were until phase 9. A local `extern` is a second, unchecked copy of a prototype:
 * get it wrong and the compiler cannot tell you, because it has nothing to compare against.
 */

#ifndef LIB_RUNTIME_RUNTIME_INFO_H
#define LIB_RUNTIME_RUNTIME_INFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Peak and total size of the newlib heap, in bytes.
 *
 * The C heap is a third allocator alongside whatever the RTOS provides, it is small, and it is
 * hard-bounded by _sbrk against the linker script's symbols (finding NX-03). @p peak is the
 * number that says whether that bound is comfortable or marginal; on both firmwares the only
 * thing that puts anything in this heap is atof() in the servo and PPS CLI commands.
 */
void sysmem_heap_usage(uint32_t *peak, uint32_t *size);

/**
 * @brief Whether the C++ static constructors ran, as a printable word.
 *
 * A file-scope object with a constructor sets a flag; this reports it. The point is that
 * "libc_init_array ran the .init_array section" is otherwise invisible on a bare-metal target
 * until something silently uses an unconstructed object.
 */
const char *cpp_static_init_status(void);

/** @brief __cplusplus of the translation unit the C++ runtime was compiled as. */
unsigned long cpp_standard(void);

/**
 * @brief Print the linker's build ID -- a SHA-1 over the linked output -- to the console.
 *
 * Emitted by -Wl,--build-id=sha1 and placed in FLASH by the linker script. Printing it is what
 * lets tools/hil/ prove it is testing the image it was handed rather than whatever happened to
 * be on the board (finding GI-01).
 */
void report_build_id(void);

#ifdef __cplusplus
}
#endif

#endif  /* LIB_RUNTIME_RUNTIME_INFO_H */
