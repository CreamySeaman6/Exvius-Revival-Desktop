"""Draws the server's own web pages (News notices, Drop Rates, reward lists) as pictures for the game window.

The phone shows these pages in a web view on top of the game. The desktop game has no browser inside it, so
the local server draws the page here and the game window shows the picture, scrolls it and follows its links.

This is not a browser. It understands the HTML and CSS these pages are written in: blocks, text, pictures,
tables, grid and flex rows, colours, gradients, borders and rounded corners. There is no scripting.

    picture, links, height = render(html, base_url, width_px, scale, fetch)

`fetch(url)` returns a picture's bytes or None. `links` is a list of (x, y, w, h, address) in picture pixels.
"""
from __future__ import annotations

import io
import math
import re
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT_DIRS = [Path(__file__).resolve().parent / 'fonts', Path(__file__).resolve().parent.parent / 'fonts']
FONT_FILES = {('body', 0): 'Roboto-Regular.ttf', ('body', 1): 'Roboto-Bold.ttf', ('body', 2): 'Roboto-Italic.ttf',
              ('display', 0): 'Cinzel-Bold.ttf', ('display', 1): 'Cinzel-Bold.ttf', ('fallback', 0): 'DejaVuSans.ttf'}

VOID = {'img', 'br', 'meta', 'link', 'hr', 'input', 'source', 'wbr'}
SKIP = {'script', 'title', 'head', 'template', 'noscript'}
BLOCK = {'html', 'body', 'div', 'p', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'section', 'article', 'header', 'footer', 'main',
         'nav', 'aside', 'ul', 'ol', 'li', 'figure', 'figcaption', 'blockquote', 'form', 'hr', 'pre', 'dl', 'dt', 'dd'}
INHERITED = {'color', 'font-family', 'font-size', 'font-weight', 'font-style', 'line-height', 'text-align', 'letter-spacing',
             'text-transform', 'visibility', 'list-style', 'text-shadow', 'overflow-wrap', 'white-space', 'border-collapse'}
UA = """
html{color:#000;font-size:16px;font-family:sans-serif;line-height:1.25}
body{margin:8px}
h1{font-size:2em;font-weight:700;margin:.67em 0}h2{font-size:1.5em;font-weight:700;margin:.83em 0}
h3{font-size:1.17em;font-weight:700;margin:1em 0}h4{font-weight:700;margin:1.33em 0}
p{margin:1em 0}ul,ol{margin:1em 0;padding-left:40px}
b,strong,th{font-weight:700}i,em{font-style:italic}small{font-size:.83em}
a{color:#9cd3ff;text-decoration:underline}
table{display:table}tr{display:table-row}td,th{display:table-cell;padding:1px}th{text-align:center}
thead,tbody,tfoot{display:table-row-group}hr{border-top:1px solid #888;margin:8px 0}
"""
NAMED = {'white': (255, 255, 255, 255), 'black': (0, 0, 0, 255), 'transparent': (0, 0, 0, 0), 'red': (255, 0, 0, 255),
         'gold': (255, 215, 0, 255), 'gray': (128, 128, 128, 255), 'grey': (128, 128, 128, 255), 'silver': (192, 192, 192, 255),
         'yellow': (255, 255, 0, 255), 'green': (0, 128, 0, 255), 'blue': (0, 0, 255, 255), 'orange': (255, 165, 0, 255)}


# ---------------------------------------------------------------------------------------------------------
# HTML
# ---------------------------------------------------------------------------------------------------------
class Node:
    __slots__ = ('tag', 'attrs', 'children', 'text', 'parent', 'style', 'pseudo')

    def __init__(self, tag, attrs=None, parent=None, text=''):
        self.tag, self.attrs, self.children, self.text, self.parent = tag, attrs or {}, [], text, parent
        self.style, self.pseudo = {}, {}


class _Reader(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node('#root')
        self.at = self.root
        self.css = []
        self.skip = 0
        self.in_style = False

    def handle_starttag(self, tag, attrs):
        if tag == 'style':
            self.in_style = True
            return
        if tag in ('script', 'title', 'template', 'noscript'):
            self.skip += 1
            return
        if self.skip or tag in ('head', 'meta', 'link', '!doctype'):
            return
        if tag == 'li' and self.at.tag == 'li':            # <li> closes the one before it
            self.at = self.at.parent
        if tag in ('tr',) and self.at.tag in ('tr',):
            self.at = self.at.parent
        if tag in ('td', 'th') and self.at.tag in ('td', 'th'):
            self.at = self.at.parent
        if tag == 'p' and self.at.tag == 'p':
            self.at = self.at.parent
        node = Node(tag, {k: (v if v is not None else '') for k, v in attrs}, self.at)
        self.at.children.append(node)
        if tag not in VOID:
            self.at = node

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID and tag not in ('style', 'script'):
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        if tag == 'style':
            self.in_style = False
            return
        if tag in ('script', 'title', 'template', 'noscript'):
            self.skip = max(0, self.skip - 1)
            return
        probe = self.at
        while probe is not self.root and probe.tag != tag:
            probe = probe.parent
        if probe is not self.root:
            self.at = probe.parent

    def handle_data(self, data):
        if self.in_style:
            self.css.append(data)
        elif not self.skip:
            self.at.children.append(Node('#text', parent=self.at, text=data))


# ---------------------------------------------------------------------------------------------------------
# CSS
# ---------------------------------------------------------------------------------------------------------
def _split(text: str, sep: str) -> list[str]:
    """Split at `sep` outside brackets and quotes (sep ' ' splits at any run of white space)."""
    out, depth, cur, quote = [], 0, [], ''
    for ch in text:
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = ''
            continue
        if ch in '"\'':
            quote = ch
            cur.append(ch)
        elif ch == '(':
            depth += 1
            cur.append(ch)
        elif ch == ')':
            depth -= 1
            cur.append(ch)
        elif depth == 0 and (ch == sep or (sep == ' ' and ch.isspace())):
            if cur or sep != ' ':
                out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur or (sep != ' ' and out):
        out.append(''.join(cur).strip())
    return [part for part in out if part or sep != ' ']


def _declarations(text: str) -> dict:
    out = {}
    for part in _split(text, ';'):
        name, colon, value = part.partition(':')
        if colon:
            value = value.strip()
            if value.endswith('!important'):
                value = value[:-10].strip()
            out[name.strip().lower() if not name.strip().startswith('--') else name.strip()] = value
    return out


def _selector(text: str):
    """[(combinator, tag, id, classes)], pseudo-element, specificity."""
    pseudo = ''
    found = re.search(r'::?(before|after)\s*$', text)
    if found:
        pseudo, text = found.group(1), text[:found.start()]
    text = re.sub(r'\s*>\s*', ' > ', text.strip())
    parts, combinator, spec = [], ' ', [0, 0, 0]
    for token in text.split():
        if token == '>':
            combinator = '>'
            continue
        token = re.sub(r':(root|link|visited)', lambda m: '' if m.group(1) != 'root' else '\x00root', token)
        if ':' in token:                                    # :hover, :first-child and the like: never matches
            return None
        root = '\x00root' in token
        token = token.replace('\x00root', '')
        tag = re.match(r'[a-zA-Z][\w-]*|\*', token)
        ident = re.search(r'#([\w-]+)', token)
        classes = tuple(re.findall(r'\.([\w-]+)', token))
        tag = tag.group(0).lower() if tag else ('html' if root else '*')
        spec[0] += 1 if ident else 0
        spec[1] += len(classes) + (1 if root else 0)
        spec[2] += 1 if tag != '*' else 0
        parts.append((combinator, tag, ident.group(1) if ident else None, classes))
        combinator = ' '
    if not parts:
        return None
    return parts, pseudo, tuple(spec)


def _rules(css: str) -> list:
    css = re.sub(r'/\*.*?\*/', '', css, flags=re.S)
    css = re.sub(r'@media[^{]*\{(?:[^{}]*\{[^{}]*\})*[^{}]*\}', '', css)     # no media queries: one layout
    out, order = [], 0
    for selectors, body in re.findall(r'([^{}]+)\{([^{}]*)\}', css):
        declared = _declarations(body)
        for one in _split(selectors, ','):
            parsed = _selector(one)
            if parsed:
                out.append((parsed[2], order, parsed[0], parsed[1], declared))
                order += 1
    out.sort(key=lambda rule: (rule[0], rule[1]))
    return out


def _matches_one(node: Node, part) -> bool:
    _, tag, ident, classes = part
    if tag != '*' and node.tag != tag:
        return False
    if ident and node.attrs.get('id') != ident:
        return False
    if classes:
        have = node.attrs.get('class', '').split()
        return all(name in have for name in classes)
    return True


def _matches(node: Node, parts) -> bool:
    if not _matches_one(node, parts[-1]):
        return False
    at, index = node, len(parts) - 1
    while index > 0:
        combinator = parts[index][0]
        index -= 1
        at = at.parent
        if combinator == '>':
            if at is None or not _matches_one(at, parts[index]):
                return False
        else:
            while at is not None and not _matches_one(at, parts[index]):
                at = at.parent
            if at is None:
                return False
    return True


_SIDES = ('top', 'right', 'bottom', 'left')


def _four(values: list[str]) -> list[str]:
    if len(values) == 1:
        return values * 4
    if len(values) == 2:
        return [values[0], values[1], values[0], values[1]]
    if len(values) == 3:
        return [values[0], values[1], values[2], values[1]]
    return values[:4]


def _expand(declared: dict) -> dict:
    """Shorthand properties written out."""
    out = {}
    for name, value in declared.items():
        if name in ('margin', 'padding'):
            for side, one in zip(_SIDES, _four(_split(value, ' '))):
                out[f'{name}-{side}'] = one
        elif name == 'border' or (name.startswith('border-') and name[7:] in _SIDES):
            sides = _SIDES if name == 'border' else (name[7:],)
            width, color, style = '3px', 'currentcolor', 'none'
            for token in _split(value, ' '):
                if token in ('solid', 'dashed', 'dotted', 'double', 'none', 'hidden'):
                    style = token
                elif re.match(r'^[\d.]', token) or token in ('thin', 'medium', 'thick'):
                    width = {'thin': '1px', 'medium': '3px', 'thick': '5px'}.get(token, token)
                else:
                    color = token
            for side in sides:
                out[f'border-{side}-width'] = width if style not in ('none', 'hidden') else '0'
                out[f'border-{side}-color'] = color
        elif name == 'border-width':
            for side, one in zip(_SIDES, _four(_split(value, ' '))):
                out[f'border-{side}-width'] = one
        elif name == 'border-color':
            for side, one in zip(_SIDES, _four(_split(value, ' '))):
                out[f'border-{side}-color'] = one
        elif name == 'font':
            tokens = _split(value, ' ')
            size_at = next((i for i, t in enumerate(tokens) if re.match(r'^[\d.]+(px|em|rem|%)', t)), None)
            if size_at is None:
                continue
            out['font-style'], out['font-weight'] = 'normal', '400'
            for token in tokens[:size_at]:
                if token in ('italic', 'oblique'):
                    out['font-style'] = 'italic'
                elif token == 'bold' or token.isdigit():
                    out['font-weight'] = '700' if token == 'bold' else token
            size, slash, height = tokens[size_at].partition('/')
            out['font-size'] = size
            out['line-height'] = height if slash else 'normal'
            out['font-family'] = ' '.join(tokens[size_at + 1:])
        elif name == 'background':
            out['background'] = value
        elif name == 'background-color':
            out['background'] = value
        elif name == 'gap':
            both = _split(value, ' ')
            out['row-gap'], out['column-gap'] = both[0], both[-1]
        elif name == 'place-items':
            both = _split(value, ' ')
            out['align-items'], out['justify-items'] = both[0], both[-1]
        elif name == 'flex':
            out['flex-grow'] = '0' if value.startswith('none') else (_split(value, ' ')[0] if value[:1].isdigit() else '1')
        elif name == 'list-style-type':
            out['list-style'] = value
        elif name == 'overflow-x' or name == 'overflow-y':
            out['overflow'] = value
        else:
            out[name] = value
    return out


def _color(value: str, current=(0, 0, 0, 255)):
    value = (value or '').strip().lower()
    if value in ('currentcolor', 'inherit', ''):
        return current
    if value in NAMED:
        return NAMED[value]
    if value.startswith('#'):
        digits = value[1:]
        if len(digits) in (3, 4):
            digits = ''.join(ch * 2 for ch in digits)
        try:
            number = int(digits, 16)
        except ValueError:
            return current
        if len(digits) == 6:
            return (number >> 16 & 255, number >> 8 & 255, number & 255, 255)
        return (number >> 24 & 255, number >> 16 & 255, number >> 8 & 255, number & 255)
    found = re.match(r'rgba?\(([^)]*)\)', value)
    if found:
        parts = [p for p in re.split(r'[,\s/]+', found.group(1).strip()) if p]
        try:
            rgb = [float(p[:-1]) * 2.55 if p.endswith('%') else float(p) for p in parts[:3]]
            alpha = 1.0 if len(parts) < 4 else (float(parts[3][:-1]) / 100 if parts[3].endswith('%') else float(parts[3]))
        except ValueError:
            return current
        return (int(rgb[0]), int(rgb[1]), int(rgb[2]), max(0, min(255, round(alpha * 255))))
    return current


class Page:
    """One page being drawn: its styles, fonts and pictures."""

    def __init__(self, html: str, base_url: str, width: int, scale: float, fetch, viewport_height: int = 0):
        reader = _Reader()
        reader.feed(html)
        self.root = reader.root
        self.rules = _rules(UA) + _rules('\n'.join(reader.css))
        self.base, self.width, self.scale, self.fetch = base_url, width, scale, fetch
        self.viewport_height = viewport_height or width * 3 // 2
        self.vars = {}
        self.fonts, self.pictures, self.missing = {}, {}, {}
        self.links = []
        for rule in self.rules:
            for name, value in rule[4].items():
                if name.startswith('--') and rule[3] == '':
                    self.vars[name] = value

    # ---- values ------------------------------------------------------------------------------------
    def resolve(self, value: str) -> str:
        for _ in range(8):
            found = re.search(r'var\(\s*(--[\w-]+)\s*(?:,([^()]*(?:\([^()]*\))?[^()]*))?\)', value)
            if not found:
                break
            value = value[:found.start()] + self.vars.get(found.group(1), (found.group(2) or '').strip()) + value[found.end():]
        return value

    def length(self, value, em: float, base: float = 0.0, auto=None):
        """A CSS length in picture pixels. `em` is the font size in picture pixels, `base` what a percentage is of."""
        if value is None:
            return auto
        value = value.strip().lower()
        if value in ('auto', 'none', 'normal', ''):
            return auto
        if value.startswith(('max(', 'min(')):
            parts = [self.length(p, em, base, 0.0) for p in _split(value[4:-1], ',')]
            return (max if value.startswith('max') else min)(parts) if parts else auto
        if value.startswith('env('):
            parts = _split(value[4:-1], ',')
            return self.length(parts[1], em, base, 0.0) if len(parts) > 1 else 0.0
        if value.startswith('calc('):
            return auto
        try:
            if value.endswith('px'):
                return float(value[:-2]) * self.scale
            if value.endswith('rem'):
                return float(value[:-3]) * 16 * self.scale
            if value.endswith('em'):
                return float(value[:-2]) * em
            if value.endswith('%'):
                return float(value[:-1]) * base / 100
            if value.endswith(('vw', 'vh')):
                return float(value[:-2]) * (self.width if value.endswith('vw') else self.viewport_height) / 100
            return float(value) * self.scale
        except ValueError:
            return auto

    # ---- styles ------------------------------------------------------------------------------------
    def style_tree(self, node: Node, parent_style: dict | None = None) -> None:
        if node.tag == '#text':
            return
        style = {name: value for name, value in (parent_style or {}).items() if name in INHERITED}
        own, before, after = {}, None, None
        if node.tag != '#root':
            for _, _, parts, pseudo, declared in self.rules:
                if _matches(node, parts):
                    if pseudo == 'before':
                        before = {**(before or {}), **declared}
                    elif pseudo == 'after':
                        after = {**(after or {}), **declared}
                    else:
                        own.update(_expand(declared))
            if node.attrs.get('style'):
                own.update(_expand(_declarations(node.attrs['style'])))
        parent_size = float((parent_style or {}).get('font-size', 16 * self.scale))
        for name, value in own.items():
            if name.startswith('--'):
                continue
            value = self.resolve(value)
            if value == 'inherit':
                if parent_style and name in parent_style:
                    style[name] = parent_style[name]
                continue
            if name == 'font-size':
                value = {'smaller': '.83em', 'larger': '1.2em', 'small': '13px', 'medium': '16px', 'large': '18px'}.get(value, value)
                style[name] = self.length(value, parent_size, parent_size, parent_size)
            else:
                style[name] = value
        style.setdefault('font-size', parent_size)
        if 'display' not in style:
            style['display'] = 'block' if node.tag in BLOCK or node.tag == '#root' else 'inline'
        node.style = style
        for which, declared in (('before', before), ('after', after)):
            if declared is not None and 'content' in declared and declared['content'].strip() not in ('none', 'normal'):
                fake = Node('::' + which, parent=node)
                fake.style = {name: value for name, value in style.items() if name in INHERITED}
                for name, value in _expand(declared).items():
                    fake.style[name] = self.resolve(value)
                fake.style.setdefault('display', 'inline')
                content = declared['content'].strip().strip('"\'')
                if content:
                    fake.children.append(Node('#text', parent=fake, text=content))
                node.pseudo[which] = fake
        for child in node.children:
            self.style_tree(child, style)

    # ---- fonts and text ----------------------------------------------------------------------------
    def font(self, style: dict, fallback: bool = False):
        family = style.get('font-family', '').lower()
        kind = 'display' if ('cinzel' in family or re.search(r'(^|,)\s*serif', family)) and 'sans-serif' not in family.split(',')[0] else 'body'
        weight = style.get('font-weight', '400')
        bold = weight in ('bold', 'bolder') or (weight.isdigit() and int(weight) >= 600)
        variant = 1 if bold else 2 if style.get('font-style') == 'italic' and kind == 'body' else 0
        size = max(4, round(float(style['font-size'])))
        key = ('fallback', 0, size) if fallback else (kind, variant, size)
        if key not in self.fonts:
            name = FONT_FILES.get(key[:2], 'Roboto-Regular.ttf')
            path = next((folder / name for folder in FONT_DIRS if (folder / name).is_file()), None)
            try:
                self.fonts[key] = ImageFont.truetype(str(path), size) if path else ImageFont.load_default()
            except OSError:
                self.fonts[key] = ImageFont.load_default()
        return self.fonts[key]

    def has_glyph(self, font, ch: str) -> bool:
        if ch < 'ƀ' or ch.isspace():
            return True
        key = (id(font), ch)
        if key not in self.missing:
            try:
                self.missing[key] = bytes(font.getmask(ch)) == bytes(font.getmask('￿')) and font.getlength(ch) == font.getlength('￿')
            except (OSError, ValueError):
                self.missing[key] = True
        return not self.missing[key]

    def pieces(self, text: str, style: dict):
        """[(font, text)] with characters the main font lacks handed to the fallback font."""
        main = self.font(style)
        out, run, run_font = [], [], main
        for ch in text:
            font = main if self.has_glyph(main, ch) else self.font(style, True)
            if font is not run_font and run:
                out.append((run_font, ''.join(run)))
                run = []
            run_font = font
            run.append(ch)
        if run:
            out.append((run_font, ''.join(run)))
        return out

    def text_width(self, text: str, style: dict) -> float:
        spacing = self.length(style.get('letter-spacing'), float(style['font-size']), 0, 0.0) or 0.0
        return sum(font.getlength(part) for font, part in self.pieces(text, style)) + spacing * len(text)

    def picture(self, src: str):
        url = urljoin(self.base, src)
        if url not in self.pictures:
            image = None
            try:
                data = self.fetch(url)
                if data:
                    image = Image.open(io.BytesIO(data))
                    image.load()
                    image = image.convert('RGBA')
            except Exception:  # noqa: BLE001 - a picture that cannot be read is simply left out
                image = None
            self.pictures[url] = image
        return self.pictures[url]


# ---------------------------------------------------------------------------------------------------------
# Boxes and layout
# ---------------------------------------------------------------------------------------------------------
class Box:
    __slots__ = ('node', 'style', 'x', 'y', 'w', 'h', 'children', 'lines', 'image', 'pad', 'border', 'margin', 'baseline',
                 'href', 'hidden')

    def __init__(self, node: Node, style: dict | None = None):
        self.node, self.style = node, style if style is not None else node.style
        self.x = self.y = self.w = self.h = 0.0
        self.children, self.lines, self.image = [], None, None
        self.pad = self.border = self.margin = (0.0, 0.0, 0.0, 0.0)
        self.baseline = None
        self.href = None
        self.hidden = False


class Layout:
    def __init__(self, page: Page):
        self.page = page

    # ---- helpers -----------------------------------------------------------------------------------
    def edges(self, box: Box, avail: float) -> None:
        style, em, length = box.style, float(box.style['font-size']), self.page.length
        box.pad = tuple(max(0.0, length(style.get(f'padding-{s}'), em, avail, 0.0)) for s in _SIDES)
        box.border = tuple(max(0.0, length(style.get(f'border-{s}-width'), em, avail, 0.0)) for s in _SIDES)
        box.border = tuple(max(1.0, round(b)) if b > 0 else 0.0 for b in box.border)
        box.margin = tuple(length(style.get(f'margin-{s}'), em, avail, 0.0) for s in _SIDES)

    def kids(self, node: Node) -> list[Node]:
        out = []
        if 'before' in node.pseudo:
            out.append(node.pseudo['before'])
        out.extend(node.children)
        if 'after' in node.pseudo:
            out.append(node.pseudo['after'])
        return [k for k in out if k.tag == '#text' or k.style.get('display') != 'none']

    @staticmethod
    def inline_level(node: Node) -> bool:
        return node.tag == '#text' or node.style.get('display', 'inline') in ('inline', 'inline-block', 'inline-flex')

    def image_size(self, node: Node, style: dict, avail: float):
        """(picture or None, width, height) for an <img>."""
        page, em = self.page, float(style['font-size'])
        picture = page.picture(node.attrs.get('src', '')) if node.attrs.get('src') else None
        if picture is None and 'display' in node.attrs.get('onerror', ''):
            return None, 0.0, 0.0
        natural_w, natural_h = picture.size if picture else (0, 0)
        attr_w = page.length(node.attrs.get('width'), em) if node.attrs.get('width', '').isdigit() else None
        attr_h = page.length(node.attrs.get('height'), em) if node.attrs.get('height', '').isdigit() else None
        width = page.length(style.get('width'), em, avail)
        height = page.length(style.get('height'), em, 0)
        ratio = (natural_w / natural_h) if natural_h else ((attr_w / attr_h) if attr_w and attr_h else 1.0)
        if width is None and height is None:
            if 'width' in style or 'height' in style:        # width:auto / height:auto: the picture's own shape
                width = attr_w if attr_w is not None and style.get('width') != 'auto' else None
            else:
                width, height = attr_w, attr_h
            if width is None and height is None:
                width = natural_w * page.scale if natural_w else (attr_w or 0.0)
        if width is None:
            width = height * ratio
        if height is None:
            height = width / ratio if ratio else 0.0
        limit = page.length(style.get('max-width'), em, avail)
        if limit is not None and width > limit:
            if style.get('object-fit') != 'contain' or 'height' not in style or style.get('height') == 'auto':
                height = height * limit / width if width else height
            width = limit
        return picture, width, height

    # ---- one element -------------------------------------------------------------------------------
    def build(self, node: Node, avail: float, shrink: bool = False) -> Box:
        """Lay out `node` as a block-level box at (0, 0). `shrink`: only as wide as its content needs."""
        page, style = self.page, node.style
        box = Box(node)
        em = float(style['font-size'])
        self.edges(box, avail)
        if node.tag == 'a' and node.attrs.get('href'):
            box.href = node.attrs['href']
        box.hidden = style.get('visibility') == 'hidden'
        frame_w = box.pad[1] + box.pad[3] + box.border[1] + box.border[3]
        frame_h = box.pad[0] + box.pad[2] + box.border[0] + box.border[2]
        border_box = style.get('box-sizing') == 'border-box'
        if node.tag == 'img':
            picture, width, height = self.image_size(node, style, avail)
            box.image, box.w, box.h = picture, width + frame_w, height + frame_h
            if picture is None and 'visibility' in node.attrs.get('onerror', ''):
                box.hidden = True
            return box
        width = page.length(style.get('width'), em, avail)
        if width is not None and not border_box:
            width += frame_w
        if width is None:
            width = avail - box.margin[1] - box.margin[3]
            if shrink:
                width = min(width, self.natural_width(node, avail) + frame_w)
        limit = page.length(style.get('max-width'), em, avail)
        if limit is not None:
            width = min(width, limit + (0 if border_box else frame_w))
        width = max(width, frame_w)
        box.w = width
        inner = width - frame_w
        display = style.get('display')
        if display == 'grid':
            content_h = self.grid(box, node, inner)
        elif display in ('flex', 'inline-flex'):
            content_h = self.flex(box, node, inner)
        elif display == 'table':
            content_h = self.table(box, node, inner)
        else:
            content_h = self.flow(box, node, inner)
        height = page.length(style.get('height'), em, 0)
        if height is not None:
            height = height if border_box else height + frame_h
            self.place_content(box, height - frame_h, content_h)
        else:
            height = content_h + frame_h
        minimum = page.length(style.get('min-height'), em, 0)
        if minimum is not None and height < minimum:
            self.place_content(box, minimum - frame_h, content_h)
            height = minimum
        box.h = height
        return box

    def place_content(self, box: Box, space: float, used: float) -> None:
        """A box taller than its content: where the content sits (align-content / align-items)."""
        style = box.style
        how = style.get('align-content') or (style.get('align-items') if style.get('display') == 'grid' else None)
        extra = space - used
        if extra <= 0 or how not in ('end', 'flex-end', 'center'):
            return
        self.shift(box, 0, extra if how != 'center' else extra / 2, children_only=True)

    def shift(self, box: Box, dx: float, dy: float, children_only: bool = False) -> None:
        if not children_only:
            box.x += dx
            box.y += dy
        for child in box.children:
            self.shift(child, dx, dy)
        if box.lines:
            for line in box.lines:
                line['x'] += dx
                line['y'] += dy
                for item in line['items']:
                    if item[0] == 'box':
                        self.shift(item[1], dx, dy)

    def natural_width(self, node: Node, avail: float) -> float:
        """How wide the content would be on one line (never more than `avail`)."""
        page = self.page
        if node.tag == '#text':
            return min(avail, page.text_width(self.transform(' '.join(node.text.split()), node.parent.style), node.parent.style))
        style, em = node.style, float(node.style['font-size'])
        if node.tag == 'img':
            return self.image_size(node, style, avail)[1]
        given = page.length(style.get('width'), em, avail)
        if given is not None:
            return given
        frame = sum(page.length(style.get(f'padding-{s}'), em, avail, 0.0) + page.length(style.get(f'border-{s}-width'), em, avail, 0.0)
                    for s in ('left', 'right'))
        kids = self.kids(node)
        if style.get('display') in ('flex', 'inline-flex') or all(self.inline_level(k) for k in kids):
            gap = page.length(style.get('column-gap'), em, avail, 0.0) if style.get('display') in ('flex', 'inline-flex') else 0.0
            total = sum(self.natural_width(k, avail) + (0 if k.tag == '#text' else self.side_margins(k, avail)) for k in kids)
            return min(avail, total + gap * max(0, len(kids) - 1) + frame)
        widest = max((self.natural_width(k, avail) + self.side_margins(k, avail) for k in kids), default=0.0)
        return min(avail, widest + frame)

    def side_margins(self, node: Node, avail: float) -> float:
        if node.tag == '#text':
            return 0.0
        em = float(node.style['font-size'])
        return sum(self.page.length(node.style.get(f'margin-{s}'), em, avail, 0.0) for s in ('left', 'right'))

    @staticmethod
    def transform(text: str, style: dict) -> str:
        how = style.get('text-transform')
        return text.upper() if how == 'uppercase' else text.lower() if how == 'lowercase' else text.title() if how == 'capitalize' else text

    # ---- normal flow -------------------------------------------------------------------------------
    def flow(self, box: Box, node: Node, inner: float) -> float:
        kids = self.kids(node)
        left, top = box.border[3] + box.pad[3], box.border[0] + box.pad[0]
        if kids and all(self.inline_level(k) for k in kids):
            return self.inline(box, kids, left, top, inner)
        y, pending, run = top, 0.0, []
        marker = node.tag in ('ul', 'ol') and node.style.get('list-style', 'disc') != 'none'

        def flush():
            nonlocal y, pending, run
            if run and any(k.tag != '#text' or k.text.strip() for k in run):
                anonymous = Box(node, {**{k: v for k, v in node.style.items() if k in INHERITED}, 'display': 'block'})
                anonymous.w = inner
                anonymous.h = self.inline(anonymous, run, 0.0, 0.0, inner)
                self.shift(anonymous, left, y + pending)
                box.children.append(anonymous)
                y += pending + anonymous.h
                pending = 0.0
            run = []

        count = 0
        for kid in kids:
            if self.inline_level(kid):
                run.append(kid)
                continue
            flush()
            child = self.build(kid, inner)
            gap = max(pending, child.margin[0]) if child.margin[0] >= 0 and pending >= 0 else pending + child.margin[0]
            offset = child.margin[3]
            if kid.style.get('margin-left') == 'auto' and kid.style.get('margin-right') == 'auto':
                offset = max(0.0, (inner - child.w) / 2)
            self.shift(child, left + offset, y + gap)
            box.children.append(child)
            if marker and kid.tag == 'li':
                count += 1
                label = f'{count}.' if node.tag == 'ol' else '•'
                child.lines = (child.lines or []) + [{'x': child.x - float(kid.style['font-size']) * 1.2, 'y': child.y + child.pad[0], 'h': 0, 'marker': True,
                                                      'items': [('text', label, kid.style, 0.0, None)]}]
            y += gap + child.h
            pending = child.margin[2]
        flush()
        return y + pending - top

    def inline(self, box: Box, kids: list[Node], left: float, top: float, inner: float) -> float:
        """Lines of text and inline boxes. Fills box.lines; returns the height used."""
        page = self.page
        tokens = []            # ('word', text, style, width, href) | ('space', style, width) | ('box', Box) | ('break',)

        def walk(node: Node, href):
            if node.tag == '#text':
                style = node.parent.style
                text = self.transform(node.text, style)
                for part in re.findall(r'\s+|\S+', text):
                    if part.isspace():
                        tokens.append(('space', style, page.text_width(' ', style), href))
                    else:
                        tokens.append(('word', part, style, page.text_width(part, style), href))
                return
            if node.tag == 'br':
                tokens.append(('break',))
                return
            display = node.style.get('display')
            if node.tag == 'img' or display in ('inline-block', 'inline-flex'):
                child = self.build(node, inner, shrink=True)
                child.href = child.href or href
                tokens.append(('box', child))
                return
            link = node.attrs.get('href') if node.tag == 'a' else href
            for kid in self.kids(node):
                walk(kid, link or href)

        for kid in kids:
            walk(kid, box.href)
        lines, items, used, last_space = [], [], 0.0, True

        def close():
            nonlocal items, used, last_space
            while items and items[-1][0] == 'space':
                used -= items[-1][3]
                items.pop()
            lines.append((items, used))
            items, used, last_space = [], 0.0, True

        for token in tokens:
            kind = token[0]
            if kind == 'break':
                close()
            elif kind == 'space':
                if not last_space:
                    items.append(('space', ' ', token[1], token[2], token[3]))
                    used += token[2]
                    last_space = True
            elif kind == 'word':
                _, text, style, width, href = token
                if used + width > inner + 0.5 and items:
                    close()
                if width > inner and style.get('overflow-wrap') in ('anywhere', 'break-word') and len(text) > 1:
                    part = ''
                    for ch in text:                                  # a word wider than the line: break inside it
                        if part and page.text_width(part + ch, style) > inner:
                            items.append(('text', part, style, page.text_width(part, style), href))
                            used = page.text_width(part, style)
                            close()
                            part = ''
                        part += ch
                    text, width = part, page.text_width(part, style)
                items.append(('text', text, style, width, href))
                used += width
                last_space = False
            else:
                child = token[1]
                width = child.w + child.margin[1] + child.margin[3]
                if used + width > inner + 0.5 and items:
                    close()
                items.append(('box', child, None, width, None))
                used += width
                last_space = False
        if items:
            close()
        align = box.style.get('text-align', 'left')
        y, out = top, []
        for items, used in lines:
            ascent = descent = height = 0.0
            for item in items:
                if item[0] == 'box':
                    child = item[1]
                    ascent = max(ascent, child.h + child.margin[0] + child.margin[2])
                else:
                    style = item[2]
                    size = float(style['font-size'])
                    font = page.font(style)
                    try:
                        up, down = font.getmetrics()
                    except AttributeError:
                        up, down = size * 0.8, size * 0.2
                    line_height = style.get('line-height', 'normal')
                    if line_height == 'normal':
                        tall = (up + down) * 1.0
                    elif re.match(r'^[\d.]+$', line_height):
                        tall = float(line_height) * size
                    else:
                        tall = page.length(line_height, size, size, up + down)
                    lead = (tall - (up + down)) / 2
                    ascent, descent = max(ascent, up + lead), max(descent, down + lead)
            height = ascent + descent
            if not items:
                height = float(box.style['font-size']) * 1.2
            start = left + (inner - used if align in ('right', 'end') else (inner - used) / 2 if align == 'center' else 0)
            x, placed = start, []
            for item in items:
                if item[0] == 'box':
                    child = item[1]
                    self.shift(child, x + child.margin[3], y + ascent - child.h - child.margin[2])
                    placed.append(('box', child, None, item[3], None))
                else:
                    placed.append((item[0], item[1], item[2], x - start, item[4], item[3]))
                x += item[3]
            out.append({'x': start, 'y': y, 'h': height, 'ascent': ascent, 'items': placed})
            if box.baseline is None:
                box.baseline = y + ascent
            y += height
        box.lines = (box.lines or []) + out
        return y - top

    # ---- grid --------------------------------------------------------------------------------------
    def grid(self, box: Box, node: Node, inner: float) -> float:
        page, style, em = self.page, node.style, float(node.style['font-size'])
        column_gap = page.length(style.get('column-gap'), em, inner, 0.0)
        row_gap = page.length(style.get('row-gap'), em, inner, 0.0)
        template = style.get('grid-template-columns', 'none')
        widths = [inner]
        found = re.match(r'repeat\(\s*auto-fi(?:ll|t)\s*,\s*minmax\(\s*([^,]+),[^)]*\)\s*\)', template)
        if found:
            least = page.length(found.group(1), em, inner, inner)
            count = max(1, int((inner + column_gap) // (least + column_gap)))
            widths = [(inner - column_gap * (count - 1)) / count] * count
        elif template not in ('none', ''):
            template = re.sub(r'repeat\(\s*(\d+)\s*,\s*([^)]+)\)', lambda m: ' '.join([m.group(2).strip()] * int(m.group(1))), template)
            tracks = _split(template, ' ')
            fixed = [page.length(t, em, inner) if not t.endswith('fr') and t not in ('auto', 'min-content', 'max-content') else None for t in tracks]
            parts = sum(float(t[:-2] or 1) if t.endswith('fr') else 1.0 for t, f in zip(tracks, fixed) if f is None)
            free = max(0.0, inner - sum(f for f in fixed if f is not None) - column_gap * (len(tracks) - 1))
            widths = [f if f is not None else free * (float(t[:-2] or 1) if t.endswith('fr') else 1.0) / (parts or 1)
                      for t, f in zip(tracks, fixed)]
        justify = style.get('justify-items', 'stretch')
        align = style.get('align-items', 'stretch')
        left, y = box.border[3] + box.pad[3], box.border[0] + box.pad[0]
        top = y
        kids = [k for k in self.kids(node) if k.tag != '#text' or k.text.strip()]
        for start in range(0, len(kids), len(widths)):
            row, x = [], left
            for kid, width in zip(kids[start:start + len(widths)], widths):
                if kid.tag == '#text':
                    wrapper = Node('#anon', parent=node)
                    wrapper.style = {**{k: v for k, v in style.items() if k in INHERITED}, 'display': 'block'}
                    wrapper.children = [kid]
                    kid = wrapper
                child = self.build(kid, width, shrink=justify != 'stretch')
                offset = child.margin[3]
                if justify == 'center':
                    offset = (width - child.w) / 2
                elif justify in ('end', 'flex-end'):
                    offset = width - child.w - child.margin[1]
                self.shift(child, x + offset, y + child.margin[0])
                row.append((child, child.h + child.margin[0] + child.margin[2]))
                x += width + column_gap
            tall = max((h for _, h in row), default=0.0)
            for child, h in row:
                if h < tall:
                    if align == 'center':
                        self.shift(child, 0, (tall - h) / 2)
                    elif align in ('end', 'flex-end'):
                        self.shift(child, 0, tall - h)
                    elif child.node.tag != 'img' and child.style.get('height') in (None, 'auto'):
                        self.place_content(child, tall - child.margin[0] - child.margin[2] - (child.pad[0] + child.pad[2] + child.border[0] + child.border[2]),
                                           child.h - (child.pad[0] + child.pad[2] + child.border[0] + child.border[2]))
                        child.h = tall - child.margin[0] - child.margin[2]          # stretch to the row
                box.children.append(child)
            y += tall + row_gap
        if kids:
            y -= row_gap
        return y - top

    # ---- flex (rows) -------------------------------------------------------------------------------
    def flex(self, box: Box, node: Node, inner: float) -> float:
        page, style, em = self.page, node.style, float(node.style['font-size'])
        gap = page.length(style.get('column-gap'), em, inner, 0.0)
        row_gap = page.length(style.get('row-gap'), em, inner, 0.0)
        wrap = style.get('flex-wrap', 'nowrap') != 'nowrap'
        align = style.get('align-items', 'stretch')
        justify = style.get('justify-content', 'flex-start')
        items, run = [], []

        def wrap_run():
            if any(k.tag != '#text' or k.text.strip() for k in run):
                wrapper = Node('#anon', parent=node)
                wrapper.style = {**{k: v for k, v in style.items() if k in INHERITED}, 'display': 'block'}
                wrapper.children = list(run)
                items.append(wrapper)
            run.clear()

        for kid in self.kids(node):
            if kid.tag == '#text':
                run.append(kid)
            else:
                wrap_run()
                items.append(kid)
        wrap_run()
        if style.get('flex-direction') == 'column':
            y = top = box.border[0] + box.pad[0]
            for kid in items:
                child = self.build(kid, inner)
                self.shift(child, box.border[3] + box.pad[3] + child.margin[3], y + child.margin[0])
                box.children.append(child)
                y += child.h + child.margin[0] + child.margin[2] + row_gap
            return y - top - (row_gap if items else 0)
        measured = []
        for kid in items:
            grow = float(kid.style.get('flex-grow', '0') or 0)
            margins = self.side_margins(kid, inner)
            if grow:
                basis = 0.0                                  # flex: 1 starts from nothing and takes what is left
            elif kid.tag == 'img':
                basis = self.image_size(kid, kid.style, inner)[1]
            else:
                given = page.length(kid.style.get('width'), float(kid.style['font-size']), inner)
                basis = given if given is not None else min(inner, self.natural_width(kid, inner) + 1.0)
            measured.append([kid, grow, basis, margins])
        rows, current, used = [], [], 0.0
        for entry in measured:
            need = entry[2] + entry[3] + (gap if current else 0)
            if wrap and current and used + need > inner + 0.5:
                rows.append(current)
                current, used = [], 0.0
                need = entry[2] + entry[3]
            current.append(entry)
            used += need
        if current:
            rows.append(current)
        y = top = box.border[0] + box.pad[0]
        left = box.border[3] + box.pad[3]
        for row in rows:
            fixed = sum(entry[2] + entry[3] for entry in row) + gap * (len(row) - 1)
            grows = sum(entry[1] for entry in row)
            free = inner - fixed
            if free < 0 and not grows:                         # too wide: shrink in proportion
                total = sum(entry[2] for entry in row) or 1.0
                for entry in row:
                    entry[2] = max(0.0, entry[2] + free * entry[2] / total)
                free = 0.0
            built = []
            for kid, grow, basis, margins in row:
                width = basis + (free * grow / grows if grows and free > 0 else 0.0)
                if kid.tag == 'img':
                    child = self.build(kid, width)
                else:
                    forced = dict(kid.style)
                    forced['width'] = f'{width / page.scale}px'
                    forced['box-sizing'] = 'border-box'
                    saved, kid.style = kid.style, forced
                    child = self.build(kid, width)
                    kid.style = saved
                    child.style = saved
                built.append(child)
            tall = max((c.h + c.margin[0] + c.margin[2] for c in built), default=0.0)
            line_base = max((c.baseline + c.margin[0] for c in built if c.baseline is not None), default=0.0)
            space = inner - sum(c.w + c.margin[1] + c.margin[3] for c in built) - gap * (len(built) - 1)
            x, between = left, gap
            if not grows and space > 0:
                if justify == 'center':
                    x += space / 2
                elif justify in ('flex-end', 'end'):
                    x += space
                elif justify == 'space-between' and len(built) > 1:
                    between = gap + space / (len(built) - 1)
                elif justify == 'space-around':
                    between = gap + space / len(built)
                    x += space / len(built) / 2
            for child in built:
                h = child.h + child.margin[0] + child.margin[2]
                dy = 0.0
                if align == 'center':
                    dy = (tall - h) / 2
                elif align in ('flex-end', 'end'):
                    dy = tall - h
                elif align == 'baseline' and child.baseline is not None:
                    dy = line_base - (child.baseline + child.margin[0])
                elif align == 'stretch' and child.node.tag != 'img' and child.style.get('height') in (None, 'auto'):
                    child.h = tall - child.margin[0] - child.margin[2]
                self.shift(child, x + child.margin[3], y + child.margin[0] + dy)
                box.children.append(child)
                x += child.w + child.margin[1] + child.margin[3] + between
            if box.baseline is None and built:
                box.baseline = y + line_base
            y += tall + row_gap
        return y - top - (row_gap if rows else 0)

    # ---- tables ------------------------------------------------------------------------------------
    def table(self, box: Box, node: Node, inner: float) -> float:
        page = self.page
        rows = []

        def gather(parent: Node):
            for kid in parent.children:
                if kid.tag == 'tr':
                    rows.append([cell for cell in kid.children if cell.tag in ('td', 'th')])
                elif kid.tag in ('thead', 'tbody', 'tfoot'):
                    gather(kid)

        gather(node)
        columns = max((len(row) for row in rows), default=0)
        if not columns:
            return 0.0
        collapse = node.style.get('border-collapse') == 'collapse'
        natural, least = [0.0] * columns, [0.0] * columns
        for row in rows:
            for index, cell in enumerate(row):
                em = float(cell.style['font-size'])
                frame = sum(page.length(cell.style.get(f'padding-{s}'), em, inner, 0.0) + page.length(cell.style.get(f'border-{s}-width'), em, inner, 0.0)
                            for s in ('left', 'right'))
                natural[index] = max(natural[index], self.natural_width(cell, 100000.0) )
                words = [page.text_width(word, cell.style) for word in re.findall(r'\S+', self.plain(cell))]
                least[index] = max(least[index], max(words, default=0.0) + frame)
        total = sum(natural)
        if total <= inner:
            spare = inner - total if page.length(node.style.get('width'), 16, inner) is not None else 0.0
            widths = [w + spare * (w / total if total else 1 / columns) for w in natural]
        else:
            flexible = sum(max(0.0, n - m) for n, m in zip(natural, least)) or 1.0
            over = total - inner
            widths = [max(m, n - over * max(0.0, n - m) / flexible) for n, m in zip(natural, least)]
        y = top = box.border[0] + box.pad[0]
        left = box.border[3] + box.pad[3]
        for row in rows:
            x, built = left, []
            for index, cell in enumerate(row):
                forced = dict(cell.style)
                forced['width'], forced['box-sizing'], forced['display'] = f'{widths[index] / page.scale}px', 'border-box', 'block'
                saved, cell.style = cell.style, forced
                child = self.build(cell, widths[index])
                cell.style = saved
                child.style = {**saved, 'display': 'block'}
                self.shift(child, x, y)
                built.append(child)
                x += widths[index] - (child.border[1] if collapse and index + 1 < len(row) else 0)
            tall = max((c.h for c in built), default=0.0)
            for child in built:
                child.h = tall
                box.children.append(child)
            row_node = row[0].parent if row else None
            if row_node is not None and row_node.style.get('background') and built:
                band = Box(row_node, {'background': row_node.style['background'], 'font-size': row_node.style['font-size'], 'display': 'block'})
                band.x, band.y, band.w, band.h = left, y, sum(widths), tall
                box.children.insert(len(box.children) - len(built), band)
            y += tall - (built[0].border[2] if collapse and built else 0)
        return y - top + (built[0].border[2] if collapse and rows and built else 0)

    def plain(self, node: Node) -> str:
        if node.tag == '#text':
            return self.transform(node.text, node.parent.style)
        return ' '.join(self.plain(kid) for kid in node.children)


# ---------------------------------------------------------------------------------------------------------
# Painting
# ---------------------------------------------------------------------------------------------------------
_corners: dict = {}


def _rounded_mask(width: int, height: int, radius: float) -> Image.Image:
    """A mask with rounded corners. Only the corners are drawn finely (four times larger, then reduced); the rest is
    filled in one go, so a very tall box costs no more than a small one."""
    mask = Image.new('L', (width, height), 255)
    r = int(math.ceil(min(radius, width / 2, height / 2)))
    if radius <= 0.5 or r < 1:
        return mask
    key = (r, round(radius * 4))
    corner = _corners.get(key)
    if corner is None:
        big = Image.new('L', (r * 4, r * 4), 0)
        ImageDraw.Draw(big).pieslice((0, 0, radius * 8 - 1, radius * 8 - 1), 180, 270, fill=255)
        corner = _corners[key] = big.resize((r, r), Image.LANCZOS)
    mask.paste(corner, (0, 0))
    mask.paste(corner.transpose(Image.FLIP_LEFT_RIGHT), (width - r, 0))
    mask.paste(corner.transpose(Image.FLIP_TOP_BOTTOM), (0, height - r))
    mask.paste(corner.transpose(Image.ROTATE_180), (width - r, height - r))
    return mask


def _stops(page: Page, parts: list[str], current):
    stops = []
    for part in parts:
        tokens = _split(part, ' ')
        position = None
        if len(tokens) > 1 and re.match(r'^[\d.]+%$', tokens[-1]):
            position = float(tokens[-1][:-1]) / 100
            tokens = tokens[:-1]
        stops.append([_color(' '.join(tokens), current), position])
    if stops:
        if stops[0][1] is None:
            stops[0][1] = 0.0
        if stops[-1][1] is None:
            stops[-1][1] = 1.0
        index = 0
        while index < len(stops):
            if stops[index][1] is None:
                end = index
                while stops[end][1] is None:
                    end += 1
                before = stops[index - 1][1]
                for k in range(index, end):
                    stops[k][1] = before + (stops[end][1] - before) * (k - index + 1) / (end - index + 1)
                index = end
            index += 1
    return stops


_ramps: dict = {}


def _ramp(stops) -> Image.Image:
    """256 x 1 RGBA strip of the gradient (alpha-aware: colours are blended premultiplied)."""
    key = tuple((tuple(colour), position) for colour, position in stops)
    if key in _ramps:
        return _ramps[key]
    strip = Image.new('RGBA', (256, 1))
    pixels = []
    for i in range(256):
        t = i / 255
        if t <= stops[0][1]:
            pixels.append(stops[0][0])
            continue
        if t >= stops[-1][1]:
            pixels.append(stops[-1][0])
            continue
        for (c0, p0), (c1, p1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                f = (t - p0) / (p1 - p0) if p1 > p0 else 0.0
                a0, a1 = c0[3] / 255, c1[3] / 255
                alpha = a0 + (a1 - a0) * f
                if alpha <= 0:
                    pixels.append((0, 0, 0, 0))
                else:
                    pixels.append(tuple(round((c0[k] * a0 * (1 - f) + c1[k] * a1 * f) / alpha) for k in range(3)) + (round(alpha * 255),))
                break
    strip.putdata(pixels)
    if len(_ramps) > 64:
        _ramps.clear()
    _ramps[key] = strip
    return strip


class Painter:
    def __init__(self, page: Page, width: int, height: int):
        self.page = page
        self.canvas = Image.new('RGBA', (width, max(1, height)), (0, 0, 0, 255))
        self.layers: dict = {}

    def gradient(self, value: str, width: int, height: int, current, box_h: float):
        """An RGBA picture of one background layer, or None. Hundreds of boxes on a page share the same one."""
        key = (value, width, height, current)
        if key not in self.layers:
            if len(self.layers) > 200:
                self.layers.clear()
            self.layers[key] = self.draw_gradient(value, width, height, current, box_h)
        layer = self.layers[key]
        return layer.copy() if layer is not None else None

    def draw_gradient(self, value: str, width: int, height: int, current, box_h: float):
        page = self.page
        value = value.strip()
        linear = re.match(r'linear-gradient\((.*)\)', value, re.S)
        radial = re.match(r'radial-gradient\((.*)\)', value, re.S)
        if linear:
            parts = _split(linear.group(1), ',')
            angle = 180.0
            if parts and re.match(r'^-?[\d.]+deg$', parts[0]):
                angle, parts = float(parts[0][:-3]), parts[1:]
            elif parts and parts[0].startswith('to '):
                angle = {'to right': 90.0, 'to left': 270.0, 'to bottom': 180.0, 'to top': 0.0}.get(parts[0], 180.0)
                parts = parts[1:]
            stops = _stops(page, parts, current)
            if len(stops) < 2:
                return None
            ramp = _ramp(stops)
            angle %= 360
            if abs(angle - 90) < 1:
                return ramp.resize((width, height), Image.BILINEAR)
            if abs(angle - 270) < 1:
                return ramp.transpose(Image.FLIP_LEFT_RIGHT).resize((width, height), Image.BILINEAR)
            vertical = ramp.rotate(-90, expand=True)                   # first colour on top
            if abs(angle) < 1:
                vertical = vertical.transpose(Image.FLIP_TOP_BOTTOM)
            return vertical.resize((width, height), Image.BILINEAR)
        if radial:
            parts = _split(radial.group(1), ',')
            rx, ry, cx, cy = width / 2, height / 2, width / 2, height / 2
            if parts and not re.match(r'^(#|rgb|[a-z]+$)', parts[0].strip().split(' ')[0]) or (parts and ' at ' in parts[0]):
                shape, _, where = parts[0].partition(' at ')
                sizes = [s for s in _split(shape.replace('circle', '').replace('ellipse', ''), ' ') if s]
                if len(sizes) >= 2:
                    rx = page.length(sizes[0], 16 * page.scale, width, rx)
                    ry = page.length(sizes[1], 16 * page.scale, height, ry)
                elif len(sizes) == 1:
                    rx = ry = page.length(sizes[0], 16 * page.scale, width, rx)
                spot = _split(where, ' ')
                if len(spot) >= 2:
                    cx = page.length(spot[0], 16 * page.scale, width, cx)
                    cy = page.length(spot[1], 16 * page.scale, height, cy)
                parts = parts[1:]
            stops = _stops(page, parts, current)
            if len(stops) < 2 or rx <= 0 or ry <= 0:
                return None
            ramp = _ramp(stops)
            # distance from the centre in units of the radii, drawn small and stretched
            small_w, small_h = max(2, min(width, 96)), max(2, min(height, 96))
            xs = Image.linear_gradient('L').rotate(90, expand=True).resize((small_w, small_h))     # 0..255 left to right
            ys = Image.linear_gradient('L').resize((small_w, small_h))                               # 0..255 top to bottom
            lut_x = [min(255, round(abs((i / 255 * width - cx) / rx) * 255)) for i in range(256)]
            lut_y = [min(255, round(abs((i / 255 * height - cy) / ry) * 255)) for i in range(256)]
            dx, dy = xs.point(lut_x), ys.point(lut_y)
            dist = ImageChops.add(ImageChops.multiply(dx, dx), ImageChops.multiply(dy, dy))          # d^2 scaled to 0..255, clipped
            root = dist.point([min(255, round(math.sqrt(i / 255) * 255)) for i in range(256)])
            colours = list(ramp.getdata())
            bands = [root.point([colours[i][k] for i in range(256)]) for k in range(4)]
            return Image.merge('RGBA', bands).resize((width, height), Image.BILINEAR)
        color = _color(value.split(' ')[0] if not value.startswith(('rgb', '#')) else value, None) if value else None
        if value.startswith('rgb'):
            color = _color(re.match(r'rgba?\([^)]*\)', value).group(0), None)
        if color is None or color[3] == 0:
            return None
        return Image.new('RGBA', (width, height), color)

    def background(self, target: Image.Image, box: Box, x: int, y: int, w: int, h: int) -> None:
        value = box.style.get('background')
        if not value or value in ('none', 'transparent') or w <= 0 or h <= 0:
            return
        page = self.page
        current = _color(box.style.get('color', ''), (0, 0, 0, 255))
        radius = page.length(box.style.get('border-radius', '0').split(' ')[0], float(box.style['font-size']), w, 0.0) or 0.0
        mask = _rounded_mask(w, h, radius) if radius > 0.5 else None
        for layer in reversed(_split(value, ',')):
            fixed = bool(re.search(r'\)\s+fixed\b|\sfixed$', layer))
            layer = re.sub(r'\s+(fixed|no-repeat|repeat|scroll)\b', '', layer)
            height = min(h, page.viewport_height) if fixed else h
            picture = self.gradient(layer, w, height, current, h)
            if picture is None:
                continue
            if mask is not None:
                alpha = ImageChops.multiply(picture.getchannel('A'), mask.crop((0, 0, w, height)))
                picture.putalpha(alpha)
            target.alpha_composite(picture, (x, y)) if self._inside(target, x, y, w, height) else self._composite_clipped(target, picture, x, y)

    @staticmethod
    def _inside(target, x, y, w, h) -> bool:
        return x >= 0 and y >= 0 and x + w <= target.width and y + h <= target.height

    @staticmethod
    def _composite_clipped(target, picture, x, y) -> None:
        left, top = max(0, x), max(0, y)
        right, bottom = min(target.width, x + picture.width), min(target.height, y + picture.height)
        if right > left and bottom > top:
            target.alpha_composite(picture.crop((left - x, top - y, right - x, bottom - y)), (left, top))

    def borders(self, target: Image.Image, box: Box, x: int, y: int, w: int, h: int) -> None:
        if not any(box.border) or w <= 0 or h <= 0:
            return
        page, style = self.page, box.style
        current = _color(style.get('color', ''), (0, 0, 0, 255))
        colors = [_color(page.resolve(style.get(f'border-{s}-color', 'currentcolor')), current) for s in _SIDES]
        widths = [int(round(b)) for b in box.border]
        radius = page.length(style.get('border-radius', '0').split(' ')[0], float(style['font-size']), w, 0.0) or 0.0
        if radius > 0.5 and len(set(widths)) == 1 and len(set(colors)) == 1 and widths[0] > 0:
            outer = _rounded_mask(w, h, radius)
            inner = Image.new('L', (w, h), 0)
            b = widths[0]
            if w > 2 * b and h > 2 * b:
                inner.paste(_rounded_mask(w - 2 * b, h - 2 * b, max(0.0, radius - b)), (b, b))
            ring = ImageChops.subtract(outer, inner)
            layer = Image.new('RGBA', (w, h), colors[0])
            layer.putalpha(ImageChops.multiply(ring, Image.new('L', (w, h), colors[0][3])))
            self._composite_clipped(target, layer, x, y)
            return
        rects = ((x, y, w, widths[0]), (x + w - widths[1], y, widths[1], h), (x, y + h - widths[2], w, widths[2]), (x, y, widths[3], h))
        for (rx, ry, rw, rh), color in zip(rects, colors):
            if rw > 0 and rh > 0 and color[3] > 0:
                self._composite_clipped(target, Image.new('RGBA', (rw, rh), color), rx, ry)

    def text(self, target: Image.Image, text: str, style: dict, x: float, baseline: float) -> None:
        page = self.page
        color = _color(style.get('color', ''), (0, 0, 0, 255))
        spacing = page.length(style.get('letter-spacing'), float(style['font-size']), 0, 0.0) or 0.0
        shadows = []
        if style.get('text-shadow') and style['text-shadow'] != 'none':
            for part in _split(style['text-shadow'], ','):
                tokens = _split(part, ' ')
                lengths = [t for t in tokens if re.match(r'^-?[\d.]+(px|em)?$', t)]
                tint = _color(' '.join(t for t in tokens if t not in lengths), color)
                numbers = [page.length(t, float(style['font-size']), 0, 0.0) for t in lengths] + [0.0, 0.0, 0.0]
                shadows.append((numbers[0], numbers[1], numbers[2], tint))
        pen = x
        runs = []
        for font, part in page.pieces(text, style):
            if spacing:
                for ch in part:
                    runs.append((font, ch, pen))
                    pen += font.getlength(ch) + spacing
            else:
                runs.append((font, part, pen))
                pen += font.getlength(part)
        if shadows:
            pad = int(max(s[2] for s in shadows) * 2 + max(abs(s[0]) + abs(s[1]) for s in shadows) + 4)
            size = float(style['font-size'])
            w, h = int(pen - x) + pad * 2, int(size * 1.6) + pad * 2
            mask = Image.new('L', (w, h), 0)
            pencil = ImageDraw.Draw(mask)
            for font, part, at in runs:
                pencil.text((at - x + pad, pad + size * 1.2), part, font=font, fill=255, anchor='ls')
            for dx, dy, blur, tint in reversed(shadows):
                shade = mask.filter(ImageFilter.GaussianBlur(blur / 2)) if blur > 0 else mask
                layer = Image.new('RGBA', (w, h), tint)
                layer.putalpha(ImageChops.multiply(shade, Image.new('L', (w, h), tint[3])))
                self._composite_clipped(target, layer, int(round(x - pad + dx)), int(round(baseline - pad - size * 1.2 + dy)))
        pencil = ImageDraw.Draw(target)
        for font, part, at in runs:
            pencil.text((at, baseline), part, font=font, fill=color, anchor='ls')
        if 'underline' in style.get('text-decoration', ''):
            thick = max(1, round(float(style['font-size']) / 14))
            pencil.rectangle((x, baseline + thick, pen, baseline + thick * 2 - 1), fill=color)

    def image(self, target: Image.Image, box: Box, x: int, y: int, w: int, h: int) -> None:
        picture = box.image
        if picture is None or w <= 0 or h <= 0:
            return
        style = box.style
        fit_w, fit_h = w, h
        if style.get('object-fit') == 'contain' and picture.width and picture.height:
            ratio = min(w / picture.width, h / picture.height)
            fit_w, fit_h = max(1, round(picture.width * ratio)), max(1, round(picture.height * ratio))
        how = Image.NEAREST if style.get('image-rendering') == 'pixelated' and fit_w >= picture.width else Image.LANCZOS
        scaled = picture.resize((fit_w, fit_h), how)
        px, py = x + (w - fit_w) // 2, y + (h - fit_h) // 2
        radius = self.page.length(style.get('border-radius', '0').split(' ')[0], 16, w, 0.0) or 0.0
        if radius > 0.5:
            scaled.putalpha(ImageChops.multiply(scaled.getchannel('A'), _rounded_mask(fit_w, fit_h, radius)))
        found = re.search(r'drop-shadow\(([^()]*(?:\([^()]*\))?[^()]*)\)', style.get('filter', ''))
        if found:
            tokens = _split(found.group(1), ' ')
            lengths = [t for t in tokens if re.match(r'^-?[\d.]+(px)?$', t)]
            tint = _color(' '.join(t for t in tokens if t not in lengths), (0, 0, 0, 255))
            numbers = [self.page.length(t, 16, 0, 0.0) for t in lengths] + [0.0, 0.0, 0.0]
            pad = int(numbers[2] * 2) + 2
            alpha = Image.new('L', (fit_w + pad * 2, fit_h + pad * 2), 0)
            alpha.paste(scaled.getchannel('A'), (pad, pad))
            if numbers[2] > 0:
                alpha = alpha.filter(ImageFilter.GaussianBlur(numbers[2] / 2))
            shade = Image.new('RGBA', alpha.size, tint)
            shade.putalpha(ImageChops.multiply(alpha, Image.new('L', alpha.size, tint[3])))
            self._composite_clipped(target, shade, int(px - pad + numbers[0]), int(py - pad + numbers[1]))
        self._composite_clipped(target, scaled, px, py)

    def paint(self, box: Box, target: Image.Image | None = None, ox: float = 0.0, oy: float = 0.0) -> None:
        target = target if target is not None else self.canvas
        x, y = int(round(box.x - ox)), int(round(box.y - oy))
        w, h = int(round(box.x + box.w - ox)) - x, int(round(box.y + box.h - oy)) - y
        if box.href and w > 0 and h > 0:
            self.page.links.append((int(round(box.x)), int(round(box.y)), w, h, urljoin(self.page.base, box.href)))
        if box.hidden:
            return
        radius = self.page.length(box.style.get('border-radius', '0').split(' ')[0], float(box.style['font-size']), w, 0.0) or 0.0
        if box.style.get('overflow') == 'hidden' and radius > 0.5 and w > 0 and h > 0 and box.node.tag != 'img':
            layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))                # drawn apart so the corners can be cut
            self.paint_inside(box, layer, box.x, box.y, 0, 0, w, h)
            layer.putalpha(ImageChops.multiply(layer.getchannel('A'), _rounded_mask(w, h, radius)))
            self._composite_clipped(target, layer, x, y)
            self.borders(target, box, x, y, w, h)
            return
        self.paint_inside(box, target, ox, oy, x, y, w, h)

    def paint_inside(self, box: Box, target, ox, oy, x, y, w, h) -> None:
        self.background(target, box, x, y, w, h)
        if box.node.tag == 'img':
            self.image(target, box, x + int(box.border[3] + box.pad[3]), y + int(box.border[0] + box.pad[0]),
                       w - int(box.border[1] + box.border[3] + box.pad[1] + box.pad[3]), h - int(box.border[0] + box.border[2] + box.pad[0] + box.pad[2]))
        self.borders(target, box, x, y, w, h)
        for child in box.children:
            self.paint(child, target, ox, oy)
        for line in box.lines or []:
            for item in line['items']:
                if item[0] == 'box':
                    self.paint(item[1], target, ox, oy)
                elif item[0] == 'text' and len(item) >= 6:
                    _, text, style, at, href, width = item
                    at += line['x']
                    if style.get('visibility') != 'hidden':
                        self.text(target, text, style, at - ox, line['y'] + line['ascent'] - oy)
                    if href:
                        self.page.links.append((int(at), int(line['y']), int(width) + 1, int(line['h']) + 1, urljoin(self.page.base, href)))
                elif item[0] == 'text':                                  # a list marker
                    self.text(target, item[1], item[2], line['x'] - ox, line['y'] + float(item[2]['font-size']) - oy)


def render(html: str, base_url: str, width: int, scale: float, fetch, viewport_height: int = 0):
    """Draw the page `width` pixels wide. Returns (RGB picture, links, title)."""
    page = Page(html, base_url, width, scale, fetch, viewport_height)
    page.style_tree(page.root)
    top = next((n for n in page.root.children if n.tag == 'html'), None)
    body = None
    for holder in ([top] if top else []) + [page.root]:
        body = next((n for n in holder.children if n.tag == 'body'), body)
    if body is None:                                         # a fragment: treat everything as the body
        body = Node('body', parent=page.root)
        body.children = [n for n in page.root.children if n.tag != 'html'] + (top.children if top else [])
        for child in body.children:
            child.parent = body
        page.style_tree(body, page.root.style)
    layout = Layout(page)
    box = layout.build(body, float(width))
    layout.shift(box, box.margin[3], box.margin[0])
    height = int(math.ceil(box.y + box.h + box.margin[2]))
    painter = Painter(page, width, max(height, page.viewport_height))
    backdrop = None
    for node in (top, body):                                 # the page colour: <html>'s, else <body>'s
        if node is not None and node.style.get('background') and backdrop is None:
            layers = _split(node.style['background'], ',')
            backdrop = _color(page.resolve(layers[-1]).split(' fixed')[0].strip(), None)
    painter.canvas.paste(backdrop or (255, 255, 255, 255), (0, 0, width, painter.canvas.height))
    full = Box(body, body.style)                             # the body's background covers the whole sheet
    full.w, full.h = float(width), float(painter.canvas.height)
    painter.background(painter.canvas, full, 0, 0, width, painter.canvas.height)
    body_style = dict(body.style)
    box.style = {k: v for k, v in body_style.items() if k != 'background'}
    painter.paint(box)
    title = re.search(r'<title>(.*?)</title>', html, re.S | re.I)
    return painter.canvas.convert('RGB'), page.links, (title.group(1).strip() if title else '')
