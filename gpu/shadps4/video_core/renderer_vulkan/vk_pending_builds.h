// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {

/// port: retires `key` from a pending-build map once its waited build finished, and only while the
/// entry still belongs to that build. The key is looked up again instead of reusing an iterator
/// taken before the wait: PipelineCache::WaitGraphics publishes shaders while it waits, and
/// publishing drains deferred selections into pending_graphics. An insertion may rehash (or
/// robin-hood shift) a tsl::robin_map, after which erasing through the old iterator corrupts the
/// map: its element count drops while the waited entry stays, the count later wraps below zero
/// and the next insertion doubles the table until std::bad_alloc.
/// Returns false, leaving the map untouched, when the key is absent or now holds another build.
template <class Map, class Key, class Build>
bool RetirePendingBuild(Map& pending, const Key& key, const Build& build) {
    const auto it = pending.find(key);
    if (it == pending.end() || it->second != build) {
        return false;
    }
    pending.erase(it);
    return true;
}

} // namespace Vulkan
