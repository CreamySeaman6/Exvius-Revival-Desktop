"""What the start window looks like: a late-1990s disc-installer page in the game's dark blue.

Everything is drawn here with Pillow at the window's real pixel size, so it is sharp on any display. The crystal,
the sky and the lettering are this program's own drawings. The two figures in front of the crystal are pixel
sprites read out of the user's own game data once it is installed (see `heroes`); until then the crystal stands
alone. No picture from the game is stored in this program.
"""
from __future__ import annotations

import io
import math
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

APP = Path(__file__).resolve().parent
FONT_DIRS = [APP / 'fonts', APP.parent / 'fonts']
WIDTH, HEIGHT = 960, 600                    # the window in layout units; multiplied by the display's scale

NIGHT = (5, 8, 26)
GOLD = (232, 196, 106)
GILT = (255, 241, 191)
INK = (255, 255, 255)
MUTED = (176, 186, 222)

_fonts: dict = {}


def font(name: str, size: float):
    key = (name, round(size))
    if key not in _fonts:
        path = next((folder / name for folder in FONT_DIRS if (folder / name).is_file()), None)
        try:
            _fonts[key] = ImageFont.truetype(str(path), round(size)) if path else ImageFont.load_default()
        except OSError:
            _fonts[key] = ImageFont.load_default()
    return _fonts[key]


def _gradient(width: int, height: int, stops) -> Image.Image:
    """Top-to-bottom gradient; stops are (position 0..1, (r, g, b))."""
    column = Image.new('RGB', (1, 256))
    pixels = []
    for i in range(256):
        t = i / 255
        for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                f = (t - p0) / (p1 - p0) if p1 > p0 else 0
                pixels.append(tuple(round(c0[k] + (c1[k] - c0[k]) * f) for k in range(3)))
                break
        else:
            pixels.append(stops[-1][1])
    column.putdata(pixels)
    return column.resize((width, height), Image.BILINEAR)


def _glow(size, centre, radius, colour, strength=1.0) -> Image.Image:
    """A soft round light as an RGBA layer."""
    width, height = size
    small = (max(2, width // 8), max(2, height // 8))
    mask = Image.new('L', small, 0)
    cx, cy, rx, ry = centre[0] / 8, centre[1] / 8, radius[0] / 8, radius[1] / 8
    steps = 28
    pencil = ImageDraw.Draw(mask)
    for i in range(steps):
        t = 1 - i / steps
        pencil.ellipse((cx - rx * t, cy - ry * t, cx + rx * t, cy + ry * t), fill=round(255 * strength * (1 - t) ** 1.6))
    mask = mask.filter(ImageFilter.GaussianBlur(max(1, min(small) / 30))).resize(size, Image.BILINEAR)
    layer = Image.new('RGBA', size, colour + (0,))
    layer.putalpha(mask)
    return layer


def crystal(height: int) -> Image.Image:
    """The crystal on its own, RGBA, `height` pixels tall (drawn three times larger and reduced, for smooth edges)."""
    k = 3
    h = height * k
    w = round(h * 0.40)
    pad = round(h * 0.06)
    image = Image.new('RGBA', (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    pencil = ImageDraw.Draw(image)

    def at(x, y):                           # x from -1 to 1 across, y from -1 (top) to 1 (bottom)
        return (pad + (x + 1) / 2 * w, pad + (y + 1) / 2 * h)

    top, bottom = (0.02, -1.0), (-0.02, 1.0)
    ul, ur, ll, lr = (-0.92, -0.42), (0.92, -0.46), (-0.90, 0.34), (0.90, 0.30)
    iul, iur, ill, ilr = (-0.30, -0.33), (0.40, -0.36), (-0.32, 0.44), (0.38, 0.41)
    facets = [((top, iul, ul), (222, 244, 255)), ((top, iur, iul), (168, 220, 255)), ((top, ur, iur), (92, 160, 246)),
              ((ul, iul, ill, ll), (140, 206, 255)), ((iul, iur, ilr, ill), (70, 148, 244)), ((iur, ur, lr, ilr), (36, 92, 204)),
              ((ll, ill, bottom), (58, 128, 226)), ((ill, ilr, bottom), (34, 86, 196)), ((ilr, lr, bottom), (18, 46, 138))]
    for points, colour in facets:
        pencil.polygon([at(*p) for p in points], fill=colour + (236,))
    # light falling through the middle facet, brighter at the top
    shine = Image.new('RGBA', image.size, (0, 0, 0, 0))
    ImageDraw.Draw(shine).polygon([at(*p) for p in (iul, iur, ilr, ill)], fill=(255, 255, 255, 255))
    fade = _gradient(image.width, image.height, [(0, (150, 150, 150)), (0.5, (30, 30, 30)), (1, (0, 0, 0))]).convert('L')
    shine.putalpha(ImageChops.multiply(shine.getchannel('A'), fade))
    image.alpha_composite(shine)
    streak = Image.new('RGBA', image.size, (0, 0, 0, 0))
    ImageDraw.Draw(streak).polygon([at(-0.78, -0.34), at(-0.62, -0.345), at(-0.70, 0.10), at(-0.80, 0.09)], fill=(255, 255, 255, 105))
    image.alpha_composite(streak.filter(ImageFilter.GaussianBlur(k * 3)))
    edge = (236, 248, 255, 210)
    for a, b in ((top, ul), (top, ur), (top, iul), (top, iur), (ul, ll), (ur, lr), (iul, ill), (iur, ilr), (ul, iul), (iul, iur), (iur, ur),
                 (ll, ill), (ill, ilr), (ilr, lr), (ll, bottom), (lr, bottom), (ill, bottom), (ilr, bottom)):
        pencil.line([at(*a), at(*b)], fill=edge, width=max(1, k))
    return image.resize((image.width // k, image.height // k), Image.LANCZOS)


def _sparkle(pencil: ImageDraw.ImageDraw, x: float, y: float, size: float, alpha: int) -> None:
    colour = (255, 255, 255, alpha)
    pencil.polygon([(x, y - size), (x + size * 0.16, y - size * 0.16), (x + size, y), (x + size * 0.16, y + size * 0.16),
                    (x, y + size), (x - size * 0.16, y + size * 0.16), (x - size, y), (x - size * 0.16, y - size * 0.16)], fill=colour)


def backdrop(scale: float, sprites=(), size=None) -> Image.Image:
    """The whole window without lettering: night sky, crystal, the two figures if their sprites are given.
    `size` is the picture's size in pixels when it is not the usual page (the window filling the screen)."""
    w, h = size or (round(WIDTH * scale), round(HEIGHT * scale))
    image = _gradient(w, h, [(0, (10, 18, 62)), (0.45, (8, 14, 52)), (1, NIGHT)]).convert('RGBA')
    cx, cy = w * 0.715, h * 0.47
    image.alpha_composite(_glow((w, h), (cx, cy), (w * 0.52, h * 0.95), (44, 96, 255), 0.55))
    image.alpha_composite(_glow((w, h), (cx, cy - h * 0.02), (w * 0.24, h * 0.60), (120, 200, 255), 0.75))
    # a beam of light through the crystal and faint rays around it
    rays = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    pencil = ImageDraw.Draw(rays)
    rnd = random.Random(7)
    for i in range(22):
        angle = rnd.uniform(0, math.tau)
        spread = rnd.uniform(0.012, 0.045)
        length = rnd.uniform(0.55, 1.1) * h
        pencil.polygon([(cx, cy), (cx + math.cos(angle - spread) * length, cy + math.sin(angle - spread) * length),
                        (cx + math.cos(angle + spread) * length, cy + math.sin(angle + spread) * length)], fill=(150, 205, 255, rnd.randint(10, 26)))
    pencil.polygon([(cx - w * 0.012, 0), (cx + w * 0.012, 0), (cx + w * 0.05, h), (cx - w * 0.05, h)], fill=(170, 220, 255, 34))
    image.alpha_composite(rays.filter(ImageFilter.GaussianBlur(scale * 5)))
    # stars
    stars = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    pencil = ImageDraw.Draw(stars)
    for i in range(170):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h * 0.94)
        size = rnd.choice((0.6, 0.6, 0.8, 1.0, 1.3)) * scale
        pencil.ellipse((x - size, y - size, x + size, y + size), fill=(200, 222, 255, rnd.randint(40, 170)))
    for i in range(9):
        _sparkle(pencil, rnd.uniform(w * 0.45, w * 0.98), rnd.uniform(h * 0.06, h * 0.80), rnd.uniform(4, 9) * scale, rnd.randint(110, 210))
    image.alpha_composite(stars)
    # the floor: a pool of light the crystal hovers over
    floor_y = h * 0.865
    image.alpha_composite(_glow((w, h), (cx, floor_y), (w * 0.30, h * 0.085), (120, 190, 255), 0.85))
    image.alpha_composite(_glow((w, h), (cx, floor_y), (w * 0.12, h * 0.030), (235, 248, 255), 0.9))
    gem = crystal(round(h * 0.60))
    halo = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    gx, gy = round(cx - gem.width / 2), round(h * 0.135)
    halo.paste((150, 215, 255, 255), (gx, gy), gem.getchannel('A'))
    image.alpha_composite(halo.filter(ImageFilter.GaussianBlur(scale * 14)))
    image.alpha_composite(gem, (gx, gy))
    sparkles = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    pencil = ImageDraw.Draw(sparkles)
    _sparkle(pencil, gx + gem.width * 0.30, gy + gem.height * 0.26, 15 * scale, 235)
    _sparkle(pencil, gx + gem.width * 0.70, gy + gem.height * 0.62, 9 * scale, 200)
    image.alpha_composite(sparkles)
    # the two figures, standing in front on either side: pixel sprites, enlarged without smoothing
    if sprites:
        zoom = max(2, round(2.7 * scale))
        spots = ((-0.165, -1), (0.150, 1))                    # left of the crystal, right of it
        for sprite, (offset, side) in zip(sprites, spots):
            box = sprite.getbbox()
            if not box:
                continue
            figure = sprite.crop(box)
            figure = figure.resize((figure.width * zoom, figure.height * zoom), Image.NEAREST)
            px = round(cx + offset * w - figure.width / 2)
            px = max(round(w * 0.49), min(px, w - figure.width - round(22 * scale)))
            py = round(floor_y + 8 * scale - figure.height)
            image.alpha_composite(_glow((w, h), (px + figure.width / 2, floor_y + 6 * scale), (figure.width * 0.40, 9 * scale), (0, 0, 20), 0.85))
            image.alpha_composite(figure, (px, max(0, py)))
    # the left side, where the lettering goes, is darkened; a thin gilt frame runs round the page
    shade = Image.new('L', (256, 1))
    shade.putdata([round(214 * max(0.0, 1 - i / 150) ** 1.2) for i in range(256)])
    veil = Image.new('RGBA', (w, h), (3, 5, 20, 0))
    veil.putalpha(shade.resize((w, h), Image.BILINEAR))
    image.alpha_composite(veil)
    foot = Image.new('L', (1, 256))
    foot.putdata([round(150 * max(0.0, (i - 200) / 55)) for i in range(256)])
    veil = Image.new('RGBA', (w, h), (2, 3, 12, 0))
    veil.putalpha(foot.resize((w, h), Image.BILINEAR))
    image.alpha_composite(veil)
    frame = ImageDraw.Draw(image)
    inset = round(10 * scale)
    frame.rectangle((inset, inset, w - inset - 1, h - inset - 1), outline=GOLD + (150,), width=max(1, round(scale)))
    frame.rectangle((inset + round(4 * scale), inset + round(4 * scale), w - inset - 1 - round(4 * scale), h - inset - 1 - round(4 * scale)),
                    outline=(120, 160, 255, 60), width=1)
    return image


def text_layer(text: str, face: str, size: float, colour, spacing: float = 0.0, shadow: bool = True) -> Image.Image:
    """One line of text as an RGBA picture with a soft drop shadow. Small text is drawn larger and reduced, which
    keeps the letter spacing even."""
    k = 3 if size < 22 else 2 if size < 40 else 1
    big = size * k
    fnt = font(face, big)
    width = sum(fnt.getlength(ch) + spacing * k for ch in text) if spacing else fnt.getlength(text)
    pad = round(size * 0.5) * k
    layer = Image.new('RGBA', (round(width) + 2 * pad, round(big * 1.5) + 2 * pad), (0, 0, 0, 0))
    base = pad + big * 1.08

    def write(target, dy, fill):
        pencil = ImageDraw.Draw(target)
        if spacing:
            x = pad
            for ch in text:
                pencil.text((x, base + dy), ch, font=fnt, fill=fill, anchor='ls')
                x += fnt.getlength(ch) + spacing * k
        else:
            pencil.text((pad, base + dy), text, font=fnt, fill=fill, anchor='ls')

    if shadow:
        dark = Image.new('RGBA', layer.size, (0, 0, 0, 0))
        write(dark, max(1, big / 14), (0, 0, 12, 230))
        layer.alpha_composite(dark.filter(ImageFilter.GaussianBlur(max(1.0, big / 12))))
    write(layer, 0, tuple(colour) + (255,) if len(colour) == 3 else colour)
    if k > 1:
        layer = layer.resize((max(1, layer.width // k), max(1, layer.height // k)), Image.LANCZOS)
    return layer


def text_length(text: str, face: str, size: float) -> float:
    """The width `text_layer` draws `text` at. Measured at the size it is drawn at: the font at `size` alone is
    rounded to whole pixels (12.5 became 12) and came out up to 6% narrower than the drawn line."""
    k = 3 if size < 22 else 2 if size < 40 else 1
    return font(face, size * k).getlength(text) / k


def wrap(text: str, face: str, size: float, width: float) -> list[str]:
    out = []
    for paragraph in text.split('\n'):
        line = ''
        for word in paragraph.split():
            trial = (line + ' ' + word).strip()
            if text_length(trial, face, size) > width and line:
                out.append(line)
                line = word
            else:
                line = trial
        out.append(line)
    return out


class Page:
    """Lays the start window's lettering over the backdrop and says where the clickable lines are."""

    def __init__(self, scale: float, sprites=(), size=None):
        self.scale = scale
        self.given_size = size
        self.base = backdrop(scale, sprites, size)
        self.base_has_figures = bool(sprites)
        self.size = self.base.size
        self.rows: list = []                # (x0, y0, x1, y1, key) of the menu lines, in pixels

    def set_figures(self, sprites) -> None:
        self.base = backdrop(self.scale, sprites, self.given_size)
        self.base_has_figures = bool(sprites)

    def render(self, headline: str, detail: str, menu: list, hot: str | None, progress, status: str, notes: list[str], busy_tick: int = 0) -> Image.Image:
        """menu: [(key, label, enabled)]. progress: None, a number from 0 to 1, or 'busy'."""
        s = self.scale
        image = self.base.copy()
        left = round(46 * s)

        def line(text, face, size, colour, x, y, spacing=0.0):
            layer = text_layer(text, face, size * s, colour, spacing * s)
            pad = round(size * s * 0.5)
            image.alpha_composite(layer, (round(x) - pad, round(y) - pad))
            return layer.width - 2 * pad

        line('EXVIUS REVIVAL  DESKTOP', 'Roboto-Bold.ttf', 11.5, GOLD, left, 30 * s, spacing=3.2)
        ImageDraw.Draw(image).rectangle((left, round(52 * s), left + round(250 * s), round(52 * s) + max(1, round(s)) - 1), fill=GOLD + (120,))
        y = 64 * s
        size = 31 if max(font('Cinzel-Bold.ttf', 31 * s).getlength(part) for part in headline.split('\n')) <= 470 * s else 26
        for part in headline.split('\n'):
            line(part, 'Cinzel-Bold.ttf', size, INK, left, y)
            y += size * 1.28 * s
        y += 8 * s
        for part in wrap(detail, 'Roboto-Regular.ttf', 14 * s, 420 * s):
            line(part, 'Roboto-Regular.ttf', 14, (226, 232, 250), left, y)
            y += 21 * s
        # the menu, as on an installer disc: one line per choice, the one under the pointer lit up
        self.rows = []
        y = max(y + 22 * s, 232 * s)
        for key, label, enabled in menu:
            lit = enabled and key == hot
            colour = GILT if lit else INK if enabled else (120, 128, 160)
            first = key == menu[0][0]
            size = 25 if first else 19
            row_h = (46 if first else 36) * s
            if lit:
                bar = Image.new('RGBA', (round(400 * s), round(row_h - 4 * s)), (0, 0, 0, 0))
                fade = Image.new('L', (256, 1))
                fade.putdata([round(70 * (1 - i / 255) ** 1.5) for i in range(256)])
                bar.paste((232, 196, 106, 255), (0, 0, bar.width, bar.height), fade.resize(bar.size))
                image.alpha_composite(bar, (left - round(14 * s), round(y + 1 * s)))
                marker = ImageDraw.Draw(image)
                my = y + row_h / 2 - 1 * s
                marker.polygon([(left - 8 * s, my - 6 * s), (left + 2 * s, my), (left - 8 * s, my + 6 * s)], fill=GILT + (255,))
            width = line(label, 'Cinzel-Bold.ttf', size, colour, left + 14 * s, y + (row_h - size * 1.42 * s) / 2)
            self.rows.append((left - 14 * s, y, left + 14 * s + max(width, 250 * s), y + row_h, key if enabled else None))
            y += row_h + (6 * s if first else 0)
        # progress and what is going on
        lower = self.size[1] - HEIGHT * s                # a taller page keeps these at its foot
        bar_y, bar_w, bar_h = round(504 * s + lower), round(430 * s), max(6, round(9 * s))
        pencil = ImageDraw.Draw(image)
        if progress is not None:
            pencil.rounded_rectangle((left - 1, bar_y - 1, left + bar_w + 1, bar_y + bar_h + 1), radius=bar_h, outline=(185, 212, 255, 170), width=1)
            pencil.rounded_rectangle((left, bar_y, left + bar_w, bar_y + bar_h), radius=bar_h, fill=(4, 8, 30, 235))
            if progress == 'busy':
                span = bar_w * 0.28
                x0 = left + (bar_w - span) * (0.5 - 0.5 * math.cos(busy_tick / 9.0))
                pencil.rounded_rectangle((x0, bar_y, x0 + span, bar_y + bar_h), radius=bar_h, fill=GOLD + (255,))
            elif progress > 0:
                pencil.rounded_rectangle((left, bar_y, left + max(bar_h, bar_w * min(1.0, progress)), bar_y + bar_h), radius=bar_h, fill=GOLD + (255,))
        if status:
            line(status, 'Roboto-Regular.ttf', 12.5, GILT, left, bar_y + bar_h + 5 * s)
        y = 548 * s + lower
        for note in notes[-2:]:
            line(note[:110], 'Roboto-Regular.ttf', 11, MUTED, left, y)
            y += 15 * s
        return image.convert('RGB')

    def row_at(self, x: float, y: float):
        for x0, y0, x1, y1, key in self.rows:
            if key and x0 <= x <= x1 and y0 <= y <= y1:
                return key
        return None


HERO_UNITS = ('100015807', '100011707')     # Awakened Rain and Pyro Glacial Lasswell, the two of the opening movie


def heroes(save: Path, data: Path, engine: Path) -> tuple:
    """The two figures' sprites from the user's own game data, or () while there is none."""
    out = []
    for unit in HERO_UNITS:
        picture = None
        cached = save / 'ios-cache' / 'notice-art' / f'unit_ills_{unit}.png'    # copied out by the server on its first start
        try:
            if cached.is_file():
                picture = Image.open(cached)
                picture.load()
        except OSError:
            picture = None
        if picture is None and (data / 'native').is_dir() and engine.is_dir():
            try:                            # straight from the unit packs, with the server's own reader
                import sys
                if str(engine) not in sys.path:
                    sys.path.insert(0, str(engine))
                import unit_art
                raw = unit_art.own_picture(unit, [str(data / 'native')])
                if raw:
                    picture = Image.open(io.BytesIO(raw))
                    picture.load()
            except Exception:  # noqa: BLE001 - the page simply has no figures
                picture = None
        if picture is None:
            return ()
        out.append(picture.convert('RGBA'))
    return tuple(out)
