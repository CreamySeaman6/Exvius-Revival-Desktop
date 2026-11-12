"""The look of the start window's own parts and of Settings: the window frame, the drawn controls, the icon.

Nothing here is a system control. Every button, switch and field is drawn with Pillow at the display's real pixel
size and shown as a picture, so edges are smooth at any display scaling, and the windows carry no frame from the
system: the strip along the top moves the window and holds its buttons, drawn like the bar above the game.

The icon is a Chocobo read out of the user's own game files once they are installed (never stored in this program);
until then it is this program's crystal.
"""
from __future__ import annotations

import functools
import os
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

import launcher_art as art

WINDOWS = os.name == 'nt'
BG, PANEL, LINE, FIELD = (7, 11, 38), (16, 26, 76), (43, 58, 134), (10, 17, 54)
INK, MUTED, GOLD, GILT = (255, 255, 255), (170, 180, 220), art.GOLD, art.GILT
BLUE = ((46, 92, 200), (11, 29, 102), (185, 212, 255))          # top, bottom, rim: the bar's buttons
BLUE_HOT = ((79, 134, 240), (21, 48, 148), GILT)
RED_HOT = ((226, 96, 76), (126, 20, 32), GILT)
GOLD_FILL = ((246, 214, 128), (198, 146, 44), (255, 236, 170))
GOLD_HOT = ((255, 236, 170), (222, 172, 66), (255, 250, 226))
BOLD, PLAIN, TITLE = 'Roboto-Bold.ttf', 'Roboto-Regular.ttf', 'Cinzel-Bold.ttf'


def hexa(colour) -> str:
    return '#%02x%02x%02x' % tuple(colour[:3])


# ----------------------------------------------------------------------------------------------------
# Drawing
# ----------------------------------------------------------------------------------------------------
def _mask(w: int, h: int, radius: float, inset: float = 0.0) -> Image.Image:
    """A rounded rectangle as a smooth-edged mask (drawn four times larger and reduced)."""
    k = 4
    mask = Image.new('L', (w * k, h * k), 0)
    ImageDraw.Draw(mask).rounded_rectangle((inset * k, inset * k, (w - inset) * k - 1, (h - inset) * k - 1),
                                           radius=max(0.0, (radius - inset) * k), fill=255)
    return mask.resize((w, h), Image.LANCZOS)


@functools.lru_cache(maxsize=512)
def panel(w: int, h: int, radius: float, top, bottom, rim, rim_w: float = 1.2, gloss: float = 0.22) -> Image.Image:
    """A rounded panel: gradient fill, a light rim, a soft highlight across the top. Do not draw on the result."""
    body = art._gradient(w, h, [(0, top), (1, bottom)]).convert('RGBA')
    if gloss:
        column = Image.new('L', (1, h))
        column.putdata([round(255 * gloss * 0.9 * max(0.0, 1 - y / (h * 0.48))) for y in range(h)])
        shine = Image.new('RGBA', (w, h), (255, 255, 255, 0))
        shine.putalpha(column.resize((w, h)))
        body.alpha_composite(shine)
    out = Image.new('RGBA', (w, h), tuple(rim) + (255,))
    out.paste(body, (0, 0), _mask(w, h, radius, rim_w))
    out.putalpha(_mask(w, h, radius))
    return out


@functools.lru_cache(maxsize=256)
def glyph(kind: str, px: int, colour=INK) -> Image.Image:
    """The sign on a button, `px` square: min, max, restore, close, down, minus, plus, grip."""
    k = 4
    n = px * k
    mask = Image.new('L', (n, n), 0)
    pencil = ImageDraw.Draw(mask)
    half = n * 0.31
    width = max(1, round(0.22 * half))

    def at(x, y):
        return (n / 2 + x * half, n / 2 + y * half)

    def stroke(*points):
        pencil.line([at(*p) for p in points], fill=255, width=width, joint='curve')
        r = width / 2
        for p in points:
            cx, cy = at(*p)
            pencil.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)

    def box(cx, cy, hw):
        (x0, y0), (x1, y1) = at(cx - hw, cy - hw), at(cx + hw, cy + hw)
        pencil.rounded_rectangle((x0, y0, x1, y1), radius=0.12 * half, outline=255, width=width)

    if kind == 'min':
        stroke((-0.56, 0.52), (0.56, 0.52))
    elif kind == 'max':
        box(0, 0, 0.64)
    elif kind == 'restore':
        box(0.18, -0.18, 0.50)
        (x0, y0), (x1, y1) = at(-0.18 - 0.72, 0.18 - 0.72), at(-0.18 + 0.72, 0.18 + 0.72)
        pencil.rectangle((x0, y0, x1, y1), fill=0)                 # the one behind shows only beside the one in front
        box(-0.18, 0.18, 0.50)
    elif kind == 'close':
        stroke((-0.52, -0.52), (0.52, 0.52))
        stroke((-0.52, 0.52), (0.52, -0.52))
    elif kind == 'down':
        stroke((-0.50, -0.24), (0, 0.28), (0.50, -0.24))
    elif kind == 'minus':
        stroke((-0.52, 0), (0.52, 0))
    elif kind == 'plus':
        stroke((-0.52, 0), (0.52, 0))
        stroke((0, -0.52), (0, 0.52))
    elif kind == 'grip':
        for i in (0.0, 0.55, 1.1):
            stroke((0.9, -0.5 + i), (-0.5 + i, 0.9))
    out = Image.new('RGBA', (px, px), tuple(colour) + (0,))
    out.putalpha(mask.resize((px, px), Image.LANCZOS))
    return out


@functools.lru_cache(maxsize=256)
def square_button(kind: str, px: int, hot: bool = False) -> Image.Image:
    """One of the bar's square buttons with its sign."""
    top, bottom, rim = (RED_HOT if kind == 'close' else BLUE_HOT) if hot else BLUE
    out = panel(px, px, px * 0.24, top, bottom, rim, 1.4 if hot else 1.2, 0.34 if hot else 0.22).copy()
    shadow = glyph(kind, px, (0, 0, 0))
    shadow.putalpha(shadow.getchannel('A').point(lambda v: v * 110 // 255))
    out.alpha_composite(shadow, (0, max(1, px // 22)))
    out.alpha_composite(glyph(kind, px))
    return out


@functools.lru_cache(maxsize=16)
def switch(on: bool, hot: bool, s: float) -> Image.Image:
    w, h = round(46 * s), round(24 * s)
    if on:
        top, bottom, rim = GOLD_HOT if hot else GOLD_FILL
        out = panel(w, h, h / 2, top, bottom, rim, max(1.0, 1.1 * s), 0.30).copy()
    else:
        out = panel(w, h, h / 2, (26, 40, 104), (11, 18, 58), (185, 212, 255) if hot else (86, 108, 190), max(1.0, 1.1 * s), 0.10).copy()
    d = h - 2 * round(3 * s)
    x, y = (w - d - round(3 * s) if on else round(3 * s)), (h - d) // 2
    shade = Image.new('RGBA', (w, h), (0, 0, 20, 0))
    shade.paste((0, 0, 20, 120), (x, y + max(1, round(s))), _mask(d, d, d / 2))
    out.alpha_composite(shade.filter(ImageFilter.GaussianBlur(max(0.6, s * 0.8))))
    knob = panel(d, d, d / 2, (255, 255, 255), (226, 232, 248), (255, 255, 255), 0.5, 0)
    out.alpha_composite(knob, (x, y))
    return out


@functools.lru_cache(maxsize=2048)
def _text(text: str, face: str, size: float, colour, shadow: bool) -> Image.Image:
    return art.text_layer(text, face, size, colour, shadow=shadow)


def text_width(text: str, face: str, size: float) -> float:
    return art.font(face, size).getlength(text)


def _lay(image: Image.Image, layer: Image.Image, x: int, y: int) -> None:
    """alpha_composite that may hang over the picture's edges."""
    x0, y0 = max(0, -x), max(0, -y)
    x1, y1 = min(layer.width, image.width - x), min(layer.height, image.height - y)
    if x1 > x0 and y1 > y0:
        image.alpha_composite(layer.crop((x0, y0, x1, y1)) if (x0, y0, x1, y1) != (0, 0) + layer.size else layer, (x + x0, y + y0))


def put_text(image: Image.Image, x: float, y: float, text: str, face: str, size: float, colour, shadow: bool = False,
             anchor: str = 'l', middle: bool = False) -> float:
    """Draws one line; `size` in pixels. y is the top of the line, or with `middle` the middle of the capital letters.
    Returns the width."""
    if not text:
        return 0.0
    size = round(size * 4) / 4
    layer = _text(text, face, size, tuple(colour), shadow)
    pad = round(size * 0.5)
    width = layer.width - 2 * pad
    if anchor == 'r':
        x -= width
    elif anchor == 'c':
        x -= width / 2
    _lay(image, layer, round(x) - pad, round(y - 0.72 * size if middle else y) - pad)
    return width


def fit_text(text: str, face: str, size: float, width: float) -> str:
    if text_width(text, face, size) <= width:
        return text
    while len(text) > 1 and text_width(text + '…', face, size) > width:
        text = text[:-1]
    return text.rstrip() + '…'


def pill_size(text: str, s: float) -> tuple:
    return (round(text_width(text, BOLD, 13.5 * s) + 36 * s), round(32 * s))


@functools.lru_cache(maxsize=128)
def pill_button(text: str, s: float, strong: bool = False, hot: bool = False) -> Image.Image:
    w, h = pill_size(text, s)
    top, bottom, rim = (GOLD_HOT if hot else GOLD_FILL) if strong else (BLUE_HOT if hot else BLUE)
    out = panel(w, h, h * 0.30, top, bottom, rim, max(1.0, 1.2 * s), 0.30 if strong else 0.22).copy()
    put_text(out, w / 2, h / 2, text, BOLD, 13.5 * s, (26, 16, 48) if strong else INK, shadow=not strong, anchor='c', middle=True)
    return out


@functools.lru_cache(maxsize=256)
def field(w: int, h: int, s: float, hot: bool = False, text: str = '', arrow: bool = False) -> Image.Image:
    """A dark field with a thin rim: what a value sits in. With `arrow` it is a list to choose from."""
    out = panel(w, h, h * 0.28, FIELD, FIELD, GOLD if hot else (74, 96, 180), max(1.0, 1.1 * s), 0).copy()
    if arrow:
        mark = glyph('down', round(h * 0.78), GILT if hot else GOLD)
        out.alpha_composite(mark, (w - mark.width - round(4 * s), (h - mark.height) // 2))
    if text:
        room = w - round(12 * s) - (round(h * 0.78) + round(4 * s) if arrow else round(12 * s))
        put_text(out, round(12 * s), h / 2, fit_text(text, PLAIN, 13.5 * s, room), PLAIN, 13.5 * s, INK, middle=True)
    return out


def strip(width: int, s: float, title: str, icon: Image.Image | None) -> Image.Image:
    """The top strip of a window: the bar's deep blue with the gold rule under it, the icon and the title."""
    h = round(STRIP * s)
    out = art._gradient(width, h, [(0, (27, 47, 124)), (1, (7, 13, 51))]).convert('RGBA')
    shine = Image.new('RGBA', (width, h), (111, 160, 255, 0))
    column = Image.new('L', (1, h))
    column.putdata([round(255 * 0.16 * max(0.0, 1 - y / (h * 0.45))) for y in range(h)])
    shine.putalpha(column.resize((width, h)))
    out.alpha_composite(shine)
    pencil = ImageDraw.Draw(out)
    rule = 2 if s >= 1.4 else 1
    pencil.rectangle((0, h - 1, width, h), fill=(2, 4, 15, 255))
    pencil.rectangle((0, h - 1 - rule, width, h - 2), fill=(200, 179, 106, 255))
    pencil.rectangle((0, 0, width, 0), fill=(127, 164, 240, 255))
    x = round(10 * s)
    if icon is not None:
        d = round(22 * s)
        out.alpha_composite(icon.resize((d, d), Image.LANCZOS), (x, (h - rule - d) // 2))
        x += d + round(9 * s)
    layer = art.text_layer(title.upper(), BOLD, 11 * s, GOLD, 2.6 * s)
    pad = round(11 * s * 0.5)
    _lay(out, layer, x - pad, round((h - rule) / 2 - 0.72 * 11 * s) - pad)
    return out


STRIP = 36                                  # height of the top strip, in layout units


class Corner:
    """The window's buttons at its top right corner: where they are and how they look."""

    def __init__(self, s: float, kinds=('min', 'max', 'close'), size: float = 24, gap: float = 6):
        self.s, self.kinds = s, kinds
        self.px, self.gap = round(size * s), round(gap * s)
        self.rects: dict = {}

    def place(self, right: int, top: int) -> None:
        x = right - self.px
        for kind in reversed(self.kinds):
            self.rects[kind] = (x, top, x + self.px, top + self.px)
            x -= self.px + self.gap

    def at(self, x: float, y: float):
        for kind, (x0, y0, x1, y1) in self.rects.items():
            if x0 <= x < x1 and y0 <= y < y1:
                return kind
        return None

    def paint(self, image: Image.Image, hot, maximized: bool = False) -> None:
        for kind, (x0, y0, _, _) in self.rects.items():
            picture = square_button('restore' if kind == 'max' and maximized else kind, self.px, hot == kind)
            image.paste(picture, (x0, y0), picture)


# ----------------------------------------------------------------------------------------------------
# The icon
# ----------------------------------------------------------------------------------------------------
def tile(picture: Image.Image, size: int, sharp: bool = False) -> Image.Image:
    """The program's icon: a rounded blue tile, like the bar's buttons, with `picture` on it."""
    out = panel(size, size, size * 0.22, (52, 100, 212), (9, 22, 84), (185, 212, 255), max(1.0, size / 40), 0.26).copy()
    box = picture.getbbox()
    if box:
        picture = picture.crop(box)
        room = size * 0.80
        k = min(room / picture.width, room / picture.height)
        if sharp and k >= 1:
            k = max(1, int(k))
        w, h = max(1, round(picture.width * k)), max(1, round(picture.height * k))
        picture = picture.resize((w, h), Image.NEAREST if sharp and k >= 1 else Image.LANCZOS)
        out.alpha_composite(picture, ((size - w) // 2, (size - h) // 2))
    out.putalpha(_mask(size, size, size * 0.22))
    return out


def crystal_icon(size: int = 256) -> Image.Image:
    return tile(art.crystal(size), size)


def _pack_file(pack: Path, want: str, engine: Path) -> bytes | None:
    """One file out of a CRI pack, read with the server engine's own reader."""
    if str(engine) not in sys.path:
        sys.path.insert(0, str(engine))
    from crilayla import decompress
    from ios_art import _cells, _plain
    with open(pack, 'rb') as fh:
        head = fh.read(16)
        if head[:4] != b'CPK ':
            return None
        header = {k: v[1] for k, v in _cells(_plain(fh.read(struct.unpack_from('<Q', head, 8)[0])))[1][0].items()}
        toc = header.get('TocOffset') or 0
        if not toc:
            return None
        fh.seek(toc)
        head = fh.read(16)
        if head[:4] != b'TOC ':
            return None
        for row in _cells(_plain(fh.read(struct.unpack_from('<Q', head, 8)[0])))[1]:
            name = (str(row.get('DirName', (0, ''))[1]) + '/' + str(row['FileName'][1])).lstrip('/')
            if name == want:
                fh.seek(toc + row['FileOffset'][1])
                data = fh.read(row['FileSize'][1])
                return decompress(data) if row['FileSize'][1] != row['ExtractSize'][1] else data
    return None


ICON_FILE = 'icon.rgba'                     # in the game folder: "FFIC", width, height (16-bit), then RGBA; the game window reads it
ICON_PACK = ('game', 'assets', 'cpk', 'common.cpk')
ICON_PICTURE = 'edge/chokobo_00/chokobo_00.png'        # the running Chocobo of the game's loading screen: 4 x 4 frames of 168 x 144


def app_icon(client: Path, engine: Path) -> Image.Image:
    """The icon as a 128-pixel picture. Made from the user's own game files when they are there (and kept in the game
    folder for the game window to use); the crystal otherwise."""
    cache, pack = client / ICON_FILE, client.joinpath(*ICON_PACK)
    try:
        if not pack.is_file():
            return crystal_icon(128)
        if cache.is_file() and cache.stat().st_mtime >= pack.stat().st_mtime:
            raw = cache.read_bytes()
            w, h = struct.unpack_from('<HH', raw, 4)
            if raw[:4] == b'FFIC' and len(raw) == 8 + w * h * 4:
                return Image.frombytes('RGBA', (w, h), raw[8:])
        import io
        data = _pack_file(pack, ICON_PICTURE, engine)
        sheet = Image.open(io.BytesIO(data)).convert('RGBA')
        frame = sheet.crop((0, 0, sheet.width // 4, sheet.height // 4))
        ink = frame.getchannel('A').point(lambda v: 255 if v >= 250 else 0)       # the bird without the soft shadow under it
        box = ink.getbbox()
        icon = tile(frame.crop(box) if box else frame, 128)
        temporary = cache.with_suffix('.tmp')
        temporary.write_bytes(b'FFIC' + struct.pack('<HH', *icon.size) + icon.tobytes())
        os.replace(temporary, cache)
        return icon
    except Exception:  # noqa: BLE001 - any trouble reading the game's files: the program's own icon
        return crystal_icon(128)


# ----------------------------------------------------------------------------------------------------
# A window without the system's frame
# ----------------------------------------------------------------------------------------------------
class Frameless:
    """Takes the system's frame off a tk window and supplies what the frame did: a taskbar button, moving, resizing,
    minimising, filling the screen's work area. `own=False` leaves the window as the system makes it."""

    def __init__(self, win, own: bool = True, taskbar: bool = True):
        self.win, self.own, self.taskbar = win, own, taskbar
        self.maximized = False
        self.before = None                  # geometry before it filled the screen
        self._grab = None
        self._styled = False
        if not own:
            return
        win.overrideredirect(True)
        if WINDOWS and taskbar:
            win.bind('<Map>', self._on_map, add='+')

    # -- Windows plumbing --
    def _hwnd(self):
        import ctypes
        try:
            return int(self.win.wm_frame(), 16)
        except Exception:  # noqa: BLE001
            user32 = ctypes.windll.user32
            user32.GetParent.restype = ctypes.c_void_p
            user32.GetParent.argtypes = [ctypes.c_void_p]
            return user32.GetParent(self.win.winfo_id())

    def _on_map(self, event) -> None:
        """A window without a frame gets no taskbar button of its own accord: ask for one, once it exists."""
        if event.widget is not self.win or self._styled or not self.taskbar:
            return
        self._styled = True
        try:
            import ctypes
            user32 = ctypes.windll.user32
            get, put = user32.GetWindowLongW, user32.SetWindowLongW
            get.argtypes = [ctypes.c_void_p, ctypes.c_int]
            put.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long]
            hwnd = self._hwnd()
            ex = get(hwnd, -20)
            put(hwnd, -20, (ex & ~0x00000080) | 0x00040000)               # not a tool window; an application window
            put(hwnd, -16, get(hwnd, -16) | 0x00020000 | 0x00080000)      # can be minimised; has a system menu
            self.win.withdraw()
            self.win.after(20, self.win.deiconify)
        except Exception:  # noqa: BLE001 - the window still works, without its taskbar button
            pass

    # -- what the strip and the corner ask for --
    # Moving and resizing are done here, by setting the window's place as the pointer moves. They must not be handed
    # to Windows as a press on a title bar (SendMessage WM_NCLBUTTONDOWN from a click handler): Windows then runs its
    # own loop inside that call, Tk goes on serving timers and events from within it, and the first Python callback
    # that fires there ends the whole program with a fatal error ("PyEval_RestoreThread: the GIL is released").
    def press(self, event, resize: bool = False) -> None:
        """A press on a free part of the strip (moves the window) or on the grip (resizes it)."""
        if not self.own or self.maximized:
            return
        self._grab = (resize, event.x_root, event.y_root, self.win.winfo_x(), self.win.winfo_y(), self.win.winfo_width(), self.win.winfo_height())

    def drag(self, event) -> None:
        if not self._grab:
            return
        resize, x0, y0, wx, wy, ww, wh = self._grab
        dx, dy = event.x_root - x0, event.y_root - y0
        if resize:
            low_w, low_h = self.win.minsize()
            self.win.geometry(f'{max(low_w, ww + dx)}x{max(low_h, wh + dy)}')
        else:
            self.win.geometry(f'+{wx + dx}+{wy + dy}')

    def release(self, _event=None) -> None:
        self._grab = None

    def minimize(self) -> None:
        if not self.own:
            self.win.iconify()
            return
        if WINDOWS:
            try:                            # posted, not sent: it is carried out by Tk's own loop, after this click is over
                import ctypes
                post = ctypes.windll.user32.PostMessageW
                post.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
                if post(self._hwnd(), 0x0112, 0xF020, 0):         # WM_SYSCOMMAND, SC_MINIMIZE
                    return
            except Exception:  # noqa: BLE001
                pass
        self.win.withdraw()                 # no taskbar to come back from here: show it again shortly
        self.win.after(1500, self.win.deiconify)

    def work_area(self) -> tuple:
        """The part of this window's screen that is free of the taskbar: x, y, width, height."""
        if WINDOWS:
            try:
                import ctypes
                from ctypes import wintypes

                class Info(ctypes.Structure):
                    _fields_ = [('size', wintypes.DWORD), ('monitor', wintypes.RECT), ('work', wintypes.RECT), ('flags', wintypes.DWORD)]
                user32 = ctypes.windll.user32
                user32.MonitorFromWindow.restype = ctypes.c_void_p
                user32.MonitorFromWindow.argtypes = [ctypes.c_void_p, wintypes.DWORD]
                user32.GetMonitorInfoW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
                info = Info()
                info.size = ctypes.sizeof(Info)
                if user32.GetMonitorInfoW(user32.MonitorFromWindow(self._hwnd(), 2), ctypes.byref(info)):
                    work = info.work
                    if work.right > work.left and work.bottom > work.top:
                        return (work.left, work.top, work.right - work.left, work.bottom - work.top)
            except Exception:  # noqa: BLE001
                pass
        return (0, 0, self.win.winfo_screenwidth(), self.win.winfo_screenheight())

    def toggle_maximize(self) -> tuple:
        """Fills the work area, or goes back. Returns the window's new size."""
        if self.maximized:
            self.maximized = False
            self.win.geometry(self.before)
            size = self.before.split('+')[0].split('x')
            return (int(size[0]), int(size[1]))
        self.before = self.win.geometry()
        x, y, w, h = self.work_area()
        self.maximized = True
        self.win.geometry(f'{w}x{h}+{x}+{y}')
        return (w, h)


def set_app_identity() -> None:
    """Windows groups taskbar buttons by program; without this ours would sit under Python's, with Python's icon."""
    if WINDOWS:
        try:
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('ExviusRevival.Desktop')
        except Exception:  # noqa: BLE001
            pass


def icon_photos(tk_image, icon: Image.Image) -> list:
    """The icon in the sizes the system asks for (title bar, taskbar, Alt+Tab)."""
    return [tk_image.PhotoImage(icon.resize((n, n), Image.LANCZOS)) for n in (128, 64, 48, 32, 24, 16)]
