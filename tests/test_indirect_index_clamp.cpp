// SPDX-License-Identifier: GPL-2.0-or-later
// Focused regression tests for the indexed indirect draw clamp (vk_index_clamp.h), adapted from
// JulioCacko/The-Order-1886_pc via IgorAugustoHH/The-Order-1886-PC-Port 726b0ab08a.
// No Vulkan device and no game assets: the planning arithmetic is device independent.
#include <cstdint>
#include <cstdio>
#include <limits>
#include "video_core/renderer_vulkan/vk_index_clamp.h"

namespace {

using Vulkan::IndexClamp::ClampIndexCount;
using Vulkan::IndexClamp::ExceedsBound;
using Vulkan::IndexClamp::MakePlan;
using Vulkan::IndexClamp::Record;

int failures = 0;

void Check(bool condition, const char* what, int line) {
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// --- ClampIndexCount ------------------------------------------------------------------------

void TestValidCountsAreUntouched() {
    // The whole point of the guard: a valid draw must be bit-identical afterwards.
    CHECK(ClampIndexCount(0, 0, 1000) == 0);
    CHECK(ClampIndexCount(1, 0, 1000) == 1);
    CHECK(ClampIndexCount(999, 0, 1000) == 999);
    CHECK(ClampIndexCount(1000, 0, 1000) == 1000);      // exactly full
    CHECK(ClampIndexCount(500, 500, 1000) == 500);      // exactly reaches the end
    CHECK(ClampIndexCount(1, 999, 1000) == 1);          // last index
}

void TestCountsPastTheBoundAreClamped() {
    CHECK(ClampIndexCount(1001, 0, 1000) == 1000);
    CHECK(ClampIndexCount(5000, 900, 1000) == 100);
    CHECK(ClampIndexCount(0xFFFFFFFAu, 0, 1000) == 1000);   // the reported case
    CHECK(ClampIndexCount(0xFFFFFFFFu, 0, 0xFFFFFFFFu) == 0xFFFFFFFFu); // fits exactly
}

void TestFirstIndexPastTheBoundYieldsZero() {
    // firstIndex beyond the buffer: nothing is readable, so the count must become 0.
    CHECK(ClampIndexCount(100, 1000, 1000) == 0);
    CHECK(ClampIndexCount(100, 5000, 1000) == 0);
    CHECK(ClampIndexCount(0, 5000, 1000) == 0);
}

void TestUnknownBoundLeavesCountAlone() {
    // max_indices == 0 means no INDEX_BUFFER_SIZE: the port must not invent a bound.
    CHECK(ClampIndexCount(0xFFFFFFFAu, 0, 0) == 0xFFFFFFFAu);
    CHECK(ClampIndexCount(1234, 7, 0) == 1234);
}

void TestZeroAndExtremeInputs() {
    CHECK(ClampIndexCount(0, 0, 0) == 0);
    CHECK(ClampIndexCount(0, 0xFFFFFFFFu, 0xFFFFFFFFu) == 0);
    constexpr auto Max = std::numeric_limits<std::uint32_t>::max();
    CHECK(ClampIndexCount(Max, 0, Max) == Max);
    CHECK(ClampIndexCount(Max, 1, Max) == Max - 1);
    CHECK(ClampIndexCount(Max, Max, Max) == 0);
}

void TestExceedsBound() {
    Record valid{};
    valid.index_count = 100;
    valid.first_index = 50;
    CHECK(!ExceedsBound(valid, 1000));

    Record bad{};
    bad.index_count = 0xFFFFFFFAu;
    bad.first_index = 0;
    CHECK(ExceedsBound(bad, 1000));

    Record edge{};
    edge.index_count = 100;
    edge.first_index = 900;
    CHECK(!ExceedsBound(edge, 1000)); // exactly fits
}

// --- MakePlan -------------------------------------------------------------------------------

void TestPlanForAValidDraw() {
    const auto plan = MakePlan(/*count=*/4, /*offset=*/64, /*stride=*/20, /*max=*/1000,
                               /*alignment=*/16);
    CHECK(plan.has_value());
    CHECK(plan->out_size == 4 * 20);
    CHECK(plan->src_begin == 64);
    CHECK(plan->src_size == 4 * 20);
    CHECK(plan->push_count == 4);
    CHECK(plan->push_first == 0);          // offset already aligned
    CHECK(plan->push_stride_dwords == 5);
    CHECK(plan->push_max_indices == 1000);
}

void TestPlanRejectsUnboundedAndEmpty() {
    CHECK(!MakePlan(4, 64, 20, /*max=*/0, 16).has_value()); // no bound: never clamp
    CHECK(!MakePlan(0, 64, 20, 1000, 16).has_value());      // no records
}

void TestPlanRejectsMisaligned() {
    // The shader addresses 32-bit words.
    CHECK(!MakePlan(4, 63, 20, 1000, 16).has_value());
    CHECK(!MakePlan(4, 64, 19, 1000, 16).has_value());
    CHECK(!MakePlan(4, 64, 18, 1000, 16).has_value()); // smaller than a record
    CHECK(MakePlan(4, 68, 20, 1000, 16).has_value());
}

void TestPlanRejectsBadAlignment() {
    CHECK(!MakePlan(4, 64, 20, 1000, /*alignment=*/0).has_value());
    CHECK(!MakePlan(4, 64, 20, 1000, /*alignment=*/24).has_value()); // not a power of two
    CHECK(MakePlan(4, 64, 20, 1000, /*alignment=*/1).has_value());
}

void TestPlanRejectsOverflowAndHugeCounts() {
    constexpr std::uint32_t Max = std::numeric_limits<std::uint32_t>::max();
    // Reached: count and stride are both 32-bit, so (count-1)*stride peaks just below 2^64 and
    // cannot wrap. What must be rejected is the allocation size, not an overflow.
    CHECK(!MakePlan(Max, 64, 20, 1000, 16).has_value()); // ~86 GB of records
    CHECK(!MakePlan(Max, 64, Max & ~3u, 1000, 16).has_value());
    // Just above the cap is rejected, at the cap is allowed.
    CHECK(!MakePlan(Vulkan::IndexClamp::ClampMaxRecords + 1, 0, 20, 1000, 16).has_value());
    CHECK(MakePlan(Vulkan::IndexClamp::ClampMaxRecords, 0, 20, 1000, 16).has_value());
}

void TestPlanAlignsSourceDown() {
    // Source range runs from the aligned base to the end of the LAST record: 2 records of
    // 20 bytes starting at 72 end at 112, so the range is 64..112 = 48 bytes.
    const auto plan = MakePlan(/*count=*/2, /*offset=*/72, /*stride=*/20, /*max=*/100, 16);
    CHECK(plan.has_value());
    CHECK(plan->src_begin == 64);
    CHECK(plan->push_first == (72 - 64) / 4); // 2 dwords into the aligned range
    CHECK(plan->src_size == 112 - 64);
}

void TestPlanMultipleRecordsAcrossStride() {
    // Stride larger than a record (e.g. 5 dwords of command + padding): the last record's end
    // decides the source size, and the shader indexes records by stride.
    const auto plan = MakePlan(/*count=*/3, /*offset=*/0, /*stride=*/32, /*max=*/256, 16);
    CHECK(plan.has_value());
    CHECK(plan->push_stride_dwords == 8);
    CHECK(plan->src_size == 2 * 32 + 20);
    CHECK(plan->out_size == 3 * 20); // output is packed: 5 dwords each
}

} // namespace

int main() {
    TestValidCountsAreUntouched();
    TestCountsPastTheBoundAreClamped();
    TestFirstIndexPastTheBoundYieldsZero();
    TestUnknownBoundLeavesCountAlone();
    TestZeroAndExtremeInputs();
    TestExceedsBound();
    TestPlanForAValidDraw();
    TestPlanRejectsUnboundedAndEmpty();
    TestPlanRejectsMisaligned();
    TestPlanRejectsBadAlignment();
    TestPlanRejectsOverflowAndHugeCounts();
    TestPlanAlignsSourceDown();
    TestPlanMultipleRecordsAcrossStride();

    if (failures) {
        std::printf("indirect-index-clamp-test: %d FAILED\n", failures);
        return 1;
    }
    std::puts("indirect-index-clamp-test: passed");
    return 0;
}
