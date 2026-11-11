# The Order: 1886 PC Port - Windows v0.8.2

A robustness release on top of v0.8.1, from a side-by-side comparison with the two other
public ports of the game (TOport by IgorAugustoHH and the DivinusX Linux package). It fixes a
black screen in Portuguese on editions without Portuguese assets, makes a damaged shader cache
harmless, adds a guard against Windows freezing when system memory runs out, and fixes a
latent stack bug in the shader resource-table walker. Tested on one PC (RTX, 32 GB) with the
CUSA00035 01.02 dump on the prologue route; no open issue is claimed resolved before the
reporter's retest.

## Fixes

1. **Portuguese stayed on a black screen before the menu on CUSA00035 (and possibly
   CUSA00076/00070):** the game opens a localization package the dump does not ship
   (`1cf8f1244dcc1dd4` for PT-PT, `b0b4572d23e56916` for PT-BR), logs `Encountered an
   error` and never shows the menu. The launcher now checks the dump: PT-BR falls back to
   PT-PT when that package exists, otherwise the game starts in English, and the log says
   so (`Language: this dump has no package for engine language ...`). The check only runs on
   the editions without PT-BR assets (CUSA00035/00076/00070) and only when their package
   folder is where it was measured; anywhere else the chosen language is kept. Verified on
   CUSA00035 (Portuguese now reaches the menu and the prologue, in English); CUSA00076/00070
   are untested.
2. **A damaged shader cache no longer ends the game at start.** A truncated cache entry
   (disk full, interrupted write) used to abort; a corrupted count could also grow memory
   without limit. Such an entry is now skipped (`Shader cache: damaged meta ... skipped`) and
   that shader is translated again. Idea from TOport.
3. **Shader resource-table walker:** when the offset of a nested table could not be
   computed, the generated walker kept an unmatched `push`, leaving its stack unbalanced;
   the failed code is now rolled back. A lookup that cannot be resolved and feeds a texture,
   sampler or buffer descriptor now reads as a null descriptor instead of a piece of a
   pointer. On the prologue every unresolved lookup (2816 in 100 s, shader cache off) was
   plain data, so no visible change is expected there. `THEORDER_SRT_GARBAGE=1` restores the
   old walker; `THEORDER_SRT_LOG=1` counts unresolved lookups.
   From TOport.

## New

- **RAM guard:** when free physical memory stays under 700 MiB for one second, the game
  stops with `STOP: system RAM almost exhausted` in the log instead of letting Windows stop
  responding (16 GB machines). Free memory is also logged every 30 s.
  `THEORDER_RAM_GUARD_MB=N` changes the limit, `0` turns it off. From TOport.
- Opt-in, defaults unchanged:
  - `THEORDER_PENDING_DRAWS=skip`: never wait for a pipeline that is still compiling
    (TOport's behaviour). Removes the 100-300 ms first-visit hitches at the cost of the
    first-visit flicker the wait was added for.
  - `THEORDER_DLSS_PRESET=N`: NGX render preset (TOport uses 11 = K, 6 = F). Not checked on
    an RTX card yet.

## Measured, not fixed yet

- **Stutters after a restart (#113):** on the same route TOport had 0 five-second windows
  with a frame over 100 ms in its second session, this port 7. Saved pipelines are reused;
  the hitches are about 40 pipeline combinations per session that never existed before,
  each a ~160 ms driver compile. TOport meets about 8. The cause of the extra combinations is
  being investigated (`docs/plans/2026-11-11-reference-ports-review.md`).
- With pipeline binaries on (the default), the driver pipeline cache that is saved is empty
  (60 bytes). `THEORDER_PIPELINE_BINARIES=0` fills it (20 MB) but did not make new
  combinations faster on this NVIDIA driver.

## Known open items

Unchanged from v0.8.1: flickering objects and decals, first-visit stutters, and missing
grenade smoke in The Atrium (also present in v0.7.9). Subtitles are being traced in the
game's UI layer.
