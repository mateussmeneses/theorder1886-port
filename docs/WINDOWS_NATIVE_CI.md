# Windows native CI validation

The workflow `.github/workflows/windows-native-validation.yml` builds and
tests the port on a clean GitHub-hosted Windows 2022 runner using MSYS2 CLANG64.
It runs on PRs touching the native runtime, renderer, build or tests. It can
also be launched explicitly with the GitHub Actions workflow-dispatch UI.

The job installs the dependencies listed in
`packaging/windows/README.md`, checks out submodules recursively, executes
`bash build.sh` and `bash tools/run_tests.sh`. It only uploads unit-test
**text logs** on failure (seven-day retention); no game files, save data,
private gameplay logs or shader caches are included.

The Windows-hosted build can consume significant GitHub Actions resources;
watch quota and runner availability. If package install or submodule download
fails, that is an infrastructure blocker, not a renderer regression.
Do not suppress build/test errors to obtain a green badge.

Passing means clean Windows compilation/linking and C/C++ and Python unit
tests passed. It is **not** GPU/driver/real gameplay evidence. Black-screen
issues #43/#47, graphics artifacts, scene-specific flicker, and #68's
cutscene stutters require separate matching hardware/capture validation.

## Selecting the correct MSYS2 installation

GitHub-hosted Windows runners can have multiple MSYS2 installations: a
preinstalled one under `C:\\msys64` and the current `msys2/setup-msys2`
instance under a temporary `D:\\a\\_temp\\msys64` path. The helper
`msys2-env.sh` must prepend the **active shell's** virtual `/clang64/bin`
and `/usr/bin` paths, rather than selecting the unrelated `C:\\msys64`
installation merely because it exists. Mixing those roots may make all
native tests fail silently due to incompatible dependent runtime DLLs.

An explicitly configured `MSYS2_ROOT` still takes priority (e.g. an
external Git Bash shell). Only when neither the override nor the current
virtual MSYS2 root is usable does the helper fall back to `C:\\msys64`.
The native CI must verify that unit-test binaries **execute** after the build;
compilation alone is not a passing result.

## Hosted Windows: missing Vulkan ICD versus genuine regressions

GitHub-hosted Windows runners can compile and link the native port without
providing a working Vulkan physical device. Seven renderer-integration tests
require an actual Vulkan instance: `pipeline-binary-initialization`,
`async-pipeline-cache`, `scene-resolution`, `taa-shader`, `camera-motion`,
`dma-barriers`, and `renderpass-barrier`.

The workflow explicitly sets `THEORDER_VULKAN_TESTS=auto`. The runner first
compiles and executes `tests/vulkan_device_probe.c` through the normal Vulkan
loader. Only `VK_ERROR_INCOMPATIBLE_DRIVER` or an instance reporting no
physical devices produce exit 77 and allow **those seven tests only** to be
marked `SKIP`. Unexpected driver/loader errors, probe compilation errors,
and all remaining CPU, IR, C++, and Python test failures stay **red**. A working
Vulkan device means the seven tests are **required** even in `auto` mode.

The default for `bash tools/run_tests.sh` is strict
(`THEORDER_VULKAN_TESTS=required`): no automatic skipping. CI output reports
the exact names/count of tests skipped. Successful hosted CI therefore means
native compilation and the CPU/IR/Python suite passed; it does **not** mean
Vulkan integration tests ran. Those tests must be executed on an actual GPU
before claiming a graphics fix or resolving black-screen/stutter issues.

## Windows package and test builds

`.github/workflows/windows-package.yml` ("Windows package", run by hand from the Actions tab on the
branch to build) builds the same CLANG64 tree as the gate, runs `packaging/windows/package.sh`,
writes `theorder1886-windows.zip.sha256` and keeps both as the workflow artifact `theorder1886-windows`
(14 days). A second job on a clean runner without MSYS2 then runs `packaging/windows/smoke-test.ps1`:
it checks the sha256, extracts the zip, looks for the expected files and for files that must not
ship, checks that every DLL imported by each executable is in the package or in System32, runs
`theorder-probe.exe --cpu-features`, and starts the launcher with `--recomplauncher-prepare` on an
empty folder (exit code 1 and the "Incomplete" message, no window). CI has no game dump, so this is
not the real smoke: `tools/smoke_package.py --game-dir ...` stays a local step of `tools/release.sh`.

Inputs:

- `tag` empty: build and smoke only.
- `tag` = a new `windows-v<version>-issue<N>-test<M>`: after the smoke passes, a pre-release is created
  on the built commit with the zip and sha256, and a note asking the reporter to retest in issue #N.
- any other new tag is refused (releases come from `tools/release.sh`); an existing tag is refused
  unless `overwrite_existing` is true, which replaces only that release's zip and sha256.

Only the final `publish` job has `contents: write`; the workflow uses nothing but `GITHUB_TOKEN`.
Delete a test pre-release (and its tag) when the issue is closed. A workflow can only be dispatched
from the Actions tab or `gh workflow run windows-package.yml --ref <branch>` once the file exists on
the default branch.
