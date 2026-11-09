from paths import ROOT
import sys
import unittest
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(ROOT))
import run


class RunIsaTests(unittest.TestCase):
    def test_development_dll_path_is_configured_even_with_copied_sdl(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            probe = root / 'out' / 'theorder-probe.exe'
            probe.parent.mkdir()
            (probe.parent / 'SDL3.dll').touch()
            clang64 = root / 'msys64' / 'clang64' / 'bin'
            clang64.mkdir(parents=True)
            env = {'MSYS2_ROOT': str(root / 'msys64'), 'PATH': 'original-path'}
            with patch.object(run, 'WINDOWS', True), patch.object(run, 'PORT', root):
                run.configure_windows_runtime(probe, env)
            self.assertEqual(env['PATH'], f'{clang64}{run.os.pathsep}original-path')

    def test_packaged_dll_path_is_not_replaced_by_development_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            probe = root / 'dist' / 'theorder-probe.exe'
            probe.parent.mkdir()
            (probe.parent / 'SDL3.dll').touch()
            clang64 = root / 'msys64' / 'clang64' / 'bin'
            clang64.mkdir(parents=True)
            env = {'MSYS2_ROOT': str(root / 'msys64'), 'PATH': 'packaged-path'}
            with patch.object(run, 'WINDOWS', True), patch.object(run, 'PORT', root):
                run.configure_windows_runtime(probe, env)
            self.assertEqual(env['PATH'], 'packaged-path')

    def test_game_folder_selection_never_uses_private_developer_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'port'
            root.mkdir()
            implicit = root.parent / 'CUSA00035'
            explicit = root.parent / 'other-game'
            implicit.mkdir()
            explicit.mkdir()
            (implicit / 'eboot.bin').write_bytes(b'test')
            (explicit / 'eboot.bin').write_bytes(b'test')

            self.assertEqual(run.select_game_folder({}, root), implicit)
            self.assertEqual(run.select_game_folder(
                {'THEORDER_GAME_DIR': str(explicit)}, root), explicit)
            self.assertEqual(run.select_game_folder(
                {'THEORDER_GAME_DIR': str(root / 'missing')}, root), implicit)
            (implicit / 'eboot.bin').unlink()
            # Preserve the explicit failed path for the caller's error message.
            self.assertEqual(run.select_game_folder(
                {'THEORDER_GAME_DIR': str(explicit / 'missing')}, root),
                explicit / 'missing')
            self.assertEqual(run.select_game_folder({}, root), implicit)

    def test_parse_cpu_features(self):
        self.assertEqual(run.parse_cpu_features('CPU: missing MOVBE, BMI1, LZCNT\n'), ['MOVBE', 'BMI1', 'LZCNT'])
        self.assertEqual(run.parse_cpu_features('CPU: complete\n'), [])

    def test_rewrite_wanted_follows_cpu_by_default(self):
        self.assertTrue(run.isa_rewrite_wanted(['MOVBE'], {}))
        self.assertFalse(run.isa_rewrite_wanted([], {}))

    def test_rewrite_forced(self):
        self.assertTrue(run.isa_rewrite_wanted([], {'THEORDER_ISA_REWRITE': '1'}))

    def test_rewrite_forced_for_a_list_of_sets(self):
        self.assertTrue(run.isa_rewrite_wanted([], {'THEORDER_ISA_REWRITE': 'MOVBE,BMI1,LZCNT'}))
        self.assertFalse(run.isa_rewrite_wanted([], {'THEORDER_ISA_REWRITE': ''}))

    def test_rewrite_disabled(self):
        self.assertFalse(run.isa_rewrite_wanted(['MOVBE', 'BMI1'], {'THEORDER_ISA_REWRITE': '0'}))


if __name__ == '__main__':
    unittest.main()
