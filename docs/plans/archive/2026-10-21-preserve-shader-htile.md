# Preserve shader-written HTILE on formatted reads

Issue: #26. Scope: guard the optional synthetic HTILE value in
`BufferCache::SynchronizeMemoryFromImage` only. Do not change the EVSM shader
hash overrides, HTILE constants, depth formats, copy paths, or GPU barriers.

## Evidence and intended behavior

`TextureCache::HtileReadValue` returns no value after `NoteMetaShaderWrite`:
a shader encoded the metadata, so subsequent reads must retain those GPU bytes.
The formatted-buffer fallback currently dereferences that empty optional and
passes undefined data to `Runtime::FillBuffer`.

The caller has already synchronized arena uploads before entering this method.
Return false immediately when there is no synthetic HTILE value, preserving the
arena contents and avoiding the unrelated image-alias fallback. Keep synthetic
fills for cleared or expanded metadata, including stencil metadata.

## Implementation and validation

1. Create a short-lived `codex/fix-26-preserve-shader-htile` branch.
2. Add the optional guard in the HTILE branch and explain why it returns early.
3. Compile a native regression harness using the actual production method body
   and actual HTILE state methods with fake runtime/cache dependencies. Exercise
   clear -> depth draw -> shader write -> formatted read -> clear, and stencil
   metadata. Require baseline failure and candidate success; after shader write
   require no fill and no image-alias fallback.
4. Run `bash tools/run_tests.sh`, recording missing dependency/build limitations
   without presenting them as successful verification.
5. Obtain independent review, commit the implementation and open a PR to `master` with `Refs #26`.

## Acceptance and limits

The native regression must fail on the original empty-optional dereference and
pass with the guard. The fix must preserve normal synthetic HTILE values and
never overwrite shader-written metadata. This corrects a demonstrated defect
in the shadow metadata path; it does not prove the reporter's missing sun
shadows are fixed. A matching game capture on the reporter's GPU is still
required before closing #26.

## Validation performed

- `python3 out/htile-native-proof.py`: baseline exited -6 on libstdc++'s
  empty-optional assertion; candidate exited 0 (`HTILE transition native proof
  passed`). The harness compiles the complete production
  `SynchronizeMemoryFromImage` body and actual HTILE state method bodies with
  fake Vulkan/runtime dependencies. It checks the synthetic cleared/expanded
  values, shader-written reads producing neither a fill nor an alias lookup,
  restoration after clear, and the stencil expanded value. Source and log are
  retained in ignored `out/`; this does not add a permanent extraction-based
  test or a production test seam.
- `git diff --check`: passed.
- `bash tools/run_tests.sh`: failed before building or running tests because
  `ninja` is unavailable (`tools/run_tests.sh: line 16: ninja: command not
  found`). This worktree also has no configured `out/gpu` build directory.
  No full renderer build, full suite, game runtime, or visual claim is made.

Independent review found no actionable production issues and independently reproduced
the baseline failure and candidate success. Real Vulkan barriers, arena bytes and
gameplay remain outside the isolated harness. The baseline is read from
`origin/master` so the local proof remains runnable after committing.

- Separate Python discovery ran 292 tests: 214 passed, 76 skipped, and the two
  existing `test_launcher_game_files` cases errored because `tkinter` is absent.
