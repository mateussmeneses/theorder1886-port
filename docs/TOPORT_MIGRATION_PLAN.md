# TOport-to-TheOrder migration: evidence, stages and acceptance

Checked on 2026-10-31. Reference: [IgorAugustoHH/The-Order-1886-PC-Port](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port), branch `main` at `5f066fec1b96dc2e06ee5e42c164cd4f689deb01`. Destination: this repository, `master` at `8b3770c87e38b8cb77d82702416e031ec1ff4948`. Comparison is code-level only; the reference's reported stutter reduction is not a matched benchmark on this port.

## Architecture and do-not-regress checklist

Both projects use shadPS4-derived Vulkan and a native x86-64 runtime. They have different GPU integrations and shader/pipeline code. Do not replace the entire `gpu/shadps4`, `gpu/shim`, `run.py` or `launcher` directories with the other project.

| Reference change | Destination status at audit | Next action |
| --- | --- | --- |
| Face depth, particle SRT, redraw depth bias, EVSM/HTILE | Already adapted; see root README credits | Compare GPU-backed rendering; do not copy again |
| Particle/point-light descriptor guard (TOport v0.1.5, shader `0x804aeead`) | Specific guard absent; destination has broad stale T# filtering and measured many new shader variants | Stage 1: implement opt-in; verify CUSA00035 |
| Driver pipeline cache and recent-pipeline warm-up | Present in destination, with persistence and visible-pipeline priority | Measure separately; do not import conflicting cache manager |
| Async compute compilation | Present with safety policy: one-shot image-writing dispatches must not be skipped | Preserve policy; do not copy reference's global default of async compute |
| Indirect indexed draw count clamp, from JulioCacko (TOport v0.1.4) | Reference implementation not present by that name | Stage 2: inspect and adapt with bounds/barriers tests |
| Compile pressure budget / fair queues | Both have custom compiler queue/worker policies | Stage 3: reproduce workload and evaluate admission-control vs actual driver wait |
| TAA/FSR/DLSS, render resolution, motion vectors | Diverged and contain independent modifications | Stage 4: visual compare; integrate only causal fixes |
| GPU memory/cache/readback accounting | Diverged; destination already has its own fixes | Stage 5: profile; avoid wholesale replacement |

## Stage 1 — opt-in particle-light guard (this PR)

**Provenance:** TOport [v0.1.5 release notes](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/blob/19779e291dda6545961469046430b93b5505e801/docs/release-v0.1.5.md), [implementation commit](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/19779e291dda6545961469046430b93b5505e801), informed by [shadPS4 #5218](https://github.com/shadps4-emu/shadPS4/pull/5218).

The reference reports that `0x804aeead` has 16 unrolled lights and a 708-dword flattened user-data layout. `flat[657]` controls the active light count, `flat[655]` the secondary light-image bitmask. First-image slots start at word 68 every 8 dwords; second-image slots start at 196 every 8 dwords. Unused lights can contain stale descriptors, producing permutations that the shader never reads.

The destination's own [shader cache report](SHADER_CACHE_OPTIMIZATION.md) already records high specialization variation in shader `0x804aeead`, including distinct SPIR-V and driver keys, but *does not prove the reference guard matches the destination's runtime content*. This PR leaves `THEORDER_POINT_LIGHT_GUARD` **off by default** pending a controlled GPU A/B on the target edition.

Code: `gpu/shadps4/shader_recompiler/resource.h`, guarded to the exact hash and 708-word layout, with fall-through for all other shaders/layouts. Focused regression cases: `tests/test_shader_resource.cpp`. Do not remove other image plausibility, memory-mapping, fallback-mask or image-type checks.

### Reproduce on Windows (separate cache/profile copies)

1. Build the branch in **MSYS2 CLANG64**: `bash build.sh`. Run `python -m unittest discover -s tests` and the native test target for `test_shader_resource.cpp` as configured in `gpu/CMakeLists.txt`. Confirm no compiler errors or test failures.
2. Use the **same game edition CUSA00035, update 01.02, eboot hash**, same GPU driver, checkpoint, camera, save state, display mode, resolution, MSAA and upscaler across conditions. Record port commit SHA and executable hash.
3. Copy the same *known initial* `user` profile into two **separate writable directories**; retain all real save data and original caches. Use `tools/batch_runs.py --help` for its profile/data-root and condition options. Never compare two runs that shared a writable pipeline cache.
4. Set `THEORDER_POINT_LIGHT_GUARD=0` for control and `=1` for candidate. Use `THEORDER_PERMUTATION_LOG=1`, `THEORDER_FRAME_STATS=1` and `--frame-trace` if the chosen runner supports them. Run repeated **cold** and **warm** visits to exactly the same scene (e.g. ABBA order) with separate output directories.
5. Capture the same frame at the same checkpoint in both modes; compare lighting, smoke, fire, particles, shadows, textures, text and HUD. Verify no new Vulkan VUID, guest fault, black/white scenes, missing light or lost UI image.
6. Record total shader `0x804aeead` unique permutations, new SPIR-V checksums, new driver pipeline keys, binary hits, synchronous compilation waits, worst frame, raw P95/P99 and stalls per minute. Do not infer performance from average FPS alone.
7. If guard-on reduces runtime builds *and* all visual/correctness gates pass, consider enabling it by default in a **separate** reviewed PR, with the old policy still available as rollback.

PowerShell, when launching the selected build:
```powershell
$env:THEORDER_POINT_LIGHT_GUARD = '0'  # baseline
$env:THEORDER_PERMUTATION_LOG = '1'
$env:THEORDER_FRAME_STATS = '1'
python run.py

$env:THEORDER_POINT_LIGHT_GUARD = '1'  # candidate, in an isolated profile
python run.py
```

## Follow-up stages (each in an independent PR)

**Stage 2 — indexed-indirect clamp.** Compare TOport's `gpu/shadps4/video_core/host_shaders/indirect_index_clamp.comp`, `vk_rasterizer.cpp/.h`, and shader CMake registration at [commit 726b0ab](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/726b0ab08a). Adapt indexed buffer bounds, command argument stride/count, buffer upload and Vulkan barriers to the destination. Leave an opt-in toggle first and test zero counts, overflow, bounds and visual equivalence; do **not** overwrite the destination rasterizer.

**Stage 3 — compiler fairness, memory pressure, preloading.** Audit the reference `bb_compile_budget.{h,cpp}`, `bb_compile_queue.h`, `vk_pipeline_cache.cpp` against the destination's queue priority, per-key driver builds, driver cache, warm-up list and worker pool. Change only one admission-control or caching choice per A/B. Preserve essential visible draws and once-only compute writes.

**Stage 4 — image parity.** Compare reference shadow/HTILE, depth aliasing, TAA, MSAA, interpolation, raster targets and lighting with already adapted destination paths. Compare the same captured scene before/after one feature. Keep benchmark and visual acceptance independent; treat black/white world regressions against `windows-v0.6.1` as a separate tracked issue.

**Stage 5 — resource lifetime and packaging.** Run 30–60 min scene walks and cold/warm traversals with RAM/VRAM, texture eviction, readback wait, guest faults, GPU validation and save/load checks. Only then consider UX, launcher and package changes. Track test identity, CPU/GPU, driver, region, build, cache state and screenshots.

### Acceptance definition

For each stage: documented source commit and file paths, isolated change, rollback, build/CI pass, consistent cold/warm timing, no missing particles/lights/shadows, no added guest/Vulkan errors and captured evidence. Label unavailable GPU-backed evidence **not tested**, rather than passing it.
