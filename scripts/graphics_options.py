#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Startup graphics options of theorder.ini (gpu/shim/port_graphics.h) for run.py.

The in-game menu and the launcher write them; run.py turns them into game patches and
environment variables at every start, so "Apply and restart" uses the new values. After a
restart the game confirms them (restart_unconfirmed=0) once it shows frames; when a launch
never did, the next start goes back to the last confirmed values.
"""
import json
from pathlib import Path

# The game's 2.40:1 picture, or the "Aspect Ratio - <name>" patch that fills a screen of that shape.
ASPECTS = ('original', '16:9', '16:10', '21:9', '32:9')
ENGINE_FPS = (60, 120, 240)
DEFAULTS = {'aspect_ratio': 'original', 'engine_fps': '120', 'deferred_readback': '1',
            'stale_readback': '1', 'async_shaders': '1'}
ASPECT_PREFIX = 'Aspect Ratio'
FPS_PREFIX = 'Frame Rate Patch'
# Button-mash QTEs scale each press by the frame time: above 30 FPS they cannot be finished (#101).
QTE_FIX = 'QTE Fix - Button Mash at Any Frame Rate'
HEADER = '# port settings (in-game menu: Insert / R3+L2)'
# God of War III port keys older versions wrote (plus cheat_*); the writers drop them.
LEFTOVER_KEYS = ('red_orb_multiplier', 'green_orb_multiplier', 'blue_orb_multiplier', 'gold_orb_multiplier',
                 'damage_dealt', 'damage_taken', 'enemy_health_bar')


def read_ini(path):
    values = {}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return values
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def clean_lines(lines):
    """Settings file lines without the God of War III port's header and keys."""
    out = []
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key and (key in LEFTOVER_KEYS or key.startswith('cheat_')):
            continue
        out.append(HEADER if line.startswith('# gow3 settings') else line)
    return out


def write_keys(path, updates):
    """Rewrites `updates` in place and appends missing keys, keeping every other line."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = [HEADER]
    done, out = set(), []
    for line in clean_lines(lines):
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            out.append(f'{key}={updates[key]}')
            done.add(key)
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if k not in done]
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')


def startup_options(ini):
    """The startup options, normalized; unknown values fall back to the defaults."""
    options = {key: ini.get(key, value) for key, value in DEFAULTS.items()}
    if options['aspect_ratio'] not in ASPECTS:
        options['aspect_ratio'] = 'original'
    if options['engine_fps'] not in {str(f) for f in ENGINE_FPS}:
        options['engine_fps'] = '120'
    for key in ('deferred_readback', 'stale_readback', 'async_shaders'):
        options[key] = '0' if options[key] == '0' else '1'
    return options


def prepare(config, state_path):
    """(options, reverted): the startup options of this launch, restoring the last confirmed
    ones when the previous launch after a restart never confirmed its own."""
    ini = read_ini(config)
    state = {}
    try:
        state = json.loads(Path(state_path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        pass
    options = startup_options(ini)
    reverted = False
    if ini.get('restart_unconfirmed') == '1':
        if state.get('attempt') and state.get('last_good'):
            options = startup_options(state['last_good'])
            write_keys(config, {**options, 'restart_unconfirmed': '0'})
            state['attempt'] = False
            reverted = True
        else:
            state['attempt'] = True
    else:
        state = {'last_good': options, 'attempt': False}
    Path(state_path).write_text(json.dumps(state, indent=2), encoding='utf-8')
    return options, reverted


def patch_names(names, options):
    """The launcher's patch list with the aspect ratio and frame rate patches of `options`."""
    names = [n for n in names if not n.startswith(ASPECT_PREFIX) and not n.startswith(FPS_PREFIX)]
    if options['aspect_ratio'] != 'original':
        names.append(f'{ASPECT_PREFIX} - {options["aspect_ratio"]}')
    # A frame rate patch is named after the game's rate: half the engine's ("60 FPS" at 120).
    if options['engine_fps'] != '60':
        names.append(f'{FPS_PREFIX} - {int(options["engine_fps"]) // 2} FPS')
        if QTE_FIX not in names:
            names.append(QTE_FIX)
    return names


def restrict_to_available(names, available):
    """names without the patches this XML lacks. A missing frame rate patch is replaced by the
    highest one it has below the requested rate (the 240 FPS patch of the first XML has no
    counterpart in the second)."""
    kept = [n for n in names if n in available]
    for name in names:
        if name in available:
            continue
        print(f'Patches: {name} is not available for this game build: skipped', flush=True)
        if name.startswith(f'{FPS_PREFIX} - '):
            wanted = int(name.split(' - ')[1].split()[0])
            lower = [int(n.split(' - ')[1].split()[0]) for n in available if n.startswith(f'{FPS_PREFIX} - ')
                     and int(n.split(' - ')[1].split()[0]) <= wanted]
            if lower:
                kept.append(f'{FPS_PREFIX} - {max(lower)} FPS')
                print(f'Patches: using {kept[-1]} instead', flush=True)
    return kept


def applied_engine_fps(names):
    """The engine rate the applied patch list gives: 60 without a frame rate patch."""
    return 2 * max((int(n.split(' - ')[1].split()[0]) for n in names if n.startswith(f'{FPS_PREFIX} - ')), default=30)


def environment(options):
    return {'THEORDER_DEFERRED_READBACK': options['deferred_readback'],
            'THEORDER_STALE_READBACK': options['stale_readback'],
            'THEORDER_ASYNC_SHADERS': options['async_shaders']}
