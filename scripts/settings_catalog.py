#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Catalog of the player-facing settings: one declarative table per place a setting lives.

  * theorder.ini keys: every key gpu/shim/port_settings.cpp reads or writes (the in-game menu
    and the launcher edit them);
  * settings.json keys of the launcher that reach the game (as THEORDER_* variables, SDL
    variables or run.py behaviour).

Each row says: key, type, default, allowed values, which variable the setting becomes, which
component reads it and what it does. tests/test_settings_catalog.py compares the table with the
C++ parser, with the launcher's defaults and with the environment the launcher builds, so a key
added on one side only fails the test.

Two defaults per ini row: `default` is what the game uses when the key is absent
(gpu/shim/port_settings.h), `launcher_default` is what the launcher writes/shows
(INI_DEFAULTS in launcher/port_launcher_win.py; None = the launcher has no control for it).
Where they differ the row carries a `disagreement` note: that is a recorded fact, not a choice.

Generators (deterministic):
  python scripts/settings_catalog.py --write-recomplauncher   -> recomplauncher.json
  python scripts/settings_catalog.py --write-env-inventory    -> docs/ENV_VARS.md
Not a runtime module: nothing in the game or the launcher imports it.
"""
import argparse
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent

BOOL = ('0', '1')
DYNAMIC = object()  # a default computed at run time (documented in the effect text)


class Setting(NamedTuple):
    key: str
    store: str                    # 'ini' (theorder.ini) or 'settings' (launcher settings.json)
    type: str                     # bool, int, float, enum, string, path, map
    default: object               # ini: string as the game parses it; settings: the launcher's value
    allowed: Tuple = ()           # the accepted values (enum/bool); empty = free
    range: Optional[Tuple] = None  # (lo, hi): values outside are clamped (or reset, see effect)
    env: Tuple = ()               # THEORDER_* / SDL_* names the setting becomes or is overridden by
    env_role: str = ''            # ini: 'override' (the game lets the variable win over the file)
                                  #      or 'derived' (run.py computes it); settings: 'launcher'
    encoding: str = ''            # settings rows: how the launcher encodes it (see env_expect)
    readers: Tuple = ('game',)    # game, launcher, run.py
    effect: str = ''
    launcher_default: object = None  # ini rows: INI_DEFAULTS value; None = no launcher control
    disagreement: str = ''        # ini rows: why default != launcher_default
    platform: str = 'any'         # 'linux' = only built into the environment off Windows
    sent_by: str = ''             # ini rows: 'run.py' when run.py puts the variable in the game's environment


def ini(key, type, default, effect, *, allowed=(), range=None, env=(), env_role='', readers=('game',),
        launcher_default=None, disagreement='', sent_by=''):
    if type == 'bool':
        allowed = BOOL
    if env and not env_role:
        env_role = 'override'
    return Setting(key, 'ini', type, default, tuple(allowed), range, tuple(env), env_role, '', tuple(readers),
                   effect, launcher_default, disagreement, 'any', sent_by)


def app(key, type, default, effect, *, allowed=(), env=(), encoding='', readers=('launcher',), platform='any'):
    return Setting(key, 'settings', type, default, tuple(allowed), None, tuple(env), 'launcher' if env else '',
                   encoding, tuple(readers), effect, None, '', platform, '')


L, R = 'launcher', 'run.py'
UPSCALERS = ('off', 'fsr3', 'fsr4', 'fsr411', 'taa', 'dlss')

# ---------------------------------------------------------------------------------------------
# theorder.ini. Order: the file's own (Save() in gpu/shim/port_settings.cpp).
INI = [
    ini('upscaler', 'enum', 'fsr3', 'Upscaler of the game image; FSR 4 / DLSS fall back to FSR 3.1 when the GPU lacks them.',
        allowed=UPSCALERS, env=('THEORDER_UPSCALER',), readers=('game', L), launcher_default='off',
        disagreement='game default fsr3; the launcher shows and writes off'),
    ini('preset', 'int', '0', 'Upscaler quality preset index: 0 Native AA, 1 Quality (x1.5), 2 Balanced (x1.7), '
        '3 Performance (x2), 4 Ultra Performance (x3).',
        range=(0, 4), env=('THEORDER_UPSCALE_PRESET',), readers=('game', L), launcher_default='1',
        disagreement='game default 0 (Native AA); the launcher shows and writes 1 (Quality)'),
    ini('sharpen', 'bool', '1', 'Sharpening pass of the upscaler (in-game menu only).'),
    ini('sharpness', 'float', '0.3', 'Upscaler sharpness, 0 to 2.', range=(0.0, 2.0), env=('THEORDER_FSR_SHARPNESS',)),
    ini('jitter', 'bool', '0', 'Camera jitter for the upscaler.', env=('THEORDER_JITTER',)),
    ini('reactive', 'bool', '0', 'Reactive mask for the upscaler.', env=('THEORDER_REACTIVE',)),
    ini('object_motion', 'bool', '1', 'Per-object motion vectors for the upscaler.', env=('THEORDER_OBJECT_MOTION',),
        readers=('game', L), launcher_default='1'),
    ini('reactive_scale', 'float', '1.0', 'Reactive mask scale, 0 to 16.', range=(0.0, 16.0),
        env=('THEORDER_REACTIVE_SCALE',)),
    ini('reactive_threshold', 'float', '0.2', 'Reactive mask threshold, 0 to 1.', range=(0.0, 1.0),
        env=('THEORDER_REACTIVE_THRESHOLD',)),
    ini('reactive_max', 'float', '0.9', 'Reactive mask ceiling, 0 to 1.', range=(0.0, 1.0),
        env=('THEORDER_REACTIVE_MAX',)),
    ini('debug_view', 'int', '0', 'Upscaler debug view: 0 none, 1 reactive mask, 2 motion vectors.', range=(0, 2)),
    ini('show_fps', 'bool', '0', 'Shows the performance overlay.', readers=('game', L), launcher_default='1',
        disagreement='game default 0 (hidden); the launcher shows and writes 1 (on)'),
    ini('fsr4_auto_exposure', 'bool', '1', 'FSR 4: the provider computes exposure itself (in-game menu check).'),
    ini('fsr4_invert_jitter', 'bool', '0', 'FSR 4: invert the sign of the jitter given to the provider (in-game menu check).'),
    ini('output_res', 'enum', '1920x1080', 'Output resolution of the upscaler and the UI (PortSettings::OutputWidths); '
        'an unknown value keeps the default.',
        allowed=('1280x720', '1920x1080', '2560x1440', '3840x2160', '5120x1440', '1600x900', '3440x1440', '3840x1080'),
        readers=('game', L), launcher_default='1920x1080'),
    ini('overlay_fps', 'bool', '1', 'Overlay item: frame rate.'),
    ini('overlay_frametime', 'bool', '1', 'Overlay item: frame time.'),
    ini('overlay_upscaler', 'bool', '1', 'Overlay item: upscaler and preset.'),
    ini('overlay_gpu', 'bool', '0', 'Overlay item: GPU load.'),
    ini('overlay_cpu', 'bool', '0', 'Overlay item: CPU load.'),
    ini('overlay_ram', 'bool', '0', 'Overlay item: system memory.'),
    ini('overlay_vram', 'bool', '0', 'Overlay item: video memory.'),
    ini('overlay_scale', 'int', '100', 'Overlay font size in percent, 50 to 300.', range=(50, 300)),
    ini('overlay_corner', 'int', '1', 'Overlay corner: 0 top left, 1 top right, 2 bottom left, 3 bottom right '
        '(anything else: top right).', allowed=('0', '1', '2', '3')),
    ini('overlay_opacity', 'int', '50', 'Overlay background opacity in percent.', range=(0, 100)),
    ini('overlay_layout', 'int', '0', 'Overlay layout: 0 one line, 1 one item per line.', allowed=('0', '1')),
    ini('menu_tab', 'int', '0', 'Last tab of the in-game menu (0 to 4).', range=(0, 4)),
    ini('overlay_advanced', 'bool', '0', 'Overlay shows the advanced items.'),
    ini('display_mode', 'enum', 'windowed', 'Window mode; an unknown value is windowed.',
        allowed=('windowed', 'borderless', 'fullscreen'), readers=('game', L), launcher_default='windowed'),
    ini('vsync', 'bool', '1', 'Vertical sync of the present.', readers=('game', L), launcher_default='1'),
    ini('fps_limit', 'enum', '0', 'Present rate limit; 0 unlimited; an unlisted value is unlimited.',
        allowed=('30', '60', '120', '240', '0'), readers=('game', L), launcher_default='0'),
    ini('async_shaders', 'bool', '1', 'Compile shaders on worker threads instead of stalling the draw.',
        env=('THEORDER_ASYNC_SHADERS',), readers=('game', L, R), launcher_default='1', sent_by=R),
    ini('fsr1', 'bool', '0', 'Legacy: always read as off (the control left the launcher and the menu).',
        env=('THEORDER_FSR1',)),
    ini('rcas', 'bool', '1', 'RCAS sharpening when the image has the window size.', env=('THEORDER_RCAS',)),
    ini('rcas_strength', 'int', '75', 'RCAS strength in percent, 0 to 100; invalid or empty is 75.', range=(0, 100)),
    ini('frames_queued', 'enum', '2', 'Frames the GPU command thread may queue ahead; 0 unbounded; an unlisted value is 1.',
        allowed=('1', '2', '0'), env=('THEORDER_FRAMES_AHEAD',), readers=('game', L), launcher_default='1',
        disagreement='game default 2 (Values::frames_queued); the launcher shows and writes 1 (lowest latency)'),
    ini('aspect_ratio', 'enum', 'original', 'Aspect ratio: the game picture (2.40:1) or the "Aspect Ratio" game patch; '
        'startup option (run.py).', allowed=('original', '16:9', '16:10', '21:9', '32:9'),
        readers=('game', L, R), launcher_default='original'),
    ini('engine_fps', 'enum', '120', 'Engine frame rate ceiling: 60 is the original 30 FPS game, 120 the 60 FPS patch, '
        '240 the 120 FPS patch; startup option (run.py computes the applied rate).', allowed=('60', '120', '240'),
        env=('THEORDER_ENGINE_FPS',), env_role='derived', readers=('game', L, R), launcher_default='120'),
    ini('deferred_readback', 'bool', '1', 'Startup option: GPU readbacks complete on a later frame.',
        env=('THEORDER_DEFERRED_READBACK',), readers=('game', L, R), launcher_default='1', sent_by=R),
    ini('stale_readback', 'bool', '1', 'Startup option: a readback may return the previous frame\'s data.',
        env=('THEORDER_STALE_READBACK',), readers=('game', L, R), launcher_default='1', sent_by=R),
    ini('restart_unconfirmed', 'bool', '0', 'Set by "Apply and restart" until the new launch shows frames; '
        'run.py restores the last confirmed startup options when a launch never cleared it.', readers=('game', R)),
    ini('fov', 'int', '100', 'Field of view in percent of the game\'s (tangent of the half angle), live.', range=(50, 200)),
    ini('mouse_look', 'bool', '1', 'Mouse look on (F7 toggles it).'),
    ini('mouse_sensitivity', 'int', '100', 'Mouse speed in percent.', range=(10, 400)),
    ini('mouse_sensitivity_y', 'int', '100', 'Vertical mouse speed in percent.', range=(10, 400)),
    ini('mouse_invert_y', 'bool', '0', 'Invert the vertical mouse axis.'),
    ini('anisotropy', 'enum', '0', 'Forced anisotropic filtering on mipmapped textures; 0 as the game asks; other values '
        'round down to 2, 4, 8 or 16.', allowed=('0', '2', '4', '8', '16'), env=('THEORDER_ANISOTROPY',),
        readers=('game', L), launcher_default='0'),
    ini('ui_language', 'enum', 'auto', 'Language of the in-game menu (not the launcher\'s ui_language): auto follows '
        'Windows, else a code of PortUi::Languages (en, pt, es, fr, de, it, ...).', env=('THEORDER_UI_LANGUAGE',)),
    ini('frame_generation', 'enum', 'off', 'AMD FSR 3.1 frame generation 2x (experimental); applied on restart.',
        allowed=('off', 'fsr'), env=('THEORDER_FRAME_GENERATION',), readers=('game', L), launcher_default='off'),
    ini('pause_game', 'bool', '1', 'The game stops while the in-game menu is open.', readers=('game', L),
        launcher_default='1'),
    ini('master_volume', 'int', '100', 'Master volume in percent; above 100 boosts behind a soft limiter; '
        'a non-number is 100.', range=(0, 200), env=('THEORDER_AUDIO_VOLUME',), readers=('game', L),
        launcher_default='100'),
    ini('audio_channels', 'enum', 'auto', 'Speaker layout of the 7.1 audio, read when the game opens its audio ports '
        '(also accepts 2.0, 2, 6, 8).', allowed=('auto', 'stereo', '5.1', '7.1'), env=('THEORDER_AUDIO_CHANNELS',),
        readers=('game', L), launcher_default='auto'),
    ini('live_resolution', 'enum', '0', 'Live resolution/preset changes (run.sh): 0 off, auto by GPU, 1 on; on restart.',
        allowed=('0', 'auto', '1'), readers=('game', L), launcher_default='auto',
        disagreement='game default 0 (off); the launcher shows and writes auto (by graphics card)'),
]

# ---------------------------------------------------------------------------------------------
# Launcher settings.json keys that reach the game (game_environment() in launcher/port_launcher_win.py).
# `encoding`: string | string_when_set | stripped_when_set | flag | flag_when_true | enum_or_default
#             | digits_when_set | path_or_default:<subdir of the data folder> | constant:<value> | derived
LR = ('launcher', 'run.py')
APP = [
    app('game_dir', 'path', DYNAMIC, 'Folder of the extracted PS4 game. Default: THEORDER_GAME_DIR if set, else '
        '"CUSA00035" next to the port folder on Windows, empty elsewhere.', env=('THEORDER_GAME_DIR',),
        encoding='string', readers=LR),
    app('update_dir', 'path', '', 'Folder of the game update (a second dump); its eboot.bin wins when present.',
        env=('THEORDER_PATCH_DIR',), encoding='string_when_set', readers=LR),
    app('experimental_upscaler', 'bool', False, 'Legacy switch kept in the file; the launcher always sends 0.',
        env=('THEORDER_EXPERIMENTAL_UPSCALER',), encoding='constant:0'),
    app('video_backend', 'enum', 'auto', 'Linux only: SDL video driver. auto leaves SDL to choose.',
        allowed=('auto', 'wayland', 'x11'), env=('SDL_VIDEO_DRIVER', 'SDL_VIDEODRIVER'), encoding='enum_or_default',
        platform='linux'),
    app('gamemode', 'bool', False, 'Linux only: run the game under GameMode.', env=('THEORDER_GAMEMODE',),
        encoding='flag', platform='linux'),
    app('mangohud', 'bool', False, 'Linux only: run the game under MangoHud.', env=('THEORDER_MANGOHUD',),
        encoding='flag', platform='linux'),
    app('user_dir', 'path', '', 'Folder of the saves and per-user files; empty keeps user/ in the data folder.',
        env=('THEORDER_USER_DIR',), encoding='string_when_set', readers=LR),
    app('mods_dir', 'path', '', 'Folder of the file mods; empty means mods/ in the data folder.',
        env=('THEORDER_MODS_DIR',), encoding='path_or_default:mods'),
    app('mods_enabled', 'bool', True, 'File mods on or off.', env=('THEORDER_MODS_ENABLED',), encoding='flag'),
    app('patches_dir', 'path', '', 'Folder of the third-party patch files; empty means patches/ in the data folder.',
        env=('THEORDER_PATCHES_DIR',), encoding='path_or_default:patches'),
    app('language', 'enum', '1', 'PS4 system language id the game starts in (run.py falls back to English when the dump '
        'has no package for it; run.py also reads this key and theorder.ini language when started directly).',
        env=('THEORDER_LANGUAGE',), encoding='string', readers=LR),
    app('player_name', 'string', '', 'Player name the game reports; empty keeps the default.',
        env=('THEORDER_USER_NAME',), encoding='stripped_when_set'),
    app('hdr', 'bool', False, 'HDR output.', env=('THEORDER_HDR',), encoding='flag_when_true'),
    app('draw_pipe', 'enum', '', 'Draw pipeline threading: empty auto (8+ threads), 1 on, 0 off.',
        allowed=('', '1', '0'), env=('THEORDER_DRAW_PIPE',), encoding='string_when_set'),
    app('readbacks', 'enum', '', 'GPU readback mode: empty relaxed, 0 off, 2 precise.', allowed=('', '0', '2'),
        env=('THEORDER_READBACKS',), encoding='string_when_set'),
    app('no_msaa', 'enum', '', 'Multisampled images: empty automatic by driver, 1 off, 0 on.', allowed=('', '1', '0'),
        env=('THEORDER_NO_MSAA',), encoding='string_when_set'),
    app('stream_memory', 'enum', 'cached', 'Memory kind of the streaming buffers; an unknown value is cached.',
        allowed=('cached', 'host', 'device'), env=('THEORDER_STREAM_MEMORY',), encoding='enum_or_default'),
    app('gpu_id', 'string', '', 'Index of the GPU to use (digits); empty lets the game pick.',
        env=('THEORDER_GPU_ID',), encoding='digits_when_set'),
    app('background_warmup', 'bool', True, 'Pipeline warm-up in the background (also sets the hot warm-up).',
        env=('THEORDER_BACKGROUND_WARMUP', 'THEORDER_HOT_WARMUP'), encoding='flag'),
    app('parallel_warmup', 'bool', True, 'Pipeline warm-up on several threads.', env=('THEORDER_PARALLEL_WARMUP',),
        encoding='flag'),
    app('pipeline_binary_cache', 'bool', True, 'Driver pipeline binary cache.', env=('THEORDER_PIPELINE_BINARIES',),
        encoding='flag'),
    app('perf_diag', 'bool', False, 'Performance diagnostics in the log.', env=('THEORDER_PERF_DIAG',), encoding='flag'),
    app('frame_capture_key', 'bool', False, 'Frame capture on a key press.', env=('THEORDER_FRAME_CAPTURE_KEY',),
        encoding='flag_when_true'),
    app('frame_stats', 'bool', False, 'Frame statistics in the log.', env=('THEORDER_FRAME_STATS',),
        encoding='flag_when_true'),
    app('gpu_profile', 'bool', False, 'GPU timing profile.', env=('THEORDER_GPU_PROFILE',), encoding='flag_when_true'),
    app('vk_validation', 'bool', False, 'Vulkan validation layer.', env=('THEORDER_VK_VALIDATION',),
        encoding='flag_when_true'),
    app('pad_style', 'enum', 'playstation', 'Button names in the launcher and the glyph set the game shows '
        '(the launcher sends the same value twice; run.py reads settings.json when the variable is missing).',
        allowed=('playstation', 'xbox'), env=('THEORDER_GLYPHS', 'THEORDER_PAD_STYLE'), encoding='string', readers=LR),
    app('game_patches', 'map', {}, 'Patch names chosen per game title id (replaces the profile\'s list).',
        env=('THEORDER_PATCHES', 'THEORDER_PATCHES_ONLY'), encoding='derived', readers=LR),
    app('controls', 'map', {}, 'Remapped keys and gamepad buttons (only the changed ones).',
        env=('THEORDER_KEY_MAP', 'THEORDER_PAD_MAP'), encoding='derived'),
]

# settings.json keys that never reach the game (launcher behaviour only); listed so that a new
# APP_DEFAULTS key must be classified as one or the other.
LAUNCHER_ONLY = {
    'ui_language': 'language of the launcher window (not the in-game menu\'s ui_language of theorder.ini)',
    'desktop_integration': 'Linux menu entry and icon',
    'close_on_play': 'closes the launcher window when the game starts',
    'check_updates': 'update check at start (always on)',
    'last_version': 'version that last ran, for the settings migration',
}

CATALOG = INI + APP


def rows(store=None):
    return [r for r in CATALOG if store in (None, r.store)]


# ---------------------------------------------------------------------------------------------
# The environment the catalog expects the launcher to build.

def env_expect(row, value, data_dir=''):
    """{name: value} that game_environment() must hold for a settings.json row set to `value`;
    names the launcher must not set are absent from the result. 'derived' rows return None."""
    enc = row.encoding
    names = row.env
    if enc == 'derived':
        return None
    if enc == 'string':
        return {n: str(value) for n in names}
    if enc == 'string_when_set':
        return {n: str(value) for n in names} if value else {}
    if enc == 'stripped_when_set':
        return {n: str(value).strip() for n in names} if str(value).strip() else {}
    if enc == 'flag':
        return {n: '1' if value else '0' for n in names}
    if enc == 'flag_when_true':
        return {n: '1' for n in names} if value else {}
    if enc == 'enum_or_default':
        if row.platform == 'linux':  # only a named backend is exported
            return {n: value for n in names} if value in row.allowed and value != row.default else {}
        return {n: value if value in row.allowed else row.default for n in names}
    if enc == 'digits_when_set':
        return {n: str(value) for n in names} if str(value).isdigit() else {}
    if enc.startswith('path_or_default:'):
        return {n: value or str(Path(data_dir) / enc.split(':', 1)[1]) for n in names}
    if enc.startswith('constant:'):
        return {n: enc.split(':', 1)[1] for n in names}
    raise ValueError(f'{row.key}: unknown encoding {enc!r}')


# ---------------------------------------------------------------------------------------------
# recomplauncher.json. The first five settings are the original ones (ids, labels, values).
# Only enumerated ini keys whose game and launcher defaults agree are offered; `defaults` is the
# launcher's value (the same as INI_DEFAULTS, which tests/test_launcher_controls.py checks).
RECOMPLAUNCHER = (
    ('display_mode', 'Display mode', (('Windowed', 'windowed'), ('Borderless', 'borderless'),
                                      ('Fullscreen', 'fullscreen'))),
    ('output_res', 'Output resolution', (('720p', '1280x720'), ('1080p', '1920x1080'), ('1440p', '2560x1440'),
                                         ('4K', '3840x2160'))),
    ('vsync', 'VSync', (('Off', '0'), ('On', '1'))),
    ('fps_limit', 'Frame rate limit', (('Unlimited', '0'), ('30 fps', '30'), ('60 fps', '60'), ('120 fps', '120'),
                                       ('240 fps', '240'))),
    ('show_fps', 'Performance overlay', (('Off', '0'), ('On', '1'))),
    ('aspect_ratio', 'Aspect ratio', (('Original (2.40:1, black bars)', 'original'), ('16:9', '16:9'),
                                      ('16:10', '16:10'), ('21:9', '21:9'), ('32:9 (Super Ultrawide)', '32:9'))),
    ('engine_fps', 'Game frame rate', (('30 FPS (original)', '60'), ('60 FPS (default)', '120'),
                                       ('120 FPS (unlocked)', '240'))),
    ('anisotropy', 'Anisotropic filtering', (('Game default', '0'), ('2x', '2'), ('4x', '4'), ('8x', '8'),
                                             ('16x', '16'))),
    ('master_volume', 'Master volume', tuple((f'{v}%', str(v)) for v in (0, 25, 50, 75, 100, 125, 150, 175, 200))),
    ('audio_channels', 'Speakers', (("Automatic (the device's)", 'auto'), ('Stereo', 'stereo'), ('5.1', '5.1'),
                                    ('7.1', '7.1'))),
    ('pause_game', 'Pause while the menu is open', (('Off', '0'), ('On', '1'))),
)

RECOMPLAUNCHER_HEAD = {
    '$schema': 'https://recomplauncher.com/schema/recomplauncher.json',
    'format': 1,
    'name': 'The Order: 1886',
    'repo': 'mateussmeneses/theorder1886-port',
    'game': {'kind': 'folder',
             'label': 'Your extracted The Order: 1886 PS4 game folder (CUSA00035 v01.02 tested)',
             'check': {'file': 'eboot.bin'}},
    'start': {'program': ['The Order 1886.exe'], 'args': ['--play']},
    'prepare': {'program': ['The Order 1886.exe'], 'args': ['--recomplauncher-prepare', '{game}'],
                'label': 'Check the PS4 game folder', 'timeout': 30},
    'config': [{'id': 'launcher', 'format': 'json', 'file': [{'path': '{install}/settings.json'}],
                'game_path': 'game_dir'},
               {'id': 'graphics', 'format': 'ini', 'file': [{'path': '{install}/theorder.ini'}]}],
}
RECOMPLAUNCHER_TAIL = {'saves': ['{install}/user/savedata']}


def recomplauncher():
    by_key = {r.key: r for r in INI}
    settings = []
    for key, label, choices in RECOMPLAUNCHER:
        row = by_key[key]
        settings.append({
            'id': key, 'label': label, 'in': 'graphics',
            'choices': [{'label': text, 'set': {key: value}} for text, value in choices],
            'defaults': {key: row.launcher_default},
        })
    return {**RECOMPLAUNCHER_HEAD, 'settings': settings, **RECOMPLAUNCHER_TAIL}


def recomplauncher_text():
    return json.dumps(recomplauncher(), indent=2, ensure_ascii=False) + '\n'


# ---------------------------------------------------------------------------------------------
# docs/ENV_VARS.md: every THEORDER_* name found in the runtime sources.
ENV_SCAN = ('src', 'gpu', 'launcher', 'scripts', 'run.py', 'run.sh')
ENV_SKIP_DIRS = {'third_party', 'externals', '__pycache__', 'build', 'out'}
ENV_SUFFIXES = {'.c', '.cpp', '.h', '.hpp', '.inc', '.py', '.sh', '.ps1'}
NAME_RE = re.compile(r'THEORDER_[A-Z0-9_]*[A-Z0-9]')
GET_ENV_RE = re.compile(r"get_env\(\s*\w+\s*,\s*'([A-Z0-9_]+)'")
READ_RE = re.compile(r'getenv|GetEnv|get_env|environ|\.get\(|Env\w*\(|\$\{?THEORDER_')
COMMENT_LINE_RE = re.compile(r'^\s*(?://+|#|\*|/\*+)\s*(.*?)\s*(?:\*/)?$')
TRAILING_RE = re.compile(r'(?://|\s#)\s+(.+?)\s*$')


def _source_files():
    out = []
    for entry in ENV_SCAN:
        base = ROOT / entry
        if base.is_file():
            out.append(base)
        elif base.is_dir():
            for path in sorted(base.rglob('*')):
                parts = set(path.relative_to(ROOT).parts)
                if path.is_file() and path.suffix in ENV_SUFFIXES and not parts & ENV_SKIP_DIRS:
                    out.append(path)
    return [p for p in out if p.resolve() != Path(__file__).resolve()]


def scan_env_vars():
    """{name: {'read': [files], 'mentioned': [files], 'note': str}} over the runtime sources."""
    found = {}
    for path in _source_files():
        rel = path.relative_to(ROOT).as_posix()
        try:
            lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        except OSError:
            continue
        touches_env = any(READ_RE.search(line) for line in lines)
        for number, line in enumerate(lines):
            names = set(NAME_RE.findall(line)) | {f'THEORDER_{n}' for n in GET_ENV_RE.findall(line)}
            for name in names:
                entry = found.setdefault(name, {'read': [], 'mentioned': [], 'note': ''})
                is_read = touches_env  # the file reads the environment and names the variable
                bucket = entry['read' if is_read else 'mentioned']
                if rel not in bucket:
                    bucket.append(rel)
                if is_read and not entry['note']:
                    entry['note'] = _note(lines, number)
    return found


def _note(lines, number):
    previous = COMMENT_LINE_RE.match(lines[number - 1]) if number else None
    if previous and previous.group(1) and not lines[number - 1].lstrip().startswith(('#!', '#include', '#if', '#def', '#end', '#else', '#pragma', '#undef')):
        return previous.group(1)
    trailing = TRAILING_RE.search(lines[number])
    return trailing.group(1) if trailing else ''


def _cell(text, limit=110):
    text = text.replace('|', '/').replace('`', "'")
    return text if len(text) <= limit else text[:limit - 3].rstrip() + '...'


def _files_cell(entry):
    files = entry['read'] or entry['mentioned']
    shown = ', '.join(f'`{f}`' for f in files[:3])
    text = shown + (f' (+{len(files) - 3})' if len(files) > 3 else '')
    return text if entry['read'] else f'(only mentioned) {text}'


def env_inventory_text():
    found = scan_env_vars()
    catalogued = {}
    for row in CATALOG:
        for name in row.env:
            catalogued.setdefault(name, f'{row.store}:{row.key}')
    lines = [
        '<!-- GENERATED by `python scripts/settings_catalog.py --write-env-inventory`; do not edit by hand. -->',
        '# THEORDER_* environment variables',
        '',
        'Every `THEORDER_*` name found in `src/`, `gpu/` (without `third_party/`), `launcher/`, `scripts/`,',
        '`run.py` and `run.sh`. "Read in" lists the files that name the variable and read the environment (a',
        'file-level heuristic: `getenv`, `get_env`, `environ`, ...); a name only in headers or text says so. The note is',
        'the comment next to the first read, when there is one. "Catalog" names the player-facing setting in',
        '`scripts/settings_catalog.py` that sets or overrides the variable; the rest are diagnostics and',
        'switches with no catalog row. Names built at run time (`THEORDER_` plus a variable part) do not',
        'appear here.',
        '',
        f'{len(found)} names, {sum(1 for n in found if n in catalogued)} in the catalog.',
        '',
        '| Name | Read in | Catalog | Note |',
        '|---|---|---|---|',
    ]
    for name in sorted(found):
        entry = found[name]
        lines.append(f'| `{name}` | {_files_cell(entry)} | {catalogued.get(name, "")} | {_cell(entry["note"])} |')
    return '\n'.join(lines) + '\n'


def write_text(path, text):
    with open(path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--write-recomplauncher', action='store_true', help='regenerate recomplauncher.json')
    parser.add_argument('--write-env-inventory', action='store_true', help='regenerate docs/ENV_VARS.md')
    args = parser.parse_args(argv)
    if not (args.write_recomplauncher or args.write_env_inventory):
        parser.print_help()
        return 2
    if args.write_recomplauncher:
        write_text(ROOT / 'recomplauncher.json', recomplauncher_text())
        print('wrote recomplauncher.json')
    if args.write_env_inventory:
        write_text(ROOT / 'docs' / 'ENV_VARS.md', env_inventory_text())
        print('wrote docs/ENV_VARS.md')
    return 0


if __name__ == '__main__':
    sys.exit(main())
