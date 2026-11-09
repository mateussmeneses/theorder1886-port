---
name: wb-test-planning
description: Plan tests from risks and requirements, mapping each scenario to the right test layer
version: 1.1.0
---
# wb-test-planning

**Suggested role:** `qa-strategist`. This file is portable and does not depend on that agent existing.

## When to use
New features, bug fixes, requirement changes or acceptance plans.

## Procedure
1. Extract observable business rules and acceptance criteria; separate gaps from facts.
2. Cover positive, negative, edge, permission, state, persistence, failure and concurrency cases.
3. Map each scenario to the ideal layer: unit, contract/API, integration, UI, exploratory.
4. Define test data, preconditions, postconditions, priority and execution evidence.

## Output
Requirement x scenario x priority x automation x result matrix, without inventing evidence.
