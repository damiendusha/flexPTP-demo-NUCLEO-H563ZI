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
 * BOTH are load-bearing now, which they were not before (findings NX-03 and H-5).
 *
 * _Min_Stack_Size reserves the MSP stack at the top of SRAM1.
 *
 * _Min_Heap_Size used to be documentation: it reserved space here, which moved `_end`, but
 * _sbrk() never consulted it and bounded itself against the MSP stack instead -- 181 776 bytes
 * above `_end`, straight through the ThreadX byte pools. Src/sysmem.c now bounds the heap at
 * __RAM_segment_used_end__, so this reservation IS the heap and its size is a real limit.
 *
 * WHICH MEANS IT HAD TO BE SIZED, not left at ST's template value. Measured on hardware with
 * a deliberately oversized heap and `osinfo` reporting the high-water mark:
 *
 *     at boot .............................. 0 bytes
 *     7 servo/PPS commands, flexPTP stopped . 0 bytes
 *     the same commands with flexPTP running . 3 600 bytes
 *     130 commands ......................... 3 600 bytes  (stable; newlib recycles)
 *
 * So the requirement is ~3.6 KB, it appears only once flexPTP is running, and it does not
 * grow. 0x2000 gives the window 10 240 bytes -- 2.8x the measured peak.
 *
 * The old 0x200 was not merely small, it was FICTION: the 3 600 bytes were being allocated
 * regardless, out of the ThreadX byte pools. Capping the heap at the old figure is what
 * demonstrated it -- the console thread wedged after seven commands, because newlib's strtod
 * does not tolerate an allocation failure. See the note in Src/sysmem.c. */
_Min_Heap_Size = 0x2000;     /* 8 KB; measured peak is 3 600 B -- see above */
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

  /*
   * The linker's build ID: a SHA-1 over the linked output, emitted because CMakeLists.txt
   * passes -Wl,--build-id=sha1.
   *
   * It is here so the RUNNING firmware can report which image it is. tools/hil/ used to have
   * no way to tell -- without --flash it drove whatever was already on the board and reported
   * a confident pass for code that was never loaded, and the only control was a warning in the
   * README (finding GI-01). Now `osinfo` prints this and the runner compares it against the
   * ELF, so testing the wrong image is a hard failure rather than a thing to remember.
   *
   * It must be KEEP()t (nothing references it) and it must be inside the loaded image, which
   * is why it sits in FLASH immediately after the vector table rather than in a note section
   * the objcopy would drop.
   */
  .note.gnu.build-id :
  {
    . = ALIGN(4);
    PROVIDE(__build_id_note_start = .);
    KEEP(*(.note.gnu.build-id))
    PROVIDE(__build_id_note_end = .);
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
   * End of everything the linker has allocated in SRAM1.
   *
   * ThreadX's _tx_initialize_low_level() reads this symbol and publishes it as
   * _tx_initialize_unused_memory -- the base of the region handed to
   * tx_application_define(), and what the CMSIS-RTOS2 wrapper carves its two byte pools
   * from under USE_DYNAMIC_MEMORY_ALLOCATION. Every thread stack and kernel object comes
   * out of there, so if this lands in the wrong place the failure is a boot-time allocation
   * failure rather than anything subtle.
   *
   * It must come AFTER ._user_heap_stack, for the same reason the FreeRTOS heap had to come
   * before it: everything between `_end` and whatever ceiling _sbrk() enforces is memory
   * malloc() may hand out, so a byte pool inside that window is a byte pool malloc() can
   * scribble on.
   *
   * ORDER IS NECESSARY AND IT IS NOT SUFFICIENT (finding NX-03). This comment used to end
   * "sitting above the reservation keeps the two allocators disjoint", which did not follow:
   * _sbrk() bounded itself against `_estack - _Min_Stack_Size`, 179 216 bytes above here, so
   * the ordering bought 2 560 bytes of separation and no more. Measured on the linked image,
   * _end = 0x200131F0, this symbol = 0x20013BF0, that old ceiling = 0x2003F800 -- and the
   * heap's measured peak is 3 600 bytes, so it was over the line in normal operation, not
   * hypothetically.
   *
   * Src/sysmem.c now bounds itself against THIS symbol, which is what makes the sentence
   * true. Both allocators still have to be told where the line is; the linker script only
   * draws it.
   *
   * Defined unconditionally, so the symbol exists while FreeRTOS is still the kernel (S2)
   * and nothing has to change here when ThreadX takes over (S3).
   */
  . = ALIGN(8);
  PROVIDE ( __RAM_segment_used_end__ = . );

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
