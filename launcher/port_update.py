"""Transactional self-update: verified download, program files swapped without ever leaving a path
empty, rollback when anything fails or the new version does not open. Pure functions, no network
unless an opener is given (tests pass local ones). Player files (USER_FILES) are never touched.

Layout while an update is pending, inside the installation folder:
  .update-pending.json   journal: phase 'moving' (half done), 'installed' (waiting for the new
                         launcher to commit) or 'rollback' (being undone); fsynced
  backup-<version>/      COPIES of the previous program files, same relative paths
  files.manifest         relative paths of every program file of that version (written by package.sh)
  .update.lock           held by the running updater; recover() stays away while it is fresh

Swap order: copy old files to the backup, then replace each new file over its old one (copy to
`<name>.upd`, os.replace), the launcher exe last, then delete files the new version dropped. A crash
at any point leaves a launcher exe on disk and a journal that recover() undoes.

The checksum parsing and the .partial download follow the idea of SpartanPort's updater
(launcher-rust/src/core/update.rs, GPL-compatible); the rest is our own.
"""
import errno
import filecmp
import hashlib
import json
import os
import re
import shutil
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath, PureWindowsPath

MANIFEST = 'files.manifest'
PENDING = '.update-pending.json'
DONE = PENDING + '.done'  # a committed journal whose backup is still being deleted
LOCK = '.update.lock'
BACKUP_PREFIX = 'backup-'
EXE = 'The Order 1886.exe'
MAX_STARTS = 2  # unsupervised starts of an 'installed' update before recover() gives up on it
LOCK_STALE = 900  # seconds after which a lock whose updater died is ignored
WATCH_SECONDS = 300  # how long a live but silent new launcher gets to commit


class UpdateError(Exception):
    pass


class Fatal(UpdateError):
    """An update failure that retrying cannot fix."""


class RestoreFailed(Fatal):
    """The previous files could not be put back; the backup is kept untouched."""


class NoSpace(Fatal):
    pass


class RuntimeChanged(Fatal):
    """The Python runtime differs: an in-place swap could leave a launcher that cannot start."""


def parse_checksum(text):
    """The 64 hex digits of a `sha256sum` line (`<hash>  <name>`), lower case, or None."""
    match = re.match(r'\s*([0-9a-fA-F]{64})(?:\s|$)', (text or '').lstrip('﻿'))
    return match.group(1).lower() if match else None


def checksum_for(text, zip_name):
    """Hash for `zip_name` from the text of a .sha256 file; UpdateError when it is not one."""
    line = next((l for l in (text or '').lstrip('﻿').splitlines() if l.strip()), '')
    match = re.fullmatch(r'\s*([0-9a-fA-F]{64})(?:\s+\*?(\S.*?))?\s*', line)
    if not match:
        raise UpdateError('the release checksum file is not a valid sha256 file')
    name = match.group(2)
    if name and name != zip_name:
        raise UpdateError(f'the release checksum is for {name}, not {zip_name}')
    return match.group(1).lower()


def fetch_checksum(zip_url, opener=None):
    """sha256 from the `<zip>.sha256` release asset. None only when it does not exist (HTTP 404);
    a file that cannot be read or fetched is an error, never 'absent'."""
    request = urllib.request.Request(zip_url + '.sha256', headers={'User-Agent': 'theorder1886-launcher'})
    try:
        with (opener or urllib.request.urlopen)(request, timeout=15) as response:
            body = response.read(4096)
    except urllib.error.HTTPError as failure:
        if failure.code == 404:
            return None
        raise
    name = urllib.parse.unquote(urllib.parse.urlparse(zip_url).path.rsplit('/', 1)[-1])
    return checksum_for(body.decode('utf-8-sig', 'replace'), name)


def verify_sha256(path, expected):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    if digest.hexdigest() != expected.lower():
        raise UpdateError('the download does not match its sha256 checksum')


def download(url, dest, opener=urllib.request.urlopen, progress=None):
    """Download to `<dest>.partial`, check the size against Content-Length, then rename to `dest`."""
    dest = Path(dest)
    partial = dest.with_name(dest.name + '.partial')
    request = urllib.request.Request(url, headers={'User-Agent': 'theorder1886-launcher'})
    try:
        with opener(request, timeout=60) as response, open(partial, 'wb') as out:
            total, done, shown = int(response.headers.get('Content-Length') or 0), 0, -1
            while chunk := response.read(1 << 20):
                out.write(chunk)
                done += len(chunk)
                percent = done * 100 // total if total else 0
                if progress and percent != shown:
                    shown = percent
                    progress(percent)
        if total and done != total:
            raise UpdateError(f'the download is incomplete ({done} of {total} bytes)')
        os.replace(partial, dest)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    return dest


def extract(archive, dest, marker=None):
    """Extract a ZIP (rejecting corrupt ones and paths that leave `dest`); returns the folder that
    holds `marker` (the package root), or `dest` when no marker is given."""
    dest = Path(dest).resolve()
    try:
        with zipfile.ZipFile(archive) as package:
            bad = package.testzip()
            if bad:
                raise UpdateError(f'the downloaded archive is corrupt ({bad})')
            for name in package.namelist():
                if not (dest / name).resolve().is_relative_to(dest):
                    raise UpdateError(f'unsafe path in the archive: {name}')
            package.extractall(dest)
    except zipfile.BadZipFile as failure:
        raise UpdateError(f'the downloaded archive is not a valid ZIP ({failure})') from failure
    if marker is None:
        return dest
    root = next((p.parent for p in dest.rglob(marker)), None)
    if root is None:
        raise UpdateError(f'{marker} is missing from the download')
    return root


def _clean(rel):
    """A manifest entry as a safe relative POSIX path; UpdateError for drives, roots and '..'."""
    rel = rel.strip().replace('\\', '/')
    windows = PureWindowsPath(rel)
    if (windows.drive or windows.root or PurePosixPath(rel).is_absolute() or '..' in rel.split('/')
            or rel.startswith('/')):
        raise UpdateError(f'unsafe path in the file manifest: {rel}')
    return rel


def _is_player_file(rel, user_files):
    top = rel.split('/')[0].casefold()
    return (top in {u.casefold() for u in user_files} or top.startswith(BACKUP_PREFIX)
            or top in (PENDING, DONE, LOCK) or rel.casefold().endswith(('.upd', '.partial')))


def program_files(root, user_files):
    """Relative program files of an installation or package: its manifest when present, otherwise
    everything except player files, backups and updater state."""
    root = Path(root)
    manifest = root / MANIFEST
    if manifest.is_file():
        names = [_clean(line) for line in manifest.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    else:
        names = [p.relative_to(root).as_posix() for p in sorted(root.rglob('*')) if p.is_file()]
    base = root.resolve()
    for rel in names:
        if not (base / rel).resolve().is_relative_to(base):
            raise UpdateError(f'unsafe path in the file manifest: {rel}')
    return [n for n in names if not _is_player_file(n, user_files)]


def pending(target):
    """The journal, None when there is none. A journal that exists but cannot be read is an error."""
    path = Path(target) / PENDING
    try:
        text = path.read_text(encoding='utf-8')
    except FileNotFoundError:
        return None
    except OSError as failure:
        raise UpdateError(f'the update journal {path} cannot be read ({failure})') from failure
    try:
        journal = json.loads(text)
        if not isinstance(journal, dict) or not {'phase', 'backup', 'old', 'new'} <= journal.keys():
            raise ValueError('missing fields')
    except ValueError as failure:
        raise UpdateError(f'the update journal {path} is damaged ({failure}); the previous version, if '
                          f'any, is in a backup-* folder next to it') from failure
    return journal


def _write_journal(target, journal):
    path = Path(target) / PENDING
    temporary = path.with_name(PENDING + '.tmp')
    with open(temporary, 'w', encoding='utf-8') as out:
        json.dump(journal, out)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, path)


def _prune_empty(target, names):
    for name in sorted({str(Path(n).parent) for n in names}, key=len, reverse=True):
        folder = Path(target) / name
        while folder != Path(target) and folder.is_dir():
            try:
                folder.rmdir()
            except OSError:
                break
            folder = folder.parent


def _put(copy, replace, source, dest):
    """Copy `source` next to `dest` and swap it in: `dest` is never missing."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.with_name(dest.name + '.upd')
    copy(source, temporary)
    replace(temporary, dest)


def _same(a, b):
    """Same bytes (timestamps can tie on coarse clocks, so they prove nothing)."""
    try:
        return filecmp.cmp(a, b, shallow=False)
    except OSError:
        return False


def _launcher_last(names):
    """Stable order with files.manifest, then the launcher exe, last."""
    def key(n):
        return 2 if n.casefold() == EXE.casefold() else 1 if n == MANIFEST else 0
    return sorted(names, key=key)


def _python_runtime(root):
    return {p.name.casefold() for p in (Path(root) / '_internal').glob('python3*.dll')}


def apply(target, new_root, version, user_files, replace=os.replace, copy=shutil.copy2,
          free_space=lambda path: shutil.disk_usage(path).free):
    """Install the package in `new_root` over `target`, keeping COPIES of the replaced files in
    backup-<version>/. Any failure puts everything back and raises UpdateError (never OSError); a
    failure to put back raises RestoreFailed and keeps the backup. A process death leaves the journal
    for recover(). On success the journal says 'installed' until commit()."""
    target, new_root = Path(target), Path(new_root)
    backup = target / f'{BACKUP_PREFIX}{version}'
    filecmp.clear_cache()
    # An update left pending (e.g. by a failed retry) is undone first; if that fails, stop: its backup
    # is the only good copy of the previous version and must not be overwritten.
    leftover = None
    try:
        leftover = pending(target)
        if leftover:
            rollback(target)
    except Exception as failure:
        raise RestoreFailed(f'a previous update could not be undone ({failure}); the previous files are '
                            f'kept in {target / (leftover or {}).get("backup", "backup-*")}') from failure
    try:
        new = program_files(new_root, user_files)
        old = program_files(target, user_files) if (target / MANIFEST).is_file() else []
        old = old + [n for n in new if n not in old and (target / n).is_file()]  # user files at shipped paths too
        # the launcher exe goes last, right after the manifest: until then the old exe still has its files
        order = _launcher_last(new)
        old_runtime, new_runtime = _python_runtime(target), _python_runtime(new_root)
        if old_runtime and new_runtime and old_runtime != new_runtime:
            raise RuntimeChanged('this release ships a different Python runtime; it cannot be swapped in place')
        needed = sum((target / n).stat().st_size for n in old if (target / n).is_file())             + max([(new_root / n).stat().st_size for n in new] or [0]) + (64 << 20)
        available = free_space(target)
        if available < needed:
            raise NoSpace(f'not enough free disk space ({available >> 20} MB free, {needed >> 20} MB needed)')
    except OSError as failure:
        raise UpdateError(f'could not read the update ({failure})') from failure
    kept = {n.casefold() for n in order}
    try:
        shutil.rmtree(backup, ignore_errors=True)
        journal = {'phase': 'moving', 'version': version, 'backup': backup.name, 'old': old, 'new': order,
                   'starts': 0}
        _write_journal(target, journal)
        for rel in old:
            if (target / rel).is_file():
                touch_lock(target)
                (backup / rel).parent.mkdir(parents=True, exist_ok=True)
                copy(target / rel, backup / rel)
        for rel in order:
            touch_lock(target)
            _put(copy, replace, new_root / rel, target / rel)
        for rel in old:
            if rel.casefold() not in kept:  # a case-only rename is the same file, not an orphan
                (target / rel).unlink(missing_ok=True)
        _prune_empty(target, [n for n in old if n.casefold() not in kept])
        _write_journal(target, {**journal, 'phase': 'installed'})
    except Exception as failure:
        try:
            rollback(target)
        except Exception as second:
            raise RestoreFailed(f'could not install the update ({failure}) and could not restore the previous '
                                f'files ({second}); they are in {backup}') from failure
        if isinstance(failure, OSError) and failure.errno == errno.ENOSPC:
            raise NoSpace(f'the disk is full ({failure}); the previous version was restored '
                          f'(backup folder: {backup})') from failure
        raise UpdateError(f'could not install the update ({failure}); the previous version was restored '
                          f'(backup folder: {backup})') from failure


def _finish(target, journal, sleep=time.sleep):
    """The commit point: the journal goes first, then the backup it pointed to. The rename is retried
    while something (antivirus, a watcher reading the journal) holds the file."""
    target = Path(target)
    for attempt in range(8):
        try:
            os.replace(target / PENDING, target / DONE)
            break
        except PermissionError:
            if attempt == 7:
                raise
            sleep(0.25)
    shutil.rmtree(target / journal['backup'], ignore_errors=True)
    (target / DONE).unlink(missing_ok=True)


def rollback(target):
    """Undo apply(): restore the old files from the backup, drop files only the new version has,
    remove backup and journal. Raises OSError when a file is held; the journal then stays for recover()."""
    target = Path(target)
    filecmp.clear_cache()
    journal = pending(target)
    if not journal:
        return False
    if journal['phase'] != 'rollback':
        _write_journal(target, {**journal, 'phase': 'rollback'})
    backup = target / journal['backup']
    for rel in _launcher_last(journal['old']):  # the launcher goes back last, as it went in last
        saved = backup / rel
        if saved.is_file() and not _same(saved, target / rel):
            _put(shutil.copy2, os.replace, saved, target / rel)
    old_only = {n.casefold() for n in journal['old']}
    for rel in journal['new']:
        if rel.casefold() not in old_only:
            (target / rel).unlink(missing_ok=True)
        (target / rel).with_name(Path(rel).name + '.upd').unlink(missing_ok=True)
    _prune_empty(target, [n for n in journal['new'] if n.casefold() not in old_only])
    _finish(target, journal)
    return True


def safe_rollback(target, tries=10, sleep=time.sleep):
    """rollback() with retries for files still held by a dying process. True only when it worked."""
    for attempt in range(tries):
        try:
            return bool(rollback(target))
        except (OSError, UpdateError):
            if attempt + 1 < tries:
                sleep(1)
    return False


def lock_held(target, now=time.time):
    try:
        return now() - os.stat(Path(target) / LOCK).st_mtime < LOCK_STALE
    except OSError:
        return False


def acquire_lock(target):
    path = Path(target) / LOCK
    for _ in range(3):
        try:
            os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY))
            return
        except FileExistsError:
            if lock_held(target):
                raise UpdateError('another update is already running')
            # left by an updater that died: one rename wins the takeover, the create above stays exclusive
            gone = path.with_name(f'{LOCK}.stale-{os.getpid()}')
            try:
                os.rename(path, gone)
                gone.unlink(missing_ok=True)
            except OSError:
                pass
    raise UpdateError('could not take the update lock')


def touch_lock(target):
    try:
        os.utime(Path(target) / LOCK)
    except OSError:
        pass


def release_lock(target):
    (Path(target) / LOCK).unlink(missing_ok=True)


def recover(target):
    """At start-up, unless an updater is running: finish a committed update's cleanup, roll back an
    update that died halfway ('moving'/'rollback'), or one that never committed after MAX_STARTS
    starts ('installed'). True when it rolled back."""
    target = Path(target)
    if lock_held(target):
        return False
    try:
        leftover = json.loads((target / DONE).read_text(encoding='utf-8'))
        shutil.rmtree(target / leftover['backup'], ignore_errors=True)
    except (OSError, ValueError, KeyError, TypeError):
        pass
    (target / DONE).unlink(missing_ok=True)
    journal = pending(target)
    if not journal:
        return False
    if journal['phase'] == 'installed':
        starts = journal.get('starts', 0) + 1
        if starts <= MAX_STARTS:
            _write_journal(target, {**journal, 'starts': starts})
            return False
    return bool(rollback(target))


def commit(target, sleep=time.sleep):
    """The new version opened: drop journal then backup. Nothing pending = nothing done."""
    journal = pending(target)
    if journal and journal['phase'] == 'installed':
        _finish(target, journal, sleep)


def commit_quietly(target):
    """commit() for the start-up path of a windowless app: a damaged journal or a held file must not
    crash it. False when it failed (the next start tries again)."""
    try:
        commit(target)
        return True
    except (OSError, UpdateError):
        return False


def wait_for_commit(target, alive, timeout=WATCH_SECONDS, sleep=time.sleep, clock=time.monotonic):
    """Watch an installed update: 'committed', 'exited' (the new launcher died first) or 'timeout'
    (alive but silent for `timeout` seconds)."""
    start = clock()
    while pending(target) is not None:
        if not alive():
            return 'committed' if pending(target) is None else 'exited'
        if clock() - start > timeout:
            return 'timeout'
        touch_lock(target)
        sleep(0.5)
    return 'committed'


def install_and_watch(target, new_root, old_version, user_files, launch, notify, apply_tries=30,
                      retry_sleep=time.sleep, timeout=WATCH_SECONDS, sleep=time.sleep, clock=time.monotonic):
    """The whole update from the new launcher's side. `launch()` starts the installed launcher and
    returns a Popen-like object; `notify(kind, detail)` tells the user: 'failed', 'restored',
    'restored_slow', 'restore_failed'. Returns the exit code."""
    target = Path(target)
    try:
        acquire_lock(target)
    except (UpdateError, OSError) as failure:
        notify('failed', str(failure))
        return 1
    try:
        failure = None
        for _ in range(apply_tries):
            try:
                apply(target, new_root, old_version, user_files)
                break
            except RestoreFailed:  # never retry: the backup is the only good copy
                notify('restore_failed', str(target))
                return 1
            except Fatal as error:  # no space, different runtime: retrying cannot help
                notify('failed', str(error))
                return 1
            except (UpdateError, OSError) as error:  # e.g. a file still in use while the old launcher closes
                failure = error
                retry_sleep(1)
        else:
            notify('failed', str(failure))
            return 1
        try:
            child = launch()
        except Exception:  # e.g. the exe was quarantined
            if safe_rollback(target, sleep=retry_sleep):
                notify('launch_failed', '')
                try:
                    launch()
                except Exception:
                    pass
            else:
                notify('restore_failed', str(target))
            return 1
        result = wait_for_commit(target, lambda: child.poll() is None, timeout, sleep=sleep, clock=clock)
        if result == 'committed':
            return 0
        if child.poll() is None:
            child.kill()
            child.wait()
        if safe_rollback(target, sleep=retry_sleep):
            notify('restored' if result == 'exited' else 'restored_slow', '')
            try:
                launch()
            except Exception:
                pass
        else:
            notify('restore_failed', str(target))
        return 1
    finally:
        release_lock(target)
