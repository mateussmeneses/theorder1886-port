# Preserve 32:9 ultrawide settings — issue #27

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/27

## Diagnosis

The launcher and Python startup code already accept `32:9`, and the 01.02 XML
contains its aspect-ratio patch. However, the C++ graphics table omits `32:9`.
Loading it falls back to `original`, the in-game aspect dropdown omits it, and
saving settings loses the requested ratio. The presenter selects its picture
rectangle using that same C++ table. This matches the issue's follow-up report
about missing in-game choices and a cropped image.

The C++ output resolution table also omits the requested `5120x1440`, despite
the launcher listing it. Reading and saving that key loses the requested size.

## Implementation plan

1. Append `32:9` to the aspect keys, names, and numeric ratios without changing
   existing indices. Replace the fixed aspect save clamp with the table bound.
2. Append `5120x1440` to the C++ output sizes, preserving existing indices and
   the default. Keep the existing renderer and launcher behavior otherwise.
3. Extend the native graphics-settings test to cover valid 32:9 parsing, a
   centered 1920x540 picture inside the 1920x1080 buffer, a full picture at
   5120x1440, settings load/save/reload, and aspect restart detection. Use an
   actually invalid ratio for the existing fallback test. Allow the existing
   test to set its environment on Linux as well as Windows.
4. Add focused Python coverage that startup accepts 32:9 and selects the
   existing built-in patch; run focused native/Python tests and compare the
   full Python suite with the unchanged baseline.
5. Obtain independent review, commit, publish the new branch from upstream
   `master`, and open a PR against upstream `master` referencing #27.

## Acceptance and limits

- The in-game dropdown's data includes 32:9; parsing and saving retain it.
- The presenter obtains the 32:9 picture rectangle from the same table.
- `5120x1440` survives C++ settings loading, saving, and reloading.
- Existing aspect/output indices and invalid-value fallback remain covered.
- This branch is independent of PR #48; no keyboard changes are included.
- Do not alter game executable patch bytes or enable parked upscaler controls.
- Native tests prove settings and picture geometry; game patch application,
  HUD layout, and visual correctness on a real 32:9 display still need Windows
  gameplay with a compatible game dump. Reference #27 without automatic closure.

## Validation results

- Native `graphics-settings-test` compiled with
  `g++ -std=c++23 -O2 -g -UNDEBUG -Wall -Wextra -Werror -Igpu/shim`
  using `tests/test_graphics_settings.cpp` and `gpu/shim/port_settings.cpp`.
  The new 32:9 assertion failed before the production change and passed after
  it. Geometry, defaults, restart detection, and load/save/reload checks passed.
- Native `upscaler-support-test`, rebuilt against the changed settings module
  with the same compiler flags: passed.
- Python focused checks: graphics options 9 passed; launcher controls 25 passed;
  patches 15 passed and 2 skipped because the prepared game executable is absent.
- Full Python suite: 293 tests, 215 passed, 76 skipped, and 2 errors caused by
  missing `tkinter`. The unchanged upstream baseline had the same two errors
  and 76 skips across 292 tests.
- `bash tools/run_tests.sh`: unavailable because `ninja` is missing; no
  configured `out/gpu` build is present. Full C/C++ and Windows visual checks
  remain pending.
- `git diff --check`: passed.
- Independent review found no actionable issues, traced the menu/presenter and
  output dimensions through the updated tables, and reran the native graphics
  test successfully in a temporary directory.

The 5120x1440 output is only honored when explicitly configured; the default
remains 1920x1080. A larger configured output can increase GPU memory use and
rendering cost. No upscaler controls are re-enabled by this change.
