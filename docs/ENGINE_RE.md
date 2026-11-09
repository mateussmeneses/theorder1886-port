# The Order: 1886 engine notes (eboot 01.02)

Facts found in the executable (`out/eboot.elf`, image base 0: addresses are eboot offsets) while
porting. Tools: `llvm-objdump -d` (MSYS2 clang64), Rizin 0.9.1 (`rz-bin -zz` for strings; its
string addresses are file offsets, 0x4000 above the virtual addresses past 0x1400000), and short
Python scripts that resolve rip-relative constants from the ELF segments.

## Decompiling with REA (Ghidra)

REA admits only PE images to Ghidra on Windows, so `python scripts/eboot_pe.py <game folder>`
wraps the eboot in one, `out/eboot_ghidra.exe`:
- eboot address A is at **0x100000000 + A** (the PE headers need room below eboot address 0);
- every relocation is applied; the 679 import slots are IAT entries named from
  `src/import_names.inc` (26 of the 355 imports keep their NID);
- the 27958 `.eh_frame_hdr` function starts are exported as `sub_<A>`, A in eboot coordinates,
  plus `_init`, `_start` and `main`.

The PE loader would pick the Windows x64 calling convention; the eboot uses System V, so the REA
server needs `REA_GHIDRA_LANGUAGE_ID=x86:LE:64:default` and `REA_GHIDRA_COMPILER_SPEC_ID=gcc`.
The first query of a session imports and analyzes the whole image (12 min on this PC, 654 s of
them auto-analysis; nothing is kept between sessions), past REA's 330 s default:
`REA_GHIDRA_STARTUP_TIMEOUT_MS=3600000`, `GHIDRA_HEADLESS_MAXMEM=8G`. REA must
run from the published npm package (`C:\Users\Mateus\tools\rea`): the Windows Ghidra boundary
needs its bundled native addon, which a source build lacks (`npx rea-agents@6.5.0` resolves to
the global `npm link` of one).

## Start-up

- `_start` (0xa0) calls `_init_env` and `atexit`, then the initializer at 0x20 (`.init_array` at
  0x166b928.., then the `.ctors` list from 0x166ac90 downwards), then `main` (0x2981b0). The
  loader must not call 0x20 itself: every global would be constructed twice (it did until
  8c2de5f, mapping the engine's 1.6 GiB of memory pools twice).
- The engine's pools at start: 8 + 24 + 810 + 654 + 52 MiB of direct memory. Loading the first
  gameplay save adds ~0.8 GiB of 2 MiB streaming chunks; 4.2 of 5.3 GB are in use there.

## Video setup (0x44ce86, called by the game-options function at 0x446080)

- The picture's aspect ratio is the float stored at 0x44ce8c (2.4); the engine centers a picture
  of that shape in its display buffer and sizes the scene targets to it (1920x800 at 2.4).
- Display mode: the flags at `0x7dc(game)` select 720p (bit 7), 1080p (bit 8) or 4K (bit 9), in
  that order, with 0x500/0x2d0, 0x780/0x438 and 0xf00/0x870 immediates (0x44cef7.., 0x44cf3d..,
  0x44cf80..). The 1080p bit is set on this setup, so `-use4K` alone changes nothing.
- The configuration is passed to 0xa82860; 0x8a10a0 copies it (from offset 0x48, the aspect) to
  the display device at 0xe0 (global 0x16b1df0).

## Field of view

Cameras keep a horizontal FOV defined for 16:9 and turn it into a vertical one with
`2*atan(tan(h/2) * 0.5625)`; the 0.5625 is a double in each function's constant pool: 0x147f2b0,
0x147f2d0, 0x147f3c0 / 0x147f3d0 (projection builders around 0x8c6a44 / 0x8c7a42), 0x147f5d0
(converted back with 2.4 at 0x147f5d8), 0x147f5e8, 0x147f610, 0x147f670. The aspect ratio
patches scale all of them but 0x147f5d0 by 2.4/aspect (Vert+).

## Command line (cr7game.cpp)

The retail executable still parses developer options: names are hashed (lower case, '-'
skipped, table at 0x15c04a0) and looked up by binary search in the parsed argument list; the
game-options function (0x446080) reads them. The port passes them with `THEORDER_ARGS` (split on
spaces). Help texts at 0x148a000.. (file offset 0x148e000..):

| Option | Help text | Tried |
|---|---|---|
| `-cores numcores` | Set number of cores to use (default 6, stored at 0x7c8, passed to the renderer config) | 8 and 10: same threads, same FPS |
| `-use720p`, `-use1080p`, `-use4K` | resolution in fullscreen mode (flag bits 7-9) | `-use4K`: 1080p bit wins |
| `-fixedfps` | run at a fixed framerate | |
| `-syncinterval sync_interval_num` | the VSYNC interval to use | 1: still 30 FPS (the port paces presentation) |
| `-noadaptivesync` | disable adaptive VSYNC | |
| `-msaa msaa_mode` | the MSAA mode to use | |
| `-disablecloth`, `-nocloth` | disable cloth on game characters | |
| `-startlevel`, `-startcheckpoint checkpoint_name` | load a level and spawn at a checkpoint | `wch_020` + `wch_020_checkpoint_025`: still the title screen |
| `-printfps` | print the frame time in the TTY when over budget | |
| `-photomode`, `-nogameui`, `-disableuiindicators`, `-gamechaptersonly`, `-usedlc`, `-enablesubtitles`, `-gamma` | | |

Checkpoint names follow `<level>_<nnn>_checkpoint_<nnn>`: `pro` (prologue), `wch`
(Whitechapel), `bwy`, `air`, `frl`, `tsl`, `eih`, `cat`, `may`, `wmp`, `emb`; the complete list
is a debug symbol table at 0x14a9b00.. (`RaDsSyMbOlPrEfIx<name>` strings beside 8-byte name
hashes).

How the options get in (decompiled with REA): `main` (0x2981b0) formats every argv entry with
`snprintf(" %s ")` (0x299a80) into one 2 KiB string and stores it at 0x165ae00; the engine then
builds the sorted table of hashed option names. `-startlevel`/`-startcheckpoint` are read by
0x447880, only while `*(*(0x16b31d8) + 0x208) == 0`: the level argument loses every `.radlvl`
(the level file extension), is hashed with the option-name hash and stored with the
checkpoint before the level starts (virtual call +0x330); without `-startlevel` and
`-frontend` the game goes to the front end. The port forwards THEORDER_ARGS as separate argv
entries (probe.c), which this concatenation handles.

## Rendering facts used by fixes

- Fullscreen clear: VS e5979e2b (x = s0 + {-1, 3, -1}, y = {1, 1, -3}, z = 1) with PS 9e98cf77
  (writes four constants). Its user data register must stay at the push-constant slot the
  pipeline gives it (91aa3ac).
- Final composite to the display buffer: VS b9c87fc1 (positions from the vertex index), PS
  a41511a2 (samples the picture at `fragcoord - (display - picture) / 2`, encodes sRGB).
- Tonemap and other fullscreen passes: VS 0ece32eb reads position and UV from a 60-byte vertex
  buffer (stride 20: (-1,1,1,0,0) (3,1,1,2,0) (-1,-3,1,0,2)) at 0x1059c18988 in every captured
  frame. `THEORDER_WATCH=1059c18988:60` showed it written once (zeros, then the triangle) and
  never again in 80 s: the guest data is static, so the one-frame black triangle comes from the
  port's side (buffer cache or a different V#), not from the engine rewriting it.

## Task system queue (startup fault at 0x20aa22)

`sub_20a950(scheduler)` runs one pending job (165 direct callers: every "help while waiting"
path). For each queue in `scheduler+0x168` (count at `+0x190`) it pops a Michael-Scott queue of
64-byte nodes addressed by index (`queue+0x148` node array; header with head `+0`, tail `+0x40`,
free list `+0x80`, count `+0xc0`; 64-bit words: index in the low 32 bits, tag above, `next ==
-1` is null) and pushes the old head node onto a Treiber free list. The fault is the read of
`node[head].next`: head already held an index outside the array, so the corruption happens
earlier (push side `sub_20b010` or node reuse). Node fields: `+8` job function, `+0x30` a
dependency counter pointer, `+0x38` a completion counter decremented after the job runs.
- Analytic in-scattering of light volumes (Sun et al.): PS 15c552f8, a2925aa8, 64b3f066, fd7df5bc
  into a 960x400 R16G16B16A16 buffer, F table in a 512x512 R32 texture.

## Job workers' idle loop (`sub_20ad20`, `sub_8a7380`)

The four "Task Kernel" workers run `sub_20ad20`: while the scheduler runs (`+0x18`) or jobs are
pending (`queue+0xc0`), `sub_20a950` pops one job; after each empty pop a stack counter goes up by
one (it is zeroed when a job ran) and the callback at `scheduler+0x1d0` gets its address. That
callback is `sub_8a7380`, the seventh argument the scheduler constructor `sub_20b860` gets from
`sub_8a10a0` (0x8a1c78): above 0x708 empty polls, unless the calling thread is the one at
`[0x1682de8]`, it calls `scePthreadYield` and zeroes the counter. There is no sleep, so idle
workers spin at 100% of a core each (97% of their samples in the prologue, of which 81.6% in
`sub_20a950`). `sub_20a950`'s `sceKernelUsleep(1000000)` calls only retry a failed re-push.
`THEORDER_TASK_IDLE` replaces the callback natively: docs/REA_NATIVE_ABSTRACTION.md.

## Per-thread TLS and Mersenne Twister (`sub_89fa60`, `sub_8a0330`, `0xc00ed7`)

- `sub_8a10a0` (`0x8a1863..0x8a1884`) installs `sub_89fa60` at `[0x170d438]` (thread-start hook,
  invoked by worker threads `sub_20a560` at `0x20a582` and the streaming thread `sub_802210` at
  `0x80224e`) and `sub_8a0330` at `[0x170d440]` (thread-exit hook), then calls `sub_89fa60(1)` on
  the main thread.
- `sub_89fa60` creates three singleton `scePthreadKeyCreate` keys stored at `[0x16a2660]`
  (caller `0x89fbed`/`0x89fc0e`), `[0x16b1b90]` (`0x89fdb3`/`0x89fdd4`) and `[0x16a2640]`
  (`0x89ff82`/`0x89ffa3`). In each block, `sub_89fa60` locks the singleton registry mutex
  (`[0x16abd28]`), calls `sub_208d60` (hash lookup), and unlocks the mutex *before* testing whether
  the entry was found and calling `scePthreadKeyCreate` + `sub_208e50` (insert). On fast host CPU
  scheduling, two threads entering `sub_89fa60` concurrently can both miss in `sub_208d60` and
  overwrite `[0x16a2640]` with a second key after the first thread already stored its TLS pointer.
- Key `[0x16a2640]` holds a `0x1384`-byte per-thread Mersenne Twister (MT19937) buffer allocated
  at `0x89ffc1` and seeded by `sub_814870(buf, seed)` (`624` `uint32_t` state words at `+0x000`,
  `624` tempered output words at `+0x9c0..+0x137c`, current index `uint32_t` at `+0x1380`
  initialized to `0x270` = `624` so the first draw calls `sub_814740` to twist and temper). All
  1,267 call sites in `eboot.elf` (including `sub_c00e70` at `0xc00ec9`, called by `sub_46f050`
  from `sub_445120`) dereference `[rax + 0x1380]` without a `NULL` check.

