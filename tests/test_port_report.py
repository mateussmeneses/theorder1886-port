"""Bug reports carry the game's last_run.log even when the launcher's log view is empty (#116)."""
import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import port_report  # noqa: E402
import port_launcher_win as launcher  # noqa: E402


class CollectLogTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def test_file_is_used_when_the_view_is_empty(self):
        log = self.dir / 'last_run.log'
        log.write_text('GPU: using Radeon\nFrame stats: 60 FPS\n', encoding='utf-8')
        text, source = port_report.collect_log('', log)
        self.assertIn('GPU: using Radeon', text)
        self.assertIn('last_run.log', source)

    def test_file_is_preferred_over_a_partial_view(self):
        log = self.dir / 'last_run.log'
        log.write_bytes(b'whole run\n')
        self.assertEqual(port_report.collect_log('after Clear', log)[0], 'whole run\n')

    def test_view_is_the_fallback(self):
        text, source = port_report.collect_log('launcher only', self.dir / 'missing.log')
        self.assertEqual((text, source), ('launcher only', 'launcher log view'))
        (self.dir / 'empty.log').write_text('', encoding='utf-8')
        self.assertEqual(port_report.collect_log('view', self.dir / 'empty.log')[0], 'view')
        self.assertEqual(port_report.collect_log('  \n', self.dir / 'missing.log')[0], '')

    def test_large_files_keep_whole_lines_of_the_tail(self):
        log = self.dir / 'last_run.log'
        with mock.patch.object(port_report, 'LOG_TAIL_BYTES', 64):
            log.write_bytes(''.join(f'line {i:04d}\n' for i in range(100)).encode())
            text, _ = port_report.collect_log('', log)
        self.assertTrue(text.startswith('line '))
        self.assertTrue(text.endswith('line 0099\n'))
        self.assertLessEqual(len(text), 64)


class ReportTest(unittest.TestCase):
    def test_report_names_version_and_log_source(self):
        md, redacted = port_report.generate_report({}, {}, 'Frame stats\n', ROOT / 'missing-repo',
                                                   version='0.8.0', log_source='last_run.log (game output file)')
        self.assertIn('0.8.0 (Release build', md)
        self.assertIn('Source: last_run.log (game output file)', md)
        self.assertNotIn('No log output available', md)
        self.assertEqual(redacted, 'Frame stats\n')

    def test_export_reads_the_run_log_of_the_saves_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = Path(tmp) / 'user'
            user.mkdir()
            (user / 'last_run.log').write_text('GPU: using RX 6500 XT\n', encoding='utf-8')
            owner = launcher.Launcher.__new__(launcher.Launcher)
            owner.app = {**launcher.APP_DEFAULTS, 'user_dir': str(user)}
            owner.var = mock.Mock(side_effect=KeyError)
            owner.messagebox = mock.Mock()
            zip_path = Path(tmp) / 'report.zip'
            with mock.patch('tkinter.filedialog.asksaveasfilename', return_value=str(zip_path)):
                owner.export_diagnostic_report()
            owner.messagebox.showerror.assert_not_called()
            with zipfile.ZipFile(zip_path) as z:
                self.assertIn('RX 6500 XT', z.read('last_run_sanitized.log').decode())
                self.assertIn(f'{launcher.VERSION} (', z.read('report.md').decode())


if __name__ == '__main__':
    unittest.main()
