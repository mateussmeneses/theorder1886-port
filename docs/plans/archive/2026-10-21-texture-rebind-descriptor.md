# Texture descriptor after overlap rebind

## Scope

Refs #29. A sampled image can be replaced while binding another overlapping image. In
`Rasterizer::BindTextures`, `FindImage(rebind_desc)` can change the copied descriptor's
base mip or layer, but the second pass retains a reference to the original descriptor.
The replacement image is then sampled and transitioned with the original subresource.
Bind the descriptor reference after the rebind branch so all later consumers use the
resolved descriptor. Keep the storage-type query before the null-image path.

This is a specific alias/subresource correction; the reported game flicker is not
confirmed fixed. @mateussmeneses must validate the visual effect on affected hardware.

## Verification

- Compile a temporary native harness from the actual production second-pass body,
  using doubles for the cache, image runtime, and proxy operations.
- Prove RED before the edit and GREEN after it when `FindImage` changes mip/layer.
  Cover normal, null and storage bindings, and descriptor consumers where practical.
- Run `bash tools/run_tests.sh`; document missing build dependencies honestly.
- Review the final diff independently, then commit and open a PR to `master` with
  `Refs #29` and a visual-validation request to @mateussmeneses.

## Evidence

- The temporary `out/texture-rebind-proof/run.py` compiles the complete production
  second-pass loop with C++ doubles, with no edits to that loop. `--baseline` reads
  the loop from `origin/master` (10e44b4); the default reads the working tree.
  Both compile with `-std=c++20 -O0 -g -Wall -Wextra -Werror`.
- Baseline: compiled successfully, then aborted on `FindTexture used stale mip`
  when the rebind lookup changed the descriptor from mip/layer 1/2 to 3/5.
- Changed code: all six scenarios pass (normal sampled, null, rebound sampled,
  rebound proxy, rebound storage, normal storage). The fixture checks the mip/layer
  received by the texture view, proxy lookup and image transition; original cached
  descriptor preservation; and storage usage classification.
- Limits: the fixture doubles `FindImage`'s mip/layer mutations and Vulkan-facing
  operations. It proves that the actual binding loop propagates the resolved
  descriptor, but does not exercise real overlap detection or a Vulkan device.
- Full gate: `bash tools/run_tests.sh` stopped before compilation because `ninja`
  is unavailable (`tools/run_tests.sh: line 16: ninja: command not found`). The
  renderer and complete suite were not built or validated here.
- Harness, generated red/green C++ and logs stay under ignored `out/`. No persistent
  test that inspects source text was added. Visual validation remains pending.

Independent review reproduced the baseline failure and six passing scenarios and
verified descriptor lifetime, null/storage paths, proxy/barrier consumers and memo
invalidation. No actionable findings. Real renderer compilation and game visuals
remain unverified.
