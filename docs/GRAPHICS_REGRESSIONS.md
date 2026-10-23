# Graphics regression checkpoint

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

## 2026-10-22 O5 symptom matrix

| Issue | Hardware/build/scene evidence | Log/capture evidence and limits | Status |
| --- | --- | --- | --- |
| #45 | RTX 4080 Laptop, v0.7.6; washed-out characters and crushed environment. Driver, scene, output mode, HDR/sRGB and upscaler are unknown. | No usable `last_run.log` or pass capture; cannot trace exposure/histogram, additive targets, or final color conversion. | Inconclusive; request one fixed scene and a frame capture. |
| #43 | RTX 3060 Ti; black/white or dark output near startup across reported 0.7.x builds. | No log or first-frame capture in the available triage record; keep separate from #47's AMD case. | Pending first-frame composition/presenter capture. |
| #47 | RX 6500 XT, driver 26.10.1, v0.7.6; black screen with text visible. A v0.7.7 retest was requested Oct 22; no result is present in the accessible issue page. | No `last_run.log`, output mode, CUSA, or presenter/framebuffer capture. | Pending reporter retest; no confirmed fix. |
| #36 | RX 6700 XT, v0.7.5; generic lighting report without a chapter. Log identifies CUSA00035 01.02, eboot `f742eb17…`, 1920x1080, FSR 3.1 on. | 65 logged stall records, 28 over 100 ms, worst 761.5 ms; max GPU-tick wait 53.5 ms. Two HTILE-read markers do not identify the bad pass; no validation error or frame capture. | Inconclusive; lighting symptom not localized. |
| #39 | RX 9070 / Adrenalin 26.8.1, v0.7.5, Chapter 5. Log identifies CUSA00076 01.02, eboot `a8ce5fe1…`, 1920x1080, FSR 3.1 on. | 171 logged stall records, 80 over 100 ms, worst 1048.9 ms; max GPU-tick wait 14.2 ms. Log has no VSM shader hash, pass capture, or Vulkan validation error. | Green/blue flares remain unlocalized. |
| #63 | Same RX 9070, driver, CUSA and eboot; retested after v0.7.7 in Chapters 8/9, FSR off at 2560x1440. This differs from #39's scene, build and output settings. | Chapter 8/9 logs contain 177 stall records, 137 over 100 ms, worst 7759.1 ms; max GPU-tick wait 11.6 ms, readback wait 0, host-copy max 3.7 ms. Concurrent driver-compile totals can exceed a frame and do not attribute its stall. No Vulkan validation error or pass capture. | Artifacts and stutter persist after v0.7.7; cause unresolved. |

The downloaded issue logs were reviewed locally and are not retained in the worktree;
only the sanitized facts above are recorded. Their frame-stall summaries lack the
pass, resource and image IDs needed to associate a long frame with a green/blue
artifact. None of these cases supplied a logged HDR/sRGB mode. Therefore #45, #36,
#39, #43, #47 and #63 remain unconfirmed; no gamma, exposure, shader or vendor
workaround is justified by these records.

For a reporter-side capture, keep the original save and settings, fix one scene,
camera/input, resolution and upscaler state, then use `tools/batch_runs.py` with
`--frame-trace --present-frames 3 --frame-capture-at <same-time> --validation`.
Capture the untouched baseline first; test only one diagnostic toggle in a separate
run and include a fresh redacted `last_run.log`, CUSA/eboot, GPU/driver and scene.
