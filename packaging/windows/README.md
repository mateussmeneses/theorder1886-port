# The Order: 1886 on Windows

The port builds and runs natively on 64-bit Windows 10 (1903+) / 11 with the same sources as
on Linux. Players: see `README-Windows.txt` (it ships in the package).

## Build

Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`), open the
**MSYS2 CLANG64** shell and install the toolchain and libraries (the counterpart of
`shell.nix`):

```bash
pacman -S --needed git patch \
  mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
  mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
  mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
  mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
```

magic_enum, miniz and xbyak (not packaged by MSYS2) are submodules under `gpu/third_party/`.
Then, in the repository:

```bash
git submodule update --init --recursive
bash build.sh                      # out/theorder-probe.exe, out/theorder-gpu-capabilities.exe
THEORDER_GAME_DIR=F:/path/to/CUSA00035 python run.py
python launcher/port_launcher_win.py
bash packaging/windows/package.sh  # dist/theorder1886-windows/ and .zip: The Order 1886.exe + DLLs
```

`package.sh` runs from MSYS2's own bash (Git Bash's runtime expands its `find` patterns) and needs
a python.org Python (`WINPYTHON=/c/path/to/python.exe` when it is not found).

`run.py` is the Windows `run.sh` (same variables; works from a plain Windows Python too).
The package freezes the launcher with PyInstaller (a private venv in `out/pyenv`) into
`The Order 1886.exe`, which also runs `run.py` (`--run`), the preparation scripts (`--script`)
and the game without the window (`--play`): players need no Python. The venv also receives
`iced-x86`, which `scripts/isa_rewrite.py` uses for CPUs without MOVBE, BMI1 or LZCNT.
`Play The Order 1886.exe` (`packaging/windows/play.c`) is a small native program that runs
`The Order 1886.exe --play` from its folder and waits for it, for players who want to start the
game directly with the saved settings.
`patches.py` finds the built-in patches' sites in the eboot.bin it is given; third-party patch
files apply only to the build they are written for (CUSA00035 01.02, by eboot.bin sha256).
From a source tree it adds `C:\msys64\clang64\bin` (or `$MSYS2_ROOT`) to `PATH` for the DLLs.

The launcher texts are written in English with the Russian beside them; the other languages
are in `launcher/port_lang.py` (one list per language in the order of `KEYS`; a missing or
empty text falls back to English). The icon (`launcher/theorder.ico`/`.png`, drawn by
`packaging/windows/make_theorder_icon.py`) is built into `The Order 1886.exe`, `theorder-probe.exe`
(the game window) and `theorder-gpu-capabilities.exe`.

## RecompLauncher

The Windows ZIP includes `recomplauncher.json` beside `The Order 1886.exe`. Select the
extracted PS4 game folder in RecompLauncher. The port checks the required files and title ID
without opening a window, then RecompLauncher records the folder in `settings.json` and
starts the existing direct-play mode. The game files stay in the selected folder.

Graphics controls edit the port's `theorder.ini`. RecompLauncher backs up the default portable
`user/savedata` folder. If you chose a custom save folder in the port, set the same folder as
the save location in RecompLauncher. Separate update folders still use the port's Update folder
setting. The setup check checks the folder structure and title ID. The tested game version is
CUSA00035 01.02.

## What is different from Linux

| Linux | Windows |
|---|---|
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base set with `arch_prctl` | GS is the TEB and cannot move. The loader rewrites the displacement to `gs:[0x1480 + 8*slot]` (the TEB's `TlsSlots[slot]`, same 9-byte instruction) and each guest thread keeps its TCB in that TLS slot. The slot is claimed first thing in `main` (Vulkan drivers take many). |
| One memfd for direct + flexible memory, `mmap(MAP_FIXED)` views | A pagefile-backed section; the PS4 range 64 GiB–1 TiB is reserved as one placeholder and views replace placeholders (`VirtualAlloc2`/`MapViewOfFile3`, as in shadPS4). Partial unmaps split views and restore page protections (GPU tracking). |
| `-no-pie`, heap below 1 TiB | `--disable-high-entropy-va`: heaps and thread stacks stay low and out of the PS4 range. |
| SIGSEGV handler, `sigsetjmp` recovery | Vectored exception handler; `port_setjmp`/`port_longjmp` (`src/compat_win.c`) restore all Win64 callee-saved registers without SEH unwinding (guest frames have no unwind data; clang's `__builtin_longjmp` restores a wrong frame pointer on Win64). |
| Guest thread stacks from `runtime_low_map` | Windows allocates them, fully committed (`pthread_attr_setstacksize`): guest code has no stack probes and would skip the guard page. The main thread stack is 16 MiB committed. |
| glibc `nanosleep`, monotonic condition variables | High-resolution waitable timers (`compat_sleep_ns`; winpthreads' sleep rounds to 15.6 ms); semaphore deadlines use `CLOCK_REALTIME` (winpthreads rejects monotonic condition variables). |
| `libtheordergpu.so` | `libtheordergpu.a`, linked into `theorder-probe.exe` by CMake (`gpu/CMakeLists.txt`), with a manifest for UTF-8 paths and long paths. |
| `execlp bash run.sh` (in-game restart) | `run.py` puts its command line in `THEORDER_RESTART_COMMAND`; the new launch waits for the old process (`--after PID`). The launcher keeps the game in a job object. |
| Mod overlay with symlinks | Symlinks when allowed (Developer Mode), else junctions for folders and hard links for files. |

Diagnostics: `THEORDER_FRAME_DUMP_TRIGGER=<file>` writes the guest display buffer and the presented
image of the next frame to `THEORDER_DUMP_DIR` (raw 32-bit pixels), which also works for menus and
movies; `tools/batch_runs.py` runs timed A/B batches; `tools/fault_stacks.py` symbolizes faults.

Not ported: userfaultfd tracking (`THEORDER_UFFD`), MangoHud, PGO (GCC flags) and the `tests/`
programs that use Linux-only APIs.
