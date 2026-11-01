// SPDX-FileCopyrightText: Copyright 2026 The Order: 1886 PC port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Adapted from JulioCacko/The-Order-1886_pc 386715a1 + 471969f7, via
// IgorAugustoHH/The-Order-1886-PC-Port 726b0ab08a ("indirect index clamp").
// The reference implements this inline in Rasterizer::ClampIndexedIndirect(); the planning
// arithmetic is split out here so it can be tested without a Vulkan device.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Vulkan::IndexClamp {

/// Upper bound on records the clamp will copy. A draw with more is not a real draw, and
/// copying it would require an allocation of many gigabytes (0xFFFFFFFF records is ~86 GB,
/// which the reference would attempt). Diagnostics still see the anomaly.
inline constexpr std::uint32_t ClampMaxRecords = 1u << 20;

/// An index count at or above this is reported as an anomaly. Diagnostics only.
inline constexpr std::uint32_t AnomalousCountThreshold = 1u << 20;

/// One indexed indirect record: the five dwords of VkDrawIndexedIndirectCommand
/// (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance).
struct Record {
    std::uint32_t index_count{};
    std::uint32_t instance_count{};
    std::uint32_t first_index{};
    std::uint32_t vertex_offset{};
    std::uint32_t first_instance{};
};

/// Result of planning a clamp pass for one drawIndirect call.
struct Plan {
    /// Destination size in bytes for the copied records.
    std::uint64_t out_size{};
    /// Alignment to apply to the ring head, from Instance::StorageMinAlignment().
    std::uint64_t alignment{};
    /// Source bytes actually read: from the aligned-down base to the last record's end.
    std::uint64_t src_begin{};
    std::uint64_t src_size{};
    /// Push constants for indirect_index_clamp.comp (count, first dword, stride dwords, max).
    std::uint32_t push_count{};
    std::uint32_t push_first{};
    std::uint32_t push_stride_dwords{};
    std::uint32_t push_max_indices{};
};

/// Bounds an index count to what the bound index buffer can supply.
///
/// GCN fetches indices past INDEX_BUFFER_SIZE as 0, so a count that runs past the end there
/// produces degenerate primitives that draw nothing. Vulkan has no such bound and reads the
/// memory anyway, so a GPU-written count of e.g. 0xFFFFFFFA keeps the GPU busy for minutes.
/// Clamping to `max_indices - first_index` reproduces the hardware's visible result exactly:
/// every index past the end was a zero vertex anyway.
///
/// Returns `count` unchanged when the count already fits, so valid draws are bit-identical.
[[nodiscard]] inline std::uint32_t ClampIndexCount(std::uint32_t count, std::uint32_t first_index,
                                                   std::uint32_t max_indices) noexcept {
    // No known bound (no INDEX_BUFFER_SIZE): leave the count as the guest wrote it.
    if (max_indices == 0) {
        return count;
    }
    const std::uint64_t available =
        std::uint64_t(max_indices) > std::uint64_t(first_index)
            ? std::uint64_t(max_indices) - std::uint64_t(first_index)
            : 0;
    return std::uint32_t(std::min<std::uint64_t>(std::uint64_t(count), available));
}

/// Whether a record's count would read past the bound index buffer, i.e. whether the guest
/// asked for more indices than the buffer can supply.
[[nodiscard]] inline bool ExceedsBound(const Record& record, std::uint32_t max_indices) noexcept {
    return ClampIndexCount(record.index_count, record.first_index, max_indices) !=
           record.index_count;
}

/// Plans the clamp dispatch, or returns nullopt when the draw must be left untouched.
///
/// Rejected (no clamp) when: no bound is known, the draw has no records, the argument buffer
/// or stride is not dword-aligned (the reference shader addresses 32-bit words), the record
/// count is larger than ClampMaxRecords, or the alignment is not a power of two.
///
/// Note on overflow: `count` and `stride` are both 32-bit, so `(count - 1) * stride` reaches at
/// most ~1.84e19, just under 2^64. The arithmetic below therefore cannot wrap for any guest
/// input, which is why there is no overflow branch; the size cap is the real guard.
[[nodiscard]] inline std::optional<Plan> MakePlan(std::uint32_t count, std::uint64_t offset,
                                                  std::uint32_t stride,
                                                  std::uint32_t max_indices,
                                                  std::uint64_t alignment) noexcept {
    constexpr std::uint64_t RecordBytes = 5 * sizeof(std::uint32_t);

    if (max_indices == 0 || count == 0) {
        return std::nullopt;
    }
    // A draw with more records than this is not a real draw; clamping it would ask for an
    // allocation of many gigabytes (0xFFFFFFFF records is ~86 GB). Leave it to the normal path.
    if (count > ClampMaxRecords) {
        return std::nullopt;
    }
    // The shader reads and writes 32-bit words and indexes records by dword stride.
    if ((offset % 4) != 0 || (stride % 4) != 0 || stride < RecordBytes) {
        return std::nullopt;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return std::nullopt;
    }

    const std::uint64_t out_size = std::uint64_t(count) * RecordBytes;
    const std::uint64_t last = std::uint64_t(count - 1) * stride;
    const std::uint64_t src_begin = offset & ~(alignment - 1);
    const std::uint64_t src_end = offset + last + RecordBytes;

    Plan plan{};
    plan.out_size = out_size;
    plan.alignment = alignment;
    plan.src_begin = src_begin;
    plan.src_size = src_end - src_begin;
    plan.push_count = count;
    plan.push_first = std::uint32_t((offset - src_begin) / 4);
    plan.push_stride_dwords = stride / 4;
    plan.push_max_indices = max_indices;
    return plan;
}

} // namespace Vulkan::IndexClamp
