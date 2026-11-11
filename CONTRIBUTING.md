# Contributing to The Order: 1886 PC Port

Thanks for testing, reporting, reviewing and improving the port. This is a work-in-progress
native PC adaptation with Vulkan translation; not all GPUs, releases and chapters are validated.

## Reporting a bug

Use the issue form that matches the symptom. Include the port release tag, game CUSA/update,
CPU, GPU and driver, chapter/checkpoint, graphics settings and a repeatable sequence.
Attach a redacted `user/last_run.log` or the launcher's **Export Bug Report** ZIP.
For stutters, distinguish first visit from a warm-cache revisit; raw frame times are more
useful than average FPS alone. For graphics bugs, use a screenshot or short capture with a
repeatable camera. **Do not upload copyrighted game dumps, saves, personal paths or tokens.**

A positive result from a test harness does not automatically close a hardware-specific report.
An issue may be marked as duplicate and linked to a master issue without losing its evidence.

## Sending a pull request

- Open a focused PR from `master`; avoid mixing launcher, shader and documentation changes.
- Reference the relevant issue and describe the user-visible regression being addressed.
- Include a deterministic regression test when possible. Run `bash tools/run_tests.sh` and
  report any platform-related skips; renderer fixes also need a matching gameplay retest.
- Describe the exact source commit/tag and retain required upstream attribution and license
  notices when porting code or ideas from SpartanPort, shadPS4, TOport, or other projects.
- Avoid claims such as "fixed all flicker", "zero stutters" or "native 4K" without matching
  measurements and evidence.
- `master` only accepts pull requests, and the `gate` check (the full Windows CLANG64 build and
  tests) must pass before a merge; documentation-only PRs pass it without the build.
- A fix that depends on a shader hash, a layout check or a toggle should show that it applies in
  the scene it targets (a log line or counter from a real run), not only that it compiles.

## Issue labels

`needs:log` means we cannot continue without `user/last_run.log`; it is closed after 21 days
without one (reopen it with the log). `status:needs-retest` means a fix shipped in the release
named in the thread: please say whether the problem is gone. Issues close as fixed after that
retest.

## Conduct and moderation

Be direct about technical shortcomings. Reports that challenge existing release claims or
cite missing credits are welcome when supported by evidence. Avoid personal insults,
mockery directed at contributors/reporters, harassment, spam and repeated off-topic comments.
Maintainers may edit or hide inappropriate comments, request missing data, or lock persistently
abusive threads. Disagreement about code is not itself grounds to silence a report.

## Support

Development support is optional and does not affect issue priority or guarantee a particular
fix. See the project's [Ko-fi](https://ko-fi.com/mateusmeneses) if you want to contribute.