r"""The optional updater.

When the start window opens it asks GitHub for the newest release of this program. If that release is newer than
the installed version, the start window offers it: Update now, Later, or Skip this version. Nothing is ever
installed without that click, and "Check for a new version" in Settings turns the question off altogether.
Nothing is sent except the request itself: no identifier, no version number, nothing about this computer.

What a release carries (made by release.sh, uploaded by hand):
    ExviusRevivalDesktop-win64-<version>.zip     the full package, for a first install
    ExviusRevivalDesktop-update-<version>.zip    the program's own files only
    update.json                                  the version, the update zip's name, size and SHA-256, a summary

"Update now", step by step:
  1. update.json says which file to fetch. The zip is downloaded into update\download, and its size and SHA-256
     are compared with update.json. A file that does not match is deleted and nothing else happens.
  2. The zip is unpacked into update\staged\files, inside this program's folder. Every name in it is checked
     first: an update may only hold the program's own top-level items (PROGRAM_ITEMS below). A zip that names
     anything else (client, save, data, logs, settings.json ...) is refused whole.
  3. update\staged\apply.txt is written last. It lists the items to replace.
  4. The start window closes and starts "Exvius Revival Desktop.exe" again. That small program (stub/stub.c) does
     the replacing, because by then nothing of this program is running any more: Python, the game window and the
     start window cannot be replaced while they run. For each item it moves the installed one into
     update\backup\<old version> and the new one into its place, writing each move to update\journal.txt first.
     If a move fails, every move made so far is undone and the old version starts again; if the computer is
     switched off half-way, the next start finds the journal and undoes it the same way.
  5. It then starts the new version, which reads update\result.txt, says what happened, and clears update\download
     and update\staged. The backup of the replaced files stays until the next update.

client\, save\, data\, logs\, pictures\ and settings.json are never touched: they are not in PROGRAM_ITEMS, and
both this file and the stub check each name against that list.

For tests, EXVIUS_DESKTOP_UPDATE_URL points the updater at another place: a folder or an http(s) address that
holds update.json and the zip beside it.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPOSITORY = 'CreamySeaman6/Exvius-Revival-Desktop'
LATEST_API = f'https://api.github.com/repos/{REPOSITORY}/releases/latest'
LATEST_FILES = f'https://github.com/{REPOSITORY}/releases/latest/download/'
RELEASES_PAGE = f'https://github.com/{REPOSITORY}/releases/latest'
INFO_NAME = 'update.json'
STUB_NAME = 'Exvius Revival Desktop.exe'
# The program's own files: the only things an update may replace. stub/stub.c has the same list.
FOLDER_ITEMS = ('app', 'runner', 'server', 'source', 'python')
FILE_ITEMS = (STUB_NAME, 'README.txt', 'LICENSES.txt', 'QUICK START.txt')
PROGRAM_ITEMS = FOLDER_ITEMS + FILE_ITEMS
# What every update must bring, so a wrong or cut-off zip is never put in place.
REQUIRED = ('app/launcher.py', 'app/version.py', 'app/updater.py', 'runner/runner.exe', 'runner/game.elf',
            'server/serve.py', 'server/latest.json', STUB_NAME)
TIMEOUT = 6.0                       # seconds for each request of the check
HEADERS = {'User-Agent': 'ExviusRevivalDesktop', 'Accept': 'application/vnd.github+json'}


class UpdateError(Exception):
    """A problem to show to the person as it is. Nothing has been changed when it is raised."""


def parse_version(text) -> tuple | None:
    """'0.2.0' or 'v0.2.0' -> (0, 2, 0); None for anything that is not numbers and dots."""
    text = str(text or '').strip().lstrip('vV')
    parts = text.split('.')
    if not text or len(parts) > 4 or not all(p.isdigit() and len(p) <= 6 for p in parts):
        return None
    return tuple(int(p) for p in parts) + (0,) * (4 - len(parts))


def newer(candidate, installed) -> bool:
    a, b = parse_version(candidate), parse_version(installed)
    return a is not None and b is not None and a > b


def folder(home: Path) -> Path:
    return home / 'update'


# ----------------------------------------------------------------------------------------------------
# The check
# ----------------------------------------------------------------------------------------------------
def _get(address: str, timeout: float, limit: int = 2 << 20) -> bytes:
    request = urllib.request.Request(address, headers=HEADERS if address.startswith('http') else {})
    with urllib.request.urlopen(request, timeout=timeout) as reply:
        body = reply.read(limit + 1)
    if len(body) > limit:
        raise ValueError('the answer is too long')
    return body


def _test_base() -> str | None:
    """EXVIUS_DESKTOP_UPDATE_URL: a folder or an address to look in instead of the release page (for tests)."""
    given = os.environ.get('EXVIUS_DESKTOP_UPDATE_URL', '').strip()
    if not given:
        return None
    if '://' not in given:
        given = Path(given).resolve().as_uri()
    return given.rstrip('/') + '/'


def _read_info(raw: bytes) -> dict:
    info = json.loads(raw.decode('utf-8'))
    update = info.get('update') if isinstance(info, dict) else None
    if not isinstance(update, dict) or parse_version(info.get('version')) is None:
        raise ValueError('update.json has no version or no update entry')
    name, size, digest = str(update.get('file') or ''), update.get('size'), str(update.get('sha256') or '').lower()
    if (not name or '/' in name or '\\' in name or not name.lower().endswith('.zip') or not isinstance(size, int) or size <= 0
            or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest)):
        raise ValueError('update.json does not describe the update file')
    return {'version': str(info['version']).strip().lstrip('vV'), 'summary': ' '.join(str(info.get('summary') or '').split())[:400],
            'file': name, 'size': size, 'sha256': digest, 'needs': str(info.get('needs') or '').strip()}


def latest(timeout: float = TIMEOUT, log=lambda text: None) -> dict | None:
    """What the newest release offers: {'version', 'summary', 'file', 'size', 'sha256', 'needs', 'url', 'page'}.
    None when there is no answer (offline), no release, or a release without update.json. Never raises."""
    base = _test_base()
    try:
        if base:
            info = _read_info(_get(base + INFO_NAME, timeout))
            info.update(url=base + info['file'], page=base)
            return info
        files = {}
        try:
            release = json.loads(_get(LATEST_API, timeout).decode('utf-8'))
            files = {str(a.get('name')): str(a.get('browser_download_url')) for a in release.get('assets') or [] if isinstance(a, dict)}
        except urllib.error.HTTPError as exc:
            if exc.code == 404:                                 # no release published
                log('update check: no release found')
                return None
            log(f'update check: the release list answered {exc.code}; asking for the file directly')
        if files and INFO_NAME not in files:
            log('update check: the newest release has no update.json, so it is not offered')
            return None
        info = _read_info(_get(files.get(INFO_NAME) or LATEST_FILES + INFO_NAME, timeout))
        if files and info['file'] not in files:
            log(f"update check: the newest release does not hold {info['file']}, so it is not offered")
            return None
        info.update(url=files.get(info['file']) or LATEST_FILES + info['file'], page=RELEASES_PAGE)
        return info
    except (OSError, ValueError) as exc:                        # offline, a timeout, an answer that is not what was expected
        log(f'update check: skipped ({type(exc).__name__}: {exc})')
        return None


def offer(installed: str, skipped: str = '', timeout: float = TIMEOUT, log=lambda text: None) -> dict | None:
    """The update to offer, or None. `skipped` is the version the person chose to skip."""
    info = latest(timeout, log)
    if info is None:
        return None
    if not newer(info['version'], installed):
        log(f"update check: {installed} is installed, the newest release is {info['version']}")
        return None
    if skipped and parse_version(skipped) == parse_version(info['version']):
        log(f"update check: {info['version']} is out and was skipped earlier")
        return None
    # A release can say that versions before some number cannot take it through the updater (`needs`).
    info['by_hand'] = bool(info['needs']) and newer(info['needs'], installed)
    log(f"update check: {info['version']} is out ({installed} installed)" + (', to be installed by hand' if info['by_hand'] else ''))
    return info


# ----------------------------------------------------------------------------------------------------
# Download and unpack
# ----------------------------------------------------------------------------------------------------
def download(home: Path, info: dict, report=lambda done, total: None) -> Path:
    """Fetch the update zip into update/download and check its size and SHA-256."""
    target_folder = folder(home) / 'download'
    shutil.rmtree(folder(home) / 'staged', ignore_errors=True)      # a new try replaces whatever an earlier one unpacked
    shutil.rmtree(target_folder, ignore_errors=True)
    target_folder.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(target_folder).free
    if free < info['size'] * 4 + (64 << 20):
        raise UpdateError(f"There is not enough free space here for the update ({info['size'] * 4 // (1 << 20) + 64} MB is needed).")
    target, part = target_folder / info['file'], target_folder / (info['file'] + '.part')
    digest, done = hashlib.sha256(), 0
    request = urllib.request.Request(info['url'], headers={'User-Agent': HEADERS['User-Agent']} if info['url'].startswith('http') else {})
    try:
        with urllib.request.urlopen(request, timeout=30) as reply, open(part, 'wb') as out:
            while True:
                block = reply.read(1 << 18)
                if not block:
                    break
                done += len(block)
                if done > info['size']:
                    break
                out.write(block)
                digest.update(block)
                report(done, info['size'])
    except OSError as exc:
        part.unlink(missing_ok=True)
        raise UpdateError(f'The update could not be downloaded ({exc}). Nothing was changed.') from exc
    if done != info['size'] or digest.hexdigest() != info['sha256']:
        part.unlink(missing_ok=True)
        raise UpdateError('The downloaded update is not the file the release describes (its size or checksum is different), '
                          'so it was not installed. Nothing was changed. Try again later.')
    part.replace(target)
    return target


def _entries(archive: zipfile.ZipFile) -> list[tuple[zipfile.ZipInfo, str]]:
    """(entry, its path below the package's top folder) for every file, after checking every name."""
    infos = [i for i in archive.infolist() if not i.is_dir()]
    if not infos:
        raise UpdateError('The update file is empty.')
    for i in infos:
        parts = i.filename.split('/')
        if i.filename.startswith('/') or '\\' in i.filename or ':' in i.filename or '..' in parts or '' in parts:
            raise UpdateError('The update file has an unsafe name inside it: ' + i.filename)
    tops = {i.filename.split('/', 1)[0] for i in infos}
    strip = len(tops) == 1 and all('/' in i.filename for i in infos) and next(iter(tops)) not in PROGRAM_ITEMS
    out = [(i, i.filename.split('/', 1)[1] if strip else i.filename) for i in infos]
    for _, name in out:
        top = name.split('/', 1)[0]
        if not (name in FILE_ITEMS or (top in FOLDER_ITEMS and '/' in name)):
            raise UpdateError('The update file holds something that is not part of this program: ' + name)
    return out


def stage(home: Path, archive_path: Path, info: dict, installed: str, report=lambda done, total: None) -> list[str]:
    """Unpack the checked zip into update/staged/files and write update/staged/apply.txt. Returns the items to replace."""
    staged = folder(home) / 'staged'
    files = staged / 'files'
    shutil.rmtree(staged, ignore_errors=True)
    if staged.exists():
        raise UpdateError('The folder update\\staged could not be cleared. Close anything that has it open and try again.')
    files.mkdir(parents=True)
    try:
        with zipfile.ZipFile(archive_path) as archive:
            entries = _entries(archive)
            names = {name for _, name in entries}
            missing = [name for name in REQUIRED if name not in names]
            if missing:
                raise UpdateError('The update file is not complete (it lacks ' + ', '.join(missing) + ').')
            total, done = sum(i.file_size for i, _ in entries), 0
            for entry, name in entries:
                target = files.joinpath(*name.split('/'))
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as source, open(target, 'wb') as out:
                    shutil.copyfileobj(source, out, 1 << 20)
                done += entry.file_size
                report(done, total)
    except (zipfile.BadZipFile, OSError) as exc:
        shutil.rmtree(staged, ignore_errors=True)
        raise UpdateError(f'The update could not be unpacked ({exc}). Nothing was changed.') from exc
    except UpdateError:
        shutil.rmtree(staged, ignore_errors=True)
        raise
    # The update's own version file must say the version the release promised.
    stated = {}
    try:
        exec((files / 'app' / 'version.py').read_text(encoding='utf-8'), stated)  # noqa: S102 - one assignment, from the checked zip
    except Exception:  # noqa: BLE001
        stated = {}
    if parse_version(stated.get('VERSION')) != parse_version(info['version']):
        shutil.rmtree(staged, ignore_errors=True)
        raise UpdateError(f"The update file says it is version {stated.get('VERSION')}, not {info['version']}. Nothing was changed.")
    # An update zip made for copying over by hand may hold a few files of python\ only. The folder is replaced
    # whole or not at all: an incomplete one is left out and the installed Python stays.
    if (files / 'python').exists() and not (files / 'python' / 'python.exe').is_file():
        shutil.rmtree(files / 'python')
    items = [name for name in PROGRAM_ITEMS if (files / name).exists()]
    backup = folder(home) / 'backup'
    shutil.rmtree(backup, ignore_errors=True)                   # one backup is kept: the version being replaced now
    (backup / installed).mkdir(parents=True, exist_ok=True)
    lines = ['exvius-update 1', f'from {installed}', f"to {info['version']}"] + [f'item {name}' for name in items]
    (staged / 'apply.txt.new').write_text('\n'.join(lines) + '\n', encoding='ascii')
    (staged / 'apply.txt.new').replace(staged / 'apply.txt')    # written last: its presence is what starts the replacing
    return items


def staged_version(home: Path) -> str | None:
    """The version waiting in update/staged to be put in place, or None."""
    try:
        for line in (folder(home) / 'staged' / 'apply.txt').read_text(encoding='ascii').splitlines():
            if line.startswith('to '):
                return line[3:].strip()
    except (OSError, ValueError):
        pass
    return None


def hand_over(home: Path) -> None:
    """Start the small program that replaces the files once this one has closed. The caller then exits."""
    stub = home / STUB_NAME
    if os.name != 'nt' or not stub.is_file():
        raise UpdateError('The update is unpacked in update\\staged, but it can only be put in place by '
                          f'"{STUB_NAME}" on Windows.')
    flags = 0x00000008 | 0x00000200                              # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    subprocess.Popen([str(stub), '--apply-after', str(os.getpid())], cwd=str(home), creationflags=flags, close_fds=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ----------------------------------------------------------------------------------------------------
# After the replacing
# ----------------------------------------------------------------------------------------------------
def result(home: Path) -> dict | None:
    """What the last replacing ended as, read once: {'ok', 'from', 'to', 'reason'}; None when there was none.
    After a good one the downloaded zip and the unpacked copy are cleared; the backup stays."""
    path = folder(home) / 'result.txt'
    try:
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return None
    try:
        path.unlink()
    except OSError:
        pass
    if len(lines) < 3 or lines[0] not in ('ok', 'failed'):
        return None
    out = {'ok': lines[0] == 'ok', 'from': lines[1].strip(), 'to': lines[2].strip(), 'reason': ' '.join(lines[3:]).strip()}
    shutil.rmtree(folder(home) / 'download', ignore_errors=True)
    shutil.rmtree(folder(home) / 'staged', ignore_errors=True)
    return out
