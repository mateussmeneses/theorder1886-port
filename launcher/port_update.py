"""Transactional self-update: verified download, program files swapped through a backup folder,
rollback when anything fails or the new version does not open. Pure functions, no network unless
an opener is given (tests pass local ones). Player files (USER_FILES) are never moved.

Layout while an update is pending, inside the installation folder:
  .update-pending.json   journal: phase 'moving' (half done) or 'installed' (waiting for the new launcher)
  backup-<version>/      the previous program files, same relative paths
  files.manifest         relative paths of every program file of that version (written by package.sh)

The checksum parsing and the .partial download follow the idea of SpartanPort's updater
(launcher-rust/src/core/update.rs, GPL-compatible); the backup/rollback flow is our own.
"""
import hashlib
import json
import os
import re
import shutil
import time
import urllib.request
import zipfile
from pathlib import Path

MANIFEST = 'files.manifest'
PENDING = '.update-pending.json'
BACKUP_PREFIX = 'backup-'


class UpdateError(Exception):
    pass


def parse_checksum(text):
    """The 64 hex digits of a `sha256sum` line (`<hash>  <name>`), lower case, or None."""
    match = re.match(r'\s*([0-9a-fA-F]{64})(?:\s|$)', text or '')
    return match.group(1).lower() if match else None


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


def _program_path(rel, user_files):
    top = rel.replace('\\', '/').split('/')[0]
    return top not in user_files and not top.startswith(BACKUP_PREFIX) and top != PENDING


def program_files(root, user_files):
    """Relative program files of an installation or package: its manifest when present, otherwise
    everything except player files, backups and the journal."""
    root = Path(root)
    manifest = root / MANIFEST
    if manifest.is_file():
        names = [line.strip().replace('\\', '/') for line in manifest.read_text(encoding='utf-8').splitlines()]
        names = [n for n in names if n and '..' not in n.split('/') and not Path(n).is_absolute()]
    else:
        names = [p.relative_to(root).as_posix() for p in sorted(root.rglob('*')) if p.is_file()]
    return [n for n in names if _program_path(n, user_files)]


def pending(target):
    try:
        return json.loads((Path(target) / PENDING).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def _write_journal(target, journal):
    path = Path(target) / PENDING
    temporary = path.with_name(PENDING + '.tmp')
    temporary.write_text(json.dumps(journal), encoding='utf-8')
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


def apply(target, new_root, version, user_files, replace=os.replace, place=None):
    """Move the current program files to backup-<version>/ and the new ones into place. Any
    failure puts everything back and raises UpdateError. A process death leaves the journal in
    phase 'moving' for recover(). `place` puts a new file in place (default: move; the running
    updater passes shutil.copy2 because it executes from the package folder). On success the journal says 'installed' until commit()."""
    target, new_root = Path(target), Path(new_root)
    place = place or replace
    new = program_files(new_root, user_files)
    if (target / MANIFEST).is_file():
        old = program_files(target, user_files)
    else:  # an install from before manifests: only replace what the new version brings, delete nothing else
        old = [n for n in new if (target / n).is_file()]
    backup = target / f'{BACKUP_PREFIX}{version}'
    shutil.rmtree(backup, ignore_errors=True)
    journal = {'phase': 'moving', 'version': version, 'backup': backup.name, 'old': old, 'new': new}
    _write_journal(target, journal)
    try:
        for rel in old:
            source = target / rel
            if source.is_file():
                (backup / rel).parent.mkdir(parents=True, exist_ok=True)
                replace(source, backup / rel)
        for rel in new:
            (target / rel).parent.mkdir(parents=True, exist_ok=True)
            place(new_root / rel, target / rel)
    except Exception as failure:
        rollback(target)
        raise UpdateError(f'could not install the update ({failure})') from failure
    _prune_empty(target, [n for n in old if n not in new])
    _write_journal(target, {**journal, 'phase': 'installed'})


def rollback(target):
    """Undo apply(): drop the new files, bring the backup back, remove backup and journal."""
    target = Path(target)
    journal = pending(target)
    if not journal:
        return False
    backup = target / journal['backup']
    for rel in journal['new']:
        # an old file that never reached the backup is still the old one: keep it
        if rel not in journal['old'] or (backup / rel).is_file():
            (target / rel).unlink(missing_ok=True)
    for rel in journal['old']:
        saved = backup / rel
        if saved.is_file():
            (target / rel).parent.mkdir(parents=True, exist_ok=True)
            os.replace(saved, target / rel)
    _prune_empty(target, [n for n in journal['new'] if n not in journal['old']])
    shutil.rmtree(backup, ignore_errors=True)
    (target / PENDING).unlink(missing_ok=True)
    return True


def recover(target):
    """At start-up: an update that died halfway ('moving') is rolled back. True when it was."""
    journal = pending(target)
    return bool(journal and journal.get('phase') == 'moving' and rollback(target))


def commit(target):
    """The new version opened: delete the backup and the journal. Nothing pending = nothing done."""
    journal = pending(target)
    if journal and journal.get('phase') == 'installed':
        shutil.rmtree(Path(target) / journal['backup'], ignore_errors=True)
        (Path(target) / PENDING).unlink(missing_ok=True)


def wait_for_commit(target, alive, timeout, sleep=time.sleep, clock=time.monotonic):
    """Watch an installed update until the new launcher commits (True), exits or times out (False)."""
    start = clock()
    while pending(target) is not None:
        if not alive() or clock() - start > timeout:
            return pending(target) is None
        sleep(0.5)
    return True
