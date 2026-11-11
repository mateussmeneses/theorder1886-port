# Native output resolution in the launcher (#35)

## Goal

Let players select a supported 16:9 output size (720p, 1080p, 1440p or 2160p) while temporal upscaling remains parked. With the upscaler off, the renderer uses the selected output size for its host scene targets and final UI composition. This provides a native 4K path independent of third-party Neo/PS4 Pro patches.

## Scope

- Add a separate experimental Output resolution selector to the launcher Graphics page. Use only the four sizes accepted by `PortSettings::OutputWidths/OutputHeights`; leave the broader `OUTPUTS` list for the aspect-ratio work untouched.
- Have `game_environment()` set `THEORDER_UPSCALER=off`, which `PortSettings::Load()` actually reads. It overrides an inherited environment value and a previously saved `upscaler` entry. Keep temporal upscaler controls hidden.
- Explain in the launcher that 4K raises GPU and memory cost and that some guest post-processing remains at its original resolution. Do not describe the NeoEnabler XML patch as supported.
- Keep default output at 1920x1080 and preserve other saved graphics settings.

## Validation

- Headless launcher test checks that `build_graphics()` offers exactly the four backend-supported sizes, independently of temporal controls.
- Save/load round trip checks the selected output resolution and preservation of unrelated INI keys.
- Environment test covers inherited `THEORDER_UPSCALER`, the old experimental switch, and saved upscaler values; `THEORDER_UPSCALER=off` reaches the C++ `PortSettings::Load()` path.
- Run focused Python tests and `bash tools/run_tests.sh`. Hardware gameplay capture is needed to confirm the visual result on a 4K display; automated checks establish the control and render-setting path only.

Validation in this worktree: focused `test_launcher_upscale.py` and `test_launcher_controls.py`
passed, as did a standalone build/run of `tests/test_upscaler_support.cpp` with
`gpu/shim/port_settings.cpp`. The full gate stopped before building because this host has no
`ninja`; Vulkan, SDL3 and FFmpeg development packages and Tkinter are also unavailable.

## Implementation order

1. Add the launcher selector and help text.
2. Set the recognized upscaler environment override.
3. Add behavior tests and run the validation gate.


## Results

Implemented on `codex/feat-35-native-output-resolution`. The selector preserves an unavailable
saved value until the player deliberately picks one of the supported sizes; opening the page
or saving an unrelated setting does not replace a stored ultrawide resolution with 720p.

- Focused Python command: `PYTHONPATH=tests python3 -m unittest test_launcher_upscale
  test_launcher_controls.ControlMapTest test_launcher_save`: 37 tests, 17 passed and
  20 Windows/Tk skips. Headless tests exercise the page, combobox preservation, save/load and
  inherited environment override; Windows widget interaction remains unverified here.
- The new page and environment behavior tests both fail on the original launcher from
  `origin/master` (no resolution choice; inherited FSR4 remains enabled), and pass on the
  changed launcher. Temporary baseline proof: ignored `out/verify_launcher_baseline.py`.
- Actual native settings test passed:
  `g++ -std=c++20 -O2 -g -UNDEBUG -Wall -Wextra -Werror -Igpu/shim
  tests/test_upscaler_support.cpp gpu/shim/port_settings.cpp -o out/upscaler-support-test`,
  then `out/upscaler-support-test`. It verifies that the environment disables saved FSR4 while
  preserving the 3840x2160 output selection.
- Existing dependency-light UI composition test also compiled with strict warnings and passed;
  it checks off/copy behavior and 4K viewport scaling, without a Vulkan device.
- Full gate attempted: `bash tools/run_tests.sh` stops at missing `ninja`. The worktree has no
  configured renderer build. Separate Python discovery: 296 tests, 217 passed, 77 skipped,
  and two existing `test_launcher_game_files` errors because `tkinter` is unavailable.
- `git diff --check` passed. Independent review found no actionable issues and reproduced
  focused Python and actual native settings results.

Renderer inspection supports native-size host scene/UI targets in the normal off path;
there was no gameplay capture. Guest compute, copies/readbacks and unsupported proxy formats
can retain the original resolution. An explicit inherited `THEORDER_RENDER_RES` activates
an existing compatibility path and bypasses the selector; `THEORDER_OUTPUT_RES` controls
compatibility output there. Normal launch scripts do not set those diagnostic overrides.
The external NeoEnabler patch and full native-4K rendering of all guest effects are not
validated by this change. Keep #35 open pending hardware and scene validation.
