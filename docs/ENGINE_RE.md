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
| `-startlevel`, `-startcheckpoint checkpoint_name` | load a level and spawn at a checkpoint | `wch_020` + `wch_020_checkpoint_025`: still the title screen (wrong level name, see "Level start") |
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

## Level start (-startlevel)

T-020, 2026-11-12. Build: CUSA00035 01.02,
`out/eboot.elf` and `out/eboot_ghidra.exe` (sha256 `0f42dce6…3146f`). Addresses are eboot
addresses; **file offset = address + 0x4000** for both load segments (program headers: text at
file 0x4000 -> 0, data at file 0x15c0000 -> 0x15bc000). Verified with `llvm-objdump -d` of
`out/eboot.elf`, REA/Ghidra pseudocode of `out/eboot_ghidra.exe` (address + 0x100000000), and the
`R_X86_64_RELATIVE` entries of the dynlib data segment (file 0x166f940..) for vtable slots.

**Conclusion: the retail eboot never reads its argv.** `sub_1fd010` (called by `main`) builds
the option table from the fixed string `"-frontend -syncinterval 2"` (0x1482726, `leaq` at
0x1fd12e, parsed by `sub_201d10` at 0x1fd13c) and hands that table to the game object
(0x1fd16c -> 0x44c6c0 -> base constructor 0xa88990, stored at `game+0x40`). The argv string
`main` builds (`" %s "` per entry, 0x298210..0x29832c) and stores at 0x165ae00 (0x298339) has
no reader: Ghidra and objdump show that store as the only reference. So every option the port
passes is ignored: `-startlevel`, and also `-cores`, `-use4K`, `-syncinterval`, `-language`,
`-usedlc`, `-disablecloth`, which explains the "same as before" results in the Command line
table. `-frontend` is always set, so 0x447880 always picks the front end.

The patch **"Command Line - Game Arguments"** (`patches/TheOrder1886-Orbis.xml`, disabled by
default; `run.py --level` adds it) changes two instructions; every byte was checked against
`llvm-objdump` of `out/eboot.elf` and decoded with `llvm-mc`:

| Site (file offset) | Original | Patched |
|---|---|---|
| 0x1fd12e (0x20112e) | `48 8d 35 f1 55 28 01` lea rsi,[0x1482726] | `48 8b 35 cb dc 45 01` mov rsi,[0x165ae00] |
| 0x298203 (0x29c203) | `85 db 0f 8e 27 01 00 00 45 31 ff 66 90` test ebx,ebx; jle 0x298332; xor r15d,r15d; nop | `83 fb 01 0f 8e 26 01 00 00 6a 01 41 5f` cmp ebx,1; jle 0x298332; push 1; pop r15 |

The second change is needed because the option checker `sub_7f4930` (called from 0xa81b30 for
`-help`) stops at the first token that does not start with '-' and reports
`'%s' is not a valid command line argument.` through `sub_203650`, which raises a debug
exception (run with only the first change: `STOP: guest raised debug exception
code=0xa0020001` after "Encountered an error:"). argv[0] is `/app0/eboot.bin`, so `main`'s loop
now starts at argv[1]; with argc <= 1 it skips the loop as before and the string is empty, and
with no `-frontend`/`-startlevel` 0x447880 still goes to the front end. The same checker rejects
options it does not know and wrong parameter counts, so with this patch only registered
options may be passed. `run.py` adds `-syncinterval 2` to keep the replaced string's setting.

**Refuted earlier claim:** the first version of this section (same day, static only) said the
tried names were simply wrong and that no patch was needed. A run with the full name
(`--level wch_020_slums --checkpoint wch_020_checkpoint_025`) still showed the title screen
(log `out/batch/base-01-20261112T160456050966Z.log`), because the option never reached the
game. The full level name *is* required, as below; it is not enough.

Call order (game object, vtable 0x161ae10, stored at 0x44c6d3 by its constructor 0x44c6c0; engine
base vtable 0x164a5d0, base constructor 0xa88990 sets `+0x30` and `+0x38` to -1):
- `sub_44cce0` (game vtable slot +0x300, the game's run function) calls slot +0x10 (0x446080,
  game options), the engine init 0xa82860 and a long list of init slots, then slot +0x18 =
  **0x447880 once** (call `*0x18(%rax)`), then the frame loop, which calls `sub_44a600` every
  iteration (0x44e7d3).
- 0x447880 first calls 0xa81b30 (engine-base slot +0x18, call at 0x4478b0): it handles `-help`,
  `-nframes`, `-nframespause` and **`-level <name>`**, which loads a level directly with
  `sub_2b23e0(*0x16b31d8, {hash(name), type 0xb, 30000})` (call at 0xa82200). Without `-level`
  nothing is loaded there.
- The gate at 0x4480c0 is `cmpq $0, 0x208(*0x16b31d8)`. `*0x16b31d8` is the level manager
  (symbol `CGameLevelMgr` in the table; created at 0x442ec8 / 0xa83dfa, base constructor
  0x2b1580). `+0x1e0` is its array of loaded levels and **`+0x208` its count**: 0x2b1580 zeroes
  it at 0x2b1954, the only increment is the array push in `sub_2b23e0` (LoadLevel) at 0x2b2891.
  A scan of every instruction that loads 0x16b31d8 found only reads of `+0x208`. In a normal boot the
  count is 0 here, because the front end itself is started from inside the same gate. So the
  gate is true for `-startlevel` too, unless `-level` is also passed.
- Inside the gate: `-frontend` gives the front end; neither `-startlevel` nor `-frontend` also
  gives the front end: level = `*(*(game+0x7a8)+0x198)`, checkpoint = `**(*(game+0x7a8)+0x160)`
  (0x4483b7..0x4483d3). With `-startlevel` and without `-frontend`, `sceSystemServiceHideSplashScreen`
  is called, the level string is copied into a 256-byte buffer, every `.radlvl` (string at
  0x1489f22, used at 0x4497a3) is removed, the name is hashed and stored at `game+0x30`, and the
  hash of the `-startcheckpoint` value, or the default checkpoint value (global 0x16da298), at
  `game+0x38` (0x4499ad, 0x4499b1). Both paths end in the virtual call `+0x330` at 0x448423.
- Slot +0x330 of the game is 0x450710: it clears bit 0x20000 of `+0x7dc`, sets the transition
  state `game+0xc78 = 0`, saves a timestamp in `+0xc58` and sets global 0x15c0d54 = 2. With
  argument 0, as here, it does not run the state machine itself.
- `sub_44a600` is the level-transition state machine (state at `game+0xc78`, 0..0xf; 0xf is
  done). State 4 compares `game+0x30` against a table built at run time at 0x16da670: the hashes
  of `mnu_010_begin` (the front end), `pro_010_catacombs`, `pro_020_westminster`,
  `pro_210_flashbacks`, `emb_020_tunnels`; other levels first go through a step that compares
  global 0x16da298 with the hash of `plr_prisoner`. **State 7 loads the level**:
  `sub_2b37b0(levelMgr, {game+0x30, game+0x38, 30000, type 0x19})` unless a level with that hash
  (`level+0x260`) is already loaded. State 9 spawns: if `game+0x38` is the default value, the
  level's own default checkpoint (level vtable +0x200), else level vtable +0x208.

Hash (options, levels, checkpoints): `h = ~0; for each byte c (A-Z lowered, sign-extended):
h = (h << 8) ^ T[h >> 56] ^ c`; leading '-' of option names is skipped. `T` is all zero in the
file except `T[0]` and is generated in `main` (0x2983d7..0x29861d): `t = i&0x80 ? 2K : 0; for
bit in 0x40..0x01: if (i & bit) t ^= K; t <<= 1` with `K = 0x15ac9329ac4bc9b5` (constants at
0xf11d60..0xf11e80, file 0xf15d60..). Checked: this reproduces all 4129 8-byte hashes stored
beside the `RaDsSyMbOlPrEfIx<name>` records between 0x1489000 and 0x14c0000 (dashes kept for
record names; only the option lookup skips them), including all 244 level and checkpoint names.

The record layout: 16 bytes of the `RaDsSyMbOlPrEfIx` prefix, with the name's hash overwriting
the 8-aligned qword inside it, then the NUL-terminated name. The code reads a hash with
`leaq <record>(%rip); andq $-8; movq (%rax)` (for example 0x442e19). The per-level loadout list
(`R7LevelLoadout`, `CR7LevelLoadoutList`) is just before the level names, so a level started
this way probably gets its own starting inventory (not verified). `-startinventory` (read
right after the gate) adds weapons by hash.

Levels (record address; the name string is 16 bytes further on) and the checkpoints whose name
starts with the same `<area>_<nnn>`:

| Level (`-startlevel`) | Record at | Checkpoints `<area>_<nnn>_checkpoint_NNN` |
|---|---|---|
| `pro_020_westminster` | 0x148b3e9 | 005 010 030 |
| `may_010_intro` | 0x148b40d | 010 020 |
| `may_020_plaza` | 0x148b42b | 010 020 030 040 |
| `may_035_street_end` | 0x148b449 | 010 020 |
| `may_040_atrium` | 0x148b46c | 010 030 060 |
| `may_050_courtyard` | 0x148b48b | 010 |
| `may_060_underground` | 0x148b4ad | 010 020 040 |
| `may_070_chase` | 0x148b4d1 | 010 |
| `may_100_mopup` | 0x148b4ef | 010 |
| `wmp_010_roundtable` | 0x148b50d | 010 015 |
| `wmp_020_lobby` | 0x148b530 | 010 |
| `tsl_020_teslaslab` | 0x148b54e | 010 020 030 035 |
| `wch_010_arrival` | 0x148b570 | 005 010 |
| `wch_020_slums` | 0x148b590 | 005 015 025 |
| `wch_030_brothel` | 0x148b5ae | 010 015 |
| `wch_070_rooftops` | 0x148b5ce | 010 015 |
| `wch_080_ambush` | 0x148b5ef | 005 010 013 |
| `wch_090_escape` | 0x148b60e | 005 |
| `wch_100_clotheslines` | 0x148b62d | 010 015 017 |
| `wch_105_cellar` | 0x148b652 | 005 |
| `wch_110_square` | 0x14afc9d | 010 030 040 |
| `wch_115_tracks` | 0x14afcbc | 020 030 040 |
| `wch_120_mainstreet` | 0x148b671 | 005 010 020 |
| `wch_125_shortcut` | 0x148b694 | 020 040 |
| `wch_130_hospitalupper` | 0x148b6b5 | 010 020 |
| `wch_140_hospitallower` | 0x148b6db | 010 |
| `wch_150_bossfight` | 0x148b701 | 005 010 070 |
| `wch_160_psycward` | 0x148b723 | 010 020 |
| `wch_170_exit` | 0x148b744 | - |
| `air_010_rappel` | 0x14afcdb | 005 010 |
| `air_020_engineering` | 0x14afcfa | 005 007 010 |
| `air_030_cockpit` | 0x14afd1e | 005 015 |
| `air_040_crew` | 0x14afd3e | 010 |
| `air_045_catwalks` | 0x14afd5b | - |
| `air_050_ballroom` | 0x14afd7c | - |
| `air_055_foyer` | 0x14afd9d | 001 007 010 |
| `air_060_propeller` | 0x14afdbb | - |
| `air_065_exterior` | 0x14afddd | - |
| `air_070_lifeboats` | 0x14afdfe | 010 017 |
| `air_075_kitchen` | 0x14afe20 | 010 017 |
| `air_080_lounge` | 0x148b761 | 010 |
| `air_090_bar` | 0x148b780 | 025 |
| `air_110_death` | 0x148b79c | 010 015 |
| `tsl_220_teslaslab` | 0x148b7ba | 010 |
| `wmp_210_roundtable` | 0x148b7dc | 010 015 |
| `frl_010_bridgewest` | 0x148b7ff | 010 020 030 033 |
| `frl_020_bridgeeast` | 0x148b822 | 005 013 |
| `wch_210_brothelreturn` | 0x148b845 | 010 015 |
| `wch_220_brothelexit` | 0x148b86b | 010 |
| `bwy_010_tunnel` | 0x148b88f | 010 020 030 040 050 060 070 |
| `bwy_020_escort` | 0x148b8ae | 010 020 040 050 |
| `bwy_025_foundry` | 0x148b8cd | 010 020 |
| `bwy_030_follow` | 0x148b8ed | 010 020 |
| `bwy_040_drydock` | 0x148b90c | 010 030 040 070 |
| `bwy_045_lowerdecks` | 0x148b92c | 010 |
| `bwy_050_warehouse` | 0x148b94f | 010 020 030 040 |
| `bwy_055_annex` | 0x148b971 | 005 010 |
| `bwy_060_escape` | 0x148b98f | 010 |
| `wmp_320_lobby` | 0x148b9ae | 010 |
| `wmp_310_roundtable` | 0x148b9cc | 010 |
| `eih_010_streets` | 0x148b9ef | 010 013 |
| `eih_020_rooftops` | 0x148ba0f | 005 010 |
| `eih_030_back` | 0x148ba30 | 010 |
| `eih_035_house` | 0x148ba4d | 010 |
| `eih_040_side` | 0x148ba6b | 005 010 |
| `eih_050_front` | 0x148ba88 | 003 010 |
| `eih_060_entrance` | 0x148baa6 | 003 010 015 |
| `eih_070_terrace` | 0x148bac7 | 007 |
| `eih_080_archives` | 0x148bae7 | 001 005 |
| `eih_090_cellar` | 0x148bb08 | 010 |
| `eih_100_lobby` | 0x148bb27 | 015 |
| `wmp_410_roundtable` | 0x148bb45 | 010 |
| `pro_010_catacombs` | 0x149d1df | 005 010 030 040 |
| `pro_210_flashbacks` | 0x148bb68 | 010 020 |
| `emb_020_tunnels` | 0x148bb8b | 005 010 |
| `wch_300_brothelagain` | 0x148bbab | 010 |
| `cat_010_streets` | 0x148bbd0 | 010 |
| `cat_020_underground` | 0x148bbf0 | 010 |
| `cat_030_crypts` | 0x148bc14 | 010 |
| `cat_040_armory` | 0x148bc33 | 010 030 040 |
| `tsl_520_teslaslab` | 0x148bc52 | 010 050 |
| `wmp_530_fin` | 0x148bc74 | 010 |

The list from 0x148b3e9 to 0x148bc74 looks like campaign order; it is not proven to be one, and
chapter numbers are not in the data read here. A checkpoint is tied to a level only by its
name prefix.

How to start a level: `python run.py --level wch_080_ambush --checkpoint wch_080_checkpoint_005`.
run.py turns these into `-startlevel`/`-startcheckpoint` and, only when `-startlevel` is present,
adds the "Command Line - Game Arguments" patch and `-syncinterval 2` (`run.level_start`,
`tests/test_run_level.py`). `--checkpoint` is optional (the level's default checkpoint is used).
Do not combine with `--frontend` (front end wins), with the engine's `-level` (the level loaded
there makes the gate false), or with options the game does not register (the option checker
stops the game; the `--perf` options `-nocamsprings`/`-nocamadditive` were never checked).
`run.level_start` therefore leaves out `-disablecloth`, `-nocloth`, `-nocamsprings`,
`-nocamadditive` and `-msaa` when a level is given, with a note on stderr, until each has been
run with the patch. `run.py` always passes `-language N -enablesubtitles -usedlc`, so the string
is never empty in practice: when the patch is chosen by name (`THEORDER_PATCHES`, the launcher)
without a level, `level_start` adds `-frontend -syncinterval 2`, the fixed string's content.
That path was not run; the claim that an empty string still reaches the front end is static only.

Validated 2026-11-12 (`out/theorder-probe.exe` of 02:35, profile from `user/` with savedata,
`THEORDER_PATCHES='Command Line - Game Arguments'`, `--guest-arg -syncinterval --guest-arg 2`,
pad `62:ly=0:5`, frame dumps at 60 s and 75 s), no title screen and no menu presses:

| Level / checkpoint | Runs ok | Scene at 60-75 s | Manifest (`out/batch/manifests/`) |
|---|---|---|---|
| `wch_080_ambush` / `wch_080_checkpoint_005` | 3/3 | Whitechapel street, Grayson with a rifle; the view moves with the stick | `regression-20261112T162548Z.json` |
| `may_040_atrium` / `may_040_checkpoint_010` | 3/3 | the Atrium, in cover, HUD prompt "EQUIP WEAPON" | `regression-20261112T163104Z.json` |
| `bwy_010_tunnel` / `bwy_010_checkpoint_010` | 3/3 | the Blackwall tunnel, a companion on the tracks ahead | `regression-20261112T163621Z.json` |
| `wch_020_slums` / `wch_020_checkpoint_025` | 2/2 | the slums, Grayson walks to a rebel ("Please Shamus!") | `regression-20261112T162100Z.json`, `…162347Z.json` |

Through run.py alone (no `THEORDER_PATCHES`, no extra argument, no checkpoint):
`--level may_040_atrium` added the patch and `-syncinterval 2` itself and reached the same
Atrium scene (`regression-20261112T204210Z.json`, Grayson's face rendered wrong in that dump; a
renderer matter, not this patch). Frame dumps: `D:\theorder-bench\cap-t020-{wch080,atrium,bwy010,p3,runpy}`. The run with only the
first patch line is `regression-20261112T161623Z.json` (debug exception). Still unknown: what an
unknown hash does in `sub_2b37b0`, and how far the starting inventory and story flags differ
from a player's (the per-level loadout list suggests the level sets its own loadout).

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

