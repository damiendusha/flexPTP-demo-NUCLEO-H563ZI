/* The newlib heap's bound.
 *
 * Finding NX-03: _sbrk() bounded the heap against `_estack - _Min_Stack_Size`, the bottom of
 * the MSP stack reservation, on ST's template assumption that the heap and the stack are the
 * only two things growing towards each other. On this firmware everything above
 * `__RAM_segment_used_end__` is the ThreadX byte pools -- every thread stack and every kernel
 * object -- and that symbol sits 2 560 bytes above `_end`, about 103 KB BELOW the ceiling the
 * template used. Past 2 560 bytes of cumulative allocation, malloc() was returning pointers
 * into thread stacks.
 *
 * A bound is invisible until it is crossed, and crossing this one corrupted rather than
 * failed. So it is arithmetic now, and the arithmetic is checked here.
 */

#include <gtest/gtest.h>

#include <cstdint>

extern "C" {
#include "sysmem_bounds.h"
}

namespace {

/* A window the size the target's actually is: _Min_Heap_Size (0x200) + _Min_Stack_Size (0x800). */
constexpr std::uintptr_t kBase = 0x20025FE0u;
constexpr std::uintptr_t kSize = 2560u;
constexpr std::uintptr_t kLimit = kBase + kSize;

std::uintptr_t grow(std::uintptr_t from, std::ptrdiff_t incr, bool *ok) {
    std::uintptr_t out = 0xDEADBEEFu;
    *ok = sysmem_bounds_next(from, kBase, kLimit, incr, &out);
    return out;
}

}  // namespace

TEST(SysmemBounds, AnAllocationInsideTheWindowIsAllowed) {
    bool ok = false;
    EXPECT_EQ(grow(kBase, 64, &ok), kBase + 64u);
    EXPECT_TRUE(ok);
}

TEST(SysmemBounds, TheWindowCanBeFilledExactly) {
    /* The boundary case in the direction that matters: the last usable byte is limit-1, so a
       request that lands the break exactly ON the limit is fine -- nothing was written there. */
    bool ok = false;
    EXPECT_EQ(grow(kBase, static_cast<std::ptrdiff_t>(kSize), &ok), kLimit);
    EXPECT_TRUE(ok);
}

TEST(SysmemBounds, OneByteBeyondTheWindowIsRefused) {
    /* This is NX-03 in one assertion. Under the old ceiling this request succeeded and
       returned a pointer into the ThreadX byte pools. */
    bool ok = true;
    grow(kBase, static_cast<std::ptrdiff_t>(kSize) + 1, &ok);
    EXPECT_FALSE(ok) << "the heap was allowed past __RAM_segment_used_end__ -- this is NX-03";
}

TEST(SysmemBounds, AnAllocationFromAPartlyUsedHeapIsBoundedToo) {
    bool ok = true;
    grow(kBase + 2000u, 561, &ok);
    EXPECT_FALSE(ok);

    ok = false;
    EXPECT_EQ(grow(kBase + 2000u, 560, &ok), kLimit);
    EXPECT_TRUE(ok);
}

TEST(SysmemBounds, AnAbsurdlyLargeRequestIsRefused) {
    /* The ordinary too-big case: no wrapping involved, just a request far past the window. */
    bool ok = true;
    grow(kBase, static_cast<std::ptrdiff_t>(PTRDIFF_MAX), &ok);
    EXPECT_FALSE(ok);

    ok = true;
    grow(kBase, static_cast<std::ptrdiff_t>(0x7FFFFFFF), &ok);
    EXPECT_FALSE(ok);
}

TEST(SysmemBounds, ARequestThatWouldWrapTheAddressSpaceIsRefused) {
    /* Why `limit - heap_end` rather than `heap_end + incr > limit`.
     *
     * This test exists because the natural formulation SURVIVED the first mutation round.
     * With the window at 0x2002_5FE0 and ptrdiff_t 32-bit, the sum cannot reach 2^32, so the
     * two formulations agree everywhere the target can actually reach and the anti-wrap
     * property was being asserted by a comment rather than by a test.
     *
     * Placing the window near the top of the address space makes them disagree: the addition
     * overflows to a small value that compares below the limit, and the natural version says
     * the request fits. The subtraction cannot be fooled that way wherever the window sits. */
    constexpr std::uintptr_t kHighBase = UINTPTR_MAX - 4096u;
    constexpr std::uintptr_t kHighLimit = UINTPTR_MAX - 2048u;

    std::uintptr_t out = 0;
    EXPECT_FALSE(sysmem_bounds_next(kHighBase, kHighBase, kHighLimit, PTRDIFF_MAX, &out))
        << "an allocation that wraps the address space was accepted";

    /* And the same window still behaves normally for a request that genuinely fits. */
    EXPECT_TRUE(sysmem_bounds_next(kHighBase, kHighBase, kHighLimit, 2048, &out));
    EXPECT_EQ(out, kHighLimit);
}

TEST(SysmemBounds, AZeroRequestReportsTheCurrentBreak) {
    /* malloc() calls sbrk(0) to discover the break. It must succeed and move nothing. */
    bool ok = false;
    EXPECT_EQ(grow(kBase + 128u, 0, &ok), kBase + 128u);
    EXPECT_TRUE(ok);
}

TEST(SysmemBounds, MemoryCanBeGivenBack) {
    bool ok = false;
    EXPECT_EQ(grow(kBase + 512u, -256, &ok), kBase + 256u);
    EXPECT_TRUE(ok);
}

TEST(SysmemBounds, ShrinkingBelowTheHeapBaseIsRefused) {
    bool ok = true;
    grow(kBase + 128u, -129, &ok);
    EXPECT_FALSE(ok) << "the break was moved below _end";

    ok = false;
    EXPECT_EQ(grow(kBase + 128u, -128, &ok), kBase);
    EXPECT_TRUE(ok) << "shrinking exactly back to _end is legitimate";
}

TEST(SysmemBounds, TheMostNegativeIncrementDoesNotOverflowOnNegation) {
    /* -PTRDIFF_MIN is not representable as a ptrdiff_t, so negating it in that type is UB.
       It must be refused, not reasoned about. */
    bool ok = true;
    grow(kBase + 128u, PTRDIFF_MIN, &ok);
    EXPECT_FALSE(ok);
}

TEST(SysmemBounds, ABreakOutsideTheWindowIsRefusedRatherThanTrusted) {
    /* Defence in depth: if the break has somehow left the window, every answer this function
       could give is wrong, so it gives none. */
    bool ok = true;
    grow(kBase - 1u, 16, &ok);
    EXPECT_FALSE(ok);

    ok = true;
    grow(kLimit + 1u, 0, &ok);
    EXPECT_FALSE(ok);
}
