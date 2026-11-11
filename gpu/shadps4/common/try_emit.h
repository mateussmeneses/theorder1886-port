// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Common {
// Failed recursive JIT emission must not leave pushes or half-computed addresses behind.
template <typename Generator, typename Emit>
bool TryEmit(Generator& generator, Emit&& emit) {
    const auto start = generator.getSize();
    if (emit()) return true;
    generator.setSize(start);
    return false;
}
}
