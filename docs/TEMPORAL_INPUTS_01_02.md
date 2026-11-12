# Temporal upscaler inputs on CUSA00035 01.02 (T-040, #138)

Status: **partial, 2026-11-12.** The static part (where the current heuristics come from, and
what the 01.02 executable says about its own frame) is done. The runtime part (pass census of a
gameplay and a cutscene frame, camera constants from two moving frames) could not be run: the
machine had 0.4–3.1 GB of free RAM during the session and the port's RAM guard stopped the game
before its window opened (see "Blocked" below). Nothing here changes the renderer.

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
  The Order runs 4x MSAA with a custom resolve; the scene color the upscaler wants is the
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

## 3. Pass table, trigger, scene color/depth, camera constants (runtime) — NOT DONE

Required by the acceptance criteria and still open:

| Item | Status |
|---|---|
| Census of a gameplay frame (S2, cover tutorial) and of a cutscene frame (S3) | not run |
| Hash and moment of the correct trigger | candidates only (`473a607b`, `92311aca`, the resolve) |
| How to recognize scene color and depth | inferred from names only (section 2) |
| Size, stage, slot and offsets of the camera matrices, ≥ 2 scenes | not found |
| Two consecutive frames showing the matrix change with the camera | not captured |
| Gameplay vs cutscene layout | not compared |

### How to run it (prepared, not executed)

The frame analyzer already dumps what is needed: every pass with targets, formats, sizes, the
census line, shaders, sampled/written images, and the first 1 KiB of every buffer bound to the
first four draws/dispatches of each pass (`FrameCapture::Buffer`, `vk_frame_capture.cpp`;
`BindBuffers` calls it for ring and V# buffers of every stage whenever a capture is armed,
independent of `gbuffer_draw`). `batch_runs.py --frame-capture-at` arms one capture; a helper
that re-creates the trigger file as soon as the game consumes it gives near-consecutive frames
(the session's script: `rearm.py CAPTURE_ROOT "T:N ..."`, not committed).

```
python <rearm.py> D:/theorder-bench/cap "30:1 40:1 48:1 56:1 64:3 74:3 82:1" &
python tools/batch_runs.py --runs 1 --secs 90 --cond t040a=THEORDER_LANGUAGE=1 \
  --pad "24:cross:0.5 28:cross:0.5 32:cross:0.5 36:cross:0.5 40:cross:0.5 50:cross:0.5 60:cross:0.5 72:rx=255:8" \
  --scene "prologue timeline (T-040)" --issue 138 --cache-state cold \
  --profile-root D:/theorder-bench/prof --data-root D:/theorder-bench/data \
  --capture-dir D:/theorder-bench/cap --report-dir D:/theorder-bench/rep \
  --frame-capture-at 9999 --frame-dump-at 30 --frame-dump-count 10 --frame-dump-spacing 6 \
  --raw-format rgba --cusa CUSA00035 --game-version 01.02 --game-dir C:/games/CUSA00035
```

(`--frame-capture-at 9999` only sets `THEORDER_CAPTURE_TRIGGER`; the helper does the arming.
`rx=255` held 8 s turns the camera in the cover tutorial; the opening cutscene supplies camera
motion without input.) Then, per capture file: list passes in order; find the 1920x800 HDR
image written by a 240x100-group CS (resolve) and the first pass that reads it; in the buffers of
the `Prepass`/main-pass first draws, look for 16-float runs with `m[3][2]==1` or `m[2][3]==1`,
`f/(f−n)` and `−nf/(f−n)` terms and a 2.4 ratio of the x/y scales; confirm by the same
offset changing between two captures while `rx` is held and staying fixed in still frames.

## 4. Blocked

`out/batch/manifests/regression-20261112T115948Z.json`, run `t040a`: `STOP system RAM almost
exhausted (399 MiB free, guard 700 MiB)` before the window opened (`src/probe.c` RAM guard).
Free physical memory over the next ~10 minutes stayed between 0.7 and 3.1 GB of 32 GB (other
applications: IDEs, a dev web server, WSL, browsers, the REA Ghidra session). The earlier #138
runs had ~22 GB free (`Memory: system RAM 21918 MiB free` in
`out/batch/d138-01-20261112T044056481126Z.log`). Disabling the guard was not considered: it
exists because Windows hangs when RAM runs out. Resume when ≥ ~12 GB is free.

## 5. Unknown

- Execution order of the passes (string order is only a hint).
- Which of `473a607b` / `92311aca` is `customresolve_cs` (and whether `_temporal` is the active
  variant on retail), and which pass reads its output.
- Format, encoding and content (camera only, or camera + objects) of `k_velocity_map`, and
  whether it is one of the three main-pass targets or a separate pass.
- GPU constant buffer, stage, slot and offsets of view/projection; whether the matrix in the
  buffer is the `c+0xe0` product or separate view and projection; whether cutscenes use the same
  layout.
- Whether the game jitters its projection for its temporal resolve (no "jitter" string exists;
  `c+0xb4/0xb8` lens shift is the only offset seen).
