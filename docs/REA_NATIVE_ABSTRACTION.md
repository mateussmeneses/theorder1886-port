# Native abstraction of eboot code (REA investigation)

Where the port's CPU time goes in the eboot's own code, what can be replaced by native Windows
code, and the first replacement (the job workers' idle callback). Dated 2026-11-08; eboot 01.02.

## 1. Environment

- REA `rea-agents` 6.7.0 as an MCP server (`npx -y rea-agents@6.7.0 mcp`, registered in the user's
  Claude configuration with `REA_GHIDRA_LANGUAGE_ID=x86:LE:64:default`,
  `REA_GHIDRA_COMPILER_SPEC_ID=gcc`, `REA_GHIDRA_STARTUP_TIMEOUT_MS=3600000`,
  `GHIDRA_HEADLESS_MAXMEM=8G`). The npx package is the published one, which carries the Windows
  Ghidra addon (see docs/ENGINE_RE.md).
- Ghidra 12.1.4 headless, OpenJDK 21.0.12.1; Rizin 0.9.1 configured for REA; `llvm-objdump`
  (MSYS2 CLANG64) for exact instruction listings.
- `open_binary` returns at once; the first query (`binary_overview`) imports and analyzes the image
  (about 12 minutes on this PC, as documented). Nothing is kept between sessions.
- REA tools used: `binary_session`, `open_binary`, `binary_overview`, `batch_decompile`, `xrefs`,
  `find_xrefs_to_name`. `find_xrefs_to_name("scePthreadYield")` answers `name_not_found`: import
  thunks carry library-qualified names, so call sites of imports were found from the import slots
  (PLT `jmp [slot]` stubs, then `call rel32` to them) with a short script instead.

## 2. Binary identity

| Field | Value |
|---|---|
| Title / version | CUSA00035, APP_VER 01.02 (`UP9000-CUSA00035_00-TO1886GAME000000`) |
| eboot.bin | 25,207,729 bytes, SHA-256 `f742eb1793f92772a5c4bdd174e29b08e0b53f68d373a37f4cda730703e558d8` |
| Ghidra wrapper | `out/eboot_ghidra.exe` (`scripts/eboot_pe.py`), SHA-256 `0f42dce63ca7e84e9adab6659e6c7884d0d33adf949219eac1a30a2eb213146f` |
| Ghidra inventory | 28,521 procedures (27,959 `.eh_frame_hdr` starts exported as `sub_<A>`), 38,905 strings |
| Linked modules | libc.prx at guest offset 0x1770000 (2,164 function starts), libSceFios2.prx at 0x1850000 (650) |

## 3. Addresses

eboot address **A** = Ghidra **0x100000000 + A** = probe guest offset **A** (the eboot is at offset 0
of the loaded image; libc.prx and libSceFios2.prx follow at the bases in `out/link.json`). Checked
three ways: `BASE`/`HEADERS` in `scripts/eboot_pe.py`, REA's `.text` segment starting at
0x100000000, and REA resolving 0x10020a560 to `sub_20a560`. PE RVA = 0x10000 + A.

`tools/guest_symbols.py <game>` writes `out/guest_symbols.json` (eboot `sub_<A>` starts plus the
modules' starts named after their exports); `tools/sample_report.py` uses it to name guest frames.

## 4. What this host executes

On a CPU with MOVBE, F16C, BMI1 and LZCNT (here an i7-14650HX) the ISA rewrite applies to **no
site**: the eboot runs as native x86-64 code. Nothing is interpreted, so a native replacement only
pays where the guest algorithm itself wastes time, or where an HLE import is expensive.
`memcpy`/`memset`/`memmove`/`memcmp`/`strlen` imports are already host functions
(`src/runtime.c`); the rest of libc is the game's own libc.prx, linked as guest code.

## 5. Dynamic map

Profiler: `THEORDER_SAMPLE` (src/probe.c), 1 kHz suspension sampling. Changes made for this work:
the sampler no longer suspends itself (`THEORDER_SAMPLE=*` used to hang), samples up to 256
threads, and unwinds guest frames through the eboot's frame-pointer chain (73% of its functions
start with `push rbp; mov rbp, rsp`); guest threads carry a `guest:<name>` thread description
(`src/runtime_thread.c`), so `THEORDER_SAMPLE=guest:` selects exactly the game's threads.

Scenario: CUSA00035 01.02, 1920x1080, warm port cache copied into an isolated profile,
`tools/batch_runs.py --pad "24:cross:0.5 28:cross:0.5 32:cross:0.5 36:cross:0.5 40:cross:0.5
50:cross:0.5 60:cross:0.5"` (title, then the in-engine prologue opening, up to ~660 draws/frame),
samples from 70 s to 110 s. RTX 5060 Laptop, NVIDIA 621.1.0.0.

| Thread | Where the samples are |
|---|---|
| `main` | 56.9% waiting for the flip (`sceKernelWaitEqueue`, vsync slack); **32.4% blocked in `BufferCache::InvalidateMemory` -> `ReadMemory`** from `Rasterizer::OnWriteFault`; ~7.7% eboot code |
| `Task Kernel` x4 | 97% eboot code, of which 81.6% self in `sub_20a950` (queue pop), 6.6% `sub_8a7380`, 6.1% `sub_20ad20`; jobs proper ~1-2% |
| audio (`AK::*`), streaming, save dialog | waiting; `AK::EventManager` ~6% eboot code (Wwise mixing) |

So the four job workers burn four cores polling empty queues, and the main thread's largest cost
is a GPU readback forced by CPU writes into GPU-tracked pages.

## 6. Imports and runtime costs

There are no per-import counters; the costs below come from host frames in the guest threads'
samples.

| Import / runtime path | Cost observed | Note |
|---|---|---|
| write faults -> `BufferCache::InvalidateMemory`/`ReadMemory` | 32.4% of main (wall) | eboot float stores at 0xcc8791 (`vmovss` into a buffer from `sub_c7c1d0`) beside GPU-written bytes; docs/BACKLOG.md item 3. GPU synchronization: not changed here |
| `scePthreadYield` (`thread_yield`) | 2.7% of the workers | called by `sub_8a7380` every 1,800 empty polls |
| `sceKernelWaitEqueue` | 57% of main | pacing wait, not work |
| `memcpy` & co. (`guest_memcpy`) | 0.1% of main | host functions already; one relaxed atomic counter each (~800k calls per run: negligible) |

## 7. Candidates

Ranked by measured cost, confidence and replacement risk (`out/rea/candidate_functions.json`).

| # | Function (eboot) | Evidence | Decision |
|---|---|---|---|
| 1 | `sub_8a7380` task idle callback | 54-byte leaf-like function; only reference is the scheduler constructor argument at 0x8a1c78; workers 97% busy polling | **replaced** (below) |
| 2 | `sub_20a950` queue pop | 81.6% of worker samples, but that is empty polling; lock-free queue with 165 callers | not replaced: #1 removes the polling |
| 3 | `sub_20ad20` worker loop | 6.1% | contract kept; its callback is swapped |
| 4 | stores at 0xcc8791 (`sub_cc85a0`) | 32% of main through write-fault readbacks | GPU memory tracking; documented only |
| 5 | `sub_877cf0` flip wait | 54% of main | not a cost |
| 6-10 | `sub_44cce0`, `sub_a86c00` (frame phase dispatcher), `sub_1fd010`, `sub_9920f0` (sub-world update: clock, dt, rwlock callbacks), `sub_2546d0` (43 KB update) | 3-5.5% inclusive each, little self time | virtual-call orchestration over global game state: no safe native equivalent |

No pure math, hashing or copy routine showed up hot on this host: the guest's own computation is
small next to the GPU translation, so there is nothing of that kind worth replacing yet.

## 8. The task idle callback

**Contract** (from the instructions at 0x8a7380..0x8a73b5, REA's decompilation and the worker loop
at 0x20ad20..0x20adb8): `void(uint64_t *idle_polls)`, System V. The worker zeroes `*idle_polls`
after running a job, adds 1 (unless it is all ones) after each empty poll and calls the callback,
re-read from `scheduler+0x1d0` each time. The callback, when `*idle_polls >= 0x709` (unsigned) and
`scePthreadSelf()` differs from the 64-bit handle at 0x1682de8, calls `scePthreadYield` and zeroes
`*idle_polls`. It touches nothing else. The scheduler constructor `sub_20b860` gets it as its seventh
argument from `sub_8a10a0` (0x8a1c78), unconditionally.

**Replacement** (`src/task_idle.h`, `src/runtime_task_idle.c`, installed by `install_task_idle` in
`src/probe.c`): when `THEORDER_TASK_IDLE` is 1 or 2 and the 54 bytes at 0x8a7380 match their FNV-1a
(no game bytes in the repository), the function's entry becomes `jmp rel32` to a 13-byte stub
(`mov r11, imm64; jmp r11`; r11 is scratch in System V) at the end of the stub area after the
image, which reaches `task_idle_callback`, a `sysv_abi` C function: the caller's convention, as for
every HLE export, so no ABI bridge is needed. A mismatch leaves the image untouched and prints why.

| `THEORDER_TASK_IDLE` | Behaviour |
|---|---|
| 0 | the guest code, unchanged (opt-out) |
| 1 | the same policy natively (yield every 1,800 empty polls) |
| 2 (default since v0.8.0) | after `THEORDER_TASK_IDLE_SPINS` (8) consecutive idle yields, sleep `THEORDER_TASK_IDLE_SLEEP_US` (200 us, high-resolution waitable timer) per further idle period |

Mode 2 keeps the idle streak in the counter's high 32 bits: the worker's own reset after a job ends
the streak exactly, without a heuristic and without per-thread state. The thread at 0x1682de8 keeps
never yielding. Worst added job latency: one sleep period, only after ~8 x 1,800 empty polls.

## 9. Tests

`tests/test_task_idle.c` (`task-idle-test`, in `tools/run_tests.sh`):
- policy (thresholds, saturation, streak, reset by a job, streak cap), environment parsing;
- install refusals (disabled, other bytes, outside the image, no stub room) leave the image
  unchanged; the patched `jmp -> stub -> sysv callback` path runs on a synthetic function;
- **differential**: with `out/sub_8a7380.bin` (`python tools/task_idle_bytes.py <game>`, the user's
  own bytes) the original machine code runs in an image with the eboot's layout (its
  `scePthreadSelf`/`scePthreadYield` PLT stubs jump to mocks; the handle at 0x1682de8) against the
  installed mode 1, on 20,000 counters (edges plus random), with the calling thread excluded one
  time in four: same counter after the call and same yields in every case (12,208 yields, 4,081
  excluded cases exercised).

## 10. Measurements

Host: Windows 11 10.0.26300, i7-14650HX (24 logical CPUs), RTX 5060 Laptop, NVIDIA 621.1.0.0.
Build: HEAD 7a52434 plus this change, in a separate worktree (the checkout's uncommitted
`runtime_file.c` fallback sent the title to a black "Encountered an error" screen, see 11).
Scenario as in section 5; each run a fresh copy of the warm port cache (`--cache-state warm`), so
each run also compiles ~500 pipelines (first visit for the driver). Statistics over the last 45 s
of presented frames (the prologue); CPU = `THEORDER_PERF_LOG` process CPU over all logical CPUs,
same window. Interleaved runs (`tools/batch_runs.py`, two batches), failed runs excluded.

| Condition | Runs | Process CPU | P50 ms | P95 ms | P99 ms | Worst ms | Frames > 50 ms |
|---|---|---|---|---|---|---|---|
| base (guest code) | 6 | 22.07 +- 0.71% | 16.65 | 17.75 | 18.43 | 255-456 | 11-20 |
| `THEORDER_TASK_IDLE=1` | 4 | 21.46 +- 0.68% | 16.65 | 17.73 | 18.37 | 253-450 | 10-16 |
| `THEORDER_TASK_IDLE=2` | 4 | **6.97 +- 0.42%** | 16.66 | **17.55** | **18.12** | 212-387 | 11-20 |

- Mode 2 takes ~15 points of process CPU, about 3.6 logical CPUs: the four workers' polling.
- P95 is 0.2 ms lower in every mode-2 run (17.52-17.57 against 17.71-17.83 for base); P99 ranges
  overlap (17.97-18.38 against 18.22-18.62). FPS is held by vsync (60 Hz) in all conditions.
- Worst frames and frames over 50 ms do not change: they are pipeline compilations.
- Mode 1 matches base, as the differential test predicts: the native path itself costs nothing
  measurable.
- Raw data: `out/rea/bench-summary.txt`, manifests `out/rea/manifest-bench3.json` and
  `manifest-bench4.json` (not versioned).

## 11. Limits

- One host (24 logical CPUs). The CPU saving is measured; whether it lowers stutter on CPUs with
  fewer cores, where four spinning workers compete with the port's shader and draw threads, is not.
  Mode 2 is the default since v0.8.0; `THEORDER_TASK_IDLE=0` restores the guest code.
- Runs end in a pre-existing GPU library failure at times: `liverpool.h:208 Unhandled exception:
  std::bad_alloc`, private memory 15-24 GB, at the prologue load. It hit 1 of 14 base runs here (and
  a 2026-11-01 run), and 3 of 9 runs with the replacement during batches that ran while this
  investigation filled the system disk with run-profile copies (1 GB each); with disk space restored,
  8 of 8 runs with the replacement completed. Its cause is not established.
- The checkout's uncommitted `src/runtime_file.c` change (GPU localization stub first) made
  `/primary/.../b0b4572d23e56916` resolve to the `ca979ee752b532b2` stub: the game showed a black
  screen with "Encountered an error" and never reached the 3D title. Profiles taken in that state
  are not used above.
- The sampler's guest unwinding skips the direct caller of a guest leaf without a frame pointer.
- `sub_8a7380` is identified by its bytes for eboot 01.02 only; other builds keep the guest code.
