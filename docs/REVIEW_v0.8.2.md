# Change review before v0.8.2 (`windows-v0.8.1..HEAD`)

Method: two-axis review of `C:\projetos\skills\skills\engineering\code-review` in two
independent sub-agents: Standards (AGENTS.md, CONTRIBUTING.md, `C:\projetos\ponytail\AGENTS.md`,
smell baseline; emitted JIT code checked by reading the Xbyak emission) and Spec
(`docs/plans/2026-11-11-reference-ports-review.md`, the phase 7 backlog, commit messages, the
TOport sources the ports came from at `C:\tb\toport`). Uncommitted WIP in `src/probe.c`,
`src/runtime_file.c`, `src/runtime_kernel.c` (subtitle trace, localization stub fallback) is
not part of this release and was stashed while packaging.

## Runtime-affecting changes and verdict

| Change | Runs in every session? | Evidence | Verdict |
|---|---|---|---|
| Missing language package -> PT-PT/English (`run.py`) | Portuguese on CUSA00035/00076/00070 only | 5 tests (fail with the check forced true); CUSA00035 PT reaches menu and prologue (2 runs) | ship |
| Damaged cache meta skipped (`serdes.h`, `LoadShaderMeta`) | the reader on every cache load | serdes-damaged test (every truncated prefix, 2^40 count; fails on the old abort); prologue runs unchanged | ship |
| SRT walker TryEmit rollback + null descriptor slot | every new shader translation | try-emit test (fails without `setSize`); 59/59 suite; prologue: 0 descriptor fallbacks among 2816 | ship |
| RAM guard thread | yes (Windows) | boot/prologue runs log `Memory: system RAM ... free`; stop path not triggered here | ship |
| `THEORDER_PENDING_DRAWS=skip`, `THEORDER_DLSS_PRESET` | only when set | build | ship (opt-in) |

## Findings and resolution

Standards:
- No bug in the JIT rollback (no labels are bound by `PushPtr`/`ComputeOffset`; the slot write
  follows the rollback) or the serdes bound (`offset <= size` holds because every advance is
  checked). Fragility noted in `try_emit.h`: emitters must not bind labels. Fixed.
- No test for `TryEmit`, `SrtFeedsDescriptor`, `THEORDER_PENDING_DRAWS`: `try-emit` test added.
  `SrtFeedsDescriptor` needs an IR graph; covered only by in-game translation runs. **Open (P3).**
- Smells: duplicated loop shape in `ZeroFillSubtree`, unused parameter, `printf` beside the log
  backend: fixed. Bare language ids in `run.py`, `thread_local` shader hash: kept (judgement).

Spec:
- **P1, fixed: the language fallback failed closed.** IDs measured on CUSA00035 would have
  switched a working Portuguese install on another edition or layout to English. Now checked
  only on CUSA00035/00076/00070 and only when the measured folder exists.
- #113 stutter: only an opt-in change ships; the extra per-session pipeline combinations are
  open and stated as such in the notes. #102: the saved driver cache stays empty with pipeline
  binaries on; stated, not changed.
- SRT port drops TOport's DMA data branch: equivalent here (with DMA on, flat offset 0 already
  reads guest memory through `read_const_dynamic`).
- Unconditional SRT counter output: made opt-in. RAM log line every 30 s kept (as TOport).
- Plan/backlog stale wording and a wrong commit id: fixed.
- `DeserializeError` derives from `std::runtime_error`, so `catch (const std::exception&)` holds.

## Gates

`build.sh` OK; `tools/run_tests.sh` (CLANG64): 59 native tests + Python suite pass. Packaged
build (`dist/theorder1886-windows.zip`, built with the WIP stashed, sha256 e5ace555...4381) run
through `The Order 1886.exe --run` on CUSA00035 with Portuguese selected: fallback logged, menu
and prologue reached (up to 650 draws/frame), RAM guard log present, no fault.

No P0 open. Decision: release v0.8.2 with the items above stated in the notes.
