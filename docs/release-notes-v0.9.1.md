# The Order: 1886 PC Port - Windows v0.9.1

A small release about safety and tooling. Nothing in rendering, sound or timing changed since v0.9.0, so no open issue is expected to behave differently in the game.

How it was tested:
- One PC (RTX, 32 GB), the CUSA00035 01.02 dump.
- Every change has automated tests and a two-axis review, and passed the `gate` check.
- The package was built, tested and opened cold and warm on the real game by `tools/release.sh`.
- The updater was run on real packages before publishing: v0.9.0 to this build, once kept and once undone (see below).

## New

- **Safer updates from the launcher.** These apply to updates made *from* v0.9.1 on; the update from v0.9.0 to v0.9.1 is still done by v0.9.0's updater.
  - The download is checked against the release's SHA-256 file. An update that cannot be checked is cancelled.
  - The files an update replaces are first copied to `backup-<version>` inside the port folder.
  - If the update fails, or the new launcher does not open, the previous version is put back automatically. If the PC or the launcher dies in the middle, the next start finishes or undoes the update.
  - The backup is deleted once the new version has opened.
  - Your files are never touched: `user`, `out`, `mods`, `theorder.ini`, `order1886.ini`, `settings.json`, the mod and patch lists and `last_run.log`.
  - An update is refused, with a message, when there is not enough free disk space or when the release ships a different Python runtime. In that case, download the zip from the releases page and extract it to a new folder.
- **Start the game in a chosen level (source builds).** `python run.py --level <name>` now really starts in that level, with `--checkpoint <name>` as an option.
  - The retail game ignores its command line; a new game patch, *Command Line - Game Arguments*, makes it read it. `run.py` turns the patch on by itself when `--level` is given.
  - Levels need their full name, for example `wch_020_slums`. The 83 names are in `docs/ENGINE_RE.md`.
  - Checked on four levels: `wch_020_slums`, `wch_080_ambush`, `may_040_atrium`, `bwy_010_tunnel`. The others were not started.
  - This is a tool for testing and for reporting problems at a given scene. A level started this way has no save behind it.
  - The patch is off by default and shows in the launcher's patch list. Leave it off for normal play: on its own it changes nothing.

## For contributors and testers

- **Packages built by CI.** The *Windows package* workflow builds the zip on a clean runner, opens it without MSYS2, and can publish a test build for one issue (`windows-v<version>-issue<N>-test<M>`).
  - A CI test build ships a different Python runtime than a release, so the launcher will not update a release in place to it (and the launcher only offers releases anyway). Extract a test build to its own folder.
- **GPU tests in the `gate` check**, on a software Vulkan driver (Mesa lavapipe). Two device tests are excluded there and named in `tools/run_tests.sh`.
- **Settings catalog.** `scripts/settings_catalog.py` lists every `theorder.ini` key with its default in the game and in the launcher; a test fails when the launcher writes a value the game does not parse. `recomplauncher.json` is generated from it.
- **Baseline tool.** `tools/bench/baseline.py` measures the scenarios cold, then warm twice, and compares two builds. The run-to-run noise measured on v0.9.0 is in `docs/baselines/`; the full baseline is still to be recorded.
- **Shader cache across GPUs and drivers:** checked, with a new test. The cache was already discarded when the GPU profile changes; nothing changed in behaviour.
- **Plan:** `docs/plans/2026-11-12-master-plan/`.

## Known open items

- **#138:** the temporal upscaler (FSR, TAA, DLSS) still does not run on CUSA00035 01.02. Its inputs are now identified (`docs/TEMPORAL_INPUTS_01_02.md`); the renderer change comes next. Frame generation stays inactive until then.
- **Unchanged from v0.9.0:**
  - flickering objects and decals;
  - first-visit stutters in areas not visited before;
  - missing grenade smoke in The Atrium;
  - black screen in gameplay/FMV since 0.7 (#98, #116);
  - Chapter 3 crash after the door (#118);
  - green/blue artifacts (#63).
  - See `docs/STATUS.md`.
