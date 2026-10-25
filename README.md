# The Order: 1886 PC Native Port

**English** | [Português (Brasil)](README.pt-BR.md) | [Español](README.es.md)

[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20Me%20a%20Coffee-ff5e5b?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/mateusmeneses)

> [!TIP]
> ### ☕ Support the Development / Apoie o Projeto
> If you enjoy this project and want to support the ongoing development, reverse engineering, and optimization of **The Order: 1886 PC Native Port**, consider buying me a coffee! Every contribution motivates and directly helps keep the project active with frequent updates.
>
> 👉 **[Click here to donate on Ko-fi: https://ko-fi.com/mateusmeneses](https://ko-fi.com/mateusmeneses)** ☕❤️

A work-in-progress PC port of **The Order: 1886** (PlayStation 4) for Windows 10 and 11.

The CPU side is native: the game's original executable runs directly on the PC's processor (the
PS4 uses the same x86-64 architecture), and a runtime written for this port replaces the PS4
system libraries, so there is no CPU emulator. The graphics are translated: they run on the
[shadPS4](https://github.com/shadps4-emu/shadPS4) video core, which converts the PS4 GPU commands
and shaders to Vulkan. This project started as a copy of the
[God of War III Remastered port](https://github.com/FelipePrado0/gow3_pc) and is being adapted to
Ready at Dawn's engine.

> **No game files are included.** You need a dump of your own copy of the game.
> This project is not affiliated with Sony Interactive Entertainment, Ready at Dawn or Santa
> Monica Studio. The Order: 1886 is a trademark of Sony Interactive Entertainment.

## Status

Checked on Windows 11 with an NVIDIA GeForce RTX 5060 Laptop GPU and the dump **CUSA00035,
version 01.02** (`eboot.bin` sha256 `f742eb17…`):

- The game boots through the logos and the title screen into a new game; the prologue cutscene
  renders in the engine (characters, lighting and textures checked in frame dumps).
- A 4-minute run of the prologue held the game's own 30 FPS in every 5-second window, with a
  99th-percentile frame time of about 34.5 ms, and no crash in more than 20 consecutive automated
  runs (`tools/batch_runs.py`).
- The Vulkan validation layer reports a single remaining warning class in these scenes
  (`magFilter-04553`, a linear sampler used with an integer texture).

Not verified yet: gameplay beyond the prologue, sound (the automated runs do not check it),
saving and loading, AMD and Intel graphics cards, other regions and versions of the game, Linux.
The work log, the fixes and the open items are in [docs/PROGRESS.md](docs/PROGRESS.md).

## Requirements

- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX. On CPUs without MOVBE, BMI1 or LZCNT (Sandy/Ivy Bridge, AMD FX)
  the loader rewrites the game's instructions that use them to emulated stubs and logs an `ISA:`
  line; older CPUs stop at start with a `STOP: this CPU lacks` line.
- A graphics card with Vulkan 1.3 and a current driver.
- Your dump of the game: the folder with `eboot.bin`, `sce_module` and `sce_sys`. Only CUSA00035
  01.02 has been tested; the patch file also lists CUSA00070, CUSA00076, CUSA00100, CUSA00670 and
  CUSA00785.
- [MSYS2](https://www.msys2.org) and [Python 3](https://www.python.org) to build it (see below).

## Building

1. Install MSYS2 (`winget install MSYS2.MSYS2`) and, in the **MSYS2 CLANG64** shell, the packages
   listed in [packaging/windows/README.md](packaging/windows/README.md).
2. Clone with the submodules and build:

   ```bash
   git clone --recursive https://github.com/mateussmeneses/theorder1886-port
   cd theorder1886-port
   bash build.sh
   ```

   The result is `out/theorder-probe.exe` (also copied as `out/theorder-probe.exe`). If the build
   says it cannot replace the executable, a game process is still running.

## Playing

The game folder is taken from `THEORDER_GAME_DIR`; without it, `run.py` looks for a `CUSA00035`
folder next to the project folder.

```powershell
$env:THEORDER_GAME_DIR = 'D:\path\CUSA00035'
python run.py
```

### Engine options

These options exist in the game's executable; `run.py` passes them on. What they do in the retail
build is not verified yet: with `--level`, the game still stops at the title screen.

| `run.py` | Game option |
|---|---|
| `--level <name>` | `-startlevel <name>` (for example `cat_010_streets`, `emb_020_tunnels`, `air_010_rappel`, `pro_020_westminster`) |
| `--checkpoint <name>` | `-startcheckpoint <name>` |
| `--no-cloth` | `-disablecloth` |
| `--no-cam-springs`, `--no-cam-additive` | `-nocamsprings`, `-nocamadditive` |
| `--msaa <n>` | `-msaa <n>` |
| `--frontend` | `-frontend` |
| `--perf` | `-disablecloth -nocamsprings -nocamadditive -msaa 1` |

## Controls

Controllers (DualSense, Xbox and others) work through SDL3, together with the keyboard.

| Key | Button |
|---|---|
| WASD | left stick |
| Arrows | right stick (camera) and d-pad |
| Space | Cross |
| Enter | Options |
| Left Shift or Esc | Circle |
| E | Square |
| Q | Triangle |
| 1 and 3 | L1 and R1 |
| R and F | L2 and R2 |
| Z and C | L3 and R3 |
| I, K, J, L | d-pad |
| Tab and Backspace | left and right touchpad click |
| Insert | the port's menu (R3+L2 on a controller) |
| Mouse | camera; left button R2 (fire), right L2 (aim), middle R3, side buttons L1/R1 |
| F7 | mouse look on/off |

On a controller without a touchpad, View (Back) is the right touchpad click.
`THEORDER_KEY_MAP` and `THEORDER_PAD_MAP` change the buttons (`src/runtime_pad.c`).

The port's menu (Insert) holds the display, field of view, mouse, frame limit and performance
overlay settings. The field of view (50-200%) changes live; above 100% shows more on every side.

## Patches

`patches/TheOrder1886-Orbis.xml` is applied in memory at start; the game files are not changed.
Every patch is off by default.

- **Frame Rate Patch - 60 FPS** (experimental): sets the four 30 FPS frame-time constants (33333
  µs) of the 01.02 executable to 16667; the sites were checked in the binary. The game also asks
  for a 30 Hz presentation, so the port must present every vblank and cap at 60:

  ```powershell
  Set-Content fps60.ini 'engine_fps=120'   # the launcher logic falls back to the 60 FPS patch
  $env:THEORDER_CONFIG = (Resolve-Path fps60.ini)
  $env:THEORDER_FLIP_RATE = '0'; $env:THEORDER_FPS_LIMIT = '60'
  python run.py
  ```

  Game speed stays correct (frame dumps line up in time with a 30 FPS run). The logos and the
  title hold 60 FPS; in a 240 s run of the prologue every 5 s window averaged 41-60 FPS (57 overall),
  the dips where the GPU command thread is busy all frame (600-700 draws per frame).
- **Aspect Ratio - 16:9 / 16:10 / 21:9** (launcher and in-game menu: *Aspect ratio*, applied on
  restart): the game draws a 2.40:1 picture with black bars. These patches change the shape of the
  picture (the float 2.4 at 0x44ce8c) and the factor 0.5625 with which every camera turns its
  horizontal field of view (defined for 16:9) into a vertical one, so the view keeps the original
  width and shows more above and below (Vert+) instead of being cropped at the sides. The port shows
  only the picture, fitted to the window: a 16:10 or 21:9 screen is filled too. The scene has more
  pixels (1920x1080 instead of 1920x800 at 16:9); at 60 FPS in the prologue (RTX 5060 Laptop) that
  cost about 1% (57.8 against 58.3 FPS on average). The HUD and subtitles keep their original size
  and layout, centered in the taller picture: the patches give the UI (Iggy) the original 2.40:1
  height and move its drawing origin down (GDraw tile origin, 0x83a08a).
- The other patches written during earlier development were removed after testing: Skip Intro
  Logos changed nothing, Auto-Dismiss Title Screen left a black screen, the two Skip Prologue
  patches could not be verified, and the 900p, 720p and letterbox removal patches wrote inside
  instructions (enabling them corrupted the code).

## Settings and diagnostics

The graphics core reads these environment variables:

| Variable | Effect |
|---|---|
| `THEORDER_NO_MSAA=1` / `0` | single-sampled or multisampled game images (multisampled by default on NVIDIA) |
| `THEORDER_ASYNC_COMPUTE=0` / `1` | compute pipelines never built for this game before: always on the GPU thread / always in the background (dispatches skipped meanwhile; menus can lose text and button icons). Default: in the background only for programs that run every frame and write no image |
| `THEORDER_PIPELINE_BINARY_CACHE_MB=N` | size of the pipeline binary cache (default 4096); the oldest binaries beyond it are removed at start |
| `THEORDER_LAZY_SHADER_CACHE=0` | translates every shader again instead of loading the cached ones |
| `THEORDER_FLIP_RATE`, `THEORDER_FPS_LIMIT` | presentation rate overrides (see the 60 FPS patch) |
| `THEORDER_VK_VALIDATION=1` | Vulkan validation layer (with `VK_ADD_LAYER_PATH` pointing to it) |
| `THEORDER_GPU_LOG=info` | GPU log level |
| `THEORDER_PERMUTATION_LOG=1` | why each new shader permutation was compiled |
| `THEORDER_BIG_TRACK_LOG=1` | who tracks memory ranges above 256 MiB |
| `THEORDER_PAD_FILE=<file>` | scripted controller input for automated runs |

Saves and the shader cache are in the project's `user` folder (`user/savedata`,
`user/cache/CUSA00035`).

## Tests

```bash
python -m unittest discover -s tests          # 273 tests, all passing on 2026-10-15
python tools/batch_runs.py --runs 8 --secs 40 --pad "24:cross 28:cross 32:cross"
python tools/fault_stacks.py out/batch/*.log
```

`tools/batch_runs.py` starts the game repeatedly, can compare conditions (`--cond`), press buttons
(`--pad`, to get past the title screen) and reports faults, freezes and compile time;
`tools/fault_stacks.py` symbolizes the fault reports.

## How it works

| Part | Where | What it does |
|---|---|---|
| Preparation | `scripts/` | Reads `eboot.bin`, applies its relocations and links the game's own `libc` and `libSceFios2` into one image |
| Loader | `src/probe.c` | Loads the image and starts the game's original code |
| Runtime | `src/runtime_*.c` | Reimplements the PS4 system functions: memory, threads, files, audio, video, controller, saves and services |
| GPU | `gpu/shadps4/` | The shadPS4 video core with cuesta4's fixes and the God of War III and The Order ports' work (asynchronous shader and pipeline builds, shader cache) |
| Port menu | `gpu/shim/port_overlay.cpp` | The Dear ImGui menu and performance overlay |
| Patches | `patches/TheOrder1886-Orbis.xml`, `scripts/patches.py` | Applied in memory at start |

The launcher in `launcher/` and the Linux packaging in `packaging/linux/` come from the God of War
III port and have not been adapted to this game.

## Credits

- **Igor Augusto and TOport contributors**: [TOport](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port) provided the
  prior public implementations adapted here for [face-depth mitigation](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951),
  [particle SRT normalization](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951),
  [redraw depth bias](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/fff668c), and
  [EVSM/HTILE shadow handling](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/dfc66af).
  These were adapted and modified in this port; they are not solely original work here.
  See the [v0.7.3 attribution correction](docs/RELEASE-v0.7.3-ATTRIBUTION.md) (issue #69).

- **Felipe Prado**: the [God of War III Remastered PC port](https://github.com/FelipePrado0/gow3_pc)
  this project is copied from.
- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the original
  native port, and **Supermedo** (Mohammed Albarghouthi), its
  [Windows fork](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4**: the [shadPS4 fork](https://github.com/cuesta4/shadPS4/tree/eltutz) (branch
  `eltutz`) whose fixes the video core carries.
- **shadPS4 team**: the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core.

Also used: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK,
[MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) and [Pillow](https://python-pillow.org).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.
