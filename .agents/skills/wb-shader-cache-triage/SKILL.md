---
name: wb-shader-cache-triage
description: Diagnose shader cache misses, invalidation and recompilation stutter
version: 1.1.0
---
# wb-shader-cache-triage

**Suggested role:** `graphics-performance`. This file is portable and does not depend on that agent existing.

## When to use
Micro-stutter, flicker, invalid caches, PSOs and shader precompilation.

## Procedure
1. Identify the pipeline, shader hashes, driver, cache key, invalidation, artifacts and game mapping.
2. Capture misses/hits, compilation per scene and timing; distinguish runtime discovery from persistence failures.
3. Compare cold vs warm start and stable vs current version, with the same scene.
4. Validate reflections, shadows, particles, transparency and fallback features after changes.

## Output
Comparable metrics, visual risk, recommendation and rollback plan.
