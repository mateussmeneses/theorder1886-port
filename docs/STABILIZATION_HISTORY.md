# O3 stabilization checkpoint — visible pipeline compile waits

Date: 2026-10-22. This checkpoint completes a first diagnosis and targeted
instrumentation pass for the O3 P0 stutter work. It does not claim a stutter
fix, a gameplay performance gain, or validation of the reported combat issues.

## Scope and changes

- `PipelineCache::WaitGraphics` now emits an opt-in event when it blocks for at
  least 0.1 ms. Each event records the flip, graphics pipeline key hash, stage
  slots 0 and 4 (key and SPIR-V hashes), duration, and completion outcome.
- The pending shader-job wait path records shader hash, hardware stage,
  permutation, flip, and duration under the same opt-in diagnostic.
- `tools/regression_report.py` retains these events in the manifest and reports
  their count, total, maximum, and count over 50 ms. The test suite covers both
  event parsers and report output.
- Diagnostics are enabled only with `THEORDER_PERMUTATION_LOG=1`. No draw,
  cache-key, wait, default setting, or pipeline-selection behavior changed.

## Version identity

| Name | Commit |
| --- | --- |
| `master` | `d5534c570f5de681596d0f4f3963bb915db676be` |
| `windows-v0.6.1` | `bbeeb45133feb1077798dd116f9931cfdd33be3f` |
| `windows-v0.7.5` | `b98248367ff9f80353abe48a97af7eb966f416ab` |
| `windows-v0.7.6-rc.1` | `f02021c2b2c4a1738c03126749145340c8204271` |
| `windows-v0.7.7` | `28064e87adfdcc304ed446a4c0b9ca0199596a3d` |

The first three rows were corrected on 2026-10-23 using `git rev-parse 'TAG^{}'`:
the earlier checkpoint listed annotated tag objects instead of commit IDs.
There is no `windows-v0.7.6` final tag in this checkout. These hashes confirm
the versions present; no regression is attributed to a release because no
equivalent-version gameplay comparison was run.

## Runtime evidence

The final instrumented run is `out/o3-checkpoint.json` on the NVIDIA RTX 5060
Laptop GPU, driver 621.1.0.0, CUSA00035 01.02, eboot SHA-256
`f742eb1793f92772a5c4bdd174e29b08e0b53f68d373a37f4cda730703e558d8`.
The runtime exited 0 without a reported guest fault. It used the same copied
prologue save and scripted Cross inputs as the preceding diagnostic runs.
The source profile contained the save but no reusable port `cache` or `shader`
directories, so the run is declared **cold port cache**. OS and driver cache
state were not controlled. The final display dump at 110 seconds is black; this
capture does not independently verify the visible scene. It is prologue-route
diagnostic evidence, not a free-movement or combat validation.

Raw frame trace:
`out/o3-checkpoint-data/o3-01-20261022T211313096809Z/frames.csv`.
Runtime log:
`out/batch/o3-01-20261022T211313096809Z.log`.
Per-frame evidence for all 121 intervals over 50 ms, joined to visible pipeline
wait events on the preceding flip, is in
`out/o3-checkpoint-data/o3-01-20261022T211313096809Z/stall-frames-over-50ms.csv`.
It includes trace position, key/stage/SPIR-V hashes, wait counters, translation
and driver worker timing, upload bytes, and readback/tick/recorder/host-copy
waits. The runtime did not collect callstacks, page faults, file I/O, swapchain
waits, or per-thread scheduling, so those classes remain unclassified.

| Metric | Result |
| --- | ---: |
| Frame intervals | 4,761 |
| P50 / P95 / P99 / P99.9 | 16.69 / 28.93 / 367.79 / 1,238.70 ms |
| Worst interval | 1,813.61 ms |
| Frames over 50 / 100 ms | 121 / 82 |
| Stutters per minute (`>1.5 × trace median`) | 132.71 |
| Sampled process CPU median | 22.37% |
| Adapter memory max / Vulkan allocations max | 2,607 / 2,118 MiB |
| Process committed memory max / working set max | 5,765 / 4,116 MiB |

### Confirmed stall class: visible graphics pipeline compilation

The run recorded 375 key-scoped `WaitGraphics` events totaling 37.37 seconds;
the longest single wait was 680.55 ms. The frame CSV recorded 37.07 seconds of
`compile_wait_ns`. The small difference includes sub-threshold waits and
capture-boundary timing. Five shader-job waits accounted for only 1.64 ms.
This identifies visible graphics-pipeline creation as the dominant measured
compile-wait class in this run. Of 82 intervals over 100 ms, 79 map to a
`WaitGraphics` event on the preceding flip; 103 of 121 intervals over 50 ms do
as well. The trace records the interval ending at the next flip, hence the
one-flip offset.

The worst interval (frame 2434, 1,813.61 ms) recorded 1,128.78 ms in the
compile-wait counter; 23 visible pipeline waits on the preceding flip account
for 1,128.79 ms, including eight individual waits over 50 ms. A representative
event elsewhere in the trace includes a full pipeline-key hash plus stage and
SPIR-V hashes: key `05550bf28547a039`, stage 0 `00000a6e5887a4a5`, SPIR-V
`64db5279fa06c801`, wait 680.552 ms. Stage numbers are reported as slots to
avoid relying on a renderer comment whose stage ordering differs from
`SwStage`.

The run also recorded 163 binary hits, 462 compiled pipelines, zero rejects,
and zero cache errors. Those are aggregate counters: they do not show that any
of the 375 waited keys was a hit. At startup, observed queued waits were logged
with `stored false` and `fresh permutation true`, so those keys had no reusable
port binary and were legitimately built. There were 375 distinct waited
pipeline-key hashes but 144 distinct stage-0 SPIR-V hashes. Repeated SPIR-V
content can still require distinct Vulkan pipelines; the data does not identify
which graphics-state fields vary, nor prove those keys can safely be merged. Prior
`identity-content-analysis.json` separately shows that compute shader
`0x804aeead` mostly produces distinct, resource-specialized SPIR-V; its image
fallback masks, layouts, and specializations remain required parts of identity.

Other observed contributors do not explain most severe frames: none of the 82
frames over 100 ms had a readback wait or a GPU-tick wait above 50 ms. Eight of
121 frames over 50 ms had some readback wait; one had a GPU-tick wait above 50
ms. Some large intervals also upload substantial data, so streaming and
unmeasured host scheduling remain candidates for the unmatched frames. The
`driver_ns` counter sums concurrent worker work and is not exclusive frame
latency or CPU time.

## Controlled-comparison status

The manifests and raw traces from the earlier `extended.json` run are no longer
present at their recorded `out/batch/runtime-data` paths, so that historical
trace could not be independently re-correlated here. Fresh runs reproduced
large compile waits and supplied event-level cause data, but no paired build
with and without a functional optimization was run. The final run is a
diagnostic, not a speedup or regression A/B. The copies all used the same source
save and input script; gameplay scene progression and the 110-second display
capture were not verified as equivalent.

No rendering behavior changed, so there is no image-completeness or draw-loss
claim. Visible waits and essential draws remain intact. The prior measured
results in `PERFORMANCE_BASELINE.md` remain the best recorded evidence; no
configuration default or version selection was changed.

## Reference review

Reviewed the pinned local copies of `HayatoG/iss_pc` (`cde38afd`),
`Deft96/order1886_pc` (`af2f9936`), shadPS4 (`8d50471b`), `gow3_pc`
(`7180a7ba`), and `bloodborne_pc` (`fce08780`). `iss_pc`'s bounded priority
compiler queue, captured-job precompile profiles, ordered bounded readback
queue, and `parallel_gpu.md` scheduling constraints are useful comparison
points. The Order already has asynchronous compilation, cache serialization,
deferred readbacks, and prepared draws; no safe queue-policy change was
justified by this single cold-port-cache trace. The other renderer forks
confirmed related cache/async patterns but did not provide a demonstrated fix
for this workload. Their relevant code is GPL-2.0 or GPL-2.0-or-later, as is
the corresponding project code; **no reference code was copied**.

## Build and tests

- MSYS2 CLANG64 `bash build.sh`: passed; final executable SHA-256
  `61eea8fae9ba8236cc7262698683e1baea0698014005dee4a272c1653befdfae`.
- `bash tools/run_tests.sh`: all 48 C/C++ tests and the Python suite passed.
- After the final report additions, `python -m unittest test_regression_harness`
  passed all 30 tests.
- The new parser test was run red before its implementation and green after.
- Runtime capture: exit 0, expected CUSA/version matched, no reported guest
  fault. This does not establish image correctness or combat behavior.

## O3.1 plan from the initial checkpoint

1. Preserve a warm profile and exact save/checkpoint, then replay identical
   movement/combat input on the same build, driver, resolution, and options.
2. Compare full graphics-key state for repeated SPIR-V hashes and determine
   which differences are legitimate; test only exact, reconstructible warmup
   candidates without relaxing visible-draw waits.
3. Classify the remaining severe frames with no matching pipeline wait,
   including upload/streaming, page faults, presentation, and per-thread CPU/GPU
   scheduling. Keep the visual draw-completeness review with any A/B.
4. Only change the default after a controlled before/after run proves image
   completeness and a repeatable frametime improvement.

## O3.1 diagnosis: buffer write-fault waits and frame submission

The O3.1 diagnostic pass adds three per-frame timings to `frames.csv`: guest
`SubmitFlip` inter-arrival, time from submission to the VideoOut request queue,
and time spent in that queue before the present thread processes it. Each row
also records `renderer_flip`, allowing graphics pipeline events to join to the
frame that produced them. `regression_report.py` now summarizes the three
timings and preserves `THEORDER_PERF_DIAG` wait profiles by source file and
line. These additions do not change presentation pacing, draw policy, or cache
behavior.

The exact-correlation run is `out/o3-1-correlated.json`, run
`o3-1-correlated-01-20261022T223021829041Z`, using CUSA00035 01.02 and the same
eboot SHA as the initial checkpoint. It replayed the same prologue save and
Cross input from a warm copy of the port cache. Exit was 0 with no guest faults;
OS and driver cache states remain uncontrolled. Per-frame details for all 426
intervals over 50 ms, including matching pipeline keys and SPIR-V hashes, are
in `out/o3-1-correlated-data/o3-1-correlated-01-20261022T223021829041Z/stall-frames-over-50ms.csv`.

| Metric | O3.1 diagnostic |
| --- | ---: |
| Frame intervals | 3,741 |
| P50 / P95 / P99 / P99.9 | 26.17 / 65.29 / 98.08 / 1,168.44 ms |
| Worst interval | 1,904.57 ms |
| Frames over 50 / 100 ms | 426 / 37 |
| Stutters/min (`>1.5 x trace median`) | 399.75 |
| Adapter / Vulkan allocations max | 2,363 / 1,927 MiB |
| Process commit / working set max | 6,751 / 5,592 MiB |

The trace recorded 83 visible pipeline waits (13.80 s total, 653.63 ms max).
Of 37 intervals over 100 ms, 22 coincide with a visible pipeline wait on the
preceding renderer flip, 18 contain over 50 ms of `compile_wait_ns`, 15 contain
over 50 ms of GPU-tick wait, and 3 contain a readback wait. This is a warm-cache
diagnostic, not a before/after performance comparison. Its cache counters were
545 binary hits and 82 compiles; they do not imply every wait key was reusable.

### Confirmed next stall path: synchronous buffer download after guest writes

`THEORDER_PERF_DIAG=1` records `Scheduler::Wait` callsites. Across the 130-second
correlated run, `buffer_cache.cpp:516` (`scheduler.Finish()` before a synchronous
buffer download) accumulated 3,206 calls and 67.19 seconds of semaphore wait,
with a 106.71 ms maximum call. Calls can overlap, so this sum is not exclusive
frame latency. `texture_cache.cpp:208` added 5.52 seconds across 375 waits. The
report shows these callsites; the raw log contains the per-window details.

The logged path is guest write fault -> `Rasterizer::OnWriteFault` -> buffer
invalidation/readback -> synchronous `BufferCache::DownloadMemory` ->
`Scheduler::Finish`. A severe frame had 1,124.14 ms total, no synchronous
compile-wait counter, 10.80 ms GPU-tick wait, 59.0 ms write-fault handling,
178.8 MiB image uploads, 86.5 MiB buffer uploads, and 87.5 ms image refresh.
The same interval's `submit_to_queue` and VideoOut queue wait were below 1 ms
and 0.1 ms respectively; this rules out those queues as its main delay, but
does not identify the unmeasured guest/host work in the remaining time.

The dominant logged buffer region was `0x105b500000`, usually 16–38 write
downloads/s from guest RIP `0x0000000800CC8791`. One cumulative log sample
counted 3,072 write-fault downloads beside GPU-written bytes and zero directly
on them. Those writes share pages with GPU-written ranges. The current
readback preserves neighboring guest-visible data; an earlier byte-precise
attempt is documented as losing ring-buffer data and hanging. Therefore no
asynchronous or byte-range bypass is justified yet. This is a measured stall
class, not a demonstrated safe fix.

VideoOut queue timing did not explain the longest frames: in the correlated run
P95/max were 1.60/36.91 ms from submit to queue and 4.70/15.13 ms in the
VideoOut queue. `guest_submit_gap` tracks the frame interval, so it marks when
the next frame was submitted rather than identifying why guest work took that
long. Per-thread guest CPU scheduling and symbolized stacks remain unavailable.

No gameplay A/B or visual-completeness comparison was made. The 110-second
capture is not combat validation. No defaults or cache identities changed.

## O3.2 next

1. Replay the same prologue save and then a verified combat checkpoint with the
   per-frame `renderer_flip`, write-fault, and semaphore callsite diagnostics.
2. Attribute the `0x105b500000` writes to guest structures and determine which
   bytes within the protected pages are actually GPU-modified before considering
   a narrower readback. Preserve the ring-buffer ordering constraint.
3. Isolate the 1.1-second upload/refresh frame from async compilation and
   guest-side work; collect per-thread scheduling or stacks for the unmatched
   time. Keep VideoOut queue timing in the trace.
4. Compare any candidate change against the same warm profile, driver, input,
   resolution, and verified scene. Require image and draw completeness before
   changing defaults or claiming a stutter reduction.

## O3.1 validation and O4 shadow checkpoint — 2026-10-22

- HEAD: `d5534c570f5de681596d0f4f3963bb915db676be`; working tree remains dirty.
- Fresh MSYS2 CLANG64 `bash build.sh` passed; `out/theorder-probe.exe` SHA-256 is `48A920FC74A57B0A1039AE8154C94620120C39423C0E721C9978A9AC7BF0928D`.
- `bash tools/run_tests.sh` passed all 48 C/C++ tests and the Python suite; `git diff --check` passed.
- O3.1 adds per-frame VideoOut timings and wait-callsite reporting; the measured synchronous readback path remains unmodified because adjacent guest-visible bytes share pages with GPU-written ranges.
- No O3 before/after performance or combat-image A/B exists; do not claim stutter reduction.
- Current public #26 page shows RTX 4050 / driver 617.42 and an Oct 20 log attachment; the supplied `last_run_sanitized.log` and `report.md` are absent locally, and the attachment download was inaccessible.
- Existing `out/regression/issue-26-htile.json` records 80-second cold-cache HTILE on/off runs, each 2,160 flips, but its rooftop checkpoint was unverified and its capture/log files are missing.
- That manifest cannot prove shadow pixels changed; no producer-to-consumer pass/resource chain or deterministic faulty draw is available.
- Existing `THEORDER_LIGHTING_TEST` (`shading`, `msdepth`, `dref`, `bias`) is diagnostic; `THEORDER_EVSM_HTILE=0` disables the current EVSM HTILE workaround. No toggle was promoted or changed.
- Fork comparison: Deft96 at `af2f993` retains EVSM-dispatch HTILE filling; IgorAugustoHH at `f8c7c9b` also fills HTILE when binding depth targets. This is a code-path hypothesis, not proof that #26 reaches that pass or that the broader fill is safe.
- O4 result: NOT VALIDATED on the reporting RTX 4050; #26 remains open. No graphics fix or default change is justified by available evidence.
- Player retest: at one reproducible outdoor checkpoint, fixed 30 FPS / 1080p / aspect / upscaler off, use `tools/batch_runs.py --cond htile_on=THEORDER_EVSM_HTILE=1 --cond htile_off=THEORDER_EVSM_HTILE=0 --cache-state unknown --user-template <copied-profile> --profile-root <new-isolated-root> --frame-trace --present-frames 3 --capture-at <same-time> --frame-capture-at <same-time> --validation`, with the same `--pad` input and settings for both conditions.
- Attach `last_run.log`, both batch manifests/reports/captures, GPU/driver, CUSA/build, checkpoint and camera/input details; expected evidence is the same scene and shadow pixels plus validation output, not merely successful flips.
- Next: obtain that paired RTX 4050 capture (and sanitized issue attachments), then map the EVSM producer, HTILE range, barriers and lighting consumer before selecting one diagnostic or correction.

## O5 lighting/output checkpoint — 2026-10-22

- HEAD remains `d5534c570f5de681596d0f4f3963bb915db676be`; no rendering code or default changed.
- Reviewed current #36/#39/#63 logs locally; removed the temporary downloads after extracting only the facts recorded in `docs/GRAPHICS_REGRESSIONS.md`.
- #36 is RX 6700 XT, v0.7.5, CUSA00035, 1080p with FSR 3.1 on; the reported lighting defect has no scene/checkpoint and remains unlocalized.
- #39 is RX 9070 / Adrenalin 26.8.1, v0.7.5, Chapter 5, CUSA00076, 1080p with FSR on; colored flares remain unlocalized.
- #63 is the same reported RX 9070/CUSA/eBoot after v0.7.7, Chapters 8/9, 1440p with FSR off; reporter confirms artifacts and stutters persist.
- #36/#39/#63 logs contain 65/171/177 stall records; 28/80/137 exceed 100 ms, with maxima 761.5/1,048.9/7,759.1 ms.
- #63 worst logged stall has low GPU-tick and host-copy waits; concurrent summed driver-compile time is not frame attribution. No VSM shader hash, pass capture, or Vulkan validation error identifies the colored artifacts.
- #45 remains NVIDIA-only evidence (RTX 4080 Laptop; washed character/crushed environment); no scene, usable runtime log, exposure/histogram capture, HDR/sRGB state or output mode is recorded.
- #43 (RTX 3060 Ti) and #47 (RX 6500 XT, driver 26.10.1) remain separate black/white-output reports; neither has a usable first-frame log/capture, and #47 has no result to the v0.7.7 retest request.
- Problems confirmed fixed in O5: none. NVIDIA #45/#43 and AMD #36/#39/#47/#63 remain unconfirmed; no workaround or issue was closed.
- No comparable before/after images or hardware reproduction were available; current main-menu capture does not validate any affected scene.
- Next: request one fixed-scene baseline and one-pass capture per reporter GPU, including CUSA/eBoot, driver, resolution, aspect, HDR/sRGB, upscaler and first affected frame; do not combine these symptoms.

## O6 multiversion and soak checkpoint — 2026-10-22

- HEAD: `d5534c570f5de681596d0f4f3963bb915db676be`; existing O3.1 executable SHA-256 `48A920FC74A57B0A1039AE8154C94620120C39423C0E721C9978A9AC7BF0928D`; local changes and saves preserved.
- Built detached worktrees for `windows-v0.6.1`, `windows-v0.7.5`, and `windows-v0.7.7`; hashes and commits are in `docs/STABILIZATION_VALIDATION.md`.
- MSYS2 CLANG64 `bash tools/run_tests.sh`: all 48 C/C++ tests and the Python suite passed.
- Isolated warm HEAD soak completed 43,954 frames over 1796.8 trace seconds with zero guest faults/hangs; worst interval 2729.8 ms, P99.9 734.0 ms, 6136 frames >50 ms and 598 >100 ms.
- In those >100 ms frames, 392 had GPU-tick wait >50 ms and 199 had compile wait >50 ms; the 2.73 s worst frame includes 2.62 s compile wait, while a 2.27 s upload-heavy frame has no compile wait.
- Peak private commit/Working Set/VMA/WDDM dedicated/WDDM shared/adapter usage were 7749/6443/2305/2872/1397/2880 MiB; startup growth slows near the end, but VMA still rises, so no leak conclusion.
- Built 30 FPS native cold/warm samples for all four versions from copied saves and per-version isolated profiles; all CUSA/eboot checks matched and no guest fault/hang was reported.
- Captured final frames differ between cold/warm and versions; timed QTE inputs did not prove a matching final position, so no performance ranking or claim that v0.6.1 is smoother.
- HEAD FSR 3.1 / requested 60 FPS smoke showed FSR on and 60 Hz vblank, but measured median was 30 FPS; no sustained 60 FPS result.
- O6 result: validation matrix written; actual combat, 60 FPS, 120 FPS, Frame Generation, AMD/NVIDIA issue scenes and visual equivalence remain unvalidated.
- Next O7: obtain a verified static checkpoint; then instrument GPU-tick waits (largest observed >100 ms class) with frame-to-submission/semaphore identity, keeping pipeline-compile stalls separate.
- Evidence: `docs/STABILIZATION_VALIDATION.md`, `out/o6-multiversion/`, `out/o6-head-soak-data/head-warm-01-20261022T224613945867Z/frames.csv`, and `out/batch/head-warm-01-20261022T224613945867Z.log`.

## O7 GPU-wait attribution — 2026-10-22

- HEAD: `d5534c570f5de681596d0f4f3963bb915db676be`; working branch `codex/o7-gpu-wait-diagnostics`; prior local edits retained.
- Added per-frame longest `Scheduler::Wait` callsite, line and target timeline tick to `--frame-trace`; synchronization behavior is unchanged and the existing `THEORDER_PERF_DIAG=1` gate remains.
- Added CSV parsing/reporting by wait site and a C++ accumulator reset test; both targeted tests failed before implementation and passed afterward.
- MSYS2 CLANG64 `bash tools/run_tests.sh`: all 48 C/C++ tests and Python tests passed; `THEORDER_LTO=OFF bash build.sh` succeeded; `git diff --check` clean.
- Isolated warm 40-second prologue smoke: CUSA00035/01.02 and eboot SHA matched, 0 guest faults/hangs, 1,037 trace intervals; this does not validate free movement or combat.
- Smoke trace: worst interval 645.8 ms, mean 29.51 FPS, 6.83 stutters/min; longest reported GPU waits were 57.8 ms at `vk_temporal_upscaler.cpp:316` and 22.9 ms at `buffer_cache.cpp:516`.
- Wait-profile sum for `buffer_cache.cpp:516` was 4,872.6 ms across 1,037 calls; these call durations may overlap and do not alone prove frame-thread causality.
- First concrete O7 lead: BufferCache waits recur frequently, but only per-frame attribution plus matching GPU timeline evidence can establish whether they cause the severe frame outliers.
- No speedup or graphics correctness claim; O4 solar shadows and O5 lighting/artifact issues remain unvalidated and open.
- Next O7.1: correlate `buffer_cache.cpp:516` target ticks with frame outliers and upload/readback volumes in a verified repeatable checkpoint; then isolate a single wait class before changing scheduling.
- Evidence: `out/o7-smoke/reports/report.md`, `out/o7-smoke/manifest.json`, and the referenced `frames.csv`.

## O7.1 pipeline wait attribution — 2026-10-22

- HEAD `d5534c570f5de681596d0f4f3963bb915db676be`, branch `codex/o7-gpu-wait-diagnostics`; current executable SHA-256 `10F0AC110C32B1632F33A02AC3AE10505E32FF51AEDDB945D6D9B742AF483648`.
- Frame traces plus `THEORDER_PERMUTATION_LOG=1` isolated the main repeated outlier class to `WaitGraphics`; queue delay and Vulkan pipeline creation are now reported separately per key.
- In `o7-split`, 14/16 frames >100 ms had compile wait >50 ms; none had a per-frame GPU-tick wait event >50 ms.
- The same capture logged 61 visible graphics pipeline waits, 21 >50 ms; summed wait 4.82 s, max 296.64 ms, queue time 0.8 ms, pipeline creation 4.83 s.
- Example: trace frame 1278 waited 357.8 ms for compilation; flip 1277's three pipeline waits sum to 357.8 ms. These builds logged `stored false`, `fresh permutation true`.
- This establishes driver pipeline creation on new pipeline keys as a cause of this stall class; compiler queue starvation is not supported by this capture. It does not explain every long frame.
- Repeating with the first run's isolated port cache still showed 13/13 frames >100 ms with compile wait >50 ms; none had GPU-tick wait >50 ms.
- The two captures shared zero high-wait pipeline keys and did not prove an identical final QTE position, so neither cache reuse failure nor a version ranking is claimed.
- Added queue-versus-create timing and parser output; the parser test failed before the change and passed after it.
- `THEORDER_LTO=OFF bash build.sh`, then `bash tools/run_tests.sh` in MSYS2 CLANG64: build succeeded and all 48 C/C++ tests plus Python tests passed; `git diff --check` clean.
- Evidence: `out/o7-split/reports/report.md`, `out/o7-cache-reuse/reports/report.md`, corresponding manifests, traces and `out/batch/o7-split-01-20261023T001227017808Z.log`.
- No performance correction or default change made; next: establish an identical checkpoint/position, determine why its visible pipeline keys are newly encountered, then test the matching warm-cache behavior before proposing prewarm or cache changes.

## O7.2 full shader identity — 2026-10-22

- HEAD remains `d5534c570f5de681596d0f4f3963bb915db676be` on `codex/o7-gpu-wait-diagnostics`; executable SHA-256 `045DB57E969A67BB5934128CDDE99031A99D2E39F956F82BA5CEEEE39D9EF197`.
- Corrected wait logs to include all six stage hashes and all six SPIR-V hashes; earlier `stage0`/`stage4` fields omitted the vertex stage at `SwStage::Vertex == 3`.
- Parser tests cover the new arrays and older log formats; red before parser update, green after. Full CLANG64 build and all 48 C/C++ plus Python tests passed; `git diff --check` clean.
- Full-stage prologue trace: 3,287 intervals, 20 frames >100 ms, 18 with compile wait >50 ms, zero with a GPU-tick wait event >50 ms.
- It logged 71 visible pipeline waits, 26 >50 ms; total pipeline creation 6.15 s, maximum 354.65 ms, queue time 0.8 ms.
- All 26 long events used fragment (stage 0) and vertex (stage 3); each had a distinct complete stage/SPIR-V identity. This supports distinct shader specializations, not a safe key merge.
- A follow-up from that run's copied port cache still had 14 visible waits >50 ms, but none of the first run's 26 high-wait keys recurred as a high wait; the final QTE position was not equivalent.
- That follow-up also showed severe waits at `buffer_cache.cpp:516` (max 283 ms), confirming the prologue can move between stall classes; do not compare its P95 or stutter rate as an A/B result.
- The preparer currently has an explicit invariant that its workers never create/compile pipelines. Moving enqueue earlier may hide driver creation, but requires a safe ownership/publication seam and a stable test checkpoint.
- No scheduling, draw, key, or default behavior changed; no performance reduction is claimed.
- Evidence: `out/o7-fullstage/reports/report.md`, `out/o7-fullstage-reuse/reports/report.md`, manifests/traces and `out/batch/o7-fullstage-01-20261023T002556850617Z.log`.
- Next: obtain a deterministic scene position or manual capture for one repeated key; test prequeue only after proving the prepared shader/resource snapshots remain valid through asynchronous publication.

## O7.3 preparation lead measurement — 2026-10-22

- HEAD `d5534c570f5de681596d0f4f3963bb915db676be`, branch `codex/o7-gpu-wait-diagnostics`; executable SHA-256 `23267D7E7A170CFB1F0804E86172DA1309F45237CB4CA330F4D428338158DB76`.
- Added diagnostic-only `PreparedDraw.ready_at` under `THEORDER_PERMUTATION_LOG`; wait events can report preparation age, with `-1` meaning no matching timestamp.
- First smoke exposed that long waits also arrive through `Rasterizer::ResolvePendingGraphics`; propagated the prepared draw into this wait path.
- CLANG64 `THEORDER_LTO=OFF bash build.sh` passed; `bash tools/run_tests.sh` passed all 48 C/C++ tests and Python tests; parser covers positive, unavailable and legacy preparation fields.
- Isolated warm-profile prologue run: 6,451 intervals, 32/80 visible pipeline waits over 50 ms, max 312.00 ms; queue 1.1 ms total and pipeline creation 8,097.2 ms total.
- The run had no matching prepared timestamp on any visible pipeline wait (`prep -1.000 ms`); no preparation lead estimate or overlap claim is supported.
- This is instrumentation only; no pipeline scheduling, key identity, draw completeness, visual output, or defaults changed. Combat remains unvalidated.
- Evidence: `out/o7-prep-measure2/reports/report.md`, `manifest.json`, `frames.csv`, and `out/batch/prep-01-20261023T004448319388Z.log`.
- Next: instrument the reason timestamp association is absent (no prepared draw versus key mismatch) on one stable checkpoint; only pursue earlier prequeue if a matched lead window is measured.

## O7.4 indirect-draw wait origin — 2026-10-22

- HEAD `d5534c570f5de681596d0f4f3963bb915db676be`, branch `codex/o7-gpu-wait-diagnostics`; executable SHA-256 `D150B1101402F6276F5A576BA4281515333ADC611E82872D40D7915CE179073B`.
- Pipeline wait logs now identify the caller (`small_target_sync`, `visible_direct`, `visible_indirect`) and preparation association (`no_draw`, `unavailable`, `not_ready`, `key_mismatch`, `no_timestamp`, `matched`).
- CLANG64 build passed; all 48 C/C++ tests and Python tests passed; parser red/green covers old and new log formats.
- Isolated prologue run on RTX 5060 Laptop, CUSA00035 01.02: 87 waits, 37 over 50 ms, max 503.23 ms; pipeline creation 9,354 ms total versus 1.1 ms queue delay.
- All 37 waits over 50 ms came from `visible_indirect`; all had `no_draw`. No key mismatch was observed. The direct prepared-draw path is not the dominant stall route in this capture.
- Code inspection confirms `DrawPreparation` enumerates only three direct-draw PM4 opcodes; six indirect variants bypass it and call pipeline selection on the GPU thread.
- The indirect caller also omits `DrawIndirectParams::is_indirect`; `BuildRuntimeInfo` forwards that bit and the vertex translator uses it for BaseVertex/BaseInstance handling. This is a correctness lead, not yet runtime-validated or changed.
- This confirms the blocked visible indirect draws wait for Vulkan pipeline creation; it does not prove all campaign stalls share this route or establish a performance gain.
- No draw, key, scheduling, or default behavior changed. Combat and cross-version comparisons remain unvalidated.
- Evidence: `out/o7-wait-source/reports/report.md`, `manifest.json`, `frames.csv`, and `out/batch/prep-01-20261023T005338619890Z.log`.
- Next: validate/fix the indirect-mode specialization first, then design a bounded prequeue that retains each PM4's SGPR offsets and shader snapshot; avoid duplicate jobs and preserve correct fallback when preparation is stale.

## O7.5 indirect vertex-ID specialization — 2026-10-22

- HEAD `d5534c570f5de681596d0f4f3963bb915db676be`, branch `codex/o7-gpu-wait-diagnostics`; executable SHA-256 `F9857BCB4ED48FF8AD2368A6FE882296B177CEBA1F68C2804B1328AC74CBC6EA`.
- Confirmed the indirect bit is part of `RuntimeInfo` equality, hence `StageSpecialization`; the vertex translator uses it to correct `VertexId`/`InstanceId` for indirect commands without matching fetch offsets.
- Added `DrawIndirectParams::Indirect()` and set `is_indirect=true` at the indirect rasterizer call. Unit test first failed when the factory returned false, then passed; it checks offsets, the flag, and distinct direct/indirect stage specializations.
- CLANG64 `THEORDER_LTO=OFF bash build.sh` and `bash tools/run_tests.sh` passed; all 48 C/C++ tests and the Python suite passed.
- Isolated RTX 5060 prologue smoke passed with zero faults/hangs; no visual capture was reviewed, so rendered correctness remains unvalidated.
- First run with the corrected specialization: 5,390 intervals; 265 visible pipeline waits, 127 over 50 ms, max 335.19 ms, pipeline creation 29,370 ms. This is a substantial first-run compile cost, not a stutter improvement.
- Warm-profile follow-up: 97 waits, 55 over 50 ms, max 324.49 ms, pipeline creation 13,900 ms. The QTE position differed, so this is not a paired performance result.
- Across those runs, 20 full SPIR-V stage identities overlapped but zero pipeline keys did; high waits in the warm run used identities absent from the first. Do not merge by SPIR-V hash alone or claim cache success.
- Evidence: `out/o7-indirect-mode/reports/report.md`, `out/o7-indirect-mode-warm/reports/report.md`, their manifests/traces, and `out/batch/indirect-01-20261023T010423757262Z.log` / `indirect-warm-01-20261023T010655275735Z.log`.
- Next: obtain a matched indirect scene/camera and image capture; then identify why full SPIR-V identities can recur with different pipeline keys and design bounded prequeueing that keeps descriptor/layout/resource specializations intact.
