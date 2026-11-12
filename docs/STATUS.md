# Project status

Single current-status page (AGENTS.md "Documentation"). Update it in the PR that changes the
status. History: `docs/PROGRESS.md`; releases: `docs/release-notes-v*.md`.

Last updated: 2026-11-12, Windows v0.9.1.

## Last release

**Windows v0.9.1** (`windows-v0.9.1`): safer updates (checksum, backup, automatic restore), start
in a chosen level with `run.py --level`, packages also built and opened by CI
(`docs/release-notes-v0.9.1.md`). v0.9.0: SpartanPort adoption (`docs/release-notes-v0.9.0.md`):
menu pause, Game tab (volume, speakers, Exit to Desktop), menu translations, 5.1/7.1 audio,
trophies kept and shown (waiting for retest), alternative keys, runtime/memory/GPU fixes;
experimental frame generation, inactive until #138. v0.8.3: far fewer shader/pipeline compiles
after the first session (#113, waiting for retest).

## Works (one PC, RTX, CUSA00035 01.02, prologue route)

- Boot, menu, prologue and its cutscenes at 60 FPS; subtitles (reported by the user).
- Second and later sessions: 0-1 five-second windows with a frame over 100 ms on the prologue
  route (v0.8.3; TOport, the reference port, 0).
- Package smoke (`tools/smoke_package.py`) cold and warm on every release.

## Open problems (by evidence)

| Problem | Issues | State |
|---|---|---|
| Flickering objects and decals | #95, #99, #30, #29 | none in the prologue cover tutorial in either port (`tools/flicker_scan.py`); needs a save at a reported scene. HUD text shimmers with the 60 FPS patch |
| Black screen in gameplay/FMV since 0.7 (menus visible) | #98, #116, #47, #24, #43 | needs logs on v0.8.3 |
| First-visit stutters in new areas | #114, #120, #68 | expected while pipelines compile once; measure per area |
| Missing grenade smoke (The Atrium) | — | reproduced on v0.7.9 and v0.8.x |
| Chapter 3 crash after the door | #118 | report read (`docs/ISSUE_EVIDENCE_2026-11.md`): out of memory after a GPU-command assert; not reproduced yet |
| Saved driver pipeline cache empty with pipeline binaries on | #102 | measured (60 bytes); no faster new pipelines when filled |
| Green/blue artifacts | #63, #39 | needs logs |
| Temporal upscaler (FSR/TAA/DLSS) never runs | #138 | inputs found and specified (`docs/TEMPORAL_INPUTS_01_02.md`); the renderer change is next |

## Being worked on

- Master plan of 2026-11-12 (`docs/plans/2026-11-12-master-plan/`). Done: issue evidence, CI
  package, software-Vulkan tests in the gate, transactional updater, settings catalog, level
  start, baseline tool and the v0.9.0 baseline, temporal-input spec. Next: the temporal upscaler
  (#138).

- SpartanPort adoption (`docs/plans/2026-11-11-spartanport-adoption.md`): phases 1-7 done
  (#125-#141). Open: #138 (temporal upscaler inactive on CUSA00035 01.02; frame generation and
  the upscaler items wait on it); retests of #99/#63 and of trophies.

- Comparison with the other public ports (TOport, DivinusX package), scene by scene:
  `docs/plans/2026-11-11-reference-ports-review.md`. Next: flicker A/B on the same scene in
  both ports.

## Process

Branches + PRs with the required `gate` check; two-axis review per PR; releases with
`tools/release.sh`; issue labels `needs:log`, `status:investigating`, `status:needs-retest`,
`status:fixed-in-release` (AGENTS.md).
