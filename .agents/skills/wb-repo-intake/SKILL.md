---
name: wb-repo-intake
description: Inspect a repository before any change - structure, build, tests, instructions and risks
version: 1.1.0
---
# wb-repo-intake

**Suggested role:** `triage-orchestrator`. This file is portable and does not depend on that agent existing.

## When to use
Entering a repository, resuming interrupted work or receiving a general analysis request.

## Procedure
1. Check `git status --short`, branch, remotes, language, build files and project instructions.
2. Identify modules, entry points, dependencies, tests, CI and failure points; read files before proposing changes.
3. Look up relevant history and issues; record which observations come from code, logs or assumptions.
4. Deliver an architecture map, critical paths, risks by severity and a small evidence-backed backlog.

## Exit criteria
Do not claim test runs, root causes or compatibility without evidence. Do not change files at this stage without an explicit request.
