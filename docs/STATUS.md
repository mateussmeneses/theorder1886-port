# Project status

Single current-status page (AGENTS.md "Documentation"). Update it in the PR that changes the
status. History: `docs/PROGRESS.md`; releases: `docs/release-notes-v*.md`.

Last updated: 2026-11-11, after Windows v0.8.3.

## Last release

**Windows v0.8.3** (`windows-v0.8.3`): far fewer shader/pipeline compiles after the first
session (#113, waiting for retest). v0.8.2: Portuguese black screen on editions without its
language package, damaged shader cache, RAM guard, SRT walker stack fix.

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
| Chapter 3 crash after the door | #118 | report attached, not read yet |
| Saved driver pipeline cache empty with pipeline binaries on | #102 | measured (60 bytes); no faster new pipelines when filled |
| Green/blue artifacts | #63, #39 | needs logs |

## Being worked on

- SpartanPort adoption (`docs/plans/2026-11-11-spartanport-adoption.md`): phases 1 (runtime and
  memory, #125-#129) and 2 (GPU/Vulkan, #130-#131, #133-#134; window #132) merged; phase 3
  (audio, #135) merged; phase 4 (menu): pause and Game tab in review, translations next. Then
  upscaling/frame generation, input, launcher.

- Comparison with the other public ports (TOport, DivinusX package), scene by scene:
  `docs/plans/2026-11-11-reference-ports-review.md`. Next: flicker A/B on the same scene in
  both ports.

## Process

Branches + PRs with the required `gate` check; two-axis review per PR; releases with
`tools/release.sh`; issue labels `needs:log`, `status:investigating`, `status:needs-retest`,
`status:fixed-in-release` (AGENTS.md).
