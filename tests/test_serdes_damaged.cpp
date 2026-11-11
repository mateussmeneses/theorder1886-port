// SPDX-FileCopyrightText: Copyright 2026 The Order 1886 PC Port contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// A truncated or damaged shader cache blob must throw Serialization::DeserializeError (which
// LoadShaderMeta turns into "missing, translate again") instead of aborting or allocating
// without bound.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "common/serdes.h"

// serdes.h asserts through the port's logging; no logger is linked here.
void assert_fail_impl() {
    std::abort();
}
void unreachable_impl() {
    std::abort();
}
namespace Common::Log {
bool ShouldLog(Level) {
    return false;
}
void Write(std::string_view, Level, const char*, int, const char*, std::string&&) {}
} // namespace Common::Log

using Serialization::Archive;
using Serialization::DeserializeError;
using Serialization::Reader;
using Serialization::Writer;

static bool Throws(auto&& fn) {
    try {
        fn();
    } catch (const DeserializeError&) {
        return true;
    }
    return false;
}

int main() {
    // Round trip still works.
    Archive ar;
    {
        Writer w{ar};
        w.Write(u32{0x1234});
        w.Write(std::vector<u32>{1, 2, 3});
    }
    std::vector<u8> bytes = ar.TakeOff();
    {
        Archive in{std::vector<u8>{bytes}};
        Reader r{in};
        u32 v{};
        std::vector<u32> list;
        r.Read(v);
        r.Read(list);
        assert(v == 0x1234 && list.size() == 3 && list[2] == 3);
    }
    // Truncated: every prefix shorter than the full blob fails cleanly.
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        Archive in{std::vector<u8>(bytes.begin(), bytes.begin() + cut)};
        assert(Throws([&] {
            Reader r{in};
            u32 v{};
            std::vector<u32> list;
            r.Read(v);
            r.Read(list);
        }));
    }
    // A garbage element count larger than the remaining bytes is rejected before allocating.
    {
        Archive bad;
        Writer w{bad};
        w.Write(size_t{1} << 40);
        w.Write(u32{7});
        Archive in{bad.TakeOff()};
        assert(Throws([&] {
            Reader r{in};
            std::vector<u32> list;
            r.Read(list);
        }));
    }
    std::puts("serdes-damaged ok");
    return 0;
}
