# Pipelines compiled again after a restart (#113), 2026-11-09

Question from #113: pipeline families stall visible draws (~110 ms driver builds) in the same
places after a restart, although the previous session reported them compiled and saved. Are saved
pipelines rebuilt (unstable keys), or are these new pipelines?

## Setup

| Item | Value |
|---|---|
| Host | i7-14650HX, RTX 5060 Laptop, NVIDIA 621.14.0.0, Windows 11 26300 |
| Game | CUSA00035 01.02 |
| Build | `auto/backlog-20261109` (`af81721`, s1-s4); s5 adds the closest-permutation log below |
| Route | prologue opening, `--pad "24:cross:0.5 28:cross:0.5 32:cross:0.5 36:cross:0.5 40:cross:0.5 50:cross:0.5 60:cross:0.5"`, 125 s, no affinity |
| Chain | s1 starts with an empty port cache (`--cache-state cold`, saves from `user/`); each later session starts from a copy of the previous session's profile (closing and reopening the game) |
| Logs | `THEORDER_BINARY_MISS_LOG=1` (every binary-cache miss: emulator key and driver key), s4-s5 also `THEORDER_PERMUTATION_LOG=1` |
| Not controlled | the NVIDIA driver's own shader disk cache (compiles were fast: 0.3-0.9 s per run) |

Data (not versioned): `out/p1/` in the worktree (`chain.sh`, `compare.py`, `perms.py`, logs,
manifests, profiles).

## Result

| Session | Binary misses (new pipelines) | Of those, driver key missed in an earlier session | Same emulator key, new driver key | STALLED draws | New shader permutations |
|---|---|---|---|---|---|
| s1 (cold) | 442 | — | — | 200 | (no log) |
| s2 | 98 | 0 | 0 | 75 | (no log) |
| s3 | 56 | 0 | 0 | 59 | (no log) |
| s4 | 50 | 0 | 0 | 46 | 75 |
| s5 | 40 | 0 | 0 | 38 | 72 |

- The binary cache works: no pipeline saved by an earlier session was compiled again, and no
  emulator key came back with another driver key.
- What repeats is new pipelines. The family named in #113 shows it: in s2 the stalled draw used
  vertex shader `0000096b66ad3540` with pixel shader stage hash `00000ad44642d3b6`, which is
  permutation 83 of program `0xad441fb1`; s1 had saved permutations 0-82. That program had 83
  permutations after s1, 119 after s2, 139 after s3 (`0xc75dd317`: 78, 114, 137): every session of
  the same route adds permutations. #113's hashes up to `…d3f1` are permutation 140.
- The cache keeps every permutation reachable at load (`perms.py`: 312 programs, no gap in the
  meta indices), so indices are stable; the new ones are specializations not seen before.

## Why a known shader keeps getting new permutations

With the closest-permutation log (s5: each new permutation compared with the existing one that
differs in the fewest specialization fields, restored ones included), 64 of the 72 new
permutations differ from an existing one in exactly one field, always a resource slot of the
material shader:

| Field that differs | New permutations | SPIR-V vs the closest |
|---|---|---|
| image view type 2D ↔ 3D (`Color2D`/`Color3D`), one slot | 35 | different |
| image sRGB, one slot | 11 | 6 different, 5 identical |
| image sRGB and type | 10 | different |
| buffer stride/format | 9 | identical (already folded into the old pipeline key) |
| image integer format | 7 | different |
| image `Color2D` → `Color2DMsaa` | 6 | different |
| image integer + FMASK (a 1920x896 FMASK surface bound in a material slot) | 2 | different |

The material shaders declare up to ~32 texture slots; which texture a slot holds (a 2D texture or
a 512x512x16 volume, sRGB or linear, sometimes an FMASK surface) varies by draw, and the shader is
specialized on each slot. Independent slots multiply, so each session meets combinations it has
not met before; the order and timing of streaming decide which. The SPIR-V really differs for most
of them (image types are part of the declaration), so they are new pipelines, not cache misses.

## What this means for #113

- Request 1 ("check why saved pipelines are rebuilt"): they are not; the stalls after a restart
  are first builds of new material permutations. The count falls each session on a fixed route
  (98 → 56 → 50 → 40 here) but slowly.
- Request 2 (precompilation before gameplay): only helps for specializations already met. The
  current background warm-up already builds the pipelines of recent sessions.
- Request 3 (skip the draw instead of waiting): conflicts with the project rule not to drop
  visible draws.

## Possible reductions (not implemented)

1. **sRGB only where degamma reads it.** `ImageSpecialization::is_srgb` changes codegen only
   through `sampler.force_degamma && format != Srgb` (`resource_patching_pass.cpp:963`). 5 of the
   72 new permutations had identical SPIR-V to their closest permutation and differed only in
   sRGB; letting `Program::CompatibleStagePermutation` ignore `is_srgb` when the SPIR-V hashes are
   equal would fold them into the old pipeline key. Small gain (~7 % of new permutations here).
2. **Slots the shader does not sample.** If a slot's declaration could be made independent of the
   bound texture when the shader never samples it on that path, most single-slot variants would
   disappear. Needs shader analysis per slot; the recompiler declares every slot today.
3. **Cross-user seeding.** Shipping a list of known material specializations (no game data, only
   specialization descriptors) for the warm-up to build at startup. Needs a review of what such a
   list may contain.

## Related observation (to verify)

`resource_patching_pass.cpp:979` emits a signed 2_10_10_10 expand when the bound image is
`NumberFormat::Snorm` with that data format, but `ImageSpecialization` holds neither the data
format nor Snorm: a permutation compiled for a Snorm normal map could be reused for a slot holding
another format of the same view type, or the reverse. Not observed; worth a test that builds two
specializations differing only in that format and checks they do not compare equal.

## Diagnostic change

`THEORDER_PERMUTATION_LOG=1` now names the closest existing permutation and how many fields
differ (`closest K (N fields): …`) instead of comparing with the last used permutation, which was
usually a different material and named unrelated fields.
