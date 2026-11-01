# Performance baseline

## 2026-11-01: Controlled Cold, Warm, and Revisit A/B Benchmarks

Host: Windows 11 (10.0.26300), Intel Family 6 Model 183, NVIDIA GeForce RTX 5060 Laptop GPU,
driver NVIDIA 621.1.0.0, Vulkan 1.4.357. Game: CUSA00035 01.02.
Executable SHA-256: `e621858cd47e0567a055b9991579d55a7817e5b857dcb778eb8f19e574361114`.
All runs are 60 seconds in the 3D title scene using `tools/batch_runs.py --frame-trace` with isolated
scratch profile directories (`--user-template user`). No visible draws or essential compute dispatches
were skipped (`THEORDER_VISIBLE_PIPELINE_PRIORITY=1` preserved in all runs).

Manifests:
- Cold (1st Visit): `out/batch/manifests/bench_cold_comparison.json`
- Warm (Full 516-pipeline profile): `out/batch/manifests/bench_warm_comparison.json`
- Revisit (2nd Visit seeded from 1st Visit output profile): `out/batch/manifests/bench_revisit_base.json`, `out/batch/manifests/bench_revisit_opt.json`

| Metric | `base_cold` (1st Visit) | `opt_cold` (1st Visit) | `base_warm` (Warm Cache) | `opt_warm` (Warm Cache) | `base_revisit` (2nd Visit) | `opt_revisit` (2nd Visit) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Logged flips (60 s) | 3,480 | 3,480 | 3,420 | 3,420 | 3,420 | 3,420 |
| Mean FPS (whole trace) | 59.21 | 59.10 | 58.53 | 58.63 | 58.46 | **58.99** |
| Mean FPS (post-boot steady) | 59.34 | 59.23 | 59.70 | 59.71 | 59.39 | **59.81** |
| Frame P95 (ms) | 17.78 | **17.75** | 17.78 | **17.70** | 17.75 | 17.83 |
| Frame P99 (ms) | 18.58 | **18.35** | 18.35 | **18.08** | 18.39 | 18.42 |
| Frame P99.9 (ms, whole trace) | 81.79 | **80.99** | 54.71 | **38.46 (-29.7%)** | 41.28 | **32.76 (-20.6%)** |
| Frame P99.9 (ms, post-boot) | 68.86 | **54.62 (-20.7%)** | 32.03 | 32.68 | 32.56 | 32.63 |
| Worst frame (ms, whole trace) | 220.63 | 250.73 | 1,049.74 | **995.18 (-5.2%)** | 849.90 | **752.85 (-11.4%)** |
| Worst frame (ms, post-boot) | 220.63 | 250.73 | 107.61 | **80.44 (-25.2%)** | 405.13 | **34.06 (-91.6%)** |
| Stalls > 50 / > 100 / > 200 ms (all) | 6 / 2 / 1 | 5 / 3 / 2 | 4 / 2 / 1 | **3 / 1 / 1** | 2 / 2 / 2 | **1 / 1 / 1** |
| Stalls > 50 / > 100 / > 200 ms (post-boot) | 5 / 1 / 1 | **4** / 2 / 2 | 2 / 1 / 0 | **2 / 0 / 0** | 1 / 1 / 1 | **0 / 0 / 0** |
| GPU-thread compiles | 183 | **100 (-45.4%)** | 109 | **29 (-73.4%)** | 74 | **25 (-66.2%)** |
| GPU-thread compile time (ms) | 808.0 | **493.7 (-38.9%)** | 881.3 | **362.1 (-58.9%)** | 631.4 | **315.5 (-50.0%)** |
| GPU-thread sync wait (ms) | 222.0 | **41.3 (-81.4%)** | 40.5 | **16.3 (-59.8%)** | 166.7 | **57.2 (-65.7%)** |
| Background worker time (ms) | 11,216.8 | **637.0 (-94.3%)** | 10,902.0 | **66.4 (-99.4%)** | 7,662.4 | **103.5 (-98.6%)** |
| Driver compile time total (ms) | 10,723.4 | **484.2 (-95.5%)** | 10,130.6 | **158.1 (-98.4%)** | 7,234.4 | **0.0 (-100.0%)** |
| Binary cache hits / misses (compiled) | 8 / 169 | 6 / **109 (-35.5%)** | 491 / 65 | 475 / **1 (-98.5%)** | 144 / 60 | 115 / **0 (-100.0%)** |
| Binary cache rejected / errors | 0 / 0 | 0 / 0 | 3 / 0 | **0 / 0** | 0 / 0 | **0 / 0** |
| Compute fallback dispatches | 86 | **0 (-100%)** | 76 | **0 (-100%)** | 47 | **0 (-100%)** |

## 2026-10-27: retire the legacy timed runner

The root-level `test_timed.py` was removed from the tracked tree. It used
a developer-specific game path and deleted files from the shared `out/dump`
directory on startup. It also sampled processes by executable name (rather
than the process it launched), measured total NVIDIA VRAM usage instead of
the game's process VRAM, and wrote logs and pad files in the working directory.
This made its measurements easy to misattribute and could destroy existing
frame captures.

The supported replacement is `tools/batch_runs.py`. It creates a unique
per-run data directory, can use independent user profiles, records the selected
executable hash and scene identity, and provides reproducible manifests.

For an isolated, non-destructive sample on Windows:

```powershell
python tools/batch_runs.py --runs 5 --secs 120 --cond base= --scene 'fixed checkpoint' --game-dir 'D:\CUSA00035' --profile-root 'D:\triage\profiles' --data-root 'D:\triage\runs' --cache-state cold --frame-trace
```

Use a copied `--user-template` for a checkpoint needing save data. The
runner does not clear your real saves, caches or captures. Never use the
legacy timed script from an old checkout if preserving the out/dump captures
matters. A passing timed run is not visual or GPU correctness evidence.

## 2026-10-25: copied current checkpoint, subtitle-diagnosis trace

`out/20261025-subtitle-current/` records one isolated 130-second run using a
copy of the packaged user profile and port caches. Game/driver identity matches
the earlier local RTX 5060 run. The script reached the current Tesla-lab
checkpoint, not the prologue; the captured guest and output buffers are
1920x1080. Launcher-only display choices were not fully reproduced, so this is
not an equivalent comparison with the user's 4K gameplay or evidence of a
regression between builds.

The CSV contains 9,029 captured guest intervals, 72.12 mean FPS and raw
P95/P99 of 21.05/31.04 ms. The largest frame reported by the runtime windows
was 911.70 ms. The run exited 0 without reported guest faults or VUIDs. The
whole-trace threshold count is not interchangeable with the old short-window
metric; a near-31 ms P99 does not exclude the much larger outliers. There is no
matched before/after pair or subtitle fix here. Original saves/caches were not
used as writable runtime directories, and no compilation default changed.

## 2026-10-25: user-tested local package, uncontrolled run

The user reports the game is very playable, with minor flicker and stutters.
The package log identifies CUSA00035 01.02, RTX 5060 Laptop/NVIDIA 621.1.0.0,
Vulkan 1.4.357, borderless 3840x2160, the 120 FPS patch, and FSR off. Startup
used on-demand compilation plus a background warm-up of 2,000 recent pipelines
over 36.06 s with six threads.

The private log contains 59 stats/pacing windows. Median per-window FPS was
78.7; the largest per-window P99 was 535.22 ms; the largest reported frame was
1,316.7 ms. It contains 56 `Stall` records over 100 ms, with zero VUID and guest
fault lines. Several stalls report zero synchronous compiles while driver time
is high. Driver/worker timings can overlap; these numbers do not establish
exclusive CPU time or the cause of the user's minor visual flicker. The short
window `stutters/min` metric is not comparable to a whole-run frame-trace rate.
No screenshot/capture was produced, and this is not a matched A/B benchmark.

## 2026-10-25: recent-pipeline hot-list A/B, inconclusive

Two interleaved 130-second prologue runs per condition compared
`THEORDER_HOT_WARMUP=1` and `=0`, using isolated copies of the same warm
profile, 4K borderless, and upscaling off. All four runs exited 0 without
reported faults or VUIDs. The harness recorded no captures; OS/driver cache
state was not controlled, and two repetitions are insufficient to separate
the setting from run-to-run variation.

| Condition | Median window FPS | Window P95/P99 of frame P99 (ms) | Maximum worst frame (ms) |
| --- | ---: | ---: | ---: |
| Hot-list enabled | 60.00 | 272.71 / 401.34 | 18,194.90 |
| Hot-list disabled | 59.33 | 510.17 / 3,888.76 | 8,074.80 |

The opposing extreme-frame results and noisy window tails make this A/B
inconclusive. It does not justify changing the warm-up default or claiming a
stutter reduction. Full run identities and per-run data are in the ignored
`out/20261025-hot-warmup-ab/` report and CSVs.

## 2026-10-25: warm-up A/B and overlap-copy smoke

An exploratory A/B compared `on_demand` with `full_warmup` using one 100-second
run per condition and isolated profile roots. Cache state was recorded as
`unknown`; this is not a controlled performance result and does not justify a
default change.

| Mode | Median FPS | Worst frame (ms) | Stutters/min |
| --- | ---: | ---: | ---: |
| On-demand | 55.4 | 542.5 | 59.05 |
| Full warm-up | 57.1 | 591.4 | 43.96 |

Full warm-up had fewer thresholded stutters in this pair but a worse worst
frame. The sample size and cache uncertainty prevent attributing that difference
to warm-up. Keep the current default and retain visible-draw priority; neither
approach can preempt a driver compile already in progress.

The local color/depth overlap-copy build also completed isolated 130-second
MSAA and no-MSAA smoke runs without crash/hang. In the MSAA run the measured
worst frame was 681.8 ms, P99 44.52 ms and 53.86 thresholded stutters/min. There
is no matched pre-change pacing baseline, so no stutter improvement is claimed.
See `GRAPHICS_REGRESSIONS.md` for the graphics-specific scope and limitations.

## 2026-10-25: reproduced stalls and pipeline queue priority

Source base: `d5534c5`, Windows 11, RTX 5060 Laptop GPU, NVIDIA 621.1.0.0,
Vulkan 1.4.357, CUSA00035 01.02. Built locally with MSYS2 CLANG64.
All runs below use the existing 130-second scripted prologue route, private
runtime directories and copies of `user`; original saves/caches were not cleared.

The baseline executable SHA-256 is
`78cad40b3c95578755f291db77a8be75141ba2772db1e249a69e9be61e51f08a`.
`out/rea-stutter/baseline.json` records 6,698 captured intervals, 53.40 mean FPS,
17.83/48.96 ms raw P95/P99 and a 1,324.628 ms worst interval. Of 50 intervals
above 100 ms, 45 also contain more than 50 ms synchronous compilation wait.
The worst interval contains 1,307.173 ms of that wait. This reproduces a
compilation-related stall; it does not establish the cause of every hitch.

REA opened and identified the baseline PE/x86-64 executable, but deep native
analysis was unavailable: Ghidra/IDA were not configured and the other reported
providers could not handle this target/host. REA process capture explicitly
rejects native Windows PTY execution. Consequently the measurements here come
from `tools/batch_runs.py`, not REA decompilation or runtime tracing. The exported
`out/rea-stutter/rea-evidence.json` has zero retained records and is not native
analysis evidence. The REA target was closed without changing server settings.

Local code inspection found that a new graphics pipeline can remain in the
ordinary compiler queue even after `WaitGraphics` needs it for a visible draw.
`THEORDER_VISIBLE_PIPELINE_PRIORITY` promotes only that queued job into the
existing priority queue before waiting. It cannot interrupt an already running
driver compile. After the user's gameplay test reported a significant
improvement, the default now enables this behavior; `=0` restores the old policy.
No visible draw is dropped, no shader specialization is merged and no cache is
invalidated. Job identity uses a weak reference to the existing packaged task,
without another allocation or retaining captured resources after completion.

Two interleaved repetitions per condition copied the same initial warm profile
from the baseline. `out/rea-stutter/priority-ab.json` records the development
build results below. These precede the final identity/lifetime refinements:

| Condition / repeat | Mean FPS | Raw P99 ms | Worst ms | Intervals/min >1.5x median |
| --- | ---: | ---: | ---: | ---: |
| FIFO / 1 | 56.68 | 18.53 | 644.10 | 26.81 |
| Priority / 1 | 57.95 | 18.34 | 594.70 | 23.48 |
| FIFO / 2 | 56.30 | 18.53 | 513.10 | 25.80 |
| Priority / 2 | 56.06 | 18.39 | 623.50 | 23.95 |

The later lifetime-guarded runtime pair, `out/rea-stutter/final.json`, exits 0
in both modes with no reported guest faults or hangs. FIFO records 56.99 mean
FPS, 18.38 ms raw P99, 565.70 ms worst and 23.47 intervals/min; priority records
56.04 FPS, 18.66 ms P99, 796.40 ms worst and 26.78 intervals/min. This pair
does not support a consistent speedup. It predates the final weak-identity
refinement; that refinement was rebuilt and checked with the focused tests,
not re-benchmarked for performance.

Captures show different dark cinematic/transitional positions, not matched
free-movement gameplay. Inputs are wall-clock scheduled; OS/driver caches and
physical input are not isolated. The earlier automated pairs do not establish
a reproducible speedup; enabling the default records the user's direct
gameplay observation and keeps the explicit rollback. This does not establish
stutter elimination.

Validation: `async-compiler-test`, `async-pipeline-cache-test` (real NVIDIA
Vulkan device), `async-fetch-snapshot-test` and the runtime build pass. The
compiler regression checks promotion order, no duplicated execution, unchanged
queue depth, repeated/finished-job promotion and captured-resource release.

Next gate: establish a visually verified repeatable gameplay checkpoint, then
attribute visible waits to queue residence versus driver execution using the
exact pipeline keys. Investigate why those exact resource-dependent combinations
are not already warmed. Do not remove required waits or increase compiler
concurrency on the strength of these results.

## Content-key reuse checkpoint (not a stutter fix)

`out\checkpoint-20261022\compute-reuse-final.json`: final guarded build, two
isolated 80 s title-screen runs using the same initial warm-profile copy.
Only `THEORDER_COMPUTE_PIPELINE_REUSE` changes (0 / 1). Both exit 0 without
reported guest faults; output captures confirm the title screen.

| Metric | Previous keys | Content reuse |
| --- | ---: | ---: |
| Captured duration (s) | 75.165 | 76.124 |
| Mean FPS | 58.418 | 56.632 |
| P95 / P99 (ms) | 17.759 / 18.242 | 17.793 / 18.514 |
| Worst frame (ms) | 699.038 | 1,360.099 |
| Last sampled binary hits / compiled | 1,118 / 47 | 1,129 / 44 |

**No frametime improvement is established.** This pair still has different
resource-dependent permutations, OS/driver state and asynchronous startup work.
Neither the fewer sampled compilations nor the worse worst-frame sample alone
establishes an optimization gain or a regression. One exact reuse selection
(`0x9846aaff`, index 1 -> 0) is logged in the enabled run; this is not a measured
count of saved driver creations. Details and the independent SPIR-V content
comparison are in `SHADER_CACHE_OPTIMIZATION.md`.

## Prologue evidence after the instrumentation checkpoint

Source: `9bd9ae7` for the first four runs; `qte.json` includes the scripted-input
fix described in `INPUT_IMPROVEMENTS.md`. Same host/game as below. Isolated profiles copy the existing
save with `--user-template user`; the original save is not used as a writable
runtime directory. The 130-second capture confirms the rendered handcuff scene.
The 300-second capture confirms the first Triangle QTE, not free movement/combat.

Local manifests under `out\checkpoint-20261022`:

| Manifest | Captured seconds | Mean FPS | P95 / P99 ms | Worst ms | Compile wait s | Binary hits / compiled |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `prologue.json` (cold port cache) | 126.465 | 36.998 | 18.066 / 313.817 | 5,300.484 | 47.331 | 163 / 472 |
| `prologue-warm.json` | 126.961 | 50.031 | 17.806 / 32.801 | 1,359.461 | 19.120 | 639 / 121 |
| `nohot.json` (same warm source, hot-list warmup disabled) | 127.907 | 47.269 | 17.892 / 97.901 | 3,067.849 | 18.513 | 594 / 124 |
| `extended.json` (300 s, later QTE) | 297.517 | 52.794 | 17.798 / 20.153 | 1,373.935 | 36.640 | 801 / 230 |
| `qte.json` (330 s, Triangle attempts, rebuilt runtime) | 326.264 | 56.519 | 17.735 / 18.305 | 991.303 | 18.425 | 1,051 / 119 |

The last run confirms the rebuilt runtime still boots/renders and receives
Triangle presses/releases plus a left-stick override. Its 310 s capture is a
dark transitional image, not evidence of free movement or successful QTE completion.
It uses the accumulated extended-run cache/save copy, so it is not an input-fix
performance A/B.

`repeat.json` subsequently uses the accumulated QTE profile: 10,195 intervals
over 175.810 s, 57.989 mean FPS, P95/P99 17.737/18.327 ms, worst 640.655 ms,
1,142 binary hits/62 compiled. Four spaced captures show different prologue
cinematic positions, including the guard close-up. This is not free movement,
an equivalent-version benchmark or a before/after optimization result.

These are diagnostic runs, not an optimization before/after. All exited 0 without
reported guest faults. Inputs are scheduled from launcher start, so compilation
changes scene progress at a given wall time; do not compare the rows as equivalent
gameplay. The "cold" label refers to the isolated port cache, not the OS/driver
cache; within-run binary reuse can still produce hits.

In the cold trace, 79 of 82 intervals over 100 ms also contain over 50 ms of
synchronous compilation wait. In the extended trace it is 90 of 93. Extended
frame 12,294 lasts 1,373.935 ms, including 1,363.117 ms of compilation wait.
This is direct evidence for visible compilation stalls, not evidence that every
stutter has that cause. A whole-trace P99 near 20 ms does not exclude second-long
outliers. No visible-draw waits were removed.

The captured 130-second route is reproducible with the existing harness:

```powershell
python tools\batch_runs.py --runs 1 --secs 130 --frame-trace `
  --scene isolated-existing-prologue-save --cache-state cold `
  --profile-root out\prologue-cold --user-template user `
  --manifest out\prologue-cold.json --report-dir out\prologue-cold-report `
  --cond prologue=THEORDER_PERF_LOG=1 `
  --pad "24:cross:0.5 28:cross:0.5 32:cross:0.5 40:cross:0.5 50:cross:0.5 60:cross:0.5 70:cross:0.5 80:cross:0.5 90:cross:0.5" `
  --frame-dump-at 110 --capture-dir out\prologue-cold-captures
```

Use the resulting isolated profile as the warm template, with a different output
root. Save progression and final scene must be checked before accepting a matched
comparison. The controller remained connected; physical input was not isolated.

## 2026-10-22 checkpoint: measured title-menu workload, not gameplay

The previous version of this document contained combat FPS, memory and improvement
tables without reproducible captures. Those numbers are withdrawn. An empty-set
hash fast path does not prove that gameplay stutters have been eliminated.

Host: Windows 11, Intel Family 6 Model 183, NVIDIA RTX 5060 Laptop GPU,
driver 621.1.0.0, Vulkan 1.4.357. Game: CUSA00035 01.02.
Source: consolidated `1fb2c3e` plus the instrumentation changes recorded in Git.
No AMD or Intel GPU compatibility is established by these runs.

Evidence is local, under `out\checkpoint-20261022` (not committed game data):

| Run / evidence | Scope | Result |
| --- | --- | --- |
| `manifest.json` | Initial native Windows launch | 0 flips, exit `0xC000007B`, not a GPU benchmark |
| `clang-manifest.json` | 30 s, CLANG64 environment | Exit 0, 1,440 logged flips |
| `metrics.json` | 65 s, isolated cold profile | Exit 0, 3,720 logged flips, 13 process samples |
| `captures\metrics-01-20261022T142847345849Z` | Final display capture | Title menu confirmed; scripted inputs did NOT establish gameplay |
| `ab.json` | 40 s cold / warm isolated port caches | Both exit 0; whole-trace statistics below |
| `native.json` | 90 s from normal PowerShell after DLL-path fix | Exit 0, 5,220 logged flips, 5,016 raw intervals; capture still frontend, not validated gameplay |
| `wait.json` | Final 25 s instrumentation check | Exit 0, 1,114 intervals; 400.853 ms measured compilation waits, maximum 146.128 ms in one interval |

The harness reports flips from periodic log messages; the CSV contains individual
observed intervals. Capture starts after the first flip and is flushed every five
seconds; abrupt process termination can omit the final unflushed tail.

| Whole-trace metric | Cold | Warm |
| --- | ---: | ---: |
| Captured intervals | 2,018 | 1,933 |
| Captured duration (s) | 35.073 | 36.298 |
| Mean FPS (frames / observed time) | 57.537 | 53.254 |
| Median frame (ms) | 16.658 | 16.679 |
| Frame P95 (ms) | 17.803 | 17.867 |
| Frame P99 (ms) | 31.827 | 32.384 |
| Worst frame (ms) | 369.008 | 1,880.082 |
| Stutters/min (>1.5x whole-trace median) | 42.768 | 51.243 |

These are single startup/menu samples, NOT a controlled optimization A/B or a
regression verdict. Warm used a copy of the earlier menu profile; cold copied
persistent data without `cache` or `shader`. OS/driver caches were not controlled.
Warm reported 267 shader metas and 155 binary hits at startup, but still compiled
new pipelines later.

The warm CSV's frame 452 took 1,880.082 ms with 1,377.143 ms of aggregate
pipeline-driver work, 2.667 ms recorder wait and 9.060 ms host-copy wait.
Driver work can run concurrently across workers: do not interpret its sum as
exclusive CPU time. Logs also record key-scoped visible-pipeline waits.
This identifies a compilation-related investigation target, not the complete cause.
Visible draws must not be dropped to improve this metric.

The final native Windows run confirms the startup-path fix without manually
preparing CLANG64: 58.934 mean FPS, 17.822/18.522 ms raw P95/P99, 401.409 ms worst
frame, 23.263 stutters/min over 85.112 captured seconds. Binary totals: 9 hits,
199 compiled, 199 saved, no reported rejection/error. A `--level` argument and
scripted Cross presses did not prove gameplay; the final capture shows the
frontend's save-information screen. Do not compare this longer startup interval
against the shorter samples as an optimization gain.

## Reproduce

From a normal PowerShell, after `build.sh` has produced the development executable:

```powershell
python tools\batch_runs.py --runs 1 --secs 65 --frame-trace --scene title-menu `
  --cache-state cold --profile-root out\baseline-cold `
  --manifest out\baseline-cold.json --report-dir out\baseline-cold-report `
  --cond measured=THEORDER_PERF_LOG=1
```

For warm runs, use a new profile root, `--cache-state warm`, and `--user-template`
pointing to a previously collected isolated profile. Never clear the user's caches
or modify the user's saves to manufacture a cold run.

Raw CSV: `THEORDER_FRAME_CSV`, set per run by `--frame-trace`.
Process sampler: `THEORDER_PERF_LOG=1`, twice-per-second sampling, five-second logs.
CSV wait/work counters are overlapping instrumentation, not a CPU/GPU frame-time split.
GPU timestamp profiling remains the existing `THEORDER_GPU_PROFILE=1` path.
Process WDDM dedicated/shared usage is now collected separately; see
`MEMORY_PROFILING.md`. Arbitrary mutex timing, asset I/O, per-thread CPU and full GPU
frame timing are not yet integrated into this report.

## Version comparison and remaining gates

- v0.7.5: `windows-v0.7.5`, `4c76cd3`.
- v0.7.6 candidate: `windows-v0.7.6-rc.1`, `f1f0c8a`; no final v0.7.6 tag was found.
- Previous remote master: `10e44b4`; consolidated master: `1fb2c3e`.
- No equivalent gameplay benchmark across those builds, bisect verdict, combat,
  explosions, transitions or 30-60 minute gameplay soak has been obtained here.
- Establish a verified repeatable gameplay checkpoint and input replay before
  attributing the user's v0.7.6 gameplay regression or claiming it fixed.
