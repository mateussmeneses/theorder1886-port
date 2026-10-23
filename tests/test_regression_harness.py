import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

import paths


def load_tool(name):
    tools_dir = str(paths.ROOT / 'tools')
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    spec = importlib.util.spec_from_file_location(name, paths.ROOT / 'tools' / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


regression_report = load_tool('regression_report')
fault_stacks = load_tool('fault_stacks')
batch_runs = load_tool('batch_runs')


class RegressionReportTest(unittest.TestCase):
    def test_graphics_pipeline_wait_events_keep_flip_key_and_latency(self):
        result = regression_report.parse_log(
            "Graphics pipeline wait: flip 123 key 0xabc stage0 0xdef stage4 0x123 "
            "spv0 0x456 spv4 0x789 wait 87.500 ms prep 123.000 ms prep_status matched "
            "source visible_direct queue 4.250 ms create 82.000 ms "
            "all_stages [1,2,0,3,0,0] all_spvs [11,22,0,33,0,0] outcome ready\n", 10)
        self.assertEqual(result['graphics_pipeline_waits'], [{
            'flip': 123, 'key': '0000000000000abc', 'stage0': '0000000000000def',
            'stage4': '0000000000000123', 'spv0': '0000000000000456',
            'spv4': '0000000000000789', 'wait_ms': 87.5, 'prep_ms': 123.0,
            'prep_status': 'matched', 'source': 'visible_direct', 'queue_ms': 4.25,
            'create_ms': 82.0, 'stage_hashes': ['0000000000000001', '0000000000000002',
                '0000000000000000', '0000000000000003', '0000000000000000', '0000000000000000'],
            'spv_hashes': ['000000000000000b', '0000000000000016', '0000000000000000',
                '0000000000000021', '0000000000000000', '0000000000000000'],
            'outcome': 'ready'}])
        self.assertEqual(regression_report.parse_log('', 10)['graphics_pipeline_waits'], [])

    def test_graphics_pipeline_wait_event_supports_older_log_format(self):
        result = regression_report.parse_log(
            "Graphics pipeline wait: flip 123 key 0xabc stage0 0xdef stage4 0x123 "
            "spv0 0x456 spv4 0x789 wait 87.500 ms outcome ready\n", 10)
        self.assertEqual(result['graphics_pipeline_waits'][0]['queue_ms'], None)
        self.assertEqual(result['graphics_pipeline_waits'][0]['create_ms'], None)
        self.assertEqual(result['graphics_pipeline_waits'][0]['prep_ms'], None)
        self.assertEqual(result['graphics_pipeline_waits'][0]['prep_status'], None)
        self.assertEqual(result['graphics_pipeline_waits'][0]['source'], None)

    def test_graphics_pipeline_wait_event_marks_missing_preparation_time(self):
        result = regression_report.parse_log(
            "Graphics pipeline wait: flip 123 key 0xabc stage0 0xdef stage4 0x123 "
            "spv0 0x456 spv4 0x789 wait 87.500 ms prep -1.000 ms prep_status key_mismatch "
            "source visible_indirect outcome ready\n", 10)
        self.assertIsNone(result['graphics_pipeline_waits'][0]['prep_ms'])
        self.assertEqual(result['graphics_pipeline_waits'][0]['prep_status'], 'key_mismatch')
        self.assertEqual(result['graphics_pipeline_waits'][0]['source'], 'visible_indirect')

    def test_shader_compilation_wait_events_keep_flip_program_and_stage(self):
        result = regression_report.parse_log(
            "Shader compilation wait: flip 456 shader 0xabc stage 4 permutation 7 "
            "wait 64.250 ms\n", 10)
        self.assertEqual(result['shader_compilation_waits'], [{
            'flip': 456, 'shader': '0000000000000abc', 'stage': 4,
            'permutation': 7, 'wait_ms': 64.25}])
        self.assertEqual(regression_report.parse_log('', 10)['shader_compilation_waits'], [])

    def test_wait_profile_keeps_callsite_and_wait_time(self):
        result = regression_report.parse_log(
            "Wait profile: buffer_cache.cpp:516; 12 calls, CPU submit 2.500 ms, "
            "semaphore wait 450.000 ms, max wait 80.000 ms\n", 10)
        self.assertEqual(result['wait_profiles'], [{
            'site': 'buffer_cache.cpp', 'line': 516, 'calls': 12,
            'submit_ms': 2.5, 'wait_ms': 450.0, 'max_wait_ms': 80.0}])
        report = regression_report.render_markdown({'runs': [{
            'measurements': {'wait_profiles': result['wait_profiles']}}]})
        self.assertIn('calls may overlap', report)
        self.assertIn('buffer_cache.cpp:516 12 calls, 450.0 ms, max 80.0 ms', report)

    def test_markdown_report_summarizes_visible_pipeline_waits(self):
        report = regression_report.render_markdown({'runs': [{
            'condition': 'o3', 'iteration': 1,
            'measurements': {'graphics_pipeline_waits': [
                {'wait_ms': 75.0}, {'wait_ms': 10.0}]}}]})
        self.assertIn('Visible graphics pipeline waits: 2 events, 85.0 ms total', report)
        self.assertIn('1 over 50 ms; per-event keys and SPIR-V hashes are in the manifest.', report)

    def test_compute_reuse_preserves_content_identity_and_permutation_indices(self):
        result = regression_report.parse_log(
            "Compute pipeline reuse: shader 00000000804aeead permutation 361 -> 7 "
            "(SPIR-V fedcba9876543210)\n"
            "Compute pipeline reuse: shader 00000000804aeead permutation 370 -> 7 "
            "(SPIR-V fedcba9876543210)\n", 10)
        self.assertEqual(result['compute_pipeline_reuse_events'], [
            {'shader': '00000000804aeead', 'permutation': 361,
             'pipeline_permutation': 7, 'spirv_hash': 'fedcba9876543210'},
            {'shader': '00000000804aeead', 'permutation': 370,
             'pipeline_permutation': 7, 'spirv_hash': 'fedcba9876543210'}])
        self.assertEqual(regression_report.parse_log('', 10)['compute_pipeline_reuse_events'], [])

    def test_cache_counters_are_cumulative_not_summed(self):
        result = regression_report.parse_log(
            "Pipeline binary cache: 10 hits, 2 compiled, 1 rejected, 2 saved, 0 cache errors\n"
            "Pipeline binary cache: 20 hits, 3 compiled, 1 rejected, 3 saved, 0 cache errors\n", 10)
        self.assertEqual(result['pipeline_binary_cache'],
                         {'hits': 20, 'compiled': 3, 'rejected': 1, 'saved': 3, 'errors': 0})

    def test_raw_frames_have_true_percentiles_and_time_weighted_fps(self):
        result = regression_report.parse_frame_trace(io.StringIO(
            "frame,frame_ms,driver_ns\n2,10,0\n3,10,0\n4,10,0\n5,100,90000000\n"))
        self.assertEqual(result['samples'], 4)
        self.assertAlmostEqual(result['fps_mean'], 4000 / 130)
        self.assertEqual(result['frame_median_ms'], 10)
        self.assertAlmostEqual(result['frame_p95_ms'], 86.5)
        self.assertAlmostEqual(result['frame_p99_ms'], 97.3)
        self.assertEqual(result['stutters'], 1)
        self.assertAlmostEqual(result['stutters_per_minute'], 60000 / 130)
        self.assertEqual(result['worst_frame_ms'], 100)

    def test_raw_frame_errors_are_not_silently_discarded(self):
        for rows in ("2", "2,0", "2,nan", "2,inf", "2,-1", "3,10\n2,10", "2,10\n2,10"):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                regression_report.parse_frame_trace(io.StringIO("frame,frame_ms\n" + rows))
        self.assertIsNone(regression_report.parse_frame_trace(
            io.StringIO("frame,frame_ms\n"))['frame_p99_ms'])

    def test_frame_trace_summarizes_videoout_timing_when_available(self):
        trace = regression_report.parse_frame_trace(io.StringIO(
            "frame,frame_ms,guest_submit_gap_ns,submit_to_queue_ns,videoout_queue_wait_ns\n"
            "2,16,1000000,2000000,3000000\n"
            "3,25,5000000,6000000,7000000\n"))
        self.assertEqual(trace['guest_submit_gap_max_ms'], 5)
        self.assertEqual(trace['submit_to_queue_max_ms'], 6)
        self.assertEqual(trace['videoout_queue_wait_max_ms'], 7)
        self.assertAlmostEqual(trace['guest_submit_gap_p95_ms'], 4.8)

    def test_frame_trace_attributes_longest_gpu_tick_wait_by_site_and_tick(self):
        trace = regression_report.parse_frame_trace(io.StringIO(
            "frame,frame_ms,gpu_tick_wait_file,gpu_tick_wait_line,gpu_tick_wait_target_tick,gpu_tick_wait_max_ns\n"
            "2,100,vk_presenter.cpp,601,18,40000000\n"
            "3,80,vk_scheduler.cpp,443,21,60000000\n"
            "4,33,,,0,0\n"
            "5,100,vk_presenter.cpp,601,23,85000000\n"))
        self.assertEqual(trace['gpu_tick_wait_site_samples'], [
            {'file': 'vk_presenter.cpp', 'line': 601, 'intervals': 2,
             'sampled_wait_ns': 125000000, 'max_wait_ns': 85000000,
             'max_frame': 5, 'max_target_tick': 23},
            {'file': 'vk_scheduler.cpp', 'line': 443, 'intervals': 1,
             'sampled_wait_ns': 60000000, 'max_wait_ns': 60000000,
             'max_frame': 3, 'max_target_tick': 21},
        ])

    def test_process_samples_keep_adapter_totals_separate(self):
        log = (
            "Process metrics: CPU -1.00%; private 11000.00 MiB; working set 4000.00 MiB; "
            "peak working set 4500.00 MiB; Vulkan allocations 3000.00 MiB; "
            "GPU adapter 6000.00 MiB; adapter budget 8000.00 MiB\n"
            "Process metrics: CPU 12.50%; private 11200.00 MiB; working set 4200.00 MiB; "
            "peak working set 4600.00 MiB; Vulkan allocations 3100.00 MiB; "
            "GPU adapter -1.00 MiB; adapter budget -1.00 MiB\n"
        )
        result = regression_report.parse_log(log, 10)
        self.assertEqual(result['summary']['process_committed_mib_max'], 11200)
        self.assertEqual(result['summary']['process_ram_mib_max'], 4200)
        self.assertEqual(result['summary']['renderer_allocated_mib_max'], 3100)
        self.assertEqual(result['summary']['adapter_used_mib_max'], 6000)
        self.assertEqual(result['summary']['process_cpu_percent_median'], 12.5)
        self.assertIsNone(result['process_metrics'][0]['cpu_percent'])
        self.assertIsNone(result['process_metrics'][1]['adapter_used_mib'])
        self.assertIsNone(result['summary']['gpu_dedicated_process_mib_max'])

    def test_gpu_process_memory_is_not_adapter_usage(self):
        log = (
            "Process metrics: CPU 1.00%; private 100.00 MiB; working set 90.00 MiB; "
            "peak working set 95.00 MiB; Vulkan allocations 40.00 MiB; "
            "GPU adapter 6000.00 MiB; adapter budget 8000.00 MiB; "
            "GPU dedicated process 1024.00 MiB; GPU shared process 256.00 MiB\n"
            "Process metrics: CPU 2.00%; private 100.00 MiB; working set 90.00 MiB; "
            "peak working set 95.00 MiB; Vulkan allocations 40.00 MiB; "
            "GPU adapter 6100.00 MiB; adapter budget 8000.00 MiB; "
            "GPU dedicated process -1.00 MiB; GPU shared process 0.00 MiB\n"
        )
        result = regression_report.parse_log(log, 10)
        self.assertEqual(result['summary']['gpu_dedicated_process_mib_max'], 1024)
        self.assertEqual(result['summary']['gpu_shared_process_mib_max'], 256)
        self.assertEqual(result['summary']['adapter_used_mib_max'], 6100)
        self.assertIsNone(result['process_metrics'][1]['gpu_dedicated_process_mib'])
        self.assertEqual(result['process_metrics'][1]['gpu_shared_process_mib'], 0)

    def test_parse_runtime_metrics_and_validation(self):
        log = '''GPU: using NVIDIA GeForce RTX 5060 Laptop GPU
GPU: NVIDIA GeForce RTX 5060 Laptop GPU, 6144 MiB VRAM budget, driver NVIDIA 617.42
Patches: CUSA00035 01.02, eboot.bin sha256 f742eb1793f92772a5c4bdd174e29b08e0b53f68d373a37f4cda730703e558d8
Frame stats: 30.0 FPS, worst frame 45.0 ms (vblank 60 Hz); 2 shader/pipeline compiles, 3.0 ms; async: capture 0.0 ms, workers 0.0 ms, sync wait 0.0 ms, snapshots 0.0 ms, GPU capture wait 0.0 ms, 1 pending draws, 2 fallback dispatches; 5 recorder syncs; 0 read faults/s (0.0 ms), 0 protect calls/s; 0 write faults/s; GPU thread 1.0 us/draw, 10 draws/frame, idle 50.0%; blocked: recorder 1.0%, host copies 0.0% (0/frame), copy threads 0.0%, GPU ticks 0.0%; reduced-size draws 0/frame of 0 in the scene
Frame pacing: median 33.3 ms, stddev 1.5 ms, p99 41.0 ms, 2 frames over 1.5x median
Memory: VRAM 2048 MiB; textures 100 (800 MiB), unused for a minute 5 (10 MiB), of them GPU-written tiled 0 (0 MiB); process 4096 MiB committed, 3000 MiB in RAM
Validation Error: [ VUID-vkCmdDraw-foo-12345 ] invalid use
Fault: access violation at guest offset 0x1234
'''
        result = regression_report.parse_log(log, 40)
        self.assertEqual(result['game']['cusa'], 'CUSA00035')
        self.assertEqual(result['gpu'], 'NVIDIA GeForce RTX 5060 Laptop GPU')
        self.assertEqual(result['validation_vuids'], ['VUID-vkCmdDraw-foo-12345'])
        self.assertEqual(result['summary']['frame_p99_samples'], 1)
        self.assertEqual(result['summary']['vram_used_mib_max'], 2048)
        self.assertEqual(result['summary']['process_ram_mib_max'], 3000)
        self.assertEqual(result['fault_count'], 1)
        self.assertFalse(result['hung'])

    def test_percentile_interpolates_and_empty_is_unknown(self):
        self.assertEqual(regression_report.percentile([10, 20, 30], 50), 20)
        self.assertEqual(regression_report.percentile([10, 20, 30], 95), 29)
        self.assertIsNone(regression_report.percentile([], 99))

    def test_expected_game_identity_does_not_assume_missing_fields_match(self):
        observed = {'cusa': 'CUSA00035', 'version': '01.02'}
        self.assertEqual(regression_report.verify_game_identity(observed, 'CUSA00035', '01.02')['status'], 'match')
        self.assertEqual(regression_report.verify_game_identity(observed, 'CUSA00076')['status'], 'mismatch')
        self.assertEqual(regression_report.verify_game_identity(None, 'CUSA00035')['status'], 'not-collected')

    @unittest.skipUnless(importlib.util.find_spec('PIL'), 'Pillow unavailable')
    def test_masked_similarity_never_auto_approves_port_golden(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            actual = root / 'capture'
            reference = root / 'reference'
            actual.mkdir()
            reference.mkdir()
            Image.new('RGB', (2, 1), (10, 10, 10)).save(actual / 'frame.png')
            Image.new('RGB', (2, 1), (10, 10, 10)).save(reference / 'frame.png')
            Image.new('L', (2, 1), 255).save(root / 'mask.png')
            result = regression_report.compare_captures(
                actual, reference, root / 'mask.png', reference_source='port',
                reference_validated=True, reference_note='old port output', diff_dir=root / 'diffs')
            self.assertEqual(result['reference_status'], 'unverified-reference; human-review-required')
            self.assertEqual(result['frames'][0]['mae_0_1'], 0)
            self.assertEqual(result['frames'][0]['assessment'], 'review-required')
            self.assertTrue(Path(result['frames'][0]['diff_file']).is_file())
            self.assertTrue(Path(result['frames'][0]['preview_file']).is_file())
            self.assertTrue(Path(result['frames'][0]['diff_file']).is_file())
            self.assertTrue(Path(result['frames'][0]['preview_file']).is_file())

    @unittest.skipUnless(importlib.util.find_spec('PIL'), 'Pillow unavailable')
    def test_presenter_raw_frames_compare_by_stem_and_channel_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            actual = root / 'actual'
            reference = root / 'reference'
            actual.mkdir()
            reference.mkdir()
            # Two RGBA pixels, captured by THEORDER_PRESENT_DUMP_TRIGGER.
            (actual / 'f000_present_2x1_rgba.raw').write_bytes(bytes((10, 20, 30, 255)) * 2)
            from PIL import Image
            Image.new('RGB', (2, 1), (10, 20, 30)).save(reference / 'f000_present_2x1.png')
            result = regression_report.compare_captures(
                actual, reference, reference_source='ps4', reference_validated=True,
                reference_note='verified PS4 capture, fixed camera')
            self.assertEqual(result['reference_status'], 'external-reference-identified; human-review-required')
            self.assertEqual(result['frames'][0]['mae_0_1'], 0)

    @unittest.skipUnless(importlib.util.find_spec('PIL'), 'Pillow unavailable')
    def test_untagged_presenter_raw_requires_operator_channel_order(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            actual = root / 'capture'
            reference = root / 'reference'
            actual.mkdir()
            reference.mkdir()
            (actual / 'output_000_1x1.raw').write_bytes(bytes((1, 2, 3, 255)))
            Image.new('RGB', (1, 1), (1, 2, 3)).save(reference / 'output_000_1x1.png')
            unknown = regression_report.compare_captures(actual, reference)
            self.assertIn('unsupported raw channel order', unknown['frames'][0]['error'])
            known = regression_report.compare_captures(actual, reference, raw_format='rgba')
            self.assertEqual(known['frames'][0]['mae_0_1'], 0)

    @unittest.skipUnless(importlib.util.find_spec('PIL'), 'Pillow unavailable')
    def test_temporal_metrics_pair_same_stream_and_consecutive_indices(self):
        with tempfile.TemporaryDirectory() as tmp:
            from PIL import Image

            root = Path(tmp)
            captures = root / 'captures'
            captures.mkdir()
            Image.new('RGBA', (1, 1), (10, 10, 10, 255)).save(captures / 'display_000_1x1_rgba.png')
            Image.new('RGBA', (1, 1), (30, 30, 30, 255)).save(captures / 'output_000_1x1_rgba.png')
            Image.new('RGBA', (1, 1), (10, 10, 10, 255)).save(captures / 'display_001_1x1_rgba.png')
            Image.new('RGBA', (1, 1), (30, 30, 30, 255)).save(captures / 'output_001_1x1_rgba.png')
            result = regression_report.compare_captures(captures)
            self.assertEqual(len(result['temporal']), 2)
            self.assertTrue(all(item['from'].startswith(('display_', 'output_')) for item in result['temporal']))
            self.assertTrue(all(item['to'].split('_')[1] == '001' for item in result['temporal']))

    def test_report_calls_out_missing_data_and_no_automatic_visual_pass(self):
        manifest = {
            'scenario': 'issue-26', 'issue_ids': [26], 'cache_state': 'cold',
            'build': {'git_sha': 'abc', 'exe_sha256': 'def'},
            'host': {'platform': 'Windows'}, 'runs': [{
                'condition': 'base', 'iteration': 1, 'scene': 'rooftop', 'exit_code': 0,
                'timed_out': False, 'measurements': {'summary': {}, 'validation_vuids': []},
                'visual': {'reference_status': 'no-reference', 'frames': [], 'temporal': []},
            }],
        }
        md = regression_report.render_markdown(manifest)
        page = regression_report.render_html(manifest)
        self.assertIn('not automatically approved', md)
        self.assertIn('not collected', md)
        self.assertIn('not an automatic pass', page)

    def test_ab_comparison_reports_directional_signals_only_from_measured_values(self):
        def run(condition, fps, p99):
            return {'condition': condition, 'measurements': {'summary': {
                'fps_median': fps, 'window_p99_of_frame_p99_ms': p99,
            }}}

        deltas = regression_report.compare_conditions({'runs': [
            run('baseline', 30, 40), run('candidate', 28, 45),
        ]})
        self.assertEqual(len(deltas), 1)
        self.assertEqual(deltas[0]['metrics']['fps_median']['delta'], -2)
        self.assertEqual(deltas[0]['metrics']['window_p99_of_frame_p99_ms']['delta'], 5)
        self.assertEqual({item['metric'] for item in deltas[0]['signals']},
                         {'fps_median', 'window_p99_of_frame_p99_ms'})
        self.assertEqual(regression_report.compare_conditions({'runs': [run('only', 30, 40)]}), [])

    def test_reports_without_captures_preserve_peak_across_runs(self):
        manifest = {'runs': [
            {'condition': 'base', 'visual': None,
             'measurements': {'summary': {'vram_used_mib_max': peak}}}
            for peak in (2000, 2000, 6000)
        ]}
        self.assertIn('6000', regression_report.render_markdown(manifest))
        self.assertIn('none captured', regression_report.render_html(manifest))

    @unittest.skipUnless(importlib.util.find_spec('PIL'), 'Pillow unavailable')
    def test_empty_mask_and_ambiguous_reference_are_not_comparisons(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            captures = root / 'captures'
            captures.mkdir()
            for index in range(2):
                Image.new('RGB', (1, 1)).save(captures / f'display_{index:03d}_1x1.png')
            Image.new('L', (1, 1), 0).save(root / 'mask.png')
            refs = root / 'refs'
            for name in ('scene-a', 'scene-b'):
                (refs / name).mkdir(parents=True)
                Image.new('RGB', (1, 1)).save(refs / name / 'display_000_1x1.png')
            result = regression_report.compare_captures(
                captures, refs, root / 'mask.png', reference_source='ps4',
                reference_validated=True, reference_note='verified')
            self.assertEqual(result['reference_status'], 'no-comparable-reference')
            self.assertEqual(result['frames'][0]['error'], 'ambiguous reference frame name')
            self.assertEqual(result['temporal'], [])

    def test_fault_stack_cli_writes_json_without_symbolizer(self):
        import subprocess

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / 'fault.log'
            output = root / 'faults.json'
            log.write_text('Flip #9 index=0\nFault: guest read at offset 0xabc\n', encoding='utf-8')
            subprocess.run([sys.executable, str(paths.ROOT / 'tools' / 'fault_stacks.py'),
                            '--no-symbols', '--json', str(output), str(log)],
                           check=True, capture_output=True, text=True)
            results = json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual(len(results), 1)
            self.assertEqual(results[0]['flip'], 9)

    def test_cold_profile_copies_saves_but_not_port_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            template = root / 'template'
            (template / 'savedata' / 'slot').mkdir(parents=True)
            (template / 'cache').mkdir()
            (template / 'shader').mkdir()
            (template / 'savedata' / 'slot' / 'save.bin').write_bytes(b'save')
            (template / 'cache' / 'pipeline.bin').write_bytes(b'pipeline')
            (template / 'shader' / 'shader.bin').write_bytes(b'shader')
            profile, reused = batch_runs._profile_for_run(
                root / 'profiles', template, 'cold', 'base', 1, 'base-01')
            self.assertFalse(reused)
            self.assertTrue((profile / 'savedata' / 'slot' / 'save.bin').is_file())
            self.assertFalse((profile / 'cache').exists())
            self.assertFalse((profile / 'shader').exists())
            self.assertTrue((template / 'cache' / 'pipeline.bin').is_file())
            warm_profile, warm_reused = batch_runs._profile_for_run(
                root / 'warm-profiles', template, 'warm', 'base', 1, 'base-01')
            self.assertFalse(warm_reused)
            self.assertTrue((warm_profile / 'savedata' / 'slot' / 'save.bin').is_file())
            self.assertTrue((warm_profile / 'cache' / 'pipeline.bin').is_file())
            self.assertTrue((warm_profile / 'shader' / 'shader.bin').is_file())
            self.assertTrue((template / 'savedata' / 'slot' / 'save.bin').is_file())
            (warm_profile / 'savedata' / 'slot' / 'save.bin').write_bytes(b'autosave')
            next_profile, _ = batch_runs._profile_for_run(
                root / 'warm-profiles', template, 'warm', 'base', 2, 'base-02')
            self.assertEqual((next_profile / 'savedata' / 'slot' / 'save.bin').read_bytes(), b'save')
            with self.assertRaises(FileExistsError):
                batch_runs._profile_for_run(
                    root / 'warm-profiles', template, 'warm', 'base', 2, 'base-02')

    def test_warm_cache_claim_requires_existing_cache_or_template(self):
        with tempfile.TemporaryDirectory() as tmp:
            profile_root = Path(tmp)
            with self.assertRaises(ValueError):
                batch_runs._profile_for_run(profile_root, None, 'warm', 'base', 1, 'base-01')
            template = profile_root / 'empty-template'
            (template / 'cache').mkdir(parents=True)
            with self.assertRaises(ValueError):
                batch_runs._profile_for_run(profile_root / 'other', template, 'warm',
                                            'base', 1, 'base-01')

    def test_legacy_summary_keeps_existing_keys_and_adds_metrics(self):
        result = batch_runs.summarize('Flip #8 index=0\n', 20)
        self.assertEqual(result['flip'], 8)
        self.assertIn('fault', result)
        self.assertIn('compile_s', result)
        self.assertIn('metrics', result)

    def test_manifest_settings_drop_secrets_and_absolute_trigger_paths(self):
        settings = batch_runs._safe_settings({
            'THEORDER_FRAME_STATS': '1', 'THEORDER_PRESENT_DUMP_TRIGGER': 'C:/private/trigger',
            'THEORDER_UPLOAD_TOKEN': 'should-not-appear', 'THEORDER_FPS_LIMIT': '60',
        })
        self.assertEqual(settings, {'THEORDER_FRAME_STATS': '1', 'THEORDER_FPS_LIMIT': '60'})

    def test_run_ini_settings_preserve_comments_and_unrelated_keys(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'theorder.ini'
            config.write_text('; keep me\naspect_ratio=original\nengine_fps=60\nengine_fps=60\n', encoding='utf-8')
            batch_runs._apply_ini_settings(config, {'aspect_ratio': '16:9', 'engine_fps': '120'})
            self.assertEqual(config.read_text(encoding='utf-8'),
                             '; keep me\naspect_ratio=16:9\nengine_fps=120\nengine_fps=120\n')

    def test_game_identity_does_not_treat_missing_ids_as_match(self):
        result = batch_runs.regression_report.verify_game_identity({}, 'CUSA00035', '01.02')
        self.assertEqual(result['status'], 'not-collected')
        self.assertTrue(all(check['status'] == 'not-collected' for check in result['checks']))

    def test_run_environment_overrides_external_gpu_profile_and_normalizes_graphics(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'run.ini'
            config.write_text('async_shaders=0\n', encoding='utf-8')
            env = {'THEORDER_GPU_USER_DIR': 'original', 'THEORDER_ASYNC_SHADERS': '1'}
            effective = batch_runs._configure_run_environment(env, Path(tmp) / 'profile', config)
            self.assertEqual(env['THEORDER_GPU_USER_DIR'], str(Path(tmp) / 'profile'))
            self.assertEqual(env['THEORDER_ASYNC_SHADERS'], '0')
            self.assertEqual(effective['THEORDER_ASYNC_SHADERS'], '0')

    def test_fault_symbolization_uses_selected_executable(self):
        from unittest.mock import patch

        with patch.object(fault_stacks, 'symbolize', return_value={}) as symbolize:
            fault_stacks.inspect_log('Fault: test\n', exe=Path('selected.exe'))
            symbolize.assert_called_once_with([], Path('selected.exe'))

    @unittest.skipUnless(importlib.util.find_spec('PIL'), 'Pillow unavailable')
    def test_legacy_converter_accepts_tagged_and_legacy_raw_frames(self):
        from unittest.mock import patch
        from PIL import Image

        converter = load_tool('convert_dumps')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            files = []
            for suffix, pixel in (('_rgba', (10, 20, 30, 255)),
                                  ('_bgra', (30, 20, 10, 255)),
                                  ('', (30, 20, 10, 255))):
                raw = root / f'frame_1x1{suffix}.raw'
                raw.write_bytes(bytes(pixel))
                files.append(str(raw))
            with patch.object(converter.os.path, 'exists', return_value=True), \
                    patch.object(converter.glob, 'glob', return_value=files):
                converter.main()
            for raw in files:
                with Image.open(Path(raw).with_suffix('.png')) as image:
                    self.assertEqual(image.getpixel((0, 0)), (10, 20, 30, 255))


if __name__ == '__main__':
    unittest.main()
