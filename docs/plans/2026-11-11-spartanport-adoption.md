# SpartanPort adoption plan (2026-11-11)

Source: FelipePrado0/SpartanPort @ 6a70960 (launcher 3.1.0), the God of War III port of the same
BBPort/shadPS4 lineage. This port split from it around 2026-10-14/16 (`90a0060`, "rename the
port's gow3 identifiers"); SpartanPort has 872 commits since, about 250 of them in the launcher.
License GPL-2.0, compatible; keep its attribution in each ported file and commit.

## Method

- Environment-variable inventory: 207 `SPARTAN_*`/`GOW3_*` names, 70 with no equivalent here.
- README feature table, docs, launcher sources (Python 8.5k lines, Rust 68k lines).
- Every non-launcher `Feat/Fix/Perf` commit since 2026-10-15 checked for presence here by its most
  distinctive added lines, normalized for the renames (`scratchpad/tools/presence.py`): of 120,
  2 present, 23 partial, 89 missing, 6 not checkable. A rewritten port can hide from this check,
  so every item below is confirmed by reading the code before it is ported.

## What applies to The Order

### A. Guest runtime and memory (game-agnostic correctness; first)

| Commit | Change | Why it matters here |
|---|---|---|
| 1fe20cd | guest threads start with the PS4 floating-point state | wrong MXCSR/x87 state changes guest math (physics, animation) |
| 9e8a080 | guest libc mutex flags kept out of host pthread memory | memory corruption under contention |
| c8e561a | guest thread and lock handles below 1 TiB | the guest packs pointers into 40 bits |
| 2c1f52f | EBUSY when the guest destroys a held mutex | PS4 semantics |
| ed3af5b, e650a33, 6f5aa50, 63d437e, e82ca70 | flexible-mapping split offsets, unmap before fixed maps, GPU-side writes and upload copies through the backing view, region caches | memory map correctness; texture/shadow garbage candidates (#99, #63) |
| 863e49e, b6b35ac, 774ca0c | start when guest memory exceeds the commit limit, handle allocation failures, log failed reservations | the `error 1450` / commit-limit closes; the stalled 16:9 boot at 4.7 GB free commit |
| 21db7f2 | memory calls and draw preparation no longer wait on a blocked GPU thread | stalls and hangs |
| 201a00f, 6525a86 | found locks/threads moved to the front of their registries; SRW lock for the region table | CPU cost per frame |
| 55fcf07 | `SLOW_WAITS` log of slow guest waits | diagnostics (TOport has the same) |

### B. GPU / Vulkan correctness

| Commit | Change |
|---|---|
| e1594f6, bc78995 | prepared draws re-read stale vertex inputs and reselect the pipeline |
| 812190a, 23d457f | VRAM staging ring shrinks after area loads; requests stop probing every block (VRAM on 6-8 GB cards) |
| c459438 | oversized texture detiling dispatches batched |
| 26fb85a | MSAA blit pipelines built with the images' real sample counts |
| 1974453, 3a555df, 655b311, 00db4fb, 9ca82cb | texture cache: texture sets recorded only when stable, compute clears of another format rejected, mip clamp, padded partial-height views, alias copies of different widths (flicker/garbage candidates) |
| a94665c, 368d899 | stale guest descriptor tables read without crashing; empty shader input guarded |
| 0c3ec5d, 61e307d | nested indirect-buffer task frames freed; constant-engine counter waits (61e307d already here) |
| d6d1aea, 6f43d65, 7cdbb40 | swapchain kept across fullscreen focus loss; taskbar not over fullscreen; window opened at render size |
| c22329f (#1124), b776be7 (#386) | bundles of texture/GPU/shader-cache fixes and readback waits: split per fix when ported |
| Stream buffer memory option, automatic MSAA per vendor (README "GPU data") | NVIDIA-laptop FPS; black lighting lines on AMD/Intel |

### C. Audio

| Commit | Change |
|---|---|
| 1e95c7c | 5.1/7.1 output, master volume and a soft limiter (we only downmix 7.1 to stereo) |
| ad2784f | DirectSound fallback when WASAPI fails |
| fe9e7b0 | MP3 mid-stream resets that cut speech (we have `runtime_mp3.c`) |
| 9e9cd33 | SDL audio init serialized between AudioOut and another SDL user |

### D. Player features (generic engine/port side)

- Upscaling: FSR 4.1 (7280104), scaled presets with Ultra Quality (9f501f6), render resolution as
  upscaler output (9bd1fec), stencil reactive mask (8d3fca2, partial here), ghosting fixes on water,
  particles and DLAA edges (9fdc30c, 3ad12a7), black eyes at native size (bf9181b).
- Frame generation 2x, AMD FSR (8192bbb, 1812d35) and NVIDIA DLSS (580432d), with pacing fixes
  (eb82a67) and profiling (312728b). Issue #31 asks for it.
- In-game menu: Options > Graphics with live settings (2013147), display/controller/mouse pages
  (fd27fc3, cb2c5bd, 138f9e6), anisotropic filtering (d402ae6), **pause the game while the menu
  is open** (6853390), **Exit to Desktop** (a553618), render resolution shown (9128131), menu
  translations (`ui_language`, 11 languages).
- Input: full keyboard stick remap (6f59269), an alternative key per button (cbdbf29), Cross/Circle
  confirm swap (fefc68f), stick deadzones, vibration and DualSense options, controller diagrams in
  the pause menu (4ccf29d), R2/L2 kept from the game while the menu opens (6c0df98).
- Trophies: notifications with four themes and a sound, trophy list window (F8 / R3+R2): the
  trophy calls are the system's, so this is game-agnostic.
- Performance overlay: FPS, frame time, GPU, CPU, RAM, VRAM, layout options (we have a smaller one).
- Discord rich presence; monitor and GPU selection; render resolution list (900p, 16:10, 4:3, 8K,
  ultrawide sizes); live engine frame-rate switching and 100/144/180/240 FPS engine rates (needs
  The Order's frame-rate patch to accept them: verify per value).

### E. Launcher

- The Rust launcher (iced, 68k lines) is the only one SpartanPort ships: no Python or PyInstaller
  in the package, GUI scale, i18n, controller mapper, trophies, save backups and restore, shadPS4
  save import, mods, update checker, Steam shortcut and art, Discord. Its `runner/` also
  reimplements our `run.py` preparation (eboot image, libc/module linking, ISA rewrite, game
  update merge, guest memory). Adopting it means adapting game-specific pages and keeping the
  runner in step with our `scripts/`, a multi-week effort.
- Python-launcher items that can come first on their own: controller mapper
  (`controller_mapper.py`), GUI scale, shadPS4 save import, save backup list with restore.

### F. Not applicable (God of War III specific)

Cheats and multipliers (orbs, health, magic, Rage of Sparta), automatic QTE sequences, enemy
health bar, free camera tuned to GoW, weapon cycling, lyre keys, voice replacement, `R_UI.WAD`
patch, CUSA01623/01715/01740 detection. The in-game menu framework behind them is reusable if The
Order cheats are reverse-engineered later.

### Effects toggles for The Order (issue #32)

SpartanPort's motion blur / depth of field / bloom switches (3598158, 5831938, eae407c) skip GoW's
own passes by shader hash. The same approach needs The Order's pass hashes (TOport had motion blur
and DoF boxes and removed them in v0.1.7): a reverse-engineering task, not a port.

## Plan (one PR per row; each PR: tests, activation evidence, smoke, two-axis review)

| Phase | Scope | Exit criterion |
|---|---|---|
| 0 | `git remote add spartan`, a rename map (gow3/spartan -> port/theorder) for cherry-picks, confirm each item by reading the code | items in A-C marked present/missing by hand |
| 1 | A (runtime and memory), grouped by subsystem (threads/locks, memory map, commit limit, perf) | suite green; prologue smoke cold/warm; batch_runs 3x A/B: no new faults, frame time not worse |
| 2 | B (GPU/Vulkan), texture-cache group first (flicker/garbage candidates #95/#99/#63) | same + flicker_scan on the cover tutorial unchanged; reporters' retest for #99/#63 |
| 3 | C (audio) | 5.1/7.1 and stereo devices; speech not cut (MP3) |
| 4 | D menu: pause while open, Exit to Desktop, Options > Graphics, translations | manual check in game; menu screenshots per language |
| 5 | D upscaling and frame generation | per-vendor test (we have NVIDIA here; AMD/Intel via reporters) |
| 6 | D input and trophies, overlay, Discord | in-game checks |
| 7 | E launcher: spike the Rust launcher on a branch (build, rebrand, run The Order through its runner); decide adopt vs. port pages to Python | a decision note with the measured gaps |

Releases bundle finished phases (v0.9.0 after phases 1-2).

## Progress (2026-11-11)

- Phase 1 merged: #125-#129.
- Phase 2 merged:
  - #130: texture cache.
  - #131: GPU/Vulkan.
  - #132: foreground window.
  - #133: the c22329f bundle, about 40% taken; the rest already existed here.
  - #134: guarded descriptor reads, a94665c.
  - Skipped: 368d899, b776be7, 7cdbb40, d6d1aea.
- Phase 3: #135 (fe9e7b0, ad2784f, the audio core of 1e95c7c). 9e9cd33 does not apply (no trophy sound).
  - The menu and launcher fields for `audio_channels` and `master_volume` come with phases 4 and 7.
- Phase 4 merged: #136 (pause while the menu is open, Game tab, Exit to Desktop; a553618 does not
  apply), #137 (menu in pt-BR, es, fr, de, it).
- Phase 5: frame generation (8192bbb, eb82a67, 137c8cc, 402c24c; DLSS FG removed upstream in 54cc763)
  ported, experimental. Blocked in game by #138: the temporal upscaler never runs on CUSA00035
  01.02 in the tested scenes, so no frame is captured. The other upscaler items (7280104, 9f501f6,
  9bd1fec, 8d3fca2, 9fdc30c, 3ad12a7, bf9181b) wait for #138 too.
- Pending checks:
  - reporters' retest for #99 and #63;
  - runtime-test hangs on Windows and stays out of the gate;
  - no regression tests yet for 1974453 and 3a555df.
