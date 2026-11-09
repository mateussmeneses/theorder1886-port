# `PipelineCache::WaitGraphics`: stale `pending_graphics` iterator (2026-11-09)

One defect behind both failure kinds of the restricted-affinity benchmark of v0.8.0
(`docs/PERFORMANCE_BASELINE.md`, 2026-11-09): the native fault at `theorder-probe.exe+0x126ce4`
and the `std::bad_alloc` reported at `liverpool.h:208`.

## Builds and evidence

| Item | Value |
|---|---|
| Benchmarked build | `19b998b` (windows-v0.8.0 + docs), worktree `theorder1886-port-aff`, exe SHA-256 `64e08804…0a92` |
| Symbolized build | same commit rebuilt in `theorder1886-port-wg`, exe SHA-256 `3500127f…4966`; only embedded source paths differ, the code layout resolves the benchmark's offsets to the same functions (below) |
| Fixed build | `19b998b` + this fix, exe SHA-256 `970b9d33…0c97` |
| Game | CUSA00035 01.02 |
| Logs | `out/affinity-bench/logs/p4/idle2-01-*.log`, `logs/p2/idle2-02-*.log` (faults); nine `bad_alloc` logs (list below); not versioned |

### The fault

Thread `shadPS4:GpuCommandProcessor`, unwound stack (benchmark build), symbolized with
`llvm-symbolizer --inlines` on the rebuilt image (ImageBase `0x140000000`):

| Frame | Offset | Function (inlined chain) |
|---|---|---|
| 0 | `+0x126ce4` | `__libcpp_atomic_refcount_decrement` ← `shared_ptr<AsyncGraphicsCompilation>::~shared_ptr` ← `pair<GraphicsPipelineKey, shared_ptr<…>>::~pair` ← `bucket_entry::destroy_value`/`clear` ← `robin_hash::erase_from_bucket` (robin_hash.h:1131) |
| 1 | `+0x1071ac` | `PipelineCache::WaitGraphics` (vk_pipeline_cache.cpp:1093-1094, `pending_graphics.erase(it)`) |
| 2 | `+0x3aecc` | `Rasterizer::ResolvePendingGraphics` |
| 3 | `+0x3b142` | `Rasterizer::Draw` (vk_rasterizer.cpp:1077) |
| 4 | `+0x2093cb` | `Liverpool::ProcessGraphics` lambda (liverpool.cpp:1000) |

### The `bad_alloc`

`liverpool.h:208` is only the coroutine's `unhandled_exception()`: an exception escaping
`ProcessGraphics` on the GPU command thread. In all nine `bad_alloc` runs (base 3, idle1 1,
idle2 5; `logs/p2/idle2-01`, `p2b/idle2-03`, `p4/base-03`, `p4/idle2-02`, `p4b/idle2-02`,
`p4c/base-02`, `p4d/base-02`, `p4d/idle1-01`, `p8/idle2-02`) the exception follows a draw stalled
on a pending pipeline (`STALLED`, the `WaitGraphics` path) by 4 to 27 log lines, and the last
`Process metrics` line jumps in one 5 s interval: private bytes 5.4 → 14.5 GiB, 5.0 → 14.2,
10.0 → 23.5, 41.7 → 77.8 (others plateau at ~41.5 GiB, the commit limit of this host, 51.6 GB
with the page file growing). Vulkan allocations stay at ~2.1 GiB: the growth is CPU heap.
"Process metrics" is the game process's private bytes (`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`),
sampled every 5 s; the commit limit is the system's (`Win32_OperatingSystem.TotalVirtualMemorySize`).

## Mechanism

All accesses to `pending_graphics` (`tsl::robin_map<GraphicsPipelineKey,
shared_ptr<AsyncGraphicsCompilation>>`, vk_pipeline_cache.h) run on the GPU command thread:
`QueueGraphicsBuild`, `RecordDeferredSelection`, `DrainDeferredSelections`, `PublishGraphics`,
`WaitGraphics`, `GetGraphicsPipeline`. Compiler workers only touch the `AsyncGraphicsCompilation`
they hold. So this is re-entrancy on one thread, not a data race:

1. `WaitGraphics(key)` takes `it = pending_graphics.find(key)` (since `964bd03`, FEAT-004).
2. While the build compiles it loops `front_pgm->pending->job.Wait(); PublishShaders();`.
3. `PublishShaders()` (wait = false) calls `DrainDeferredSelections()` once a shader was
   published (`54a41ab`, pre-queueing, `THEORDER_PREQUEUE_SUBMISSION_PIPELINES`, default on; only
   in windows-v0.8.0). Draining `pending_graphics.emplace`s the deferred selections whose stages
   are now ready.
4. An insertion can rehash the robin map (all iterators invalid) or robin-hood shift entries.
5. `WaitGraphics` then calls `pending_graphics.erase(it)`.

`robin_hash::erase_from_bucket(pos)` (tsl-robin-map, MSYS2 `clang64/include/tsl/robin_hash.h`)
clears `*pos.m_bucket`, decrements `m_nb_elements` unconditionally and derives the bucket index
as `pos.m_bucket - m_buckets`. After a rehash `pos` points into the freed array:
- if that memory was reused, clearing it runs a `shared_ptr` destructor on garbage: the fault;
- otherwise the element count drops by one while the waited entry stays in the new table.
  `rehash_impl` copies `m_nb_elements` instead of counting, so the error persists; when every
  build is retired the count wraps to `SIZE_MAX`, and the next insertion's
  `while (rehash_on_extreme_load(...))` loop doubles the table until allocation fails:
  the `bad_alloc` with power-of-two jumps of CPU memory.

Reproduced outside the game with a one-off program (scratchpad, not versioned): six entries, an
iterator to one, three insertions (16 → 32 buckets), erase through the iterator. For every
waited position: `size()` 8 with 9 entries walked, the waited key still present; after retiring
all entries `size()` = 18446744073709551615; the next insertion throws `std::bad_alloc`
(allocator capped at 256 MiB per array).

With `THEORDER_PREQUEUE_SUBMISSION_PIPELINES=0` `RecordDeferredSelection` returns at once, so
`deferred_selections` stays empty, `DrainDeferredSelections` returns immediately and nothing is
inserted during the wait: the stale iterator cannot occur.

Without a rehash an insertion may still shift the waited entry; `erase(it)` then removes another
pending build (its publication is lost and the pipeline is compiled again later) and leaves the
waited one, which `PublishGraphics` publishes again later (`graphics_pipelines.emplace` keeps the
first). The count stays consistent in that case.

## Fix

`gpu/shadps4/video_core/renderer_vulkan/vk_pending_builds.h`: `RetirePendingBuild(map, key,
build)` looks the key up again and erases only while the entry still holds the waited build.
`WaitGraphics` keeps the `shared_ptr` it waits on (lifetime is unchanged: the build was already
held by value) and retires it with `RetirePendingBuild` on both paths (completed, failed). Defined
behavior for cases that do not occur today but are now safe: key removed during the wait → nothing
erased, the waited result is still published/returned; key now holding another build → that
build stays pending. No synchronization, container, compile priority or prequeue behavior changed.

Opt-in diagnostic: `THEORDER_PENDING_WAIT_LOG=1` logs each wait during which the pending map
changed size or bucket count (two `size_t` reads per wait otherwise).

## Tests

`tests/test_pending_builds.cpp` (`pending-builds-test`, in `tools/run_tests.sh`), real
`tsl::robin_map` with a 320-byte key like `GraphicsPipelineKey`: insertion that rehashes during
the wait, then retiring everything and inserting again (the `bad_alloc` path); insertions that
shift the waited entry without a rehash (no other build lost); removal and replacement during the
wait; nested waits with several publications; prequeue on and off; 200 rounds of three worker
threads completing builds while the owner waits, drains and publishes (every key published exactly
once, `size()` equal to the walked count after every step). Mutation check: the v0.8.0 pattern
(`erase(it)` after the wait) fails the first case.

Results: see `docs/AUTONOMOUS_CONTINUATION_REPORT.md` (unit, full suite, live 4P runs).
