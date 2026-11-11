from paths import ROOT
import json
import os
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(ROOT))
import run
import graphics_options


class RunLanguageTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        root = Path(self.temp_dir.name)
        self.port = root / 'port'
        self.port.mkdir()
        self.appdata = root / 'appdata'
        self.appdata.mkdir()
        self.ini = root / 'theorder.ini'
        self.ini.write_text('', encoding='utf-8')
        port_patch = patch.object(run, 'PORT', self.port)
        env_patch = patch.dict(os.environ, {'APPDATA': str(self.appdata)}, clear=True)
        locale_patch = patch('locale.getdefaultlocale', return_value=('en_US', 'UTF-8'))
        for active_patch in (port_patch, env_patch, locale_patch):
            active_patch.start()
            self.addCleanup(active_patch.stop)

    def save_language(self, path, language):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'language': language}), encoding='utf-8')

    def resolve(self, cli=None, env=None):
        return run.resolve_language(cli, env or {}, self.ini, graphics_options)

    def test_saved_language_from_every_settings_location(self):
        paths = (
            self.port / 'settings.json',
            self.port / 'user' / 'settings.json',
            self.appdata / 'theorder1886-launcher' / 'settings.json',
            self.appdata / 'order1886-launcher' / 'settings.json',
        )
        for path in paths:
            for language in (17, 'pt-br'):
                with self.subTest(path=path, language=language):
                    self.save_language(path, language)
                    try:
                        self.assertEqual(self.resolve(), 17)
                    finally:
                        path.unlink()

    def test_cli_takes_priority_over_environment_ini_and_json(self):
        self.save_language(self.port / 'settings.json', 17)
        self.ini.write_text('language=es-es\n', encoding='utf-8')
        self.assertEqual(self.resolve('fr', {'THEORDER_LANGUAGE': 'de'}), 2)

    def test_environment_takes_priority_over_ini_and_json(self):
        self.save_language(self.port / 'settings.json', 17)
        self.ini.write_text('language=es-es\n', encoding='utf-8')
        self.assertEqual(self.resolve(env={'THEORDER_LANGUAGE': 'de'}), 4)

    def test_ini_takes_priority_over_json(self):
        self.save_language(self.port / 'settings.json', 17)
        self.ini.write_text('language=es-es\n', encoding='utf-8')
        self.assertEqual(self.resolve(), 3)

    def test_portable_settings_take_priority_over_user_and_legacy(self):
        self.save_language(self.port / 'settings.json', 'pt-br')
        self.save_language(self.port / 'user' / 'settings.json', 'fr')
        self.save_language(self.appdata / 'theorder1886-launcher' / 'settings.json', 'de')
        self.assertEqual(self.resolve(), 17)

    def test_malformed_json_falls_back_to_english_locale(self):
        (self.port / 'settings.json').write_text('{ invalid json', encoding='utf-8')
        self.assertEqual(self.resolve(), 1)


class EngineLanguageTest(unittest.TestCase):
    """PT-BR (PS4 id 17) plays as PT-PT on builds without its assets; the title comes from param.sfo."""

    def test_pt_br_falls_back_on_builds_without_it(self):
        for title in ('CUSA00035', 'CUSA00076', 'CUSA00070'):
            self.assertEqual(run.engine_language(17, title), 7, title)

    def test_pt_br_kept_where_it_exists(self):
        self.assertEqual(run.engine_language(17, 'CUSA00785'), 17)

    def test_other_languages_use_the_table(self):
        self.assertEqual(run.engine_language(1, 'CUSA00035'), 0)
        self.assertEqual(run.engine_language(0, 'CUSA00035'), 18)

    def setUp(self):
        self.temp_dir = TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)

    def make_dump(self, *packages):
        game = Path(self.temp_dir.name) / 'game'
        folder = game / '_data' / '1408209885' / 'ps4' / 'primary' / '3861c21fc34b7612' / 'v1'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / '2690af50a36aa2b1').write_bytes(b'')
        for package in packages:
            (folder / package).write_bytes(b'')
        return game

    def test_missing_language_package_falls_back_to_english(self):
        # CUSA00035 01.02 ships neither Portuguese package: both boot to a black screen.
        game = self.make_dump()
        self.assertEqual(run.available_language(17, 7, game), (1, 0))
        self.assertEqual(run.available_language(17, 17, game), (1, 0))

    def test_pt_br_without_its_package_uses_pt_pt_when_present(self):
        game = self.make_dump(run.LANGUAGE_PACKAGES[7])
        self.assertEqual(run.available_language(17, 17, game), (17, 7))
        self.assertEqual(run.available_language(17, 7, game), (17, 7))

    def test_present_package_and_unknown_languages_are_kept(self):
        game = self.make_dump(run.LANGUAGE_PACKAGES[17])
        self.assertEqual(run.available_language(17, 17, game), (17, 17))
        self.assertEqual(run.available_language(2, 2, self.make_dump()), (2, 2))
        # Not an unpacked dump (no _data): nothing is checked.
        self.assertEqual(run.available_language(17, 7, Path(self.temp_dir.name)), (17, 7))


if __name__ == '__main__':
    unittest.main()
