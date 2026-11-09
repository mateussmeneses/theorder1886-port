# 4P stability retest with the `WaitGraphics` fix (2026-11-09)

Planned sample: 20 runs `THEORDER_TASK_IDLE=0` (base) + 20 runs `THEORDER_TASK_IDLE=2` (idle2) at
4 P-cores, with the build that carries the `PipelineCache::WaitGraphics` fix
(`docs/WAIT_GRAPHICS_INVESTIGATION.md`), compared with the v0.8.0 4P batches of
`docs/PERFORMANCE_BASELINE.md` (2026-11-09, restricted affinity). Affinity on this CPU measures
contention between the port's own threads; it does not stand in for an older CPU (#65) and does not
reproduce #40.

## Setup

| Item | Value |
|---|---|
| Host | Windows 11 10.0.26300, i7-14650HX, RTX 5060 Laptop (NVIDIA 621.14.0.0, Vulkan 1.4.357), commit limit 51.6 GB |
| Game | CUSA00035 01.02, eboot SHA-256 `f742eb17…58d8` |
| Build | `19b998b` + WaitGraphics fix + opt-in wait log, worktree `theorder1886-port-wg` (no checkout WIP), `theorder-probe.exe` SHA-256 `970b9d33…0c97` |
| Compared build | `19b998b` (v0.8.0), `64e08804…0a92`, batches `p4`, `p4b`, `p4c`, `p4d` |
| Affinity | `--affinity 0x5500` (logical processors 8, 10, 12, 14: one per P-core, no SMT sibling) |
| Route | prologue opening, `--pad "24:cross:0.5 28:cross:0.5 32:cross:0.5 36:cross:0.5 40:cross:0.5 50:cross:0.5 60:cross:0.5"`, 125 s, `--frame-trace` |
| Cache | warm: the port cache of `user/` copied into an isolated profile per run (`--user-template`) |
| Environment | both: `THEORDER_FRAME_STATS=1 THEORDER_PERF_LOG=1 THEORDER_PENDING_WAIT_LOG=1`; base `THEORDER_TASK_IDLE=0`, idle2 `=2`; pre-queueing on (default) |
| Order | 4 batches (`r4p1`..`r4p4`) of 5 + 5, conditions alternating run by run, batches sequential |
| Guard | free disk and system commit checked before each batch; a watcher logged commit and game private bytes every 2 s and would stop the game above 30 GB private (never triggered) |
| Window | last 45 s of presented frames (prologue); CPU over the same window |

Data (not versioned): `out/affinity-bench/` (`manifest-r4p*.json`, `batch-r4p*.log`,
`logs/r4p*/`, `data/r4p*/*/frames.csv`, `retest.log`, `memwatch.log`, `r4p*.json`,
`retest_combine.py`, `retest.json`).

## Outcome

| Condition | Complete | Wilson 95 % | `bad_alloc` | Fault `+0x126ce4` (WaitGraphics) | Watchdog | Other native fault | Runner error | Resource stop |
|---|---|---|---|---|---|---|---|---|
| base (fixed) | 19/20 | 0.76-0.99 | 0 | 0 | 0 | 1 | 0 | 0 |
| idle2 (fixed) | 19/20 | 0.76-0.99 | 0 | 0 | 0 | 1 | 0 | 0 |
| base (v0.8.0) | 7/10 | | 3 | 0 | 0 | 0 | 0 | 0 |
| idle2 (v0.8.0) | 5/8 | | 2 | 1 | 0 | 0 | 0 | 0 |

(The v0.8.0 runner labelled the `bad_alloc` runs `HUNG` or `EXIT 23`; causes were read from each log.)

Fisher exact, two-sided, both conditions together: the two failure kinds the fix targets, 6/18
(v0.8.0) against 0/40 (fixed), p = 0.0005; any failure 6/18 against 2/40, p = 0.008. Per condition
the samples are small (base 3/10 vs 1/20, p = 0.10; idle2 3/8 vs 1/20, p = 0.06).

Direct evidence that the hazard occurred and was survived: with `THEORDER_PENDING_WAIT_LOG=1`,
every completed run logged 13-42 waits during which the pending map grew, and in 14 runs (11 base,
3 idle2) one of those waits rehashed the map (8 → 16 or 16 → 32 buckets). Each of those is the
exact case that made v0.8.0 erase through a dangling iterator. No `bad_alloc` in any log; peak
private bytes 5.75-5.76 GiB in both conditions (mean of per-run peaks 5.54-5.55 GiB, 95 % CI
±0.07), the same plateau as the completed v0.8.0 runs.

### The two failures (both at boot, neither in the code that was changed)

Both happened 9 s apart, as the last two runs of `r4p4` (09:49:22 and 09:49:30 UTC), after 38
completed runs; a Windows Restart Manager session started at 09:49:01 UTC (Application log) and
Defender installed an update at 09:50; no display-driver event (no TDR) in the System log.

1. `r4p4/base-05`, 4.6 s, flip 1: read of `0x40` at `theorder-probe.exe+0x16bc7b`, thread
   `shadPS4:GpuCommandProcessor`. Symbolized on the same image: `Scheduler::Finish` (first
   instruction reading `this`, `this` = null) ← `Fsr4Upscaler::Impl::Destroy` (vk_fsr4.cpp:93)
   ← `~Impl` ← `~Fsr4Upscaler` ← `TemporalUpscaler::OnFrameStart` replacing `fsr4`
   (vk_temporal_upscaler.cpp:317, upscaler change at frame 1) ← `Rasterizer::DrawRecord` ←
   `Rasterizer::Draw` ← `Liverpool::ProcessGraphics`. The `Scheduler&` member of the destroyed
   `Impl` read as null: the object being destroyed was not intact. The same upscaler switch at
   frame 1 runs in every run that reaches frame 1 (the `Output resolution … (live)` line is in 39 of
   40 logs; the other is the idle2 run that faulted before its first frame).
2. `r4p4/idle2-05`, 4.6 s, flip 0: read of `0x20` in `nvoglv64.dll+0xf33248` (NVIDIA driver)
   inside `vkWaitForFences` from `Presenter::GetRenderFrame` (vk_presenter.cpp:932) ←
   `PrepareBlankFrame` ← `VideoOutDriver::SwapThread`, before the first frame.

Neither stack touches `pending_graphics` or `WaitGraphics`; no earlier log under `out/affinity-bench/logs` (v0.8.0,
all sets) recorded either fault. Cause not established: one occurrence each. They are
recorded as open, not attributed to the fix or to the idle mode.

### System commit during the batches

The guard's log (`memwatch.log`, every 2 s) shows system commit at 30-45 GB for most of the run
time and up to 50.4 of 51.6 GB (05:43:40 and 06:05:14 local), against 21-26 GB
between batches and a game private-bytes peak of 5.76 GiB. Part of the gap is the guest
memory section: `src/runtime_memory.c` creates one pagefile-backed `SEC_COMMIT` section of 5056 MiB
direct memory + 1 GiB flexible memory, charged to system commit but not to the process's
`PrivateUsage`. That accounts for about 12 GB per running game (6 GiB section + ≤ 5.8 GiB
private), not for all of the 45-50 GB peaks: at 05:43:27 commit was 44.3 GB with the next run's
game at 0.68 GiB private, 12 s after the previous game was last seen, so commit from the previous
process (or something else) was still charged. Not measured further (no per-process commit
breakdown, the guard samples one game process). At the two boot faults commit was 31.8 and
29.7 GB, so they did not happen near the limit. Hosts with a smaller page file could reach the
commit limit; worth a per-process commit sample in the next batch.

## Frame time and CPU (completed runs, last 45 s; mean ± 95 % CI of per-run values)

| Condition | n | FPS | P50 ms | P95 ms | P99 ms | P99.9 ms | Worst ms | Frames > 33 ms | > 100 ms | Process CPU | Cores |
|---|---|---|---|---|---|---|---|---|---|---|---|
| base (fixed) | 19 | 56.49 ± 0.57 | 16.66 ± 0.02 | 18.46 ± 0.84 | 39.12 ± 1.44 | 284.9 ± 27.7 | 469.4 ± 38.3 | 50.7 ± 8.4 | 11.4 ± 1.4 | 16.35 % | 3.92 |
| idle2 (fixed) | 19 | 57.08 ± 0.27 | 16.65 ± 0.01 | 17.64 ± 0.14 | 36.02 ± 2.30 | 233.8 ± 10.9 | 356.3 ± 43.2 | 36.2 ± 5.0 | 12.7 ± 1.1 | 5.79 % | 1.39 |
| base (v0.8.0) | 7 | 56.75 | 16.61 | 17.86 | 37.94 | 242.2 | 425.5 | 40.7 | 13.1 | 16.42 % | 3.94 |
| idle2 (v0.8.0) | 5 | 57.00 | 16.64 | 17.62 | 25.51 | 263.3 | 475.8 | 28.6 | 11.8 | 5.41 % | 1.30 |

Medians (fixed): base FPS 56.90, P99 38.2, P99.9 278.3, worst 447.5; idle2 FPS 56.99, P99 37.3,
P99.9 237.4, worst 356.4.

Per-run distributions (fixed build):
- P99 base 36.3-48.0 (all 19 above 36 ms); idle2 22.6-40.8 (17 of 19 at 33.8-40.8, two at 22.6
  and 23.6).
- P99.9 base 197.9-391.9; idle2 201.2-294.4.
- Worst frame base 326.5-607.2; idle2 228.9-516.4.

Reading:
- CPU: mode 2 keeps the 4P game at 1.39 of 4 cores against a saturated 3.92 (same as v0.8.0).
- FPS, P95, P99.9 and worst frame are better with idle2 (non-overlapping CIs for P95, P99.9 and
  worst). P99 overlaps: at 4P both conditions now miss about one vblank in sixty in most runs.
  The v0.8.0 idle2 P99 of 25.5 ms came from 3 of 5 runs at 18 ms; here 2 of 19 runs are that low,
  so the earlier "mode 2 removes most missed vblanks at 4P" does not hold up on a larger sample.
- Slow frames remain pipeline-compile waits (`compile_wait` 2.1-2.6 s per run inside frames over
  33 ms; `analyze.py` attribution, overlapping columns).
- The fix does not change frame time measurably: the v0.8.0 means fall inside or near the fixed
  build's spread, and v0.8.0 completed runs are a survivor sample (failed runs excluded).

## Conclusions

- The two failure kinds of the earlier 4P sample (`bad_alloc` and the `0x126ce4` fault) did not
  occur in 40 runs with the fix, while the condition that caused them (a rehash of the pending map
  during a wait) occurred in 14 of those runs. Combined with the unit tests and the mutation check,
  the fix is verified for this host and route.
- Two new, single-occurrence boot faults (FSR 4 context teardown at the first upscaler switch;
  NVIDIA driver in the presenter's fence wait) remain open, unattributed.
- Mode 2 stays the default (lower CPU, equal or better frame time). No claim about #65 or #40.
