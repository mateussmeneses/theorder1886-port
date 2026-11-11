# Custom sampler border-color cache

## Evidence and scope

`TextureCache::GetSampler` currently keys samplers by the S# bytes, depth comparison,
and additional LOD bias. `Sampler` separately reads the custom RGBA entry from the
guest border-color table. Reusing an S# with a different table or changing its entry
therefore reuses a Vulkan sampler containing the previous color.

This is a deterministic color correctness defect relevant to investigation of #39;
it is not evidence that the reported Chapter 5 flare problem is resolved. Visual
validation remains with @mateussmeneses.

## Intended change

1. Snapshot the custom RGBA entry once in `GetSampler`, only when the host supports
   custom border colors and the guest requests one.
2. Include that snapshot in the existing cache hash, and pass the same snapshot to
   the sampler constructor. Preserve built-in colors and unsupported-host fallback.
3. Add a regression harness that compiles the production cache method and sampler
   constructor with lightweight dependency doubles. Verify changed table/content,
   unchanged colors, built-in colors, unsupported custom colors, depth, and LOD bias.

## Validation and completion

Demonstrate failure on the baseline and success with the fix using the same test.
Run the focused native test and `bash tools/run_tests.sh`; record environment failures
honestly. No broad graphics behavior changes or claim of issue #39 closure.

## Validation results

- `python3 out/test_sampler_border_cache.py`: compiles the complete production
  `GetSampler` and `Sampler` constructor bodies, with dependency doubles for Vulkan,
  the instance, guest descriptors, and cache containers. Uses production
  `common/hash.h` and the system `XXH3_64bits` implementation. This temporary harness
  is deliberately under ignored `out/`, without a new production test API.
- Baseline `origin/master` fails both changed-table and changed-entry checks
  (exit 1); the fixed code passes (exit 0). Additional checks cover all RGBA
  channels, equal colors at different addresses, built-in colors, unsupported
  custom colors with a null table, depth and LOD separation, and one table read
  across cache-key calculation and sampler construction.
- `git diff --check`: passed.
- `bash tools/run_tests.sh`: attempted, stops before building because `ninja`
  is not installed (`out/tests-build.log`). The focused harness does not replace
  compilation against the renderer's actual dependencies or visual validation.

Independent review reproduced baseline exit 1 and fixed exit 0, checked the real
BorderColorBuffer/BitField API and RGBA types, sole constructor caller, snapshot
lifetime, built-in/unsupported gating and hash/LRU consistency. No actionable
findings. Real concurrency, Vulkan allocation and visual output remain untested.
The PR references #39 and asks @mateussmeneses for visual validation.
