#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepares the game image and starts the native probe (Windows and Linux; run.sh wraps it on Linux).

Environment variables (THEORDER_GAME_DIR, THEORDER_DATA_DIR,
THEORDER_PATCHES, THEORDER_MODS_* / THEORDER_MODS_*, THEORDER_USER_DIR, ...).
Extra arguments go to the game probe.
The in-game "Apply and restart" runs this script again through THEORDER_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

WINDOWS = sys.platform == 'win32'
EXE_SUFFIX = '.exe' if WINDOWS else ''
if WINDOWS:
    import ctypes

PORT = Path(__file__).resolve().parent


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    if not WINDOWS:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return
            except PermissionError:
                pass
            time.sleep(0.1)
        return
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    if not WINDOWS:
        return 0
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `theorder-probe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """THEORDER_ISA_REWRITE=1 or a list of sets (MOVBE,BMI1,LZCNT) forces the ISA rewrite table,
    0 disables it; otherwise the CPU decides."""
    policy = (get_env(env, 'ISA_REWRITE') or '').strip()
    if policy == '0':
        return False
    return bool(policy) or bool(missing)


def run_script(name, *args, capture=False, check=True):
    # Inside the packaged launcher executable (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    env = {**os.environ, 'PYTHONIOENCODING': 'utf-8'}
    if capture:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                encoding='utf-8', env=env, creationflags=no_console())
        if not check:
            return result.returncode, result.stdout
        if result.returncode:
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed launcher child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, env=env, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def launch_prefix(env):
    """Linux integrations chosen in the launcher: MangoHud is its Vulkan layer (an environment
    variable), GameMode a command that starts the game."""
    if WINDOWS:
        return []
    if get_env(env, 'MANGOHUD') == '1':
        env.setdefault('MANGOHUD', '1')
    if get_env(env, 'GAMEMODE') == '1':
        if gamemode := shutil.which('gamemoderun'):
            return [gamemode]
        print('GameMode: gamemoderun not found, starting without it', flush=True)
    return []


def get_env(env, key, default=None):
    """The environment variable THEORDER_<key>, or default."""
    return env.get(f'THEORDER_{key}', default)



def select_game_folder(env, port_root=None):
    """Prefer the user-configured game, then the neighboring CUSA00035.

    Never probe a developer's home directory. Preserve the caller's explicit
    path for the eventual missing-eboot error if neither candidate is usable.
    """
    root = PORT if port_root is None else Path(port_root)
    configured = get_env(env, 'GAME_DIR')
    fallback = root.parent / 'CUSA00035'
    for candidate in (configured, fallback):
        if candidate and (Path(candidate) / 'eboot.bin').is_file():
            return Path(candidate)
    return Path(configured) if configured else fallback

def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


PS4_TO_ENGINE_LANG = {
    0: 18, 1: 0, 2: 2, 3: 5, 4: 4, 5: 3, 6: 6, 7: 7, 8: 8,
    9: 19, 10: 21, 11: 20, 12: 12, 13: 10, 14: 13, 15: 11,
    16: 9, 17: 17, 18: 1, 19: 15, 20: 16
}

# Builds without the PT-BR (engine 17) assets; Portuguese plays as PT-PT (7) there.
NO_PT_BR_TITLES = ('CUSA00076', 'CUSA00070', 'CUSA00035')


def engine_language(ps4_lang, title_id):
    """The engine's language id for a PS4 one; PT-BR falls back to PT-PT on builds without it."""
    if ps4_lang == 17 and title_id in NO_PT_BR_TITLES:
        return 7
    return PS4_TO_ENGINE_LANG.get(ps4_lang, ps4_lang)


# Localization package the game opens at boot for an engine language (seen in the file log of
# CUSA00035 01.02: open(.../ps4/primary/3861c21fc34b7612/v.../<id>) -> not found). Without it the
# game logs "Encountered an error" and stays on a black screen before the menu.
LANGUAGE_PACKAGES = {7: '1cf8f1244dcc1dd4', 17: 'b0b4572d23e56916'}


def language_package_present(game, engine_lang):
    """False only when the language has a known package and the dump has no copy of it."""
    package = LANGUAGE_PACKAGES.get(engine_lang)
    if not package or not game:
        return True
    data = Path(game) / '_data'
    if not data.is_dir():
        return True  # not an unpacked dump: nothing to check
    for folder in ('primary', 'PRIMARY', 'gpu', 'GPU'):
        if any(data.glob(f'*/ps4/{folder}/*/*/{package}')):
            return True
    return False


def available_language(ps4_lang, engine_lang, game):
    """(ps4_lang, engine_lang) the dump can boot: PT-BR -> PT-PT -> English when packages lack."""
    if language_package_present(game, engine_lang):
        return ps4_lang, engine_lang
    if engine_lang == 17 and language_package_present(game, 7):
        return ps4_lang, 7
    return 1, PS4_TO_ENGINE_LANG[1]


LANG_CODE_TO_PS4 = {
    'ja': 0, 'japanese': 0,
    'en': 1, 'en-us': 1, 'us': 1, 'english': 1,
    'fr': 2, 'french': 2,
    'es': 3, 'es-es': 3, 'spanish': 3,
    'de': 4, 'german': 4,
    'it': 5, 'italian': 5,
    'nl': 6, 'dutch': 6,
    'pt-pt': 7,
    'ru': 8, 'russian': 8,
    'ko': 9, 'korean': 9,
    'zh-hant': 10, 'zh-hans': 11,
    'fi': 12, 'finnish': 12,
    'sv': 13, 'swedish': 13,
    'da': 14, 'danish': 14,
    'no': 15, 'norwegian': 15,
    'pl': 16, 'polish': 16,
    'pt': 17, 'pt-br': 17, 'br': 17, 'portuguese': 17,
    'en-gb': 18, 'uk': 18,
    'tr': 19, 'turkish': 19,
    'es-419': 20, 'es-la': 20, 'la': 20,
    'ar': 21, 'arabic': 21
}


def resolve_language(cli_lang, env, ini_path, graphics_options=None):
    for candidate in (cli_lang, get_env(env, 'LANGUAGE')):
        if candidate is not None:
            val = str(candidate).strip().lower()
            if val in LANG_CODE_TO_PS4:
                return LANG_CODE_TO_PS4[val]
            if val.isdigit():
                return int(val)
    if graphics_options:
        ini_vals = graphics_options.read_ini(ini_path)
        if 'language' in ini_vals:
            val = str(ini_vals['language']).strip().lower()
            if val in LANG_CODE_TO_PS4:
                return LANG_CODE_TO_PS4[val]
            if val.isdigit():
                return int(val)
    try:
        # Portable launcher settings: check local folder first
        for s_candidate in (PORT / 'settings.json', PORT / 'user' / 'settings.json'):
            if s_candidate.is_file():
                s_data = json.loads(s_candidate.read_text(encoding='utf-8'))
                if 'language' in s_data:
                    val = str(s_data['language']).strip().lower()
                    if val in LANG_CODE_TO_PS4:
                        return LANG_CODE_TO_PS4[val]
                    if val.isdigit():
                        return int(val)
        # Legacy fallback if migration hasn't happened yet
        appdata = os.environ.get('APPDATA')
        if appdata:
            for s_name in ('theorder1886-launcher', 'order1886-launcher'):
                s_file = Path(appdata) / s_name / 'settings.json'
                if s_file.is_file():
                    s_data = json.loads(s_file.read_text(encoding='utf-8'))
                    if 'language' in s_data:
                        val = str(s_data['language']).strip().lower()
                        if val in LANG_CODE_TO_PS4:
                            return LANG_CODE_TO_PS4[val]
                        if val.isdigit():
                            return int(val)
    except Exception:
        pass
    try:
        import locale
        loc = (locale.getdefaultlocale()[0] or '').lower()
        if any(p in loc for p in ('pt', 'brazil', 'portuguese')):
            return 17
        if any(p in loc for p in ('es', 'spanish')):
            return 20
        if any(p in loc for p in ('fr', 'french')):
            return 2
        if any(p in loc for p in ('de', 'german')):
            return 4
        if any(p in loc for p in ('it', 'italian')):
            return 5
        if any(p in loc for p in ('ru', 'russian')):
            return 8
        if any(p in loc for p in ('pl', 'polish')):
            return 16
    except Exception:
        pass
    return 1


def find_probe_executable():
    """Finds the probe executable (theorder-probe)."""
    return find_executable('theorder-probe' + EXE_SUFFIX)


def configure_windows_runtime(probe, env):
    """Keep development probes on their matching toolchain DLL search path."""
    development = probe.resolve().is_relative_to((PORT / 'out').resolve())
    if WINDOWS and (development or not (probe.parent / 'SDL3.dll').is_file()):
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    cli_lang = None
    cli_costume = None
    translated_args = []
    i = 0
    while i < len(args):
        if args[i] in ('--startlevel', '--level') and i + 1 < len(args):
            translated_args.extend(['--guest-arg', '-startlevel', '--guest-arg', args[i + 1]])
            i += 2
        elif args[i] in ('--startcheckpoint', '--checkpoint') and i + 1 < len(args):
            translated_args.extend(['--guest-arg', '-startcheckpoint', '--guest-arg', args[i + 1]])
            i += 2
        elif args[i] in ('--no-cloth', '--disablecloth'):
            translated_args.extend(['--guest-arg', '-disablecloth'])
            i += 1
        elif args[i] in ('--no-cam-springs', '--nocamsprings'):
            translated_args.extend(['--guest-arg', '-nocamsprings'])
            i += 1
        elif args[i] in ('--no-cam-additive', '--nocamadditive'):
            translated_args.extend(['--guest-arg', '-nocamadditive'])
            i += 1
        elif args[i] == '--msaa' and i + 1 < len(args):
            translated_args.extend(['--guest-arg', '-msaa', '--guest-arg', args[i + 1]])
            i += 2
        elif args[i] == '--frontend':
            translated_args.extend(['--guest-arg', '-frontend'])
            i += 1
        elif args[i] == '--perf':
            translated_args.extend(['--guest-arg', '-disablecloth', '--guest-arg', '-nocamsprings', '--guest-arg', '-nocamadditive', '--guest-arg', '-msaa', '--guest-arg', '1'])
            i += 1
        elif args[i] in ('--language', '--lang', '-l') and i + 1 < len(args):
            cli_lang = args[i + 1]
            i += 2
        elif args[i] in ('--costume', '--dlc-costume') and i + 1 < len(args):
            cli_costume = args[i + 1]
            i += 2
        else:
            translated_args.append(args[i])
            i += 1
    args = translated_args
    # As run.sh: relative paths (THEORDER_GAME_DIR, etc.) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(get_env(env, 'DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    config_default = data / 'theorder.ini'
    # The settings of releases up to 0.2.0 (gow3.ini) move to theorder.ini once.
    if not config_default.exists() and (data / 'gow3.ini').is_file():
        try:
            (data / 'gow3.ini').replace(config_default)
        except OSError:
            config_default = data / 'gow3.ini'
    env.setdefault('THEORDER_CONFIG', get_env(env, 'CONFIG', str(config_default)))
    # Values this script derived on the previous start ("Apply and restart" inherits them) are
    # derived again from the new options.
    for key in env.pop('THEORDER_DERIVED', '').split(','):
        env.pop(key, None)
    derived = []
    sys.path.insert(0, str(PORT / 'scripts'))
    import graphics_options
    graphics, reverted = graphics_options.prepare(env['THEORDER_CONFIG'], out / 'graphics_state.json')
    if reverted:
        print('Graphics: the last restart never reached the game; previous options restored', flush=True)
    env.update(graphics_options.environment(graphics))
    if not get_env(env, 'FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['THEORDER_FSR411_DIR'] = str(data / 'fsr4_411')

    probe_env = get_env(env, 'PROBE')
    probe = Path(probe_env) if probe_env else find_probe_executable()
    if not probe or not probe.is_file():
        fail(f'theorder-probe{EXE_SUFFIX} / theorder-probe{EXE_SUFFIX} not found: build it with build.sh or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    configure_windows_runtime(probe, env)
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              text=True, creationflags=no_console())
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    if features.returncode:
        fail(f'Cannot start the native probe (exit 0x{features.returncode & 0xffffffff:08X}); '
             'check that its DLLs match the executable and the CLANG64 toolchain.')
    missing = parse_cpu_features(features.stdout)

    game = select_game_folder(env)
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set THEORDER_GAME_DIR).')
    update_view = None
    patch_dir = get_env(env, 'PATCH_DIR')
    if patch_dir:
        import game_update
        try:
            update_view = game_update.create_view(game, patch_dir, out)
        except (ValueError, OSError) as error:
            fail(str(error))
        game = update_view
        print(f'Game and update combined from {patch_dir}', flush=True)
    original_game = game.resolve()
    game = Path(run_script('mods.py', game, '--out', out,
                           '--mods-dir', get_env(env, 'MODS_DIR', str(data / 'mods')),
                           '--config', get_env(env, 'MODS_CONFIG', str(data / 'mods.json')),
                           '--enabled', get_env(env, 'MODS_ENABLED', '1'), capture=True).strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out)
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            code, output = run_script('isa_rewrite.py', out / 'boot-linked.bin', '--out', out,
                                      capture=True, check=False)
            print(output, end='', flush=True)
            if code == 0:
                isa_args = ['--isa-rewrite', str(out / 'isa-rewrite.bin')]
        run_script('content_profile.py', game, '--out', out, '--sku', get_env(env, 'CONTENT_SKU', 'full'))

        import patches
        from patches import game_profile_for, patch_requirements, patch_status, selected_patches
        profile = game_profile_for(game)
        engine_fps, fps_patched = 60, False
        # The window shows the game's icon (BMP: the format SDL loads without extra libraries); on
        # Linux the launcher's own icon (the AppImage ships the one chosen when it was built) and
        # an application id the desktop entry matches, so the shell shows the same icon.
        icon_file = game / 'sce_sys' / 'icon0.png'
        if not WINDOWS:
            env.setdefault('SDL_APP_ID', 'theorder')
            if (PORT / 'launcher' / 'theorder.png').is_file():
                icon_file = PORT / 'launcher' / 'theorder.png'
        try:
            from PIL import Image
            with Image.open(icon_file) as icon:
                icon.convert('RGBA').resize((64, 64), Image.LANCZOS).save(out / 'window_icon.bmp')
            env.setdefault('THEORDER_WINDOW_ICON', str(out / 'window_icon.bmp'))
        except (ImportError, OSError):
            pass
        # Linear image readbacks fix textures; preserve upscaler setting from theorder.ini
        env.setdefault('THEORDER_READBACK_LINEAR', '0')
        env.setdefault('THEORDER_FACE_DEPTH_FIX', '1')
        env.setdefault('THEORDER_FACE_SECONDARY_FIX', '1')
        env.setdefault('THEORDER_PARTICLE_SRT_FIX', '1')
        env.setdefault('THEORDER_EVSM_HTILE', '1')
        patches_val = get_env(env, 'PATCHES', '')
        patches_only = get_env(env, 'PATCHES_ONLY') == '1'
        if profile:
            names = selected_patches(profile[1], profile[2], patches_val, patches_only)
            names = graphics_options.patch_names(names, graphics)
            names = graphics_options.restrict_to_available(names, patches.available_patches(profile[1], profile[2]))
            env['THEORDER_PATCHES'] = ';'.join(names)
            env['THEORDER_PATCHES_ONLY'] = '1'
        (out / 'patch_status.json').unlink(missing_ok=True)
        run_script('patches.py', '--out', out, '--extra', get_env(env, 'PATCHES', ''), '--game-dir', game,
                   '--patches-dir', get_env(env, 'PATCHES_DIR', str(data / 'patches')),
                   '--patches-config', get_env(env, 'PATCHES_CONFIG', str(data / 'patches.json')))
        if profile:
            # Only the patches found in this eboot.bin count: a frame rate patch that was skipped
            # leaves the engine at 60 FPS, and the notes' memory and VBlank needs follow the same list.
            status = patch_status(out) or {}
            applied = status.get('applied', [])
            # The in-game menu's field of view writes these constants (probe.c set_fov_sites).
            env['THEORDER_FOV_SITES'] = ' '.join(hex(site) for site in status.get('fov_sites', []))
            engine_fps = graphics_options.applied_engine_fps(applied)
            fps_patched = any(n.startswith(f'{graphics_options.FPS_PREFIX} - ') for n in applied)
            dmem, vblank = patch_requirements(profile[1], applied, profile[2])
            # The Order presents at 30 Hz (flip rate 1) and steps 33.3 ms per frame. Its 60 FPS patch
            # halves the step, so the presentation has to follow (every vblank, at most 60 FPS) or
            # the game runs in slow motion. Derived like the values below, so a restart back to 30 FPS
            # drops them (flip rate 0 without the patch doubles the game's speed).
            if profile[3] == 'The Order 1886':
                applied_fps = 120 if f'{graphics_options.FPS_PREFIX} - 120 FPS' in applied else (60 if f'{graphics_options.FPS_PREFIX} - 60 FPS' in applied else None)
                if applied_fps:
                    for key, value in (('FLIP_RATE', '0'), ('FPS_LIMIT', str(applied_fps))):
                        if not get_env(env, key):
                            env[f'THEORDER_{key}'] = value
                            derived.append(f'THEORDER_{key}')
                    print(f'The Order {applied_fps} FPS patch: presenting every vblank, at most {applied_fps} FPS', flush=True)
            for key, value in (('DMEM_MB', dmem), ('VBLANK_HZ', vblank)):
                if value and not get_env(env, key):
                    env[f'THEORDER_{key}'] = str(value)
                    derived.append(f'THEORDER_{key}')
        if not get_env(env, 'VBLANK_HZ'):
            env['THEORDER_VBLANK_HZ'] = '60'
            derived.append('THEORDER_VBLANK_HZ')
        # Without the frame rate patch the engine steps 1/60 s per frame: the present thread must
        # not flip faster, or the game speeds up.
        env['THEORDER_ENGINE_FPS'] = str(engine_fps)
        derived.append('THEORDER_ENGINE_FPS')
        # The Order runs at half its engine setting: 30 FPS at 60, 60 with the frame rate patch.
        print(f'Game frame rate: {engine_fps // 2} FPS'
              + ('' if fps_patched or graphics['engine_fps'] == '60' else
                 ' (the frame rate patch was not found in this eboot.bin)'), flush=True)
        env['THEORDER_DERIVED'] = ','.join(derived)
        # The guest memory pool is committed in full at start: a resolution patch above 1080p
        # can need more than 16 GB of RAM, and the game then closes when the page file cannot grow.
        import guest_memory
        if warning := guest_memory.memory_warning(get_env(env, 'DMEM_MB'), guest_memory.physical_memory_mb(),
                                                  get_env(env, 'STREAM_MEMORY', 'cached')):
            print(warning, flush=True)

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(get_env(env, 'USER_DIR', str(data / 'user'))).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('THEORDER_GPU_USER_DIR', str(user_dir))
        from save_backup import backup
        try:
            if made := backup(user_dir):
                print(f'Saves backed up to {made}', flush=True)
        except OSError as error:
            print(f'Save backup failed, starting anyway: {error}', flush=True)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        # Windows: the probe starts the new launch and ends, so it waits for this process. Linux:
        # the probe replaces itself with the new launch while this process still waits for it.
        after = ['--after', str(os.getpid())] if WINDOWS else []
        restart = [sys.executable, *this, *after, *args]
        restart_cmd = subprocess.list2cmdline(restart) if WINDOWS else shlex.join(restart)
        env['THEORDER_RESTART_COMMAND'] = restart_cmd
        # Resolve language & pass guest arguments
        ps4_lang = resolve_language(cli_lang, env, env['THEORDER_CONFIG'], graphics_options)
        # profile[1] is the patch XML, not the title: read the title ID from param.sfo.
        title_id = patches.game_title_id(game) or ''
        engine_lang = engine_language(ps4_lang, title_id)
        if ps4_lang == 17 and engine_lang == 7:
            print(f'Language: Title {title_id} lacks native PT-BR assets, mapping Portuguese to engine id 7 (PT-PT)', flush=True)
        fallback_lang, fallback_engine = available_language(ps4_lang, engine_lang, game)
        if fallback_engine != engine_lang:
            print(f'Language: this dump has no package for engine language {engine_lang} '
                  f'({LANGUAGE_PACKAGES.get(engine_lang)}); the game would stay on a black screen, '
                  f'using engine language {fallback_engine} instead', flush=True)
            ps4_lang, engine_lang = fallback_lang, fallback_engine
        env['THEORDER_LANGUAGE'] = str(ps4_lang)
        print(f'Language: PS4 id {ps4_lang} -> engine id {engine_lang}', flush=True)

        if 'THEORDER_GLYPHS' not in env:
            for s_c in (PORT / 'settings.json', PORT / 'user' / 'settings.json'):
                if s_c.is_file():
                    try:
                        s_obj = json.loads(s_c.read_text(encoding='utf-8'))
                        if 'pad_style' in s_obj:
                            env['THEORDER_GLYPHS'] = s_obj['pad_style']
                            break
                    except Exception:
                        pass

        guest_lang_args = []
        if '-language' not in args:
            guest_lang_args.extend(['--guest-arg', '-language', '--guest-arg', str(engine_lang)])
        if '-enablesubtitles' not in args:
            guest_lang_args.extend(['--guest-arg', '-enablesubtitles'])
        if '-usedlc' not in args:
            guest_lang_args.extend(['--guest-arg', '-usedlc'])

        timeout_val = get_env(env, 'TIMEOUT', '0')
        command = [*launch_prefix(env), str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', timeout_val, *isa_args, *guest_lang_args, *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)
        if update_view:
            game_update.remove_view(update_view)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
