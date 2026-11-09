# Autonomous continuation report: post-v0.8.0 (2026-11-09)

Scope: CPU-affinity benchmark follow-up, `PipelineCache::WaitGraphics`, the `bad_alloc` memory
growth, main-thread write-fault readbacks, 4P stability retest. Nothing was committed, pushed,
tagged or released; Git history is untouched.

## 1. Initial Git state

- `master` = `origin/master` = `19b998bc0de7` ("docs(release): record v0.8.0 validation results",
  tag `windows-v0.8.0` on `9a4b1b4`).
- User WIP present and preserved (not edited, not staged): `src/runtime_file.c`,
  `src/runtime_kernel.c`, subtitle traces in `src/probe.c`, submodule `gpu/third_party/fsr-vulkan`
  (modified content). The WIP `runtime_file.c` fallback stops the game with "Encountered an error",
  so every measurement here ran from a clean worktree (`../theorder1886-port-wg`, detached
  `19b998b` plus only the changes of this task).
- Earlier local changes of the affinity benchmark, kept: `tools/batch_runs.py` (`--affinity`),
  `tests/test_regression_harness.py`, `docs/PERFORMANCE_BASELINE.md`, `docs/PROGRESS.md`.
- Host: Windows 11 10.0.26300, i7-14650HX, RTX 5060 Laptop (NVIDIA 621.14.0.0), ~50 GB free on
  C:, commit limit 51.6 GB (21.4 GB in use at the start).

## 2. Validation of the previous benchmark

`out/affinity-bench/combine.py` over the per-batch `analyze.py` JSON reproduces every number of
the 2026-11-09 restricted-affinity tables. Each failed run was re-read from its log: the runner's
`HUNG`/`EXIT 23` labels hid `bad_alloc` in 9 runs (base 3, idle1 1, idle2 5); 2 runs (idle2)
faulted at `theorder-probe.exe+0x126ce4`; one 2P base run (`p2/base-03`) was not a crash but CPU
starvation (frames up to 15.6 s, watchdog at 125 s while still presenting).

## 3. WaitGraphics investigation

Full account: `docs/WAIT_GRAPHICS_INVESTIGATION.md`. The benchmark build (`64e08804…0a92`) was
rebuilt from the same commit with symbols (`3500127f…4966`) and the fault stack symbolized with
`llvm-symbolizer --inlines`: `robin_hash::erase_from_bucket` → `~shared_ptr<AsyncGraphicsCompilation>`
from `pending_graphics.erase(it)` in `WaitGraphics` (vk_pipeline_cache.cpp:1093-1094), via
`ResolvePendingGraphics`, `Rasterizer::Draw`, `Liverpool::ProcessGraphics`.

## 4. `pending_graphics` container analysis

`tsl::robin_map<GraphicsPipelineKey, shared_ptr<AsyncGraphicsCompilation>>`, used only on the GPU
command thread (re-entrancy, not a data race). `WaitGraphics` held an iterator across
`PublishShaders()`, which since `54a41ab` (pre-queueing, only in windows-v0.8.0) drains deferred
selections and inserts into the same map. An insertion may rehash (iterator dangling) or
robin-hood shift entries (iterator points at another entry). In tsl's `erase_from_bucket` the
element count is decremented unconditionally and `rehash_impl` copies the count, so a stale erase
after a rehash either faults (freed memory reused) or leaves the count one low; once every build is
retired the count wraps to `SIZE_MAX` and the next insertion's `rehash_on_extreme_load` loop
doubles the table until `std::bad_alloc`. Reproduced outside the game with a one-off program
(`size()` = 18446744073709551615, then `bad_alloc`). With
`THEORDER_PREQUEUE_SUBMISSION_PIPELINES=0` nothing is inserted during a wait.

## 5. Fix and tests

- `gpu/shadps4/video_core/renderer_vulkan/vk_pending_builds.h` (new): `RetirePendingBuild(map,
  key, build)` looks the key up again and erases only if it still holds the waited build.
- `vk_pipeline_cache.cpp`: `WaitGraphics` keeps the waited `shared_ptr` and retires through
  `RetirePendingBuild` on both paths; opt-in `THEORDER_PENDING_WAIT_LOG=1` logs waits during which
  the map changed size or bucket count. No synchronization, priority or pre-queue behavior changed.
- `tests/test_pending_builds.cpp` (`pending-builds-test`, in `tools/run_tests.sh`, target in
  `gpu/CMakeLists.txt`): real `tsl::robin_map` with a 320-byte key; insertion that rehashes during
  the wait then the `bad_alloc` path; shifts without rehash; removal and replacement during the
  wait; nested and repeated waits; pre-queue on and off; 200 rounds of 3 worker threads completing
  builds concurrently (each key published exactly once, `size()` = walked count after every step).
- Results: `pending-builds-test` passed 10/10 consecutive runs; mutation check: the v0.8.0 pattern
  (`erase(it)` after the wait) fails the first case; clean build of the fixed tree
  (`970b9d33…0c97`); `bash tools/run_tests.sh` in the clean worktree: "PASS: all 57 native GPU/CPU
  tests and Python suite" (Python: 348 tests OK, 41 skipped; that run predates copying the 3
  affinity harness tests into the worktree, see section 13).
- Live, 4P, fixed build (`manifest-fixdiag.json`): pre-queue on 2/2 complete with 40 and 26 waits
  during which the map grew; pre-queue off 2/2 complete with 0 such waits; private ≤ 5.8 GiB.

## 6. `bad_alloc` investigation and origin of the memory growth

`liverpool.h:208` is the coroutine's `unhandled_exception()`, not the allocation site. In all nine
`bad_alloc` logs the exception follows a draw `STALLED` on a pending pipeline (the `WaitGraphics`
path) by 4-27 lines, and private bytes jump in one 5 s sample (5.4 → 14.5, 5.0 → 14.2,
10.0 → 23.5, 41.7 → 77.8 GiB; others plateau at the commit limit) while Vulkan allocations stay at
~2.1 GiB. Counters: "Process metrics … private" is `PROCESS_MEMORY_COUNTERS_EX.PrivateUsage` of the
game (`gpu/shim/port_process_memory.cpp`, every 5 s); the limit is the system commit limit
(`Win32_OperatingSystem.TotalVirtualMemorySize`). The growing structure is the bucket array of
`pending_graphics` after its element count wrapped (section 4); power-of-two jumps match the
doubling. No separate memory investigation document was needed: the growth has one proven cause,
written up in `docs/WAIT_GRAPHICS_INVESTIGATION.md`. After the fix: 0 `bad_alloc` in 44 runs,
per-run peak private 5.5-5.8 GiB.

## 7. Readback analysis

`docs/GPU_READBACK_INVESTIGATION.md`. Eboot `0xcc8791` (`VMOVSS [RAX], XMM0` in `sub_cc85a0`,
caller `sub_ca47c0`) writes a 36-byte constant block into a two-slot ping-pong buffer returned by
`sub_c7c1d0`; the two slots (`…23b4`, `…23e4`) share a 4 KiB page with 1 KiB the GPU writes
(`…2414`). Each store faults on the write-protected page and the game thread blocks while the GPU
thread runs `DownloadMemory` with a full `scheduler.Finish()`: ~56 downloads/s, 186-331 ms/s
(19-33 % of that thread's time), none touching GPU-written bytes. Diagnosis only; three hypotheses
recorded (tick wait instead of `Finish()`, smaller window, reuse of the read-watch copy), each
needing a capture-matched A/B.

## 8. Ghidra/REA usage

REA `rea-agents` 6.7.0 with Ghidra 12.1.4 on `out/eboot_ghidra.exe` (from `scripts/eboot_pe.py`,
SHA-256 `0f42dce6…146f`; eboot `f742eb17…58d8`; Ghidra address = `0x100000000` + eboot offset):
`inspect_native_instruction` (0x100cc8791), `read_function_instructions` (`sub_cc85a0`,
`sub_c7c1d0`), `procedure_callers`, `xrefs`, `find_xrefs_to_name`, `batch_decompile`. The REA
session was closed after use. Port-side stacks used `llvm-symbolizer`/`llvm-objdump` from MSYS2
CLANG64 on the exact images named above.

## 9. Stability runs

`docs/AFFINITY_STABILITY_RETEST.md`: 4P (`0x5500`), 20 base + 20 idle2, fixed build, prologue
route, warm cache, frame traces, conditions alternated, four sequential batches, disk/commit checks
and a private-bytes guard (never triggered; game private ≤ 5.76 GiB, but system commit reached 50.4 of
51.6 GB, only partly explained by the 6 GiB guest memory section; see the retest
document).

- base 19/20, idle2 19/20 complete; 0 `bad_alloc`, 0 `0x126ce4` faults, 0 watchdog stops, 0 runner
  errors, 0 resource stops.
- 14 runs had a pending-map rehash during a wait (the v0.8.0 failure condition) and completed.
- Two single, unattributed boot faults 9 s apart in the last batch: `Scheduler::Finish` on a null
  scheduler from `Fsr4Upscaler::Impl::Destroy` (base) and `nvoglv64.dll` inside the presenter's
  fence wait (idle2). A confirmation batch is in section 10.

## 10. A/B data

| | base | idle2 |
|---|---|---|
| Complete (fixed / v0.8.0) | 19/20 / 7/10 | 19/20 / 5/8 |
| FPS | 56.49 ± 0.57 | 57.08 ± 0.27 |
| P50 / P95 ms | 16.66 / 18.46 ± 0.84 | 16.65 / 17.64 ± 0.14 |
| P99 ms | 39.12 ± 1.44 | 36.02 ± 2.30 |
| P99.9 ms | 284.9 ± 27.7 | 233.8 ± 10.9 |
| Worst ms | 469.4 ± 38.3 | 356.3 ± 43.2 |
| Process CPU (cores of 4) | 3.92 | 1.39 |
| Peak private GiB (max) | 5.76 | 5.75 |

(mean ± 95 % CI of per-run values, completed runs, last 45 s.) Targeted failure kinds 6/18
(v0.8.0) → 0/40 (Fisher p = 0.0005); any failure 6/18 → 2/40 (p = 0.008). The v0.8.0 claim that
idle2 removes most missed vblanks at 4P did not hold on 19 runs (P99 CIs overlap).

CONFIRMATION_BATCH_PLACEHOLDER

## 11. Tests

| Test | Result |
|---|---|
| `pending-builds-test` (new) | 10/10 runs pass |
| Mutation: v0.8.0 erase pattern | fails as intended (`!pending.contains(MakeKey(3))`) |
| `tools/run_tests.sh`, clean worktree | 57 native tests + Python suite pass (348 OK, 41 skipped) |
| Fixed-build game runs | fixdiag 4/4, retest 38/40 (failures above) |

FINAL_SUITE_PLACEHOLDER

## 12. Modified files (uncommitted)

This task:
- `gpu/shadps4/video_core/renderer_vulkan/vk_pending_builds.h` (new)
- `gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp` (`WaitGraphics` only)
- `tests/test_pending_builds.cpp` (new), `gpu/CMakeLists.txt`, `tools/run_tests.sh`
- `docs/WAIT_GRAPHICS_INVESTIGATION.md`, `docs/GPU_READBACK_INVESTIGATION.md`,
  `docs/AFFINITY_STABILITY_RETEST.md`, `docs/NEXT_SESSION.md`, this report (new);
  `docs/PERFORMANCE_BASELINE.md`, `docs/PROGRESS.md` (sections added)

Earlier in the same session (affinity benchmark): `tools/batch_runs.py`,
`tests/test_regression_harness.py`, `docs/PERFORMANCE_BASELINE.md`, `docs/PROGRESS.md`.

User WIP, untouched: `src/runtime_file.c`, `src/runtime_kernel.c`, `src/probe.c`,
`gpu/third_party/fsr-vulkan`.

Not versioned: `out/affinity-bench/` (manifests, logs, frame traces, analysis scripts).

## 13. Limitations

- One host, one route (prologue opening), one GPU/driver; affinity is not an older CPU.
- The retest ran with `THEORDER_PENDING_WAIT_LOG=1` (one log line per changed wait); the v0.8.0
  batches ran without it.
- v0.8.0 frame-time statistics come from completed runs only (survivor sample).
- The two boot faults have one occurrence each; no cause established.
- The readback hypotheses are untested.
- `bash tools/run_tests.sh` in the worktree ran before the 3 affinity harness tests were copied in.

## 14. Final working-tree state

FINAL_STATE_PLACEHOLDER

## 15. Exact next task

NEXT_TASK_PLACEHOLDER
