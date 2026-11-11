# Keyboard Options mapping — issue #38

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/38

## Diagnosis and scope

The launcher advertises `Options → Return`, and the runtime's layout comment says
the same, but the runtime maps Return to Cross. Selecting the default in the
launcher therefore leaves Options unavailable by default; explicitly adding
`options=Return` also sends Cross through the old default binding.

This is the smallest deterministic issue found in the open-issue triage: #27
already has 32:9/5120×1440 support, #28 depends on language assets, and #34 needs
new visual assets. Renderer and performance reports need game/hardware evidence.

## Implementation plan

1. Change only Return's default runtime binding to `BTN_OPTIONS`. Keep Space as
   Cross and preserve gamepad, touchpad, and overlay mappings.
2. Extend native map tests to cover Return sending only Options, Space sending
   Cross, explicit/default Options bindings, and remapping Options to another
   key. Add launcher regression coverage for its matching defaults and saved
   Options remapping reaching `THEORDER_KEY_MAP`.
3. Update the controls section of all three READMEs: Space is Cross, Enter is
   Options, and remove the stale statement that keyboard Options is unavailable.
4. Run focused regression checks and the available suite, review the diff with
   an independent agent, then commit and open a PR against upstream `master`
   linking #38. Use a fork because the authenticated account has read access
   to the upstream repository.

## Acceptance and validation limits

- Default Return emits Options without Cross; Space still emits Cross.
- Remapping Options removes Return's default Options binding.
- Explicit `options=Return` does not introduce a simultaneous Cross press.
- Existing gamepad and touchpad mappings remain covered by native map tests.
- Launcher defaults and environment serialization agree with the runtime.
- Record unavailable checks and any pre-existing suite failures in the PR.
- Test the default and remapped Options keys during gameplay on Windows before
  claiming the full pause-menu report is resolved. This environment has no game
  dump or Windows runtime; native map tests prove the host input correction.

## Results

- Native `pad-map-test`: the new Return assertion failed before the runtime
  change and passed after it. Compiled with `-Wall -Wextra -Werror`, SDL3
  headers, and the locally available SDL3 library, without the renderer. The
  map-only test now supplies the missing mouse-input GPU stub for standalone
  linking.
- `python3 -m unittest discover -s tests -p 'test_launcher_controls.py' -v`:
  27 tests passed (25 existing, 2 added).
- `python3 -m unittest discover -s tests`: 294 tests, 216 passed, 76 skipped,
  2 errors from missing `tkinter`. The unchanged baseline had the same two
  errors and 76 skips across 292 tests.
- `bash tools/run_tests.sh`: unavailable because `ninja` is missing and this
  checkout has no configured `out/gpu` build. Full C/C++ and Windows/gameplay
  validation remain pending.
- `git diff --check`: passed. Independent review found no actionable code
  issues and independently reran the focused native and Python checks.
- The PR references #38 without automatically closing it: the Options mapping
  is corrected, but the complete in-game pause-menu report remains unverified.
