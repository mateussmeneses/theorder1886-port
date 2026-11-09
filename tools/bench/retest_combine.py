"""Combine the 4P retest batches (fixed build) and compare with the v0.8.0 4P batches.

usage: python retest_combine.py r4p1 r4p2 ... [--json retest.json]
Frame/CPU stats: analyze.py JSON (completed runs, last 45 s). Outcomes: batch logs (every run).
Memory: highest 'Process metrics' private MiB of each run log. Pending-wait lines: THEORDER_PENDING_WAIT_LOG.
CI: 95 % Student t on the mean of per-run values; completion: Wilson 95 % interval.
"""
import json, math, re, statistics, sys
from pathlib import Path

T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 15: 2.13,
       19: 2.09, 20: 2.09, 24: 2.07, 29: 2.05}
def t95(df): return T95.get(df) or T95[min(T95, key=lambda k: abs(k - df))]
def ci(v):
    if len(v) < 2: return (statistics.mean(v), 0.0, 0.0)
    m, s = statistics.mean(v), statistics.stdev(v)
    return (m, s, t95(len(v) - 1) * s / math.sqrt(len(v)))
def wilson(k, n, z=1.96):
    p = k / n; d = 1 + z * z / n; c = (p + z * z / (2 * n)) / d; h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return c - h, c + h

batches = [a for a in sys.argv[1:] if not a.startswith('--') and not a.endswith('.json')]
out_json = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
runs = []
for b in batches:
    text = Path(f'batch-{b}.log').read_text(encoding='utf-8', errors='replace')
    rows = {r['run']: r for r in json.loads(Path(f'{b}.json').read_text())['rows']}
    for m in re.finditer(r'^\s*(\w+)\s+(\d+): flip\s+(\d+) compile\s+[0-9.]+s (.+)$', text, re.M):
        label, idx, flips, status = m[1], int(m[2]), int(m[3]), m[4].strip()
        logs = sorted(Path(f'logs/{b}').glob(f'{label}-{idx:02d}-*.log'))
        lt = logs[0].read_text(encoding='utf-8', errors='replace') if logs else ''
        priv = [float(x) for x in re.findall(r'Process metrics: .*?private ([0-9.]+) MiB', lt)]
        waits = re.findall(r'pending pipelines changed while waiting for \S+: (\d+) -> (\d+) entries, (\d+) -> (\d+) buckets', lt)
        row = next((r for k, r in rows.items() if k.startswith(f'{label}-{idx:02d}-')), None)
        if 'bad_alloc' in lt: cause = 'bad_alloc'
        elif status.startswith('FAULT'): cause = 'native fault ' + status.split()[-1]
        elif 'HUNG' in status: cause = 'watchdog'
        elif status != 'ok': cause = status
        else: cause = 'ok'
        runs.append(dict(batch=b, label=label, idx=idx, flips=flips, status=status, cause=cause, row=row,
                         max_private_mib=max(priv) if priv else None, stalls=len(re.findall(r' STALLED$', lt, re.M)),
                         waits_changed=len(waits), waits_rehash=sum(w[2] != w[3] for w in waits),
                         waits_shrank=sum(int(w[1]) < int(w[0]) for w in waits)))

KEYS = ('fps', 'p50', 'p95', 'p99', 'p999', 'worst', 'over33', 'over100', 'cpu', 'cores', 'sync_wait_ms')
summary = {}
for label in ('base', 'idle2'):
    every = [r for r in runs if r['label'] == label]
    ok = [r for r in every if r['cause'] == 'ok']
    lo, hi = wilson(len(ok), len(every))
    s = dict(runs=len(every), ok=len(ok), completion_ci95=(round(lo, 3), round(hi, 3)),
             failures=[f"{r['batch']}/{label}-{r['idx']:02d}: {r['cause']} (flip {r['flips']})" for r in every if r['cause'] != 'ok'],
             stats={k: [round(x, 3) for x in ci([r['row'][k] for r in ok if r['row']])] for k in KEYS},
             median={k: round(statistics.median(r['row'][k] for r in ok if r['row']), 3) for k in ('fps', 'p99', 'p999', 'worst')},
             max_private_gib=round(max(r['max_private_mib'] for r in every if r['max_private_mib']) / 1024, 2),
             private_gib_ci=[round(x / 1024, 3) for x in ci([r['max_private_mib'] for r in ok])],
             waits_changed=[r['waits_changed'] for r in every], waits_rehash=sum(r['waits_rehash'] for r in every),
             stalls_mean=round(statistics.mean(r['stalls'] for r in every), 1),
             per_run_p99=[round(r['row']['p99'], 1) for r in ok], per_run_p999=[round(r['row']['p999'], 1) for r in ok],
             per_run_worst=[round(r['row']['worst'], 1) for r in ok])
    summary[label] = s
    print(f"\n## {label}: {len(ok)}/{len(every)} complete (Wilson 95 % {lo:.2f}-{hi:.2f}); failures: {s['failures']}")
    for k in KEYS:
        m, sd, h = s['stats'][k]
        print(f"  {k:12} mean {m:9.2f}  sd {sd:8.2f}  95% CI +-{h:7.2f}")
    print(f"  median fps/p99/p99.9/worst: {s['median']}")
    print(f"  max private {s['max_private_gib']} GiB; per-run peak mean {s['private_gib_ci']}")
    print(f"  waits with pending map changed per run: {s['waits_changed']} (rehash during wait: {s['waits_rehash']}); STALLED draws per run mean {s['stalls_mean']}")
    print(f"  per-run p99 {s['per_run_p99']}\n  per-run p99.9 {s['per_run_p999']}\n  per-run worst {s['per_run_worst']}")

old = json.loads(Path('combined.json').read_text()).get('4P (0x5500)', {})
print('\n## v0.8.0 4P (p4, p4b, p4c, p4d) for comparison')
for label in ('base', 'idle2'):
    o = old.get(label)
    if o:
        st = o['stats']
        print(f"  {label}: {o['ok']}/{o['runs']} complete, failed {o['failed']}; fps {st['fps'][0]:.2f} p99 {st['p99'][0]:.2f} "
              f"p99.9 {st['p999'][0]:.2f} worst {st['worst'][0]:.1f} cpu {st['cpu'][0]:.2f}")
if out_json:
    Path(out_json).write_text(json.dumps({'summary': summary, 'runs': [{k: v for k, v in r.items() if k != 'row'} for r in runs]}, indent=1))
