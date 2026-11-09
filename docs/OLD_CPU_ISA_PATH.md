# Intel 2nd/3rd generation CPUs (#65), 2026-11-09

#65: the game does not start on Intel 2nd or 3rd generation CPUs (Sandy Bridge, Ivy Bridge). The
reporter gave no log. This host (i7-14650HX) has every instruction set the PS4 code uses, so the
old-CPU path was exercised by forcing the ISA rewrite (`THEORDER_ISA_REWRITE`), which rewrites
the same sites a CPU without those sets needs rewritten.

## What an older CPU lacks

| Set | Jaguar (PS4) | Sandy Bridge (2nd) | Ivy Bridge (3rd) | Sites in CUSA00035 01.02 eboot |
|---|---|---|---|---|
| SSE4.2, POPCNT, AVX | yes | yes | yes | (required, checked at start) |
| MOVBE | yes | no | no | 123 |
| BMI1 (incl. TZCNT) | yes | no | no | 1869 |
| LZCNT | yes | no | no | 89 |
| F16C | yes | **no** | yes | 5 (`vcvtph2ps xmmN, xmmN`) |

The port's own executable was scanned (`llvm-objdump`): no AVX2, FMA, BMI2, F16C, MOVBE or LZCNT;
only `tzcnt` (194, compiler output for `__builtin_ctz`), which runs as `bsf` on CPUs without BMI1
and gives the same result for the nonzero inputs it is emitted for.

## Finding: Sandy Bridge stopped at start

`scripts/isa_rewrite.py` marked every F16C site unsupported, and the probe stops when a needed
site is unsupported. With `THEORDER_ISA_REWRITE=1` (every set, the Sandy Bridge case) all three
runs ended at start:

```
ISA: 2086 sites (MOVBE 123, BMI1 1869, LZCNT 89, F16C 5); 76 relocated, 10 int3, 5 unsupported
STOP: this CPU lacks  and 5 sites use encodings the rewrite cannot emulate
```

So the port could not run on Sandy Bridge at all. Ivy Bridge has F16C and was not affected by
this; its path (MOVBE, BMI1, LZCNT rewritten) had not been run in-game before either.

## Change

- `vcvtph2ps xmm, xmm` sites become int3 sites. The Windows exception handler converts the four
  halves of the source XMM register on the trapped CONTEXT and zeroes bits 255:128 of the
  destination in the AVX extended state, as the VEX.128 instruction does
  (`isa_rewrite_trap_context`). Memory forms, ymm destinations and `vcvtps2ph` stay unsupported
  (none occur in this eboot). Linux keeps them unsupported (no XMM path in its handler).
- `isa_half_to_float` matches the host's F16C for all 65536 inputs (`isa-emulate-test`); the
  int3 path is tested end to end on real code (`isa-stub-test`: values and the zeroed upper
  half). Mutations (NaN not quieted, upper half not zeroed) fail those tests.
- `THEORDER_ISA_REWRITE` accepts a list of sets (`MOVBE+BMI1+LZCNT` is Ivy Bridge), and a forced
  rewrite without a usable table now stops instead of running unrewritten.

## In-game result

Prologue route, 125 s, warm cache, conditions interleaved, 3 runs each, same build
(`out/c2/manifest-c2b.json`; per run):

| Path | Complete | FPS | P95 ms | P99 ms | Worst ms | Stutters/min |
|---|---|---|---|---|---|---|
| native | 3/3 | 58.6 / 58.6 / 58.3 | 17.5-17.6 | 18.1-18.2 | 714 / 654 / 641 | 22.0 / 23.0 / 26.9 |
| Ivy Bridge (`MOVBE+BMI1+LZCNT`, 2081 sites) | 3/3 | 58.1 / 58.3 / 57.9 | 17.5-17.6 | 31.1-31.5 | 847 / 611 / 662 | 45.4 / 40.9 / 50.9 |
| Sandy Bridge (`1`, all 2086 sites) | 3/3 | 57.0 / 57.9 / 57.2 | 17.6-17.7 | 31.3-31.4 | 1499 / 799 / 1089 | 48.3 / 45.4 / 45.9 |

- Both older-CPU paths now reach and play the prologue; before this change the Sandy Bridge path
  stopped at start in 3/3 runs.
- The rewrite costs about one missed vblank in a hundred frames on this CPU (P99 18 → 31 ms,
  stutters per minute doubled): the stubs call the emulator for every executed site, and BMI1 has
  1869 sites. On a 2011-2012 CPU the cost will be higher. Whether the five F16C sites executed on
  this route is not recorded (the hit report is printed only on a fault).
- Next step if old CPUs matter: rank sites by hits (`isa_rewrite_report`) and emit inline
  sequences for the hottest kinds (e.g. `tzcnt` → `bsf` with a zero check) instead of calling
  the emulator.

## Not verified

No run on a real Sandy or Ivy Bridge CPU. A forced rewrite on a newer CPU executes the same
patched code, but not the older CPU's timing, cache sizes or driver; the reporter's confirmation
is still needed.
