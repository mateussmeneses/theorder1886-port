---
name: wb-gameplay-acceptance
description: Accept a game port scene by scene with regression evidence per build
version: 1.1.0
---
# wb-gameplay-acceptance

**Suggested role:** `release-auditor`. This file is portable and does not depend on that agent existing.

## When to use
Testing after a PR, or changes to input, launcher, renderer, audio or graphics features.

## Procedure
1. Prepare a matrix of scenes, hardware/driver, mouse-keyboard/controller input, cold/warm launch.
2. Check menus, resolution, DLSS/FSR when implemented, save/load, shadows, particles, audio and crashes.
3. Capture video/logs, baseline, version, FPS/frametime and evidence of side effects.
4. Separate pre-existing defects from new ones; record blockers and acceptance status per build.

## Output
Regression checklist, known results and release risks.
