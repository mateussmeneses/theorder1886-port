# Additive render-target classification

## Evidence and scope

`Rasterizer::BeginRenderingFull` classifies every color attachment using blend slot zero,
although graphics-pipeline construction uses each attachment's own blend state. The classifier
also accepts source factor `One` for non-ADD functions, and ignores the color buffer's blend
bypass. A false positive can admit an unrelated float target into the existing black-clear
heuristic. This is a concrete classification defect relevant to the investigation of #45;
its contribution to the reported overexposure has not been established by a game capture.

## Implementation

1. Create `codex/fix-45-additive-target-classification` from the provided checkout.
2. Put the additive-color predicate in the existing `vk_blend_rewrite.h`, requiring enabled
   blending, no bypass, destination `One`, source `One` or `SrcAlpha`, and function `Add`.
3. Pass the current attachment's blend control and bypass into the predicate in the rasterizer.
4. Preserve target-size restrictions, clear tracking, SNORM handling, and all other heuristics.

## Validation

- Exercise the production rasterizer expression in a native source-extraction harness so a
  correct helper cannot conceal a call site that still selects slot zero.
- Demonstrate baseline RED and patched GREEN for disabled/bypassed blending, wrong destination,
  both accepted sources, all other blend functions, and independent MRT states.
- Run the existing native `blend-rewrite-test` and attempt `bash tools/run_tests.sh`. Record environmental failures separately from regression failures.
- Do not claim #45 visually resolved without a matching hardware/game capture.

## Result

Implemented the helper and current-attachment call site, with a permanent matrix in the existing
`tests/test_blend_rewrite.cpp`. No other clear heuristics or trackers changed.

Validation on 2026-10-21:

- `python3 out/verify_additive_target.py --baseline origin/master`: RED, eight classification
  failures (bypass, non-ADD functions, and both directions of MRT independence).
- `python3 out/verify_additive_target.py`: GREEN. This ignored local proof compiles the actual
  production expression, the production `BlendControl` declaration and helper, and the same
  native matrix; it does not require Vulkan or replace the renderer with a test-side classifier.
- The complete existing `test_blend_rewrite.cpp` also passed as a standalone native executable
  (`out/blend-rewrite-standalone`). Its generated translation unit uses the production
  `NumberFormat` and `BlendControl` declarations and full blend-helper header body. Only the
  unavailable header include dependencies are omitted; no blend implementation is mocked.
- `bash tools/run_tests.sh`: blocked before compilation because `ninja` is unavailable
  (`out/tests-build.log`). Direct compilation of the normal native target is blocked by missing
  `fmt/format.h`; CMake, Clang, Vulkan headers, and initialized submodules are also absent.
- `python3 -m unittest discover -s tests`: 292 tests, 76 skips, two unrelated errors importing
  unavailable `tkinter` in `test_launcher_game_files`. All remaining executed tests passed.
- `git diff --check`: passed.

The extraction harness, generated C++, binaries, and proof logs are ignored local artifacts in
`out/`, not a new permanent test framework. Full renderer compilation and game/hardware capture
remain pending. This change corrects the classifier; it does not establish that #45's visible
overexposure is resolved.

Independent review found no actionable issues, checked the per-target pipeline blend state,
and reproduced baseline failure and candidate success. It also compiled the extracted full
blend test with `-Wall -Wextra -Werror` and passed. Renderer integration and render-state memo
reuse remain outside the isolated proof.
