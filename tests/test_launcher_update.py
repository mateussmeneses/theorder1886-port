"""Transactional updater (launcher/port_update.py): no network, only temporary folders."""
import hashlib
import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import port_update as pu  # noqa: E402

USER = ('user', 'out', 'mods', 'theorder.ini', 'settings.json')


class Crash(BaseException):
    """Stands for the process dying: nothing in apply() may catch it."""


class FakeResponse:
    def __init__(self, data, length=None):
        self.stream = io.BytesIO(data)
        self.headers = {'Content-Length': str(len(data) if length is None else length)}

    def read(self, n=-1):
        return self.stream.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def tree(root):
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(Path(root).rglob('*')) if p.is_file()}


def write(root, files):
    for name, data in files.items():
        path = Path(root) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.target = self.tmp / 'install'
        self.new = self.tmp / 'new'
        self.old_program = {'The Order 1886.exe': 'old exe', 'run.py': 'old run', 'scripts/old_only.py': 'orphan',
                            'scripts/shared.py': 'old shared', pu.MANIFEST: 'The Order 1886.exe\nrun.py\n'
                            'scripts/old_only.py\nscripts/shared.py\n' + pu.MANIFEST + '\n'}
        self.user = {'user/saves/slot1.bin': 'SAVE', 'user/cache/a.spv': 'cache', 'mods/m/mod.json': '{}',
                     'settings.json': '{"a": 1}', 'theorder.ini': '[x]'}
        self.new_program = {'The Order 1886.exe': 'new exe', 'run.py': 'new run', 'scripts/shared.py': 'new shared',
                            'scripts/added.py': 'added'}
        write(self.target, {**self.old_program, **self.user})
        write(self.new, {**self.new_program, pu.MANIFEST: '\n'.join(self.new_program) + '\n' + pu.MANIFEST + '\n'})
        self.user_before = {k: v for k, v in tree(self.target).items() if k.split('/')[0] in USER}

    def tearDown(self):
        self._tmp.cleanup()

    def assert_user_intact(self):
        now = {k: v for k, v in tree(self.target).items() if k.split('/')[0] in USER}
        self.assertEqual(now, self.user_before)

    def assert_old_restored(self):
        got = tree(self.target)
        for name, data in self.old_program.items():
            self.assertEqual(got.get(name), data.encode(), name)
        self.assertNotIn('scripts/added.py', got)
        self.assertFalse((self.target / pu.PENDING).exists())
        self.assertEqual([p for p in self.target.iterdir() if p.name.startswith('backup-')], [])
        self.assert_user_intact()


class Download(Base):
    def test_parse_checksum(self):
        digest = 'a' * 64
        self.assertEqual(pu.parse_checksum(f'{digest.upper()} *theorder1886-windows.zip\n'), digest)
        self.assertEqual(pu.parse_checksum(f'{digest}  x.zip'), digest)
        self.assertIsNone(pu.parse_checksum('<html>not found</html>'))
        self.assertIsNone(pu.parse_checksum(''))

    def test_download_goes_through_partial(self):
        dest = self.tmp / 'u.zip'
        seen = []
        data = b'x' * 3000

        def opener(request, timeout=0):
            seen.append(dest.with_name(dest.name + '.partial').exists())
            return FakeResponse(data)
        pu.download('http://x/u.zip', dest, opener=opener)
        self.assertEqual(dest.read_bytes(), data)
        self.assertFalse(dest.with_name('u.zip.partial').exists())

    def test_truncated_download_is_rejected_and_leaves_nothing(self):
        dest = self.tmp / 'u.zip'
        with self.assertRaises(pu.UpdateError):
            pu.download('http://x/u.zip', dest, opener=lambda r, timeout=0: FakeResponse(b'x' * 10, length=999))
        self.assertFalse(dest.exists())
        self.assertFalse(dest.with_name('u.zip.partial').exists())

    def test_wrong_hash(self):
        f = self.tmp / 'f.bin'
        f.write_bytes(b'data')
        pu.verify_sha256(f, hashlib.sha256(b'data').hexdigest())
        with self.assertRaises(pu.UpdateError):
            pu.verify_sha256(f, '0' * 64)

    def test_corrupt_zip(self):
        bad = self.tmp / 'bad.zip'
        bad.write_bytes(b'PK\x03\x04 this is not a zip')
        with self.assertRaises(pu.UpdateError):
            pu.extract(bad, self.tmp / 'out')
        good = self.tmp / 'good.zip'
        with zipfile.ZipFile(good, 'w') as z:
            z.writestr('pkg/The Order 1886.exe', 'x')
            z.writestr('pkg/run.py', 'y')
        self.assertEqual(pu.extract(good, self.tmp / 'ok', 'The Order 1886.exe'), self.tmp / 'ok' / 'pkg')

    def test_zip_slip_is_rejected(self):
        evil = self.tmp / 'evil.zip'
        with zipfile.ZipFile(evil, 'w') as z:
            z.writestr('../escape.txt', 'x')
        with self.assertRaises(pu.UpdateError):
            pu.extract(evil, self.tmp / 'out2')
        self.assertFalse((self.tmp / 'escape.txt').exists())


class Apply(Base):
    def test_success_swaps_program_removes_orphan_keeps_backup_until_commit(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        got = tree(self.target)
        self.assertEqual(got['run.py'], b'new run')
        self.assertEqual(got['scripts/added.py'], b'added')
        self.assertNotIn('scripts/old_only.py', got)  # orphan listed only in the old manifest
        self.assertTrue((self.target / 'backup-0.9.0' / 'scripts' / 'old_only.py').exists())
        self.assertEqual(pu.pending(self.target)['phase'], 'installed')
        self.assert_user_intact()
        pu.commit(self.target)
        self.assertFalse((self.target / 'backup-0.9.0').exists())
        self.assertIsNone(pu.pending(self.target))
        self.assertEqual(tree(self.target)['run.py'], b'new run')
        self.assert_user_intact()

    def test_old_install_without_manifest_replaces_only_what_the_package_brings(self):
        (self.target / pu.MANIFEST).unlink()
        write(self.target, {'my_notes.txt': 'keep me'})
        pu.apply(self.target, self.new, '0.9.0', USER)
        got = tree(self.target)
        self.assertEqual(got['run.py'], b'new run')
        self.assertEqual(got['my_notes.txt'], b'keep me')
        pu.commit(self.target)
        self.assertEqual(tree(self.target)['my_notes.txt'], b'keep me')
        self.assert_user_intact()

    def test_file_in_use_rolls_back(self):
        real = pu.os.replace

        def replace(src, dst):
            if Path(src).name == 'run.py' and 'backup-' in str(dst):
                raise PermissionError('in use')
            return real(src, dst)
        with self.assertRaises(pu.UpdateError):
            pu.apply(self.target, self.new, '0.9.0', USER, replace=replace)
        self.assert_old_restored()

    def test_failure_while_placing_new_files_rolls_back(self):
        real, calls = pu.os.replace, []

        def replace(src, dst):
            calls.append(dst)
            if Path(src).parent == self.new / 'scripts' and Path(src).name == 'shared.py':
                raise OSError('disk full')
            return real(src, dst)
        with self.assertRaises(pu.UpdateError):
            pu.apply(self.target, self.new, '0.9.0', USER, replace=replace)
        self.assert_old_restored()

    def test_interruption_between_the_two_moves_is_recovered_on_next_start(self):
        real = pu.os.replace

        def replace(src, dst):
            if Path(src).parent == self.new:  # first new file about to be placed: all old ones are in the backup
                raise Crash()
            return real(src, dst)
        with self.assertRaises(Crash):
            pu.apply(self.target, self.new, '0.9.0', USER, replace=replace)
        self.assertEqual(pu.pending(self.target)['phase'], 'moving')
        self.assertNotIn('run.py', tree(self.target))  # the install is really half done
        self.assertTrue(pu.recover(self.target))
        self.assert_old_restored()

    def test_interruption_midway_through_new_files(self):
        real, state = pu.os.replace, {'n': 0}

        def replace(src, dst):
            if Path(src).parent.is_relative_to(self.new):
                state['n'] += 1
                if state['n'] == 3:
                    raise Crash()
            return real(src, dst)
        with self.assertRaises(Crash):
            pu.apply(self.target, self.new, '0.9.0', USER, replace=replace)
        self.assertTrue(pu.recover(self.target))
        self.assert_old_restored()

    def test_new_version_that_fails_to_start_is_rolled_back(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        clock = {'t': 0.0}

        def sleep(s):
            clock['t'] += s
        # the new launcher dies without ever calling commit()
        self.assertFalse(pu.wait_for_commit(self.target, lambda: False, 20, sleep=sleep, clock=lambda: clock['t']))
        self.assertTrue(pu.rollback(self.target))
        self.assert_old_restored()

    def test_hanging_new_version_times_out(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        clock = {'t': 0.0}

        def sleep(s):
            clock['t'] += s
        self.assertFalse(pu.wait_for_commit(self.target, lambda: True, 5, sleep=sleep, clock=lambda: clock['t']))

    def test_new_version_that_starts_commits(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        ticks = {'n': 0}

        def sleep(s):
            ticks['n'] += 1
            if ticks['n'] == 3:
                pu.commit(self.target)
        self.assertTrue(pu.wait_for_commit(self.target, lambda: True, 20, sleep=sleep, clock=lambda: ticks['n']))
        self.assertFalse(list(self.target.glob('backup-*')))

    def test_user_files_in_the_package_are_never_moved(self):
        write(self.new, {'settings.json': 'FROM PACKAGE', 'user/x.bin': 'p', 'mods/p.json': 'p'})
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assert_user_intact()
        self.assertNotIn('user/x.bin', tree(self.target))

    def test_recover_and_commit_do_nothing_without_an_update(self):
        before = tree(self.target)
        self.assertFalse(pu.recover(self.target))
        pu.commit(self.target)
        self.assertEqual(tree(self.target), before)

    def test_installed_but_uncommitted_is_not_touched_by_recover(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertFalse(pu.recover(self.target))
        self.assertEqual(tree(self.target)['run.py'], b'new run')


if __name__ == '__main__':
    unittest.main()
