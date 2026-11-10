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


if __name__ == '__main__':
    unittest.main()
