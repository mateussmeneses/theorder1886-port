// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
/// The Order: 1886's full scene size, the game resolution patched at start (THEORDER_ORDER_RESOLUTION:
/// 800 retail letterboxed default, 720, 1080, 2160).
inline Size GameSize() {
    static const Size size = [] {
        const char* env = std::getenv("THEORDER_ORDER_RESOLUTION");
        if (!env) env = std::getenv("BB_ORDER_RESOLUTION");
        const int lines = env ? std::atoi(env) : 0;
        if (env && env[0] == 'r') {
            const char* aspect = std::getenv("THEORDER_ORDER_ASPECT");
            if (!aspect) aspect = std::getenv("BB_ORDER_ASPECT");
            if (aspect) {
                const double value = std::atof(aspect);
                if (value >= 1.78 && value <= 2.4) {
                    return Size{1920, uint32_t(std::lround(1920.0 / value / 2.0)) * 2};
                }
            }
            return Size{1920, 800};
        }
        return lines == 2160 ? Size{3840, 2160}
             : lines == 1080 ? Size{1920, 1080}
             : lines == 720  ? Size{1280, 720}
             : Size{1920, 800}; // retail default is 1920x800
    }();
    return size;
}

inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
