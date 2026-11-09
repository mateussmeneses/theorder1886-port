# Drives the clean-room loop on Windows: analyst -> spec -> leak check -> rebuilder (container) -> verifier.
param(
  [Parameter(Mandatory)] [string]$Target,
  [Parameter(Mandatory)] [string]$Feature,
  [int]$MaxRounds = 3,
  [string]$Container = 'rea-clean'
)
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot 'lab')

claude -p "Use the analyst agent on $Target. Feature: $Feature"
claude -p "Use the spec-writer agent to write handoff/SPEC.md from analysis/evidence.md"

for ($round = 1; $round -le $MaxRounds; $round++) {
  aiwh spec check handoff/SPEC.md
  if ($LASTEXITCODE -ne 0) { Write-Error 'spec leaks analysis details, stopping'; exit 1 }
  docker cp handoff/SPEC.md "${Container}:/work/SPEC.md"
  docker exec $Container claude -p "Use the rebuilder agent on /work/SPEC.md"
  if (Test-Path rebuild) { Remove-Item -Recurse -Force rebuild }
  docker cp "${Container}:/work/src" ./rebuild

  claude -p "Use the verifier agent."
  $last = Get-Content report.md -Tail 1
  Write-Output $last
  if ($last -match 'RESULT wrong=0 crash=0') { Write-Output "parity reached in round $round"; exit 0 }

  claude -p "Use the spec-writer agent to add every wrong or crash divergence in report.md as a case under Test cases in handoff/SPEC.md"
}
Write-Output "no parity after $MaxRounds rounds, read report.md"; exit 2
