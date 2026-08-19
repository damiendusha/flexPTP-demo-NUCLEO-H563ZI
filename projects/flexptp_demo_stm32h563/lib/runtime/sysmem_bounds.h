/* The newlib heap's bound, as arithmetic that can be tested.
 *
 * WHY THIS IS A HEADER AND NOT FIVE LINES INSIDE _sbrk()
 *
 * The bound WAS five lines inside _sbrk(), and it was wrong for the whole life of the project
 * (finding NX-03): it bounded the heap against the MSP stack reservation, `_estack -
 * _Min_Stack_Size`, when what actually sits above the heap on this firmware is the ThreadX
 * byte pools -- every thread stack and kernel object. About 103 KB of them were inside the
 * range malloc() could hand out.
 *
 * Nobody noticed because a bound is invisible until it is crossed, and crossing this one
 * corrupts rather than fails. Separating the arithmetic from the linker symbols is what makes
 * it observable: sysmem_bounds_test.cpp exercises the window, the ceiling and the
 * overflow with no target and no linker script.
 *
 * Pure, header-only and free of every dependency but stdint/stdbool, on the same terms as the
 * units in Src/flexptp_nsd/.
 */
#ifndef SYSMEM_BOUNDS_H
#define SYSMEM_BOUNDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Where the heap would end after a request of @p incr, if the request is allowed.
 *
 * @param heap_end  current break
 * @param base      lowest address the heap may occupy (`_end`)
 * @param limit     first address the heap may NOT occupy (`__RAM_segment_used_end__`)
 * @param incr      bytes requested; newlib also passes negative values to return memory
 * @param out       the new break, written only when the request is allowed
 * @return true if the request fits
 *
 * Growth is tested by subtraction rather than as `heap_end + incr > limit`, so the comparison
 * cannot be defeated by the addition wrapping.
 *
 * Honest scope for that: on THIS target it is defensive rather than load-bearing. `ptrdiff_t`
 * is 32-bit signed, so `incr` tops out at 0x7FFFFFFF, and the heap sits at ~0x2002_0000 --
 * the sum cannot reach 2^32. Mutation testing is what established that: rewriting the check
 * the natural way passed the whole suite until a case was added with the window placed high
 * enough in the address space for the addition to overflow. The subtraction is kept because it
 * is correct wherever the window is, and the test now says which of the two facts it is
 * checking.
 *
 * Shrinking is bounded too -- newlib hands memory back through a negative incr, and below
 * `base` is not memory it was ever given.
 */
static inline bool sysmem_bounds_next(uintptr_t heap_end, uintptr_t base, uintptr_t limit,
                                      ptrdiff_t incr, uintptr_t *out) {
    if ((heap_end < base) || (heap_end > limit)) {
        return false; /* the break itself is outside the window; nothing sensible to do */
    }

    if (incr > 0) {
        if ((uintptr_t)incr > (limit - heap_end)) {
            return false;
        }
        *out = heap_end + (uintptr_t)incr;
        return true;
    }

    /* incr <= 0. Negate in an unsigned type: -PTRDIFF_MIN is not representable as ptrdiff_t. */
    const uintptr_t back = (uintptr_t)(-(intmax_t)incr);
    if (back > (heap_end - base)) {
        return false;
    }
    *out = heap_end - back;
    return true;
}

#endif /* SYSMEM_BOUNDS_H */
