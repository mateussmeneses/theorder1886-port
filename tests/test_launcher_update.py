"""Transactional updater (launcher/port_update.py): no network, only temporary folders."""
import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

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
        self.build()

    def build(self):
        shutil.rmtree(self.tmp / 'install', ignore_errors=True)
        shutil.rmtree(self.tmp / 'new', ignore_errors=True)
        self.target = self.tmp / 'install'
        self.new = self.tmp / 'new'
        self.old_program = {'The Order 1886.exe': 'old exe', 'run.py': 'old run', '_internal/lib.dll': 'old lib',
                            'scripts/old_only.py': 'orphan', 'scripts/shared.py': 'old shared',
                            pu.MANIFEST: 'The Order 1886.exe\nrun.py\n_internal/lib.dll\nscripts/old_only.py\n'
                            'scripts/shared.py\n' + pu.MANIFEST + '\n'}
        self.user = {'user/saves/slot1.bin': 'SAVE', 'user/cache/a.spv': 'cache', 'mods/m/mod.json': '{}',
                     'settings.json': '{"a": 1}', 'theorder.ini': '[x]'}
        # the manifest lists the exe first, as `find | sort` does: the updater must still swap it last
        self.new_program = {'The Order 1886.exe': 'new exe', 'run.py': 'new run', '_internal/lib.dll': 'new lib',
                            'scripts/shared.py': 'new shared', 'scripts/added.py': 'added'}
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
        self.assertEqual([p for p in self.target.iterdir()
                          if p.name.startswith('backup-') or p.suffix in ('.upd', '.done')], [])
        self.assert_user_intact()


class Download(Base):
    def test_parse_checksum(self):
        digest = 'a' * 64
        self.assertEqual(pu.parse_checksum(f'{digest.upper()} *theorder1886-windows.zip\n'), digest)
        self.assertEqual(pu.parse_checksum(f'{digest}  x.zip'), digest)
        self.assertEqual(pu.parse_checksum('\ufeff' + digest), digest)
        self.assertIsNone(pu.parse_checksum('<html>not found</html>'))
        self.assertIsNone(pu.parse_checksum(''))

    def test_download_goes_through_partial(self):
        dest = self.tmp / 'u.zip'
        data = b'x' * 3000
        pu.download('http://x/u.zip', dest, opener=lambda r, timeout=0: FakeResponse(data))
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

    def test_file_in_use_rolls_back(self):
        real = os.replace

        def replace(src, dst):
            if Path(dst).name == 'run.py':
                raise PermissionError('in use')
            return real(src, dst)
        with self.assertRaises(pu.UpdateError) as caught:
            pu.apply(self.target, self.new, '0.9.0', USER, replace=replace)
        self.assertIn('backup-0.9.0', str(caught.exception))  # every error says where the backup is
        self.assert_old_restored()

    def test_failure_while_placing_new_files_rolls_back(self):
        real = os.replace

        def replace(src, dst):
            if Path(dst).name == 'shared.py':
                raise OSError('disk full')
            return real(src, dst)
        with self.assertRaises(pu.UpdateError):
            pu.apply(self.target, self.new, '0.9.0', USER, replace=replace)
        self.assert_old_restored()

    def test_power_loss_at_every_step_leaves_a_launcher_and_a_journal(self):
        """Old files are copied, never moved out: a launcher exe is on disk after a crash at any step,
        and the journal lets recover() put the old tree back."""
        steps = []
        counter = {'n': 0, 'at': None}
        real_replace, real_copy = os.replace, shutil.copy2

        def tick():
            counter['n'] += 1
            if counter['n'] == counter['at']:
                raise Crash()

        def replace(src, dst):
            tick()
            steps.append(Path(dst).name)
            return real_replace(src, dst)

        def copy(src, dst):
            tick()
            return real_copy(src, dst)
        pu.apply(self.target, self.new, '0.9.0', USER, replace=replace, copy=copy)
        total = counter['n']
        self.assertGreater(total, 8)
        self.assertEqual(steps[-1], 'The Order 1886.exe')  # the launcher is swapped last
        for at in range(1, total + 1):
            self.build()
            counter.update(n=0, at=at)
            with self.assertRaises(Crash):
                pu.apply(self.target, self.new, '0.9.0', USER, replace=replace, copy=copy)
            exe = self.target / 'The Order 1886.exe'
            self.assertTrue(exe.is_file() and exe.stat().st_size > 0, f'no launcher after a crash at step {at}')
            if exe.read_bytes() == b'new exe':  # the exe goes last: every other new file is already there
                for name, data in self.new_program.items():
                    self.assertEqual((self.target / name).read_bytes(), data.encode(), (at, name))
            counter['at'] = None
            self.assertTrue(pu.recover(self.target), f'journal missing after a crash at step {at}')
            self.assert_old_restored()

    def test_user_file_at_a_path_the_package_also_ships_survives_rollback(self):
        write(self.target, {'patches/mine.xml': 'USER XML'})
        write(self.new, {'patches/mine.xml': 'shipped'})
        (self.new / pu.MANIFEST).write_text('\n'.join(self.new_program) + '\npatches/mine.xml\n' + pu.MANIFEST + '\n')
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertEqual((self.target / 'patches/mine.xml').read_bytes(), b'shipped')
        self.assertEqual((self.target / 'backup-0.9.0/patches/mine.xml').read_bytes(), b'USER XML')
        self.assertTrue(pu.rollback(self.target))
        self.assertEqual((self.target / 'patches/mine.xml').read_bytes(), b'USER XML')

    def test_os_error_writing_the_journal_becomes_update_error(self):
        with mock.patch.object(pu, '_write_journal', side_effect=PermissionError('read-only folder')):
            with self.assertRaises(pu.UpdateError):
                pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertEqual(tree(self.target)['run.py'], b'old run')

    def test_unsafe_manifest_entries_are_rejected(self):
        for bad in ('/x', 'C:x', 'C:\\x', '\\x', '../x', 'a/../../x'):
            (self.new / pu.MANIFEST).write_text('run.py\n' + bad + '\n')
            with self.assertRaises(pu.UpdateError, msg=bad):
                pu.apply(self.target, self.new, '0.9.0', USER)
            self.assert_old_restored()

    def test_user_files_match_case_insensitively(self):
        write(self.new, {'Settings.json': 'FROM PACKAGE', 'User/x.bin': 'p', 'MODS/p.json': 'p'})
        (self.new / pu.MANIFEST).write_text('run.py\nSettings.json\nUser/x.bin\nMODS/p.json\n')
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assert_user_intact()
        self.assertNotIn('User/x.bin', tree(self.target))

    def test_journal_is_fsynced_and_a_corrupt_one_is_reported(self):
        with mock.patch('os.fsync') as fsync:
            pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertGreaterEqual(fsync.call_count, 2)
        (self.target / pu.PENDING).write_text('{not json')
        with self.assertRaises(pu.UpdateError):
            pu.pending(self.target)
        with self.assertRaises(pu.UpdateError):
            pu.recover(self.target)

    def test_rollback_restores_files_with_equal_size_and_timestamp(self):
        """Coarse clocks (CI) give old and new files the same size and mtime: only content tells them apart."""
        pu.apply(self.target, self.new, '0.9.0', USER)
        stamp = (self.target / 'backup-0.9.0' / 'run.py').stat().st_mtime_ns
        os.utime(self.target / 'run.py', ns=(stamp, stamp))
        self.assertTrue(pu.rollback(self.target))
        self.assertEqual((self.target / 'run.py').read_bytes(), b'old run')

    def test_installed_update_that_never_commits_is_rolled_back_after_n_starts(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        for _ in range(pu.MAX_STARTS):
            self.assertFalse(pu.recover(self.target))
        self.assertTrue(pu.recover(self.target))
        self.assert_old_restored()

    def test_journal_left_in_rollback_phase_is_finished_by_recover(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        journal = pu.pending(self.target)
        journal['phase'] = 'rollback'
        (self.target / pu.PENDING).write_text(json.dumps(journal))
        self.assertTrue(pu.recover(self.target))
        self.assert_old_restored()

    def test_safe_rollback_retries_and_reports_failure(self):
        with mock.patch.object(pu, 'rollback', side_effect=[PermissionError('held'), PermissionError('held'), True]) as r:
            self.assertTrue(pu.safe_rollback(self.target, tries=5, sleep=lambda s: None))
            self.assertEqual(r.call_count, 3)
        with mock.patch.object(pu, 'rollback', side_effect=PermissionError('held')):
            self.assertFalse(pu.safe_rollback(self.target, tries=3, sleep=lambda s: None))

    def test_commit_removes_the_journal_before_the_backup(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        with mock.patch('shutil.rmtree', side_effect=Crash()):  # killed while deleting the backup
            with self.assertRaises(Crash):
                pu.commit(self.target)
        self.assertIsNone(pu.pending(self.target))  # committed: it can no longer be rolled back
        self.assertFalse(pu.recover(self.target))
        self.assertFalse(list(self.target.glob('backup-*')))  # recover finished the cleanup
        self.assertFalse((self.target / pu.DONE).exists())
        self.assertEqual(tree(self.target)['run.py'], b'new run')

    def test_lock_blocks_recover_and_second_updater(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        pu.acquire_lock(self.target)
        with self.assertRaises(pu.UpdateError):
            pu.acquire_lock(self.target)
        for _ in range(pu.MAX_STARTS + 3):
            self.assertFalse(pu.recover(self.target))  # held: neither counts nor rolls back
        self.assertEqual(pu.pending(self.target)['phase'], 'installed')
        old = os.stat(self.target / pu.LOCK).st_mtime - 10_000
        os.utime(self.target / pu.LOCK, (old, old))  # a dead updater's stale lock
        pu.acquire_lock(self.target)
        pu.release_lock(self.target)
        self.assertFalse((self.target / pu.LOCK).exists())

    def test_new_version_that_fails_to_start_is_rolled_back(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertEqual(pu.wait_for_commit(self.target, lambda: False, 300, sleep=lambda s: None), 'exited')
        self.assertTrue(pu.rollback(self.target))
        self.assert_old_restored()

    def test_alive_but_slow_new_version_gets_the_full_wait(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        clock = {'t': 0.0}

        def sleep(s):
            clock['t'] += s
        self.assertEqual(pu.wait_for_commit(self.target, lambda: True, 300, sleep=sleep, clock=lambda: clock['t']),
                         'timeout')
        self.assertGreater(clock['t'], 299)

    def test_new_version_that_starts_commits(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        ticks = {'n': 0}

        def sleep(s):
            ticks['n'] += 1
            if ticks['n'] == 3:
                pu.commit(self.target)
        self.assertEqual(pu.wait_for_commit(self.target, lambda: True, 20, sleep=sleep, clock=lambda: ticks['n']),
                         'committed')
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

    def test_first_start_of_an_installed_update_is_not_touched_by_recover(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertFalse(pu.recover(self.target))
        self.assertEqual(tree(self.target)['run.py'], b'new run')


class SecondPass(Base):
    def test_f1_apply_after_failed_rollback_refuses_and_keeps_the_backup(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        with mock.patch.object(pu, 'rollback', side_effect=PermissionError('disk full')):
            with self.assertRaises(pu.RestoreFailed):
                pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertEqual((self.target / 'backup-0.9.0' / 'run.py').read_bytes(), b'old run')
        self.assertIsNotNone(pu.pending(self.target))

    def test_f1_new_apply_over_a_pending_update_rolls_it_back_first(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertEqual((self.target / 'backup-0.9.0' / 'run.py').read_bytes(), b'old run')  # not a new/old mix
        self.assertEqual(tree(self.target)['run.py'], b'new run')

    def test_f1_no_retry_after_a_failed_rollback(self):
        calls = []

        def apply(*args, **kwargs):
            calls.append(1)
            raise pu.RestoreFailed('could not restore')
        notes = []
        with mock.patch.object(pu, 'apply', apply):
            code = pu.install_and_watch(self.target, self.new, '0.9.0', USER, lambda: None,
                                        lambda kind, detail='': notes.append(kind), retry_sleep=lambda s: None)
        self.assertEqual((code, len(calls), notes), (1, 1, ['restore_failed']))

    def test_f2a_rollback_puts_the_launcher_back_last(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        with mock.patch.object(pu, '_put', wraps=pu._put) as put:
            pu.rollback(self.target)
        names = [Path(c.args[3]).name for c in put.call_args_list]
        self.assertEqual(names[-1], 'The Order 1886.exe')
        self.assertEqual(names[-2], pu.MANIFEST)

    def test_f2b_changed_python_runtime_refuses_the_in_place_update(self):
        write(self.target, {'_internal/python313.dll': 'old py'})
        write(self.new, {'_internal/python314.dll': 'new py'})
        before = tree(self.target)
        with self.assertRaises(pu.RuntimeChanged):
            pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertEqual(tree(self.target), before)
        notes, calls = [], []
        code = pu.install_and_watch(self.target, self.new, '0.9.0', USER, lambda: None,
                                    lambda kind, detail='': notes.append(kind), retry_sleep=calls.append)
        self.assertEqual((code, notes, calls), (1, ['failed'], []))  # no retries

    def test_f3_damaged_journal_does_not_crash_the_start_path(self):
        (self.target / pu.PENDING).write_text('{broken')
        self.assertFalse(pu.commit_quietly(self.target))

    def test_f3_commit_retries_a_permission_error(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        real, state = os.replace, {'n': 0}

        def replace(src, dst):
            state['n'] += 1
            if state['n'] <= 2:
                raise PermissionError('watcher reading the journal')
            return real(src, dst)
        with mock.patch('os.replace', replace):
            pu.commit(self.target, sleep=lambda s: None)
        self.assertIsNone(pu.pending(self.target))

    @unittest.skipUnless(sys.platform == 'win32', 'the launcher module is imported on Windows only')
    def test_f4_play_without_window_commits_once_the_game_started(self):
        import port_launcher_win as launcher
        process = mock.Mock()
        process.stdout.readline.return_value = b''
        process.wait.return_value = 0
        with mock.patch.object(launcher, 'attach_stdio'), mock.patch.object(launcher, 'open_run_log'), \
                mock.patch.object(launcher, 'run_command', return_value=['x']), \
                mock.patch.object(launcher, 'game_environment', return_value={}), \
                mock.patch.object(launcher.subprocess, 'Popen', return_value=process), \
                mock.patch.object(launcher, 'FROZEN', True), mock.patch.object(launcher, 'WINDOWS', True), \
                mock.patch.object(pu, 'commit_quietly') as commit:
            launcher.play_without_window({})
        commit.assert_called_once_with(launcher.PORT_DIR)

    def test_f5_apply_refreshes_the_lock_for_every_file(self):
        with mock.patch.object(pu, 'touch_lock') as touch:
            pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertGreaterEqual(touch.call_count, len(self.new_program))

    def test_f5_no_retry_when_the_disk_is_full(self):
        import errno
        calls = []

        def copy(src, dst):
            calls.append(1)
            raise OSError(errno.ENOSPC, 'No space left on device')
        real_apply = pu.apply
        notes = []
        with mock.patch.object(pu, 'apply', lambda *a, **k: real_apply(*a, copy=copy, **k)):
            code = pu.install_and_watch(self.target, self.new, '0.9.0', USER, lambda: None,
                                        lambda kind, detail='': notes.append(kind), retry_sleep=lambda s: None)
        self.assertEqual((code, notes), (1, ['failed']))
        self.assertLessEqual(len(calls), 2)  # one failing copy, not 30 attempts

    def test_f5_stale_lock_takeover_is_exclusive(self):
        pu.acquire_lock(self.target)
        old = os.stat(self.target / pu.LOCK).st_mtime - 10_000
        os.utime(self.target / pu.LOCK, (old, old))
        real_open, state = os.open, {'n': 0}

        def opener(path, flags, *rest):
            state['n'] += 1
            if state['n'] == 2:  # a competitor creates the lock right after we cleared the stale one
                os.close(real_open(path, os.O_CREAT | os.O_WRONLY))
                raise FileExistsError()
            return real_open(path, flags, *rest)
        with mock.patch('os.open', opener):
            with self.assertRaises(pu.UpdateError):
                pu.acquire_lock(self.target)
        self.assertFalse(list(self.target.glob('.update.lock.stale*')))

    def test_f6_case_only_rename_keeps_the_new_file(self):
        write(self.target, {'Bin/x.dll': 'old dll'})
        (self.target / pu.MANIFEST).write_text((self.target / pu.MANIFEST).read_text() + 'Bin/x.dll\n')
        write(self.new, {'bin/x.dll': 'new dll'})
        (self.new / pu.MANIFEST).write_text('\n'.join(self.new_program) + '\nbin/x.dll\n' + pu.MANIFEST + '\n')
        pu.apply(self.target, self.new, '0.9.0', USER)
        got = {k.casefold(): v for k, v in tree(self.target).items()}
        self.assertEqual(got['bin/x.dll'], b'new dll')

    def test_f7_launch_failure_rolls_back_and_notifies(self):
        calls, notes = [], []

        def launch():
            calls.append(1)
            raise OSError('quarantined by antivirus')
        code = pu.install_and_watch(self.target, self.new, '0.9.0', USER, launch,
                                    lambda kind, detail='': notes.append(kind), retry_sleep=lambda s: None)
        self.assertEqual((code, notes), (1, ['launch_failed']))
        self.assert_old_restored()
        self.assertFalse((self.target / pu.LOCK).exists())

    def test_f8_content_cache_is_cleared(self):
        pu.apply(self.target, self.new, '0.9.0', USER)
        with mock.patch('filecmp.clear_cache') as clear:
            pu.rollback(self.target)
        self.assertTrue(clear.called)
        with mock.patch('filecmp.clear_cache') as clear:
            pu.apply(self.target, self.new, '0.9.0', USER)
        self.assertTrue(clear.called)

    def test_f9_not_enough_free_space_stops_before_touching_anything(self):
        before = tree(self.target)
        with self.assertRaises(pu.NoSpace):
            pu.apply(self.target, self.new, '0.9.0', USER, free_space=lambda p: 10)
        self.assertEqual(tree(self.target), before)
        self.assertIsNone(pu.pending(self.target))
        pu.apply(self.target, self.new, '0.9.0', USER, free_space=lambda p: 10 ** 12)


class FakeChild:
    def __init__(self, alive=True, on_poll=None):
        self.alive, self.on_poll, self.killed = alive, on_poll, False

    def poll(self):
        if self.on_poll:
            self.on_poll()
        return None if self.alive else 1

    def kill(self):
        self.killed, self.alive = True, False

    def wait(self):
        pass


class Glue(Base):
    def run_glue(self, children, **extra):
        launched, notes = [], []

        def launch():
            launched.append(1)
            return children[len(launched) - 1] if len(launched) <= len(children) else FakeChild()
        code = pu.install_and_watch(self.target, self.new, '0.9.0', USER, launch,
                                    lambda kind, detail='': notes.append(kind),
                                    retry_sleep=lambda s: None, sleep=lambda s: None, **extra)
        return code, launched, notes

    def test_success(self):
        child = FakeChild(on_poll=lambda: pu.commit(self.target))
        code, launched, notes = self.run_glue([child])
        self.assertEqual((code, len(launched), notes), (0, 1, []))
        self.assertEqual(tree(self.target)['run.py'], b'new run')
        self.assertFalse((self.target / pu.LOCK).exists())
        self.assert_user_intact()

    def test_new_version_exits_without_committing_restores_and_restarts_old(self):
        code, launched, notes = self.run_glue([FakeChild(alive=False)])
        self.assertEqual((code, len(launched), notes), (1, 2, ['restored']))
        self.assert_old_restored()

    def test_new_version_alive_but_not_committing_is_killed_after_the_wait(self):
        child = FakeChild()
        clock = iter(range(0, 100_000, 100))
        code, launched, notes = self.run_glue([child], clock=lambda: next(clock))
        self.assertTrue(child.killed)
        self.assertEqual((code, notes), (1, ['restored_slow']))
        self.assert_old_restored()

    def test_rollback_failure_is_reported_not_claimed_as_restored(self):
        with mock.patch.object(pu, 'safe_rollback', return_value=False):
            code, launched, notes = self.run_glue([FakeChild(alive=False)])
        self.assertEqual((code, len(launched), notes), (1, 1, ['restore_failed']))

    def test_os_error_in_apply_takes_the_fallback_path(self):
        with mock.patch.object(pu, '_write_journal', side_effect=PermissionError('Program Files')):
            code, launched, notes = self.run_glue([], apply_tries=2)
        self.assertEqual((code, launched, notes), (1, [], ['failed']))
        self.assertFalse((self.target / pu.LOCK).exists())

    def test_second_updater_is_refused(self):
        pu.acquire_lock(self.target)
        code, launched, notes = self.run_glue([])
        self.assertEqual((code, notes), (1, ['failed']))


class Checksum(unittest.TestCase):
    URL = 'https://x/releases/download/windows-v1/theorder1886-windows.zip'
    DIGEST = 'ab' * 32

    def fetch(self, body=None, error=None):
        def opener(request, timeout=0):
            self.assertTrue(request.full_url.endswith('.zip.sha256'))
            if error:
                raise error
            return FakeResponse(body)
        return pu.fetch_checksum(self.URL, opener=opener)

    def http(self, code):
        return pu.urllib.error.HTTPError(self.URL, code, 'x', {}, None)

    def test_valid_forms(self):
        line = f'{self.DIGEST}  theorder1886-windows.zip\n'.encode()
        self.assertEqual(self.fetch(line), self.DIGEST)
        self.assertEqual(self.fetch(b'\xef\xbb\xbf' + line), self.DIGEST)  # UTF-8 BOM (PowerShell)
        self.assertEqual(self.fetch(f'{self.DIGEST}\n'.encode()), self.DIGEST)  # no name

    def test_404_means_absent(self):
        self.assertIsNone(self.fetch(error=self.http(404)))

    def test_unparseable_body_is_an_error_not_absence(self):
        for body in (b'', b'<html>Not Found</html>', b'\n\n', b'zz' * 32):
            with self.assertRaises(pu.UpdateError, msg=body):
                self.fetch(body)

    def test_other_http_errors_propagate(self):
        with self.assertRaises(pu.urllib.error.HTTPError):
            self.fetch(error=self.http(500))

    def test_checksum_for_another_file_is_an_error(self):
        with self.assertRaises(pu.UpdateError):
            self.fetch(f'{self.DIGEST}  other.zip\n'.encode())

    @unittest.skipUnless(sys.platform == 'win32', 'the launcher module is imported on Windows only')
    def test_launcher_wrapper_and_three_argument_form(self):
        import inspect
        import port_launcher_win as launcher
        with mock.patch('urllib.request.urlopen', side_effect=self.http(404)):
            self.assertIsNone(launcher.release_checksum(self.URL))
        with mock.patch('urllib.request.urlopen', return_value=FakeResponse(b'<html>')):
            with self.assertRaises(pu.UpdateError):
                launcher.release_checksum(self.URL)
        with mock.patch('urllib.request.urlopen', side_effect=self.http(500)):
            with self.assertRaises(pu.urllib.error.HTTPError):
                launcher.release_checksum(self.URL)
        # v0.9.0 starts the new launcher with `--install-update TARGET PID` only
        self.assertEqual(inspect.signature(launcher.install_update).parameters['old_version'].default, 'previous')
        with mock.patch.object(launcher, 'ctypes') as ctypes_mock, \
                mock.patch.object(pu, 'install_and_watch', return_value=0) as run:
            ctypes_mock.windll.kernel32.OpenProcess.return_value = 0
            self.assertEqual(launcher.install_update(str(Path('t')), '123'), 0)
        self.assertEqual(run.call_args.args[2], 'previous')


if __name__ == '__main__':
    unittest.main()
