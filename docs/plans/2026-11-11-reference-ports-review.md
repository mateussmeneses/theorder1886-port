# Reference ports review (2026-11-11)

Compared against `master` e60ce20:

- **A**: DivinusX/a-port-of-the-order-1886-thats-actually-alive @ a674beb (v0.2.17). A Linux
  package of *this* lineage (no `src/`/`gpu/` tree; patches live in `docs/verification/*.patch`).
- **B**: IgorAugustoHH/The-Order-1886-PC-Port (TOport) @ ceebdf3 (v0.1.7). Sibling lineage of the
  same BBPort/shadPS4 base. Built locally (C:\tb\toport) and run on the same dump and route.

Method: env-var inventory (209 `BB_*` in B vs 238 here; 89 names only in B, each classified),
release notes v0.1.0–v0.1.7, credits, code diffs of the differing subsystems, and runs of both
ports on CUSA00035 01.02 with the prologue pad route (`24..60 s: cross`), English.

## Measured: both ports, same machine and route

RTX (10de:2d59), 60 Hz cap, 1080p, 29 five-second windows per 150 s session. Each port starts
from the same save with an empty shader cache (session 1), then reuses that profile (session 2).

| Session | Port | windows > 100 ms | windows > 50 ms | new driver pipeline builds |
|---|---|---|---|---|
| 1 cold | ours | 15 | 16 | 113 keys |
| 1 cold | TOport | 3 | 4 | (counted as ~700 compiles, front-loaded) |
| 2 warm | ours | **7** | 8 | 39 keys, 161 ms mean, none repeated from session 1 |
| 2 warm | TOport | **0** | **0** | "297 of 305 pipelines built at start, 8 on first use" |
| 3 warm | ours | 7 | – | 38 keys, none repeated from session 2 |
| 2 warm | ours, `THEORDER_PIPELINE_BINARIES=0` | 4 | 4 | 49 keys, 169 ms mean |

Reading: our saved pipelines are reused (0 repeated keys), so the hitches after a restart are
**pipelines that did not exist before**, ~40 per session, each a full ~160 ms driver compile
that a visible draw waits on. TOport meets only ~8 unseen ones. The permutation log shows ours
differ in one image slot (sRGB fmt 37 vs 9, or 2D vs 3D view: `image 31 type 9/9 ... srgb
true/false`) and compute 0x9846aaff buffer 0 stride (16256 vs 4). Both ports have the same
specialization fields, directional matching and implausible-T# filter, so the source of the
extra combinations is still open (next: `THEORDER_PERMUTATION_LOG=1` on both ports, same
draw, compare the T# words).

Side finding: with pipeline binaries on, the driver `VkPipelineCache` we save is **60 bytes**
(header only; the driver does not fill it in CAPTURE_DATA mode). With them off it is 20 MB, as
TOport's 12 MB. It did not make new combinations faster on this driver (169 ms), so issue #102's
"null cache" premise is fixed in code but the cache is empty in the default mode.

## Ported in this session (v0.8.2)

| Change | Files | Default | Evidence |
|---|---|---|---|
| Damaged shader-cache blob no longer aborts (B 154a2e6) | `common/serdes.h`, `vk_pipeline_serialization.cpp` | on | `tests/test_serdes_damaged.cpp`, fails on the old abort |
| SRT walker: failed emission rolled back (`Common::TryEmit`), unresolved **descriptor** lookups get a null slot (B `BB_SRT_GARBAGE`) | `flatten_extended_userdata_pass.cpp`, `common/try_emit.h`, `ir/srt_resource_use.h` | on (`THEORDER_SRT_GARBAGE=1`: old) | prologue: 2816 unresolved lookups / 100 s, **all data, 0 descriptor**: the null-slot half changes nothing there; the push-without-pop fix is a latent stack bug |
| RAM guard: quit with a log line before Windows freezes (B `BB_RAM_GUARD_MB`) | `src/probe.c` | 700 MiB (`THEORDER_RAM_GUARD_MB=0`: off) | not reproduced here |
| `THEORDER_PENDING_DRAWS=skip`: never wait for a pending pipeline (B "async-skip") | `vk_rasterizer.cpp` | off (wait, as now) | see below |
| `THEORDER_DLSS_PRESET` (B: K=11, F=6 when upscaling the finished frame) | `vk_dlss.cpp` | 0 = driver (unchanged) | not checked on an RTX card |

## Findings

1. **Unresolved SRT data lookups read user data 0.** With `THEORDER_DIRECT_MEMORY_ACCESS` off
   (our default since v0.6.1, "experimental"), an unresolved `ReadConst` returns
   `flatbuffer[0]` (the low dword of a pointer) as material/light data:
   `emit_spirv_context_get_set.cpp` `EmitReadConst`. Shaders 0xf5c17eec, 0xd5df0cdc, 0x493fdd71
   (14 lookups each), 0xb2e8fc5b, 0x8c8a4817... B runs with DMA **on** by default plus two cheap
   paths (`BB_DMA_CLEAN_SKIP`, `BB_DMA_SYNC_GENERATION`: skip the resident-range walk while no page
   became CPU-modified). One A/B here (DMA on, first session with the DMA shader variants): 2.6 s
   of compiles vs 0.6 s and one 4 s hitch; needs a warm second session before any verdict.
2. **Stutter policy differs.** Here a visible draw (>= 256x256 target) waits for its pipeline
   (964bd03, the first-visit flicker fix "Class C"); B skips it. Our warm-template run showed
   ~200 ms hitches in windows with 0 counted compiles: `Pipeline: 136-180 ms in the driver in the
   background (compiled, not stored yet)` + `STALLED` draws (issue #113's signature).
3. **Portuguese on CUSA00035 boots to a permanent black screen** (32 draws/frame), engine
   language 7 *and* 17: the dump has no package for either (`1cf8f1244dcc1dd4` for 7,
   `b0b4572d23e56916` for 17: `open(...) -> not found`, then `Encountered an error`). English
   boots. Edition-specific (the reporter's PT install works with subtitles); a launcher check
   that falls back to English when the language package is missing would turn it into a warning.
4. B features with no equivalent, not ported: area prefetch + Oodle package index (a5327ef,
   ~1.7k lines, calls the game's Oodle decoder at eboot +0xe804d0/+0xe80640/+0xe81790 from a host
   thread: verify offsets first), disk program index (`BB_DISK_PROGRAMS`), compute skip only for
   recurring programs (we use fallback dispatches instead), preload screen with progress,
   `BB_TIMESCALE`, shared pipeline map hit rate. Bloodborne-only: `BB_SOUND_HACK`.
5. Already here (no action): sun staging sync, HTILE expand, sampler border colour key, point
   light guard 0x804aeead, 7.1 downmix, indirect index clamp, driver `VkPipelineCache` (#102),
   deferred modules + meta prefetch, addr64 loads, 1440p/4K output, 21:9/32:9.

## Credits in B's README, checked

JulioCacko (index clamp): here. mfrafael (1440p/4K output): here. illusion0001 (resolution and
aspect patches): `patches/TheOrder1886-Orbis.xml`. shadPS4 PR #4798 (NVIDIA SSBO barrier): here.
artemkaVG / shadPS4 #5218 (light guard): here. ShadowOfTheColossus-PC, HayatoG, bbhost:
references only.

## Building B locally

Three local fixes were needed with this MSYS2 (none affects B's code paths): clone under a short
path with `core.longpaths` (FSR generated headers exceed MAX_PATH), our
`gpu/patches/fsr-vulkan/0003-ffx-sdk-include-libc-headers.patch`, `<cstdlib>` in sirit
`stream.h`, and `-Wno-unused-but-set-global` (newer clang).

## After v0.8.2: why TOport meets fewer new pipelines (2026-11-11)

- TOport's permutation trace (`BB_PERM_TRACE`) only logs the synchronous path, so its "1 new
  permutation" in a session is not comparable; the specialization fields, directional matching
  and implausible-T# filter are the same in both ports (`specialization.h`).
- What differs is *when* combinations are met: TOport compiles ~700 in its first session and
  ~8 later; it prepares programs long before their first use (`Disk program ready: ...
  prepared 36349 ms before use`, area prefetch from the package being streamed). Ours meets
  ~40 unseen combinations in every later session (none repeated).
- The area prefetch is portable here: the three Oodle entry points it calls match byte for byte
  in our CUSA00035 01.02 eboot (prologues at +0xe804d0 create, +0xe80600 free, +0xe80640 decode;
  checked on `out/eboot.elf`). TOport refuses to run them when a prologue differs.
- Next step: port the package index (`runtime_oodle_index.c`) and the area prefetch
  (`vk_pipeline_cache.cpp` a5327ef) behind `THEORDER_AREA_PREFETCH`, off by default, and
  measure session 2/3 hitch windows A/B on the prologue route.

## Root cause found: the particle SRT fix never applied here (2026-11-11)

The four programs with 116-126 permutations each (0xc75dd317, 0xad441fb1, 0xa8792bbb,
0x8c8a4817) are TOport's "billboard VS programs with 16 guarded GI texture pairs", whose unused
tail held recycled T#s. We already had TOport's `bb_particle_srt.h` (on by default), but a
diagnostic showed every call rejected on `info.buffers.size()`: our recompiler declares one buffer
fewer (7/9 vs TOport's 8/10). Accepting that count, all calls apply (`active 7`, as TOport's
"live guard count is 7"). Same route, three sessions in a row, cold profile first:

| | before | after | TOport |
|---|---|---|---|
| windows > 100 ms, session 1 / 2 / 3 | 15 / 7 / 7 | **3 / 1 / 1** | 3 / 0 / – |
| driver pipeline builds after session 1 | 38-39 | **0** | ~8 |
| stored permutations | 821 | **472** | 398 |
| stored pipelines | 638 | **341** | 305 |

Picture: the prologue cutscene renders normally with the fix (no visible artefact in a frame
dump); not compared pixel for pixel (the two dumps caught different moments).
`THEORDER_PARTICLE_SRT_FIX=0` disables it. The area prefetch is no longer the next step for #113.

## Flicker A/B, prologue cover tutorial (2026-11-11)

Method: `tools/flicker_scan.py` on ~90 consecutive presented frames (`*_FRAME_DUMP_TRIGGER`
rewritten every 2 ms), A-B-A pattern (a pixel changes for one frame and returns), checked against
a planted one-frame object (found, 0.31 % of the triple; real scene 0.000). Both ports reach the
"COVER TRANSFER" tutorial (static camera for over a minute) from the same save: ours at ~50 s,
TOport at ~60 s, when our profile holds only `savedata` (a full template profile loads another
point).

| Build | Distinct frames / 90 | ABA, max per triple | Where |
|---|---|---|---|
| ours, no patches | 92 | 0.000 % | — |
| ours, 60 FPS patch | 92 | 0.013 % (every triple) | HUD tutorial text edges (x 1556-1866, y 320-404): half-pixel text shimmer |
| TOport (30 FPS) | 46 | 0.000 % | — |

No object flicker in this scene in either port. The reported flicker (#95, #99, #29, #30) needs
scenes this route does not reach (combat, Whitechapel): a save at such a checkpoint is the next
input. The HUD text shimmer only appears with the 60 FPS patch.

### 16:9 retest (#29)

With `aspect_ratio=16:9` actually applied (`Patches: ... 'Aspect Ratio - 16:9'`), two runs on the
same scene: 92 distinct frames each, ABA max 0.011 % per triple, all of it the same HUD text edges
(x 1512-1886, y 308-404). No object flicker with 16:9 either. (A first attempt never presented a
frame: it stalled during boot allocation while free commit memory was down to ~4.7 GB; the two
repeats with ~30 GB free were normal.)
