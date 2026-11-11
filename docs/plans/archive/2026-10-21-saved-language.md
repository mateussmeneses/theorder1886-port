# Read saved language preferences — issue #28

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/28

## Diagnosis and scope

`run.py::resolve_language` reads portable and legacy launcher `settings.json`
files using `json.loads`, but `run.py` never imports `json`. The surrounding
exception handler hides the resulting `NameError`, so a saved language such as
Portuguese (Brazil), PS4 id 17, is ignored and the system locale is used instead.
CLI, environment, and INI preferences take precedence and are unaffected.

This is a small deterministic defect related to #28, independently reproducible
with synthetic settings and a mocked English system locale. The current
regional engine-language fallback is intentional and is outside this change.
The mouse report #37 needs gameplay tuning beyond a small UI range mismatch;
the AppContent service-label error in #7 is already handled in current code.

## Implementation plan

1. Add focused tests using temporary portable/user/legacy settings directories
   and a fixed locale. Demonstrate that saved PS4 language 17 is ignored before
   the fix. Cover both legacy launcher names and preserve CLI/environment/INI
   precedence and invalid-JSON fallback.
2. Import the missing standard-library `json` module in `run.py`; preserve
   language IDs, regional asset fallback, settings paths, and precedence.
3. Run the new language tests, existing run/launcher tests, and the full Python
   suite against the unchanged upstream baseline. Record unavailable checks.
4. Obtain independent review, commit, publish this branch from upstream
   `master`, and open a PR against upstream `master` referencing #28.

## Acceptance and limits

- A saved language in each supported JSON settings location is respected when
  no higher-priority preference is set, independently of the system locale.
- CLI, environment, and INI preferences retain their existing precedence.
- Malformed JSON still permits the existing locale/default fallback.
- No changes to game patches, regional language mapping, or game assets.
- The branch is independent of PR #48 and PR #49.
- Synthetic tests prove preference resolution. Audio/subtitle availability for
  each regional game dump and Windows gameplay remain unverified; do not
  automatically close the broader PT-BR issue.

## Validation results

- New language regression suite: 6 test methods. Before the import, 9 failures
  (8 numeric/alias subcases across four locations and portable-file precedence).
  After the import, all 6 methods pass.
- Existing `test_run_isa.py`: 4 passed; `test_launcher_controls.py`: 25 passed.
- Full Python suite: 298 tests, 220 passed, 76 skipped, 2 existing errors caused
  by missing `tkinter`. The unchanged upstream baseline had the same two errors
  and 76 skips across 292 tests.
- `bash tools/run_tests.sh`: unavailable because `ninja` is missing and
  `out/gpu` is not configured. This change only adds a Python standard-library
  import and Python tests; no C/C++ sources change.
- `git diff --check`: passed.
- Independent review found no actionable issues and reran all 10 `test_run_*`
  tests successfully, checking settings, environment, and locale isolation.

The direct-start case requires no higher-priority language choice. The normal
launcher Play action already passes `THEORDER_LANGUAGE` and therefore bypasses
the broken JSON fallback; this change does not imply that every Play launch
previously ignored its language selection.
