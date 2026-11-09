"""Combine per-batch analyze.py JSON into one table per CPU configuration and condition."""
import json
import statistics
from pathlib import Path

GROUPS = {'full (24 LPs)': ['full'], '8P (0x5555)': ['p8'], '4P (0x5500)': ['p4', 'p4b', 'p4c', 'p4d'],
          '2P (0x5000)': ['p2', 'p2b']}
KEYS = ('fps', 'p50', 'p95', 'p99', 'p999', 'worst', 'over33', 'over50', 'over100', 'over200', 'cpu', 'cores',
        'cores_per_available', 'compile_ms', 'sync_wait_ms')
out = {}
for name, batches in GROUPS.items():
    rows = []
    for b in batches:
        p = Path(f'{b}.json')
        if p.exists():
            rows += json.loads(p.read_text())['rows']
    out[name] = {}
    print(f'\n### {name}')
    for label in ('base', 'idle1', 'idle2'):
        every = [r for r in rows if r['label'] == label and r['status'] != '?']
        if not every:
            continue
        ok = [r for r in every if r['status'] == 'ok']
        failed = [r['status'] for r in every if r['status'] != 'ok']
        s = {k: (statistics.mean(r[k] for r in ok), statistics.stdev(r[k] for r in ok) if len(ok) > 1 else 0.0)
             for k in KEYS} if ok else {}
        attr = {a: statistics.mean(r['slow_attr'][a] for r in ok) for a in ok[0]['slow_attr']} if ok else {}
        out[name][label] = dict(runs=len(every), ok=len(ok), failed=failed, flips=[r['flips'] for r in every],
                                stats=s, slow_attr_ms=attr,
                                per_run={k: [round(r[k], 2) for r in ok] for k in ('p95', 'p99', 'p999', 'cpu', 'fps')})
        print(f'{label}: runs {len(every)}, ok {len(ok)}, failed {failed}')
        if ok:
            print('  ' + ' '.join(f'{k}={s[k][0]:.2f}+-{s[k][1]:.2f}' for k in KEYS))
            print('  per run: ' + json.dumps(out[name][label]['per_run']))
            print('  waits in frames >33 ms (ms/run): ' + ', '.join(f'{a}={v:.0f}' for a, v in attr.items()))
Path('combined.json').write_text(json.dumps(out, indent=1))
