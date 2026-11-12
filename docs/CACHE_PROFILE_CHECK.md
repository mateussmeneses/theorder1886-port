# Cache profile check (T-024)

Question: can a shader or pipeline cache written on one GPU or driver be reused on another?
Verdict: **already protected** for the normal (plain file) cache. Two latent or minor gaps are
listed at the end; neither is reachable in the shipped configuration.

## How the check works

- `PipelineCache::PipelineCache` (`vk_pipeline_cache.cpp:562-632`) fills `Shader::Profile` from the
  device: limits, every shader feature, and the vendor workarounds derived from
  `instance.GetDriverID()` (`needs_clip_distance_emulation`, `needs_ssbo_store_load_barrier`,
  `needs_lds_barriers`, `needs_unorm_fixup`, `needs_manual_interpolation`), plus
  `needs_integer_interpolation_fix`, `dref_broadcast` and `translation_revision`
  (`shader_recompiler/profile.h`).
- The raw struct is stored as blob `ShaderProfile/profile`. `CheckCacheProfile`
  (`vk_pipeline_serialization.cpp:1234`) compares it at startup. A missing blob, another size
  (struct changed) or any differing member (`operator==`, `profile.h:78`) logs
  "Pipeline cache isn't compatible with current system: rebuilding it", calls `DataBase::Clear()`
  (deletes every meta, SPIR-V and pipeline key file) and stores the new profile.
- It runs exactly once per start on every path: `StartOnDemand` (`:1373`), `StartBackgroundWarmUp`
  (`:1581`) or `WarmUp` (`:1278`), all reached from `vk_presenter.cpp:181-196` before any lazy
  `LoadShaderMeta` / `ShaderBinary` read. The warning is emitted once, because the profile is
  rewritten right after.
- Test seam: the comparison is now `Shader::StoredProfileMatches` (`shader_recompiler/profile_blob.h`),
  used by `CheckCacheProfile` and by `tests/test_cache_profile_check.cpp`.

## Artifact table

| Artifact | Key | Mismatch behaviour |
|---|---|---|
| Permutation SPIR-V (`.spv`) | `pgm_hash_permutation`, checked by `SpvHash` (`:1048-1051`) | Profile differs: file deleted by `Clear()`. Same profile: content hash must match the meta. |
| Shader meta (`.meta`) | pgm hash / key value | Same as above; damaged or short blob throws `DeserializeError`, treated as "translate again". |
| Pipeline key (`.key`) | graphics/compute key hash | Same as above (`Clear()`); stale entries counted as "stale pipelines". |
| Profile blob (`.bin`) | fixed name `profile` | Compared as a whole struct; rewritten after `Clear()`. |
| Pipeline binaries (`VK_KHR_pipeline_binary`, `Decode` `:129`) | file stores GPB1 magic, version, pipeline hash, **global key** (`vkGetPipelineKeyKHR(nullptr)`, `vk_instance.cpp:245-270`) and per-pipeline key, plus XXH3 checksum | Any difference in global key (driver and device identity, including driver update by the Vulkan spec), pipeline key (derives from the SPIR-V and state) or checksum returns nullopt: the binary is rejected (`binary_rejected`) and the pipeline compiles normally. Not covered by `Clear()`, and not needed. |
| Driver `VkPipelineCache` (`LoadDriverCacheBlob`, `vk_pipeline_cache.cpp:~395-425`) | header magic/version, payload hash, `vendorID`, `deviceID`, `pipelineCacheUUID` | "Driver pipeline cache ignored (different GPU/driver)"; empty cache is used. |

## Driver version

No `Profile` field is read from `driverVersion` (it is only used for the display name,
`vk_instance.cpp:219`). A driver update can still change Profile through the properties and
features it reports (float controls, subgroup size, extension and feature support): the next
start sees a different Profile and rebuilds. A driver update that changes only compiler output is
covered where it matters: `pipelineCacheUUID` (driver cache) and the pipeline binary global key
change with the driver build. SPIR-V itself does not depend on the driver build, only on the Profile.

## Gaps (not fixed here)

1. **Archived cache mode is not protected, but is unreachable.** `DataBase::Clear()` returns early
   when `IsPipelineCacheArchived()` (`cache_storage.cpp:331`), and the zip would keep the old
   `profile.bin` entry. `gpu/shim/core/emulator_settings.h:59` hardcodes it to `false`, so it only
   matters if archive support is turned on. If so, `Clear()` must recreate the archive.
2. **Debug env vars that change emitted SPIR-V are not in Profile**: `THEORDER_SRT_GARBAGE`
   (`flatten_extended_userdata_pass.cpp:700`), `THEORDER_LDS_BARRIERS_MULTIWAVE`
   (`ir_passes.h:66`), `THEORDER_POINT_LIGHT_GUARD` (`resource.h:133`). Flipping one on the same
   machine reuses SPIR-V built with the other setting. They are A/B knobs: clear the cache when
   toggling them. Folding them into `Profile` would invalidate every existing cache, so it was not done.

## Reports of artifacts after a GPU or driver change

The code does not explain them through a stale shader cache: the profile and binary keys reject it. Check instead
the first start after the change (the log must show "rebuilding it"), the texture/render-target
caches (not covered by this check), or a driver regression.

## Test

`tests/test_cache_profile_check.cpp` (target `cache-profile-check`, in `tools/run_tests.sh`): a blob
written for a Profile that differs in a vendor workaround, a feature, `subgroup_size`,
`supported_spirv`, `translation_revision`, or in size, is reported as not matching. With the
comparison replaced by `return true` the test aborts on the first case (checked locally with clang;
the full suite runs in the `gate`).
