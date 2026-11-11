// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Common::TryEmit (SRT walker): a failed emission leaves no bytes behind, so a push emitted
// before the failure (PushPtr before ComputeOffset) cannot unbalance the walker's stack.
#include <cassert>
#include <cstdio>

#include <xbyak/xbyak.h>

#include "common/try_emit.h"

int main() {
    Xbyak::CodeGenerator c(4096);
    c.nop();
    const size_t before = c.getSize();
    // Failure after a push and a partial address computation: everything is cut.
    const bool failed = Common::TryEmit(c, [&] {
        c.push(c.rdi);
        c.mov(c.r10d, c.dword[c.rsi + 8]);
        return false;
    });
    assert(!failed);
    assert(c.getSize() == before);
    // The next emission starts exactly where the failed one started.
    c.mov(c.dword[c.rsi + 4], 0);
    const size_t after_fill = c.getSize();
    assert(after_fill > before);
    // Success keeps the bytes.
    const bool ok = Common::TryEmit(c, [&] {
        c.push(c.rdi);
        c.pop(c.rdi);
        return true;
    });
    assert(ok);
    assert(c.getSize() == after_fill + 2);
    std::puts("try-emit ok");
    return 0;
}
