# Release review: Windows v0.9.1

Scope: every change since `windows-v0.9.0`, PRs #143-#154 plus the baseline PR, reviewed on two axes. **Standards** were checked against AGENTS.md, CONTRIBUTING.md and the ponytail rules. **Spec** was checked against the master plan (`docs/plans/2026-11-12-master-plan/`), the issues and the release notes.

## Per-PR reviews

Each PR had its own two-axis review before merging.

The defects those reviews found were fixed before the merge:

| PR | Defects fixed |
|---|---|
| #144 | 21 findings in two passes: a swap that was not crash-safe, recovery of an installed update, checksum and path handling; a rollback that skipped files with the same size and time (it now compares content) |
| #153 | the first, static conclusion (only the level name was wrong) was refuted by runs on the game and redone: the retail game ignores its command line. In review: the fixed options are kept when the patch is chosen by name; unverified options are left out with a level |
| #149 | lavapipe was not loaded on the runner (registry, not environment); the two excluded device tests are named |
| baseline | the tool stopped on an empty frame trace and lost the summary (a run ended by the RAM guard); the benchmark settings ran the game at 30 FPS (`engine_fps=60` is the original 30 FPS) |

PRs #145, #146, #148, #150, #151 and #154 and the documents of #147 had no defect left open at merge.

## Release pass

- **Code shipped in the package** (diff `windows-v0.9.0..master` outside docs, tests, tools and CI): `launcher/port_update.py` and its use in the launcher, `run.py` (level start), the new game patch (off by default), `profile_blob.h` (the profile comparison moved to a function, same behaviour), the package manifest. No defect that blocks the release.
- **Rendering, sound, timing:** unchanged. No A/B was needed.
- **Cross-PR consistency:** checked and OK.
  - Every new test is in `tools/run_tests.sh`; the byte check of the new patch is skipped in CI, where there is no game executable.
  - The new patch is `isEnabled="false"`; started from the launcher with the patch on and no level, `run.py` passes the game's own fixed options, so the game starts as before.
- **Leftovers:** no debug output, TODOs or stray files.

Findings, all addressed in this release PR:
1. **Version bump:** `launcher/port_launcher_win.py` and its test.
2. **READMEs (en, pt-BR, es):** they still said `--level` stops at the title screen. Corrected.
3. **Release notes:**
   - the safer updater only acts on updates made from v0.9.1 on;
   - only four of the 83 levels were started;
   - the cache check changed no behaviour;
   - a CI test build cannot be swapped in place over a release (different Python runtime: 3.12 on the runner, 3.14 in the release). Found in this pass, when the updater refused that pair of real packages, as designed.
4. **docs/STATUS.md:** the last release, #118 and #138 states, the work in progress.

Optional, not done:
- The CI test-build text does not say "extract to its own folder".
- `regression_report.py` fails when the report and the repository are on different drives (worked around with `--report-dir`).
- The five defaults where the game and the launcher disagree (`upscaler`, `preset`, `show_fps`, `frames_queued`, `live_resolution`) wait for the maintainer's decision; behaviour is the same as v0.9.0.

## Updater on real packages

The automated tests use synthetic trees. Before publishing, `launcher/port_update.py` is run on the real v0.9.0 zip and the v0.9.1 zip built by `tools/release.sh` (`DRY_RUN=1`):
- apply, then commit: the installed tree must equal the new package, the player files must be unchanged, backup and journal gone;
- apply, then rollback: the tree must be identical to v0.9.0 with the player files.

The release is not published if either fails. The launcher's own update button (download, restart, watch) is not covered by this run; it is first used by players on the update after v0.9.1.

## Risks stated in the notes

- **Updater:** new code on the update path, first exercised by players on the next release.
- **Game patch:** off by default; a level started by command line has no save behind it.

## Gates

- The `gate` check passed on every PR, now with the GPU tests on a software Vulkan driver.
- `tools/release.sh` reruns the build, the full test suite and the package smoke, cold and warm, on the real game.

Decision: release
