# Expose the supported mouse sensitivity range — issue #37

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/37

## Diagnosis and scope

The Controls tab passes a 10–300% range to the mouse sensitivity slider.
The Image tab and settings parser already accept 10–400%. `SliderRow` uses
`ImGuiSliderFlags_AlwaysClamp` and stores the result clamped to the supplied
bounds, so a player using Controls cannot choose 301–400%, including the 400%
workaround described in the issue's comments.

This is a confirmed UI range mismatch. It does not explain all mouse response
or aiming behavior: players can already reach 400% through Image, and the
issue's comment says that this can still feel slow. Preserve the existing
mouse-to-stick curve, smoothing, defaults, and gamepad behavior.

## Implementation plan

1. Create an independent branch from upstream `master` and change only the
   Controls tab's upper sensitivity bound from 300 to 400.
2. Inspect both slider call sites and the shared clamping helper. Verify with
   the real settings module that the default remains 100%, 400% survives
   load/save/reload, and values outside 10–400% retain the existing clamping.
3. Run the available Python suite and report pre-existing errors. Full overlay
   compilation and Windows/gameplay checks require dependencies absent here.
   For this one-literal UI correction, use focused verification and independent
   source review rather than add a permanent test tied to source text.
4. Obtain independent review, commit, publish the branch, and open a PR against
   upstream `master` referencing #37 without automatically closing it.

## Acceptance and limits

- Both Image and Controls expose the supported 10–400% sensitivity range.
- Settings keep the same 100% default, 10–400% limits, and persistence.
- Production changes are limited to one slider bound; no unrelated refactor.
- This branch excludes the changes in PRs #48, #49, and #50.
- In-game interaction, camera responsiveness, and aim feel still need Windows
  gameplay validation. Do not claim that the broader mouse report is resolved.

## Validation results

- Source inspection: Image and Controls now both pass 10–400 to `SliderRow`;
  the settings parser still clamps to 10–400 and the default remains 100.
- Standalone verification against the real `port_settings.cpp`, compiled with
  `g++ -std=c++23 -O2 -g -UNDEBUG -Wall -Wextra -Werror -Igpu/shim`: passed
  default 100, limits for -20/10/100/300/400/999, and 400 load/save/reload.
  This checks existing backend behavior; it does not exercise the ImGui widget.
  The temporary verification source and log are in ignored `out/`.
- Full Python suite: 292 tests, 214 passed, 76 skipped, 2 existing errors from
  missing `tkinter`, matching the unchanged upstream baseline recorded for the
  preceding tasks against the same upstream commit and environment.
- Full overlay/C++ build unavailable: `ninja` and the initialized ImGui
  submodule/configured GPU build are absent. No Windows gameplay was run.
- `git diff --check`: passed.
- Independent review found no actionable issues, checked the slider/storage
  wiring, and successfully reran the standalone settings verification.
