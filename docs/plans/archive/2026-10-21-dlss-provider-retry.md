# DLSS provider retry after fallback

## Scope

When an NGX feature creation or evaluation fails, the renderer falls back to FSR 3.1 and
sets `dlss_failed`. A later explicit DLSS selection must clear that failure so DLSS can
be tried again. The change is limited to the existing provider transition and does not
add another upscaler, frame generation, or DLSS version support.

## Steps

1. Capture the current failure with a native behavioral harness using the production
   `OnFrameStart` provider transition and `UseDlss` decision, with light test doubles for
   rendering dependencies. Cover an NGX failure, FSR 3.1 fallback, and DLSS reselection.
2. Check the edge case where DLSS is reselected before the fallback has a frame; keep
   any fix tied to explicit selection rather than a persistent automatic retry.
3. Change the narrow provider state transition, run the harness again, and review the
   diff for unintended provider changes.
4. Run `bash tools/run_tests.sh` and record any environment skips. Run the relevant
   native settings test if the full gate cannot reach it.

## Limits

No RTX/NGX game runtime is available here. The PR must ask @mateussmeneses to validate
the failure, fallback, and reselection in game. A passing harness proves only the
provider-state behavior, not image quality or DLSS 4.5 support.

## Validation record

- A temporary C++ harness under ignored `out/` compiled the production `OnFrameStart`
  and `UseDlss` functions and the DLSS failure branch with rendering doubles. Both
  ordinary and immediate reselection failed before the fix at `UseDlss()`; both passed
  after it. The harness does not exercise NGX or Vulkan image processing.
- `tests/test_upscaler_support.cpp` passed when compiled with `port_settings.cpp` using
  local `g++ -std=c++23 -UNDEBUG`.
- `bash tools/run_tests.sh` stopped before any suite ran: `ninja: command not found`.

Independent review reproduced both retry timings and checked 120 unchanged fallback
frames with no automatic DLSS retry. Resource-category transitions, feature creation
and evaluation retry/reset paths were inspected; no actionable findings. The PR
references #31 and asks @mateussmeneses for runtime/visual validation.
