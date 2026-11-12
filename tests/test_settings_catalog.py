"""Contract between scripts/settings_catalog.py, the C++ settings parser, the launcher and run.py:
a key added on one side only, a default that drifts or a launcher setting that no longer reaches the
game fails here. Also the generated recomplauncher.json and docs/ENV_VARS.md."""
import json
import os
import re
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import graphics_options  # noqa: E402
import port_launcher_win as launcher  # noqa: E402
import settings_catalog as catalog  # noqa: E402

CPP = (ROOT / 'gpu/shim/port_settings.cpp').read_text(encoding='utf-8')
HEADER = (ROOT / 'gpu/shim/port_settings.h').read_text(encoding='utf-8')
INI_ROWS = {r.key: r for r in catalog.rows('ini')}
APP_ROWS = {r.key: r for r in catalog.rows('settings')}
ORIGINAL_SETTINGS = ('display_mode', 'output_res', 'vsync', 'fps_limit', 'show_fps')


def cpp_read_keys():
    """Keys Set() parses: `key == "x"` plus the overlay_keys list."""
    keys = set(re.findall(r'\bkey == "([a-z0-9_]+)"', CPP))
    overlay = re.search(r'overlay_keys\[\] = \{(.*?)\};', CPP, re.S).group(1)
    return keys | set(re.findall(r'"([a-z0-9_]+)"', overlay))


def cpp_written_keys():
    """Keys Save() writes: `name=` at the start of a format string or after \\n."""
    save = CPP[CPP.index('void Save()'):CPP.index('namespace {\nstd::vector')]
    keys = set()
    for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', save):
        keys |= set(re.findall(r'(?:^|\\n)([a-z][a-z0-9_]*)=', literal))
    return keys


def cpp_env_overrides():
    """(variable, key) pairs of the environment table in Load(), plus THEORDER_UPSCALER."""
    table = CPP[CPP.index('env_keys[] = {'):CPP.index('for (const auto& [env, key] : env_keys)')]
    pairs = set(re.findall(r'\{"(THEORDER_[A-Z0-9_]+)",\s*"([a-z0-9_]+)"\}', table))
    assert 'getenv("THEORDER_UPSCALER")' in CPP
    return pairs | {('THEORDER_UPSCALER', 'upscaler')}


def header_defaults():
    """{field: literal} of the `std::atomic<T> name{literal}` initializers of Values."""
    out = {}
    for line in HEADER.splitlines():
        if 'std::atomic<' in line:
            for name, value in re.findall(r'(\w+)\{([^{}]*)\}', line):
                out[name] = value
    return out


def same_number(a, b):
    try:
        return float(a) == float(b)
    except ValueError:
        return False


class CppParserTest(unittest.TestCase):
    def test_every_key_the_parser_reads_or_writes_is_in_the_catalog_and_vice_versa(self):
        in_cpp = cpp_read_keys() | cpp_written_keys()
        self.assertGreater(len(in_cpp), 40)  # the extraction itself works
        self.assertEqual(sorted(in_cpp - set(INI_ROWS)), [], 'ini key in port_settings.cpp but not in the catalog')
        self.assertEqual(sorted(set(INI_ROWS) - in_cpp), [], 'catalog ini key port_settings.cpp does not use')

    def test_reads_and_writes_agree(self):
        # restart_unconfirmed and fsr1 are written and read; nothing is read-only or write-only.
        self.assertEqual(sorted(cpp_read_keys() ^ cpp_written_keys()), [])

    def test_environment_overrides_match(self):
        in_catalog = {(name, r.key) for r in INI_ROWS.values() if r.env_role == 'override' for name in r.env}
        self.assertEqual(sorted(cpp_env_overrides() ^ in_catalog), [])
        self.assertIn('getenv("THEORDER_ENGINE_FPS")', CPP)  # the 'derived' variable

    def test_catalog_defaults_are_the_games_initializers(self):
        literals = header_defaults()
        checked = 0
        for key, row in INI_ROWS.items():
            literal = literals.get(key)
            if literal is None:
                continue
            literal = literal.rstrip('f')
            if literal in ('true', 'false'):
                literal = '1' if literal == 'true' else '0'
            if not re.fullmatch(r'-?[0-9.]+', literal) or not re.fullmatch(r'-?[0-9.]+', row.default):
                continue  # enum rows keep an index in the header (checked by name in the test below)
            with self.subTest(key=key):
                self.assertTrue(same_number(literal, row.default), f'{key}: header {literal}, catalog {row.default}')
            checked += 1
        self.assertGreater(checked, 25)

    def test_indirect_defaults_spelled_out(self):
        self.assertIn('upscaler{UpscalerFsr3}', HEADER)
        self.assertIn('preset{NativeAA}', HEADER)
        self.assertEqual((INI_ROWS['upscaler'].default, INI_ROWS['preset'].default), ('fsr3', '0'))
        self.assertIn('OutputDefault = 1;', HEADER)  # 1920x1080
        self.assertIn('overlay_items{7}', HEADER)  # fps, frametime, upscaler
        self.assertEqual([INI_ROWS[f'overlay_{n}'].default for n in
                          ('fps', 'frametime', 'upscaler', 'gpu', 'cpu', 'ram', 'vram')],
                         ['1', '1', '1', '0', '0', '0', '0'])

    def test_ranges_are_the_clamps_in_the_parser(self):
        for key, lo, hi in (('overlay_scale', 50, 300), ('overlay_opacity', 0, 100), ('mouse_sensitivity', 10, 400),
                            ('fov', 50, 200), ('master_volume', 0, 200)):
            self.assertEqual(INI_ROWS[key].range, (lo, hi))
            self.assertRegex(CPP, rf'{lo}, {hi}\)')


class DefaultsTest(unittest.TestCase):
    def test_launcher_defaults_are_the_catalogs(self):
        for key, row in INI_ROWS.items():
            self.assertEqual(row.launcher_default, launcher.INI_DEFAULTS.get(key), key)
        self.assertEqual(sorted(set(launcher.INI_DEFAULTS) - set(INI_ROWS)), [])
        for key, row in APP_ROWS.items():
            self.assertIn(key, launcher.APP_DEFAULTS)
            if row.default is not catalog.DYNAMIC:
                self.assertEqual(row.default, launcher.APP_DEFAULTS[key], key)

    def test_every_settings_json_key_is_classified(self):
        known = set(APP_ROWS) | set(catalog.LAUNCHER_ONLY)
        self.assertEqual(sorted(set(launcher.APP_DEFAULTS) - known), [], 'launcher key missing from the catalog')
        self.assertEqual(sorted(known - set(launcher.APP_DEFAULTS)), [], 'catalog key the launcher no longer has')
        self.assertEqual(set(APP_ROWS) & set(catalog.LAUNCHER_ONLY), set())

    def test_startup_option_defaults_match_run_py(self):
        for key, value in graphics_options.DEFAULTS.items():
            self.assertEqual(INI_ROWS[key].default, value, key)
            self.assertEqual(INI_ROWS[key].launcher_default, value, key)
            self.assertEqual(INI_ROWS[key].readers.count('run.py'), 1, key)
        run_py_keys = set(graphics_options.DEFAULTS) | {'restart_unconfirmed'}
        self.assertEqual({k for k, r in INI_ROWS.items() if 'run.py' in r.readers}, run_py_keys)
        self.assertEqual({k for k, r in INI_ROWS.items() if 'launcher' in r.readers}, set(launcher.INI_DEFAULTS))

    def test_disagreements_are_recorded_and_only_those(self):
        found = {k for k, r in INI_ROWS.items()
                 if r.launcher_default is not None and r.launcher_default != r.default}
        recorded = {k for k, r in INI_ROWS.items() if r.disagreement}
        self.assertEqual(sorted(found), sorted(recorded))
        self.assertEqual(sorted(found), ['frames_queued', 'live_resolution', 'preset', 'show_fps', 'upscaler'])

    def test_defaults_and_choices_are_values_the_game_parses(self):
        for key, row in INI_ROWS.items():
            if row.allowed:
                self.assertIn(row.default, row.allowed, key)
            if row.launcher_default is not None and row.allowed:
                self.assertIn(row.launcher_default, row.allowed, key)
        offered = {'upscaler': [v for v, _ in launcher.UPSCALERS], 'display_mode': [v for v, _ in launcher.DISPLAY_MODES],
                   'fps_limit': [v for v, _ in launcher.FPS_LIMITS], 'audio_channels': [v for v, _ in launcher.AUDIO_CHANNELS],
                   'frame_generation': [v for v, _ in launcher.FRAME_GENERATION],
                   'frames_queued': [v for v, _ in launcher.FRAMES_QUEUED],
                   'output_res': [v for v, _ in launcher.OUTPUTS], 'aspect_ratio': [v for v, _ in launcher.ASPECT_CHOICES],
                   'engine_fps': [v for v, _ in launcher.ENGINE_FPS_CHOICES],
                   'anisotropy': [v for v, _ in launcher.ANISOTROPY_CHOICES],
                   'preset': [v for v, _ in launcher.PRESETS], 'live_resolution': [v for v, _ in launcher.LIVE],
                   'master_volume': [v for v, _ in launcher.MASTER_VOLUMES]}
        for key, values in offered.items():
            row = INI_ROWS[key]
            for value in values:
                if row.range:
                    self.assertTrue(row.range[0] <= int(value) <= row.range[1], (key, value))
                else:
                    self.assertIn(value, row.allowed, (key, value))
        for key, values in {'draw_pipe': launcher.DRAW_PIPE, 'readbacks': launcher.READBACKS, 'no_msaa': launcher.NO_MSAA,
                            'stream_memory': launcher.STREAM_MEMORY, 'video_backend': launcher.VIDEO_BACKENDS,
                            'pad_style': [(k, 0) for k in launcher.PAD_STYLES]}.items():
            self.assertEqual(sorted(v for v, _ in values), sorted(APP_ROWS[key].allowed), key)


class EnvironmentContractTest(unittest.TestCase):
    """For every settings.json row with an encoding, game_environment() holds the variable the
    catalog says, in the encoding the catalog says."""

    @staticmethod
    def samples(row):
        enc = row.encoding
        if enc in ('flag', 'flag_when_true'):
            return [True, False]
        if enc == 'enum_or_default':
            return list(row.allowed) + ['not-a-value']
        if enc == 'digits_when_set':
            return ['', '2', 'abc']
        if enc == 'stripped_when_set':
            return ['', '  Ana  ']
        if enc.startswith('path_or_default'):
            return ['', 'C:/custom/folder']
        if enc == 'string_when_set':
            return list(row.allowed) if row.allowed else ['', 'C:/update']
        if enc == 'string':
            return list(row.allowed) if row.allowed else ['C:/game dir', '17']
        return ['anything', True, False]  # constant

    def build(self, row, value):
        settings = {**launcher.APP_DEFAULTS, 'game_dir': 'game', row.key: value}
        with mock.patch.dict(os.environ, {}, clear=True), \
                mock.patch.object(launcher, 'WINDOWS', row.platform != 'linux'):
            return launcher.game_environment(settings)

    def test_every_env_row_reaches_the_game_with_the_cataloged_encoding(self):
        checked = 0
        for row in APP_ROWS.values():
            if not row.env or row.encoding == 'derived':
                continue
            for value in self.samples(row):
                with self.subTest(key=row.key, value=value):
                    env = self.build(row, value)
                    expected = catalog.env_expect(row, value, launcher.DATA_DIR)
                    for name in row.env:
                        if name in expected:
                            self.assertEqual(env.get(name), expected[name], name)
                        else:
                            self.assertNotIn(name, env, name)
                    checked += 1
        self.assertGreater(checked, 50)

    def test_derived_rows_reach_the_game(self):
        env = self.build(APP_ROWS['pad_style'], 'xbox')
        self.assertEqual((env['THEORDER_GLYPHS'], env['THEORDER_PAD_STYLE']), ('xbox', 'xbox'))
        controls = {'key': {'cross': 'Q'}, 'pad': {'cross': 'b'}}
        env = self.build(APP_ROWS['controls'], controls)
        self.assertEqual((env['THEORDER_KEY_MAP'], env['THEORDER_PAD_MAP']), ('cross=Q', 'cross=b'))
        env = self.build(APP_ROWS['controls'], {})
        self.assertNotIn('THEORDER_KEY_MAP', env)
        with mock.patch.object(launcher, 'game_profile_of', return_value=('CUSA1', ('ids', 'xml', '01.02', 'The Order 1886'))):
            env = self.build(APP_ROWS['game_patches'], {'CUSA1': ['A', 'B']})
        self.assertEqual((env['THEORDER_PATCHES'], env['THEORDER_PATCHES_ONLY']), ('A;B', '1'))

    def test_every_env_row_has_an_encoding(self):
        for row in APP_ROWS.values():
            self.assertEqual(bool(row.env), bool(row.encoding), row.key)

    def test_a_launcher_variable_not_in_the_catalog_fails(self):
        """The launcher's own THEORDER_* assignments are all cataloged (or set without a setting)."""
        source = (ROOT / 'launcher/port_launcher_win.py').read_text(encoding='utf-8')
        start = source.index('def game_environment(s):')
        body = source[start:source.index('\n\n\n', start)]
        assigned = set(re.findall(r"env\['((?:THEORDER|SDL)_[A-Z0-9_]+)'\]", body))
        assigned |= set(re.findall(r"'(THEORDER_[A-Z0-9_]+)'", body))
        in_catalog = {n for r in APP_ROWS.values() for n in r.env}
        # paths the launcher derives from its own folders, not from a setting
        unowned = {'THEORDER_MODS_CONFIG', 'THEORDER_PATCHES_CONFIG', 'THEORDER_FSR4_DIR',
                   'THEORDER_UPSCALER'}  # the last one is only removed, so the ini choice wins
        self.assertEqual(sorted(assigned - in_catalog - unowned), [])
        self.assertEqual(sorted(in_catalog - assigned), [])

    def test_ini_variables_run_py_sends_are_what_graphics_options_builds(self):
        sent = {n for r in INI_ROWS.values() if r.sent_by == 'run.py' for n in r.env}
        self.assertEqual(sent, set(graphics_options.environment(graphics_options.startup_options({}))))
        for key, row in INI_ROWS.items():
            if row.sent_by:
                options = graphics_options.startup_options({key: '0'})
                self.assertEqual(graphics_options.environment(options)[row.env[0]], '0', key)


class EnvironmentNamesTest(unittest.TestCase):
    def test_every_cataloged_variable_is_read_somewhere(self):
        found = catalog.scan_env_vars()
        for row in catalog.CATALOG:
            for name in row.env:
                if name.startswith('THEORDER_'):
                    self.assertIn(name, found, f'{row.key}: {name} appears nowhere in the sources')


class GeneratedFilesTest(unittest.TestCase):
    def test_recomplauncher_json_is_generated_and_deterministic(self):
        first = catalog.recomplauncher_text()
        self.assertEqual(first, catalog.recomplauncher_text())
        self.assertEqual(first.encode('utf-8'), catalog.recomplauncher_text().encode('utf-8'))
        on_disk = (ROOT / 'recomplauncher.json').read_bytes().replace(b'\r\n', b'\n')
        self.assertEqual(on_disk, first.encode('utf-8'), 'run: python scripts/settings_catalog.py --write-recomplauncher')

    def test_original_settings_keep_their_ids_and_values(self):
        settings = json.loads(catalog.recomplauncher_text())['settings']
        original = {
            'display_mode': (['windowed', 'borderless', 'fullscreen'], 'windowed'),
            'output_res': (['1280x720', '1920x1080', '2560x1440', '3840x2160'], '1920x1080'),
            'vsync': (['0', '1'], '1'),
            'fps_limit': (['0', '30', '60', '120', '240'], '0'),
            'show_fps': (['0', '1'], '1'),
        }
        self.assertEqual([s['id'] for s in settings[:5]], list(original))
        for setting in settings[:5]:
            values, default = original[setting['id']]
            self.assertEqual([c['set'][setting['id']] for c in setting['choices']], values)
            self.assertEqual(setting['defaults'], {setting['id']: default})
            self.assertEqual(setting['in'], 'graphics')

    def test_settings_are_enumerated_launcher_choices_the_game_accepts(self):
        for setting in json.loads(catalog.recomplauncher_text())['settings']:
            key = setting['id']
            row = INI_ROWS[key]
            self.assertGreaterEqual(len(setting['choices']), 2)  # schema: minItems 2
            for choice in setting['choices']:
                value = choice['set'][key]
                if row.range:
                    self.assertTrue(row.range[0] <= int(value) <= row.range[1])
                else:
                    self.assertIn(value, row.allowed)
            self.assertEqual(setting['defaults'][key], launcher.INI_DEFAULTS[key])
            self.assertIn(setting['defaults'][key], [c['set'][key] for c in setting['choices']])
            if key not in ORIGINAL_SETTINGS:  # the five original ones ship as they were (show_fps is disputed)
                self.assertEqual(row.default, row.launcher_default, f'{key}: disputed default must not be offered')

    def test_env_inventory_is_generated_and_lists_the_catalog(self):
        text = catalog.env_inventory_text()
        self.assertEqual(text, catalog.env_inventory_text())
        on_disk = (ROOT / 'docs/ENV_VARS.md').read_text(encoding='utf-8')
        # Not compared byte for byte: a PR that adds a variable need not regenerate the inventory.
        self.assertIn('GENERATED by', on_disk.splitlines()[0])
        for row in catalog.CATALOG:
            for name in row.env:
                if name.startswith('THEORDER_'):
                    self.assertIn(f'| `{name}` |', on_disk)


if __name__ == '__main__':
    unittest.main()
