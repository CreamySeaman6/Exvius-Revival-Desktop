"""The local game server for Exvius Revival Desktop.

Runs the same engine and start-up code as the iPhone release's server (server/serve.py), with three
differences that suit a game running on the same computer:
  * it listens on 127.0.0.1 only, on the port the launcher picked, so nothing else on the network can reach it;
  * no HTTPS, certificates or DNS responder: the game window talks to it directly;
  * it stops by itself when the launcher goes away (its standard input closes).
"""
from __future__ import annotations

import argparse
import os
import sys
import threading
from pathlib import Path

HOME = Path(os.environ.get('EXVIUS_DESKTOP_HOME', Path(__file__).resolve().parent.parent))
SERVER = HOME / 'server'


# ----------------------------------------------------------------------------------------------------
# In-game web pages. The phone shows the server's News, Drop Rates and reward pages in a web view; the
# desktop game window asks for them here as a picture instead (see webpage.py and runner/host_web.c):
#   GET /__desktop/page?url=<the address the game asked for>&post=<its form fields>&width=<pixels>&height=<pixels>&scale=<pixels per CSS pixel>
# The answer is "FFPG", five 32-bit numbers (version, width, height, links, 0... see below), the links, then RGB rows.
# ----------------------------------------------------------------------------------------------------
_PICTURES: dict = {}
PAGE_PIXELS = 32_000_000
_PICTURES_BYTES = [0]


def _local(port: int, address: str, form: str = '') -> tuple[int, bytes, str]:
    """Ask this server for `address` (whatever host it names). Returns (status, body, content type)."""
    import urllib.error
    import urllib.parse
    import urllib.request
    parts = urllib.parse.urlsplit(address)
    path = urllib.parse.quote(urllib.parse.unquote(parts.path), safe='/%') or '/'
    target = f'http://127.0.0.1:{port}{path}' + (f'?{parts.query}' if parts.query else '')
    data = form.encode('utf-8') if form else None
    request = urllib.request.Request(target, data=data, headers={'Host': parts.netloc or '127.0.0.1', 'X-Device-Platform': 'android',
                                                                 **({'Content-Type': 'application/x-www-form-urlencoded'} if data else {})})
    try:
        with urllib.request.urlopen(request, timeout=60) as reply:
            return reply.status, reply.read(), reply.headers.get('Content-Type', '')
    except urllib.error.HTTPError as exc:
        return exc.code, b'', ''
    except OSError:
        return 0, b'', ''


def _picture(port: int, address: str):
    if address not in _PICTURES:
        status, body, _ = _local(port, address)
        body = body if status == 200 else None
        if _PICTURES_BYTES[0] > 96 << 20:                 # keep the store small: start again when it has grown large
            _PICTURES.clear()
            _PICTURES_BYTES[0] = 0
        _PICTURES[address] = body
        _PICTURES_BYTES[0] += len(body or b'')
    return _PICTURES[address]


def page_picture(port: int, query: str) -> tuple[int, bytes]:
    import re
    import struct
    import time
    import urllib.parse
    from concurrent.futures import ThreadPoolExecutor
    fields = urllib.parse.parse_qs(query, keep_blank_values=True)
    address = fields.get('url', [''])[0]
    form = fields.get('post', [''])[0]
    if '?' in form:                                        # as the Android app does: only what follows a "?"
        form = form.split('?', 1)[1]
    try:
        width = max(64, min(4096, int(fields.get('width', ['640'])[0])))
        height = max(64, min(8192, int(fields.get('height', ['960'])[0])))
        scale = max(0.25, min(8.0, float(fields.get('scale', ['1.6'])[0])))
    except ValueError:
        return 400, b'bad size'
    if not address:
        return 400, b'no address'
    started = time.time()
    if urllib.parse.urlsplit(address).path.lower().endswith(('.png', '.jpg', '.jpeg')):     # a bare picture, shown full width
        html = ('<!doctype html><html><body style="margin:0;background:#000;text-align:center">'
                f'<img src="{address}" style="width:100%;height:auto"></body></html>')
    else:
        status, body, kind = _local(port, address, form)
        if status != 200:
            return 502 if status == 0 else status, b'page not available'
        html = body.decode('utf-8', 'replace')
    import webpage
    sources = {urllib.parse.urljoin(address, m) for m in re.findall(r'<img[^>]*\ssrc="([^"]+)"', html)}
    with ThreadPoolExecutor(max_workers=8) as pool:        # the pictures on the page, several at a time
        list(pool.map(lambda one: _picture(port, one), sources))
    picture, links, _ = webpage.render(html, address, width, scale, lambda one: _picture(port, one), height)
    # A very long page at a high resolution would be hundreds of MB. Past 32 million pixels it is handed over
    # smaller; the game window stretches it back to the width it asked for.
    if picture.width * picture.height > PAGE_PIXELS:
        from PIL import Image
        factor = (PAGE_PIXELS / (picture.width * picture.height)) ** 0.5
        smaller = (max(320, int(picture.width * factor)), max(1, int(picture.height * factor)))
        fx, fy = smaller[0] / picture.width, smaller[1] / picture.height
        picture = picture.resize(smaller, Image.LANCZOS)
        links = [(int(x * fx), int(y * fy), max(1, int(w * fx)), max(1, int(h * fy)), target) for x, y, w, h, target in links]
    out = [b'FFPG', struct.pack('<IIII', 1, picture.width, picture.height, len(links))]
    for x, y, w, h, target in links:
        raw = target.encode('utf-8')[:2000]
        out.append(struct.pack('<iiiiH', x, y, w, h, len(raw)) + raw)
    out.append(picture.tobytes())
    print(f'[desktop] drew {address} at {picture.width}x{picture.height} ({len(sources)} pictures, {len(links)} links) '
          f'in {time.time() - started:.1f} s', flush=True)
    return 200, b''.join(out)


# ----------------------------------------------------------------------------------------------------
# Summon banner art that is not on this computer. The deal banners' art (step-ups, free dailies, value
# summons...) is downloaded by the server from the Wayback Machine after it starts (deal_art.py), one file
# every half second or slower; before that has finished, or when the archive cannot be reached, the server
# answers 404. The game treats a failed banner download as a lost connection: "A connection error has
# occurred" on the Summon screen, then back to the title (a user's report on 0.1.2, reproduced here with the
# archive unreachable). A summon banner that would be a 404 is answered with a plain titled stand-in instead.
# The game keeps every banner file it has downloaded, so each stand-in is listed in
# save/banner-standins.txt; once the real file is on this computer the game's copy is deleted and the game
# downloads the real one the next time it shows that banner.
# ----------------------------------------------------------------------------------------------------
_STANDIN_LOCK = threading.Lock()
_GACHA_ART = '/static_banner/gacha/'


def _standin_list() -> Path:
    return Path(os.environ.get('FFBE_SERVER_ROOT', HOME / 'save')) / 'banner-standins.txt'


def standin_picture(rel: str) -> bytes:
    """A titled stand-in for the summon banner picture `rel` (the game's path, e.g. en/sd/static_banner/gacha/sub/x-fit1.png)."""
    import banner_fit
    import deal_art
    plain = banner_fit.is_plain(rel)
    real = banner_fit.real_rel(rel)
    name = os.path.basename(real)
    title, size = '', ((640, 392) if '/gacha/sub/' in '/' + real else (640, 160))
    try:
        for target, _url, label, dims in deal_art._targets():
            if target == name:
                title, size = label, dims
                break
    except Exception:  # noqa: BLE001 - an untitled stand-in is still better than the connection error
        pass
    data = deal_art.render(title, size)
    if banner_fit.wants(real):
        data, _ = banner_fit.fit(data, banner_fit.BASE_FRAME if plain else banner_fit.FRAME)
    return data


def note_standin(rel: str) -> None:
    with _STANDIN_LOCK:
        path = _standin_list()
        try:
            known = set(path.read_text(encoding='utf-8').split()) if path.is_file() else set()
            if rel not in known:
                with open(path, 'a', encoding='utf-8') as out:
                    out.write(rel + '\n')
        except OSError:
            pass


def replace_standins(log=print) -> int:
    """Delete the game's copy of each stand-in whose real picture is now here, so the game downloads the real one."""
    import banner_fit
    import data_paths
    game_files = HOME / 'client' / 'game' / 'files'
    with _STANDIN_LOCK:
        path = _standin_list()
        if not path.is_file():
            return 0
        try:
            names = path.read_text(encoding='utf-8').split()
        except OSError:
            return 0
        kept, replaced = [], 0
        for rel in names:
            real = os.path.basename(banner_fit.real_rel(rel))
            if os.path.isfile(os.path.join(data_paths.DEAL_ART_ROOT, real)):
                try:
                    (game_files / rel).unlink(missing_ok=True)
                    replaced += 1
                    continue
                except OSError:
                    pass
            kept.append(rel)
        try:
            if kept:
                path.write_text(''.join(r + '\n' for r in kept), encoding='utf-8')
            else:
                path.unlink(missing_ok=True)
        except OSError:
            pass
    if replaced:
        log(f'[desktop] {replaced} stand-in banner picture(s) will be replaced by the real ones')
    return replaced


def with_pages(handler, port: int):
    class Handler(handler):
        def _gacha_art_or_standin(self) -> bool:
            """Serve a summon banner picture through the server; answer a 404 with a stand-in. True when handled."""
            import io
            import urllib.parse
            rel = urllib.parse.unquote(self.path.split('?', 1)[0]).lstrip('/')
            if rel.startswith('banner/'):
                rel = rel[len('banner/'):].lstrip('/')
            if _GACHA_ART not in '/' + rel or not rel.lower().endswith('.png') or '..' in rel:
                return False
            real_out, buffer = self.wfile, io.BytesIO()
            self.wfile = buffer
            try:
                super().do_GET()
            finally:
                self.wfile = real_out
            answer = buffer.getvalue()
            if not answer.startswith(b'HTTP/1.1 404') and not answer.startswith(b'HTTP/1.0 404'):
                real_out.write(answer)
                return True
            try:
                data = standin_picture(rel)
            except Exception as exc:  # noqa: BLE001 - then the 404 as it was
                print(f'[desktop] no stand-in for {rel}: {exc!r}', flush=True)
                real_out.write(answer)
                return True
            note_standin(rel)
            print(f'[desktop] {rel} is not on this computer yet; a stand-in picture was sent', flush=True)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)
            self.close_connection = True
            return True

        def do_GET(self):
            if _GACHA_ART in self.path and self._gacha_art_or_standin():
                return
            if self.path.startswith('/__desktop/page?'):
                try:
                    status, body = page_picture(port, self.path.split('?', 1)[1])
                except Exception as exc:  # noqa: BLE001 - a page that cannot be drawn must not take the server down
                    import traceback
                    traceback.print_exc()
                    status, body = 500, repr(exc).encode('utf-8', 'replace')
                self.send_response(status)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path == '/__desktop/files':            # files this server made itself, for the game to read in place
                lines = []
                try:
                    import ios_tables
                    built = ios_tables.art()
                    if built:
                        lines.append(f'en/sd/area/Ver{built[1]}_spdungeon1.cpk\t{built[0]}')
                except Exception as exc:  # noqa: BLE001 - without the list the game simply downloads the file
                    print(f'[desktop] file list failed: {exc!r}', flush=True)
                body = ('\n'.join(lines) + '\n').encode('utf-8')
                self.send_response(200)
                self.send_header('Content-Type', 'text/plain; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            super().do_GET()
    return Handler


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(SERVER))
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import net_trust
    trust = net_trust.apply()                       # the server fetches banner art from the internet too
    import serve

    serve.configure_engine()
    capture_server = serve.start_engine()

    def build_art():                      # as serve.main does: prepared in the background, cached in save/
        try:
            import event_archive
            import ios_tables
            event_archive.fetch_added_art(log=serve.log)
            ios_tables.art()
        except Exception as exc:  # noqa: BLE001 - art is optional; the server keeps running
            serve.log(f'background art step failed: {exc!r}')

    def fetch_deal_art():
        try:
            replace_standins(serve.log)               # real pictures fetched in an earlier session
            import deal_art
            deal_art.fetch(log=serve.log)
            replace_standins(serve.log)
        except Exception as exc:  # noqa: BLE001
            serve.log(f'deal art step failed: {exc!r}')

    threading.Thread(target=build_art, name='ios-art', daemon=True).start()
    threading.Thread(target=fetch_deal_art, name='deal-art', daemon=True).start()
    handler = with_pages(serve.make_handler(capture_server, b'', b''), args.port)
    server = serve.Server(('127.0.0.1', args.port), handler)
    threading.Thread(target=server.serve_forever, name=f'HTTP-{args.port}', daemon=True).start()
    serve.log(f'https checks for downloads: {trust}')
    serve.log(f'HTTP listening on 127.0.0.1:{args.port} (this computer only)')
    serve.prune_request_logs()
    serve.log('ready')
    try:
        sys.stdin.buffer.read()           # returns when the launcher closes the pipe or exits
    except (OSError, ValueError):
        threading.Event().wait()          # no pipe (started by hand): run until stopped
    serve.log('launcher closed; stopping')
    os._exit(0)                           # daemon threads may be mid-request; nothing needs unwinding


if __name__ == '__main__':
    sys.exit(main())
