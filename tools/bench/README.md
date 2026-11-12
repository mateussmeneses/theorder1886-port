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

## Release baseline (`baseline.py`)

`baseline.py` drives `tools/batch_runs.py` (one `--runs 1` call per state) for the scenarios in
`scenarios/S1.json`..`S3.json` with the fixed settings in `bench.ini` (1080p, windowed, vsync, 60 cap,
upscaler off). Per scenario it runs the chain **cold -> warm-1 -> warm-2** `n` times (default 3):
cold starts from the savedata of `--user-template` (default `user/`), warm-1 copies the profile cold
left, warm-2 the one warm-1 left. Profiles are deleted when each chain ends (even on failure); a
failed state ends its chain. Run data goes under `--out-root` (default env `THEORDER_BENCH_ROOT`,
else `out/bench`). The summary (commit, exe sha256, GPU/driver, OS, P50/P95/P99/P99.9, worst frame,
5 s windows with a frame > 50 / > 100 ms, pipeline compile counters, failures) is written to
`docs/baselines/<tag>.json`.

```
python tools/bench/baseline.py --dry-run                       # print the batch_runs commands
python tools/bench/baseline.py --tag windows-v0.9.0 --out-root D:\theorder-bench
python tools/bench/baseline.py --aa 5 --scenarios S2 --tag windows-v0.9.0   # A/A: dispersion (tag gets -aa)
python tools/bench/baseline.py --compare docs/baselines/A.json docs/baselines/B.json   # Markdown table
```

Frame statistics come from the per-run `frames.csv` (`--frame-trace`), inside the scenario's
measurement window (`start_s`/`end_s` from the first presented frame, or `last_s`). Windows marked
TODO in a scenario's `notes` are unconfirmed. Disk: a chain holds up to three ~1 GB profiles; the tool
refuses to start under `--min-free-gb` (default 4). Tests: `tests/test_baseline.py`.
