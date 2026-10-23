#include <cassert>
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"

int main() {
    Vulkan::WaitDiagnostics stats;
    stats.Add("texture", 81, 10, 30);
    stats.Add("texture", 81, 20, 40);
    stats.Add("present", 506, 0, 5);
    auto rows = stats.Take();
    assert(rows[0].calls == 2 && rows[0].submit_ns == 30 && rows[0].wait_ns == 70);
    assert(rows[0].max_wait_ns == 40 && rows[1].calls == 1);
    assert(stats.Take()[0].calls == 0);
    stats.Add("vk_scheduler.cpp", 443, 10, 40, 18);
    stats.Add("vk_presenter.cpp", 601, 10, 80, 21);
    auto longest = stats.TakeLongestWait();
    assert(longest.file == "vk_presenter.cpp" && longest.line == 601);
    assert(longest.tick == 21 && longest.wait_ns == 80);
    longest = stats.TakeLongestWait();
    assert(longest.file.empty() && longest.wait_ns == 0);
    stats.Take();
    for (unsigned i = 0; i < 100; ++i) stats.Add("many", i, 1, 2);
    rows = stats.Take();
    unsigned calls = 0;
    for (const auto& row : rows) calls += row.calls;
    assert(calls == 100);
}
