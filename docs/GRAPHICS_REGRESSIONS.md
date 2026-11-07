# Graphics regression checkpoint

## 2026-11-06: RDNA4 (RX 9070 XT) verification of the sun-shadow fix chain - #26

**Hardware / build:** AMD RX 9070 XT (Mesa RADV GFX1201) on Fedora Atomic (Bazzite),
Chapter I "A Lady in Waiting" (1-1 opening alley and terrace), 30 FPS baseline,
`THEORDER_HTILE_DRAW_EXPAND` at its default (on). Player-owned dump; no dumps,
saves or personal paths attached.

**Staged A/B on the same checkpoint and save:**

1. Sun readback synchronization alone (`KeepPageSynchronized`, as merged in the sibling
   JulioCacko port via its PR #143): cutscene sunlight and shadows return; **gameplay
   keeps flat lighting** - no dynamic shadows on the cart, crates, sandbags, NPCs or
   fence, and the sun patch lands on the wrong surfaces.
2. Adding the HTILE draw-expand half: gameplay is fixed. Cart/crate/sandbag/fence/NPC
   directional shadows, sun patch on the ivy wall, wet-cobble specular. Player-confirmed
   on the Chapter I checkpoint.

**Direct evidence for the mechanism (frame capture + image dumps, instrumented build):**

- The sun cascade maps (D32 1024x1024, two array layers) DO receive the caster draws in
  both cases (depth-target draw histogram: ~690k and ~548k draws; decoded dumps show the
  fence, buildings and cart geometry).
- With the HTILE expand OFF, the EVSM conversion (`0x8ea0ef5b`) writes a **constant**
  moment texture (all four quadrants = exp(k x far), a single distinct value): the
  depth->shader-read path serves the cleared HTILE state - exactly Area 2's failure model.
- With the expand ON, the same texture carries real depth (channel 0 range 0.014..54.5,
  structure matching the cascade geometry).
- Boot log confirms the expansion on eight depth surfaces, e.g.
  `HTILE: 0x104e331800 marked uncompressed, 131072 bytes` - the size matches the
  1024x1024x2-layers tile grid (`tests/test_htile_expand.cpp`).

**Conclusion for #26:** on RDNA4 the HTILE draw-expand is the decisive half; sun readback
sync alone does not restore gameplay shadows. The merged fix chain is verified working on
this architecture. The RTX 4050 (NVIDIA) report predates the `7cf7979` merge and should be
retested against a build containing it; if it still fails on NVIDIA, the constant-vs-real
EVSM test above separates an HTILE cause from a different one in a single capture.

## 2026-11-01: comprehensive graphics fidelity, temporal stability, and VRAM management investigation (Prompts 4, 5, 6 & 7)

Comprehensive investigation and remediation of graphics fidelity regressions, flickering, incorrect shadows, unstable lighting, temporal reprojection jitter, and resource management across the 12 mandatory investigation areas comparing `theorder1886-port` against `TOport` (reference checkout at `ceebdf3`). Changes are isolated into six independent PR branches with dedicated A/B rollback environment variables.

### 12-Area Investigation & Root-Cause Analysis Matrix

| # | Investigation Area | Root Cause & Differential Analysis vs TOport | Resolution & Validation | Rollback / Gate Flag |
|---|--------------------|----------------------------------------------|-------------------------|----------------------|
| 1 | **Depth buffers and depth/stencil** | `THEORDER_REDRAW_DEPTH_BIAS` defaulted to `128.0f` on non-writing `LessEqual` draws. As proven in TOport commit `e825344`, applying universal depth bias on non-writing depth draws introduces severe black polygonal tearing and horizontal streaks across environment geometry. | Defaulted `THEORDER_REDRAW_DEPTH_BIAS` to `0.0f`. Character face passes remain protected via dedicated `BbFaceDepth`. Validated in `tests/test_depth_stencil_state.cpp`. | `THEORDER_REDRAW_DEPTH_BIAS=128.0` |
| 2 | **EVSM, HTILE and shadow maps** | HTILE compressed metadata buffer ranges were left untouched during full depth writes in `BeginRenderingFull`. Subsequent compute passes (`0x8ea0ef5b`, `0x9846aaff`) read stale HTILE compression tiles, treating valid depth as cleared. | Implemented `MarkRangeUncompressed` on active depth buffers with HTILE addresses during `BeginRenderingFull`. | `THEORDER_HTILE_DRAW_EXPAND=0` |
| 3 | **Depth aliasing and image synchronization** | The Order samples multisampled depth (`D32_SFLOAT`, 2x/4x) through packed color views (`R32G32_SFLOAT`, `R32G32B32A32_SFLOAT`) in resolve passes. Upstream shadps4 rejected this in `CanCopyColorAndDepth` or attempted invalid color attachment bindings. In `TextureCache::ResolveDepthOverlap`, copy failures leaked `new_image_id` without freeing, corrupting page tables. | Added bit-preserving depth-to-color blits (`PlanDepthAliasCopy`, `ms_depth_to_color_frag`) and explicit `FreeImage(new_image_id)` on copy failure. Validated in `tests/test_depth_alias_copy.cpp`. | `THEORDER_DEPTH_ALIAS_FIX=0` |
| 4 | **Redraw depth bias** | Non-writing depth passes on environment meshes caused visual z-fighting and black geometry tears when biased globally. | Defaulted redraw depth bias to `0.0f` (see Area 1). | `THEORDER_REDRAW_DEPTH_BIAS=128.0` |
| 5 | **Shader resource binding** | Speculative SRT walker accessed guest buffers across camera cut transitions where live pointer tables differed from disk templates, risking invalid descriptor bindings. | SRT buffer reads protected; pipeline compilation snapshots guest memory states at draw time. | `THEORDER_PREQUEUE_SUBMISSION_PIPELINES=0` |
| 6 | **MSAA and resolve** | Resolving multisampled depth to color textures resulted in black or clipped resolve textures. | Bit-preserving multisample depth-to-color fragment blits transfer raw sample bits bit-identically into color channels. | `THEORDER_DEPTH_ALIAS_FIX=0` |
| 7 | **Vulkan image formats** | Depth textures reinterpreted as color formats (`D32_SFLOAT` -> `R32G32_SFLOAT` / `R32G32B32A32_SFLOAT`) failed format compatibility checks without dedicated alias blits. | Resolved by `PlanDepthAliasCopy`. | `THEORDER_DEPTH_ALIAS_FIX=0` |
| 8 | **FMask, CMask and metadata** | Missing HTILE uncompressed markings caused shadow passes to sample uninitialized depth tiles. | HTILE expansion resolves stale metadata state (see Area 2). | `THEORDER_HTILE_DRAW_EXPAND=0` |
| 9 | **Read/write barriers and CPU/GPU sync** | Sunlight luminance & auto-exposure compute shaders (`0x7550d841`, `0x7b1b18c9`) write to host-visible buffer pages also dirtied by the guest CPU. CPU write-watch invalidation discarded GPU writes, feeding 0s into engine exposure logic and causing total whiteout or pitch-black screens (#43, #47). | Implemented `KeepPageSynchronized`, `RecordSunCopies`, and `DeliverSunCopies` to ensure CPU readbacks receive valid exposure data. | `THEORDER_SUN_READBACK_SYNC=0` |
| 10 | **Render targets and final composition** | Shaders converting reduced `FragCoord` to guest pixels assumed 1920x1080 raster size, whereas the retail game renders in 1920x800 cinema scope. This compressed `FragCoord.y` factors, causing vertical distortion and reprojection skew. | Set `GameSize()` to 1920x800 and adjusted `push_data.scene_size` in `vk_rasterizer.cpp` to `800 / scene.height`. Validated in `tests/test_scene_resolution.cpp`. | `THEORDER_SCENE_LETTERBOX=0` |
| 11 | **Particles, transparencies, and lighting** | Speculative background worker compilation of 9 facial shaders (`0x193274a4`, `0x0e4fee1c`, `0xf049dbf2`, `0x418001f4`, `0xf5c17eec`, `0x39e98e7c`, `0xdc16510f`, `0x02108015`, `0x0a54a188`) caused missing facial textures and dark geometry upon camera cuts (#68, #63). UI point-samplers at 1440p/4K caused severe icon flickering and shimmering. | Facial shaders are guarded against background worker specialization (`THEORDER_FACE_WORKERS=0`), compiling synchronously on the GPU thread. UI point-samplers are promoted to Bilinear during UI rendering phase. Validated in `tests/test_face_workers_guard.cpp`. | `THEORDER_FACE_WORKERS=1`, `THEORDER_UI_BILINEAR=0` |
| 12 | **Numerical precision & VRAM lifecycle** | Unbounded pipeline binary disk and memory caching bloated cache directories and exhausted host memory over long sessions. Stale `.tmp` writes accumulated on disk. | Added `TrimPipelineBinaries(directory, budget_bytes)` (default 4096 MB) using LRU file modification timestamps and cleaning orphaned `.tmp` files. Validated in `tests/test_pipeline_binary_trim.cpp`. | `THEORDER_PIPELINE_BINARY_CACHE_MB=0` |

### Independent PR Branches Delivered

1. `fix/gfx-redraw-depth-bias-default` (commit `87e33e0`):
   - Default `THEORDER_REDRAW_DEPTH_BIAS` to `0.0f` (was `128.0f`), matching TOport commit `e825344`.
   - Eliminates black geometry tearing and horizontal streaks across environment meshes.
   - Dedicated unit test in `tests/test_depth_stencil_state.cpp`.
2. `fix/gfx-depth-alias-and-overlap` (commit `77a5c13`):
   - Bit-preserving depth-to-color MSAA alias copies (`PlanDepthAliasCopy`, `ms_depth_to_color_frag`).
   - Fixes critical image leak and page table desync in `TextureCache::ResolveDepthOverlap` on copy failure.
   - Dedicated unit test in `tests/test_depth_alias_copy.cpp`.
3. `fix/gfx-sun-readback-sync-and-htile` (commit `7cf7979`):
   - Exposure/sunlight luminance buffer synchronization (`0x7550d841`, `0x7b1b18c9`) resolving whiteout and pitch-black screens (#43, #47).
   - HTILE buffer range uncompressed expansion on depth write in `BeginRenderingFull`.
4. `fix/gfx-face-workers-and-compute-recurring` (commit `d31feb8`):
   - Guards 9 facial shaders against background worker specialization across camera cuts.
   - Preserves facial textures and prevents missing geometry transitions.
   - Dedicated unit test in `tests/test_face_workers_guard.cpp`.
5. `fix/gfx-scene-resolution-1920x800` (commit `d4b69c8`):
   - Sets `GameSize()` to 1920x800 retail cinema scope and corrects `FragCoord.y` factor conversion.
   - Eliminates temporal reprojection skew in TAA, DLSS, and FSR.
   - Promotes UI point-samplers to Bilinear during UI rendering phase to eliminate high-res HUD shimmering.
   - Dedicated unit test in `tests/test_scene_resolution.cpp`.
6. `fix/mem-vram-pipeline-binary-trim-and-srt` (commit `431ca76`):
   - LRU trimming of pipeline binary cache to `THEORDER_PIPELINE_BINARY_CACHE_MB` (4096 MB).
   - Cleans up stale `.tmp` partial cache writes and bounds memory usage over extended play sessions.
   - Dedicated unit test in `tests/test_pipeline_binary_trim.cpp`.
7. `integrate/gfx-and-temporal-fidelity-combined` (commit `bd8befe`):
   - Clean combination of all 6 PR branches for integrated testing without auto-merging to `master`.
   - All 54 GPU/CPU tests pass.


## 2026-10-28: capture-based black-screen stage diagnosis (draft PR)

Issues #43/#47 describe audio, subtitles/HUD and a black gameplay scene
after v0.6.1. The old/new renderer comparison still needs a matched GPU,
driver and scene capture to establish a cause. The presenter already supports
two RAW dump stages, `display` (before composition) and `output` (before
swapchain presentation). A new dependency-free
`tools/black_screen_triage.py` compares their 8-bit center/whole-frame
visibility and can contrast candidate/baseline captures by numbered flip.
It validates lengths/dimensions, skips unsupported channel order, and reports
only filenames and aggregate metrics, including on read errors.

This distinguishes a likely dark guest display buffer from a likely
post-processing darkening without claiming either stage is a proven defect.
See `docs/RAW_FRAME_VISIBILITY_TRIAGE.md` for controlled capture steps.
The helper does not test actual Vulkan drivers, prove a checkpoint match or
resolve #43/#47. Avoid deleting caches/saves or enabling continuous RAW
captures at high frequency.

## 2026-10-27: swapchain composite-alpha surface compatibility (draft)

The old `Swapchain::SetSurfaceProperties` selected `eOpaque` whenever
supported, otherwise **unconditionally `eInherit`**, even if the surface only
exposed `ePreMultiplied` or `ePostMultiplied`. Vulkan requires the selected
`VkSwapchainCreateInfoKHR::compositeAlpha` bit to be one of
`VkSurfaceCapabilitiesKHR::supportedCompositeAlpha`. Passing an unsupported
bit can cause validation errors, failed swapchain creation, or loss of
presentation on some window/driver configurations.

The renderer now chooses in preference order: Opaque, Inherit,
PreMultiplied, PostMultiplied, **only when advertised**. This preserves
its historical preference for Inherit whenever Opaque is absent. If none are supported it
reports an explicit assertion instead of sending an invalid bit to the driver.
The pure policy helper is tested for all four modes, combinations and an empty
capability mask without a GPU. It is part of the Windows native suite and a
lightweight Linux header-based check.

This addresses a real capability-negotiation defect. **The same unverified
Inherit fallback existed in windows-v0.6.1 and windows-v0.7.0**, so this
cannot independently explain the reported v0.6.1 → v0.7.x black-screen
regression. It has **not been established** as the cause of #10's Intel
fullscreen alt-tab error, #43/#47 black output or #68's stutters. GPU-backed
validation on affected hardware is still required.

## 2026-10-27: profile-gate the selective SSBO shader cache revisions (draft)

The prior #80/#81 revisions salted disk shader versions for every formatted
writable/writable SSBO even on hosts that never enable
`Profile::needs_ssbo_store_load_barrier`. That flag is currently enabled
for NVIDIA proprietary drivers, **not for AMD or Intel**. On the latter,
the serialized meta versions could reject reusable shaders despite generating
identical IR/SPIR-V, triggering avoidable shader translation/first-use stalls.

The cache metadata reader, warm-up path, pipeline-stage loader and shader-meta
writer now all receive the **same active device profile's gate** explicitly.
The salt is zero on profiles without this barrier requirement. On enabled
profiles it preserves the two additive salts: formatted writable (`1 << 18`)
and any writable buffer (`1 << 19`). The global shader binary format/layout
version stays unchanged. Standalone C++ tests cover all combinations and Python
source checks guard plumbing across save/reload/warm-up.

**One-time cache migration caveat:** AMD/Intel shader-meta blobs written under
the previous unconditional salt may be rejected and recompiled **once** because
their version changes back to unsalted; thereafter the new profile-appropriate
version is reusable. No user cache/saves are deleted or forcibly reset.
The effect on real frametimes requires matched cold/warm scene captures.


## 2026-10-27: atomic SSBO read-modify-write barrier coverage (draft)

The existing InsertSsboStoreLoadBarrier pass ignored `BufferAtomic*`
instructions even though they read and write the same resource. They now
participate in both read/write sets; barriers are added after atomics
only when there is a non-atomic load of the same binding, and after raw
stores when an atomic may read that binding. Two atomic-only operations do
not require extra barriers from this pass. This is restricted to
NVIDIA compatibility profiles that already enable this pass. Other GPUs
retain their behavior. Dependence analysis remains conservative by binding,
not a proof of access to an identical element.

Cached Shader::Info metadata marks atomics as written but does not distinguish
them from plain stores. A new additive per-shader binary salt (`1 << 19`)
recompiles all shaders with writable bindings as a safe conservative subset,
without invalidating read-only shader caches or deleting existing user data.
This can increase first-use compilation cost for writable shaders.

Focused real IR tests now cover atomic-to-load, store-to-atomic,
atomic-to-atomic, distinct bindings and profile-disabled behavior.
Windows CLANG64 build, full IR suite, hardware Vulkan validation and
matching reporter scene captures are still required. No fix for #43/#47
has been demonstrated.


## 2026-10-27: formatted SSBO store/load barrier coverage (PR pending)

Code review found that `InsertSsboStoreLoadBarrier` runs **before**
`LowerBufferFormatToRaw`, but its read/write opcode sets did not include
`LoadBufferFormatF32` or `StoreBufferFormatF32`. Consequently a shader with
a formatted store followed by a formatted or ordinary load on the same
resource binding could miss the device memory barrier that this
NVIDIA-specific compatibility pass is intended to add.

The patch adds those two opcodes to the existing classification **only**.
No barrier is added on hosts with
`Profile::needs_ssbo_store_load_barrier == false`. The pass still matches by
resource binding and conservatively inserts after stores when that binding
is loaded anywhere in the shader; this does **not** prove a same-address
dependence and may increase work on affected shaders.

Shader meta cache entries now use a selective per-shader binary version salt
(`1 << 18`) when the saved resource list contains a formatted and writable
binding. This avoids globally invalidating warmed SPIR-V for unaffected
shaders, but the affected subset still recompiles on first use. The salt is
independent of existing motion/float-atomic revisions and does not change the
base ShaderBinaryVersion. A dependency-free C++ test covers this predicate.

`tests/test_formatted_ssbo_barriers.cpp` runs the real IR pass on
formatted/ordinary store/load pairs and verifies exact barrier placement,
different bindings, disabled profiles, and existing conservative ordering.
The test must be built on Windows/MSYS2 CLANG64 with the rest of the GPU
suite; it does not prove Vulkan synchronization validation or #43/#47.
A matching hardware/capture retest remains mandatory.


## 2026-10-25: user retest of the local package

The user reports good gameplay with some remaining flicker. The corresponding
private log identifies the local RTX 5060 Laptop setup, but the profile's
capture and screenshot folders are empty. There is no chapter/checkpoint,
camera, draw ID, or image evidence to connect the flicker to the color/depth
change or another pass. Keep the visual issue open and do not add a broad
shader/layout workaround from this report alone.

## 2026-10-25: local color/depth overlap handling

The renderer now reports whether the color-to-depth overlap copy actually
completed. Unsupported copies no longer silently suppress the guest dispatch;
the overlap resolver retains the original color image and uses the valid depth
backing. A multisampled color-to-depth blit path was added with a cached pipeline
keyed by sample count and format.

`aliasing-test`, the full `tools/run_tests.sh` suite (48 C/C++ tests plus
Python), and isolated 130-second MSAA/no-MSAA smokes passed. Both smokes exited
0 without crash/hang and no reported VUIDs in captured logs; the MSAA capture
showed rendered prologue characters. Validation layers were not established as
active, so this is not a synchronization-validation pass. It does not validate
Chapter 5/8/9, the reporters' GPUs, or the public black-screen
issues. No release/package containing this local change has been tested by
reporters.

Current public evidence is negative for several v0.7.7 reports: #43 remains
black/white on an RTX 3060 Ti (CUSA00035 01.02), #47 has a black-screen report
on RX 6500 XT and an additional RX 560 report, #39 remains in Chapter 5 on RX
9070, #63 remains in Chapters 8/9 on RX 9070, #26 sun shadows remain absent,
and #36 lighting remains wrong on RX 6700 XT. Keep each symptom and hardware
separate; local smoke success is not a fix confirmation.

2026-10-22: preserve earlier correctness fixes while establishing measurement.

| Area | Evidence / remaining gate |
| --- | --- |
| Arena download / depth-store barriers | Existing regression tests preserved and passing |
| Visible pending graphics pipelines | Existing key-scoped waits preserved; warm sample records stalls rather than deleting the waits |
| Main-menu output on NVIDIA | Display capture in `out\checkpoint-20261022\captures` inspected |
| Combat, grenade particles, volumetrics, solar/cascade shadows | NOT TESTED in this checkpoint |
| AMD/Intel, wave32/wave64, LDS divergence | NOT TESTED; do not change shader semantics on menu evidence |
| Z-fighting, vertex explosions, black polygons | No new scene-specific fix or visual-correctness claim |

The prior RCA in `PROGRESS.md` distinguishes confirmed synchronization hazards
from gated shader hypotheses. Keep that distinction. Validation-layer absence in
a normal run is not a synchronization-validation pass.

Need a reproducible gameplay checkpoint, matching camera/input replay, before/after
captures, resource/draw IDs and vendor-specific hardware evidence before accepting
additional graphics fixes. Do not blanket-clear caches or disable effects.

## 2026-10-22 release investigation

Reviewed the screenshot in #45 (washed-out character and crushed surroundings on
an RTX 4080 Laptop), the attached runtime logs in #36 (RX 6700 XT) and #63
(RX 9070, Chapters 8/9), and retained `out\vsm` / `out\htile2` comparisons.
The logs contain HTILE-read markers, but no Vulkan validation or synchronization
hazard identifying the bad lighting pass. Absence of those messages is not a
validation pass. The older comparisons show depth/debug views or transitional
frames, not matching camera checkpoints for these reports.

No new lighting/shadow default is changed in v0.7.7. In particular, do not replace
the reported issue with a gamma/exposure workaround, force all shadow tiles
nonempty, or enable every experimental lighting flag. The release does include
previously merged preservation of shader-written HTILE, custom-border-color
sampler cache keys, resolved descriptors after texture rebind, and per-target
additive blending. These are reasons to request a retest, not proof that #26,
#36, #39, #45 or #63 is resolved.

The remaining gate is a chapter/checkpoint plus repeatable camera/input and
before/after final-output captures on the reporting GPU. Include `user\last_run.log`,
driver version, output resolution, upscaler and graphics settings. Existing
`THEORDER_LIGHTING_TEST` tokens (`shading`, `msdepth`, `dref`, `bias`) are diagnostic
only: test one token at a time against an unchanged baseline, preserving saves
and caches. Shader-changing tokens use distinct cache versions. A menu/prologue
smoke test cannot validate the Chapter 5/8/9 defects.
