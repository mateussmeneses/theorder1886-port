# Stabilization validation — O6 (2026-10-22)

## Conditions and decision

- Host: Windows x86-64, NVIDIA GeForce RTX 5060 Laptop GPU, driver 621.1.0.0, Vulkan 1.4.357. The checked game identity is CUSA00035 / 01.02 with eboot SHA-256 `f742eb1793f92772a5c4bdd174e29b08e0b53f68d373a37f4cda730703e558d8`.
- Each short cold/warm pass started from a copy of the same isolated save, used the same timed input script (24–90 s), then captured three output frames at about 105 s. Settings: 30 FPS, 1920×1080, original aspect, upscaler off, HDR off, VSync on.
- Port cache was isolated by run and version. Cold omitted the user shader/pipeline cache; warm used cache files created by that version's cold run while restoring the original save. NVIDIA driver and OS caches were shared and uncontrolled.
- Every pass exited without a reported guest fault or hang, and all CUSA/version checks matched. The captured final frames visibly differ across cold/warm passes, including within the same build. Therefore the timed QTE route did **not** establish equivalent final positions; the short performance numbers below are smoke diagnostics, not a version ranking or evidence that v0.6.1 is smoother.

## Builds and short passes

All three release commits were checked out in detached worktrees and built with MSYS2 CLANG64 (`THEORDER_LTO=OFF`). The HEAD executable is the existing tested build for `d5534c570f5de681596d0f4f3963bb915db676be`; its source tree still contains the O3.1 changes.

| Version | Commit | Executable SHA-256 | Cold / warm flips | Sampled median FPS, cold / warm | Worst sampled frame, cold / warm | Per-frame CSV |
| --- | --- | --- | ---: | ---: | ---: | --- |
| windows-v0.6.1 | `bbeeb45133feb1077798dd116f9931cfdd33be3f` | `4a28cec9ae8572e3e658aef28b3e58dac22c8247b21e997d5d5eff1c84026b74` | 3480 / 3420 | 29.5 / 29.4 | 1165 / 2867 ms | unavailable in this tag |
| windows-v0.7.5 | `b98248367ff9f80353abe48a97af7eb966f416ab` | `49706a712a8c6b1dd43d0e1e71c632a33d4e251df4d6dba5d89dc858d04842d1` | 3600 / 3660 | 30.0 / 30.0 | 634 / 433 ms | unavailable in this tag |
| windows-v0.7.7 | `28064e87adfdcc304ed446a4c0b9ca0199596a3d` | `d2f243cadc0947f026a98ae9dc27c4eb458907dc699759761550df69601c8ce7` | 3300 / 3540 | 28.1 / 30.0 | 967 / 833 ms | available |
| HEAD | `d5534c570f5de681596d0f4f3963bb915db676be` | `48a920fc74a57b0a1039ae8154c94620120c39423c0e721c9978a9ac7bf0928d` | 3420 / 3420 | 28.9 / 29.9 | 834 / 3834 ms | available |

“Sampled” here means the runtime's roughly five-second summary windows, not raw per-frame percentiles. The different final frames invalidate a direct performance comparison even though save source, input script, CUSA, eboot, resolution and settings matched.

### Available short per-frame traces

| Run | Frames | P50 / P95 / P99 / P99.9 / max (ms) | Frames >50 / >100 ms | >50 / >100 ms per minute | Compile wait >50 ms among >100 ms frames |
| --- | ---: | --- | ---: | ---: | ---: |
| v0.7.7 cold | 3219 | 33.33 / 34.23 / 200.32 / 690.15 / 966.50 | 65 / 43 | 32.4 / 21.4 | 41 |
| v0.7.7 warm | 3479 | 33.32 / 34.16 / 34.61 / 384.29 / 833.04 | 18 / 12 | 9.0 / 6.0 | 10 |
| HEAD cold | 3339 | 33.33 / 34.21 / 67.38 / 566.79 / 833.79 | 48 / 32 | 23.8 / 15.9 | 29 |
| HEAD warm | 3360 | 33.34 / 34.17 / 34.68 / 520.95 / 3833.68 | 28 / 16 | 13.9 / 8.0 | 14 |

These traces are short and their final images differ; the apparent cold/warm reduction in some counts is not a controlled claim. v0.6.1 and v0.7.5 do not emit this CSV, so their per-frame P95/P99/P99.9 and >50/>100 ms counts are not collected. Older tags also do not log the current VMA and per-process WDDM metrics.

## HEAD warm soak — 30 minutes

One completed 1800-second run reached 43,954 recorded frame intervals (1796.8 seconds of trace). It had no reported guest fault or hang. The log reports 764 pipeline binary-cache hits, 427 compiled/saved, zero rejected or cache errors, and 4.63 s in its compile summary. The per-frame `compile_wait_ns` values are a separate measure and must not be added to compiler totals.

| P50 / P95 / P99 / P99.9 / worst | Frames >50 / >100 ms | >50 / >100 ms per minute | `compile_wait` sum / max | `gpu_tick_wait` sum / max |
| --- | ---: | ---: | ---: | ---: |
| 33.424 / 66.983 / 100.348 / 733.987 / 2729.843 ms | 6136 / 598 | 204.9 / 20.0 | 103.9 s / 2618.1 ms | 691.1 s / 327.6 ms |

For the 598 frames over 100 ms, 392 contain a GPU-tick wait over 50 ms, 199 contain a compile wait over 50 ms, and 86 overlap other measured wait categories; four have none of those waits. These categories are not exclusive. The largest frame (ID 1221, 2729.8 ms) includes 2618.1 ms compile wait. Another 2267.3 ms frame (ID 216) has no compile wait and uploads 161.7 MiB of images plus 86.6 MiB of buffers; its remaining cause is not identified. The trace has no pipeline key or SPIR-V hash per frame, so it proves multiple stall classes, not the individual pipeline or GPU submission responsible.

Peak memory samples from the same run: process private commit 7748.8 MiB; Working Set 6443.2 MiB; renderer VMA allocations 2305.2 MiB; WDDM dedicated process 2872.4 MiB; WDDM shared process 1397.2 MiB; Vulkan adapter-wide usage 2880.2 MiB. Keep these separate; none is an additive component of another. Private commit rose sharply during startup and stayed in a narrower 7627–7749 MiB band in the final five minutes. VMA still rose from about 2130 to 2305 MiB in that interval, so this run alone does not establish a leak or a full plateau. The startup log's Vulkan memory budget was 6239 MiB; the separately sampled budget field varied, peaking at 7131 MiB.

The 60-FPS/FSR 3.1 interaction smoke selected FSR 3.1 (`Upscaler: ... (on)`) and logged a 60-Hz vblank/frame limit, with no guest fault. Its measured median remained about 30 FPS, so it did not validate a sustained 60-FPS workload. 120 FPS and Frame Generation were not tested.

## Evidence files

- 30-minute trace: [`frames.csv`](../out/o6-head-soak-data/head-warm-01-20261022T224613945867Z/frames.csv); runtime log: [`head-warm-01 log`](../out/batch/head-warm-01-20261022T224613945867Z.log). The batch wrapper defaults to eight runs; it was stopped after this first completed run to avoid unintended extra cycles. Its final manifest was therefore not written; the raw trace and log remain intact.
- Multiversion manifests and reports: [`out/o6-multiversion`](../out/o6-multiversion/); final-frame contact sheet: [`contact.png`](../out/o6-multiversion/previews/contact.png). Captures are raw BGRA at 1920×1080 under each manifest's `capture_dir`.
- The MSYS2 CLANG64 test command `bash tools/run_tests.sh` passed all 48 C/C++ tests and the Python suite. The current HEAD binary hash matches the O3.1 tested build; all three release worktrees built successfully.
- No player save, live cache, configuration, issue status, release or remote branch was changed. Worktrees are detached and remain at `C:\projetos\fork\theorder1886-port-o6-validation-20261022\` for review.

## O7 next action

Use a verified static checkpoint or a player-provided matching save/camera before ranking versions. For the next diagnostic, prioritize GPU-tick wait frames by count (392/598 over-100-ms frames, including 306 where it is the only tracked >50-ms wait); retain pipeline compilation as a separate class because it explains the largest single frame. Add frame-to-submission/semaphore identity to those samples before changing synchronization or compiler behavior. Repeat at 30 FPS/native first; treat the current 60-FPS/FSR pass only as an interaction smoke.

## O7.5 indirect-mode correction — 2026-10-23

`DrawIndirect` previously populated SGPR offsets but left `is_indirect` false.
The existing translator uses that bit to subtract Vulkan BaseVertex/BaseInstance
when the fetch shader does not supply offsets. The caller now explicitly selects
indirect mode. Runtime specialization equality and serialization already include
the bit; required resource/layout specializations and visible-draw waits are intact.
Tests exercise the factory, direct/indirect specialization inequality and the
actual translator prologue (zero versus two base-offset subtractions).
Final CLANG64 `THEORDER_LTO=OFF bash build.sh` and `bash tools/run_tests.sh`
exited 0: 48 C/C++ test targets passed, and Python ran 323 tests with 39 skipped.
The final executable SHA-256 still matches the corrected capture build in the
summary below. Local logs: `out/o7-final-build.log`, `out/o7-final-tests.log`,
`out/test-shader-resource.log`, and `out/test-python.log`; `git diff --check` passed.

Two 125-second runs used copies of `out/o7-fullstage/warm-template-after`, the
same timed inputs, RTX 5060 Laptop/driver 621.1.0.0, CUSA00035 01.02 and the eboot
SHA above. Both output 1920x1080 with a logged 60-Hz vblank; these are correctness
smokes, not the required 30-FPS gameplay benchmark. Port caches were isolated;
driver/OS caches and physical-controller input were not isolated. Both exited 0
with no reported fault or hang. The interrupted earlier true-mode run lasted
28.688 seconds and has no capture; it is excluded.

| Diagnostic run | P50/P95/P99/P99.9 (ms) | Worst (ms) | >50/>100 ms frames | >50/>100 per minute | Mean trace FPS |
| --- | --- | ---: | ---: | --- | ---: |
| Previous false mode | 16.596/17.666/31.880/656.370 | 1598.873 | 41/37 | 20.33/18.34 | 51.75 |
| Corrected true mode | 16.555/17.719/58.082/573.890 | 1044.123 | 70/58 | 34.66/28.72 | 50.97 |

These columns are **not a performance A/B**: the inspected 80-second and
120-second captures show different cinematic camera positions. Both render the
prologue, but neither establishes free movement, combat or draw completeness.
The [false-mode image](evidence/o7-indirect/indirect_false_80s.png) shows the floor;
the [true-mode image](evidence/o7-indirect/indirect_true_80s.png) shows a character's
face. No pixel-diff verdict, image improvement, or fastest-version claim follows.

The corrected run has 6,177 trace intervals, 272 visible pipeline waits (80 over
50 ms), 361.97 ms maximum wait, 19,285.6 ms summed pipeline creation and 3.4 ms
summed queue delay. All 80 long waits originate at `visible_indirect`; 57/58
intervals over 100 ms contain >50 ms compile wait. The previous mode has 104 waits
(61 over 50 ms), 15,481.3 ms creation and 1.5 ms queue delay. These observations
support driver pipeline creation as the measured blocking class; they do not
support queue starvation or prove safe reuse between different full keys.

Peak process private commit/working set was 5570/3951 MiB in false mode and
7491/5565 MiB in true mode; adapter-wide usage was 2649/2737 MiB. This run did not
collect per-process WDDM or VMA peaks. New legitimate indirect specializations
have a compile/memory cost; the scene mismatch prevents attributing these deltas
to the fix alone. Stutters remain, and the candidate is suitable only for
experimental review, with the matched gameplay gate still open.

Sanitized [summary and key-scoped events](evidence/o7-indirect/summary.json)
include executable, trace and log SHA-256, percentiles, memory, and selected
pipeline keys/full stage identities. Complete local artifacts remain in
`out/o7-indirect-visual-reference/` and `out/o7-indirect-visual-resume/`.
The earlier `out/o7-indirect-false.exe` is retained as a reference; no version
has been selected as the best without comparable scene evidence.

Reproduce the corrected smoke with a fresh isolated output root:

```powershell
python tools/batch_runs.py --runs 1 --secs 125 --frame-trace --cusa CUSA00035 --game-version 01.02 `
  --cond indirect-true=THEORDER_PERMUTATION_LOG=1 `
  --pad "24:cross:0.5 28:cross:0.5 32:cross:0.5 40:cross:0.5 50:cross:0.5 60:cross:0.5" `
  --cache-state warm --user-template out/o7-fullstage/warm-template-after `
  --profile-root out/o7-player-retest/profiles --data-root out/o7-player-retest/data `
  --capture-dir out/o7-player-retest/captures --frame-dump-at 40 --frame-dump-count 5 --frame-dump-spacing 20 `
  --manifest out/o7-player-retest/manifest.json --report-dir out/o7-player-retest/reports
```

The profile template is local test data, not distributed with the repository.
For a player retest, substitute a copy of the player's matching save/cache;
never clear the original profile. Expected smoke result: a visible prologue,
no fault/hang, and populated captures/trace. Acceptance of visual correctness or
performance additionally requires the same static scene/camera, 30 FPS/native,
and matching driver/settings before and after. AMD, solar shadows, lighting,
particles and campaign progression remain NOT VALIDATED.

The current [#66](https://github.com/mateussmeneses/theorder1886-port/issues/66)
and its comments were rechecked: the body now says the elevator cutscene was
resolved, while stutter/black objects remain; another reporter prefers v0.7.5
in chapters 1/2. These reports do not establish a regression cause or resolution
by this change. No issue is closed by this checkpoint.
