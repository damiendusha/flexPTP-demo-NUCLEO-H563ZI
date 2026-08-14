/**************************************************************************/
/*                                                                        */
/*  Low-level ThreadX initialisation for the STM32H563ZI.                 */
/*                                                                        */
/*  Adapted from Eclipse ThreadX v6.5.1.202602a:                          */
/*      Modules/threadx/ports/cortex_m33/gnu/example_build/               */
/*          tx_initialize_low_level.S                                     */
/*  which is MIT-licensed, Copyright (c) Microsoft Corporation and the    */
/*  Eclipse ThreadX contributors.                                         */
/*                                                                        */
/*  Eclipse ThreadX keeps this file in example_build/ rather than in the  */
/*  port's src/, because it is board-specific: the clock, the tick rate,  */
/*  the vector table symbol and the end-of-RAM marker are all the         */
/*  integrator's to supply. (ST's fork moves it into src/ pre-adapted,    */
/*  which is one of the few real differences between the two -- see       */
/*  docs/threadx_netxduo_feasibility.md.)                                 */
/*                                                                        */
/*  CHANGES FROM THE UPSTREAM EXAMPLE, each deliberate:                   */
/*                                                                        */
/*  1. SYSTEM_CLOCK is 250 MHz, not the example's 6 MHz, and the SysTick  */
/*     reload is derived from TX_TIMER_TICKS_PER_SECOND (1000) instead of */
/*     a hardcoded 100. Both come from Inc/tx_user.h so the tick rate is  */
/*     stated in exactly one place.                                       */
/*                                                                        */
/*  2. The vector table symbol is g_pfnVectors, this project's name for   */
/*     it (Startup/startup_stm32h563zitx.s), not the example's _vectors.  */
/*                                                                        */
/*  3. HardFault_Handler and UsageFault_Handler are NOT defined here.     */
/*     The upstream example provides both -- an infinite loop for the     */
/*     former, and stack-overflow recovery for the latter -- and both     */
/*     would collide at link time with the reporting handlers in          */
/*     Src/main.c.                                                        */
/*                                                                        */
/*     Those are kept instead. They decode CFSR/HFSR and the faulting PC  */
/*     and LR through the polled UART writer, and on this part they are   */
/*     what actually catches a task stack overflow: the M33 port programs */
/*     PSPLIM on every context switch, so an overflow faults on the       */
/*     offending push (UFSR.STKOF) rather than being caught by any        */
/*     software pattern check. See docs/architecture.md.                  */
/*                                                                        */
/*     What is given up is ThreadX's recovery path, which terminates the  */
/*     offending thread via _tx_thread_stack_error_handler and pends      */
/*     PendSV to carry on. This firmware reports and halts, which is the  */
/*     behaviour it already had and the behaviour a demo wants. S3 wires  */
/*     _tx_thread_stack_error_handler in as a notification so the kernel  */
/*     still learns of it before the halt.                                */
/*                                                                        */
/**************************************************************************/

#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"
#endif

#ifndef TX_TIMER_TICKS_PER_SECOND
#error "tx_user.h must define TX_TIMER_TICKS_PER_SECOND (it is the SysTick divisor here)"
#endif

/* SYSCLK after init_pll(): HSE 8 MHz -> PLL1 -> 250 MHz. Must track
   configCPU_CLOCK_HZ / SystemCoreClock; if the clock tree changes, change it here too. */
SYSTEM_CLOCK        =     250000000
SYSTICK_CYCLES      =     ((SYSTEM_CLOCK / TX_TIMER_TICKS_PER_SECOND) - 1)

    .text
    .balign 4
    .syntax unified
    .eabi_attribute Tag_ABI_align_preserved, 1

/**************************************************************************/
/*  _tx_initialize_low_level                                              */
/*                                                                        */
/*  Called by _tx_initialize_kernel_enter() before tx_application_define. */
/**************************************************************************/
    .global  _tx_initialize_low_level
    .thumb_func
.type _tx_initialize_low_level, function
_tx_initialize_low_level:

    /* Disable interrupts during ThreadX initialization.  */
    CPSID   i

    /* Set base of available memory to the end of the linker-allocated RAM.
     *
     * __RAM_segment_used_end__ is PROVIDEd by stm32h563zitx_FLASH.ld immediately after the
     * heap/stack reservation in SRAM1. Everything above it is what ThreadX hands to
     * tx_application_define(), and what the CMSIS-RTOS2 wrapper carves its two byte pools
     * from under USE_DYNAMIC_MEMORY_ALLOCATION.  */
    LDR     r0, =_tx_initialize_unused_memory       // Build address of unused memory pointer
    LDR     r1, =__RAM_segment_used_end__           // Build first free address
    ADD     r1, r1, #4                              //
    STR     r1, [r0]                                // Setup first unused memory pointer

    /* Setup Vector Table Offset Register.
     *
     * SystemInit() has already done this, but ThreadX sets it unconditionally and doing it
     * twice with the same value is harmless. g_pfnVectors is this project's vector table.  */
    MOV     r0, #0xE000E000                         // Build address of NVIC registers
    LDR     r1, =g_pfnVectors                       // Pickup address of vector table
    STR     r1, [r0, #0xD08]                        // Set vector table address

    /* Enable the cycle count register.  */
    LDR     r0, =0xE0001000                         // Build address of DWT register
    LDR     r1, [r0]                                // Pickup the current value
    ORR     r1, r1, #1                              // Set the CYCCNTENA bit
    STR     r1, [r0]                                // Enable the cycle count register

    /* Set system stack pointer from vector value.  */
    LDR     r0, =_tx_thread_system_stack_ptr        // Build address of system stack pointer
    LDR     r1, =g_pfnVectors                       // Pickup address of vector table
    LDR     r1, [r1]                                // Pickup reset stack pointer
    STR     r1, [r0]                                // Save system stack pointer

    /* Configure SysTick for a TX_TIMER_TICKS_PER_SECOND tick.  */
    MOV     r0, #0xE000E000                         // Build address of NVIC registers
    LDR     r1, =SYSTICK_CYCLES
    STR     r1, [r0, #0x14]                         // Setup SysTick Reload Value
    MOV     r1, #0x7                                // Build SysTick Control Enable Value
    STR     r1, [r0, #0x10]                         // Setup SysTick Control

    /* Configure handler priorities.
     *
     * SVCall and PendSV must both sit at the lowest priority (0xFF); the kernel relies on
     * it. This mirrors what the FreeRTOS port did with PendSV/SysTick at 15.  */
    LDR     r1, =0x00000000                         // Rsrv, UsgF, BusF, MemM
    STR     r1, [r0, #0xD18]                        // Setup System Handlers 4-7 Priority Registers
    LDR     r1, =0xFF000000                         // SVCl, Rsrv, Rsrv, Rsrv
    STR     r1, [r0, #0xD1C]                        // Setup System Handlers 8-11 Priority Registers
    LDR     r1, =0x40FF0000                         // SysT, PnSV, Rsrv, DbgM
    STR     r1, [r0, #0xD20]                        // Setup System Handlers 12-15 Priority Registers

    /* Return to caller.  */
    BX      lr


/**************************************************************************/
/*  Shells for the vectors ThreadX owns.                                  */
/*                                                                        */
/*  HardFault_Handler and UsageFault_Handler are deliberately absent --   */
/*  see the note at the top of this file.                                 */
/**************************************************************************/

    .section .text
    .balign 4
    .syntax unified
    .eabi_attribute Tag_ABI_align_preserved, 1
    .global  __tx_BadHandler
    .thumb_func
.type __tx_BadHandler, function
__tx_BadHandler:
    B       __tx_BadHandler


/* The kernel's tick. Without this ThreadX never advances time: no tx_thread_sleep, no
 * timers, no preemption time-slicing -- and the SysTick vector would fall through to
 * Default_Handler's infinite loop on the first tick. It replaces the FreeRTOS port's
 * SysTick_Handler, which goes away with FreeRTOS in S3. */
    .section .text
    .balign 4
    .syntax unified
    .eabi_attribute Tag_ABI_align_preserved, 1
    .global  SysTick_Handler
    .thumb_func
.type SysTick_Handler, function
SysTick_Handler:
    PUSH    {r0,lr}                                 // Save LR (r0 keeps the stack aligned)
#if (defined(TX_ENABLE_EXECUTION_CHANGE_NOTIFY) || defined(TX_EXECUTION_PROFILE_ENABLE))
    BL      _tx_execution_isr_enter
#endif
    BL      _tx_timer_interrupt
#if (defined(TX_ENABLE_EXECUTION_CHANGE_NOTIFY) || defined(TX_EXECUTION_PROFILE_ENABLE))
    BL      _tx_execution_isr_exit
#endif
    POP     {r0,lr}
    BX      lr


/* Template for an application ISR that calls ThreadX services. This project's three
 * interrupts (ETH, USART3, GPDMA1_Ch0) are plain C handlers and do not use it, but it is
 * kept because it is the documented shape for one and costs a few bytes. */
    .section .text
    .balign 4
    .syntax unified
    .eabi_attribute Tag_ABI_align_preserved, 1
    .global  __tx_IntHandler
    .thumb_func
.type __tx_IntHandler, function
__tx_IntHandler:
    PUSH    {r0,lr}
#if (defined(TX_ENABLE_EXECUTION_CHANGE_NOTIFY) || defined(TX_EXECUTION_PROFILE_ENABLE))
    BL      _tx_execution_isr_enter
#endif
    /* Application interrupt work goes here. */
#if (defined(TX_ENABLE_EXECUTION_CHANGE_NOTIFY) || defined(TX_EXECUTION_PROFILE_ENABLE))
    BL      _tx_execution_isr_exit
#endif
    POP     {r0,lr}
    BX      lr


    .section .text
    .balign 4
    .syntax unified
    .eabi_attribute Tag_ABI_align_preserved, 1
    .global  __tx_NMIHandler
    .thumb_func
.type __tx_NMIHandler, function
__tx_NMIHandler:
    B       __tx_NMIHandler


    .section .text
    .balign 4
    .syntax unified
    .eabi_attribute Tag_ABI_align_preserved, 1
    .global  __tx_DBGHandler
    .thumb_func
.type __tx_DBGHandler, function
__tx_DBGHandler:
    B       __tx_DBGHandler

    .end
