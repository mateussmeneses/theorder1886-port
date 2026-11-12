"""Launcher fields for the settings of the SpartanPort adoption phases 3-5 (theorder.ini keys the
game reads: master_volume, audio_channels, pause_game, frame_generation): defaults and choices
the game accepts (gpu/shim/port_settings.cpp), their pages and their Portuguese texts."""
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import port_lang  # noqa: E402
import port_launcher_win as launcher  # noqa: E402


class NewSettingsTest(unittest.TestCase):
    def test_defaults_are_the_games_own(self):
        d = launcher.INI_DEFAULTS
        self.assertEqual((d['master_volume'], d['audio_channels'], d['pause_game'], d['frame_generation']),
                         ('100', 'auto', '1', 'off'))
        self.assertIn('pause_game', launcher.INI_FLAGS)

    def test_choices_are_values_the_game_parses(self):
        self.assertEqual([v for v, _t in launcher.AUDIO_CHANNELS], ['auto', 'stereo', '5.1', '7.1'])
        self.assertEqual([v for v, _t in launcher.FRAME_GENERATION], ['off', 'fsr'])
        volumes = [int(v) for v, _t in launcher.MASTER_VOLUMES]
        self.assertEqual((min(volumes), max(volumes)), (0, 200))
        self.assertIn(100, volumes)

    def test_each_setting_is_on_a_page(self):
        tabs = launcher.Launcher.TAB_KEYS if hasattr(launcher, 'Launcher') else None
        if tabs is None:
            tabs = next(v.TAB_KEYS for v in vars(launcher).values() if hasattr(v, 'TAB_KEYS'))
        self.assertIn('frame_generation', tabs['performance'])
        for key in ('master_volume', 'audio_channels', 'pause_game'):
            self.assertIn(key, tabs['game'])

    def test_portuguese_texts(self):
        pt = port_lang.table('pt')
        for text in ('Master volume', 'Speakers', 'Pause the game while the menu is open', 'Frame generation',
                     "Automatic (the device's)", 'Stereo'):
            self.assertIn(text, pt)


if __name__ == '__main__':
    unittest.main()
