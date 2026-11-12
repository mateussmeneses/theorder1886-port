# The Order: 1886 PC Port - Windows v0.9.0

This release brings over the fixes and features of FelipePrado0/SpartanPort (a God of War III port on a related codebase) that apply to The Order: 1886, adapted to this port. The plan is in `docs/plans/2026-11-11-spartanport-adoption.md`.

How it was tested:
- One PC (RTX, 32 GB), the CUSA00035 01.02 dump, the prologue route (the cover tutorial and the opening).
- Every change has automated tests and a two-axis review.
- Changes to rendering or timing were compared run against run with the previous master: 3 sessions each, empty shader cache, the same commit checked in every session.
- No open issue is claimed resolved before its reporter retests.

## New in the game

- **The menu pauses the game.** While the port's menu is open (Insert / R3+L2), the game and its sound stop, and the last frame stays under the menu.
  - Not on loading screens.
  - Settings that change live (field of view, for example) show once the menu closes.
  - On by default; Game tab > *Pause the game* turns it off.
- **Game tab:**
  - master volume, 0-200%, with a soft limiter above 100%;
  - speakers: automatic, stereo, 5.1 or 7.1, applied on restart;
  - *Exit to Desktop*, with a confirmation.
- **The menu in Portuguese (Brazil), Spanish, French, German and Italian.** It follows Windows' display language, or Game tab > *Menu language*. The translations were written and reviewed by an AI; corrections are welcome.
- **The menu works with the d-pad and the arrow keys.** They used to jump from the tab bar straight to *Close*.
- **5.1 and 7.1 sound.** The game's 7.1 mix goes as it is to a 7.1 device, folded to a 5.1 device, and to stereo as before. On a 7.1 device, the port's level log in one session showed the game filling the center and surround channels.
- **DirectSound fallback.** Sound keeps working when WASAPI fails.
- **MP3 speech:** a stream reset no longer cuts speech. This is SpartanPort's fix (their #1101); its effect in The Order was not measured.
- **Trophies (waiting for a retest):**
  - Unlocks are kept in `<user dir>/trophies.dat` and reported back to the game. The user dir is `THEORDER_USER_DIR` or the port's user folder. Before, the game was always told nothing was unlocked.
  - The platinum is granted with the last trophy.
  - A notice shows each unlock with its icon, read from the game's own trophy file. Names are optional: `trophy_names.txt`.
  - In the game, only the notice was checked, with `THEORDER_TROPHY_PREVIEW`. No real unlock happened on the tested route.
- **Alternative inputs.** `THEORDER_KEY_MAP` and `THEORDER_PAD_MAP` give a button up to four keys or controller buttons, for example `cross=Space|G`.
- **Launcher:**
  - audio and pause settings on the Game page;
  - frame generation on the Performance page;
  - translated into Portuguese only; other launcher languages show these in English.

## Also fixed or ported from SpartanPort

These first two were reproduced as problems before the change:
- **Small page file:** the game starts on 8-16 GB PCs with a small page file (Windows error 1450/1455). Guest memory is committed as the game maps it, which replaces v0.8.3's fallback that shrank memory in 64 MiB steps.
- **Address range:** the game starts when another program already holds part of the PS4 address range.

The rest were ported but not seen failing in this game. A/B runs after each one found no regression: no faults, the same draws per frame and frame times. No speed-up is claimed.
- **Runtime:**
  - guest threads start with the console's FPU state;
  - handles stay below 1 TiB;
  - a destroyed held mutex returns EBUSY.
- **Memory:**
  - released memory reads back as zero; released pages stay resident until Windows trims them;
  - two freezes removed: memory calls and draw preparation no longer wait on a blocked GPU thread.
- **Textures and GPU:**
  - stale texture-set memos, wrong-format clears and mip clamps;
  - real MSAA sample counts;
  - fewer VRAM staging blocks kept;
  - evicted samplers destroyed only once the GPU is done with them;
  - pipelines the driver refuses are skipped instead of crashing;
  - the shader cache is flushed on exit;
  - stale guest descriptor tables read as empty instead of closing the game.
- **Window:** the game window may take the foreground from the launcher.

## Experimental: frame generation (AMD FSR 3.1, 2x)

The code is in and its GPU test runs, but **it does nothing in the game yet**. On CUSA00035 01.02, the port's temporal upscaler (FSR, TAA, DLSS) does not run in the tested scenes (#138), and frame generation needs the upscaled frame. Turning it on (Performance > *Frame generation*) only adds latency for now, so leave it off.

## Known open items

- **#138:** the temporal upscaler (FSR, TAA, DLSS) does not run on CUSA00035 01.02 in the tested scenes. This predates this release: choosing an upscaler has no effect there.
- **Unchanged from v0.8.3:**
  - flickering objects and decals;
  - first-visit stutters in areas not visited before;
  - missing grenade smoke in The Atrium;
  - black screen in gameplay/FMV since 0.7 (#98, #116);
  - Chapter 3 crash after the door (#118);
  - green/blue artifacts (#63).
  - See `docs/STATUS.md`.
- **Not taken from SpartanPort:**
  - its Rust launcher (`docs/plans/2026-11-12-launcher-decision.md`);
  - its trophy themes and list;
  - Discord;
  - its upscaler improvements, which wait on #138.
