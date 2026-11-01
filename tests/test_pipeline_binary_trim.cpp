// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

int main() {
    namespace fs = std::filesystem;
    const fs::path test_dir = fs::temp_directory_path() / "test_theorder_bin_trim";
    std::error_code ec;
    fs::remove_all(test_dir, ec);
    fs::create_directories(test_dir, ec);
    assert(!ec);

    const auto write_dummy_file = [&](const fs::path& path, size_t size_bytes) {
        std::ofstream ofs(path, std::ios::binary);
        std::vector<char> data(size_bytes, 0x5a);
        ofs.write(data.data(), data.size());
    };

    // Case 1: Under budget (total 2 MB, budget 4 MB) -> 0 removed
    write_dummy_file(test_dir / "pipe1.bin", 1024 * 1024);
    write_dummy_file(test_dir / "pipe2.bin", 1024 * 1024);
    size_t removed = Vulkan::TrimPipelineBinaries(test_dir, 4 * 1024 * 1024);
    assert(removed == 0);
    assert(fs::exists(test_dir / "pipe1.bin"));
    assert(fs::exists(test_dir / "pipe2.bin"));

    // Case 2: Over budget (total 5 MB, budget 4 MB) -> oldest removed down to 75% (3 MB)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_dummy_file(test_dir / "pipe3.bin", 1024 * 1024);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_dummy_file(test_dir / "pipe4.bin", 1024 * 1024);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_dummy_file(test_dir / "pipe5.bin", 1024 * 1024);

    // Total now 5 MB. With budget 4 MB, target 75% is 3 MB. Needs to remove 2 oldest files (pipe1, pipe2).
    removed = Vulkan::TrimPipelineBinaries(test_dir, 4 * 1024 * 1024);
    assert(removed == 2);
    assert(!fs::exists(test_dir / "pipe1.bin"));
    assert(!fs::exists(test_dir / "pipe2.bin"));
    assert(fs::exists(test_dir / "pipe3.bin"));
    assert(fs::exists(test_dir / "pipe4.bin"));
    assert(fs::exists(test_dir / "pipe5.bin"));

    fs::remove_all(test_dir, ec);
    std::puts("pipeline-binary-trim-test: PASS");
    return 0;
}
