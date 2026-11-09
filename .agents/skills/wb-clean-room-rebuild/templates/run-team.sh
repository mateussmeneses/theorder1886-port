#!/usr/bin/env bash
# Drives the clean-room loop: analyst -> spec -> leak check -> rebuilder (container) -> verifier.
set -euo pipefail
TARGET="$1"; FEATURE="$2"; MAX_ROUNDS="${MAX_ROUNDS:-3}"; CONTAINER="${CONTAINER:-rea-clean}"
cd "$(dirname "$0")/lab"

claude -p "Use the analyst agent on $TARGET. Feature: $FEATURE"
claude -p "Use the spec-writer agent to write handoff/SPEC.md from analysis/evidence.md"

for round in $(seq 1 "$MAX_ROUNDS"); do
  aiwh spec check handoff/SPEC.md || { echo "spec leaks analysis details, stopping"; exit 1; }
  docker cp handoff/SPEC.md "$CONTAINER":/work/SPEC.md
  docker exec "$CONTAINER" claude -p "Use the rebuilder agent on /work/SPEC.md"
  rm -rf rebuild && docker cp "$CONTAINER":/work/src ./rebuild

  claude -p "Use the verifier agent."
  tail -n 1 report.md
  grep -q 'RESULT wrong=0 crash=0' report.md && { echo "parity reached in round $round"; exit 0; }

  claude -p "Use the spec-writer agent to add every wrong or crash divergence in report.md as a case under Test cases in handoff/SPEC.md"
done
echo "no parity after $MAX_ROUNDS rounds, read report.md"; exit 2
