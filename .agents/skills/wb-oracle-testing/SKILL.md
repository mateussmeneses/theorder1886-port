---
name: wb-oracle-testing
description: Build and validate test oracles - byte, behavior and runtime comparisons against a trusted reference - and prove each oracle rejects planted failures before trusting its passes
---

# Oracle Testing

An oracle is any check that compares a result with a reference you trust. The model proposes; the oracle decides. Oracles are software and have bugs: an oracle with a blind spot does not fail one case, it quietly approves hundreds.

## Oracle types
| Type | Compares | Typical use |
|---|---|---|
| Byte | Compiled output against original machine code or files byte for byte | Matching decompilation, file-format writers, codecs |
| Behavior | Same inputs into reference and candidate, compare outputs | Rewrites, ports, clean-room rebuilds, refactors, API clones |
| Runtime | Build and use the whole program; scripted scenarios, screenshots, metrics | Games, UIs, integrations |
| Statistical | Distribution/metric within tolerance of a baseline | Backtests, ML outputs, performance |

## Procedure
1. **Pick the reference** - original binary, previous release, public specification, upstream test suite, or a recorded golden output. Record its exact version and hash.
2. **Define comparison semantics** - what must match exactly, what is normalized (timestamps, addresses, ordering) and the tolerance for numeric output. Normalization is where blind spots hide: if addresses are relocated before comparing, also compare the values stored at those addresses.
3. **Generate inputs** - spec cases, ordinary use, edge values (empty, maximum, unicode, negative, zero), malformed or corrupt input, and properties (round-trip, idempotence, monotonicity).
4. **Plant failures** - create at least three wrong variants: change one constant, swap one branch, shift one boundary by one. Run the oracle on each. Every variant must be rejected. If any passes, fix the oracle first and treat every earlier green result as suspect.
5. **Audit after an oracle fix** - re-run everything the weaker oracle accepted; search for the same defect class across the codebase (for example, every floating-point literal reference).
6. **Report** - match rate per input group, divergences as input / reference output / candidate output, and the planted-failure results. End machine-readable reports with one summary line (for example `RESULT wrong=0 crash=0`).

## Templates
`templates/planted-failures.md` - checklist and prompt for the three mutants.

## Deliverables
Oracle definition (reference, semantics, tolerances), input corpus, planted-failure evidence, and the match-rate report.
