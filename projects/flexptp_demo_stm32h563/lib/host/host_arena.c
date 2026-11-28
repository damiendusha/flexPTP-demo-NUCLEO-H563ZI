#define _GNU_SOURCE

#include "host_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <sanitizer/asan_interface.h>

/* Under AddressSanitizer, a poisoned gap in front of every allocation, and everything not handed
 * out poisoned too. ASan watches malloc(), not mmap(), so without this the whole arena -- every
 * descriptor ring and frame buffer the driver touches -- is one block it cannot see into: a
 * descriptor written past the end of its ring lands in whatever was allocated next, and nothing
 * notices. With it, the access is reported where it happens.
 *
 * Only under ASan: elsewhere the gap is zero and the macros, from the sanitizer header, do
 * nothing, so a plain build lays memory out exactly as before. ASan tracks memory in 8-byte
 * granules, so allocations are 8-aligned at least while it is on. */
#if defined(__SANITIZE_ADDRESS__)
#define ARENA_REDZONE 64u
#define ARENA_MIN_ALIGN 8u
#else
#define ARENA_REDZONE 0u
#define ARENA_MIN_ALIGN 1u
#endif

/* One MAP_32BIT region, bump-allocated. 4 MiB is far more than the descriptor rings and
   frame buffers need (the firmware's own ETH region is ~75 KB) and keeps the arithmetic
   trivial. */
#define ARENA_BYTES (4u * 1024u * 1024u)

static uint8_t *s_base;
static size_t s_used;

static void arena_init(void) {
    if (s_base != NULL) {
        return;
    }
    void *p = mmap(NULL, ARENA_BYTES, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) {
        perror("host_arena: mmap(MAP_32BIT)");
        fprintf(stderr,
                "host_arena: could not obtain memory below 4 GB.\n"
                "The driver stores addresses in uint32_t descriptor and register fields, so\n"
                "the host tests need low memory. See test/host/host_arena.h.\n");
        abort();
    }
    if (!host_arena_fits32(p)) {
        fprintf(stderr, "host_arena: MAP_32BIT returned %p, which does not fit in 32 bits\n", p);
        abort();
    }
    s_base = (uint8_t *)p;
    s_used = 0;
    ASAN_POISON_MEMORY_REGION(s_base, ARENA_BYTES);
}

void *host_arena_alloc(size_t size, size_t align) {
    arena_init();
    if (align == 0) {
        align = 4;
    }
    if (align < ARENA_MIN_ALIGN) {
        align = ARENA_MIN_ALIGN;
    }
    size_t off = (s_used + ARENA_REDZONE + (align - 1)) & ~(align - 1);
    if (off + size > ARENA_BYTES) {
        fprintf(stderr, "host_arena: exhausted (%zu requested, %zu of %u used)\n",
                size, s_used, ARENA_BYTES);
        abort();
    }
    void *p = s_base + off;
    s_used = off + size;
    ASAN_UNPOISON_MEMORY_REGION(p, size);
    memset(p, 0, size);
    return p;
}

void host_arena_reset(void) {
    if (s_base != NULL) {
        /* The gaps are poisoned, so clearing across them needs them unpoisoned first. */
        ASAN_UNPOISON_MEMORY_REGION(s_base, s_used);
        memset(s_base, 0, s_used);
        ASAN_POISON_MEMORY_REGION(s_base, ARENA_BYTES);
    }
    s_used = 0;
}

int host_arena_fits32(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return (v >> 32) == 0 && (uintptr_t)(uint32_t)v == v;
}
