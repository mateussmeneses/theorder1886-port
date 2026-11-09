---
name: wb-debug-rigorous
description: Debug failures with reproduction, falsifiable hypotheses and recorded evidence
version: 1.1.0
---
# wb-debug-rigorous

**Suggested role:** `root-cause-debugger`. This file is portable and does not depend on that agent existing.

## When to use
Bugs, intermittent failures, crashes, exceptions, regressions or unexpected results.

## Procedure
1. Collect symptom, version/build, environment, steps, expected/actual, logs and the last known good commit.
2. Form a falsifiable hypothesis and a minimal repro; establish a measurement before fixing.
3. Isolate one variable per experiment. Compare regressions with `git diff` and `git bisect` when feasible.
4. Fix the demonstrated cause and add a regression test; run relevant tests and record command + output.

## Exit criteria
Deliver a confirmed cause or ranked hypotheses, evidence, the change, and validation performed/not performed.
