---
name: wb-binary-recompilation
description: Binary Recompilation workflows for relevant projects
---

# Binary Recompilation

## Evidence first
Inventory original ABI, calling conventions, translated code and runtime imports.

## Procedure
1. Inspect project documentation, source and baseline tests.
2. Establish the stated goal, available resources, constraints and risks.
3. Prepare a minimal change plan with objective acceptance criteria.
4. Implement or evaluate incrementally; capture observed results.
5. Review regressions, maintainability and the next milestone.

## Deliverables
Verify target build and compare deterministic behavior with golden traces.

Do not assume integrations or credentials are available; propose them for user approval.
