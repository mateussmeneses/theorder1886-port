---
name: wb-pr-release
description: Prepare a PR and release checklist without automatic merges
version: 1.1.0
---
# wb-pr-release

**Suggested role:** `release-auditor`. This file is portable and does not depend on that agent existing.

## When to use
Planning a delivery, consolidating a PR, a release candidate or a version regression.

## Procedure
1. Check `git status`, source branch, changelog, compatibility matrix and tests.
2. Keep changes traceable, small and reversible; identify migrations and flags.
3. Prepare the PR summary, evidence, limitations, rollback, acceptance and pending manual tests.
4. Do not merge into the main branch without explicit authorization.

## Output
Go/No-Go conditioned on evidence; open items by severity.
