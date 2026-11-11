"""Smoke-test the packaged build: run dist/theorder1886-windows through `The Order 1886.exe --run`
on the prologue pad route, twice in a row on one fresh profile (cold, then warm), and check
that both sessions reach 3D gameplay without a fault.

    python tools/smoke_package.py --game-dir C:\\games\\CUSA00035 [--secs 120] [--keep]

The profile is a copy of user/ without its shader caches, in out/smoke/ (deleted afterwards
unless --keep); the player's dist/.../user is never touched. Exit code 0 = pass.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / 'dist' / 'theorder1886-windows'
PRESSES = (24, 28, 32, 36, 40, 50, 60)  # get past the title and into the prologue
GAMEPLAY_DRAWS = 300  # 32 draws/frame is a stuck/black screen; the prologue is several hundred


def session(log_path, profile, game_dir, secs, extra_env):
    pad = profile.parent / 'pad.txt'
    pad.write_text('')
    env = dict(os.environ, THEORDER_GAME_DIR=str(game_dir), THEORDER_USER_DIR=str(profile),
               THEORDER_GPU_USER_DIR=str(profile), THEORDER_PAD_FILE=str(pad),
               THEORDER_FRAME_STATS='1', **extra_env)
    with open(log_path, 'w', encoding='utf-8', errors='replace') as log:
        proc = subprocess.Popen([str(DIST / 'The Order 1886.exe'), '--run'], env=env, cwd=DIST,
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        presses = list(PRESSES)
        start = time.time()
        while time.time() - start < secs and proc.poll() is None:
            if presses and time.time() - start >= presses[0]:
                pad.write_text('cross')
                time.sleep(0.5)
                pad.write_text('')
                presses.pop(0)
            time.sleep(0.1)
        exited = proc.poll()
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)], capture_output=True)
    text = Path(log_path).read_text(encoding='utf-8', errors='replace')
    draws = [int(n) for n in re.findall(r'(\d+) draws/frame', text)]
    worst = [float(w) for w in re.findall(r'worst frame ([\d.]+) ms', text)]
    return {
        'exited_early': exited is not None,
        'faults': len(re.findall(r'^(Fault:|STOP:)', text, re.M)),
        'max_draws': max(draws, default=0),
        'windows': len(worst),
        'over_100ms': sum(w > 100 for w in worst),
        'driver_builds': text.count('in the driver'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--game-dir', required=True, type=Path)
    parser.add_argument('--secs', type=float, default=120)
    parser.add_argument('--keep', action='store_true')
    parser.add_argument('--env', action='append', default=[], help='VAR=VALUE for the game')
    args = parser.parse_args()
    if not (DIST / 'The Order 1886.exe').is_file():
        sys.exit(f'no package at {DIST}: run packaging/windows/package.sh first')
    work = ROOT / 'out' / 'smoke'
    shutil.rmtree(work, ignore_errors=True)
    profile = work / 'user'
    skip = {'CUSA00035', 'CUSA00076', 'CUSA00785', 'CUSA00070', 'CUSA00100', 'CUSA00670'}
    shutil.copytree(ROOT / 'user', profile, ignore=lambda d, names: [
        n for n in names if d.endswith('cache') and (n in skip or n.startswith('driver-'))])
    extra = dict(item.split('=', 1) for item in args.env)
    ok = True
    for label in ('cold', 'warm'):
        result = session(work / f'{label}.log', profile, args.game_dir, args.secs, extra)
        passed = (not result['exited_early'] and result['faults'] == 0 and
                  result['max_draws'] >= GAMEPLAY_DRAWS)
        ok &= passed
        print(f"{label}: {'PASS' if passed else 'FAIL'} {result}")
    logs = work / 'logs'
    logs.mkdir(exist_ok=True)
    for name in ('cold.log', 'warm.log'):
        shutil.copy2(work / name, logs / name)
    if not args.keep:
        shutil.rmtree(profile, ignore_errors=True)
    print(f'logs: {logs}')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
