/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved. 
 * 
 * Redistribution and use in source and binary forms, with or without modification, 
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED 
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT 
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 * 
 * Author: Adam Dunkels <adam@sics.se>
 *
 */
#ifndef __CC_H__
#define __CC_H__

#include "cpu.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

typedef uint32_t sys_prot_t;

/* LWIP_PROVIDE_ERRNO is deliberately NOT defined here.
 *
 * Inc/lwipopts.h says "LIBC ships errno" and does `#undef LWIP_PROVIDE_ERRNO` to say so -- but
 * lwip/opt.h includes lwipopts.h first and arch/cc.h only later, via lwip/arch.h, so a bare
 * `#define LWIP_PROVIDE_ERRNO` in this file ran *after* that undef and quietly won. lwIP
 * provided its own errno after all, which is the opposite of the stated configuration
 * (finding N15). Newlib's errno is the one this project links against; leaving the macro
 * undefined here is what actually expresses that.
 *
 * Undefining it is only half the job, though, and the half that does not compile on its own:
 * lwIP requires cc.h to supply errno when it is not providing one itself, and api/err.c wants
 * the E* constants at file scope. Hence the include. */
#include <errno.h>

#if defined (__GNUC__) & !defined (__CC_ARM)

#define LWIP_TIMEVAL_PRIVATE 0
#include <sys/time.h>

#endif

/* define compiler specific symbols */
#if defined (__ICCARM__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT 
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_USE_INCLUDES

#elif defined (__GNUC__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__ ((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#elif defined (__CC_ARM)

#define PACK_STRUCT_BEGIN __packed
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#elif defined (__TASKING__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#endif

/* A failed lwIP assertion now says so and stops.
 *
 * The stock macro here was printf(), and it could do neither. printf() reaches newlib's
 * _write(), which calls __io_putchar() -- declared weak in Src/syscall.c and defined nowhere,
 * so the linker resolved it to address 0 and relaxed the call into a `nop.w`. _write() walked
 * the buffer, emitted not one character and returned len. And the macro had no halt, so
 * execution continued into whatever the assertion was guarding.
 *
 * That mattered most for the check this project deliberately turns on: lwipopts.h enables
 * LWIP_FREERTOS_CHECK_CORE_LOCKING and LWIP_ASSERT_CORE_LOCKED(), routing every unlocked lwIP
 * call into sys_check_core_locking() -> LWIP_ASSERT. It is the automated form of the bug P3.6
 * fixed by hand in check_dhcp_state(), and it had never been able to fire (finding N5).
 *
 * serial_io_panic_write() is the project's writer for exactly this situation: polled, no FIFO,
 * no DMA, no RTOS object, safe with interrupts disabled or the scheduler stopped. Halting is
 * right for a violated stack invariant, and it also gets rid of ~9 KB of newlib printf that
 * was linked in solely for this macro. */
#include "standard_output/serial_io.h"

#define LWIP_PLATFORM_ASSERT(x)                                              \
    do {                                                                     \
        serial_io_panic_write("\n\n*** FATAL: lwIP assertion failed\n  ");    \
        serial_io_panic_write(x);                                            \
        serial_io_panic_write("\n  at " __FILE__ "\nHalted.\n");             \
        for (;;) {                                                           \
        }                                                                    \
    } while (0)

/* Define random number generator function */
#define LWIP_RAND() ((u32_t)rand())

#endif /* __CC_H__ */
