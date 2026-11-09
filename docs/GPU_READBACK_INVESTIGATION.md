# Write-fault readbacks on the game's main thread: eboot `0xcc8791` (2026-11-09)

Diagnosis only. No synchronization, memory tracking or fault handling was changed.

## Provenance

| Item | Value |
|---|---|
| Game | CUSA00035 01.02, eboot SHA-256 `f742eb17…58d8` |
| Static image | `out/eboot_ghidra.exe` (`scripts/eboot_pe.py`), SHA-256 `0f42dce6…146f`; Ghidra address = `0x100000000` + eboot offset |
| Tool | REA (`rea-agents` 6.7.0) with Ghidra 12.1.4; `inspect_native_instruction`, `read_function_instructions`, `procedure_callers` |
| Runtime | guest image at `0x800000000`: RIP `0x800cc8791` = eboot `0xcc8791` (frame-stats "Buffer downloads" lines) |
| Port builds | `19b998b` (v0.8.0) benchmark build `64e08804…0a92`; fixed build `970b9d33…0c97` (WaitGraphics fix only, nothing here) |
| Logs | `out/affinity-bench/logs/{full,fixdiag}/*.log` (not versioned) |

## What the game does at `0xcc8791`

`0x100cc8791: VMOVSS dword ptr [RAX], XMM0` is the first of nine stores in `sub_cc85a0` (only
direct caller `sub_ca47c0`): `RAX` is the return value of `sub_c7c1d0(this + 0x2288)`, and the
function writes a 0x24-byte block right after (`[RAX]`..`[RAX+0x20]`: five floats loaded from
the globals at `0x16da6c0` (+0xa8..+0xb4) and `0x16b1fd8` (+0x44, or a constant when null), a
bit field of `[0x16da6c0]+0xa0`, the low byte of a counter at `this+0x22f8` (incremented at the
end of the function) and a count at `this+0x2f38`). What the globals represent was not resolved.

`sub_c7c1d0` hands out constant memory in one of two ways (flag at `+0x2a` of its descriptor):
an atomic bump allocation (`LOCK XADD`) from a per-frame arena (global at `0x170e020`), or a
two-slot ping-pong: it flips a bit at `+0x26` and returns `base + bit * (count * stride)`. In
the logs the faulting stores land at `0x105b5023b4` and `0x105b5023e4`, 0x30 apart: the two
slots of the ping-pong. The GPU writes 1 KiB starting at `0x105b502414` (frame-stats "Buffer
readback" line), 0x30 bytes after the second slot, in the same 4 KiB page.

## What the port does with that store

1. The page holds GPU-written bytes, so the buffer cache keeps it write-protected
   (`memory_tracker`, GPU-modified region).
2. The store faults; `page_manager.cpp` calls `Rasterizer::OnWriteFault` on the faulting (game)
   thread, `assume_locks = false`.
3. `Rasterizer::InvalidateMemory` → `TextureCache::ResolveReadbacks`, then
   `BufferCache::InvalidateMemory` → `memory_tracker->InvalidateRegion` → `BufferCache::ReadMemory`
   (`is_write = true`, `async = false`).
4. `ReadMemory` sends the flush to the GPU command thread with `Liverpool::SendCommand<true>`: the
   game thread blocks on a semaphore until it ran.
5. On the GPU thread: `DownloadMemory` of a 512 KiB window around the address, which calls
   `scheduler.Finish()` (all submitted GPU work), copies the window back to guest memory, then
   marks only the written range CPU-modified.

Measured (frame stats, prologue, last 45 s of each run): ~56 such writes a second, about one per
frame, each one download; 186-331 ms of download time a second (full CPU, v0.8.0: 305-331 ms/s;
4P, fixed build: 186-228 ms/s), i.e. 19-33% of the wall time of the thread that stores. Earlier
profiling put 32.4% of the main thread's samples here (`docs/REA_NATIVE_ABSTRACTION.md`).
"Write-fault downloads: 0 on GPU-written bytes, N beside them": none of the counted stores
touched a byte the GPU wrote. The cost is page-granular sharing between a CPU-written constant
slot and a GPU-written range, not a CPU read of GPU data.

## Why the obvious shortcuts are wrong

- Skipping the download, or not protecting the page: the next upload of the page would replace
  the GPU's 1 KiB with the CPU's stale copy.
- Byte-precise skipping (tried 2026-10-18, PROGRESS.md): after one fault unprotects the page the
  CPU later writes over bytes the GPU wrote (ring buffer `0x105b502000`, CPU at `+0x3b4`, GPU
  `+0x414..0x813`); skipping the GPU's bytes at upload dropped those CPU writes and the game hung
  at boot. The fault only reveals the first store; later stores to an unprotected page are
  invisible to the tracker.
- Returning old data or pretending the download finished breaks the coherence the game relies on.

## Hypotheses for a later, separate change (not implemented)

1. **Wait for the writer, not for everything.** `DownloadMemory` calls `scheduler.Finish()`; the
   download only needs the submissions up to the last one that wrote the window (a tick per
   GPU-modified range). Measure first: the tick of the last write of `0x105b502414..+0x400`
   against the current tick when the fault arrives; if the writer is usually a frame old, a
   tick wait would return without a drain. Coherence stays identical (same bytes downloaded).
2. **Smaller window.** The window is 512 KiB for scattered islands elsewhere; this page needs
   only its GPU range (1 KiB). Cost is copy size and the read-back mapping, secondary to the wait.
3. **Keep the GPU range synchronized ahead of the fault.** The read-watch already copies
   `0x105b502414` (1 KiB) back after each GPU write ("copied back after each write from now on");
   if that copy is complete when the store faults, the write path could use it instead of a new
   download. Needs proof that the copied-back bytes are the latest GPU write for that page at the
   time of the fault (tick ordering), otherwise it is stale data.

### Hypothesis 1 measured (2026-11-09): mostly refuted

`THEORDER_READBACK_TICK_LOG=1` (`cf3c84c`) records the scheduler tick of the last GPU write per
4 KiB page and classifies the newest write behind every synchronous download. Prologue route,
2 runs × 125 s, warm cache (`out/r1/manifest-r1.json` in the worktree):

| Run | Sync downloads | Writer in the open command buffer | Submitted, running | Complete |
|---|---|---|---|---|
| 1 | 6943 | 6265 (90.2 %) | 25 | 653 |
| 2 | 6977 | 6272 (89.9 %) | 14 | 691 |

In gameplay windows 99-100 % are in the open command buffer (e.g. 270 of 272 in one 5 s
window); the complete ones cluster in the loading part. The newest write to the downloaded range
is recorded in the very command buffer being built when the game's store faults, so submitting it
and waiting (`Finish()`) is required: a wait on an older tick would return stale bytes. A tick
wait could only shortcut ~10 % of downloads. Hypotheses 2 (smaller window, copy cost) and 3
(serve the store from the read-watch copy) do not depend on the writer's age; 3 would have to
prove that copy is newer than a write still unsubmitted, which this measurement makes unlikely.

Each of these needs a capture-matched A/B (same route, warm cache, frame traces, image
comparison of the scene) before it can ship.
