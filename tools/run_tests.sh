#!/usr/bin/env bash
# tools/run_tests.sh: builds and runs the unit tests (C/C++ targets of gpu/CMakeLists.txt and the
# Python suite) from the repository root. Windows: run inside MSYS2 CLANG64, e.g.
#   MSYSTEM=CLANG64 /c/msys64/usr/bin/bash.exe -lc 'cd /c/projetos/fork/theorder1886-port && bash tools/run_tests.sh'
set -uo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f msys2-env.sh && $(uname -s) == MINGW* || $(uname -s) == MSYS* ]] && source ./msys2-env.sh
tests=(mp3 videodec services low-handles memory memory-reserve pad-map pad audio-downmix cpu-check isa-emulate isa-stub tsc-clock task-idle
       pipeline-binary-cache pipeline-binary-trim pipeline-binary-initialization motion-history motion-shader buffer-addr64 buffer-inline png-decode
       aliasing blend-rewrite depth-stencil-state depth-alias-copy htile-expand serdes-damaged try-emit particle-srt guest-memory face-workers-guard async-compiler warmup-inbox wave-stats pass-census
       frame-capture lighting-test-knob amd-paths no-msaa async-pipeline-cache pending-builds async-fetch-snapshot
       ui-composition upscaler-support perf-overlay overlay-input compute-pipeline-reuse readback-queue wait-diagnostics
       graphics-settings readback-sources page-state scene-resolution taa-shader camera-motion dma-barriers
       renderpass-barrier shader-resource format-remap multiwave-barriers formatted-ssbo-barriers
       swapchain-policy raw-capture-format)
if [[ $(uname -s) != MINGW* && $(uname -s) != MSYS* ]]; then
    # the runtime loader suites link ../src/compat_win.c and gpu/CMakeLists.txt registers
    # them only under WIN32; skip them on other platforms.
    win_only=(mp3 videodec services low-handles memory memory-reserve pad-map pad audio-downmix cpu-check isa-emulate isa-stub tsc-clock task-idle)
    filtered=()
    for name in "${tests[@]}"; do
        keep=1
        for w in "${win_only[@]}"; do [[ "$name" == "$w" ]] && keep=0 && break; done
        (( keep )) && filtered+=("$name")
    done
    tests=("${filtered[@]}")
fi
declare -A args=([isa-stub]=tests/data/isa_fixture.bin)
# Original eboot bytes for the differential half (tools/task_idle_bytes.py; never committed).
[[ -f out/sub_8a7380.bin ]] && args[task-idle]=out/sub_8a7380.bin
if ! ninja -C out/gpu "${tests[@]/%/-test}" > out/tests-build.log 2>&1; then
    grep -E 'error:|FAILED' out/tests-build.log | head -20; echo 'test build failed (out/tests-build.log)'; exit 1
fi
failed=()
skipped=()
# Default is strict: skip hardware-bound tests only when explicitly requested
# AND the Vulkan preflight conclusively finds no compatible ICD/device.
vulkan_mode=${THEORDER_VULKAN_TESTS:-required}
vulkan_unavailable=0
case "$vulkan_mode" in
    required) ;;
    auto)
        if ! clang -std=c11 -O2 -Wall -Wextra -Werror \
            tests/vulkan_device_probe.c -o out/vulkan-device-probe $( [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]] && echo -lvulkan-1 || echo -lvulkan ) \
            > out/test-vulkan-probe.log 2>&1; then
            failed+=(vulkan-probe-build)
            tail -10 out/test-vulkan-probe.log
        else
            if timeout 30 out/vulkan-device-probe >> out/test-vulkan-probe.log 2>&1; then
                echo 'Vulkan preflight: physical device available, GPU tests required'
            else
                result=$?
                if [[ "$result" == 77 ]]; then
                    vulkan_unavailable=1
                    echo 'Vulkan preflight: ICD/device unavailable; skip only explicit GPU tests'
                else
                    failed+=(vulkan-probe-runtime)
                    echo "Vulkan preflight: unexpected exit $result, not skipping GPU tests"
                    tail -10 out/test-vulkan-probe.log
                fi
            fi
        fi ;;
    *) echo "Invalid THEORDER_VULKAN_TESTS=$vulkan_mode (required|auto)"; exit 2 ;;
esac
# Only these seven tests require a live Vulkan instance/physical device.
vulkan_device_tests=(pipeline-binary-initialization async-pipeline-cache
                     scene-resolution taa-shader camera-motion dma-barriers
                     renderpass-barrier)
# Dependency-free shader cache salt test: do not invalidate all warmed shaders.
if ! clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Igpu/shadps4 \
    tests/test_ssbo_barrier_cache.cpp -o out/ssbo-barrier-cache-test \
    > out/test-ssbo-barrier-cache.log 2>&1 ||
   ! timeout 300 out/ssbo-barrier-cache-test >> out/test-ssbo-barrier-cache.log 2>&1; then
    failed+=(ssbo-barrier-cache)
    tail -5 out/test-ssbo-barrier-cache.log
fi
# Dependency-free diagnostic sampler test (no Vulkan device or copyrighted assets needed).
if ! clang++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread -Igpu/shim \
    tests/test_texture_reject_diag.cpp -o out/texture-reject-diag-test \
    > out/test-texture-reject-diag.log 2>&1 ||
   ! timeout 300 out/texture-reject-diag-test >> out/test-texture-reject-diag.log 2>&1; then
    failed+=(texture-reject-diag)
    tail -5 out/test-texture-reject-diag.log
fi
for t in "${tests[@]}"; do
    if ((vulkan_unavailable)); then
        for gpu_test in "${vulkan_device_tests[@]}"; do
            if [[ "$t" == "$gpu_test" ]]; then
                printf 'SKIP: %s requires Vulkan; no ICD/device on this runner\n' "$t" \
                    | tee "out/test-$t.log"
                skipped+=("$t")
                continue 2
            fi
        done
    fi
    if ! timeout 300 "out/gpu/$t-test" ${args[$t]:-} > "out/test-$t.log" 2>&1; then
        failed+=("$t"); tail -3 "out/test-$t.log"
    fi
done
if ! timeout 300 out/gpu/buffer-inline-test disabled > out/test-buffer-inline-off.log 2>&1; then
    failed+=(buffer-inline-off); tail -3 out/test-buffer-inline-off.log
fi
# Runtime (C) tests that build on their own.
if ! { clang -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c \
         src/runtime_content.c -o out/content-test && out/content-test && out/content-test --addons; } \
     > out/test-content.log 2>&1; then
    failed+=(content); tail -3 out/test-content.log
fi
# The launcher tests need Tk and a Windows home: a Windows Python (WINPYTHON, as package.sh).
python=${WINPYTHON:-}
for candidate in "${LOCALAPPDATA:-/c/Users/${USER:-$(id -un)}/AppData/Local}"/Python/pythoncore-3*/python.exe \
                 "${LOCALAPPDATA:-/c/Users/${USER:-$(id -un)}/AppData/Local}"/Programs/Python/Python3*/python.exe; do
    [[ -z $python && -x $candidate ]] && python=$candidate
done
[[ -z ${USERPROFILE:-} && -d /c/Users/${USERNAME:-${USER:-$(id -un)}} ]] && export USERPROFILE="C:\\Users\\${USERNAME:-${USER:-$(id -un)}}"
"${python:-python}" -m unittest discover -s tests > out/test-python.log 2>&1 || failed+=(python)
tail -1 out/test-python.log
if ((${#failed[@]})); then echo "FAILED: ${failed[*]}"; exit 1; fi
if ((${#skipped[@]})); then
    echo "PASS: CPU/IR/C tests and Python suite; SKIPPED ${#skipped[@]} GPU tests: ${skipped[*]}"
else
    echo "PASS: all ${#tests[@]} native GPU/CPU tests and Python suite"
fi
