# O7.5 checkpoint — 2026-10-23

- Source HEAD/master: `d5534c570f5de681596d0f4f3963bb915db676be`; branch `codex/o7-gpu-wait-diagnostics`; prior work preserved in [history](STABILIZATION_HISTORY.md).
- Cause measured: visible indirect draws bypass preparation and block in Vulkan pipeline creation; O7.4 had 37 waits >50 ms, all `visible_indirect/no_draw`, with 1.1 ms total queue delay.
- Concrete defect: `DrawIndirect` omitted `is_indirect`, selecting direct vertex/instance-ID semantics; `RuntimeInfo` and serialized specializations already distinguish the bit.
- Fix: `DrawIndirectParams::Indirect()` retains PM4 SGPR offsets and sets the mode; no key fields, essential waits, or draw policy are removed.
- Tests: factory regression failed with the old false bit, then passed; stage identity and actual translator IR checks cover direct/indirect separation and base-offset subtraction.
- Build/checks: CLANG64 `THEORDER_LTO=OFF bash build.sh`, `bash tools/run_tests.sh`, `git diff --check`; build and 48 C/C++ targets passed; Python ran 323 tests, 39 skipped.
- Tested executable SHA-256: `fe73508844a72aa96555ca5669d15c60653851e06baa3364fd1bd3b35f606ddc` (dirty source build).
- Fresh isolated smoke: RTX 5060 Laptop, CUSA00035 01.02, 125-second limit; exit 0, 0 reported faults/hangs; inspected title and prologue cinematic captures.
- Trace: 6,177 intervals; 272 visible pipeline waits, 80 >50 ms, 361.97 ms max; creation sum 19,285.6 ms versus 3.4 ms queue delay. This is not a stutter gain.
- Reproduction and evidence: [validation](STABILIZATION_VALIDATION.md); local `out/o7-indirect-visual-resume/manifest.json`, report, captures and CSV. Old interrupted true-mode run ended before capture and is excluded.
- A/B gate: timed inputs do not establish identical scene/camera; combat, free movement, draw completeness, AMD and solar shadows remain NOT VALIDATED. No version ranking or default configuration change.
- Next O7.6: establish a matched static gameplay checkpoint at 30 FPS/native; then investigate indirect prequeue with full SGPR/resource snapshots and bounded jobs, preserving all specializations.
- Publication authorized by user after completion: working branch and PR with limitations; no release, automatic issue closure or master replacement.
