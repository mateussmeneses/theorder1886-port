---
name: wb-vulkan-frame-pacing
description: Investigate Vulkan frame pacing and CPU/GPU/IO bottlenecks with A/B measurements
version: 1.1.0
---
# wb-vulkan-frame-pacing

**Suggested role:** `graphics-performance`. This file is portable and does not depend on that agent existing.

## When to use
Stuttering, irregular FPS, GPU/CPU spikes, high VRAM or Vulkan synchronization issues.

## Procedure
1. Identify build, driver, GPU, scene, resolution, renderer, capture and reference version.
2. Measure frametime p50/p95/p99, 1% lows, VRAM/RAM, CPU/GPU usage, loading and shader compilation time.
3. Separate GPU-bound, CPU-bound, IO-bound, shader compilation, PSO/cache and synchronization; design A/B tests.
4. Avoid optimizing by intuition; change one variable at a time and record instruments and settings.

## Output
Hypotheses, reproducible data collection, experiment and keep/revert decision.
