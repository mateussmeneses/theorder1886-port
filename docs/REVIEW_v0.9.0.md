# Release review: Windows v0.9.0

Scope: every change since `windows-v0.8.3`, PRs #119-#141, reviewed on two axes. **Standards** were checked against AGENTS.md, CONTRIBUTING.md and the ponytail rules. **Spec** was checked against the adoption plan, the issues and the release notes.

## Per-PR reviews

Each PR had its own two-axis review before merging.

The defects those reviews found were fixed before the merge:

| PR | Defects fixed |
|---|---|
| #133 | a refused compute pipeline bound as `VK_NULL_HANDLE`; a GC pressure floor that had no effect |
| #134 | the test and activation evidence for guarded descriptor reads |
| #135 | stereo port state without a stream; a non-numeric volume muted the game; the dummy-driver test |
| #136 | vblanks frozen during the pause; no menu before the first frame; exit holding the menu lock |
| #137 | 24 translation fixes; a stricter printf-conversion check |
| #139 | the experimental label and honest menu text; the runtime test skips on a GPU without the feature |
| #140 | a wrong "already unlocked" code (it now answers 0, as before); an atomic trophy save; an overlay race |

## Release pass

- **Code:** no defect that blocks the release.
- **Cross-PR consistency:** checked and OK.
  - Every new `theorder.ini` key is parsed, saved, and has an environment variable.
  - The launcher defaults parse in the game.
  - Every new test is in `tools/run_tests.sh`, with the GPU tests in the Vulkan list.
  - Every menu text has a translation key, and the tests enforce it.
- **Leftovers:** no debug output, TODOs or stray files.

Findings, all addressed in this release PR:
1. **Version bump:** `launcher/port_launcher_win.py` and its test.
2. **Release notes:**
   - "fixed" → "ported, not seen failing in this game" for the SpartanPort correctness fixes;
   - the surround evidence is one session's level log;
   - speakers apply on restart;
   - the pause does not apply on loading screens;
   - trophies wait for a retest (only the preview was seen);
   - the pad map takes `|` too;
   - the launcher texts are translated into Portuguese only;
   - DLSS is named in #138;
   - the other open problems are listed;
   - user-relevant memory fixes added: the small page file (error 1450/1455), two freezes, EBUSY, and the resident cost of zeroed released pages.
3. **docs/STATUS.md:** the last release and the work in progress.

Optional, not done:
- README.pt-BR/es don't mention the Game tab, trophies and `|` maps.
- The launcher's Speakers list shows `2`/`6`/`8` as unknown; the game rewrites them on its next save.

## Risks stated in the notes

- **Pause:** on by default.
- **Trophy state:** now reported to the game.
- **Frame generation:** experimental, off by default; it adds swapchain images only when on.
- **#138:** the temporal upscaler is inactive on CUSA00035 01.02. This predates the release.

## Gates

- The `gate` check passed on every PR.
- `tools/release.sh` reruns the build, the full test suite (71 native tests plus the Python suite) and the package smoke, cold and warm.

Decision: release
