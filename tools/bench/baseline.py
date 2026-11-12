"""Release baseline: scenarios S1-S3, cache chain cold -> warm-1 -> warm-2, summary JSON.

    python tools/bench/baseline.py --tag windows-v0.9.0 --out-root D:\\theorder-bench
    python tools/bench/baseline.py --dry-run
    python tools/bench/baseline.py --aa 5 --scenarios S2 --tag windows-v0.9.0-aa
    python tools/bench/baseline.py --compare docs/baselines/A.json docs/baselines/B.json

Every state of a chain is one `tools/batch_runs.py --runs 1` call (the runner owns the game launch,
pad route, isolated profile and manifest). cold starts from the savedata of --user-template; warm-1
copies the profile the cold run left, warm-2 the one warm-1 left. All profiles of a chain are
deleted when the chain ends; only logs, frame CSVs, manifests and the summary JSON stay.
Design: docs/plans/2026-11-12-master-plan/09-BENCHMARK_STRATEGY.md sections 3-6 and 10.
"""
import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SCENARIO_DIR = Path(__file__).resolve().parent / 'scenarios'
BENCH_INI = Path(__file__).resolve().parent / 'bench.ini'
BATCH_RUNS = ROOT / 'tools' / 'batch_runs.py'
BASELINE_DIR = ROOT / 'docs' / 'baselines'
STATES = ('cold', 'warm1', 'warm2')
CACHE_OF = {'cold': 'cold', 'warm1': 'warm', 'warm2': 'warm'}
CONDITION = 'base'
WINDOW_S = 5.0
# (key, label, higher is better)
METRICS = (('fps', 'FPS', True), ('p50', 'P50 ms', False), ('p95', 'P95 ms', False),
           ('p99', 'P99 ms', False), ('p999', 'P99.9 ms', False), ('worst', 'worst ms', False),
           ('windows_over50', '5 s windows >50 ms', False), ('windows_over100', '5 s windows >100 ms', False),
           ('compiles', 'pipeline compiles', False), ('compile_ms', 'compile ms', False),
           ('binary_compiled', 'binary cache compiled', False))


def percentile(values, p):
    """Linear interpolation, as tools/bench/analyze.py."""
    v = sorted(values)
    if not v:
        return None
    k = (len(v) - 1) * p / 100
    f = int(k)
    return v[f] + (v[min(f + 1, len(v) - 1)] - v[f]) * (k - f)


def load_scenario(scenario_id, directory=SCENARIO_DIR):
    return json.loads((Path(directory) / f'{scenario_id}.json').read_text(encoding='utf-8'))


# ---- parsing -------------------------------------------------------------------------------

def read_frame_ms(path):
    """The frame_ms column of a THEORDER_FRAME_CSV trace (time starts at its first frame)."""
    with Path(path).open(encoding='ascii', errors='replace', newline='') as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or 'frame_ms' not in reader.fieldnames:
            raise ValueError(f'{path}: no frame_ms column')
        out = []
        for row in reader:
            try:
                ms = float(row['frame_ms'])
            except (TypeError, ValueError):
                continue
            if math.isfinite(ms) and ms > 0:
                out.append(ms)
    return out


def select_window(frame_ms, window):
    """Frames whose end time (cumulative frame_ms from the first frame) is inside the window.
    window: {start_s, end_s} and/or {last_s} (the final seconds of the trace)."""
    total = sum(frame_ms) / 1000.0
    start = float(window.get('start_s') or 0.0)
    end = window.get('end_s')
    end = float(end) if end is not None else math.inf
    if window.get('last_s'):
        start = max(start, total - float(window['last_s']))
    picked, t = [], 0.0
    for ms in frame_ms:
        t += ms / 1000.0
        if start < t <= end:
            picked.append((t, ms))
    return picked


def frame_metrics(timed):
    """Frame-time statistics of [(end_time_s, ms)]. A 5 s window is counted when it holds a frame
    above the threshold; windows are bins of the end time, counted over the whole selection."""
    ms = [m for _, m in timed]
    if not ms:
        return None
    bins = {}
    for t, m in timed:
        bins[int(t // WINDOW_S)] = max(bins.get(int(t // WINDOW_S), 0.0), m)
    return {
        'frames': len(ms), 'seconds': round(sum(ms) / 1000.0, 3), 'fps': len(ms) * 1000.0 / sum(ms),
        'p50': percentile(ms, 50), 'p95': percentile(ms, 95), 'p99': percentile(ms, 99),
        'p999': percentile(ms, 99.9), 'worst': max(ms),
        'windows': len(bins),
        'windows_over50': sum(1 for m in bins.values() if m > 50),
        'windows_over100': sum(1 for m in bins.values() if m > 100),
    }


def parse_run(record, frame_csv, window):
    """One run of a batch_runs manifest -> flat metrics (None frame stats when the trace is missing)."""
    measurements = record.get('measurements') or {}
    compiles = sum(w.get('pipeline_compiles', 0) for w in measurements.get('frame_stats', []))
    compile_ms = sum(w.get('compile_ms', 0.0) for w in measurements.get('frame_stats', []))
    binary = measurements.get('pipeline_binary_cache') or {}
    run = {'run_id': record.get('run_id'), 'state': record.get('run_state'),
           'ok': record.get('run_state') == 'ok', 'compiles': compiles, 'compile_ms': compile_ms,
           'binary_hits': binary.get('hits'), 'binary_compiled': binary.get('compiled'),
           'gpu': measurements.get('gpu'), 'driver': measurements.get('driver')}
    frames = None
    if frame_csv and Path(frame_csv).is_file():
        frames = frame_metrics(select_window(read_frame_ms(frame_csv), window))
    run['frames'] = frames
    if frames:
        run.update({k: frames[k] for k in ('fps', 'p50', 'p95', 'p99', 'p999', 'worst',
                                           'windows_over50', 'windows_over100')})
    return run


# ---- aggregation ---------------------------------------------------------------------------

def dispersion(values):
    """min / median / max and coefficient of variation (sample stdev / mean; 0 for one value)."""
    values = [v for v in values if v is not None]
    if not values:
        return None
    mean = statistics.mean(values)
    cv = statistics.stdev(values) / mean if len(values) > 1 and mean else 0.0
    return {'n': len(values), 'min': min(values), 'median': statistics.median(values),
            'max': max(values), 'cv': cv}


def summarize_state(runs):
    """Per scenario/state: the ok runs' metrics as value lists with dispersion; failures listed."""
    good = [r for r in runs if r.get('ok') and r.get('frames')]
    metrics = {}
    for key, _, _ in METRICS:
        values = [r.get(key) for r in good if r.get(key) is not None]
        if values:
            metrics[key] = {'values': values, **dispersion(values)}
    return {'n': len(runs), 'ok': len(good), 'metrics': metrics,
            'failures': [{'run_id': r.get('run_id'), 'state': r.get('state') if not r.get('ok')
                          else 'no frame trace'} for r in runs if r not in good]}


# ---- running -------------------------------------------------------------------------------

def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def git_value(*args):
    result = subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def chain_paths(out_root, tag, scenario_id, repeat, state):
    base = Path(out_root) / tag / f'{scenario_id}-r{repeat:02d}' / state
    return {'dir': base, 'profile_root': base / 'profile', 'data_root': base / 'data',
            'manifest': base / 'manifest.json', 'report_dir': base / 'report'}


def batch_command(scenario, state, paths, template, args, python=None):
    """The exact tools/batch_runs.py command for one state of a chain."""
    cond = f'{CONDITION}=THEORDER_CONFIG={args.bench_ini}'
    for key, value in sorted((scenario.get('env') or {}).items()):
        cond += f',{key}={value}'
    cmd = [python or sys.executable, str(BATCH_RUNS), '--runs', '1', '--secs', str(scenario['secs']),
           '--frame-trace', '--scene', f"{scenario['id']} {scenario['title']} ({state})",
           '--cache-state', CACHE_OF[state], '--profile-root', str(paths['profile_root']),
           '--user-template', str(template), '--data-root', str(paths['data_root']),
           '--manifest', str(paths['manifest']), '--report-dir', str(paths['report_dir']),
           '--cond', cond]
    if scenario.get('pad'):
        cmd += ['--pad', scenario['pad']]
    if scenario.get('game_args'):
        cmd += ['--game-args', scenario['game_args']]
    if args.game_dir:
        cmd += ['--game-dir', str(args.game_dir)]
    return cmd


def find_profile(profile_root):
    """The single run profile batch_runs made under <profile_root>/<label>/<run_id>."""
    found = sorted(p for p in (Path(profile_root) / CONDITION).glob('*') if p.is_dir()) \
        if (Path(profile_root) / CONDITION).is_dir() else []
    return found[-1] if found else None


def read_manifest_run(paths, window):
    """(manifest, run dict) after a batch_runs call; run is None when no run was recorded."""
    manifest_path = paths['manifest']
    if not manifest_path.is_file():
        return None, None
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if not manifest.get('runs'):
        return manifest, None
    record = manifest['runs'][0]
    csvs = sorted(Path(paths['data_root']).glob('*/frames.csv'))
    return manifest, parse_run(record, csvs[0] if csvs else None, window)


def run_chain(scenario, repeat, args, runner=None, log=print):
    """cold -> warm1 -> warm2 for one scenario. Returns {state: run dict}; always deletes the
    profiles the chain made. A failed state ends the chain (the next one has no cache to copy)."""
    runner = runner or (lambda cmd: subprocess.run(cmd, cwd=ROOT).returncode)
    results, template, made, manifests, broken = {}, args.user_template, [], [], None
    try:
        for state in args.states:
            if broken:
                results[state] = {'ok': False, 'state': f'skipped: {broken}'}
                continue
            paths = chain_paths(args.out_root, args.tag, scenario['id'], repeat, state)
            code = runner(batch_command(scenario, state, paths, template, args))
            manifest, run = read_manifest_run(paths, scenario['window'])
            if manifest:
                manifests.append(manifest)
            profile = find_profile(paths['profile_root'])
            if profile:
                made.append(paths['profile_root'])
            if run is None:
                run = {'ok': False, 'state': f'batch_runs exit {code}, no run recorded'}
            results[state] = run
            log(f"{scenario['id']} r{repeat} {state}: {run.get('state')}")
            if not run.get('ok') or not profile:
                broken = f'{state} failed, no cache to copy'
            template = profile
    finally:
        for root in made:
            shutil.rmtree(root, ignore_errors=True)
    results['_manifests'] = manifests
    return results


def host_info(manifests):
    """Commit, executable, GPU/driver, OS from the first manifest/run that has them."""
    info = {'commit': git_value('rev-parse', 'HEAD'),
            'dirty': bool(git_value('status', '--porcelain')),
            'exe_sha256': None, 'os': platform.platform(), 'gpu': None, 'driver': None,
            'power_plan': None}
    for manifest in manifests:
        build = manifest.get('build') or {}
        info['exe_sha256'] = info['exe_sha256'] or build.get('exe_sha256')
        info['commit'] = build.get('git_sha') or info['commit']
        info['os'] = (manifest.get('host') or {}).get('platform') or info['os']
        for record in manifest.get('runs', []):
            m = record.get('measurements') or {}
            info['gpu'] = info['gpu'] or m.get('gpu')
            info['driver'] = info['driver'] or m.get('driver')
    if info['exe_sha256'] is None:
        exe = os.environ.get('THEORDER_PROBE')
        exe = Path(exe) if exe else ROOT / 'out' / 'theorder-probe.exe'
        info['exe_sha256'] = sha256_file(exe) if exe.is_file() else None
    if os.name == 'nt':
        plan = subprocess.run(['powercfg', '/getactivescheme'], capture_output=True, text=True)
        match = re.search(r'\((.+)\)', plan.stdout or '')
        info['power_plan'] = match.group(1) if match else None
    return info


def build_summary(tag, args, scenarios, per_state_runs, manifests):
    """per_state_runs: {scenario_id: {state: [run dicts]}}."""
    out = {'schema_version': 1, 'tag': tag, 'created_utc': datetime.now(timezone.utc).isoformat(),
           **host_info(manifests), 'n': args.n, 'bench_ini_sha256': sha256_file(BENCH_INI),
           'scenarios': {}}
    for scenario in scenarios:
        states = per_state_runs.get(scenario['id'], {})
        out['scenarios'][scenario['id']] = {
            'title': scenario['title'], 'secs': scenario['secs'], 'pad': scenario['pad'],
            'window': scenario['window'],
            'states': {state: summarize_state(runs) for state, runs in states.items()}}
    return out


# ---- reports -------------------------------------------------------------------------------

def _fmt(value):
    return '-' if value is None else (f'{value:.2f}' if abs(value) < 1000 else f'{value:.0f}')


def compare_table(a, b, label_a='A', label_b='B'):
    """Markdown table of per-metric medians for every scenario/state both summaries have."""
    lines = [f'| scenario | state | metric | {label_a} | {label_b} | change |', '|---|---|---|---:|---:|---:|']
    for sid in sorted(set(a['scenarios']) & set(b['scenarios'])):
        for state in STATES:
            sa = a['scenarios'][sid]['states'].get(state)
            sb = b['scenarios'][sid]['states'].get(state)
            if not sa or not sb:
                continue
            for key, label, _ in METRICS:
                ma, mb = sa['metrics'].get(key), sb['metrics'].get(key)
                if not ma or not mb:
                    continue
                change = '-' if not ma['median'] else f"{(mb['median'] - ma['median']) / ma['median'] * 100:+.1f}%"
                lines.append(f"| {sid} | {state} | {label} | {_fmt(ma['median'])} (n={ma['n']}) | "
                             f"{_fmt(mb['median'])} (n={mb['n']}) | {change} |")
            for side, s in ((label_a, sa), (label_b, sb)):
                if s['failures']:
                    lines.append(f"| {sid} | {state} | failures {side} | {len(s['failures'])} of {s['n']} | | |")
    return '\n'.join(lines)


def aa_table(summary):
    """Markdown dispersion table (min / median / max / CV) of an A/A summary."""
    lines = ['| scenario | state | metric | n | min | median | max | CV |', '|---|---|---|---:|---:|---:|---:|---:|']
    for sid, scenario in sorted(summary['scenarios'].items()):
        for state in STATES:
            s = scenario['states'].get(state)
            if not s:
                continue
            for key, label, _ in METRICS:
                m = s['metrics'].get(key)
                if m:
                    lines.append(f"| {sid} | {state} | {label} | {m['n']} | {_fmt(m['min'])} | "
                                 f"{_fmt(m['median'])} | {_fmt(m['max'])} | {m['cv'] * 100:.1f}% |")
            if s['failures']:
                lines.append(f"| {sid} | {state} | failures | {len(s['failures'])} of {s['n']} | | | | |")
    return '\n'.join(lines)


# ---- CLI -----------------------------------------------------------------------------------

def default_tag():
    described = git_value('describe', '--tags', '--always') or 'unknown'
    return described if described.startswith('windows-') else f'windows-{described}'


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--tag', help='baseline name, also the file docs/baselines/<tag>.json (default: git describe)')
    p.add_argument('--scenarios', default='S1,S2,S3', help='comma-separated scenario ids')
    p.add_argument('--states', default=','.join(STATES), help='subset of cold,warm1,warm2 (in chain order)')
    p.add_argument('-n', '--n', type=int, default=3, help='chains per scenario (default 3)')
    p.add_argument('--aa', type=int, metavar='N', help='A/A: N identical chains, report the dispersion')
    p.add_argument('--out-root', type=Path,
                   default=Path(os.environ.get('THEORDER_BENCH_ROOT') or ROOT / 'out' / 'bench'),
                   help='run data and profiles (default: env THEORDER_BENCH_ROOT, else out/bench)')
    p.add_argument('--out', type=Path, help='summary JSON (default docs/baselines/<tag>.json)')
    p.add_argument('--user-template', type=Path, default=ROOT / 'user',
                   help='profile whose savedata starts every cold run (copied, never written)')
    p.add_argument('--game-dir', type=Path, help='passed to batch_runs --game-dir')
    p.add_argument('--bench-ini', type=Path, default=BENCH_INI)
    p.add_argument('--min-free-gb', type=float, default=4.0, help='refuse to start below this free space')
    p.add_argument('--dry-run', action='store_true', help='print the batch_runs commands and exit')
    p.add_argument('--compare', nargs=2, metavar=('A.json', 'B.json'), type=Path,
                   help='print a Markdown comparison of two summaries and exit')
    args = p.parse_args(argv)
    args.states = [s for s in args.states.split(',') if s]
    if any(s not in STATES for s in args.states) or not args.states:
        p.error(f'--states must be a subset of {",".join(STATES)}')
    if args.states != [s for s in STATES if s in args.states]:
        p.error('--states must keep chain order cold,warm1,warm2')
    args.scenario_ids = [s for s in args.scenarios.split(',') if s]
    if args.aa is not None:
        args.n = args.aa
    if args.n < 1:
        p.error('n must be at least 1')
    args.tag = args.tag or default_tag()
    if args.aa is not None and not args.tag.endswith('-aa'):
        args.tag += '-aa'
    if ',' in str(args.bench_ini):
        p.error('--bench-ini path must not contain a comma (batch_runs --cond syntax)')
    return args


def dry_run(args, scenarios, out=print):
    for scenario in scenarios:
        for repeat in range(1, args.n + 1):
            template = args.user_template
            for state in args.states:
                paths = chain_paths(args.out_root, args.tag, scenario['id'], repeat, state)
                cmd = batch_command(scenario, state, paths, template, args, python='python')
                out(' '.join(f'"{c}"' if (' ' in c or not c) else c for c in cmd))
                template = paths['profile_root'] / CONDITION / '<run_id>'
            out(f"# then delete {Path(args.out_root) / args.tag / (scenario['id'] + f'-r{repeat:02d}')}/*/profile")


def main(argv=None):
    args = parse_args(argv)
    if args.compare:
        a, b = (json.loads(Path(f).read_text(encoding='utf-8')) for f in args.compare)
        print(compare_table(a, b, a.get('tag', 'A'), b.get('tag', 'B')))
        return 0
    scenarios = [load_scenario(s) for s in args.scenario_ids]
    if args.dry_run:
        dry_run(args, scenarios)
        return 0
    if not args.user_template.is_dir():
        sys.exit(f'user template not found: {args.user_template} (a profile with savedata)')
    Path(args.out_root).mkdir(parents=True, exist_ok=True)
    free_gb = shutil.disk_usage(args.out_root).free / 1e9
    if free_gb < args.min_free_gb:
        sys.exit(f'only {free_gb:.1f} GB free under {args.out_root}; need {args.min_free_gb} (--min-free-gb)')
    per_state, manifests = {}, []
    for repeat in range(1, args.n + 1):
        for scenario in scenarios:
            chain = run_chain(scenario, repeat, args)
            manifests += chain.pop('_manifests')
            for state, run in chain.items():
                per_state.setdefault(scenario['id'], {}).setdefault(state, []).append(run)
    summary = build_summary(args.tag, args, scenarios, per_state, manifests)
    out = args.out or BASELINE_DIR / f'{args.tag}.json'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    print(f'summary: {out}')
    if args.aa is not None:
        print(aa_table(summary))
    print(f'free under out root: {shutil.disk_usage(args.out_root).free / 1e9:.1f} GB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
