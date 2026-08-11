/*
******************************************************************************
**
**  File        : stm32h563zitx_FLASH.ld
**
**  Abstract    : Linker script for STM32H563ZI (NUCLEO-H563ZI)
**                2048Kbytes FLASH and 640Kbytes SRAM
**
**                Ported from STM32H735 linker script.
**                H5 has a flat SRAM layout (SRAM1/2/3) instead of
**                the H7's segmented TCM + domain SRAM architecture.
**
**  Target      : STMicroelectronics STM32H563ZI
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
*****************************************************************************
** @attention
**
** Copyright (c) 2024 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(SRAM1) + LENGTH(SRAM1);    /* end of SRAM1 */
/* Generate a link error if heap and stack don't fit into RAM.
 *
 * _Min_Stack_Size is load-bearing: _sbrk() in Src/sysmem.c bounds the newlib heap against
 * _estack - _Min_Stack_Size, so it is what keeps malloc() out of the MSP stack.
 *
 * _Min_Heap_Size is NOT. It reserves space in ._user_heap_stack below, which moves `end`, but
 * _sbrk() never consults it -- nothing enforces a minimum heap. It is documentation, and the
 * reservation is the only reason it has any effect at all (finding H-5). */
_Min_Heap_Size = 0x200;      /* required amount of heap  */
_Min_Stack_Size = 0x800; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  SRAM1  (xrw) : ORIGIN = 0x20000000, LENGTH = 256K
  SRAM2  (xrw) : ORIGIN = 0x20040000, LENGTH = 64K
  SRAM3  (xrw) : ORIGIN = 0x20050000, LENGTH = 320K
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH
  .ARM (READONLY) :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array (READONLY) :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array (READONLY) :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array (READONLY) :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into SRAM1, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >SRAM1 AT> FLASH

  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >SRAM1

  /*
   * FreeRTOS heap FIRST, then the newlib heap/stack reservation.
   *
   * This order matters. _sbrk() in Src/sysmem.c starts the newlib heap at `_end` and bounds
   * it only against `_estack - _Min_Stack_Size` -- it never consults _Min_Heap_Size. So
   * everything placed AFTER `_end` and below the stack is inside the range malloc() can hand
   * out.
   *
   * With .FreeRTOS_sec after ._user_heap_stack, the 30 KB ucHeap sat squarely in that range:
   * measured at ucHeap = 0x200190d0 with _end = 0x200186d0 and the _sbrk ceiling at
   * 0x2003F800. After roughly 2.5 KB of cumulative newlib allocation, malloc() would have
   * begun returning pointers inside the FreeRTOS heap -- silently corrupting task stacks and
   * control blocks.
   *
   * Reversing the two puts ucHeap below `_end`, out of reach of _sbrk entirely.
   */
  .FreeRTOS_sec (NOLOAD) :
  {
    *(.FreeRTOSHeapSection)
  } >SRAM1

  /* User_heap_stack section, used to check that there is enough RAM left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >SRAM1

  /*
   * Ethernet DMA buffers and descriptors — placed in SRAM3.
   *
   * On the H7, these lived in RAM_D1 (AXI-SRAM) which was accessible
   * by the Ethernet DMA without cache coherency issues (when MPU was
   * configured correctly). On the H5, SRAM3 is large (320K) and
   * accessible by the Ethernet DMA controller. If DCACHE is enabled,
   * ensure this region is configured as non-cacheable via the MPU or
   * use cache maintenance operations.
   */
  .EthDrv_sec (NOLOAD) :
  {
    *(.ETHStateAndDecripSection)
    *(.ETHBufferSection)
  } >SRAM3

  .lwIP_sec (NOLOAD) :
  {
    *(.lwIPHeapSection)
  } >SRAM3

  /*
   * EtherLib's 32 KB dynamic pool. Declared by Inc/etherlib_options.h as
   * section(".ETHLibPool") and had no placement at all -- neither here nor in the H743
   * project (finding F29).
   *
   * That is not harmless once EtherLib is actually compiled. Measured by linking a stand-in
   * array with the same attribute: unreferenced it was discarded silently by --gc-sections,
   * and referenced it landed as an ORPHAN output section -- PROGBITS with a load address, so
   * 32 KB of zeros stored in flash, placed immediately after .data and therefore covered by
   * neither the startup copy loop (which stops at _edata) nor the bss zeroing loop (which
   * starts after it). Uninitialised, and paid for in flash.
   *
   * NOLOAD in SRAM3 removes the flash cost and makes "not initialised" explicit rather than
   * accidental; mp_init() sets up the pool header itself, so zeroing is not required.
   * Placed now rather than when EtherLib is first built, so that build does not also have to
   * diagnose this.
   */
  .ETHLib_sec (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ETHLibPool)
    . = ALIGN(4);
  } >SRAM3

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
