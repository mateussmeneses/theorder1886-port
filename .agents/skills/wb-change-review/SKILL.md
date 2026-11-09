---
name: wb-change-review
description: Review diffs and risks before approval, with file:line findings and reproducible failure scenarios
version: 1.1.0
---
# wb-change-review

**Suggested role:** `code-reviewer`. This file is portable and does not depend on that agent existing.

## When to use
Reviewing a PR, comparing old/new code, auditing a refactor or checking for regressions.

## Procedure
1. Identify base, head, commits and changed files; examine the real diff and the callers' context.
2. Classify changes: behavior, API, database, UI, security, performance and compatibility.
3. Prioritize findings P0 to P3 with path/line, failing scenario and reproducible proof.
4. Suggest unit, integration, E2E and acceptance tests; state any verification that could not be run.

## Output
Change summary, improvements/regressions, risks, essential tests and a reasoned decision.
