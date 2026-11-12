"""tools/bench/baseline.py on synthetic frame traces and manifests (no game, no GPU)."""
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / 'tools' / 'bench'))
import baseline  # noqa: E402


def write_frames(path, intervals):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('frame,frame_ms\n' + ''.join(f'{i + 1},{ms}\n' for i, ms in enumerate(intervals)))


def synthetic_args(tmp, **over):
    ns = baseline.parse_args(['--out-root', str(tmp), '--tag', 'windows-test', '--user-template', str(tmp)])
    for key, value in over.items():
        setattr(ns, key, value)
    return ns


class ParserTests(unittest.TestCase):
    def test_window_selection_and_metrics(self):
        # 10 s of 16.667 ms frames, one 120 ms frame near the end of the first 5 s window and one 60 ms later.
        intervals = [16.667] * 600
        intervals[100] = 120.0
        intervals[500] = 60.0
        timed = baseline.select_window(intervals, {'start_s': 0, 'end_s': None})
        metrics = baseline.frame_metrics(timed)
        self.assertEqual(metrics['frames'], 600)
        self.assertEqual(metrics['worst'], 120.0)
        self.assertEqual(metrics['windows_over100'], 1)
        self.assertEqual(metrics['windows_over50'], 2)
        self.assertAlmostEqual(metrics['p50'], 16.667)
        last = baseline.select_window(intervals, {'last_s': 2})
        self.assertLess(len(last), 150)
        self.assertGreater(len(last), 100)
        self.assertEqual(len(baseline.select_window(intervals, {'start_s': 100})), 0)

    def test_percentiles(self):
        self.assertEqual(baseline.percentile([1, 2, 3, 4, 5], 50), 3)
        self.assertAlmostEqual(baseline.percentile(list(range(1, 1001)), 99.9), 999.001)
        self.assertIsNone(baseline.percentile([], 50))

    def test_frame_csv_skips_bad_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'frames.csv'
            path.write_text('frame,frame_ms,x\n1,16.6,0\n2,abc,0\n3,-1,0\n4,17.0,0\n')
            self.assertEqual(baseline.read_frame_ms(path), [16.6, 17.0])
            path.write_text('frame,other\n1,2\n')
            with self.assertRaises(ValueError):
                baseline.read_frame_ms(path)

    def test_parse_run_counts_compiles_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            csv_path = Path(tmp) / 'frames.csv'
            write_frames(csv_path, [16.7] * 300)
            record = {'run_id': 'base-01', 'run_state': 'ok', 'measurements': {
                'frame_stats': [{'pipeline_compiles': 3, 'compile_ms': 10.0},
                                {'pipeline_compiles': 2, 'compile_ms': 5.5}],
                'pipeline_binary_cache': {'hits': 7, 'compiled': 4}, 'gpu': 'RTX', 'driver': '1.2'}}
            run = baseline.parse_run(record, csv_path, {'start_s': 0})
            self.assertTrue(run['ok'])
            self.assertEqual((run['compiles'], run['compile_ms'], run['binary_compiled']), (5, 15.5, 4))
            failed = baseline.parse_run({**record, 'run_state': 'FAULT x'}, csv_path, {})
            self.assertFalse(failed['ok'])
            missing = baseline.parse_run(record, Path(tmp) / 'nope.csv', {})
            self.assertIsNone(missing['frames'])


def fake_summary(tag, p99s, fails=0):
    runs = [{'ok': True, 'run_id': str(i), 'frames': {'frames': 1}, 'p99': v, 'p50': 16.7, 'fps': 59.9}
            for i, v in enumerate(p99s)]
    runs += [{'ok': False, 'run_id': 'bad', 'state': 'HUNG'}] * fails
    window = {'last_s': 45}
    return {'tag': tag, 'scenarios': {'S2': {'title': 't', 'secs': 1, 'pad': '', 'window': window,
                                              'states': {'warm1': baseline.summarize_state(runs)}}}}


class SummaryTests(unittest.TestCase):
    def test_summarize_state(self):
        runs = [{'ok': True, 'frames': {'frames': 1}, 'p99': v} for v in (20, 30, 25)]
        runs.append({'ok': False, 'run_id': 'bad', 'state': 'HUNG'})
        s = baseline.summarize_state(runs)
        self.assertEqual((s['n'], s['ok']), (4, 3))
        self.assertEqual(s['metrics']['p99']['median'], 25)
        self.assertEqual(s['failures'], [{'run_id': 'bad', 'state': 'HUNG'}])

    def test_dispersion(self):
        d = baseline.dispersion([10, 12, 14])
        self.assertEqual((d['min'], d['median'], d['max'], d['n']), (10, 12, 14, 3))
        self.assertAlmostEqual(d['cv'], 2 / 12)
        self.assertEqual(baseline.dispersion([5])['cv'], 0.0)
        self.assertIsNone(baseline.dispersion([]))

    def test_compare_table(self):
        table = baseline.compare_table(fake_summary('A', [20, 20, 20]), fake_summary('B', [30, 30, 30], fails=1),
                                       'A', 'B')
        self.assertIn('| S2 | warm1 | P99 ms | 20.00 (n=3) | 30.00 (n=3) | +50.0% |', table)
        self.assertIn('failures B', table)

    def test_aa_table(self):
        table = baseline.aa_table(fake_summary('x', [10, 12, 14]))
        self.assertIn('| S2 | warm1 | P99 ms | 3 | 10.00 | 12.00 | 14.00 | 16.7% |', table)


class ChainTests(unittest.TestCase):
    def fake_runner(self, tmp, fail_state=None):
        """Plays batch_runs: writes manifest, frame CSV and a ~profile with cache for a command."""
        calls = []

        def run(cmd):
            calls.append(cmd)
            arg = lambda name: cmd[cmd.index(name) + 1]  # noqa: E731
            state = arg('--scene').rsplit('(', 1)[1].rstrip(')')
            profile = Path(arg('--profile-root')) / 'base' / f'base-01-{state}'
            (profile / 'cache').mkdir(parents=True)
            (profile / 'cache' / 'blob').write_bytes(b'x' * 100)
            data = Path(arg('--data-root')) / f'base-01-{state}'
            write_frames(data / 'frames.csv', [16.7] * 400)
            run_state = 'HUNG' if state == fail_state else 'ok'
            Path(arg('--manifest')).write_text(json.dumps({
                'build': {'exe_sha256': 'ab' * 32, 'git_sha': 'c0ffee'}, 'host': {'platform': 'Win-test'},
                'runs': [{'run_id': f'base-01-{state}', 'run_state': run_state, 'measurements': {
                    'frame_stats': [{'pipeline_compiles': 1, 'compile_ms': 2.0}],
                    'pipeline_binary_cache': {'hits': 1, 'compiled': 1}, 'gpu': 'GPU', 'driver': 'D'}}]}))
            return 0
        return run, calls

    def test_chain_deletes_profiles_and_threads_templates(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            args = synthetic_args(tmp / 'out')
            args.user_template = tmp
            scenario = baseline.load_scenario('S2')
            runner, calls = self.fake_runner(tmp)
            results = baseline.run_chain(scenario, 1, args, runner=runner, log=lambda *_: None)
            self.assertEqual([results[s]['ok'] for s in baseline.STATES], [True] * 3)
            templates = [Path(c[c.index('--user-template') + 1]) for c in calls]
            self.assertEqual(templates[0], tmp)
            self.assertEqual(templates[1].name, 'base-01-cold')
            self.assertEqual(templates[2].name, 'base-01-warm1')
            self.assertEqual([c[c.index('--cache-state') + 1] for c in calls], ['cold', 'warm', 'warm'])
            leftover = [p for p in (tmp / 'out').rglob('profile') if any(p.iterdir())]
            self.assertEqual(leftover, [])
            self.assertTrue(any((tmp / 'out').rglob('frames.csv')))  # run data stays

    def test_failed_state_ends_chain_and_still_cleans(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            args = synthetic_args(tmp / 'out')
            args.user_template = tmp
            runner, calls = self.fake_runner(tmp, fail_state='warm1')
            results = baseline.run_chain(baseline.load_scenario('S1'), 1, args, runner=runner, log=lambda *_: None)
            self.assertTrue(results['cold']['ok'])
            self.assertFalse(results['warm1']['ok'])
            self.assertEqual(len(calls), 2)
            self.assertIn('skipped', results['warm2']['state'])
            self.assertEqual([p for p in (tmp / 'out').rglob('profile') if any(p.iterdir())], [])

    def test_summary_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            args = synthetic_args(tmp / 'out')
            args.user_template = tmp
            scenario = baseline.load_scenario('S2')
            runner, _ = self.fake_runner(tmp)
            per_state, manifests = {}, []
            for repeat in (1, 2):
                chain = baseline.run_chain(scenario, repeat, args, runner=runner, log=lambda *_: None)
                manifests += chain.pop('_manifests')
                for state, run in chain.items():
                    per_state.setdefault('S2', {}).setdefault(state, []).append(run)
            summary = baseline.build_summary('windows-test', args, [scenario], per_state, manifests)
            self.assertEqual(summary['exe_sha256'], 'ab' * 32)
            self.assertEqual(summary['gpu'], 'GPU')
            cold = summary['scenarios']['S2']['states']['cold']
            self.assertEqual((cold['n'], cold['ok']), (2, 2))
            self.assertAlmostEqual(cold['metrics']['p99']['median'], 16.7)
            json.dumps(summary)


class CommandTests(unittest.TestCase):
    def test_scenarios_load(self):
        for sid in ('S1', 'S2', 'S3'):
            scenario = baseline.load_scenario(sid)
            self.assertEqual(scenario['id'], sid)
            self.assertIn('TODO' if sid != 'S2' else 'notes', json.dumps(scenario))
        ini = baseline.BENCH_INI.read_text()
        for line in ('display_mode=windowed', 'vsync=1', 'fps_limit=60', 'upscaler=off', 'output_res=1920x1080'):
            self.assertIn(line, ini)

    def test_dry_run_command_lines(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = baseline.main(['--dry-run', '--scenarios', 'S2', '-n', '1', '--tag', 'windows-t',
                                  '--out-root', 'R', '--user-template', 'U'])
        self.assertEqual(code, 0)
        lines = [l for l in out.getvalue().splitlines() if l.startswith('python')]
        self.assertEqual(len(lines), 3)
        self.assertIn('--cache-state cold', lines[0])
        self.assertIn('--user-template U', lines[0])
        self.assertIn('--cache-state warm', lines[1])
        self.assertIn('<run_id>', lines[1])
        self.assertIn('--secs 125', lines[0])
        self.assertIn('--frame-trace', lines[0])
        self.assertIn('batch_runs.py --runs 1', lines[0])
        self.assertIn('--pad "24:cross:0.5', lines[0])
        self.assertIn('THEORDER_CONFIG=', lines[0])

    def test_aa_and_defaults(self):
        args = baseline.parse_args(['--aa', '5', '--tag', 'windows-v0.9.0'])
        self.assertEqual(args.n, 5)
        self.assertEqual(args.tag, 'windows-v0.9.0-aa')
        self.assertEqual(baseline.parse_args(['--tag', 'x']).n, 3)
        with self.assertRaises(SystemExit):
            baseline.parse_args(['--states', 'warm1,cold'])

    def test_compare_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = Path(tmp) / 'a.json', Path(tmp) / 'b.json'
            a.write_text(json.dumps(fake_summary('A', [20, 21, 22])))
            b.write_text(json.dumps(fake_summary('B', [40, 41, 42])))
            out = io.StringIO()
            with redirect_stdout(out):
                baseline.main(['--compare', str(a), str(b)])
            self.assertIn('+95.2%', out.getvalue())


if __name__ == '__main__':
    unittest.main()
