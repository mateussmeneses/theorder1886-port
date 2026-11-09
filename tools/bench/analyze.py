"""Per-run and per-condition stats for the affinity benchmark.

usage: python analyze.py <data dir> <log dir> <available logical CPUs> <batch log> [--json out.json]
Window: the last WINDOW seconds of presented frames (the prologue); every frame/CPU metric uses it.
Condition statistics use only runs that finished 'ok'; failed runs are listed with their status.
CPU: 'Process metrics: CPU x%' (THEORDER_PERF_LOG) is normalized over all 24 logical CPUs
(std::thread::hardware_concurrency); cores = x * 24 / 100; per available = cores / N.
Wait columns of frames.csv overlap (they are per-thread waits), so they attribute, not add up.
"""
import csv
import json
import re
import statistics
import sys
from pathlib import Path

WINDOW_S = 45.0
SYSTEM_LPS = 24
data_dir, log_dir, available = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
batch_text = Path(sys.argv[4]).read_text(encoding='utf-8', errors='replace')
out_json = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
STATUS = {(m[0], int(m[1])): (int(m[2]), m[3].strip()) for m in
          re.findall(r'^\s*(\w+)\s+(\d+): flip\s+(\d+) compile\s+[0-9.]+s (.+)$', batch_text, re.M)}
WAITS = ('compile_wait_ns', 'readback_wait_ns', 'gpu_tick_wait_ns', 'recorder_wait_ns', 'host_copy_wait_ns',
         'driver_ns')


def pct(v, p):
    v = sorted(v)
    k = (len(v) - 1) * p / 100
    f = int(k)
    return v[f] + (v[min(f + 1, len(v) - 1)] - v[f]) * (k - f)


rows = []
for d in sorted(data_dir.iterdir()):
    fr = d / 'frames.csv'
    if not fr.exists():
        continue
    label, index = d.name.split('-')[0], int(d.name.split('-')[1])
    flips, state = STATUS.get((label, index), (0, '?'))
    frames = list(csv.DictReader(fr.open()))
    tail, acc = [], 0.0
    for r in reversed(frames):
        if acc >= WINDOW_S * 1000:
            break
        tail.append(r)
        acc += float(r['frame_ms'])
    ms = [float(r['frame_ms']) for r in tail]
    if not ms:  # failed before presenting (counted from the batch log)
        continue
    slow = [r for r in tail if float(r['frame_ms']) > 33.4]
    log = log_dir / f'{d.name}.log'
    text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
    draws = [int(x) for x in re.findall(r'(\d+) draws/frame', text)]
    cpu = [float(c) for c in re.findall(r'Process metrics: CPU ([0-9.]+)%', text)]
    n = max(1, int(WINDOW_S // 5))
    cpu_tail = cpu[-n - 1:-1] if len(cpu) > n else cpu
    stats = re.findall(r'Frame stats: .*?; (\d+) shader/pipeline compiles, ([0-9.]+) ms; .*?sync wait ([0-9.]+) ms',
                       text)[-n:]
    cache = re.findall(r'Pipeline binary cache: (\d+) hits, (\d+) compiled', text)
    cpu_mean = statistics.mean(cpu_tail) if cpu_tail else float('nan')
    cores = cpu_mean * SYSTEM_LPS / 100
    rows.append(dict(
        label=label, run=d.name, status=state, flips=flips, max_draws=max(draws) if draws else 0,
        frames=len(ms), fps=len(ms) / (sum(ms) / 1000) if ms else 0.0,
        p50=pct(ms, 50), p95=pct(ms, 95), p99=pct(ms, 99), p999=pct(ms, 99.9), worst=max(ms),
        over33=sum(x > 33.4 for x in ms), over50=sum(x > 50 for x in ms), over100=sum(x > 100 for x in ms),
        over200=sum(x > 200 for x in ms), slow_ms=sum(float(r['frame_ms']) for r in slow),
        slow_attr={k[:-3]: sum(float(r[k]) for r in slow) / 1e6 for k in WAITS},
        cpu=cpu_mean, cores=cores, cores_per_available=cores / available,
        compiles=sum(int(s[0]) for s in stats), compile_ms=sum(float(s[1]) for s in stats),
        sync_wait_ms=sum(float(s[2]) for s in stats),
        final_cache_hits=int(cache[-1][0]) if cache else None, final_compiled=int(cache[-1][1]) if cache else None,
        task_idle=(re.search(r'Task idle: mode \d[^\n]*', text) or [None])[0],
    ))

KEYS = ('fps', 'p50', 'p95', 'p99', 'p999', 'worst', 'over33', 'over50', 'over100', 'over200', 'cpu', 'cores',
        'cores_per_available', 'compiles', 'compile_ms', 'sync_wait_ms')
print(f"{'run':30} {'fps':>6} {'p50':>6} {'p95':>6} {'p99':>7} {'p99.9':>8} {'worst':>8} {'>33':>4} {'>50':>4} "
      f"{'>100':>4} {'>200':>4} {'cpu%':>6} {'cores':>5} {'/avail':>6} {'comp':>5} {'syncms':>8} status")
for r in rows:
    print(f"{r['run'][:30]:30} {r['fps']:6.2f} {r['p50']:6.2f} {r['p95']:6.2f} {r['p99']:7.2f} {r['p999']:8.2f} "
          f"{r['worst']:8.1f} {r['over33']:4} {r['over50']:4} {r['over100']:4} {r['over200']:4} {r['cpu']:6.2f} "
          f"{r['cores']:5.2f} {r['cores_per_available']:6.2f} {r['compiles']:5} {r['sync_wait_ms']:8.1f} "
          f"{r['status'][:14]} flips={r['flips']} draws<={r['max_draws']}")
summary = {}
for label in sorted({r['label'] for r in rows}):
    every = [r for r in rows if r['label'] == label]
    sel = [r for r in every if r['status'] == 'ok']
    failed = [f"{r['run'][:8]}: {r['status']}" for r in every if r['status'] != 'ok']
    flips = [r['flips'] for r in every]
    if not sel:
        print(f"\n{label}: no completed run; failed {failed}; flips {flips}")
        continue
    s = {k: (statistics.mean(r[k] for r in sel), statistics.stdev(r[k] for r in sel) if len(sel) > 1 else 0.0)
         for k in KEYS}
    s.update(runs=len(sel), failed=failed, flips_all_runs=flips, slow_ms=statistics.mean(r['slow_ms'] for r in sel),
             slow_attr_ms={a: statistics.mean(r['slow_attr'][a] for r in sel) for a in sel[0]['slow_attr']})
    summary[label] = s
    print(f"\n{label}: ok n={len(sel)}, failed {failed}, flips per run {flips}")
    print('  ' + ' '.join(f"{k}={s[k][0]:.2f}+-{s[k][1]:.2f}" for k in KEYS))
    print(f"  frames >33 ms: {s['slow_ms']:.0f} ms per run; waits inside them (ms per run, overlapping): " +
          ', '.join(f"{a}={v:.0f}" for a, v in s['slow_attr_ms'].items()))
if out_json:
    Path(out_json).write_text(json.dumps({'rows': rows, 'summary': summary}, indent=1))
