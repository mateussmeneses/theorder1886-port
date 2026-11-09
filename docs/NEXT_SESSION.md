# Next session handoff

Living state of the post-v0.8.0 continuation (WaitGraphics, `bad_alloc`, readbacks, 4P
stability). Updated after each milestone; the full account is
`docs/AUTONOMOUS_CONTINUATION_REPORT.md`.

## State (2026-11-09, evening)

- `master` (local) carries the WaitGraphics fix `922a4bb`, `batch_runs --affinity` `6f8788f`,
  these documents `f1e6935` and AI Workbench files; autonomous work continues on branch
  `auto/backlog-20261109` in the clean worktree `../theorder1886-port-wg`, following
  `docs/plans/2026-11-09-autonomous-backlog.md` (progress in its "Diário" section).
- User WIP, untouched and never to be committed: `src/runtime_file.c`, `src/runtime_kernel.c`,
  subtitle traces in `src/probe.c`, submodule `gpu/third_party/fsr-vulkan`. The WIP
  `runtime_file.c` fallback shows a black screen ("Encountered an error"): measure from the clean
  worktree, never from the main checkout.
- The 4P retest is done (`docs/AFFINITY_STABILITY_RETEST.md`, 5 batches, 47/50 complete, 0
  `bad_alloc`, 0 WaitGraphics faults); its two boot faults are open (plan items S1-S3).

## Milestone 1: WaitGraphics iterator invalidation (done, fix implemented and unit-tested)

- Root cause confirmed: see `docs/WAIT_GRAPHICS_INVESTIGATION.md`. The same stale `erase(it)`
  explains both failure kinds of the affinity benchmark: the `0x126ce4` fault and the
  `bad_alloc` (the map's element count wraps below zero, then an insertion doubles the table
  until allocation fails).
- Fix: `gpu/shadps4/video_core/renderer_vulkan/vk_pending_builds.h` (`RetirePendingBuild`, by key
  and only while the entry still holds the waited build) used by `PipelineCache::WaitGraphics`;
  opt-in `THEORDER_PENDING_WAIT_LOG=1` logs waits during which the pending map changed.
- Test: `tests/test_pending_builds.cpp` (`pending-builds-test`, in `tools/run_tests.sh`); the v0.8.0
  pattern fails it (mutation check). Full suite in the clean worktree: 57 native tests and the
  Python suite pass.

## Milestone 2: live diagnostic at 4P (done)

`out/affinity-bench/manifest-fixdiag.json`, fixed build, idle2, `THEORDER_PENDING_WAIT_LOG=1`:
prequeue on 2/2 runs complete, 40 and 26 waits during which the pending map grew (no rehash, 8
buckets); prequeue off (`THEORDER_PREQUEUE_SUBMISSION_PIPELINES=0`) 2/2 complete, 0 such waits.
Private bytes peaked at 5.5-5.8 GiB in all four runs. The hazard fires tens of times a run; in
v0.8.0 it turned into a fault or `bad_alloc` only when the insertion rehashed.

## Milestone 3: readback diagnosis (done, nothing changed)

`docs/GPU_READBACK_INVESTIGATION.md`: `0xcc8791` writes a 36-byte constant block into a two-slot
buffer (`sub_c7c1d0`) whose 4 KiB page also holds 1 KiB the GPU writes; ~56 downloads/s,
186-331 ms/s with a full `scheduler.Finish()` each. Next hypothesis there: wait for the writer's
tick instead of `Finish()` (measure tick gap first).

## Next exact actions

Follow `docs/plans/2026-11-09-autonomous-backlog.md` from the first item not marked done in its
"Diário" section.
