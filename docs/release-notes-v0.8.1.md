# The Order: 1886 PC Port - Windows v0.8.1

A stability and compatibility release on top of v0.8.0. It fixes a crash and memory
blow-up in the pipeline cache, adds a fix for button-mash QTEs with the frame rate
patches and the missing instruction emulation that stopped Intel 2nd generation
(Sandy Bridge) CPUs at start, and makes bug reports carry the game log. The QTE and
old-CPU changes are tested by analysis and on one newer PC, not yet in the reported
scene or on real 2nd/3rd generation hardware; no open issue is claimed resolved before
the reporter's retest.

## Fixes

1. **Crash and `std::bad_alloc` while pipelines compile (v0.8.0 regression):**
   `PipelineCache::WaitGraphics` erased a pending pipeline through an iterator kept
   across a wait, during which new pipelines are queued. When that insertion
   reorganized the table, the erase hit the wrong entry: a crash
   (`theorder-probe.exe+0x126ce4`) or a corrupted count that later grew the table until
   memory ran out (up to 78 GB private, other programs crashed). The build now erases
   by key. Measured on one PC at 4 cores: those failures went from 6 of 18 runs (Wilson 95 %
   16-56 %) to 0 of 50 (0-7 %) (`docs/WAIT_GRAPHICS_INVESTIGATION.md`, `docs/AFFINITY_STABILITY_RETEST.md`).
2. **Button-mash QTEs above 30 FPS (#101):** the game scales each press by the frame
   time while the meter drains per second, so with the 60 FPS patch a press counted
   half and with 120 FPS a quarter (the prologue's drowning QTE could not be passed).
   A new one-byte patch, applied together with the frame rate patch, makes each press
   count as at 30 FPS. Not yet confirmed in that scene. The patch is located by a byte
   signature checked on CUSA00035 01.02 only; on another build where it is not found
   exactly once it is skipped (the log says `Patches: QTE Fix … skipped`).
3. **Intel 2nd generation CPUs (Sandy Bridge, #65):** five F16C instructions of the
   game were never emulated, so the port stopped at start on CPUs without F16C. They
   are now emulated by the exception handler. The rewrite that older CPUs need was also
   made cheaper: with MOVBE loads inline, a forced Sandy/Ivy Bridge path in the prologue
   went from a 99th-percentile frame time of 31 ms back to 18 ms, the same as unrewritten
   code, with about 25 % more stutters per minute; BEXTR is now inline too (its gain is
   not measured yet) (`docs/OLD_CPU_ISA_PATH.md`). Tested by forcing the old-CPU path on
   a new CPU, not on real 2nd/3rd generation hardware. 3rd generation (Ivy Bridge) has F16C
   and only uses the MOVBE/BMI1/LZCNT rewrite.
4. **Bug reports without a log (#116):** "Export Bug Report (.zip)" used only the
   launcher's log window, empty after reopening the launcher. It now includes
   `user/last_run.log` and the port version.

## Diagnostics

- `THEORDER_PERMUTATION_LOG=1` names the closest existing shader permutation when a new
  one is compiled. With it, the stalls that come back after a restart (#113) were traced
  to new material-shader variants, not to lost cache entries
  (`docs/PIPELINE_SESSION_REBUILDS.md`).
- Opt-in: `THEORDER_READBACK_TICK_LOG=1`, `THEORDER_PENDING_WAIT_LOG=1`,
  `THEORDER_ISA_REWRITE=MOVBE+BMI1+LZCNT` to exercise an older CPU's path. A value of
  `THEORDER_ISA_REWRITE` other than 0, 1 or a set list now stops at start.
- Always on: an FSR 4 context check before destruction (an open boot fault) that only
  prints when the object changed; it does not change behavior.

## Known open items

Reported on the v0.8.0 build during this release's testing and not fixed here:

- **Flickering objects and decals** (for example a wall poster that appears every other
  frame in Whitechapel). Present since at least v0.7.9; cause not found yet. A first
  measurement pointed at `stale_readback`, but a 3 + 3 repeat did not confirm it.
- **Stutters on first visits.** A fresh install compiles pipelines while you play
  (1374 compiled in one session); a second visit is smoother. New material-shader
  variants keep appearing each session (#113).
- **Grenade smoke missing.** Not reproduced yet.
- **Subtitles do not show** in cutscenes with dialogue.
- #117 (crash at `theorder-probe.exe+0x126ce4`) is the WaitGraphics crash fixed above.

- Rare boot faults on this test PC: FSR 4 context teardown at the first frame (2 in 50
  runs) and an NVIDIA driver fault in the presenter (1 in 50); one new guest fault seen
  once (`0xc00ed7`).
- Stalls on first visits and new material combinations remain (#113, #114, #68).
