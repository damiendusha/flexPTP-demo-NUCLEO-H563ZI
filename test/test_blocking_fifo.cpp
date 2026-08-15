/* The console FIFO, against a real ThreadX kernel.
 *
 * WHY THIS EXISTS
 *
 * `blocking_fifo.c` carried finding F67 (N1, N7) and had no automated coverage of any kind.
 * Two defects, both of a shape that reading the code does not reliably catch:
 *
 *   N1  occupancy was derived from writeIdx and readIdx alone, so writeIdx == readIdx had to
 *       mean both "empty" and "full". A push that exactly filled the buffer read back as
 *       empty: the whole backlog abandoned, and producers told the buffer was entirely free.
 *   N7  writeSem doubled as the writer token and bfifo_pop() released it unconditionally from
 *       the consumer, handing it to a second writer mid-push.
 *
 * The fix was free-running totals whose DIFFERENCE is the occupancy, correct across the 2^32
 * wrap. That is arithmetic nobody can eyeball, and it is exercised here.
 *
 * WHY IT LINKS THE REAL KERNEL
 *
 * The FIFO is semaphores and a critical section; a fake of either would be a fake of the thing
 * under test. test/ already links real ThreadX (its Linux port) and ST's real CMSIS-RTOS2
 * wrapper for the context-rule tests, so the same target serves here -- the semaphores are the
 * board's semaphores, and osSemaphoreAcquire() really blocks.
 *
 * What it cannot cover: the ISR producer. __get_IPSR() is hard-coded to 0 in the host shim, so
 * bfifo_push()'s "may not wait here" branch is not reachable, exactly as for the
 * transmit-timestamp path. That half stays on the bench.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <numeric>
#include <vector>

extern "C" {
#include "cmsis_os2.h"
#include "blocking_fifo.h"
#include "tx_api.h"
}

/* The shim's PRIMASK, which blocking_fifo.c's critical section reads and writes. */
extern "C" {
uint32_t host_primask = 0;

/* The shim's IPSR. Zero means thread context, which is what every test here wants
   except the ISR-context cases in test_rtos_context.cpp. */
uint32_t host_fake_ipsr = 0U;

/* The wrapper reports this as the system timer frequency. Nothing here reads it. */
uint32_t SystemCoreClock = 250000000U;
}

namespace {

constexpr uint32_t kSize = 64;

class Fifo : public ::testing::Test {
  protected:
    void SetUp() override { ASSERT_TRUE(bfifo_create(&f_, mem_, kSize)); }
    void TearDown() override { bfifo_destroy(&f_); }

    /** n bytes with a recognisable pattern, so a misplaced block copy shows up as data. */
    static std::vector<uint8_t> pattern(uint32_t n, uint8_t seed = 0) {
        std::vector<uint8_t> v(n);
        std::iota(v.begin(), v.end(), seed);
        return v;
    }

    BFifo f_{};
    uint8_t mem_[kSize]{};
};

}  // namespace

TEST_F(Fifo, StartsEmpty) {
    EXPECT_EQ(bfifo_get_used(&f_), 0u);
    EXPECT_EQ(bfifo_get_free(&f_), kSize);
}

TEST_F(Fifo, RoundTripsData) {
    auto in = pattern(16);
    EXPECT_EQ(bfifo_push(&f_, in.data(), in.size()), 16u);
    EXPECT_EQ(bfifo_get_used(&f_), 16u);

    uint8_t out[16] = {};
    EXPECT_EQ(bfifo_read(&f_, out, sizeof(out)), 16u);
    EXPECT_EQ(std::memcmp(out, in.data(), 16), 0);

    EXPECT_EQ(bfifo_pop(&f_, 16, 0), 16u);
    EXPECT_EQ(bfifo_get_used(&f_), 0u);
}

TEST_F(Fifo, AFullFifoReadsAsFullAndNotAsEmpty) {
    /* Finding F67/N1, in one assertion.
     *
     * With occupancy derived from the two indices, filling the buffer exactly makes
     * writeIdx == readIdx, which the old code reported as EMPTY -- so the console dropped its
     * entire backlog and every producer was told the buffer was completely free. */
    auto in = pattern(kSize);
    ASSERT_EQ(bfifo_push(&f_, in.data(), kSize), kSize);

    EXPECT_EQ(bfifo_get_used(&f_), kSize) << "a full FIFO reported as empty -- this is F67/N1";
    EXPECT_EQ(bfifo_get_free(&f_), 0u);
}

TEST_F(Fifo, AFullFifoStillYieldsItsContents) {
    /* The consequence of N1 that was actually visible: not the number, but the data. */
    auto in = pattern(kSize, 7);
    ASSERT_EQ(bfifo_push(&f_, in.data(), kSize), kSize);

    std::vector<uint8_t> out(kSize);
    EXPECT_EQ(bfifo_read(&f_, out.data(), kSize), kSize);
    EXPECT_EQ(std::memcmp(out.data(), in.data(), kSize), 0);
}

TEST_F(Fifo, APushBeyondCapacityIsTruncatedRatherThanWrapped) {
    auto in = pattern(kSize + 16);
    EXPECT_EQ(bfifo_push(&f_, in.data(), kSize + 16), kSize);
    EXPECT_EQ(bfifo_get_used(&f_), kSize);
}

TEST_F(Fifo, DataWrappingTheBufferEndIsContiguousOnTheWayOut) {
    /* The two-block copy on both sides. Push 48, pop 40, then push 40 more: the second push
       straddles the end of the buffer and the read that follows straddles it again. */
    auto first = pattern(48, 0);
    ASSERT_EQ(bfifo_push(&f_, first.data(), 48), 48u);
    ASSERT_EQ(bfifo_pop(&f_, 40, 0), 40u);

    auto second = pattern(40, 100);
    ASSERT_EQ(bfifo_push(&f_, second.data(), 40), 40u);
    ASSERT_EQ(bfifo_get_used(&f_), 48u);

    std::vector<uint8_t> out(48);
    ASSERT_EQ(bfifo_read(&f_, out.data(), 48), 48u);
    EXPECT_EQ(std::memcmp(out.data(), first.data() + 40, 8), 0) << "tail of the first push";
    EXPECT_EQ(std::memcmp(out.data() + 8, second.data(), 40), 0) << "the wrapped push";
}

TEST_F(Fifo, OccupancyIsCorrectAcrossTheThirtyTwoBitTotalWrap) {
    /* The property the F67 fix rests on and the one that cannot be eyeballed: occupancy is a
     * MODULAR difference of two free-running totals. Nudged to just below 2^32 so that the
     * next push wraps pushedTotal while poppedTotal has not yet wrapped -- the arrangement in
     * which a plain subtraction gives a nonsense answer.
     *
     * Reaching it honestly would take 4 GB through a console FIFO, so the totals are set
     * directly. That is a white-box shortcut and worth naming as one; the alternative is not
     * testing the property at all. */
    f_.pushedTotal = 0xFFFFFFF0u;
    f_.poppedTotal = 0xFFFFFFF0u;
    ASSERT_EQ(bfifo_get_used(&f_), 0u);

    auto in = pattern(32);
    ASSERT_EQ(bfifo_push(&f_, in.data(), 32), 32u);

    EXPECT_EQ(f_.pushedTotal, 0x10u) << "the total did not wrap; the test proves nothing";
    EXPECT_EQ(bfifo_get_used(&f_), 32u) << "occupancy is wrong across the 2^32 wrap";
    EXPECT_EQ(bfifo_get_free(&f_), kSize - 32u);

    std::vector<uint8_t> out(32);
    EXPECT_EQ(bfifo_read(&f_, out.data(), 32), 32u);
    EXPECT_EQ(std::memcmp(out.data(), in.data(), 32), 0);
}

TEST_F(Fifo, PushAllDeliversEveryByteOfAnOversizedPayload) {
    /* bfifo_push_all() is what MSG() uses. A payload larger than the FIFO must not be
     * truncated -- it blocks until the consumer has made room -- so this needs a consumer.
     * The thread below is the console drain thread's role. */
    constexpr uint32_t kTotal = kSize * 4;
    auto in = pattern(kTotal);
    std::vector<uint8_t> got;
    got.reserve(kTotal);

    static BFifo *s_f;
    static std::vector<uint8_t> *s_got;
    static std::atomic<bool> s_done{false};
    s_f = &f_;
    s_got = &got;
    s_done = false;

    osThreadAttr_t attr{};
    attr.name = "drain";
    attr.stack_size = 8192;
    attr.priority = osPriorityBelowNormal; /* below the pusher, so the FIFO really does fill */
    osThreadId_t th = osThreadNew(
        [](void *) {
            uint8_t buf[16];
            while (s_got->size() < kTotal) {
                bfifo_wait_avail(s_f);
                uint32_t n = bfifo_read(s_f, buf, sizeof(buf));
                bfifo_pop(s_f, n, 0);
                s_got->insert(s_got->end(), buf, buf + n);
            }
            s_done = true;
            osThreadExit();
        },
        nullptr, &attr);
    ASSERT_NE(th, nullptr);

    EXPECT_EQ(bfifo_push_all(&f_, in.data(), kTotal), kTotal);

    for (int i = 0; (i < 2000) && !s_done; i++) {
        osDelay(1);
    }
    ASSERT_TRUE(s_done) << "the consumer never drained the FIFO";
    ASSERT_EQ(got.size(), kTotal);
    EXPECT_EQ(std::memcmp(got.data(), in.data(), kTotal), 0)
        << "bytes were lost or reordered across a blocking push";
}

TEST_F(Fifo, PopWithATimeoutReturnsZeroOnAnEmptyFifo) {
    /* The console thread relies on this to not park for ever on an empty FIFO. */
    EXPECT_EQ(bfifo_pop(&f_, 8, 10), 0u);
}

TEST_F(Fifo, ZeroLengthOperationsAreNoOps) {
    auto in = pattern(8);
    EXPECT_EQ(bfifo_push(&f_, in.data(), 0), 0u);
    EXPECT_EQ(bfifo_pop(&f_, 0, 0), 0u);

    uint8_t out[8];
    EXPECT_EQ(bfifo_read(&f_, out, 0), 0u);
    EXPECT_EQ(bfifo_get_used(&f_), 0u);
}

TEST_F(Fifo, ReadDoesNotConsume) {
    /* read() then pop() is the console thread's sequence: it must be able to look at the data,
       hand it to the DMA, and only then release the space. */
    auto in = pattern(8);
    ASSERT_EQ(bfifo_push(&f_, in.data(), 8), 8u);

    uint8_t out[8];
    EXPECT_EQ(bfifo_read(&f_, out, 8), 8u);
    EXPECT_EQ(bfifo_get_used(&f_), 8u) << "read consumed data";
    EXPECT_EQ(bfifo_read(&f_, out, 8), 8u) << "the same bytes must still be there";

    EXPECT_EQ(bfifo_pop(&f_, 8, 0), 8u);
    EXPECT_EQ(bfifo_get_used(&f_), 0u);
}

TEST_F(Fifo, PoppingMoreThanIsPresentPopsWhatThereIs) {
    auto in = pattern(8);
    ASSERT_EQ(bfifo_push(&f_, in.data(), 8), 8u);
    EXPECT_EQ(bfifo_pop(&f_, 64, 0), 8u);
    EXPECT_EQ(bfifo_get_used(&f_), 0u);
}

TEST_F(Fifo, TheCriticalSectionRestoresPrimaskRatherThanEnablingIt) {
    /* bfifo_crit_exit() re-enables interrupts only if they were enabled on entry. Pushing from
     * a context that already had them masked must leave them masked -- otherwise a push from
     * inside a critical section silently opens a window in it. */
    auto in = pattern(4);

    host_primask = 1;  /* caller had already disabled interrupts */
    bfifo_push(&f_, in.data(), 4);
    EXPECT_EQ(host_primask, 1u) << "the push re-enabled interrupts its caller had masked";

    host_primask = 0;
    bfifo_push(&f_, in.data(), 4);
    EXPECT_EQ(host_primask, 0u);
}

/* --- kernel bring-up, as test_rtos_context.cpp does it ------------------------------------ */

namespace {

int g_result = 1;

void test_main(void *arg) {
    (void)arg;
    g_result = RUN_ALL_TESTS();
    std::exit(g_result);  /* tx_kernel_enter() never returns */
}

}  // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    if (osKernelInitialize() != osOK) {
        std::fprintf(stderr, "osKernelInitialize() failed\n");
        return 2;
    }

    osThreadAttr_t attr{};
    attr.name = "tests";
    attr.stack_size = 65536;
    attr.priority = osPriorityNormal;
    if (osThreadNew(test_main, nullptr, &attr) == nullptr) {
        std::fprintf(stderr, "could not create the test thread\n");
        return 2;
    }

    osKernelStart();
    return 2;
}
