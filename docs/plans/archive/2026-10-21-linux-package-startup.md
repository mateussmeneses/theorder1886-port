# Include the Linux package's Python entry point

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/2

## Evidence and scope

`packaging/appimage.sh` builds `packaging/default.nix`. That derivation filters
the checkout before installing it. Both the source allowlist and `installPhase`
include `run.sh` but omit `run.py`. The installed `run.sh` executes `run.py`, so
the package cannot reach game preparation through this entry point.

This change addresses that packaging blocker. Official Linux support, the
package's remaining dependencies, Steam Deck compatibility, and gameplay remain
unverified.

## Implementation

1. Include `run.py` in the derivation's source allowlist.
2. Install it beside `run.sh`, preserving the directory layout expected by both.
3. Exercise the installation commands in an isolated package fixture and launch
   its real `run.sh` with isolated data and no game dump. Check that startup
   reaches the missing-game check instead of failing to find `run.py`.
4. Run applicable Python tests, attempt the repository validation gate, and have
   an independent agent review the diff before opening a PR against `master`.

## Acceptance

The package's selected source and installed files contain `run.py`. The fixture
entry point reaches Python startup/game validation without a missing-entry-file
error. Report fixture checks separately from a real Nix/AppImage build, which
requires Nix and prebuilt game binaries.

## Validation results

- The isolated fixture reproduced the original failure: Python could not open
  the installed `run.py`. With the change, the selected source and installed
  package both contain it, the real `run.sh` reaches the expected `No eboot.bin`
  message, and startup creates graphics state in the isolated data directory.
- The fixture interprets the recipe's literal source list and executes its
  installation shell. It substitutes probe binaries and an empty generated
  shader directory; it does not evaluate Nix, run the wrapper, or exercise a GPU.
- `bash tools/run_tests.sh` cannot build: `ninja: command not found`. Nix and
  prebuilt game binaries are also unavailable, so a real Nix/AppImage build
  remains pending.
- Python discovery: 292 tests, 214 passed, 76 skipped, and the same two existing
  errors caused by missing `tkinter` in `test_launcher_game_files`.
- Independent GPT-6.1 Sol review found no actionable issues and reran the fixture
  successfully. `git diff --check` passed.
