#!/usr/bin/env bash
# tools/release.sh VERSION GAME_DIR: build, test, package, smoke and publish windows-vVERSION from
# an up-to-date, clean master. Run inside MSYS2 CLANG64, e.g.
#   MSYSTEM=CLANG64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash.exe -lc \
#     'cd /c/projetos/theorder1886-port && bash tools/release.sh 0.8.4 /c/games/CUSA00035'
# The version bump, docs/release-notes-vVERSION.md and docs/REVIEW_vVERSION.md come first, in a
# reviewed PR merged to master (AGENTS.md). Nothing is published until every step has passed;
# DRY_RUN=1 stops before the tag.
# RETEST_ISSUES="<numbers>" labels the issues this release fixes for their reporters' retest.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
# Git for Windows (core.autocrlf=true in its system config) and the GitHub CLI first: MSYS2's own
# git has no autocrlf, so every CRLF checkout looked modified and the clean-tree check failed; and
# the login shell's PATH has no gh. (Seen on the v0.9.0 release.)
for tool_dir in "/c/Program Files/Git/cmd" "/c/Program Files/GitHub CLI"; do
    [[ -d $tool_dir ]] && PATH="$tool_dir:$PATH"
done
export PATH
version=${1:?usage: tools/release.sh VERSION GAME_DIR}
game_dir=${2:?usage: tools/release.sh VERSION GAME_DIR}
tag="windows-v$version"
notes="docs/release-notes-v$version.md"
review="docs/REVIEW_v$version.md"
fail() { echo "release: $*" >&2; exit 1; }

# 1. Preconditions: master, clean (submodule build patches aside), synced, documents present.
[[ $(git branch --show-current) == master ]] || fail 'not on master'
dirty=$(git status --porcelain --ignore-submodules=dirty)
[[ -z $dirty ]] || fail "working tree not clean:"$'\n'"$dirty"
git fetch -q origin
[[ $(git rev-parse HEAD) == $(git rev-parse origin/master) ]] || fail 'master differs from origin/master'
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && fail "$tag already exists"
grep -q "^VERSION = '$version'" launcher/port_launcher_win.py || fail "launcher VERSION is not $version"
[[ -s $notes ]] || fail "missing $notes"
[[ -s $review ]] || fail "missing $review"
grep -q 'Decision: release' "$review" || fail "$review has no release decision"

# 2. Build and the full test gate.
./build.sh > out/release-build.log 2>&1 || fail 'build failed (out/release-build.log)'
bash tools/run_tests.sh || fail 'tests failed'
rm -f ./*.spv

# 3. Package (a Windows Python for PyInstaller; the login shell may lack LOCALAPPDATA).
if [[ -z ${WINPYTHON:-} ]]; then
    for candidate in /c/Users/*/AppData/Local/Programs/Python/Python3*/python.exe /c/Python3*/python.exe; do
        [[ -x $candidate ]] && WINPYTHON=$candidate
    done
fi
[[ -n ${WINPYTHON:-} ]] || fail 'no Windows Python found; set WINPYTHON'
WINPYTHON=$WINPYTHON bash packaging/windows/package.sh > out/release-package.log 2>&1 ||
    fail 'packaging failed (out/release-package.log)'
(cd dist && sha256sum theorder1886-windows.zip > theorder1886-windows.zip.sha256)

# 4. Smoke the package itself, cold then warm, on the real game.
python tools/smoke_package.py --game-dir "$game_dir" || fail 'package smoke failed (out/smoke/logs)'
rm -rf out/stage out/pyi-work

[[ ${DRY_RUN:-0} == 1 ]] && { echo "DRY_RUN: $tag ready, not published"; exit 0; }

# 5. Publish.
git tag -a "$tag" -m "Windows v$version"
git push -q origin "$tag"
title=$(head -1 "$notes" | sed 's/^# //')
gh release create "$tag" dist/theorder1886-windows.zip dist/theorder1886-windows.zip.sha256 \
    --title "$title" --notes-file "$notes"

# 6. The issues this release fixes, waiting for their reporters: RETEST_ISSUES="113 99" labels them
# for retest (the retest comment is written per issue). Not every issue the notes mention: they
# also list open problems and other projects' issue numbers (v0.9.0 named #138 and SpartanPort's #1101).
for issue in ${RETEST_ISSUES:-}; do
    gh issue edit "$issue" --add-label status:needs-retest >/dev/null 2>&1 &&
        echo "labelled #$issue status:needs-retest (post the retest request in the thread)"
done
echo "published $tag"
