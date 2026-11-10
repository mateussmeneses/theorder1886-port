# Change review before v0.8.1 (`4c9db6d..7bd018f`)

Method: `wb-change-review` (AI Workbench skill): real diff of the 34 code files plus callers;
classified by runtime impact; findings P0-P3 with the failing scenario. Then the two-axis review of
`C:\projetos\skills\skills\engineering\code-review` in two independent sub-agents: Standards
(AGENTS.md, CONTRIBUTING.md, `C:\projetos\ponytail\AGENTS.md`, smell baseline) and Spec (plan,
issues #65/#101/#113/#116, release notes).

## Runtime-affecting changes and verdict

| Change | Runs in every session? | Evidence | Verdict |
|---|---|---|---|
| WaitGraphics erase by key (`vk_pending_builds.h`) | yes | unit test + mutation; 0/50 vs 6/18 targeted failures at 4P | ship |
| ISA rewrite: F16C int3/XMM path, inline MOVBE loads and BEXTR | only on CPUs lacking MOVBE/BMI1/LZCNT/F16C, or `THEORDER_ISA_REWRITE` | oracle tests vs host instructions (65536 halves, 2352 BEXTR cases, MOVBE snippets), mutations; 3+3+3 in-game runs complete | ship; untested on real old CPUs |
| QTE patch (`jbe`→`jmp`, sub_b105b0) | with any frame rate patch | decompiled logic, unique signature in CUSA00035 01.02; loader skips non-unique or missing signatures | ship; not verified in the QTE |
| Bug report log/version | on export | 6 tests | ship |
| `THEORDER_ISA_REWRITE` list parsing; forced rewrite without a table stops | only when set | C and Python tests | ship |
| FSR 4 canaries, permutation/readback/pending-wait logs | dtor check always; logs opt-in | build + gate | ship |

## Findings

- **P1 — QTE fix unverified in game.** The scripted route does not reach the mash QTE; the
  change is reasoned from the decompiled update. Failure mode if wrong: presses count as at
  30 FPS while the frame rate patch is on (no crash). Needs a reporter or manual test at 60 FPS.
- **P2 — Other SKUs (CUSA00076/00785/00100) not scanned for the QTE signature.** `locate`
  requires a unique match; otherwise the patch is skipped with a log line, so the risk is that
  the fix is missing there, not a wrong write.
- **P2 — Old-CPU path not run on real Sandy/Ivy Bridge.** Forced rewrite on a newer CPU executes
  the same patched code but not the old CPU's driver or timing. Inline BEXTR frame-time gain
  unmeasured (noisy host on 2026-11-10).
- **P2 — New rare guest fault seen once** (`guest offset 0xc00ed7`, main thread, before the first
  frame, null + 0x1380) in 36 boot runs of the candidate build; no earlier log has it, cause not
  established, not tied to a change in this range.
- **P3 — Hit counters are non-atomic** (`inc`/`lea` in stubs from several guest threads): counts
  are diagnostics only and may undercount; same as the earlier stub code.
- **P3 — `THEORDER_ISA_REWRITE` values other than 0, 1 or a set list now stop at start** instead of
  being ignored (intended: a test mode should not run unrewritten).

No P0. Decision: the candidate can go to a manual test (`dist/theorder1886-windows.zip`); release
after that test, with the P1/P2 items stated in the release notes.

## Two-axis review (second pass)

- **Standards:** every emitted byte sequence (MOVBE, BEXTR, F16C path) decoded by hand: no bug.
  Judgement calls fixed in `53aa9d1`: `shortcut:` comments for the forms that still call the
  emulator and the Windows-only XMM path, one shared scratch-slot table, `pushes_scratch`
  instead of the order-dependent `low_stack`, removal condition for the FSR 4 canaries. Not
  changed: set names repeated in probe.c messages (P3). The Standards pass read the ISA, probe,
  cpu_check and FSR 4 files in full; the other files were covered by the first pass above.
- **Spec:** release-notes intro overstated the QTE and Sandy Bridge fixes (fixed); missing
  caveat that the QTE signature was checked on CUSA00035 only, while #101's reporter uses
  CUSA00785 (added); no confidence interval for the crash numbers (Wilson added); the FSR 4
  check is always on, not opt-in (stated); F16C helps 2nd gen only, 3rd gen uses the
  MOVBE/BMI1/LZCNT path (stated). Open, not fixed: G3/#101 not reproduced in game; #113
  precompilation step not implemented; S4/C1 not run.

Gates after the fixes: `isa-stub-test` ok, `bash tools/run_tests.sh` PASS (57 native + Python).
