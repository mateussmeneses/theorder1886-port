"""Parse batch-run evidence and produce conservative visual/performance reports."""
import hashlib
import csv
import html
import json
import math
import os
import re
import statistics
from pathlib import Path


FRAME_STATS = re.compile(
    r'Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms .*?; '
    r'(\d+) shader/pipeline compiles, ([\d.]+) ms;.*?'
    r'(\d+) pending draws, (\d+) fallback dispatches;'
)
FRAME_PACING = re.compile(
    r'Frame pacing: median ([\d.]+) ms, stddev ([\d.]+) ms, p99 ([\d.]+) ms, '
    r'(\d+) frames over 1\.5x median'
)
MEMORY = re.compile(
    r'Memory: (?:VRAM|GPU adapter) (\d+) MiB; textures (\d+) \((\d+) MiB\), .*?'
    r'process (\d+) MiB committed, (\d+) MiB in RAM'
)
PROCESS_METRICS = re.compile(
    r'Process metrics: CPU (-?[\d.]+)%; private (-?[\d.]+) MiB; working set (-?[\d.]+) MiB; '
    r'peak working set (-?[\d.]+) MiB; Vulkan allocations (-?[\d.]+) MiB; '
    r'GPU adapter (-?[\d.]+) MiB; adapter budget (-?[\d.]+) MiB'
    r'(?:; GPU dedicated process (-?[\d.]+) MiB; GPU shared process (-?[\d.]+) MiB)?'
)
PIPELINE_CACHE = re.compile(
    r'Pipeline binary cache: (\d+) hits, (\d+) compiled, (\d+) rejected, (\d+) saved, '
    r'(\d+) cache errors'
)
COMPUTE_REUSE = re.compile(
    r'Compute pipeline reuse: shader ([0-9a-f]{16}) permutation (\d+) -> (\d+) '
    r'\(SPIR-V ([0-9a-f]{16})\)'
)
GRAPHICS_PIPELINE_WAIT = re.compile(
    r'Graphics pipeline wait: flip (\d+) key 0x([0-9a-f]{1,16}) '
    r'stage0 0x([0-9a-f]{1,16}) stage4 0x([0-9a-f]{1,16}) '
    r'spv0 0x([0-9a-f]{1,16}) spv4 0x([0-9a-f]{1,16}) '
    r'wait ([\d.]+) ms(?: prep (-?[\d.]+) ms(?: prep_status (\w+))?)?'
    r'(?: source (\w+))?'
    r'(?: queue ([\d.]+) ms create ([\d.]+) ms)? '
    r'(?:all_stages \[([\d,]+)\] all_spvs \[([\d,]+)\] )?'
    r'outcome (ready|published|failed)'
)
SHADER_COMPILATION_WAIT = re.compile(
    r'Shader compilation wait: flip (\d+) shader 0x([0-9a-f]{1,16}) '
    r'stage (\d+) permutation (\d+) wait ([\d.]+) ms'
)
WAIT_PROFILE = re.compile(
    r'Wait profile: ([^:]+):(\d+); (\d+) calls, CPU submit ([\d.]+) ms, '
    r'semaphore wait ([\d.]+) ms, max wait ([\d.]+) ms'
)
GAME_IDENTITY = re.compile(
    r'Patches: (CUSA\d+) (\d{2}\.\d{2}), eboot\.bin sha256 ([0-9a-fA-F]{64})'
)
GPU_LINE = re.compile(r'^GPU: using (.+)$', re.MULTILINE)
DRIVER_LINE = re.compile(r'^GPU: .*?, ([\d,]+) MiB VRAM budget, driver (.+)$', re.MULTILINE)
VUID = re.compile(r'VUID-[A-Za-z0-9_.-]+')
FAULT = re.compile(r'^Fault: (.*)$', re.MULTILINE)
RAW_CAPTURE = re.compile(r'_(\d+)x(\d+)(?:_([A-Za-z0-9]+))?\.raw$', re.IGNORECASE)
ROOT = Path(__file__).resolve().parent.parent


def percentile(values, percent):
    """Linear-interpolated percentile; returns None for an empty sample."""
    ordered = sorted(float(value) for value in values if value is not None and math.isfinite(float(value)))
    if not ordered:
        return None
    position = (len(ordered) - 1) * min(100.0, max(0.0, float(percent))) / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def verify_game_identity(observed, expected_cusa=None, expected_version=None):
    """Compare expected game identifiers without treating absent log fields as a match."""
    observed = observed or {}
    checks = []
    for field, expected in (('cusa', expected_cusa), ('version', expected_version)):
        if expected:
            actual = observed.get(field)
            checks.append({'field': field, 'expected': expected, 'observed': actual,
                           'status': 'not-collected' if actual is None else
                           'match' if str(actual).casefold() == str(expected).casefold() else 'mismatch'})
    states = {check['status'] for check in checks}
    status = ('not-specified' if not checks else 'mismatch' if 'mismatch' in states else
              'not-collected' if 'not-collected' in states else 'match')
    return {'status': status, 'checks': checks}


def parse_frame_trace(stream):
    """Summarize actual guest flip intervals, not percentiles of window percentiles."""
    reader = csv.DictReader(stream)
    if not reader.fieldnames or not {'frame', 'frame_ms'}.issubset(reader.fieldnames):
        raise ValueError('frame trace requires frame and frame_ms columns')
    intervals = []
    timing_columns = {
        'guest_submit_gap_ns': 'guest_submit_gap',
        'submit_to_queue_ns': 'submit_to_queue',
        'videoout_queue_wait_ns': 'videoout_queue_wait',
    }
    timings = {name: [] for column, name in timing_columns.items()
               if column in (reader.fieldnames or [])}
    gpu_wait_sites = {}
    has_gpu_wait_event = 'gpu_tick_wait_max_ns' in (reader.fieldnames or [])
    previous = 0
    for row in reader:
        try:
            frame, ms = int(row['frame']), float(row['frame_ms'])
        except (TypeError, ValueError) as error:
            raise ValueError(f'invalid frame trace row {reader.line_num}') from error
        if frame <= previous or not math.isfinite(ms) or ms <= 0:
            raise ValueError(f'invalid frame trace row {reader.line_num}')
        previous = frame
        intervals.append(ms)
        for column, name in timing_columns.items():
            if name in timings:
                try:
                    value = int(row[column])
                except (TypeError, ValueError) as error:
                    raise ValueError(f'invalid frame trace row {reader.line_num}') from error
                if value < 0:
                    raise ValueError(f'invalid frame trace row {reader.line_num}')
                timings[name].append(value / 1e6)
        if has_gpu_wait_event:
            try:
                wait_ns = int(row.get('gpu_tick_wait_max_ns') or 0)
                wait_line = int(row.get('gpu_tick_wait_line') or 0)
                target_tick = int(row.get('gpu_tick_wait_target_tick') or 0)
            except (TypeError, ValueError) as error:
                raise ValueError(f'invalid GPU wait trace row {reader.line_num}') from error
            wait_file = row.get('gpu_tick_wait_file') or ''
            if wait_ns < 0 or wait_line < 0 or target_tick < 0:
                raise ValueError(f'invalid GPU wait trace row {reader.line_num}')
            if wait_ns:
                if not wait_file or not wait_line:
                    raise ValueError(f'incomplete GPU wait trace row {reader.line_num}')
                key = (wait_file, wait_line)
                site = gpu_wait_sites.setdefault(key, {
                    'file': wait_file, 'line': wait_line, 'intervals': 0,
                    'sampled_wait_ns': 0, 'max_wait_ns': 0,
                    'max_frame': None, 'max_target_tick': None,
                })
                site['intervals'] += 1
                site['sampled_wait_ns'] += wait_ns
                if wait_ns > site['max_wait_ns']:
                    site['max_wait_ns'] = wait_ns
                    site['max_frame'] = frame
                    site['max_target_tick'] = target_tick
    duration_ms = sum(intervals)
    median = percentile(intervals, 50)
    stutters = sum(ms > 1.5 * median for ms in intervals) if intervals else 0
    result = {
        'samples': len(intervals),
        'duration_seconds': duration_ms / 1000,
        'fps_mean': len(intervals) * 1000 / duration_ms if duration_ms else None,
        'fps_median': 1000 / median if median else None,
        'frame_median_ms': median,
        'frame_p95_ms': percentile(intervals, 95),
        'frame_p99_ms': percentile(intervals, 99),
        'worst_frame_ms': max(intervals, default=None),
        'stutters': stutters,
        'stutters_per_minute': stutters * 60000 / duration_ms if duration_ms else None,
        'stutter_definition': 'frame_ms > 1.5 * whole-trace median',
    }
    for name, values in timings.items():
        result[f'{name}_p95_ms'] = percentile(values, 95)
        result[f'{name}_max_ms'] = max(values, default=None)
    result['gpu_tick_wait_site_samples'] = sorted(
        gpu_wait_sites.values(), key=lambda site: (-site['max_wait_ns'], site['file'], site['line']))
    return result


def parse_log(text, seconds):
    """Extract only measurements actually printed by the current runtime."""
    frame_stats = []
    for match in FRAME_STATS.finditer(text):
        frame_stats.append({
            'fps': float(match.group(1)),
            'worst_frame_ms': float(match.group(2)),
            'pipeline_compiles': int(match.group(3)),
            'compile_ms': float(match.group(4)),
            'pending_draws': int(match.group(5)),
            'fallback_dispatches': int(match.group(6)),
        })
    pacing = [
        {
            'median_ms': float(match.group(1)),
            'stddev_ms': float(match.group(2)),
            'p99_ms': float(match.group(3)),
            'spikes': int(match.group(4)),
        }
        for match in FRAME_PACING.finditer(text)
    ]
    memory = [
        {
            'vram_used_mib': int(match.group(1)),
            'textures': int(match.group(2)),
            'texture_mib': int(match.group(3)),
            'process_committed_mib': int(match.group(4)),
            'process_ram_mib': int(match.group(5)),
        }
        for match in MEMORY.finditer(text)
    ]
    process_fields = ('cpu_percent', 'private_mib', 'working_set_mib', 'peak_working_set_mib',
                      'renderer_allocated_mib', 'adapter_used_mib', 'adapter_budget_mib',
                      'gpu_dedicated_process_mib', 'gpu_shared_process_mib')
    process_metrics = [
        {key: float(value) if value is not None and float(value) >= 0 else None
         for key, value in zip(process_fields, match.groups())}
        for match in PROCESS_METRICS.finditer(text)
    ]
    pipeline_cache = [
        dict(zip(('hits', 'compiled', 'rejected', 'saved', 'errors'), map(int, match.groups())))
        for match in PIPELINE_CACHE.finditer(text)
    ]
    def maximum(field, legacy=None):
        values = [item[field] for item in process_metrics if item[field] is not None]
        if legacy:
            values.extend(item[legacy] for item in memory)
        return max(values, default=None)
    identity = GAME_IDENTITY.search(text)
    gpu = GPU_LINE.search(text)
    driver = DRIVER_LINE.search(text)
    faults = FAULT.findall(text)
    vuids = sorted(set(VUID.findall(text)))
    validation_lines = [line.strip()[:500] for line in text.splitlines()
                        if VUID.search(line) or re.search(r'Validation (?:Error|Warning)', line, re.IGNORECASE)]
    flips = re.findall(r'Flip #(\d+)', text)
    stall_ms = [float(value) for value in re.findall(r'^Stall: ([\d.]+) ms frame', text, re.MULTILINE)]
    windows = len(frame_stats)
    fault = faults[0] if faults else ''
    return {
        'flip': int(flips[-1]) if flips else 0,
        'fault': fault,
        'fault_count': len(faults),
        'hung': not fault and windows < max(1, seconds // 10),
        'frame_stats': frame_stats,
        'frame_pacing': pacing,
        'memory_samples': memory,
        'process_metrics': process_metrics,
        'pipeline_binary_cache': pipeline_cache[-1] if pipeline_cache else None,
        'compute_pipeline_reuse_events': [
            {'shader': shader, 'permutation': int(index), 'pipeline_permutation': int(canonical),
             'spirv_hash': spirv}
            for shader, index, canonical, spirv in COMPUTE_REUSE.findall(text)
        ],
        'graphics_pipeline_waits': [
            {'flip': int(flip), 'key': key.zfill(16), 'stage0': stage0.zfill(16),
             'stage4': stage4.zfill(16), 'spv0': spv0.zfill(16), 'spv4': spv4.zfill(16),
             'wait_ms': float(wait_ms), 'prep_ms': float(prep_ms) if prep_ms and float(prep_ms) >= 0 else None,
             'prep_status': prep_status or None,
             'source': source or None,
             'queue_ms': float(queue_ms) if queue_ms else None,
             'create_ms': float(create_ms) if create_ms else None,
             'stage_hashes': [f'{int(value):016x}' for value in stage_hashes.split(',')]
             if stage_hashes else None,
             'spv_hashes': [f'{int(value):016x}' for value in spv_hashes.split(',')]
             if spv_hashes else None, 'outcome': outcome}
            for flip, key, stage0, stage4, spv0, spv4, wait_ms, prep_ms, prep_status, source, queue_ms, create_ms,
                stage_hashes, spv_hashes, outcome
            in GRAPHICS_PIPELINE_WAIT.findall(text)
        ],
        'shader_compilation_waits': [
            {'flip': int(flip), 'shader': shader.zfill(16), 'stage': int(stage),
             'permutation': int(permutation), 'wait_ms': float(wait_ms)}
            for flip, shader, stage, permutation, wait_ms in SHADER_COMPILATION_WAIT.findall(text)
        ],
        'wait_profiles': [
            {'site': site, 'line': int(line), 'calls': int(calls),
             'submit_ms': float(submit), 'wait_ms': float(wait), 'max_wait_ms': float(maximum)}
            for site, line, calls, submit, wait, maximum in WAIT_PROFILE.findall(text)
        ],
        'stall_frame_ms': stall_ms,
        'validation_vuids': vuids,
        'validation_messages': validation_lines,
        'gpu': gpu.group(1).strip() if gpu else None,
        'driver': driver.group(2).strip() if driver else None,
        'vram_budget_mib': int(driver.group(1).replace(',', '')) if driver else None,
        'game': ({'cusa': identity.group(1), 'version': identity.group(2),
                  'eboot_sha256': identity.group(3).lower()} if identity else None),
        'compile_s': sum(item['compile_ms'] for item in frame_stats) / 1000.0,
        'windows': windows,
        'summary': {
            'fps_median': percentile([item['fps'] for item in frame_stats], 50),
            'worst_frame_ms_max': max((item['worst_frame_ms'] for item in frame_stats), default=None),
            'window_p95_of_frame_p99_ms': percentile([item['p99_ms'] for item in pacing], 95),
            'window_p99_of_frame_p99_ms': percentile([item['p99_ms'] for item in pacing], 99),
            'frame_p99_samples': len(pacing),
            # Legacy key retained for older manifest consumers; this is adapter-wide usage.
            'vram_used_mib_max': maximum('adapter_used_mib', 'vram_used_mib'),
            'adapter_used_mib_max': maximum('adapter_used_mib', 'vram_used_mib'),
            'renderer_allocated_mib_max': maximum('renderer_allocated_mib'),
            'gpu_dedicated_process_mib_max': maximum('gpu_dedicated_process_mib'),
            'gpu_shared_process_mib_max': maximum('gpu_shared_process_mib'),
            'process_committed_mib_max': maximum('private_mib', 'process_committed_mib'),
            'process_ram_mib_max': maximum('working_set_mib', 'process_ram_mib'),
            'process_cpu_percent_median': percentile(
                [item['cpu_percent'] for item in process_metrics], 50),
        },
    }


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _image_files(directory):
    if not directory or not Path(directory).is_dir():
        return []
    allowed = {'.png', '.bmp', '.jpg', '.jpeg', '.webp'}
    return sorted(path for path in Path(directory).rglob('*')
                  if path.suffix.lower() in allowed
                  or (path.suffix.lower() == '.raw' and RAW_CAPTURE.search(path.name)))


def _load_capture(path, raw_format=None):
    from PIL import Image

    path = Path(path)
    if path.suffix.lower() != '.raw':
        with Image.open(path) as source:
            return source.convert('RGB')
    match = RAW_CAPTURE.search(path.name)
    if not match:
        raise ValueError('raw filename does not include width, height, and channel order')
    width, height = int(match.group(1)), int(match.group(2))
    channel_order = (match.group(3) or raw_format or '').lower()
    if channel_order not in {'rgba', 'bgra'}:
        raise ValueError(f'unsupported raw channel order: {channel_order}')
    data = path.read_bytes()
    if len(data) != width * height * 4:
        raise ValueError('raw byte count does not match filename dimensions')
    decoder = 'RGBA' if channel_order == 'rgba' else 'BGRA'
    return Image.frombytes('RGBA', (width, height), data, 'raw', decoder).convert('RGB')


def _frame_key(path):
    match = RAW_CAPTURE.search(Path(path).name)
    return Path(path).stem.rsplit('_', 1)[0] if match and match.group(3) else Path(path).stem


def _temporal_stream(path):
    name = Path(path).stem
    raw_match = RAW_CAPTURE.search(Path(path).name)
    if (raw_match and raw_match.group(3)) or re.search(r'_(?:rgba|bgra)$', name, re.IGNORECASE):
        name = name.rsplit('_', 1)[0]
    match = re.match(r'^f(\d+)_(.+)$', name)
    if match:
        return match.group(2), int(match.group(1))
    match = re.match(r'^(.+?)_(\d+)_(\d+x\d+)$', name)
    if match:
        return f'{match.group(1)}_{match.group(3)}', int(match.group(2))
    match = re.match(r'^(.+?)[_-](\d+)$', name)
    if match:
        return match.group(1), int(match.group(2))
    return None


def _masked_pair_metrics(actual_path, reference_path, mask_path, tolerance, diff_path=None,
                         raw_format=None):
    from PIL import Image, ImageChops, ImageStat

    try:
        actual = _load_capture(actual_path, raw_format)
        reference = _load_capture(reference_path, raw_format)
    except (OSError, ValueError) as error:
        return {'error': str(error)}
    if actual.size != reference.size:
        return {'error': 'dimension mismatch', 'actual_size': list(actual.size),
                'reference_size': list(reference.size)}
    include = Image.new('L', actual.size, 255)
    if mask_path:
        with Image.open(mask_path) as source:
            mask = source.convert('L')
        if mask.size != actual.size:
            return {'error': 'mask dimension mismatch', 'actual_size': list(actual.size),
                    'mask_size': list(mask.size)}
        include = mask
    included_pixels = sum(include.histogram()[1:])
    if not included_pixels:
        return {'error': 'mask excludes every pixel', 'included_pixels': 0}
    difference = ImageChops.difference(actual, reference)
    if diff_path:
        visible = difference.point(lambda value: min(255, value * 4))
        visible = ImageChops.multiply(visible, Image.merge('RGB', (include, include, include)))
        Path(diff_path).parent.mkdir(parents=True, exist_ok=True)
        visible.save(diff_path)
    stats = ImageStat.Stat(difference, mask=include)
    mae = sum(stats.mean) / (len(stats.mean) * 255.0)
    rms = math.sqrt(sum(value * value for value in stats.rms) / len(stats.rms))
    changed = difference.convert('L').point(lambda value: 255 if value > tolerance else 0)
    changed = ImageChops.multiply(changed, include)
    changed_pixels = sum(changed.histogram()[1:])
    psnr = float('inf') if rms == 0 else 20.0 * math.log10(255.0 / rms)
    return {
        'actual': Path(actual_path).name,
        'reference': Path(reference_path).name,
        'actual_sha256': sha256_file(actual_path),
        'reference_sha256': sha256_file(reference_path),
        'width': actual.width,
        'height': actual.height,
        'included_pixels': included_pixels,
        'mae_0_1': mae,
        'psnr_db': psnr,
        'changed_fraction': changed_pixels / included_pixels,
        'tolerance_perceived_luma': tolerance,
        'assessment': 'review-required',
        'diff_file': str(diff_path) if diff_path else None,
    }


def compare_captures(capture_dir, reference_dir=None, mask_path=None, tolerance=8,
                     reference_source='unknown', reference_validated=False, reference_note='',
                     diff_dir=None, raw_format=None):
    """Compare only same-named frames; never grades a port capture as a golden reference."""
    try:
        from PIL import Image, ImageChops, ImageStat  # noqa: F401
    except ImportError:
        return {'available': False, 'error': 'Pillow is not installed', 'frames': [], 'temporal': []}

    captures = _image_files(capture_dir)
    videos = sorted(path for path in Path(capture_dir).rglob('*')
                    if path.suffix.lower() in {'.mp4', '.webm', '.mkv', '.avi'}) if capture_dir and Path(capture_dir).is_dir() else []
    refs = {}
    ambiguous_refs = set()
    for path in _image_files(reference_dir):
        key = _frame_key(path)
        if key in refs:
            ambiguous_refs.add(key)
        refs[key] = path
    frames = []
    previews = []
    diff_root = Path(diff_dir) if diff_dir else None
    for path in captures:
        reference = refs.get(_frame_key(path))
        metric = {'actual': path.name, 'actual_sha256': sha256_file(path)}
        if _frame_key(path) in ambiguous_refs:
            metric['error'] = 'ambiguous reference frame name'
        elif reference:
            diff_path = diff_root / f'{path.stem}_vs_{reference.stem}.diff.png' if diff_root else None
            metric.update(_masked_pair_metrics(path, reference, mask_path, tolerance, diff_path,
                                               raw_format))
        if diff_root:
            try:
                preview_path = diff_root / f'{path.stem}.preview.png'
                preview_path.parent.mkdir(parents=True, exist_ok=True)
                _load_capture(path, raw_format).save(preview_path)
                metric['preview_file'] = str(preview_path)
                previews.append(str(preview_path))
            except (OSError, ValueError) as error:
                metric['preview_error'] = str(error)
        frames.append(metric)

    temporal = []
    from PIL import Image, ImageChops, ImageStat
    streams = {}
    for path in captures:
        if sequence := _temporal_stream(path):
            streams.setdefault(sequence[0], []).append((sequence[1], path))
    adjacent_pairs = []
    for stream_frames in streams.values():
        stream_frames.sort(key=lambda entry: entry[0])
        adjacent_pairs.extend((first[1], second[1])
                              for first, second in zip(stream_frames, stream_frames[1:])
                              if second[0] == first[0] + 1)
    for first, second in adjacent_pairs:
        try:
            before = _load_capture(first, raw_format)
            after = _load_capture(second, raw_format)
        except (OSError, ValueError):
            continue
        if before.size != after.size:
            continue
        include = Image.new('L', before.size, 255)
        if mask_path:
            with Image.open(mask_path) as source:
                include = source.convert('L')
            if include.size != before.size:
                continue
        if not include.getbbox():
            continue
        stats = ImageStat.Stat(ImageChops.difference(before, after), mask=include)
        temporal_result = {'from': first.name, 'to': second.name,
                           'mean_delta_0_1': sum(stats.mean) / (len(stats.mean) * 255.0)}
        if diff_root:
            temporal_path = diff_root / f'temporal_{first.stem}_to_{second.stem}.png'
            temporal_path.parent.mkdir(parents=True, exist_ok=True)
            visual = ImageChops.difference(before, after).point(lambda value: min(255, value * 4))
            visual = ImageChops.multiply(visual, Image.merge('RGB', (include, include, include)))
            visual.save(temporal_path)
            temporal_result['diff_file'] = str(temporal_path)
        temporal.append(temporal_result)

    external_reference = reference_source in {'ps4', 'shadps4'}
    trusted = bool(external_reference and reference_validated and reference_note.strip())
    if not reference_dir:
        status = 'no-reference'
    elif not any(frame.get('mae_0_1') is not None for frame in frames):
        status = 'no-comparable-reference'
    elif trusted:
        status = 'external-reference-identified; human-review-required'
    else:
        status = 'unverified-reference; human-review-required'
    return {
        'available': True,
        'capture_dir': str(capture_dir) if capture_dir else None,
        'capture_files': [str(path) for path in captures],
        'video_files': [str(path) for path in videos],
        'preview_files': previews,
        'reference_dir': str(reference_dir) if reference_dir else None,
        'reference_source': reference_source,
        'reference_validated_by_user': bool(reference_validated),
        'reference_note': reference_note,
        'reference_status': status,
        'mask': str(mask_path) if mask_path else None,
        'tolerance': int(tolerance),
        'frames': frames,
        'temporal': temporal,
        'temporal_metric_note': 'Frame-to-frame delta includes motion and temporal effects; it is not a standalone flicker verdict.',
    }


def _condition_summary(runs):
    fields = ('fps_median', 'worst_frame_ms_max', 'window_p95_of_frame_p99_ms',
              'window_p99_of_frame_p99_ms', 'vram_used_mib_max',
              'process_committed_mib_max', 'process_ram_mib_max')
    result = {}
    for field in fields:
        values = [run.get('measurements', {}).get('summary', {}).get(field) for run in runs]
        values = [value for value in values if value is not None]
        result[field] = {
            'samples': len(values),
            'median': statistics.median(values) if values else None,
            'max': max(values) if values else None,
            'p95': percentile(values, 95),
            'p99': percentile(values, 99),
        }
    return result


def compare_conditions(manifest):
    """Return measured deltas from the first condition; these are review signals, not verdicts."""
    runs = manifest.get('runs', [])
    labels = list(dict.fromkeys(run.get('condition', 'run') for run in runs))
    if len(labels) < 2:
        return []
    grouped = {label: [run for run in runs if run.get('condition') == label] for label in labels}
    baseline = labels[0]
    baseline_summary = _condition_summary(grouped[baseline])
    deltas = []
    for label in labels[1:]:
        candidate = _condition_summary(grouped[label])
        row = {'baseline': baseline, 'condition': label, 'metrics': {}, 'signals': []}
        for field, direction in (('fps_median', 'lower-worse'),
                                 ('window_p95_of_frame_p99_ms', 'higher-worse'),
                                 ('window_p99_of_frame_p99_ms', 'higher-worse'),
                                 ('worst_frame_ms_max', 'higher-worse'),
                                 ('vram_used_mib_max', 'higher-worse'),
                                 ('process_committed_mib_max', 'higher-worse'),
                                 ('process_ram_mib_max', 'higher-worse')):
            aggregate = 'max' if field.endswith('_max') else 'median'
            before = baseline_summary[field][aggregate]
            after = candidate[field][aggregate]
            if before is None or after is None:
                continue
            delta = after - before
            row['metrics'][field] = {
                'baseline': before, 'condition': after, 'delta': delta,
                'delta_percent': delta * 100.0 / abs(before) if before else None,
            }
            if (direction == 'lower-worse' and delta < 0) or (direction == 'higher-worse' and delta > 0):
                row['signals'].append({'metric': field, 'direction': direction, 'delta': delta})
        base_faults = sum(bool(run.get('measurements', {}).get('fault')) for run in grouped[baseline])
        new_faults = sum(bool(run.get('measurements', {}).get('fault')) for run in grouped[label])
        base_timeouts = sum(bool(run.get('timed_out')) for run in grouped[baseline])
        new_timeouts = sum(bool(run.get('timed_out')) for run in grouped[label])
        if new_faults > base_faults:
            row['signals'].append({'metric': 'fault_count', 'baseline': base_faults, 'condition': new_faults})
        if new_timeouts > base_timeouts:
            row['signals'].append({'metric': 'timeout_count', 'baseline': base_timeouts,
                                   'condition': new_timeouts})
        row['note'] = 'Directional deltas are candidate regressions only; compare same scene, build, cache, and conditions.'
        deltas.append(row)
    return deltas


def _format(value, digits=2):
    return 'not collected' if value is None else f'{value:.{digits}f}'


def render_markdown(manifest, report_dir=None):
    report_dir = Path(report_dir or ROOT).resolve()

    def link_target(path):
        target = Path(path)
        if not target.is_absolute():
            target = ROOT / target
        return os.path.relpath(target.resolve(), report_dir).replace(os.sep, '/')

    runs = manifest.get('runs', [])
    labels = list(dict.fromkeys(run.get('condition', 'run') for run in runs))
    lines = [
        '# Regression run report', '',
        f"Generated: {manifest.get('created_utc', 'unknown')}",
        f"Scenario: {manifest.get('scenario') or 'unspecified'}",
        f"Issues: {', '.join('#'+str(i) for i in manifest.get('issue_ids', [])) or 'none specified'}",
        'Visual verdict: **not automatically approved**. Review captures and provenance manually.', '',
        '## Run configuration', '',
        f"- Commit: `{manifest.get('build', {}).get('git_sha') or 'not collected'}`",
        f"- Executable SHA-256: `{manifest.get('build', {}).get('exe_sha256') or 'not collected'}`",
        f"- Host: {manifest.get('host', {}).get('platform', 'not collected')}",
        f"- Cache state declared by operator: `{manifest.get('cache_state', 'unknown')}` (the harness never deletes caches)",
        f"- Pad script: `{manifest.get('pad_script') or 'none'}`",
        f"- Reference: `{manifest.get('reference', {}).get('source', 'none')}`; "
        f"{manifest.get('reference', {}).get('status', 'no reference')}", '',
        '## Condition summaries', '',
        '| Condition | Runs | Failures | FPS median | Max worst frame (ms) | Window p95 of frame p99 (ms) | Window p99 of frame p99 (ms) | Max adapter usage (MiB) | Max process working set (MiB) |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |',
    ]
    for label in labels:
        group = [run for run in runs if run.get('condition') == label]
        summary = _condition_summary(group)
        failures = sum(bool(run.get('exit_code') not in (0, None) or run.get('timed_out')
                            or run.get('measurements', {}).get('fault')) for run in group)
        lines.append(
            f"| {label} | {len(group)} | {failures} | "
            f"{_format(summary['fps_median']['median'])} | "
            f"{_format(summary['worst_frame_ms_max']['max'])} | "
            f"{_format(summary['window_p95_of_frame_p99_ms']['median'])} | "
            f"{_format(summary['window_p99_of_frame_p99_ms']['median'])} | "
            f"{_format(summary['vram_used_mib_max']['max'], 0)} | "
            f"{_format(summary['process_ram_mib_max']['max'], 0)} |"
        )
    comparisons = compare_conditions(manifest)
    lines += ['', '## A/B deltas: review signals, not verdicts', '']
    if not comparisons:
        lines.append('No second condition was collected; no A/B delta is available.')
    else:
        for comparison in comparisons:
            lines.append(f"### `{comparison['condition']}` vs `{comparison['baseline']}`")
            if not comparison['metrics']:
                lines.append('No matching measured metrics across both conditions.')
            for field, values in comparison['metrics'].items():
                percent = (f" ({values['delta_percent']:+.2f}%)" if values['delta_percent'] is not None else '')
                lines.append(f"- `{field}`: {values['baseline']:.3f} -> {values['condition']:.3f}; "
                             f"delta {values['delta']:+.3f}{percent}.")
            signals = ', '.join(item['metric'] for item in comparison['signals'])
            lines.append('- Candidate signals: ' + (signals or 'none by direction only; visual review still required.'))
            lines.append(f"- {comparison['note']}")
    lines += [
        '', 'Percentiles summarize sampled 5-second runtime windows; they are not raw per-frame distributions. '
        'Missing telemetry is shown as “not collected”, never inferred.', '', '## Runs', '',
    ]
    for run in runs:
        metrics = run.get('measurements', {})
        summary = metrics.get('summary', {})
        lines += [
            f"### {run.get('condition', 'run')} / iteration {run.get('iteration', '?')}", '',
            f"- Scene: {run.get('scene') or 'unspecified'}; cache: `{run.get('cache_state', 'unknown')}`; "
            f"duration: {run.get('elapsed_seconds', 'not collected')} s; exit: {run.get('exit_code', 'unknown')}; "
            f"timeout: {run.get('timed_out', False)}",
            f"- Game: {metrics.get('gpu') or 'GPU not parsed'}; driver: {metrics.get('driver') or 'not parsed'}; "
            f"CUSA/version: {(metrics.get('game') or {}).get('cusa', 'not parsed')} "
            f"{(metrics.get('game') or {}).get('version', '')}; eboot SHA: "
            f"`{(metrics.get('game') or {}).get('eboot_sha256', 'not parsed')}`",
            f"- Expected identity check: `{run.get('game_identity_check', {}).get('status', 'not specified')}`",
            f"- FPS median: {_format(summary.get('fps_median'))}; worst frame max: "
            f"{_format(summary.get('worst_frame_ms_max'))} ms; p99 samples: "
            f"{summary.get('frame_p99_samples', 0)}; max adapter usage: {_format(summary.get('vram_used_mib_max'), 0)} MiB; "
            f"max working set: {_format(summary.get('process_ram_mib_max'), 0)} MiB; "
            f"max private commit: {_format(summary.get('process_committed_mib_max'), 0)} MiB; "
            f"max Vulkan allocations: {_format(summary.get('renderer_allocated_mib_max'), 0)} MiB; "
            f"faults: {metrics.get('fault_count', 0)}; "
            f"VUIDs: {', '.join(metrics.get('validation_vuids', [])) or 'none recorded'}",
            f"- Log: `{run.get('log_path', 'not recorded')}`; captures: `{run.get('capture_dir', 'none')}`",
        ]
        trace = run.get('frame_trace')
        if run.get('frame_trace_error'):
            lines.append(f"- Frame trace unavailable: {run['frame_trace_error']}")
        if trace:
            lines.append(
                f"- Raw guest frames: {trace['samples']}; mean FPS: {_format(trace['fps_mean'])}; "
                f"P95/P99: {_format(trace['frame_p95_ms'])}/{_format(trace['frame_p99_ms'])} ms; "
                f"stutters/min: {_format(trace['stutters_per_minute'])} "
                f"(>1.5x whole-trace median). CSV: `{run.get('frame_trace_path')}`. "
                "Captured intervals exclude startup before the first flip and may omit the unflushed tail.")
            videoout_metrics = [
                (name, trace.get(f'{name}_p95_ms'), trace.get(f'{name}_max_ms'))
                for name in ('guest_submit_gap', 'submit_to_queue', 'videoout_queue_wait')
                if trace.get(f'{name}_p95_ms') is not None
            ]
            if videoout_metrics:
                lines.append('- VideoOut timing P95/max (ms): ' + '; '.join(
                    f'{name} {_format(p95)}/{_format(maximum)}' for name, p95, maximum in videoout_metrics))
            gpu_wait_sites = trace.get('gpu_tick_wait_site_samples', [])
            if gpu_wait_sites:
                top = ', '.join(
                    f"{site['file']}:{site['line']} ({site['intervals']} intervals, "
                    f"max {_format(site['max_wait_ns'] / 1e6)} ms, target tick {site['max_target_tick']})"
                    for site in gpu_wait_sites[:5])
                lines.append('- Longest GPU semaphore wait per frame interval: ' + top +
                             '. These are per-interval maxima, not total wait counts.')
        pipeline_waits = metrics.get('graphics_pipeline_waits', [])
        if pipeline_waits:
            prep_samples = [wait.get('prep_ms') for wait in pipeline_waits
                            if wait.get('prep_ms') is not None]
            queue_samples = [wait.get('queue_ms') for wait in pipeline_waits if wait.get('queue_ms') is not None]
            create_samples = [wait.get('create_ms') for wait in pipeline_waits if wait.get('create_ms') is not None]
            breakdown = ''
            if queue_samples and len(queue_samples) == len(pipeline_waits) and len(create_samples) == len(pipeline_waits):
                breakdown = (f"; queue {sum(queue_samples):.1f} ms, pipeline creation "
                             f"{sum(create_samples):.1f} ms")
            if prep_samples and len(prep_samples) == len(pipeline_waits):
                breakdown += f"; preparation lead max {_format(max(prep_samples))} ms"
            prep_statuses = {}
            for wait in pipeline_waits:
                if wait.get('prep_status'):
                    prep_statuses[wait['prep_status']] = prep_statuses.get(wait['prep_status'], 0) + 1
            if prep_statuses:
                breakdown += '; preparation association ' + ', '.join(
                    f'{status} {count}' for status, count in sorted(prep_statuses.items()))
            sources = {}
            for wait in pipeline_waits:
                if wait.get('source'):
                    sources[wait['source']] = sources.get(wait['source'], 0) + 1
            if sources:
                breakdown += '; wait source ' + ', '.join(
                    f'{source} {count}' for source, count in sorted(sources.items()))
            lines.append(
                f"- Visible graphics pipeline waits: {len(pipeline_waits)} events, "
                f"{sum(wait['wait_ms'] for wait in pipeline_waits):.1f} ms total, "
                f"max {_format(max(wait['wait_ms'] for wait in pipeline_waits))} ms, "
                f"{sum(wait['wait_ms'] > 50 for wait in pipeline_waits)} over 50 ms{breakdown}; "
                "per-event keys and SPIR-V hashes are in the manifest.")
        wait_profiles = metrics.get('wait_profiles', [])
        if wait_profiles:
            sites = {}
            for entry in wait_profiles:
                site = (entry['site'], entry['line'])
                totals = sites.setdefault(site, {'calls': 0, 'wait_ms': 0.0, 'max_wait_ms': 0.0})
                totals['calls'] += entry['calls']
                totals['wait_ms'] += entry['wait_ms']
                totals['max_wait_ms'] = max(totals['max_wait_ms'], entry['max_wait_ms'])
            top_sites = sorted(sites.items(), key=lambda item: item[1]['wait_ms'], reverse=True)[:5]
            lines.append('- GPU semaphore wait sites (summed call time; calls may overlap): ' + '; '.join(
                f"{site}:{line} {value['calls']} calls, {value['wait_ms']:.1f} ms, "
                f"max {value['max_wait_ms']:.1f} ms" for (site, line), value in top_sites))
        visual = run.get('visual') or {}
        if visual:
            lines.append(f"- Visual comparison: `{visual.get('reference_status', 'unreviewed')}`; "
                         f"{len(visual.get('frames', []))} captured images, "
                         f"{len(visual.get('temporal', []))} consecutive-frame deltas.")
            for path in (visual.get('capture_files', []) + visual.get('video_files', [])
                         + visual.get('preview_files', [])):
                lines.append(f"  - Capture: [{Path(path).name}]({link_target(path)})")
            for frame in visual.get('frames', []):
                if frame.get('mae_0_1') is not None:
                    lines.append(f"  - `{frame.get('actual')}` vs `{frame.get('reference')}`: "
                                 f"MAE={frame['mae_0_1']:.5f}, PSNR={_format(frame.get('psnr_db'))} dB, "
                                 f"pixels over tolerance={frame.get('changed_fraction', 0):.3%}; review required. "
                                 + (f"[diff]({link_target(frame['diff_file'])})" if frame.get('diff_file') else ''))
                elif frame.get('error'):
                    lines.append(f"  - `{frame.get('actual')}`: comparison unavailable: {frame['error']}.")
            for item in visual.get('temporal', []):
                if item.get('diff_file'):
                    lines.append(f"  - Temporal delta `{item['from']}` -> `{item['to']}`: "
                                 f"{item['mean_delta_0_1']:.5f}; [diff]({link_target(item['diff_file'])}).")
        lines.append('')
    return '\n'.join(lines).rstrip() + '\n'


def render_html(manifest, report_dir=None):
    report_dir = Path(report_dir or Path.cwd()).resolve()

    def href(path):
        path = Path(path)
        if not path.is_absolute():
            path = Path(__file__).resolve().parent.parent / path
        return os.path.relpath(path.resolve(), report_dir).replace(os.sep, '/')

    rows = []
    for run in manifest.get('runs', []):
        metrics = run.get('measurements', {})
        summary = metrics.get('summary', {})
        capture_links = []
        visual = run.get('visual') or {}
        for path in (visual.get('preview_files', []) + visual.get('video_files', [])
                     + [frame['diff_file'] for frame in visual.get('frames', []) if frame.get('diff_file')]
                     + [item['diff_file'] for item in visual.get('temporal', []) if item.get('diff_file')]):
            name = Path(path).name
            link = html.escape(href(path), quote=True)
            if path in visual.get('preview_files', []):
                capture_links.append(f'<a href="{link}"><img src="{link}" alt="{html.escape(name, quote=True)}" '
                                     f'width="240"><br>{html.escape(name)}</a>')
            else:
                capture_links.append(f'<a href="{link}">{html.escape(name)}</a>')
        rows.append('<tr>' + ''.join(f'<td>{html.escape(str(value))}</td>' for value in (
            f"{run.get('condition')} / {run.get('iteration')}", run.get('scene') or 'unspecified',
            _format(summary.get('fps_median')), _format(summary.get('window_p99_of_frame_p99_ms')),
            _format(summary.get('vram_used_mib_max'), 0), _format(summary.get('process_ram_mib_max'), 0),
            ', '.join(metrics.get('validation_vuids', [])) or 'none recorded',
        )) + f'<td>{", ".join(capture_links) or "none captured"}</td></tr>')
    delta_rows = []
    for comparison in compare_conditions(manifest):
        for field, values in comparison['metrics'].items():
            delta_rows.append('<tr>' + ''.join(f'<td>{html.escape(str(value))}</td>' for value in (
                comparison['baseline'], comparison['condition'], field,
                f"{values['baseline']:.3f}", f"{values['condition']:.3f}", f"{values['delta']:+.3f}",
                'candidate regression signal' if any(s['metric'] == field for s in comparison['signals'])
                else 'no directional signal',
            )) + '</tr>')
    title = html.escape(manifest.get('scenario') or 'Unlabeled regression run')
    return ('<!doctype html><html lang="en"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>{title}</title><h1>{title}</h1>'
            '<p>Visual results are evidence, not an automatic pass. Inspect frames and reference provenance.</p>'
            '<table><thead><tr><th>Run</th><th>Scene</th><th>FPS median</th><th>Window p99 of frame p99 (ms)</th>'
            '<th>Adapter usage max (MiB)</th><th>Working set max (MiB)</th><th>VUIDs</th><th>Captures</th></tr></thead>'
            '<tbody>' + ''.join(rows) + '</tbody></table><h2>A/B deltas: review signals, not verdicts</h2>'
            '<table><thead><tr><th>Baseline</th><th>Condition</th><th>Metric</th><th>Baseline value</th>'
            '<th>Condition value</th><th>Delta</th><th>Interpretation</th></tr></thead><tbody>'
            + ''.join(delta_rows) + '</tbody></table></html>')


def write_reports(manifest, report_dir):
    report_dir = Path(report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    md_path = report_dir / 'report.md'
    html_path = report_dir / 'report.html'
    md_path.write_text(render_markdown(manifest, report_dir), encoding='utf-8')
    html_path.write_text(render_html(manifest, report_dir), encoding='utf-8')
    return md_path, html_path


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--out', type=Path, help='report directory (defaults beside the manifest)')
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    md_path, html_path = write_reports(manifest, args.out or args.manifest.parent)
    print(f'Wrote {md_path} and {html_path}')


if __name__ == '__main__':
    main()
