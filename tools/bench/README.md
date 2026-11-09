# Benchmark analysis scripts

The scripts behind the numbers in `docs/AFFINITY_STABILITY_RETEST.md`,
`docs/PERFORMANCE_BASELINE.md` (2026-11-09) and `docs/PIPELINE_SESSION_REBUILDS.md`. They read a
batch directory produced by `tools/batch_runs.py` and are run from inside it (relative paths).

| Script | Input | Output |
|---|---|---|
| `analyze.py <data dir> <log dir> <available LPs> <batch log> [--json out.json]` | one batch | per-run and per-condition frame time and CPU over the last 45 s |
| `combine.py` | `full.json`, `p8.json`, `p4*.json`, `p2*.json` | affinity tables (`combined.json`) |
| `retest_combine.py r4p1 … [--json retest.json]` | `batch-<b>.log`, `<b>.json`, `logs/<b>/` | 4P retest outcome, Wilson intervals, memory, pending-map waits |
| `memwatch.ps1 [-Log] [-LimitGB] [-StopFlag]` | — | resource guard during batches: system commit and game private bytes every 2 s |
| `topology.py` | — | logical processor → core map (`topology.json`) |
| `pipeline_chain_compare.py s1.log s2.log …` | session logs with `THEORDER_BINARY_MISS_LOG=1` | binary misses repeated across sessions |
| `permutation_gaps.py <cache/CUSA…> [pgm …]` | a profile's shader cache | permutation indices, metas and where loading stops |
