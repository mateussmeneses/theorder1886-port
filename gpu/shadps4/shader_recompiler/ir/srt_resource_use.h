// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <string_view>
#include <boost/container/small_vector.hpp>
#include <magic_enum/magic_enum.hpp>
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Optimization {
inline bool IsSrtDescriptorOperand(IR::Opcode opcode, unsigned operand) {
    const std::string_view name = magic_enum::enum_name(opcode);
    if (opcode == IR::Opcode::ImageHandle) return operand < 2;
    // The raw sample has two separate handles. All other image instructions
    // use argument 0 for their (possibly combined image/sampler) handle;
    // argument 1 is coordinates, LOD, or sample data, never a second descriptor.
    if (opcode == IR::Opcode::ImageSampleRaw) return operand < 2;
    if (name.starts_with("Image")) return operand == 0;
    if (name.starts_with("LoadBuffer") || name.starts_with("StoreBuffer") ||
        name.starts_with("BufferAtomic") || opcode == IR::Opcode::ReadConstBuffer)
        return operand == 0;
    return false;
}
inline bool IsSrtReadBoundary(IR::Opcode opcode) {
    const std::string_view name = magic_enum::enum_name(opcode);
    // A coordinate/address influences which word is read, not the bits of
    // the descriptor. Classify that later read separately, like ReadConst.
    return opcode == IR::Opcode::ReadConst || opcode == IR::Opcode::ReadConstBuffer ||
           name.starts_with("LoadBuffer") ||
           (name.starts_with("Image") && opcode != IR::Opcode::ImageHandle);
}
inline bool SrtFeedsDescriptor(const IR::Inst* inst, bool* truncated = nullptr) {
    boost::container::small_vector<const IR::Inst*, 64> work{inst}, seen{inst};
    while (!work.empty()) {
        const auto* node = work.back(); work.pop_back();
        for (const auto& use : node->Uses()) {
            if (IsSrtDescriptorOperand(use.user->GetOpcode(), use.operand)) return true;
            if (IsSrtReadBoundary(use.user->GetOpcode())) continue;
            if (std::ranges::find(seen, use.user) != seen.end()) continue;
            if (seen.size() >= 4096) {
                if (truncated) *truncated = true;
                return true;
            }
            seen.push_back(use.user); work.push_back(use.user);
        }
    }
    return false;
}
} // namespace Shader::Optimization
