# Change review before v0.8.3 (`windows-v0.8.2..HEAD`)

Method: two-axis review of `C:\projetos\skills\skills\engineering\code-review` in two
independent sub-agents: Standards (AGENTS.md, CONTRIBUTING.md, `C:\projetos\ponytail\AGENTS.md`,
smell baseline) and Spec (`docs/plans/2026-11-11-reference-ports-review.md`, commit messages,
#113, TOport's `bb_particle_srt.h` and call sites at `C:\tb\toport`). Uncommitted WIP in
`src/probe.c`, `src/runtime_file.c`, `src/runtime_kernel.c` is not part of this release and was
stashed while packaging.

## Runtime-affecting change and verdict

| Change | Runs in every session? | Evidence | Verdict |
|---|---|---|---|
| `BbParticleSrt::Normalize` accepts this port's buffer count (7/9) | yes, for 4 billboard VS programs | particle-srt test (fails with the old exact count); prologue, cold + 2 warm sessions: >100 ms windows 15/7/7 -> 3/1/1, driver builds after session 1 38-39 -> 0, permutations 821 -> 472-477 (n=1 per build) | ship |
| Normalize in the prepared-draw fast path | yes | as above, repeated after the change (3/1, 0 driver builds, 477) | ship |

## Findings and resolution

Standards:
- No real bug; the zeroed words are exactly the verified image origins.
- No regression test: added `tests/test_particle_srt.cpp` (this port's and TOport's counts apply;
  two fewer buffers, a moved descriptor, too many probes, another program, or a buffer reading
  a zeroed word are rejected). Fixed.
- Gap: buffer descriptors were not checked against the zeroed range. Now rejected if any buffer
  fetch offset falls in either table. Fixed (verified still applying in game).
- Smells kept (judgement): positional schema tuples, anonymous `+1`.

Spec:
- **Missing call site:** the prepared-draw fast path compared a raw flat buffer against the
  normalized stored one, so it always fell back to the slow path for these programs. Fixed.
- TOport's `reader_revision` guard is not ported (our walker has no such field); the exact word
  count, image count and the 32 checked image origins pin the layout. Stated in the code comment.
- Evidence wording: n=1 per build and "not compared pixel for pixel" now in the notes.
- TOport ships this fix opt-in (`BB_PARTICLE_SRT_FIX=1`); ours is on by default. Notes say
  "adapted from TOport".
- #113 retest requested in the issue after publishing.

## Gates

`build.sh` OK; `tools/run_tests.sh` (CLANG64): 60 native tests + Python suite pass. Packaged build
smoke: below.

No P0 open. Decision: release v0.8.3.
