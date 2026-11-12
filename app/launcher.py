r"""Exvius Revival Desktop: first-run setup and start-up.

What it does, in order:
  1. asks for the game's APK (version 9.1.0) and copies the game library and assets out of it;
  2. downloads the game data for the local server (about 14.6 GB, resumable);
  3. starts the local game server on this computer only (127.0.0.1), then the game window;
  4. stops the server when the game window closes.

Everything it creates stays inside the folder this program was unpacked to:
  client\    the game library and assets taken from your APK, the game's own downloads, and desktop.ini
  data\      the server's game data
  save\      your save and the server's records
  logs\      log files
  pictures\  pictures taken with the camera button in the game window
  settings.json  what was chosen in Settings
Deleting the folder removes all of it. Nothing is written to the registry or anywhere else.

Without a window:  launcher.py --headless [--apk FILE] [--skip-data] [--play]
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

APP = Path(__file__).resolve().parent
HOME = Path(os.environ.get('EXVIUS_DESKTOP_HOME', APP.parent))
CLIENT = HOME / 'client'
GAME = CLIENT / 'game'
DATA = Path(os.environ.get('EXVIUS_DESKTOP_DATA', HOME / 'data'))
SAVE = HOME / 'save'
LOGS = HOME / 'logs'
SERVER = HOME / 'server'
RUNNER_DIR = HOME / 'runner'
WINDOWS = os.name == 'nt'
RUNNER = RUNNER_DIR / ('runner.exe' if WINDOWS else 'runner')
SETTINGS = HOME / 'settings.json'

GAME_VERSION = '9.1.0'
LIBRARY_ENTRY = 'lib/arm64-v8a/libgame.so'
# SHA-256 of libgame.so from the 9.1.0 APK this program was built and tested with.
KNOWN_LIBRARIES = {'01d2aa87a9ce893b071a044a048af179eac3caae0147065e8ca398e1adb7d051': '9.1.0'}
# The Exvius Offline Android app's APK carries a game library changed to work inside that app (it registers the
# app's own rules class). It does not start here: the window stays black. It is recognised by that class's name.
MODIFIED_MARK = b'com/exvius/local/NativeRules'
MODIFIED_TEXT = ('This APK is the Exvius Offline Android app. Its game library was changed to work inside that app and '
                 'does not start here (the game window stays black). Exvius Revival Desktop needs the original '
                 f'FINAL FANTASY BRAVE EXVIUS {GAME_VERSION} APK, Global version: the game as it came from the app store.')
NO_WINDOW = 0x08000000 if WINDOWS else 0   # CREATE_NO_WINDOW
RESTART_CODE = 75                           # runner exit code: start the game again


class SetupError(Exception):
    """A problem to show to the person as it is."""


def settings() -> dict:
    try:
        return json.loads(SETTINGS.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_settings(values: dict) -> None:
    SETTINGS.write_text(json.dumps(values, indent=1), encoding='utf-8')


# ----------------------------------------------------------------------------------------------------
# Step 1: the APK
# ----------------------------------------------------------------------------------------------------
def game_installed() -> bool:
    return (GAME / 'libgame.so').is_file() and (GAME / 'assets').is_dir() and (CLIENT / 'installed.json').is_file()


def _open_packages(path: Path) -> list[zipfile.ZipFile]:
    """The APK itself, or every APK inside an .xapk / .apks / .apkm bundle."""
    try:
        outer = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as exc:
        raise SetupError(f'That file could not be opened as an APK ({exc}).') from exc
    names = outer.namelist()
    if LIBRARY_ENTRY in names or any(n.startswith('assets/') for n in names):
        return [outer]
    inner = [n for n in names if n.lower().endswith('.apk')]
    if not inner:
        raise SetupError('That file does not look like the game\'s APK: it has no game library and no assets inside.')
    return [zipfile.ZipFile(io.BytesIO(outer.read(n))) for n in inner]


def install_apk(path: Path, report=lambda text, done, total: None) -> dict:
    """Copy lib/arm64-v8a/libgame.so and assets/ out of the APK into client/game."""
    packages = _open_packages(path)
    library = next((p for p in packages if LIBRARY_ENTRY in p.namelist()), None)
    if library is None:
        other = sorted({n.split('/')[1] for p in packages for n in p.namelist() if n.startswith('lib/') and n.count('/') >= 2})
        hint = f' It only has: {", ".join(other)}.' if other else ''
        raise SetupError('This APK has no 64-bit ARM game library (lib/arm64-v8a/libgame.so).' + hint +
                         ' A complete (not split) 9.1.0 APK, or the full .xapk / .apks bundle, is needed.')
    entries = [(p, info) for p in packages for info in p.infolist()
               if info.filename.startswith('assets/') and not info.is_dir()]
    if not entries:
        raise SetupError('This APK has no assets folder, so it is not the complete game.')
    for _, info in entries:
        parts = info.filename.split('/')
        if '..' in parts or info.filename.startswith('/') or '\\' in info.filename:
            raise SetupError('This APK has an unsafe file name inside it: ' + info.filename)
    total = library.getinfo(LIBRARY_ENTRY).file_size + sum(info.file_size for _, info in entries)
    done = 0
    staging = CLIENT / 'game.installing'
    shutil.rmtree(staging, ignore_errors=True)
    (staging / 'assets').mkdir(parents=True)
    digest = hashlib.sha256()
    report('Copying the game library', done, total)
    modified, tail = False, b''
    with library.open(LIBRARY_ENTRY) as source, open(staging / 'libgame.so', 'wb') as out:
        while True:
            block = source.read(4 << 20)
            if not block:
                break
            out.write(block)
            digest.update(block)
            modified = modified or MODIFIED_MARK in tail + block
            tail = block[-len(MODIFIED_MARK):]
            done += len(block)
            report('Copying the game library', done, total)
    library_hash = digest.hexdigest()
    if modified:
        shutil.rmtree(staging, ignore_errors=True)
        raise SetupError(MODIFIED_TEXT)
    for package, info in entries:
        target = staging / info.filename
        target.parent.mkdir(parents=True, exist_ok=True)
        with package.open(info) as source, open(target, 'wb') as out:
            shutil.copyfileobj(source, out, 4 << 20)
        done += info.file_size
        report('Copying the game assets', done, total)
    # Keep what the game itself downloaded (client/game/files) if this is a repeat install.
    old_files = GAME / 'files'
    if old_files.is_dir():
        shutil.move(str(old_files), str(staging / 'files'))
    if (GAME / 'prefs.tsv').is_file():
        shutil.move(str(GAME / 'prefs.tsv'), str(staging / 'prefs.tsv'))
    shutil.rmtree(GAME, ignore_errors=True)
    staging.rename(GAME)
    (GAME / 'files').mkdir(exist_ok=True)
    shutil.copyfile(RUNNER_DIR / 'font.ttf', CLIENT / 'font.ttf')
    record = {'apk': path.name, 'library_sha256': library_hash, 'known_version': KNOWN_LIBRARIES.get(library_hash),
              'assets': len(entries), 'installed_at': time.strftime('%Y-%m-%d %H:%M:%S')}
    (CLIENT / 'installed.json').write_text(json.dumps(record, indent=1), encoding='utf-8')
    return record


# ----------------------------------------------------------------------------------------------------
# Step 2: the server's game data
# ----------------------------------------------------------------------------------------------------
def _use_system_certificates() -> None:
    """Verify HTTPS the way the browsers on this computer do; see net_trust.py. Without this the game-data download
    failed with CERTIFICATE_VERIFY_FAILED on PCs whose Windows had not fetched the download server's root yet."""
    import net_trust
    if not _use_system_certificates.told:
        _use_system_certificates.told = True
        log_error('https checks: ' + net_trust.apply())


_use_system_certificates.told = False


def _fetcher():
    _use_system_certificates()
    os.environ['FFBE_DATA_DIR'] = str(DATA)
    os.environ['FFBE_MANIFEST'] = str(SERVER / 'latest.json')
    if str(SERVER) not in sys.path:
        sys.path.insert(0, str(SERVER))
    import fetch_data
    fetch_data.DATA = DATA
    fetch_data.MANIFEST = SERVER / 'latest.json'
    return fetch_data


def data_status() -> tuple[bool, float]:
    """(everything installed, gigabytes still to download)."""
    manifest = json.loads((SERVER / 'latest.json').read_text(encoding='utf-8'))
    fetch_data = _fetcher()
    packs = [pack for pack in manifest['packs'] if pack['id'] not in fetch_data.SKIP]
    try:
        installed = json.loads((DATA / 'installed.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        installed = {}
    first = next((i for i, pack in enumerate(packs) if installed.get(pack['id']) != pack['sha256']), None)
    if first is None:
        return True, 0.0
    return False, sum(pack['size'] for pack in packs[first:]) / 1e9


def fetch_game_data(report=lambda line: None) -> None:
    fetch_data = _fetcher()
    seen = {'certificate': False}

    def log(message: str) -> None:
        if 'CERTIFICATE_VERIFY_FAILED' in message:
            seen['certificate'] = True
        if 'interrupted' in message or 'could not' in message:
            log_error('download: ' + message.strip())           # the reason, for logs/launcher.log
        report(message.replace('Start the server with:  docker compose up -d', '').strip())
    fetch_data.log = log
    try:
        result = fetch_data.main([])
    except SystemExit as exc:           # fetch_data reports its own failures this way
        if seen['certificate']:
            raise SetupError('The download server\'s certificate could not be checked on this computer, so nothing was '
                             'downloaded. This usually comes from antivirus "HTTPS scanning" or a network filter. Try again '
                             'with that switched off for a moment, or on another network. The details are in logs\\launcher.log.') from exc
        raise SetupError(str(exc.code) if exc.code not in (None, 0) else 'The data download stopped.') from exc
    except OSError as exc:
        raise SetupError(f'The data download failed: {exc}') from exc
    if result not in (0, None):
        raise SetupError('The data download did not finish.')


def free_space_gb(folder: Path) -> float:
    probe = folder
    while not probe.exists():
        probe = probe.parent
    return shutil.disk_usage(probe).free / 1e9


# ----------------------------------------------------------------------------------------------------
# Step 3: server and game
# ----------------------------------------------------------------------------------------------------
def release_environment() -> dict:
    """The server switches of the iPhone release (server/docker-compose.yml), so both behave the same."""
    out = {}
    compose = SERVER / 'docker-compose.yml'
    if compose.is_file():
        inside = False
        for line in compose.read_text(encoding='utf-8').splitlines():
            if re.match(r'^\s{4}environment:\s*$', line):
                inside = True
                continue
            if inside and re.match(r'^\s{4}\S', line):
                break
            found = re.match(r'^\s{6}([A-Z][A-Z0-9_]*):\s*(.*?)\s*(?:#.*)?$', line) if inside else None
            if found and not found.group(1).startswith(('TS_', 'FFBE_')):
                value = found.group(2)
                if len(value) >= 2 and value[0] == value[-1] and value[0] in '"\'':
                    value = value[1:-1]
                if '${' not in value:
                    out[found.group(1)] = value
    return out


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def python_exe(windowed: bool = False) -> str:
    bundled = HOME / 'python' / ('pythonw.exe' if windowed else 'python.exe')
    return str(bundled) if WINDOWS and bundled.is_file() else sys.executable


def _rotate(log: Path) -> None:
    if log.exists():
        older = log.with_suffix(log.suffix + '.1')
        try:
            older.unlink(missing_ok=True)
            log.rename(older)
        except OSError:
            pass


def start_server() -> tuple[subprocess.Popen, int]:
    LOGS.mkdir(exist_ok=True)
    SAVE.mkdir(exist_ok=True)
    port = free_port()
    environment = dict(os.environ)
    environment.update(release_environment())
    environment.update(settings().get('server_environment', {}))
    environment.update(settings().get('extra_environment', {}))      # the "Other settings" page wins
    environment.update({
        'FFBE_ENGINE': str(SERVER / 'engine'), 'FFBE_DATA_DIR': str(DATA), 'FFBE_SERVER_ROOT': str(SAVE),
        'FFBE_HTTP_PORT': str(port), 'FFBE_ADVERTISE_IP': '127.0.0.1', 'PYTHONUNBUFFERED': '1',
        'PYTHONDONTWRITEBYTECODE': '1', 'PYTHONUTF8': '1',
        # The desktop game is the Android game: it reads the Android sound banks as they are, so the
        # iPhone-only sound rewrite (and the file renaming that goes with it) stays off.
        'EXVIUS_IOS_HCA': '0', 'EXVIUS_IOS_HCA_PLAIN': '0', 'EXVIUS_IOS_SOUND_REFETCH': '',
    })
    log_path = LOGS / 'server.log'
    _rotate(log_path)
    log = open(log_path, 'w', encoding='utf-8', errors='replace')
    process = subprocess.Popen([python_exe(), str(APP / 'desktop_server.py'), '--port', str(port)], cwd=str(HOME),
                               env=environment, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=NO_WINDOW)
    deadline = time.time() + 180
    while time.time() < deadline:
        if process.poll() is not None:
            raise SetupError(f'The game server stopped while starting (code {process.returncode}). '
                             f'Its messages are in {log_path}.')
        try:
            with urllib.request.urlopen(f'http://127.0.0.1:{port}/__exvius/hello', timeout=2) as response:
                if response.status == 200:
                    return process, port
        except OSError:
            time.sleep(0.5)
    stop_server(process)
    raise SetupError(f'The game server did not answer within three minutes. Its messages are in {log_path}.')


def stop_server(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        process.stdin.close()            # the server exits when this pipe closes
        process.wait(timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        process.kill()


def _drop_copies_read_in_place() -> None:
    """Earlier builds had the game download its own copy of the server-built Vortex banner pack (242 MB each time the
    banners changed). It is read in place now, so those copies only take up room."""
    try:
        for old in (GAME / 'files' / 'en' / 'sd' / 'area').glob('Ver*_spdungeon1.cpk'):
            old.unlink()
        hidden = GAME / 'files-removed.txt'             # names the game deleted from its own folder; the pack must not stay hidden
        if hidden.is_file():
            lines = hidden.read_text(encoding='utf-8').splitlines()
            kept = [line for line in lines if not line.endswith('_spdungeon1.cpk')]
            if len(kept) != len(lines):
                hidden.write_text(''.join(line + '\n' for line in kept), encoding='utf-8')
    except OSError:
        pass


# The unit conversion table (F_SACRIFICE_MST) is loaded on demand, so it only works once its list is in the game's
# table registry. The 9.1.0 library registers it only when FUNCTION_SP_CLASSUP is valid (0x1d3d154 bl
# UnitClassupUtils::isEnableSPClassup; 0x1d3d158 tbz w0 -> skip), and both places that register the lists (the
# registry's creation and TitleScene::initConnect) run before F_FUNCTION_MST is loaded. So the table never loads:
# Convert to Prism / Fragments lists nothing and NV awakening shows "Unit Not in Possession" until the player
# returns to the title screen. The tbz becomes a nop; the screens stay gated on FUNCTION_SP_CLASSUP elsewhere.
SACRIFICE_REGISTER_PATCH = {'9.1.0': (0x1D3D158, bytes.fromhex('a0000036'), bytes.fromhex('1f2003d5'))}


def _patch_library() -> None:
    """Apply SACRIFICE_REGISTER_PATCH to the installed library, once. Only a library the install recorded as a known
    version is touched, and only when the bytes there are the original instruction."""
    try:
        version = json.loads((CLIENT / 'installed.json').read_text(encoding='utf-8')).get('known_version')
        if version not in SACRIFICE_REGISTER_PATCH:
            return
        offset, original, patched = SACRIFICE_REGISTER_PATCH[version]
        with open(GAME / 'libgame.so', 'r+b') as library:
            library.seek(offset)
            if library.read(4) == original:
                library.seek(offset)
                library.write(patched)
    except (OSError, ValueError):
        pass                                             # the game still starts; conversion needs Return to Title


def start_game(port: int, extra_environment: dict | None = None) -> subprocess.Popen:
    _patch_library()
    environment = dict(os.environ)
    environment['FFBE_SERVER'] = f'127.0.0.1:{port}'
    environment['FFBE_PICTURES'] = str(HOME / 'pictures')      # where the camera button in the game window saves
    if 'window' in settings():                        # a fixed picture size, "WIDTHxHEIGHT"; normally the game window sizes itself
        environment.setdefault('FFBE_WINDOW', settings()['window'])
    if settings().get('read_data_in_place', True):   # the game reads the server's files directly instead of downloading copies
        environment.setdefault('FFBE_OVERLAY', str(DATA / 'native'))
        environment.setdefault('FFBE_NO_SETUP_RESTART', '1')   # the packs a first start needs are already there
        _drop_copies_read_in_place()
    if 'volume' in settings() and 'volume' not in window_settings():      # from earlier builds' settings.json
        save_window_settings({**window_settings(), 'volume': str(settings()['volume'])})
    if settings().get('cores'):                      # processor cores for the game; 1 is the old single-core behaviour
        environment.setdefault('FFBE_CORES', str(settings()['cores']))
    for name, value in settings().get('extra_environment', {}).items():      # "Other settings": the game's own switches
        if name.startswith('FFBE_'):                 # (FFBE_TOUCH_LOG=1, FFBE_VERBOSE=1 ...) go to the game as well
            environment[name] = str(value)
    environment.update(extra_environment or {})
    return subprocess.Popen([str(RUNNER), str(CLIENT), str(RUNNER_DIR / 'game.elf'), '/ffbe/game/libgame.so',
                             '/none.apk', '/ffbe/game'], cwd=str(RUNNER_DIR), env=environment,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=NO_WINDOW)


def library_is_modified() -> bool:
    """Is the installed game library the Exvius Offline app's (installed by an earlier version of this program)?
    Looked up once and kept in client/installed.json."""
    record_path = CLIENT / 'installed.json'
    try:
        record = json.loads(record_path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False
    if record.get('known_version'):
        return False
    if 'modified' not in record:
        found, tail = False, b''
        try:
            with open(GAME / 'libgame.so', 'rb') as source:
                for block in iter(lambda: source.read(4 << 20), b''):
                    if MODIFIED_MARK in tail + block:
                        found = True
                        break
                    tail = block[-len(MODIFIED_MARK):]
        except OSError:
            return False
        record['modified'] = found
        try:
            record_path.write_text(json.dumps(record, indent=1), encoding='utf-8')
        except OSError:
            pass
    return bool(record['modified'])


def play(extra_environment: dict | None = None, on_started=lambda: None) -> int:
    if library_is_modified():
        raise SetupError(MODIFIED_TEXT + ' Choose "Use a different APK" in the start window.')
    server, port = start_server()
    try:
        while True:
            game = start_game(port, extra_environment)
            on_started()
            code = game.wait()
            if code != RESTART_CODE:        # the game asks for one restart after its first table download
                return code
    finally:
        stop_server(server)


# ----------------------------------------------------------------------------------------------------
# Without a window
# ----------------------------------------------------------------------------------------------------
def headless(args) -> int:
    def say(text):
        print(text, flush=True)

    try:
        if args.apk or not game_installed():
            if not args.apk:
                say('The game is not set up yet: pass the APK with --apk FILE')
                return 2
            last = [-1]

            def report(text, done, total):
                percent = done * 100 // max(1, total)
                if percent // 10 != last[0]:
                    last[0] = percent // 10
                    say(f'{text}: {percent}%')
            record = install_apk(Path(args.apk), report)
            say(f"game files copied from {record['apk']} ({record['assets']} asset files); library "
                + (f"is version {record['known_version']}" if record['known_version'] else 'is not the tested 9.1.0 build'))
        ready, gigabytes = data_status()
        if not ready and not args.skip_data:
            say(f'downloading the game data ({gigabytes:.1f} GB)')
            fetch_game_data(say)
        elif not ready:
            say(f'game data is not complete ({gigabytes:.1f} GB to download); skipped as asked')
        if args.play:
            say('starting the server and the game')
            extra = dict(pair.split('=', 1) for pair in args.env)
            code = play(extra, lambda: say('game started'))
            say(f'game closed (code {code})')
            return code
        return 0
    except SetupError as exc:
        say('Stopped: ' + str(exc))
        return 1


# ----------------------------------------------------------------------------------------------------
# Settings: settings.json (launcher and server), client/desktop.ini (the game window)
# ----------------------------------------------------------------------------------------------------
WINDOW_INI = CLIENT / 'desktop.ini'


def window_settings() -> dict:
    out = {}
    try:
        for line in WINDOW_INI.read_text(encoding='utf-8').splitlines():
            name, eq, value = line.partition('=')
            if eq and name.strip() and not name.lstrip().startswith(('#', ';')):
                out[name.strip()] = value.strip()
    except OSError:
        pass
    return out


def save_window_settings(values: dict) -> None:
    CLIENT.mkdir(parents=True, exist_ok=True)
    WINDOW_INI.write_text(''.join(f'{name}={value}\n' for name, value in values.items()), encoding='utf-8')


def setting_value(entry: dict) -> str:
    """The value in force for one entry of settings_spec: what was chosen, else the release's value, else the default."""
    if entry['where'] == 'window':
        return window_settings().get(entry['key'], entry['default'])
    if entry['where'] == 'launcher':
        value = settings().get(entry['key'], entry['default'])
        return ('1' if value else '0') if isinstance(value, bool) else str(value)
    chosen = settings().get('server_environment', {})
    if entry['key'] in chosen:
        return str(chosen[entry['key']])
    return release_environment().get(entry['key'], entry['default'])


def set_setting(entry: dict, value: str) -> None:
    if entry['where'] == 'window':
        values = window_settings()
        values[entry['key']] = value
        save_window_settings(values)
        return
    values = settings()
    if entry['where'] == 'launcher':
        if value == entry['default'] and entry['kind'] != 'toggle':
            values.pop(entry['key'], None)
        else:
            values[entry['key']] = (value == '1') if entry['kind'] == 'toggle' else (int(value) if value.isdigit() else value)
    else:
        chosen = values.setdefault('server_environment', {})
        if value == release_environment().get(entry['key'], entry['default']):
            chosen.pop(entry['key'], None)        # back at the release's value: nothing to remember
        else:
            chosen[entry['key']] = value
    save_settings(values)


def extra_settings_text() -> str:
    return '\n'.join(f'{name}={value}' for name, value in settings().get('extra_environment', {}).items())


def save_extra_settings(text: str) -> None:
    out = {}
    for line in text.splitlines():
        name, eq, value = line.partition('=')
        name = name.strip()
        if eq and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
            out[name] = value.strip().strip('"')
    values = settings()
    if out:
        values['extra_environment'] = out
    else:
        values.pop('extra_environment', None)
    save_settings(values)


# ----------------------------------------------------------------------------------------------------
# The window
# ----------------------------------------------------------------------------------------------------
BG, PANEL, LINE, INK, MUTED, GOLD, GILT = '#070b26', '#101a4c', '#2b3a86', '#ffffff', '#aab4dc', '#e8c46a', '#fff1bf'


def window() -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox

    from PIL import ImageTk

    import launcher_art as art
    import launcher_ui as ui
    import settings_spec
    import settings_window

    if WINDOWS:                                   # real pixels on scaled displays, so the page is drawn sharp
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:  # noqa: BLE001 - older Windows: the page is simply enlarged by the system
            pass
    ui.set_app_identity()
    root = tk.Tk()
    root.title('Exvius Revival Desktop')

    def callback_failed(kind, value, trace) -> None:       # an error inside a click or a timer: record it, carry on
        import traceback
        text = ''.join(traceback.format_exception(kind, value, trace))
        log_error(text)
        if sys.stderr is not None and sys.stderr is not _error_log:
            print(text, file=sys.stderr)
    root.report_callback_exception = callback_failed
    try:
        scale = root.winfo_fpixels('1i') / 96.0
    except tk.TclError:
        scale = 1.0
    scale = max(1.0, min(3.0, round(scale * 4) / 4))
    if os.environ.get('EXVIUS_DESKTOP_SCALE'):                   # for tests: as if the display were scaled
        scale = float(os.environ['EXVIUS_DESKTOP_SCALE'])
    if root.winfo_screenheight() < art.HEIGHT * scale + 80:      # a small screen: fit the page on it
        scale = max(0.75, (root.winfo_screenheight() - 80) / art.HEIGHT)
    events: queue.Queue = queue.Queue()
    state = {'busy': False, 'headline': '', 'detail': '', 'main': ('', None), 'hot': None, 'progress': None, 'status': '',
             'notes': [], 'tick': 0, 'photo': None, 'dirty': True, 'corner': None, 'pressed': None, 'armed': None, 'icon': None,
             'icons': None, 'settings': None}
    # The window has no frame from the system unless "Windows title bar" is chosen in Settings: the page is the whole
    # window, pressing on a free part of it moves it, and its three buttons are drawn in its top right corner.
    own_frame = window_settings().get('system_frame', '0') != '1'
    figures = art.heroes(SAVE, DATA, SERVER / 'engine')
    pages = {'normal': art.Page(scale, figures)}
    page = pages['normal']
    width, height = page.size
    root.geometry(f'{width}x{height}+{max(0, (root.winfo_screenwidth() - width) // 2)}+{max(0, (root.winfo_screenheight() - height) // 3)}')
    root.resizable(False, False)
    root.configure(bg=BG)
    frame = ui.Frameless(root, own_frame)
    corner = ui.Corner(scale)
    canvas = tk.Canvas(root, width=width, height=height, highlightthickness=0, bd=0, bg=BG)
    canvas.pack(fill='both', expand=True)
    picture = canvas.create_image(0, 0, anchor='nw')

    def set_icon() -> None:
        """The Chocobo from the user's own game files once they are installed; the crystal until then."""
        state['icon'] = ui.app_icon(CLIENT, SERVER / 'engine')
        state['icons'] = ui.icon_photos(ImageTk, state['icon'])
        try:
            root.iconphoto(True, *state['icons'])
        except tk.TclError:
            pass
    set_icon()

    def menu() -> list:
        idle = not state['busy']
        rows = [('main', state['main'][0], idle and state['main'][1] is not None), ('settings', 'Settings', idle)]
        if game_installed():
            rows.append(('apk', 'Use a different APK', idle))
        rows += [('folder', 'Open the game folder', True), ('quit', 'Quit', True)]
        return rows

    def draw() -> None:
        image = page.render(state['headline'], state['detail'], menu(), state['hot'], state['progress'], state['status'],
                            state['notes'], state['tick'])
        if own_frame:
            corner.place(image.width - round(24 * page.scale), round(22 * page.scale))
            corner.paint(image, state['corner'], frame.maximized)
        state['photo'] = ImageTk.PhotoImage(image)
        canvas.itemconfigure(picture, image=state['photo'])
        state['dirty'] = False

    def open_folder(path: Path) -> None:
        if WINDOWS:
            os.startfile(str(path))  # noqa: S606 - opens the install folder in Explorer
        else:
            subprocess.Popen(['xdg-open', str(path)])

    def note(line: str) -> None:
        line = line.strip()
        if not line:
            return
        found = re.search(r'(\d+)% \(', line)             # "data-004.zip: 62% (0.91 of 1.46 GB, 25 MB/s)"
        if found:
            state['progress'] = int(found.group(1)) / 100
            state['status'] = line
        else:
            state['notes'] = (state['notes'] + [line])[-2:]
        state['dirty'] = True

    def run(job, *job_args) -> None:
        """Run one step off the window's thread; results come back through `events`."""
        state['busy'] = True
        state['dirty'] = True

        def work():
            try:
                job(*job_args)
                events.put(('done', None))
            except SetupError as exc:
                events.put(('failed', str(exc)))
            except Exception as exc:  # noqa: BLE001 - show anything unexpected instead of closing silently
                events.put(('failed', f'{type(exc).__name__}: {exc}'))
        threading.Thread(target=work, daemon=True).start()

    def choose_apk() -> None:
        chosen = filedialog.askopenfilename(
            title='Choose your FINAL FANTASY BRAVE EXVIUS 9.1.0 APK or XAPK',
            filetypes=[('Android packages', '*.apk *.xapk *.apks *.apkm'), ('All files', '*.*')])
        if not chosen:
            return
        state.update(headline='Reading your APK', detail=chosen, progress=0.0, status='', notes=[])

        def job():
            record = install_apk(Path(chosen), lambda text, done, total: events.put(('progress', (text, done, total))))
            if not record['known_version']:
                events.put(('note', 'This APK\'s game library is not the 9.1.0 build this program was tested with. '
                                    'If the game does not start, use a 9.1.0 APK.'))
        run(job)

    def download_data() -> None:
        _, gigabytes = data_status()
        if free_space_gb(DATA) < gigabytes + 16:
            if not messagebox.askyesno('Low disk space', f'About {gigabytes + 16:.0f} GB of free space is needed here while '
                                       f'the game data is downloaded and unpacked, and {free_space_gb(DATA):.0f} GB is free. '
                                       'Continue anyway?'):
                return
        state.update(headline='Downloading\nthe game data', detail='This window can be closed and the download continued later; '
                     'it carries on where it stopped.', progress='busy', status='', notes=[])
        run(fetch_game_data, lambda line: events.put(('note', line)))

    def start_playing() -> None:
        state.update(headline='Starting the game', detail='The game server is starting on this computer. The game window opens in a moment.',
                     progress='busy', status='', notes=[])
        run(play, None, lambda: events.put(('hide', None)))

    def refresh() -> None:
        state.update(progress=None, status='')
        if not game_installed():
            state.update(headline='Please insert your\nAPK / XAPK',
                         detail='This program contains no game files. Choose your own FINAL FANTASY BRAVE EXVIUS '
                                f'{GAME_VERSION} APK (Android, Global version) and the game is copied out of it into this folder.',
                         main=('Choose APK or XAPK', choose_apk))
        else:
            ready, gigabytes = data_status()
            if not ready:
                state.update(headline='One more disc:\nthe game data',
                             detail=f'{gigabytes:.1f} GB to download into the "data" folder here. It needs about {gigabytes + 16:.0f} GB '
                                    'of free space while it unpacks.',
                             main=('Download game data', download_data))
            else:
                state.update(headline='Ready to play', detail='The game runs entirely on this computer. Your save is in the "save" folder.',
                             main=('Play', start_playing))
        state['dirty'] = True

    def click(key: str) -> None:
        if key == 'main' and state['main'][1]:
            state['main'][1]()
        elif key == 'settings':
            open_settings()
        elif key == 'apk':
            choose_apk()
        elif key == 'folder':
            open_folder(HOME)
        elif key == 'quit':
            close()

    def on_move(event) -> None:
        button = corner.at(event.x, event.y) if own_frame else None
        hot = None if button else page.row_at(event.x, event.y)
        if hot != state['hot'] or button != state['corner']:
            state['hot'], state['corner'] = hot, button
            canvas.configure(cursor='hand2' if hot or button else '')
            draw()

    def maximize() -> None:
        """Fill the screen's work area with the page, drawn again at that size; or go back."""
        nonlocal page
        canvas.configure(cursor='watch')
        root.update_idletasks()
        w, h = frame.toggle_maximize()
        if frame.maximized:
            pages['large'] = art.Page(min(w / art.WIDTH, h / art.HEIGHT), pages.get('figures', figures), size=(w, h))
            page = pages['large']
        else:
            pages.pop('large', None)
            page = pages['normal']
        corner.s, corner.px, corner.gap = page.scale, round(24 * page.scale), round(6 * page.scale)
        state.update(hot=None, corner=None)
        canvas.configure(cursor='')
        draw()

    def settings_open() -> bool:
        return state['settings'] is not None and state['settings'].alive()

    def on_press(event) -> None:
        state['pressed'] = state['armed'] = None
        if settings_open():                     # Settings is this window's dialog: bring it forward, do nothing else
            state['settings'].come_forward()
            return
        state['pressed'] = corner.at(event.x, event.y) if own_frame else None
        if state['pressed']:
            return
        key = page.row_at(event.x, event.y)
        if key:
            state['armed'] = key                # like any button, a line acts when the mouse button comes up on it
        else:
            frame.press(event)                  # a free part of the page moves the window

    def on_release(event) -> None:
        frame.release()
        button, state['pressed'] = state['pressed'], None
        key, state['armed'] = state['armed'], None
        if key:
            if page.row_at(event.x, event.y) == key:
                click(key)
                state['dirty'] = True
            return
        if button and corner.at(event.x, event.y) == button:
            if button == 'close':
                close()
            elif button == 'min':
                state.update(corner=None, dirty=True)
                frame.minimize()
            else:
                maximize()

    def on_double(event) -> None:
        if settings_open():
            state['settings'].come_forward()
        elif own_frame and not corner.at(event.x, event.y) and not page.row_at(event.x, event.y):
            maximize()

    def poll() -> None:
        try:
            while True:
                kind, value = events.get_nowait()
                if kind == 'progress':
                    text, done, total = value
                    state.update(status=f'{text}: {done * 100 // max(1, total)}%', progress=done / max(1, total), dirty=True)
                elif kind == 'note':
                    note(value)
                elif kind == 'hide':
                    root.withdraw()
                elif kind in ('done', 'failed'):
                    state['busy'] = False
                    root.deiconify()
                    if kind == 'failed':
                        note('Stopped: ' + value)
                        messagebox.showerror('Exvius Revival Desktop', value)
                    else:
                        set_icon()                                                # the game's files may have just arrived
                        if not page.base_has_figures:
                            found = art.heroes(SAVE, DATA, SERVER / 'engine')     # and so may the data
                            if found:
                                pages['figures'] = found
                                for each in pages.values():
                                    if isinstance(each, art.Page):
                                        each.set_figures(found)
                    refresh()
        except queue.Empty:
            pass
        if state['progress'] == 'busy':
            state['tick'] += 1
            state['dirty'] = state['dirty'] or state['tick'] % 2 == 0
        if state['dirty']:
            draw()
        root.after(60, poll)

    def close() -> None:
        if state['busy'] and not messagebox.askyesno('Exvius Revival Desktop', 'A step is still running. Close anyway?'):
            return
        root.destroy()

    # ---- the settings window -----------------------------------------------------------------------
    def open_settings(first_page: int = 0, plain: bool = False) -> None:
        """`plain`: with the system's frame and as a window of its own, whatever the "Windows title bar" setting says.
        That is what Settings falls back to if its frameless window fails to open or to appear."""
        if settings_open():
            state['settings'].come_forward()
            return

        def default(entry):
            return release_environment().get(entry['key'], entry['default']) if entry['where'] == 'server' else entry['default']

        def start_over():
            try:
                (SAVE / 'state' / 'replay.json').unlink(missing_ok=True)
            except OSError as exc:
                return str(exc)
            return None
        api = {'value': setting_value, 'set': set_setting, 'default': default, 'extra_text': extra_settings_text,
               'save_extra': save_extra_settings, 'start_over': start_over}
        own = own_frame and not plain

        def stuck(page: int) -> None:
            log_error('settings: opening it again with the system\'s frame')
            open_settings(page, plain=True)
        log_error(f'settings: opening ({"own frame" if own else "plain window" if plain else "system frame"}, scale {scale})')
        try:
            state['settings'] = settings_window.SettingsWindow(tk, messagebox, root, scale, settings_spec.GROUPS, api, own, state['icon'],
                                                               first_page, modal=not own_frame and not plain, on_stuck=stuck if own else None, log=log_error)
        except Exception:  # noqa: BLE001 - record it; the frameless window gets one more try with the system's frame
            import traceback
            log_error(traceback.format_exc())
            state['settings'] = None
            if own:
                stuck(first_page)
            else:
                messagebox.showerror('Exvius Revival Desktop', 'Settings could not be opened. The reason is in logs\\launcher.log in the game folder.')

    canvas.bind('<Motion>', on_move)
    canvas.bind('<Leave>', lambda _e: (state.update(hot=None, corner=None, dirty=True)))
    canvas.bind('<ButtonPress-1>', on_press)
    canvas.bind('<B1-Motion>', frame.drag)
    canvas.bind('<ButtonRelease-1>', on_release)
    canvas.bind('<Double-Button-1>', on_double)
    root.protocol('WM_DELETE_WINDOW', close)
    refresh()
    draw()
    if os.environ.get('EXVIUS_DESKTOP_OPEN_SETTINGS'):       # for screenshots in tests
        root.after(400, lambda: open_settings(int(os.environ.get('EXVIUS_DESKTOP_SETTINGS_PAGE', '0'))))
    poll()
    root.mainloop()
    return 0


_error_log = None


def keep_errors() -> None:
    """The start window runs without a console, so whatever goes wrong in it would vanish. Keep it in
    logs\\launcher.log: Python's own errors, and through faulthandler a crash inside a library."""
    global _error_log
    try:
        import faulthandler
        path = LOGS / 'launcher.log'
        if path.is_file() and path.stat().st_size > 1_000_000:
            path.unlink()
        _error_log = open(path, 'a', encoding='utf-8', buffering=1)
        _error_log.write(f'--- started {time.strftime("%Y-%m-%d %H:%M:%S")}, Python {sys.version.split()[0]}, {sys.platform}\n')
        if sys.stderr is None:
            sys.stderr = _error_log
        if sys.stdout is None:
            sys.stdout = _error_log
        faulthandler.enable(_error_log)
    except Exception:  # noqa: BLE001 - the program runs the same without the log
        _error_log = None
    # A fatal error inside Python itself (not an exception) is written by the C runtime to its own error output and
    # switches faulthandler off first; without a console that output goes nowhere. Point it at a file too.
    if WINDOWS and _error_log is not None:
        try:
            import ctypes
            crt = ctypes.CDLL('ucrtbase')
            crt.__acrt_iob_func.restype = ctypes.c_void_p
            crt.__acrt_iob_func.argtypes = [ctypes.c_uint]
            crt._wfreopen.restype = ctypes.c_void_p
            crt._wfreopen.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_void_p]
            crt._wfreopen(str(LOGS / 'launcher-fatal.log'), 'a', crt.__acrt_iob_func(2))
        except Exception:  # noqa: BLE001
            pass


def log_error(text: str) -> None:
    if _error_log is not None:
        try:
            _error_log.write(text.rstrip() + '\n')
        except Exception:  # noqa: BLE001
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description='Exvius Revival Desktop')
    parser.add_argument('--headless', action='store_true', help='no window: do the steps and print progress')
    parser.add_argument('--apk', help='the game APK to set up from')
    parser.add_argument('--skip-data', action='store_true', help='do not download the game data')
    parser.add_argument('--play', action='store_true', help='start the server and the game after setting up')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='extra setting for the game window')
    args = parser.parse_args()
    for folder in (CLIENT, LOGS):
        folder.mkdir(parents=True, exist_ok=True)
    if args.headless:
        return headless(args)
    keep_errors()
    try:
        return window()
    except Exception:  # noqa: BLE001 - leave a record, then fail as before
        import traceback
        log_error(traceback.format_exc())
        raise


if __name__ == '__main__':
    sys.exit(main())
