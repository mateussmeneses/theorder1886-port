import json
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import graphics_options as g


class GraphicsOptionsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.ini = Path(self.temp.name) / 'theorder.ini'
        self.state = Path(self.temp.name) / 'state.json'

    def tearDown(self):
        self.temp.cleanup()

    def test_defaults_and_invalid_values(self):
        self.ini.write_text('aspect_ratio=4:3\nengine_fps=90\nstale_readback=yes\n', encoding='utf-8')
        options, reverted = g.prepare(self.ini, self.state)
        self.assertFalse(reverted)
        self.assertEqual(options, {'aspect_ratio': 'original', 'engine_fps': '120', 'deferred_readback': '1',
                                   'stale_readback': '1', 'async_shaders': '1'})

    def test_write_keys_keeps_other_lines(self):
        self.ini.write_text('# comment\nshow_fps=1\nengine_fps=60\n', encoding='utf-8')
        g.write_keys(self.ini, {'engine_fps': '240', 'vsync': '0'})
        self.assertEqual(self.ini.read_text(encoding='utf-8'), '# comment\nshow_fps=1\nengine_fps=240\nvsync=0\n')

    def test_write_keys_drops_god_of_war_leftovers(self):
        self.ini.write_text('# gow3 settings (in-game menu: Insert / R3+L2)\nshow_fps=1\nred_orb_multiplier=1.000\n'
                            'cheat_health=0\nenemy_health_bar=0\n', encoding='utf-8')
        g.write_keys(self.ini, {'engine_fps': '120'})
        self.assertEqual(self.ini.read_text(encoding='utf-8'), f'{g.HEADER}\nshow_fps=1\nengine_fps=120\n')

    def test_unconfirmed_restart_is_tried_once_then_reverted(self):
        self.ini.write_text('aspect_ratio=16:10\nengine_fps=120\n', encoding='utf-8')
        good, _ = g.prepare(self.ini, self.state)  # a normal, confirmed launch
        g.write_keys(self.ini, {'aspect_ratio': '21:9', 'engine_fps': '240', 'restart_unconfirmed': '1'})
        tried, reverted = g.prepare(self.ini, self.state)  # the launch of "Apply and restart"
        self.assertFalse(reverted)
        self.assertEqual((tried['aspect_ratio'], tried['engine_fps']), ('21:9', '240'))
        back, reverted = g.prepare(self.ini, self.state)  # it never confirmed: back to the good ones
        self.assertTrue(reverted)
        self.assertEqual(back, good)
        ini = g.read_ini(self.ini)
        self.assertEqual((ini['aspect_ratio'], ini['restart_unconfirmed']), ('16:10', '0'))

    def test_confirmed_restart_becomes_the_last_good(self):
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'engine_fps': '240', 'restart_unconfirmed': '1'})
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'restart_unconfirmed': '0'})  # the game confirmed
        g.prepare(self.ini, self.state)
        self.assertEqual(json.loads(self.state.read_text())['last_good']['engine_fps'], '240')

    def test_patch_names(self):
        launcher = ['Frame Rate Patch - 120 FPS', 'Skip Intro Logos']
        options = g.startup_options({'aspect_ratio': '16:9', 'engine_fps': '120'})
        # The XML's "60 FPS" patch: the game runs at half the engine rate.
        self.assertEqual(g.patch_names(launcher, options),
                         ['Skip Intro Logos', 'Aspect Ratio - 16:9', 'Frame Rate Patch - 60 FPS',
                          'QTE Fix - Button Mash at Any Frame Rate'])
        original = g.startup_options({'engine_fps': '60'})
        self.assertEqual(g.patch_names(['Aspect Ratio - 21:9', 'Skip Intro Logos'], original),
                         ['Skip Intro Logos'])

    def test_qte_fix_follows_the_frame_rate_patch(self):
        # #101: the fix exists in the XML and joins only a selection with a frame rate patch.
        from patches import available_patches
        patch_file = paths.ROOT / 'patches/TheOrder1886-Orbis.xml'
        self.assertIn(g.QTE_FIX, available_patches(patch_file, '01.02'))
        names = g.patch_names(['QTE Fix - Button Mash at Any Frame Rate'], g.startup_options({'engine_fps': '240'}))
        self.assertEqual(names.count(g.QTE_FIX), 1)
        self.assertNotIn(g.QTE_FIX, g.patch_names([], g.startup_options({'engine_fps': '60'})))

    def test_ultrawide_startup_selects_the_existing_patch(self):
        from patches import selected_patches
        self.ini.write_text('aspect_ratio=32:9\nengine_fps=60\noutput_res=5120x1440\n', encoding='utf-8')
        options, reverted = g.prepare(self.ini, self.state)
        self.assertFalse(reverted)
        self.assertEqual(options['aspect_ratio'], '32:9')
        names = g.patch_names([], options)
        self.assertEqual(names, ['Aspect Ratio - 32:9'])
        patch_file = paths.ROOT / 'patches/TheOrder1886-Orbis.xml'
        self.assertEqual(selected_patches(patch_file, '01.02', ';'.join(names)), names)
        self.assertEqual(g.read_ini(self.ini)['output_res'], '5120x1440')

    def test_applied_engine_fps(self):
        # The engine runs above 60 only with a frame rate patch in the applied list.
        self.assertEqual(g.applied_engine_fps(['Skip Intro Logos', 'Frame Rate Patch - 120 FPS']), 240)
        self.assertEqual(g.applied_engine_fps(['Frame Rate Patch - 60 FPS']), 120)
        self.assertEqual(g.applied_engine_fps(['Skip Intro Logos']), 60)
        self.assertEqual(g.applied_engine_fps([]), 60)

    def test_environment(self):
        env = g.environment(g.startup_options({'stale_readback': '0'}))
        self.assertEqual(env, {'THEORDER_DEFERRED_READBACK': '1', 'THEORDER_STALE_READBACK': '0', 'THEORDER_ASYNC_SHADERS': '1'})


if __name__ == '__main__':
    unittest.main()
