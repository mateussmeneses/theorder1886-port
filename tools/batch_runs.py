"""Repeated timed runs of the game with A/B conditions, summarized as failure rates.

    python tools/batch_runs.py --runs 8 --secs 40 --cond base= --cond nomsaa=THEORDER_NO_MSAA=1
    python tools/batch_runs.py --runs 4 --secs 90 --game-args "--level cat_010_streets"
    python tools/batch_runs.py --runs 4 --secs 90 --pad "22:cross 26:cross 30:cross"

--pad presses buttons at the given seconds after launch (SECS:TOKEN[,TOKEN][:HOLD]) through
THEORDER_PAD_FILE (src/runtime_pad.c), e.g. to get past the title screen.

Each --cond is LABEL=[VAR=VALUE[,VAR=VALUE...]]. Conditions are interleaved run by run, so a
drift (shader cache warming, temperature) affects all of them alike. Logs land in out/batch/;
a run fails on a fault or when it stops presenting before the timeout (fewer stats windows).
"""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import fault_stacks
import regression_report
from regression_report import compare_captures, parse_log, write_reports

ROOT = Path(__file__).resolve().parent.parent
GAME_DIR = os.environ.get('THEORDER_GAME_DIR') or str(ROOT.parent / 'CUSA00035')


def parse_affinity(text):
    """A CPU affinity mask from hex (0x5500) or decimal text; it must name at least one CPU."""
    try:
        mask = int(text, 0)
    except ValueError:
        raise ValueError(f'not a CPU mask: {text!r}') from None
    if mask <= 0 or mask >= 1 << 64:
        raise ValueError(f'CPU mask must select 1 to 64 logical processors: {text!r}')
    return mask


@contextlib.contextmanager
def inherited_affinity(mask):
    """Windows: the runner takes mask only while it starts a child, which inherits it (as do the
    game and its threads, started by that child); the runner's own mask is restored at once.
    The runtime records guest thread affinity without applying it (src/runtime_thread.c)."""
    if mask is None:
        yield
        return
    if os.name != 'nt':
        raise RuntimeError('--affinity is implemented for Windows only')
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    pointer = ctypes.POINTER(ctypes.c_size_t)
    kernel32.GetProcessAffinityMask.argtypes = (wintypes.HANDLE, pointer, pointer)
    kernel32.SetProcessAffinityMask.argtypes = (wintypes.HANDLE, ctypes.c_size_t)
    process = kernel32.GetCurrentProcess()
    current, system = ctypes.c_size_t(), ctypes.c_size_t()
    if not kernel32.GetProcessAffinityMask(process, ctypes.byref(current), ctypes.byref(system)):
        raise OSError(ctypes.get_last_error(), 'GetProcessAffinityMask failed')
    if mask & ~system.value:
        raise ValueError(f'CPU mask {mask:#x} names processors outside the system mask {system.value:#x}')
    if not kernel32.SetProcessAffinityMask(process, ctypes.c_size_t(mask)):
        raise OSError(ctypes.get_last_error(), 'SetProcessAffinityMask failed')
    try:
        yield
    finally:
        kernel32.SetProcessAffinityMask(process, current)


def kill_orphans():
    # Legacy global cleanup is opt-in; never kill an unrelated game process by default.
    subprocess.run(['taskkill', '/F', '/IM', 'theorder-probe.exe'], capture_output=True)


def terminate_process_tree(process):
    """Terminate only the process tree started for this run."""
    if process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True)
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait(timeout=10)
    finally:
        if os.name != 'nt':
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def _sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def _build_info(exe):
    result = {'exe_sha256': _sha256(exe) if exe.is_file() else None}
    git = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True)
    result['git_sha'] = git.stdout.strip() if git.returncode == 0 else None
    status = subprocess.run(['git', 'status', '--porcelain'], cwd=ROOT, capture_output=True, text=True)
    result['dirty'] = bool(status.stdout.strip()) if status.returncode == 0 else None
    return result


def _safe_settings(env):
    excluded = {'THEORDER_GAME_DIR', 'THEORDER_DATA_DIR', 'THEORDER_USER_DIR',
                'THEORDER_GPU_USER_DIR', 'THEORDER_CONFIG', 'THEORDER_PAD_FILE',
                'THEORDER_RESTART_COMMAND', 'THEORDER_PROBE', 'THEORDER_FRAME_CSV',
                'THEORDER_USER_NAME', 'THEORDER_MODS_CONFIG', 'THEORDER_PATCHES_CONFIG'}
    return {
        key: value for key, value in sorted(env.items())
        if key.startswith('THEORDER_') and key not in excluded
        and not key.endswith(('_DIR', '_FILE', '_PATH', '_TRIGGER',
                               '_CONFIG', '_CSV', '_LOG', '_EXE'))
        and not any(word in key.upper() for word in ('SECRET', 'TOKEN', 'PASSWORD'))
        # Arbitrary THEORDER_* options may hold private filesystem paths.
        and not re.search(r'(?:[A-Za-z]:[\\/]|\\\\|(?:^|\s)/[^\s]+)', str(value))
    }


def _read_ini(path):
    path = Path(path)
    if not path.is_file():
        return {'sha256': None, 'values': {}}
    values = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.strip()
        if not line or line.startswith(('#', ';')) or '=' not in line:
            continue
        key, value = line.split('=', 1)
        values[key.strip()] = value.strip()
    return {'sha256': _sha256(path), 'values': values}


def _configure_run_environment(env, run_profile, run_ini):
    if run_profile:
        env['THEORDER_USER_DIR'] = str(run_profile)
        env['THEORDER_GPU_USER_DIR'] = str(run_profile)
    scripts_dir = str(ROOT / 'scripts')
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    import graphics_options

    effective = graphics_options.environment(
        graphics_options.startup_options(graphics_options.read_ini(run_ini)))
    env.update(effective)
    return effective


def _apply_ini_settings(path, settings):
    """Update key=value settings in a private run config, preserving comments and unrelated keys."""
    path = Path(path)
    if not settings:
        return
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines() if path.is_file() else []
    remaining = dict(settings)
    output = []
    for line in lines:
        stripped = line.strip()
        if stripped and not stripped.startswith(('#', ';')) and '=' in stripped:
            key, _ = stripped.split('=', 1)
            key = key.strip()
            if key in settings:
                indent = line[:len(line) - len(line.lstrip())]
                output.append(f'{indent}{key}={settings[key]}')
                remaining.pop(key, None)
                continue
        output.append(line)
    output.extend(f'{key}={value}' for key, value in remaining.items())
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(output) + '\n', encoding='utf-8')


def _profile_for_run(profile_root, template, cache_state, label, iteration, run_id):
    if not profile_root:
        return None, False
    root = Path(profile_root).resolve()
    target = root / label / run_id
    if target.exists():
        raise FileExistsError(f'run profile already exists; refusing to reuse saves/cache: {target}')
    if cache_state == 'warm' and not template:
        raise ValueError('warm cache requires an existing profile or --user-template with cache files')
    if cache_state == 'warm' and not _has_port_cache(template):
        raise ValueError(f'warm user template has no shader/pipeline cache: {template}')
    target.parent.mkdir(parents=True, exist_ok=True)
    if template:
        target.mkdir()
        persistent = ['savedata', 'cheats', 'custom_configs', 'custom_modules',
                      'custom_trophy', 'data', 'home', 'patches', 'trophy']
        if cache_state != 'cold':
            persistent.extend(('cache', 'shader'))
        for name in persistent:
            source = Path(template) / name
            if source.is_dir():
                shutil.copytree(source, target / name)
    else:
        target.mkdir()
    return target, False


def _has_port_cache(profile):
    return any((Path(profile) / name).is_dir()
               and any(path.is_file() for path in (Path(profile) / name).rglob('*'))
               for name in ('cache', 'shader'))


def _stored_path(path):
    if not path:
        return None
    path = Path(path).resolve()
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.name


def _scheduled_file(path, seconds, started, stop):
    if stop.wait(max(0.0, float(seconds) - (time.monotonic() - started))):
        return
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).touch(exist_ok=True)


def _scheduled_repeated_trigger(path, first_seconds, count, spacing, started, stop):
    path = Path(path)
    for index in range(count):
        target = started + float(first_seconds) + index * float(spacing)
        if stop.wait(max(0.0, target - time.monotonic())):
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch(exist_ok=True)
        deadline = time.monotonic() + 2.0
        while path.exists() and time.monotonic() < deadline:
            if stop.wait(0.01):
                return


def summarize(log: str, secs: int):
    metrics = parse_log(log, secs)
    fault = re.search(r'^Fault: .*? at (\S+)', log, re.M)
    return {
        'flip': metrics['flip'],
        'fault': fault.group(1) if fault else metrics['fault'],
        'hung': metrics['hung'],
        'compile_s': metrics['compile_s'],
        'windows': metrics['windows'],
        'metrics': metrics,
    }


def play_pad(script: str, pad_file: Path, stop: threading.Event):
    start = time.monotonic()
    for step in script.split():
        secs, tokens, *hold = step.split(':')
        if stop.wait(max(0.0, float(secs) - (time.monotonic() - start))):
            return
        pad_file.write_text(tokens.replace(',', ' '))
        stop.wait(float(hold[0]) if hold else 0.3)
        pad_file.write_text('')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runs', type=int, default=8)
    parser.add_argument('--frame-trace', action='store_true',
                        help='capture buffered per-frame CSV and true whole-trace P95/P99')
    parser.add_argument('--secs', type=int, default=40)
    parser.add_argument('--cond', action='append', default=[], help='LABEL=VAR=VALUE,...')
    parser.add_argument('--setting', action='append', default=[], help='global run INI setting KEY=VALUE')
    parser.add_argument('--setting-cond', action='append', default=[],
                        help='per-condition INI setting LABEL=KEY=VALUE (repeatable)')
    parser.add_argument('--game-args', default='', help='extra run.py arguments, e.g. "--level cat_010_streets"')
    parser.add_argument('--pad', default='', help='scripted presses: "SECS:TOKEN[,TOKEN][:HOLD] ..."')
    parser.add_argument('--scene', default='', help='human-readable scene/checkpoint under test')
    parser.add_argument('--issue', type=int, action='append', default=[], help='related GitHub issue (repeatable)')
    parser.add_argument('--cache-state', choices=('cold', 'warm', 'unknown'), default='unknown',
                        help='declared port cache state; no cache is deleted by this tool')
    parser.add_argument('--cache-cond', action='append', default=[],
                        help='per-condition cache state: LABEL=cold|warm|unknown (repeatable)')
    parser.add_argument('--profile-root', type=Path,
                        help='isolated root for per-condition user profiles; required for cold/warm claims')
    parser.add_argument('--data-root', type=Path,
                        help='isolated root for run.py preparation/config artifacts (defaults under out/batch)')
    parser.add_argument('--user-template', type=Path,
                        help='existing isolated user profile copied into new profiles (saves retained)')
    parser.add_argument('--game-dir', type=Path, help='override THEORDER_GAME_DIR')
    parser.add_argument('--cusa', default='', help='expected title ID, checked against the runtime log')
    parser.add_argument('--game-version', default='', help='expected game version, checked against the runtime log')
    parser.add_argument('--validation', action='store_true', help='enable Vulkan validation layer/log capture')
    parser.add_argument('--capture-dir', type=Path, help='directory for unique per-run captures')
    parser.add_argument('--present-frames', type=int, default=0,
                        help='dump this many consecutive final display frames as raw RGBA/BGRA')
    parser.add_argument('--capture-at', type=float, default=10,
                        help='seconds after launch to trigger final display dumps')
    parser.add_argument('--frame-dump-at', type=float,
                        help='seconds after launch to dump the actual presented framebuffer (works without upscaling)')
    parser.add_argument('--frame-dump-count', type=int, default=1,
                        help='number of presenter frames to dump after --frame-dump-at')
    parser.add_argument('--frame-dump-spacing', type=float, default=0.5,
                        help='seconds between requested presenter frame dumps')
    parser.add_argument('--raw-format', choices=('rgba', 'bgra'),
                        help='channel order for untagged presenter .raw files; do not guess')
    parser.add_argument('--frame-capture-at', type=float,
                        help='seconds after launch to trigger one full pass/shader frame capture')
    parser.add_argument('--reference-dir', type=Path, help='external/golden frame directory, matched by frame stem')
    parser.add_argument('--reference-source', choices=('ps4', 'shadps4', 'port', 'other', 'unknown'),
                        default='unknown')
    parser.add_argument('--reference-validated', action='store_true',
                        help='declare human verification of a PS4/shadPS4 reference; never auto-set')
    parser.add_argument('--reference-note', default='')
    parser.add_argument('--mask', type=Path, help='grayscale mask: nonzero pixels participate in comparison')
    parser.add_argument('--pixel-tolerance', type=int, default=8,
                        help='luma delta threshold used only for changed-pixel reporting')
    parser.add_argument('--manifest', type=Path, help='JSON manifest destination')
    parser.add_argument('--report-dir', type=Path, help='Markdown/HTML report destination')
    parser.add_argument('--affinity', default='',
                        help='Windows: CPU affinity mask for the game process tree, e.g. 0x5500 '
                             '(opt-in; the runner itself is not restricted)')
    parser.add_argument('--kill-existing', action='store_true',
                        help='legacy opt-in: kill every theorder-probe.exe before the batch')
    args = parser.parse_args()
    try:
        affinity = parse_affinity(args.affinity) if args.affinity else None
    except ValueError as error:
        parser.error(str(error))
    if args.runs < 1 or args.secs < 1:
        parser.error('--runs and --secs must be positive')
    if args.present_frames < 0:
        parser.error('--present-frames cannot be negative')
    if args.frame_dump_count < 1 or args.frame_dump_spacing <= 0:
        parser.error('--frame-dump-count must be positive and --frame-dump-spacing must be > 0')
    if args.cache_state != 'unknown' and not args.profile_root:
        parser.error('--profile-root is required to claim cold/warm port cache state')
    if args.user_template and not args.profile_root:
        parser.error('--user-template requires --profile-root')
    if args.user_template and not args.user_template.is_dir():
        parser.error(f'user template does not exist: {args.user_template}')
    conds = []
    for c in args.cond or ['base=']:
        label, _, assigns = c.partition('=')
        env = dict(a.split('=', 1) for a in assigns.split(',') if a)
        conds.append((label, env))
    labels = [label for label, _ in conds]
    if len(labels) != len(set(labels)):
        parser.error('condition labels must be unique')
    cache_states = {label: args.cache_state for label in labels}
    for declaration in args.cache_cond:
        label, separator, state = declaration.partition('=')
        if not separator or label not in cache_states or state not in ('cold', 'warm', 'unknown'):
            parser.error('--cache-cond must name an existing condition and use cold|warm|unknown')
        cache_states[label] = state
    if any(state != 'unknown' for state in cache_states.values()) and not args.profile_root:
        parser.error('--profile-root is required to claim cold/warm port cache state')

    global_ini_settings = {}
    for declaration in args.setting:
        key, separator, value = declaration.partition('=')
        if not separator or not key.strip():
            parser.error('--setting must be KEY=VALUE')
        global_ini_settings[key.strip()] = value
    ini_settings = {label: dict(global_ini_settings) for label in labels}
    for declaration in args.setting_cond:
        label, separator, assignment = declaration.partition('=')
        key, setting_separator, value = assignment.partition('=')
        if not separator or not setting_separator or label not in ini_settings or not key.strip():
            parser.error('--setting-cond must be LABEL=KEY=VALUE for an existing condition')
        ini_settings[label][key.strip()] = value

    exe_env = os.environ.get('THEORDER_PROBE')
    exe = Path(exe_env) if exe_env else ROOT / 'out' / 'theorder-probe.exe'
    if not exe.is_file():
        parser.error(f'probe executable not found: {exe}')
    print(f'exe {exe.name} built {time.ctime(exe.stat().st_mtime)}')
    out = ROOT / 'out' / 'batch'
    (out / 'configs').mkdir(parents=True, exist_ok=True)
    (out / 'manifests').mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    manifest_path = args.manifest or out / 'manifests' / f'regression-{stamp}.json'
    report_dir = args.report_dir or manifest_path.parent / manifest_path.stem
    capture_root = args.capture_dir or out / 'captures'
    data_root = (args.data_root or out / 'runtime-data').resolve()
    manifest = {
        'schema_version': 1,
        'created_utc': datetime.now(timezone.utc).isoformat(),
        'scenario': args.scene,
        'issue_ids': args.issue,
        'duration_seconds': args.secs,
        'pad_script': args.pad,
        'game_args': args.game_args,
        'cache_state': args.cache_state,
        'cache_state_by_condition': cache_states,
        'cache_state_note': 'Port user shader/pipeline cache only; OS and driver caches are not controlled.',
        'raw_format': args.raw_format,
        'cpu_affinity': f'{affinity:#x}' if affinity is not None else None,
        'host': {
            'platform': platform.platform(), 'system': platform.system(),
            'release': platform.release(), 'machine': platform.machine(),
            'processor': platform.processor(),
        },
        'build': _build_info(exe),
        'reference': {
            'directory': _stored_path(args.reference_dir),
            'source': args.reference_source,
            'validated_by_user': args.reference_validated,
            'note': args.reference_note,
            'mask': _stored_path(args.mask),
            'pixel_tolerance': args.pixel_tolerance,
        },
        'conditions': [{'label': label, 'cache_state': cache_states[label],
                'environment': _safe_settings(extra), 'ini_settings': ini_settings[label]}
                   for label, extra in conds],
        'runs': [],
    }
    if args.kill_existing:
        kill_orphans()

    results = {label: [] for label, _ in conds}
    for i in range(args.runs):
        for label, extra in conds:
            env = dict(os.environ)
            env.update({'THEORDER_GAME_DIR': str(args.game_dir or GAME_DIR),
                        'THEORDER_TIMEOUT': str(args.secs), 'THEORDER_FRAME_STATS': '1'})
            env.setdefault('THEORDER_FRAME_STATS', '1')
            env.update(extra)
            run_stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
            safe_label = re.sub(r'[^A-Za-z0-9_.-]+', '_', label).strip('._') or 'condition'
            run_id = f'{safe_label}-{i + 1:02d}-{run_stamp}'
            run_data_dir = data_root / run_id
            run_data_dir.mkdir(parents=True, exist_ok=False)
            env['THEORDER_DATA_DIR'] = str(run_data_dir)
            env['THEORDER_PROBE'] = str(exe.resolve())
            run_profile, profile_reused = _profile_for_run(
                args.profile_root, args.user_template, cache_states[label], safe_label, i + 1, run_id)
            if run_profile:
                env['THEORDER_USER_DIR'] = str(run_profile)
            elif args.user_template:
                parser.error('--user-template requires --profile-root')
            # Seed from the requested config, but let every run write its own copy.
            source_config = extra.get('THEORDER_CONFIG', os.environ.get('THEORDER_CONFIG'))
            run_ini = run_data_dir / 'theorder.ini'
            if source_config:
                source_path = Path(source_config)
                if not source_path.is_absolute():
                    source_path = ROOT / source_path
                if source_path.is_file():
                    shutil.copy2(source_path, run_ini)
            _apply_ini_settings(run_ini, ini_settings[label])
            env['THEORDER_CONFIG'] = str(run_ini)
            frame_trace_path = run_data_dir / 'frames.csv' if args.frame_trace else None
            if frame_trace_path:
                env['THEORDER_FRAME_CSV'] = str(frame_trace_path)
            effective_graphics = _configure_run_environment(env, run_profile, run_ini)
            if args.validation:
                env.setdefault('THEORDER_VK_VALIDATION', '1')
                env.setdefault('VK_ADD_LAYER_PATH', r'C:\msys64\clang64\bin')
                env.setdefault('VK_LOADER_LAYERS_ENABLE', '*validation*')
            capture_dir = None
            if (args.capture_dir or args.present_frames or args.frame_dump_at is not None
                    or args.frame_capture_at is not None):
                capture_dir = (capture_root / run_id).resolve()
                capture_dir.mkdir(parents=True, exist_ok=False)
                env['THEORDER_DUMP_DIR'] = str(capture_dir)
                if args.present_frames:
                    env['THEORDER_PRESENT_DUMP_TRIGGER'] = str(capture_dir / 'present.trigger')
                    env['THEORDER_PRESENT_DUMP_COUNT'] = str(args.present_frames)
                if args.frame_dump_at is not None:
                    env['THEORDER_FRAME_DUMP_TRIGGER'] = str(capture_dir / 'frame.trigger')
                if args.frame_capture_at is not None:
                    env['THEORDER_CAPTURE_TRIGGER'] = str(capture_dir / 'frame-capture.trigger')
                    env['THEORDER_CAPTURE_DIR'] = str(capture_dir / 'frame-analysis')
            log_path = out / f'{run_id}.log'
            stop = threading.Event()
            pad_file = None
            if args.pad:
                pad_file = out / f'{run_id}.pad'
                pad_file.write_text('')
                env['THEORDER_PAD_FILE'] = str(pad_file)
            if args.validation and not Path(env['VK_ADD_LAYER_PATH']).is_dir():
                print(f'WARNING: Vulkan validation layer directory not found: {env["VK_ADD_LAYER_PATH"]}',
                      flush=True)
            timed_out = False
            started = time.monotonic()
            actions = []
            with open(log_path, 'w', encoding='utf-8', errors='replace') as f:
                with inherited_affinity(affinity):
                    process = subprocess.Popen(
                        [sys.executable, str(ROOT / 'run.py')] + args.game_args.split(),
                        env=env, cwd=ROOT, stdin=subprocess.DEVNULL,
                        stdout=f, stderr=subprocess.STDOUT, start_new_session=(os.name != 'nt'))
                if args.pad and pad_file:
                    actions.append(threading.Thread(target=play_pad,
                                                    args=(args.pad, pad_file, stop), daemon=True))
                if capture_dir and args.present_frames:
                    actions.append(threading.Thread(target=_scheduled_file,
                                                    args=(capture_dir / 'present.trigger', args.capture_at,
                                                          started, stop), daemon=True))
                if capture_dir and args.frame_dump_at is not None:
                    actions.append(threading.Thread(
                        target=_scheduled_repeated_trigger,
                        args=(capture_dir / 'frame.trigger', args.frame_dump_at,
                              args.frame_dump_count, args.frame_dump_spacing, started, stop),
                        daemon=True))
                if capture_dir and args.frame_capture_at is not None:
                    actions.append(threading.Thread(target=_scheduled_file,
                                                    args=(capture_dir / 'frame-capture.trigger',
                                                          args.frame_capture_at, started, stop), daemon=True))
                for action in actions:
                    action.start()
                try:
                    process.wait(timeout=args.secs + 40)
                except subprocess.TimeoutExpired:
                    timed_out = True
                    terminate_process_tree(process)
                finally:
                    stop.set()
                    if process.poll() is None:
                        terminate_process_tree(process)
            stop.set()
            for action in actions:
                action.join(timeout=1)
            if pad_file and pad_file.exists():
                pad_file.write_text('')
            elapsed = time.monotonic() - started
            log_text = log_path.read_text(encoding='utf-8', errors='replace')
            r = summarize(log_text, args.secs)
            game_identity = regression_report.verify_game_identity(
                r['metrics'].get('game'), args.cusa, args.game_version)
            if game_identity['status'] == 'mismatch':
                print(f'WARNING: expected game identity mismatch: {game_identity}', flush=True)
            results[label].append(r)
            state = (f'FAULT {r["fault"]}' if r['fault'] else
                     'TIMEOUT' if timed_out else 'HUNG' if r['hung'] else
                     f'EXIT {process.returncode}' if process.returncode else 'ok')
            print(f'{label:>10} {i + 1:2}: flip {r["flip"]:5} compile {r["compile_s"]:5.1f}s {state}',
                  flush=True)
            visual = None
            if capture_dir:
                visual = compare_captures(
                    capture_dir, args.reference_dir, args.mask, args.pixel_tolerance,
                    args.reference_source, args.reference_validated, args.reference_note,
                    diff_dir=capture_dir / 'diffs', raw_format=args.raw_format)
            fault = fault_stacks.inspect_log(log_text, _stored_path(log_path),
                                            resolve_symbols=True, exe=exe.resolve())
            game = r['metrics'].get('game') or {}
            if args.cusa:
                game['cusa_expected'] = args.cusa
            if args.game_version:
                game['version_expected'] = args.game_version
            frame_trace, frame_trace_error = None, None
            if frame_trace_path:
                try:
                    with frame_trace_path.open(encoding='ascii', newline='') as stream:
                        frame_trace = regression_report.parse_frame_trace(stream)
                except (OSError, ValueError) as error:
                    frame_trace_error = str(error)
                    print(f'WARNING: frame trace unavailable: {error}', flush=True)
            record = {
                'run_id': run_id, 'condition': label, 'iteration': i + 1,
                'scene': args.scene, 'issue_ids': args.issue,
                'cache_state': cache_states[label], 'profile_reused': profile_reused,
                'user_profile': _stored_path(run_profile),
                'data_dir': _stored_path(run_data_dir),
                'started_utc': datetime.fromtimestamp(time.time() - elapsed, timezone.utc).isoformat(),
                'elapsed_seconds': round(elapsed, 3), 'exit_code': process.returncode,
                'timed_out': timed_out, 'log_path': _stored_path(log_path),
                'capture_dir': _stored_path(capture_dir), 'settings': {
                    'environment': _safe_settings(env),
                    'config': _read_ini(env.get('THEORDER_CONFIG', '')),
                    'applied_ini_settings': ini_settings[label],
                    'effective_graphics_environment': effective_graphics,
                },
                'measurements': {**r['metrics'], 'game': game},
                'game_identity_check': game_identity,
                'fault_stack': fault,
                'visual': visual,
                'frame_trace_path': _stored_path(frame_trace_path),
                'frame_trace': frame_trace,
                'frame_trace_error': frame_trace_error,
            }
            manifest['runs'].append(record)
    print('summary:')
    for label, rs in results.items():
        failed = sum(1 for r in rs if r['fault'] or r['hung'])
        flips = sorted(r['flip'] for r in rs)
        print(f'  {label:>10}: {failed}/{len(rs)} failed '
              f'({sum(1 for r in rs if r["fault"])} faults, {sum(1 for r in rs if r["hung"])} hung), '
              f'median flip {flips[len(flips) // 2]}, '
              f'mean compile {sum(r["compile_s"] for r in rs) / len(rs):.1f}s')
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    md_path, html_path = write_reports(manifest, report_dir)
    print(f'manifest: {manifest_path}')
    print(f'reports: {md_path}, {html_path}')


if __name__ == '__main__':
    main()
