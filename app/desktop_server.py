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


def with_pages(handler, port: int):
    class Handler(handler):
        def do_GET(self):
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
            import deal_art
            deal_art.fetch(log=serve.log)
        except Exception as exc:  # noqa: BLE001
            serve.log(f'deal art step failed: {exc!r}')

    threading.Thread(target=build_art, name='ios-art', daemon=True).start()
    threading.Thread(target=fetch_deal_art, name='deal-art', daemon=True).start()
    handler = with_pages(serve.make_handler(capture_server, b'', b''), args.port)
    server = serve.Server(('127.0.0.1', args.port), handler)
    threading.Thread(target=server.serve_forever, name=f'HTTP-{args.port}', daemon=True).start()
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
