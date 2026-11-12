# Temporal upscaler inputs on CUSA00035 01.02 (T-040, #138)

Status: **done, 2026-11-12** (static half earlier the same day, runtime half from 25 frame
captures of 4 scenes, sections 3–4). Diagnosis and specification only: nothing here changes the
renderer. Section 2 was written before the runtime captures; where they disagree, section 3 wins
(2x MSAA, not 4x; the 2D pass runs after FXAA).

Address convention: eboot addresses `A` as in `docs/ENGINE_RE.md` (Ghidra VA = 0x100000000 + A).
In `out/eboot_ghidra.exe` (sha256 `0f42dce6…146f`, built from eboot.bin sha256 `f742eb17…58d8`),
data past 0x1400000 sits at file offset A and code at file offset A + 0x400 (both checked with
REA `address_to_file_offset`: 0x10149f898 → 0x149f898, 0x10082b5a0 → 0x82b9a0).

## 1. Where the current heuristics come from

| Heuristic (code) | Value | Origin | Verified by |
|---|---|---|---|
| `TemporalUpscaler::trigger_hash` (`vk_temporal_upscaler.h:206`) | CS `0x9a9cf8a9` | God of War III Remastered port (`docs/upscaler.md` "God of War III's frame", 2026-10-10, 2560x1440 render; "Status": FSR before the post-processing combine 9a9cf8a9) | `git log -S9a9cf8a9`: first in `ea506243` (initial commit, 2026-10-14), the GoW3 baseline; never changed for The Order |
| `CameraMotion::OnConstants` (`vk_camera_motion.cpp:221`) | 864-byte buffer, `data[0]==3000` (far), `data[1]==1/far`, size at [4..5], view at [8..19], proj at [52,57,62,63], inverse view at [180..191] | Same GoW3 port; `docs/motion_vectors.md` calls 864 bytes "the shared scene buffer" of GoW3's G-buffer draws | `git log -S"3000.0f"` and `-S"864"` on `renderer_vulkan/`: only `ea506243` |
| `gbuffer_draw` (`vk_rasterizer.cpp:338`) | `popcount(mrt_mask) >= 5` and a depth target | GoW3 G-buffer (5+ targets) | same commit; `d36f8f4d` ("no God of War leftovers") removed launcher/docs leftovers but not these |
| scene color (`vk_rasterizer.cpp` ~344) | RGBA16F, ≤2 targets, drawn with the G-buffer depth | GoW3 | depends on `gbuffer_draw`, so it never fires on 01.02 |

Conclusion: none of the four detections was ever written for The Order. They are GoW3
(CUSA01623-era port) facts. On 01.02, #138's logs show: no 5+-target pass (every 3+-target draw
uses exactly 3), no 864-byte buffer, and `9a9cf8a9` is never dispatched. This matches a different
engine, not a different build of the same one.

## 2. The Order's frame, from the executable (static)

The renderer is Ready At Dawn's `rad07` engine (`d:/work/rad07/dev/main/src/libs/gs/cgrenderer.c`,
`cgfullscreeneffect.c`, `cgdisplay.c`, … in the string table). Its GPU marker names, in table
order at 0x14b3c0a–0x14b4012 (literal strings, read from the file), are the render passes:

| # | Marker / class | Shader name next to it | Notes |
|---|---|---|---|
| 1 | `Prepass` (`CGRenderPassPrepass`) | — | depth pre-pass; "Prepass material doesn't have permutation" |
| 2 | `Fill Non-MSAA Depth` | `fs_filltransparentdepth` | `k_non_msaa_depth` |
| 3 | `Alt FX`, `Damage` | — | `k_alt_fx_map`, `k_damage_map` |
| 4 | `Shadow Casting Lights`, `ShadowMap Rendering`, `EVSM filtering`, `Shadow Cascade Lights`, `Shadow Cascade %u` | — | `k_cascade_depth_map0/1`, `k_shadow_depth_map0/1` |
| 5 | `Lighting Pass` (`CGRenderPassLights`) | — | |
| 6 | `LowresTransparents`, `Depth Downscale`, `Copy alpha` | `fs_depthdownscale`, `fs_copyalpha` | `k_lowres_depth*`, `k_lowres_transparents` |
| 7 | `Main Pass` (`CGRenderPassMain`): `Opaques`, `Alpha Test`, `Dynamic Opaques`, `Skydome`, `Skirts` | — | forward main pass; `k_main_pass_map` |
| 8 | `Transparent Pass`, `Low-Res Composite` | `fs_lowrescomposite` | |
| 9 | `2D Elements Pass` (`CGRenderPass2D`) | `finaldisplay_ps` | |
| 10 | `Resolve Pass` (`CGRenderPassResolve`) | `customresolve_cs`, `_temporal` (0x149f39b, 0x149f3ac) | MSAA custom resolve, with a temporal variant |
| 11 | `Full Screen Effects Pass` (`CGRenderPassFullscreenEffects`): `Bloom`, `Motion Blur` | — | |
| 12 | `Velocity Tile Generation`, `Velocity Neighbor Checking` | `fs_velocitytiles`, `fs_velocityneighbormax`, `fs_motionblur` | `k_velocity_map`, `k_velocity_tiles`, `k_velocity_tile_map_2..32` |
| 13 | `Depth of Field` (or `Depth of Field Disabled`) | `fs_downsample` | `k_dof*`, bokeh shapes |
| 14 | `Bloom Treshold` (sic), `Bloom Downsample/Blur H/Blur V/Upscale`, `Flare Threshold/Downscale` | `fs_bloom`, `fs_bloomblur`, `fs_flaredownscale` | |
| 15 | `Tone Mapping` | `fs_tonemapping` | `k_tone_mapped` |
| 16 | `FXAA` | `fs_fxaa` | `k_back_buffer`, `k_default_display` |

Caveat: this is the order of the strings in `.rodata`, which usually follows the source but is
not proof of execution order (the 2D pass in particular is listed before Resolve). The runtime
census has to confirm the order.

Render-target/resource names (hashed symbols, 0x149f700–0x14a0000) include `k_main_pass_map`,
`k_velocity_map`, `k_prev_frame_map`, `k_default_depth`, `k_non_msaa_depth`, `k_depth_normals`,
`k_normals_map`, `k_vtx_normals_map`, `k_highres_map`, `k_tone_mapped`, `k_iggy_map` (Iggy UI),
and `k_bone_cache_curr` / `k_bone_cache_prev` / `k_bone_cache_vect` (0x149f300..).

What this means for the upscaler (inferred, not yet observed):

- **The game already renders a velocity buffer** (`k_velocity_map`, input to its tile-based motion
  blur) and keeps the previous frame's skinning (`k_bone_cache_prev`). If that buffer holds
  per-pixel screen motion of camera *and* objects, it is a better FSR motion input than the port's
  own camera reprojection plus object-motion replay. Its format, scale and sign are unknown.
- **The game already has a temporal resolve** (`customresolve_cs` + `_temporal`, `k_prev_frame_map`).
  The Order runs MSAA (2x on this setup, section 3) with a custom resolve; the scene color the upscaler wants is the
  *input* of that resolve's output stage, i.e. the resolved HDR image before `Full Screen Effects`.
  The upscaler should replace or follow the resolve, before motion blur, DoF, bloom and tone
  mapping; the HUD (`2D Elements`, Iggy) must stay out.
- The two compute shaders seen at 240x100 groups (1920x800 at 8x8) in #138, `473a607b` and
  `92311aca`, are candidates for the resolve and for a full-screen effect; which is which is not
  known. The trigger the T-041 spec needs is "after the resolve CS writes the 1920x800 HDR
  image", not a GoW3-style "before the combine".
- The 3-target main pass with 1920x800 depth is consistent with a forward renderer (color plus two
  auxiliary targets, one of which may be `k_velocity_map` or normals). The scene depth is the
  depth of `Prepass`/`Main Pass` (MSAA) or `k_non_msaa_depth` after `Fill Non-MSAA Depth`.

### Camera projection on the CPU

`sub_8c68e0` (one of the projection builders listed in `ENGINE_RE.md` "Field of view"; REA
`procedure_pseudo_code 0x1008c6a44`) builds the camera matrices from a camera object `c`:

- `c+0xa8` horizontal FOV (16:9 based, turned vertical with `2*atan(tan(h/2)*0.5625)`),
  `c+0xac` near, `c+0xb0` far, `c+0xb4`/`c+0xb8` lens-shift angles, `c+0x98..0xa7` a rotation
  (passed to `sub_816270` to make a 4x4), `c+0x290..0x29b` position;
- projection terms: x = 2n / width, y = 2n / height (aspect from `*(0x16b54e0)+0x398`, or
  `+0x370 / +0x378` when zero), z = far/(far−near), w = −near·far/(far−near), row 3 = (0,0,1,0):
  **left-handed D3D-style, depth 0..1, not reversed**;
- the product with the rotation is stored as four 16-byte rows at `c+0xe0..0x11f`, and
  `sub_6b67e0(c+0x120, c+0xe0, 0)` derives a second matrix (probably the inverse) at `c+0x120`.

So in GPU constants the projection should be recognizable by `m[2][2] = f/(f−n)`,
`m[2][3] = −n·f/(f−n)` (or transposed) and `m[3][2] = 1`, with `m[1][1]/m[0][0] = 2.4` at the
default aspect. The GoW3 test (`data[0] == 3000`) cannot match this layout. Who copies `c+0xe0`
into a GPU constant buffer was not traced (no direct code reference found; the GPU buffers are
probably filled from a per-view struct by the `cgsrtdraw_ps4` / `cgrawinterface_ps4` code).

### Method notes (static)

- Strings: REA `search_strings` (regex) over Ghidra's string inventory, then a Python dump of
  the file around the hits; names sit after an 8-byte hash and a truncated `RaDsSyMbOlPrEfIx`.
- Ghidra reports no references to the marker or shader-name strings, and a scan of the code for
  rip-relative displacements and absolute 8-byte pointers to them found none either: they are
  reached indirectly (hash tables or a string table), so no function was tied to a pass name.

## 3. Runtime: the 01.02 frame (pass table)

Build: `out/theorder-probe.exe` of 2026-11-12 02:35, branch `feat/level-start`, RTX 5060 Laptop,
NVIDIA 621.14, output 1920x1080, defaults (upscaler setting on, but it never triggers), warm
profile copied from `user`, n=1 run per scene. Frame captures: `FrameCapture` text dumps
(`THEORDER_CAPTURE_TRIGGER`), re-armed in bursts by a scratch helper so that captures land 2 flips
apart. Image numbers (`img#N`) are the texture cache's stable IDs inside one run; addresses and
draw counts differ per scene, roles and formats do not.

| Run | Scene | Manifest (`out/batch/manifests/`) | Captures (flips) |
|---|---|---|---|
| wch | `--level wch_080_ambush --checkpoint wch_080_checkpoint_005`, `rx=255` held 64–74 s | `regression-20261112T204616Z.json` | 2814, 2816 (still); 3125/3127/3129, 3343/3345/3347 (turning) |
| pro | prologue opening cutscene (title route `24..40 s: cross`) | `regression-20261112T205130Z.json` | 2497/2499, 2977/2979, 3457/3459 (one shot), 3936/3938 (after a cut) |
| atr | `--level may_040_atrium --checkpoint may_040_checkpoint_010` (HUD on screen), `rx=255` 66–74 s | `regression-20261112T205323Z.json` | 3009/3011, 3334/3336/3338 |
| cov | prologue cover tutorial (title route to 60 s) | `regression-20261112T205518Z.json` | 4063/4065, 4649/4651 (these two carry the cutscene camera of pro, near 0.01: the route timing is approximate) |

Capture files: `D:\theorder-bench\cap-t040-<run>\<run id>\frame-analysis\frame_*.txt` (kept;
cited below), screen dumps next to them (`diffs\display_*.preview.png`).

Pass order of one gameplay frame (wch, flip 2814; the other 24 captures have the same structure,
only draw counts differ). "samples" is the census field `samples c<color samples> d<depth> r<raster>`.

| Order | Role (marker it matches) | Shaders | Targets / output | Reads |
|---|---|---|---|---|
| 0 | previous frame to display (letterboxed copy) | VS `b9c87fc1` PS `a41511a2` | display buffer R8G8B8A8Srgb 1920x1080 | img#6 |
| 1–3 | **Prepass**, 363 draws (334 in atr), samples `c1,2,1 d2 r2` | VS `a989f410`, … | **img#3 A2B10G10R10 1920x800, 1 sample (velocity)**; img#5 A2B10G10R10, 2 samples; img#4 R16Uint, 1 sample; **depth img#238 D32Sfloat 1920x800, 2 samples** | — |
| 5 | **Fill Non-MSAA Depth** (full-screen) | VS `0ece32eb` PS `ba0b4687` | **img#72 D32Sfloat 1920x800, 1 sample** | img#238 |
| 7 | depth query to 1x1 R32F | PS `f77d3cf1` | img#7 R32Sfloat 1x1 | img#238 |
| 9 | 70 more draws into the velocity target | VS `6f6894fe` | img#3, own depth img#234 D32Sfloat 1920x800 (1 sample) | — |
| 12–65 | shadow maps (512x512 D32), EVSM/blur CS (`8ea0ef5b` `fcddd82d` `2cc821bb` `39b36fff` `f93acac2`), light/AO tile CS at 120x50 / 60x25 (`d2bb4884` `f371a798` `c1a458ad` `b10111eb`) | | | img#238, img#4, img#72 |
| 67–82 | **Main Pass**, 363 draws + small forward passes, samples `c2,1 d2 r2` | VS `527b7ca4` PS `cdda337e`, … | **img#17 RGBA16F 1920x800, 2 samples (HDR scene color)**; img#243 A2B10G10R10, 1 sample; depth img#238 read-only | img#14 (lighting), material textures |
| 69–75, 84 | LowresTransparents (960x400 img#16 with depth img#73 960x400) and **Low-Res Composite** into img#17 | PS `7b95d387` (composite) | img#17 | img#16, img#73, img#234 |
| 85 | **Resolve Pass = CS `92311aca`** (`customresolve_cs` temporal), 240x100 groups of 8x8 | | **img#21 RGBA16F 1920x800, 1 sample (resolved HDR)** | img#20 R32F 1x1 (exposure), img#3 velocity, img#17 (as `sampler2DMS`), img#74/img#76 RGBA16F 1920x800 (history) |
| 87 | one draw into img#21 with depth img#72 | VS `35aa217d` PS `b047652a` | img#21 | |
| 88–94 | **Velocity Tile Generation / Neighbor max / Motion Blur** | PS `c1737167` (tiles), `5ba4e1ae`, `ea102418`, `574764e2` (motion blur) | R16G16F 960x400 → 480x200 → … → 60x25; motion blur → img#22 RGBA16F 1920x800 | img#3, img#21 |
| 95–102 | **Depth of Field** (960x400 near/far) | | img#23 RGBA16F 1920x800 | img#22, img#234 |
| 103–105 | luminance reduction | CS `af9a3732` `e10bc8a9` `c799dd1c` | img#20/img#26 R32F 1x1 (exposure, read next frame) | img#23 |
| 106–135 | **Bloom / Flare** | | B10G11R11 960x400, 256x256 | img#23 |
| 136 | **Tone Mapping** | VS `0ece32eb` PS `4403da37` | img#32 R8G8B8A8Srgb 1920x800 | img#23, bloom, img#34 (lens dirt) |
| 137 | **FXAA** (inferred from the marker order) | VS `1422b2b5` PS `2b1826e7` | img#6 A2B10G10R10 1920x800 | img#32 |
| 138+ | **2D Elements** (HUD; atr shows the prompt here) | VS `37573b18` | img#6, depth img#72 | |

The string order of section 2 holds, except that the 2D pass comes after FXAA, not before the
resolve. The frame has 2x MSAA (not 4x as guessed above): every geometry pass reports 2 depth
samples.

Evidence for the velocity role of img#3: written by the pre-pass (one sample, like every vector
target), read by the resolve, by the tile chain whose outputs are 2-channel R16G16F at 1/2 … 1/32
size, and by the motion blur. The tile shader decodes it (section 4).

## 4. Inputs for a temporal upscaler

### Trigger

- **Trigger: CS `92311aca`.** It is the temporal MSAA resolve: SPIR-V from the port's cache
  (`user/cache/CUSA00035/0x0000000092311aca_0.spv`, decompiled with `spirv-cross`) reads one
  `sampler2DMS` (img#17), three samplers (exposure, velocity, history bilinear) and writes one
  storage image (img#21). It runs once per frame in all 25 captures of the 4 scenes,
  gameplay and cutscene, before every full-screen effect.
- **`473a607b` is the non-temporal resolve** of the same pass: its SPIR-V reads only a
  `sampler2DMS` and one sampler (no velocity, no history) and writes one image. It was not seen in
  any of the 25 captures. In `out/batch/fgdbg-01-20261112T031347683953Z.log` (a #138 debug log that
  printed each compute shader's first dispatch) it ran in frame 1, before the first flip, and
  `92311aca` from frame 2 on. So `473a607b` is the resolve used when there is no history. It
  probably also runs after camera cuts; no capture shows that.
- `9a9cf8a9` (current `trigger_hash`) never appears: it is God of War III's.
- Moment: run the upscaler **in place of (or right after) `92311aca`**, i.e. after the
  Low-Res Composite into img#17 and before Motion Blur. HUD is drawn much later into img#6 and does
  not enter the scene images. Upscaling here also upscales motion blur, DoF, bloom and tone
  mapping, which then run at output resolution only if their targets follow (they are separate
  1920x800 allocations).

### Scene color and depth (how to recognize them)

- **Scene color (pre-resolve):** the storage/sampled RGBA16F 1920x800 image that `92311aca` reads as
  `sampler2DMS`: 2 samples, written by the main pass (2 color targets, 2-sample depth). FSR needs
  one sample. Options for T-041: resolve it first (the game's own `473a607b` does exactly that,
  without history), or take the resolved `92311aca` output img#21 (already temporally filtered by
  the game, so two temporal filters in a row).
- **Scene color (resolved):** the image `92311aca` writes (RGBA16F 1920x800, 1 sample).
- **Depth:** the 1-sample D32Sfloat 1920x800 written by the full-screen pass right after the
  pre-pass that samples the 2-sample pre-pass depth (img#72 here, VS `0ece32eb` PS `ba0b4687`).
  The 2-sample depth (img#238) is the pre-pass / main pass depth. Depth is D3D 0..1, not
  reversed (projection below).
- Exposure: the 1x1 R32F that `92311aca` samples (written by the luminance reduction of the
  previous frame).
- The aspect is 2.4: every scene target is 1920x800 inside a 1920x1080 display buffer.

### The game's velocity buffer

- Image: A2B10G10R10 Unorm 1920x800, 1 sample, the first color target of the pre-pass (also drawn
  by a later 70-draw pass with its own depth).
- Encoding, from the velocity-tile shader `c1737167` (decompiled SPIR-V, gathers of R, G, A):
  `v.x = s * R * (A_bit0 ? -1 : 1)`, `v.y = s * G * (A_bit1 ? -1 : 1)`, where `A_bit = uint(3*A)`,
  `s` = flat user data dword 19 = **16.0** (constant in every capture), then multiplied by dword 18
  = 0.5 for the half-size tile. That gives pixels at 1920x800, magnitude clamped to 16 px per
  axis with 10-bit precision (16/1023 ≈ 0.016 px). B is not read by this shader. `92311aca`
  has the same 0.5 and 16 at its user data dwords 62–63.
- Content: per vertex from the current and previous view-projection (the pre-pass VS receives both,
  below) and, for skinned meshes, `k_bone_cache_prev`. So camera **and** object motion, inferred
  from the shader inputs (not checked pixel by pixel).
- Not known: the direction (current→previous or the reverse) and the sign/axis convention relative
  to FSR's. A pixel readback of img#3 under a known camera turn would settle it.
- For FSR it replaces the port's camera reprojection and object-motion replay, with one limit:
  motions above 16 px per axis are clamped (fast camera turns at 1920x800 exceed that).

### Camera constants

**Per-view block, 1760 bytes (V# size), in guest memory**, rebuilt each frame. Seen at
0x104575db04 and 0x104575e1e4 (two alternating addresses). It is bound:

- as V# **slot 0 of the bloom pixel shaders `c02c15fc` and `0d7b3891`** (the only V# bindings of it
  that the capture shows, in every capture of all 4 scenes);
- through a user-data pointer (`s2:s3` = 0x10_4575db04 / 0x10_4575e1e4) to the tile/lighting compute
  shaders `d2bb4884`, `f371a798`, `c1a458ad`, `b10111eb`;
- geometry vertex shaders get the two view-projections copied into their flattened user data
  (pre-pass VS `a989f410`: dwords 60–75 current, 76–91 previous, identical to block floats 80–95 and
  144–159).

Layout (float index into the block; row-vector convention, `clip = (x y z 1) · M`; rows of 4):

| Floats | Content | Check |
|---|---|---|
| 0–2 | camera position | equals 76–78 |
| 4–15 | view rotation 3x4 | = transposed 48–58 rotation (by inspection) |
| **16–31** | **projection**: [16] = x scale (negative), [21] = y scale, [26] = f/(f−n), [27] = 1, [30] = −n·f/(f−n) | [21]/[16] = −2.40000 in all 25 captures; [26], [30] match [176]/[177] to 6 digits |
| 32–47 | inverse projection | [32] = 1/[16], [37] = 1/[21] |
| 48–63 | view (world→camera) 4x4 | 48–63 · 16–31 = 80–95 (max error 2e-4, wch flip 3127); 48–63 · 64–79 = identity |
| 64–79 | inverse view (camera→world) | translation 76–78 = camera position |
| **80–95** | **view-projection, current frame** | = VS user data |
| 96–111 | inverse view-projection, current | product with 80–95 = identity within print precision (0.03 on terms of ~300) |
| 112–143 | projection and inverse projection again (previous frame?; equal to 16–47 in every capture, the FOV never changed between frames) | |
| **144–159** | **view-projection, previous frame** | = VS user data 76–91 |
| 160–175 | inverse view-projection, previous | |
| 176, 177 | near, far | wch 0.11/700, atr 0.1/700, pro 0.01/50 and 0.1/50 |
| 178–181 | 0.4436, 45.6, f/(f−n), −nf/(f−n) (first two not identified) | |

Projection jitter: floats 24, 25 (the z-row x/y terms where a jitter would go) are 0 in all 25
captures. The game's temporal resolve runs without a jittered projection; T-041 has to add the
jitter itself (the port's viewport jitter).

Two consecutive frames with the camera turning (wch, `rx=255` held). Each capture carries frame N
(floats 80–95) and frame N−1 (144–159); captures are 2 flips apart:

| Flip | 80, 82, 83 (current) | 144, 146, 147 (previous) | yaw from view row 0 |
|---|---|---|---|
| 3125 | −0.90810, −0.15354, −0.15352 | −0.91899, −0.00054, −0.00054 | 8.83° |
| 3127 | −0.83073, −0.42768, −0.42762 | −0.87797, −0.29550, −0.29546 | 25.32° |
| 3129 | −0.68962, −0.66108, −0.66097 | −0.76757, −0.54999, −0.54991 | 41.37° |

The previous matrix of flip 3127 lies between the current ones of 3125 and 3127, the previous of
3129 between those of 3127 and 3129 (≈ 8° per frame). With the camera still (flips 2814/2816)
the two differ only in the 4th–5th digit (idle sway). Translation row (92–95) and the y scale
(85 = 2.20558) are as expected for a pure yaw. The same holds in the cutscene (pro, flips
2977/2979: 80 = 0.19403 → 0.20722, previous 0.18799 → 0.20201) and in atr.

### Gameplay vs cutscene

Same pass structure, same resolve `92311aca`, same velocity target, same 1760-byte block and
layout in the cutscene (pro) as in gameplay (wch, atr, cov). Differences are values only: near/far
(0.01/50 and 0.1/50 in the cutscene, 0.1–0.11/700 in gameplay) and FOV (y scale 3.93917 in the
cutscene and in atr, 2.20559 in wch, 2.75699 after the cut in pro). **A detector must not test a
fixed far plane or FOV** (the GoW3 test `data[0] == 3000` is the wrong kind of test). Robust tests
on the block: [27] == 1, [26] == f/(f−n) and [30] == −n·f/(f−n) computed from [176]/[177],
[21]/[16] == −(aspect) (2.4 unpatched; aspect patches change it), [76..78] == [0..2].

### Spec summary for T-041

1. Trigger on CS `92311aca` (temporal resolve); treat `473a607b` (resolve without history) as a
   history reset.
2. Color: the 2-sample RGBA16F that `92311aca` reads (resolve it), or its RGBA16F output.
   Depth: the 1-sample D32Sfloat 1920x800 made by the "Fill Non-MSAA Depth" pass.
   Exposure: the 1x1 R32F it samples. Output size 1920x800 (2.4:1), not 1920x1080.
3. Motion: decode the game's A2B10G10R10 velocity (R, G magnitudes × 16 px, sign bits in A), or
   keep the port's camera reprojection using block floats 80–95 / 144–159. The direction and
   axis convention of the game's vectors still need one readback.
4. Camera: the 1760-byte block found by content (above), via the V# of PS `c02c15fc`/`0d7b3891` or
   the user-data pointer of the tile CS; it arrives after the geometry passes in command order
   (bloom), so for the current frame it is available before the trigger only through the CS user
   data (`d2bb4884`, before the main pass) or the VS user data.
5. Jitter must be added by the port; the game's matrices are unjittered.

## 5. Unknown

- Direction, axis convention and exact unit of the game's velocity vectors (decoded from shader
  code, not checked against pixels); what B and the second pre-pass target img#5 (2 samples) hold;
  img#4 (R16Uint) is probably a material/object ID.
- Who writes the resolve's history image (img#74/img#76): no captured pass or dispatch writes it,
  and `92311aca` has a single image store. Probably a copy outside draws/dispatches (DMA), not
  traced.
- Whether `473a607b` also runs on camera cuts (only the first frame was observed).
- What the 70-draw pass into the velocity target with its own 1-sample depth (img#234) is.
- Floats 112–143 and 178–179 of the block; the second matrix at camera object `c+0x120`.
- Which CPU function writes the 1760-byte block (not traced in the eboot).
- FXAA for pass 137 is inferred from the marker order only.
- Gameplay captures are n=1 run per scene; behaviour at other output resolutions or with aspect
  patches was not captured.

## 6. Draft summary for #138 (for the maintainer to post)

> **Where the temporal upscaler's inputs are on CUSA00035 01.02** (details:
> `docs/TEMPORAL_INPUTS_01_02.md`)
>
> The upscaler's detection came from the God of War III port: the trigger CS `9a9cf8a9`, the
> 5+-target G-buffer and the 864-byte camera buffer starting with far = 3000 are all God of War
> III facts (they arrived with the initial commit). The Order's RAD engine has a different frame.
> Frame captures of four scenes (Whitechapel and the Atrium via `--level`, the prologue cutscene
> and cover tutorial; 25 frames, n=1 run each) show:
>
> - The game already has a **temporal MSAA resolve**: CS `92311aca` (one of the two 1920x800
>   passes) reads the 2-sample RGBA16F scene color, a velocity buffer, the exposure and a history
>   image, and writes the resolved RGBA16F 1920x800 image before motion blur, DoF, bloom and tone
>   mapping. `473a607b` is the same resolve without history (seen on the first frame). This is
>   the trigger for the upscaler.
> - The game already renders **velocity**: A2B10G10R10 1920x800 from the depth pre-pass (the
>   3-target pass), decoded by its motion-blur shaders as ±16 px per axis with the signs in alpha.
> - **Camera**: a 1760-byte per-view block with projection, view, inverse matrices and the current
>   and previous view-projection, near/far at floats 176/177. It was found in gameplay and in the
>   cutscene, with the matrices following an 8°-per-frame camera turn. Near, far and FOV change per
>   shot, so no fixed value can identify it. The game does not jitter its projection.
> - Scene depth: the 1-sample D32 copy of the 2-sample pre-pass depth.
>
> Next: T-041 builds the detection on these (with a rollback switch) and checks the velocity
> direction with a readback. Nothing has changed in the port yet.
