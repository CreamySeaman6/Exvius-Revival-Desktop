"""The Settings window of the start window.

The pages come from settings_spec.GROUPS. Everything on a page is drawn with Pillow (launcher_ui) at the display's
real pixel size: the lettering once per page as one picture, each control as a small picture of its own that is
redrawn when the pointer is on it or its value changes. Only the places where something is typed are real text
fields, sitting inside a drawn field.
"""
from __future__ import annotations

import os

from PIL import Image, ImageDraw, ImageTk

import launcher_art as art
import launcher_ui as ui
from launcher_ui import BG, BOLD, FIELD, GILT, GOLD, INK, LINE, MUTED, PANEL, PLAIN, TITLE, hexa

SIDE, FOOT, MARGIN, RAIL = 236, 56, 26, 16             # layout units; multiplied by the display's scale
OTHER = ('Other settings', 'The game server has more switches than the pages here show; server\\README.md in this folder lists all of '
         'them. Write any of them here, one NAME=VALUE per line. They win over the pages on the left.')
NOTE = 'Changes are saved as you make them and apply the next time you press Play.'


class SettingsWindow:
    """api: value(entry) -> str, set(entry, value), default(entry) -> str, extra_text() -> str, save_extra(text),
    start_over() -> error text or None.

    With the system's frame (`own_frame` false, `modal` true) this is an ordinary dialog: owned by the start window
    and holding the pointer. Without the system's frame it is neither: it is a plain window of its own, made exactly
    like the start window, so that whatever shows the one shows the other, and nothing can leave the program holding
    the pointer for a window that is not on the screen. The start window keeps it in front and does nothing else
    while it is open (`launcher.window`). If it cannot be made, or is not on the screen shortly after opening,
    `on_stuck(page)` is called so that it can be opened with the system's frame instead."""

    def __init__(self, tk, messagebox, root, scale: float, groups: list, api: dict, own_frame: bool, icon, first_page: int = 0,
                 modal: bool = True, on_stuck=None, log=None):
        self.tk, self.messagebox, self.s, self.groups, self.api, self.icon = tk, messagebox, scale, groups, api, icon
        self.on_stuck, self.log = on_stuck, log or (lambda _text: None)
        self.top = tk.Toplevel(root)
        try:
            self.assemble(root, own_frame, modal, first_page)
        except Exception:
            try:
                self.top.destroy()                 # never leave half a window behind
            except Exception:  # noqa: BLE001
                pass
            raise

    def assemble(self, root, own_frame: bool, modal: bool, first_page: int) -> None:
        tk, s, groups, top = self.tk, self.s, self.groups, self.top
        top.title('Settings - Exvius Revival Desktop')
        top.configure(bg=hexa(BG))
        modal = modal and not own_frame
        if own_frame and os.environ.get('EXVIUS_DESKTOP_TEST_STUCK') == '2':      # for tests: the frameless window fails to open
            raise RuntimeError('test: the frameless Settings window could not be made')
        if modal:
            top.transient(root)
        sw, sh = top.winfo_screenwidth(), top.winfo_screenheight()
        w, h = min(round(1000 * s), sw - 40), min(round(650 * s), sh - 80)
        x = max(0, min(root.winfo_x() + round(30 * s), sw - w))
        y = max(0, min(root.winfo_y() + round(30 * s), sh - h - 40))
        top.geometry(f'{w}x{h}+{x}+{y}')
        top.minsize(min(w, round(760 * s)), min(h, round(440 * s)))
        self.frame = ui.Frameless(top, own_frame, taskbar=False)
        self.own = own_frame
        self.corner = ui.Corner(s, ('max', 'close'))
        self.family = 'Segoe UI' if ui.WINDOWS else 'DejaVu Sans'
        self.page = 0
        self.controls: list = []
        self.widgets: list = []             # (widget, canvas item)
        self.photos: dict = {}
        self.listing = None                 # the open list of a choice
        self.hot = (None, None)             # (control, part) under the pointer
        self.side_hot = self.strip_hot = self.foot_hot = None
        self.thumb = None                   # (top, bottom) of the scroll thumb on the rail
        self.rail_grab = None
        self.pending = None

        def canvas(**options):
            return tk.Canvas(top, highlightthickness=0, bd=0, bg=hexa(BG), **options)
        top.grid_rowconfigure(1, weight=1)
        top.grid_columnconfigure(1, weight=1)
        self.strip = None
        if own_frame:
            self.strip = canvas(height=round(ui.STRIP * s))
            self.strip.grid(row=0, column=0, columnspan=3, sticky='ew')
            self.strip.bind('<Configure>', lambda _e: self.paint_strip())
            self.strip.bind('<Motion>', self.strip_move)
            self.strip.bind('<Leave>', lambda _e: self.strip_set(None))
            self.strip.bind('<ButtonPress-1>', self.strip_press)
            self.strip.bind('<B1-Motion>', self.frame.drag)
            self.strip.bind('<ButtonRelease-1>', self.strip_release)
            self.strip.bind('<Double-Button-1>', lambda e: self.corner.at(e.x, e.y) or self.maximize())
        self.side = canvas(width=round(SIDE * s))
        self.side.configure(bg=hexa(PANEL))
        self.side.grid(row=1, column=0, rowspan=2, sticky='ns')
        self.side.bind('<Configure>', lambda _e: self.paint_side())
        self.side.bind('<Motion>', lambda e: self.side_set(self.tab_at(e.y)))
        self.side.bind('<Leave>', lambda _e: self.side_set(None))
        self.side.bind('<Button-1>', lambda e: self.tab_at(e.y) is not None and self.build(self.tab_at(e.y)))
        self.view = canvas(yscrollincrement=round(22 * s))
        self.view.grid(row=1, column=1, sticky='nsew')
        self.view.configure(yscrollcommand=lambda *_a: self.paint_rail())
        self.view.bind('<Configure>', self.view_resized)
        self.view.bind('<Motion>', self.view_move)
        self.view.bind('<Leave>', lambda _e: self.set_hot(None, None))
        self.view.bind('<Button-1>', self.view_click)
        self.rail = canvas(width=round(RAIL * s))
        self.rail.grid(row=1, column=2, sticky='ns')
        self.rail.bind('<Configure>', lambda _e: self.paint_rail())
        self.rail.bind('<ButtonPress-1>', self.rail_press)
        self.rail.bind('<B1-Motion>', self.rail_drag)
        self.rail.bind('<ButtonRelease-1>', lambda _e: setattr(self, 'rail_grab', None))
        self.foot = canvas(height=round(FOOT * s))
        self.foot.grid(row=2, column=1, columnspan=2, sticky='ew')
        self.foot.bind('<Configure>', lambda _e: self.paint_foot())
        self.foot.bind('<Motion>', lambda e: self.foot_set(self.foot_at(e.x, e.y)))
        self.foot.bind('<Leave>', lambda _e: self.foot_set(None))
        self.foot.bind('<ButtonPress-1>', self.foot_press)
        self.foot.bind('<B1-Motion>', self.frame.drag)
        self.foot.bind('<ButtonRelease-1>', self.frame.release)
        for sequence in ('<MouseWheel>', '<Button-4>', '<Button-5>'):
            top.bind(sequence, self.wheel)
        top.bind('<Escape>', lambda _e: self.close_list() if self.listing else top.destroy())
        top.update_idletasks()
        self.build(max(0, min(first_page, len(groups))))
        if modal:
            top.grab_set()
            top.focus_force()
        else:
            top.after(1, self.come_forward)        # once this click is over and the window is on the screen
            top.after(700, self.check_shown)

    # ---- being on the screen ----------------------------------------------------------------------
    def alive(self) -> bool:
        try:
            return bool(self.top.winfo_exists())
        except Exception:  # noqa: BLE001
            return False

    def come_forward(self) -> None:
        """In front of the start window, with the keyboard. Never raises: a window that cannot take the keyboard is
        still a window that can be used."""
        if not self.alive():
            return
        for step in (self.top.deiconify, self.top.lift, self.top.focus_force):
            try:
                step()
            except Exception as exc:  # noqa: BLE001
                self.log(f'settings: {step.__name__} failed: {exc}')

    def check_shown(self) -> None:
        if not self.alive():
            return
        try:
            shown = bool(self.top.winfo_viewable())
        except Exception:  # noqa: BLE001
            shown = False
        if self.own and os.environ.get('EXVIUS_DESKTOP_TEST_STUCK') == '1':       # for tests: the frameless window never appears
            shown = False
        if shown:
            self.log('settings: on the screen')
            return
        self.log('settings: not on the screen 0.7 s after opening')
        page = self.page
        try:
            self.top.destroy()
        except Exception:  # noqa: BLE001
            pass
        if self.on_stuck:
            self.on_stuck(page)

    # ---- the strip along the top ------------------------------------------------------------------
    def paint_strip(self) -> None:
        width = self.strip.winfo_width()
        if width < 8:
            return
        base = self.photos.get('strip-base')
        if not base or base.width != width:
            base = self.photos['strip-base'] = ui.strip(width, self.s, 'Settings', self.icon)
        image = base.copy()
        self.corner.place(width - round(8 * self.s), (image.height - 2 - self.corner.px) // 2)
        self.corner.paint(image, self.strip_hot, self.frame.maximized)
        self.photos['strip'] = ImageTk.PhotoImage(image)
        self.strip.delete('all')
        self.strip.create_image(0, 0, anchor='nw', image=self.photos['strip'])

    def strip_set(self, kind) -> None:
        if kind != self.strip_hot:
            self.strip_hot = kind
            self.strip.configure(cursor='hand2' if kind else '')
            self.paint_strip()

    def strip_move(self, event) -> None:
        self.strip_set(self.corner.at(event.x, event.y))

    def strip_press(self, event) -> None:
        self.close_list()
        self.pressed = self.corner.at(event.x, event.y)
        if not self.pressed:
            self.frame.press(event)

    def strip_release(self, event) -> None:
        self.frame.release()
        kind, self.pressed = getattr(self, 'pressed', None), None
        if kind and self.corner.at(event.x, event.y) == kind:
            if kind == 'close':
                self.top.destroy()
            else:
                self.maximize()

    def maximize(self) -> None:
        self.frame.toggle_maximize()
        self.strip_hot = None

    # ---- the list of pages on the left ------------------------------------------------------------
    def names(self) -> list:
        return [name for name, _, _ in self.groups] + [OTHER[0]]

    def tab_at(self, y: float):
        row = int((y - round(14 * self.s)) // round(36 * self.s))
        return row if 0 <= row < len(self.names()) and y >= round(14 * self.s) else None

    def side_set(self, row) -> None:
        if row != self.side_hot:
            self.side_hot = row
            self.side.configure(cursor='hand2' if row is not None else '')
            self.paint_side()

    def paint_side(self) -> None:
        s = self.s
        w, h = round(SIDE * s), max(8, self.side.winfo_height())
        image = Image.new('RGBA', (w, h), PANEL + (255,))
        pencil = ImageDraw.Draw(image)
        rh, y = round(36 * s), round(14 * s)
        for i, name in enumerate(self.names()):
            chosen = i == self.page
            if chosen:
                lit = art._gradient(w, rh, [(0, (44, 62, 160)), (1, (28, 40, 120))])
                fade = Image.new('L', (256, 1))
                fade.putdata([round(255 * (1 - i2 / 255 * 0.85)) for i2 in range(256)])
                image.paste(lit, (0, y), fade.resize((w, rh)))
                pencil.rectangle((0, y, max(2, round(3 * s)) - 1, y + rh - 1), fill=GOLD + (255,))
            elif i == self.side_hot:
                pencil.rectangle((0, y, w, y + rh - 1), fill=(24, 36, 100, 255))
            ui.put_text(image, round(20 * s), y + rh / 2, name, BOLD if chosen else PLAIN, 13.5 * s, GILT if chosen else INK, middle=True)
            y += rh
        pencil.rectangle((w - 1, 0, w - 1, h), fill=(30, 42, 110, 255))
        self.photos['side'] = ImageTk.PhotoImage(image)
        self.side.delete('all')
        self.side.create_image(0, 0, anchor='nw', image=self.photos['side'])

    # ---- the foot: the note, Close, the corner to resize by ---------------------------------------
    def foot_rects(self) -> dict:
        s = self.s
        w, h = self.foot.winfo_width(), self.foot.winfo_height()
        bw, bh = ui.pill_size('Close', s)
        bw += round(30 * s)
        x1 = w - round(24 * s)
        rects = {'close': (x1 - bw, (h - bh) // 2, x1, (h - bh) // 2 + bh)}
        if self.own and not self.frame.maximized:
            g = round(16 * s)
            rects['grip'] = (w - g, h - g, w, h)
        return rects

    def foot_at(self, x: float, y: float):
        for name, (x0, y0, x1, y1) in self.foot_rects().items():
            if x0 <= x < x1 and y0 <= y < y1:
                return name
        return None

    def foot_set(self, name) -> None:
        if name != self.foot_hot:
            self.foot_hot = name
            self.foot.configure(cursor='hand2' if name == 'close' else 'sizing' if name == 'grip' and ui.WINDOWS else 'bottom_right_corner' if name == 'grip' else '')
            self.paint_foot()

    def foot_press(self, event) -> None:
        self.close_list()
        name = self.foot_at(event.x, event.y)
        if name == 'close':
            self.top.destroy()
        elif name == 'grip':
            self.frame.press(event, resize=True)

    def paint_foot(self) -> None:
        s = self.s
        w, h = self.foot.winfo_width(), self.foot.winfo_height()
        if w < 8:
            return
        image = Image.new('RGBA', (w, h), BG + (255,))
        ImageDraw.Draw(image).rectangle((round(MARGIN * s), 0, w - round(24 * s), 0), fill=(26, 36, 96, 255))
        rects = self.foot_rects()
        x0, y0, x1, y1 = rects['close']
        room = x0 - round(MARGIN * s) - round(16 * s)
        ui.put_text(image, round(MARGIN * s), h / 2, ui.fit_text(NOTE, PLAIN, 12.5 * s, room), PLAIN, 12.5 * s, MUTED, middle=True)
        top, bottom, rim = ui.GOLD_HOT if self.foot_hot == 'close' else ui.GOLD_FILL
        button = ui.panel(x1 - x0, y1 - y0, (y1 - y0) * 0.30, top, bottom, rim, max(1.0, 1.2 * s), 0.30).copy()
        ui.put_text(button, button.width / 2, button.height / 2, 'Close', BOLD, 13.5 * s, (26, 16, 48), anchor='c', middle=True)
        image.alpha_composite(button, (x0, y0))
        if 'grip' in rects:
            g = rects['grip']
            mark = ui.glyph('grip', g[2] - g[0], GOLD if self.foot_hot == 'grip' else (86, 108, 190))
            image.alpha_composite(mark, (g[0] - round(2 * s), g[1] - round(2 * s)))
        self.photos['foot'] = ImageTk.PhotoImage(image)
        self.foot.delete('all')
        self.foot.create_image(0, 0, anchor='nw', image=self.photos['foot'])

    # ---- the rail: where the page is scrolled to --------------------------------------------------
    def paint_rail(self) -> None:
        s = self.s
        w, h = round(RAIL * s), self.rail.winfo_height()
        if h < 8:
            return
        first, last = self.view.yview()
        self.rail.delete('all')
        self.thumb = None
        if last - first >= 0.999:
            return
        pad = round(6 * s)
        y0, y1 = pad + (h - 2 * pad) * first, pad + (h - 2 * pad) * last
        tw, th = max(4, round(6 * s)), max(round(24 * s), round(y1 - y0))
        y0 = min(y0, h - pad - th)
        grabbed = self.rail_grab is not None
        picture = ui.panel(tw, th, tw / 2, GOLD if grabbed else (86, 108, 190), GOLD if grabbed else (60, 80, 160), GILT if grabbed else (120, 146, 226), 1.0, 0)
        self.photos['thumb'] = ImageTk.PhotoImage(picture)
        self.rail.create_image((w - tw) // 2, round(y0), anchor='nw', image=self.photos['thumb'])
        self.thumb = (round(y0), round(y0) + th)

    def rail_press(self, event) -> None:
        self.close_list()
        if not self.thumb:
            return
        if self.thumb[0] <= event.y < self.thumb[1]:
            self.rail_grab = event.y - self.thumb[0]
            self.paint_rail()
        else:
            self.view.yview_scroll(-1 if event.y < self.thumb[0] else 1, 'pages')

    def rail_drag(self, event) -> None:
        if self.rail_grab is None:
            return
        pad, h = round(6 * self.s), self.rail.winfo_height()
        self.view.yview_moveto((event.y - self.rail_grab - pad) / max(1, h - 2 * pad))

    def wheel(self, event):
        step = -1 if (getattr(event, 'delta', 0) > 0 or getattr(event, 'num', 0) == 4) else 1
        if self.listing and len(self.listing['c']['labels']) > self.listing['visible']:
            lst = self.listing
            lst['first'] = max(0, min(len(lst['c']['labels']) - lst['visible'], lst['first'] + step * 2))
            self.paint_list()
            return 'break'
        self.close_list()
        self.view.yview_scroll(step * 3, 'units')
        return 'break'

    # ---- one page ---------------------------------------------------------------------------------
    def view_resized(self, _event) -> None:
        if self.photos.get('page-width') == self.view.winfo_width():
            self.paint_rail()
            return
        if self.pending:
            self.top.after_cancel(self.pending)
        self.pending = self.top.after(50, lambda: self.build(self.page, keep=True))

    def control_size(self, entry: dict) -> tuple:
        s = self.s
        h = round(30 * s)
        if entry['kind'] == 'toggle':
            return ui.switch(False, False, s).size
        if entry['kind'] == 'choice':
            widest = max(ui.text_width(label, PLAIN, 13.5 * s) for _, label in entry['choices'])
            return (round(min(300 * s, max(120 * s, widest + 52 * s))), h)
        if entry['kind'] == 'number':
            return (round((28 + 6 + 84 + 6 + 28) * s), h)
        return (round(230 * s), h)

    def build(self, index: int, keep: bool = False) -> None:
        tk, s, view = self.tk, self.s, self.view
        self.pending = None
        at = view.yview()[0] if keep and index == self.page else 0.0
        self.listing = None
        for widget, _ in self.widgets:
            widget.destroy()
        self.widgets, self.controls, self.hot = [], [], (None, None)
        view.delete('all')
        self.page = index
        width = max(view.winfo_width(), round(320 * s))
        left, right = round(MARGIN * s), width - round(10 * s)
        other = index >= len(self.groups)
        name, about, entries = (OTHER[0], OTHER[1], []) if other else self.groups[index]
        sizes = [self.control_size(entry) for entry in entries]
        words = []                          # (x, y, text, face, size, colour)
        rules = []
        y = round(20 * s)
        words.append((left, y, name, TITLE, 22 * s, INK))
        y += round(38 * s)
        for part in art.wrap(about, PLAIN, 13.5 * s, right - left):
            words.append((left, y, part, PLAIN, 13.5 * s, MUTED))
            y += round(20 * s)
        y += round(10 * s)
        for entry, (cw, ch) in zip(entries, sizes):
            rules.append(y)
            y += round(11 * s)
            y0 = y
            text_w = max(round(220 * s), right - left - cw - round(34 * s))
            for part in art.wrap(entry['label'], BOLD, 14 * s, text_w):
                words.append((left, y, part, BOLD, 14 * s, INK))
                y += round(21 * s)
            for part in (art.wrap(entry['help'], PLAIN, 12.5 * s, text_w) if entry['help'] else []):
                words.append((left, y, part, PLAIN, 12.5 * s, MUTED))
                y += round(18 * s)
            row = max(y - y0, ch)
            self.add_control(entry, right - cw, y0 + (row - ch) // 2, cw, ch)
            y = y0 + row + round(11 * s)
        box = None
        if other:
            box = tk.Text(view, bg=hexa(FIELD), fg='#ffffff', insertbackground='#ffffff', font=('Consolas' if ui.WINDOWS else 'DejaVu Sans Mono', -round(14 * s)),
                          relief='flat', bd=0, highlightthickness=max(1, round(s)), highlightbackground=hexa((74, 96, 180)), highlightcolor=hexa(GOLD),
                          padx=round(10 * s), pady=round(10 * s), selectbackground=hexa(LINE), selectforeground='#ffffff')
            box.insert('1.0', self.api['extra_text']())
            box.bind('<KeyRelease>', lambda _e: self.api['save_extra'](box.get('1.0', 'end')))
            height = max(round(160 * s), view.winfo_height() - y - round(24 * s))
            item = view.create_window(left, y, anchor='nw', window=box, width=right - left, height=height)
            self.widgets.append((box, item))
            y += height
        else:
            rules.append(y)
            y += round(18 * s)
            x = left
            actions = [('Put this page back to its defaults', self.defaults)]
            if name == 'History replay':
                actions.append(('Start the history over', self.start_over))
            for label, command in actions:
                w, h = ui.pill_size(label, s)
                self.controls.append({'kind': 'button', 'x': x, 'y': y, 'w': w, 'h': h, 'label': label, 'command': command, 'parts': {'go': (0, 0, w, h)}})
                x += w + round(12 * s)
            y += round(32 * s)
        total = y + round(22 * s)
        image = Image.new('RGBA', (width, total), BG + (255,))
        pencil = ImageDraw.Draw(image)
        for rule in rules:
            pencil.rectangle((left, rule, right, rule), fill=(26, 36, 96, 255))
        for x, ty, text, face, size, colour in words:
            ui.put_text(image, x, ty, text, face, size, colour)
        self.photos['page'] = ImageTk.PhotoImage(image.convert('RGB'))
        self.photos['page-width'] = view.winfo_width()
        view.tag_lower(view.create_image(0, 0, anchor='nw', image=self.photos['page']))
        for control in self.controls:
            self.paint(control)
        view.configure(scrollregion=(0, 0, width, total))
        view.yview_moveto(at)
        self.paint_side()
        self.paint_rail()

    def add_control(self, entry: dict, x: int, y: int, w: int, h: int) -> None:
        tk, s, view = self.tk, self.s, self.view
        control = {'kind': entry['kind'], 'entry': entry, 'x': x, 'y': y, 'w': w, 'h': h, 'parts': {}}
        now = self.api['value'](entry)
        typed = dict(bd=0, relief='flat', highlightthickness=0, bg=hexa(FIELD), fg='#ffffff', insertbackground='#ffffff',
                     disabledbackground=hexa(FIELD), selectbackground=hexa(LINE), selectforeground='#ffffff', font=(self.family, -round(14 * s)))
        if entry['kind'] == 'toggle':
            control['parts'] = {'flip': (0, 0, w, h)}
        elif entry['kind'] == 'choice':
            control['values'] = [value for value, _ in entry['choices']]
            control['labels'] = [label for _, label in entry['choices']]
            if now not in control['values']:
                control['values'].append(now)
                control['labels'].append(now)
            control['parts'] = {'list': (0, 0, w, h)}
        elif entry['kind'] == 'number':
            b, g = round(28 * s), round(6 * s)
            control['parts'] = {'minus': (0, (h - b) // 2, b, (h - b) // 2 + b), 'plus': (w - b, (h - b) // 2, w, (h - b) // 2 + b)}
            control['field'] = (b + g, 0, w - b - g, h)
            variable = control['variable'] = tk.StringVar(value=now)

            def changed(*_a, entry=entry, variable=variable):
                text = variable.get().strip()
                if text.isdigit() and entry['low'] <= int(text) <= entry['high']:
                    self.api['set'](entry, str(int(text)))
            variable.trace_add('write', changed)
            box = tk.Entry(view, textvariable=variable, justify='center', **typed)
            item = view.create_window(x + w // 2, y + h // 2, window=box, width=w - 2 * (b + g) - round(14 * s), height=h - round(8 * s))
            self.widgets.append((box, item))
        else:
            control['field'] = (0, 0, w, h)
            variable = tk.StringVar(value=now)
            variable.trace_add('write', lambda *_a, entry=entry, variable=variable: self.api['set'](entry, variable.get().strip()))
            box = tk.Entry(view, textvariable=variable, **typed)
            item = view.create_window(x + w // 2, y + h // 2, window=box, width=w - round(20 * s), height=h - round(8 * s))
            self.widgets.append((box, item))
        self.controls.append(control)

    def paint(self, control: dict) -> None:
        s, kind = self.s, control['kind']
        hot = self.hot[1] if self.hot[0] is control else None
        w, h = control['w'], control['h']
        if kind == 'toggle':
            entry = control['entry']
            image = ui.switch(self.api['value'](entry) == entry['on'], bool(hot), s)
        elif kind == 'choice':
            now = self.api['value'](control['entry'])
            label = control['labels'][control['values'].index(now)] if now in control['values'] else now
            image = ui.field(w, h, s, bool(hot) or (self.listing is not None and self.listing['c'] is control), label, True)
        elif kind == 'button':
            image = ui.pill_button(control['label'], s, False, bool(hot))
        else:
            image = Image.new('RGBA', (w, h), (0, 0, 0, 0))
            x0, y0, x1, y1 = control['field']
            image.alpha_composite(ui.field(x1 - x0, y1 - y0, s, False), (x0, y0))
            for part, (px0, py0, px1, _) in control['parts'].items():
                image.alpha_composite(ui.square_button(part, px1 - px0, hot == part), (px0, py0))
        control['photo'] = ImageTk.PhotoImage(image)
        if 'item' in control:
            self.view.itemconfigure(control['item'], image=control['photo'])
        else:
            control['item'] = self.view.create_image(control['x'], control['y'], anchor='nw', image=control['photo'])

    def locate(self, event) -> tuple:
        x, y = self.view.canvasx(event.x), self.view.canvasy(event.y)
        for control in self.controls:
            if control['x'] <= x < control['x'] + control['w'] and control['y'] <= y < control['y'] + control['h']:
                for part, (x0, y0, x1, y1) in control['parts'].items():
                    if x0 <= x - control['x'] < x1 and y0 <= y - control['y'] < y1:
                        return (control, part)
        return (None, None)

    def set_hot(self, control, part) -> None:
        if (control, part) == self.hot:
            return
        before, self.hot = self.hot[0], (control, part)
        for changed in {id(c): c for c in (before, control) if c is not None}.values():
            self.paint(changed)
        self.view.configure(cursor='hand2' if control is not None else '')

    def view_move(self, event) -> None:
        if self.listing:
            row = self.list_row(event)
            if row != self.listing['hot']:
                self.listing['hot'] = row
                self.paint_list()
            self.view.configure(cursor='hand2' if row is not None else '')
            return
        self.set_hot(*self.locate(event))

    def view_click(self, event) -> None:
        self.view.focus_set()
        if self.listing:
            row, control = self.list_row(event), self.listing['c']
            self.close_list()
            if row is not None:
                self.api['set'](control['entry'], control['values'][row])
                self.paint(control)
            return
        control, part = self.locate(event)
        if control is None:
            return
        entry = control.get('entry')
        if part == 'flip':
            self.api['set'](entry, entry['off'] if self.api['value'](entry) == entry['on'] else entry['on'])
            self.paint(control)
        elif part == 'list':
            self.open_list(control)
        elif part in ('minus', 'plus'):
            text = control['variable'].get().strip()
            now = int(text) if text.isdigit() else int(self.api['value'](entry) or 0)
            span = entry['high'] - entry['low']
            step = 1 if span <= 200 else 5 if span <= 1000 else 10 if span <= 5000 else 100 if span <= 100000 else 1000
            now = (now // step) * step + (step if part == 'plus' else (-step if now % step == 0 else 0))
            control['variable'].set(str(max(entry['low'], min(entry['high'], now))))
        elif part == 'go':
            control['command']()

    def defaults(self) -> None:
        for entry in self.groups[self.page][2]:
            self.api['set'](entry, self.api['default'](entry))
        self.build(self.page, keep=True)

    def start_over(self) -> None:
        if self.messagebox.askyesno('Start the history over', 'The replay will begin again at "History starts at" the next time you play. '
                                    'Your units, items and progress are kept. Start over?', parent=self.top):
            problem = self.api['start_over']()
            if problem:
                self.messagebox.showerror('Start the history over', problem, parent=self.top)

    # ---- the list a choice opens ------------------------------------------------------------------
    def open_list(self, control: dict) -> None:
        s, view = self.s, self.view
        rows, rh = len(control['labels']), round(30 * s)
        visible = min(rows, 8)
        pad = round(5 * s)
        h = visible * rh + 2 * pad
        top, bottom = view.canvasy(0), view.canvasy(0) + view.winfo_height()
        y = control['y'] + control['h'] + round(4 * s)
        if y + h > bottom and control['y'] - h - round(4 * s) >= top:
            y = control['y'] - h - round(4 * s)
        now = self.api['value'](control['entry'])
        current = control['values'].index(now) if now in control['values'] else 0
        self.listing = {'c': control, 'x': control['x'], 'y': y, 'w': control['w'], 'h': h, 'rh': rh, 'pad': pad, 'visible': visible,
                        'first': max(0, min(rows - visible, current - visible // 2)), 'hot': None, 'current': current, 'item': None, 'hidden': []}
        for _, item in self.widgets:                               # typed fields would show through the list
            box = view.bbox(item)
            if box and box[2] > control['x'] and box[0] < control['x'] + control['w'] and box[3] > y and box[1] < y + h:
                view.itemconfigure(item, state='hidden')
                self.listing['hidden'].append(item)
        self.set_hot(None, None)
        self.paint(control)
        self.paint_list()

    def list_row(self, event):
        lst = self.listing
        x, y = self.view.canvasx(event.x), self.view.canvasy(event.y)
        if not (lst['x'] <= x < lst['x'] + lst['w'] and lst['y'] + lst['pad'] <= y < lst['y'] + lst['h'] - lst['pad']):
            return None
        return lst['first'] + int((y - lst['y'] - lst['pad']) // lst['rh'])

    def paint_list(self) -> None:
        s, lst = self.s, self.listing
        control, w, h, rh, pad = lst['c'], lst['w'], lst['h'], lst['rh'], lst['pad']
        image = ui.panel(w, h, 9 * s, (22, 34, 98), (9, 15, 52), (185, 212, 255), max(1.0, 1.2 * s), 0.05).copy()
        rows = len(control['labels'])
        more = rows > lst['visible']
        for i in range(lst['visible']):
            row, y = lst['first'] + i, pad + i * rh
            if row == lst['hot']:
                image.alpha_composite(ui.panel(w - 2 * pad, rh, 6 * s, (58, 80, 176), (40, 58, 150), (58, 80, 176), 0, 0), (pad, y))
            chosen = row == lst['current']
            if chosen:
                d = max(4, round(6 * s))
                image.alpha_composite(ui.panel(d, d, d / 2, GOLD, GOLD, GOLD, 0, 0), (pad + round(7 * s), y + (rh - d) // 2))
            room = w - 2 * pad - round(34 * s)
            ui.put_text(image, pad + round(20 * s), y + rh / 2, ui.fit_text(control['labels'][row], PLAIN, 13.5 * s, room), PLAIN, 13.5 * s,
                        GILT if chosen or row == lst['hot'] else INK, middle=True)
        if more:
            track = h - 2 * pad
            th = max(round(16 * s), round(track * lst['visible'] / rows))
            ty = pad + round((track - th) * lst['first'] / max(1, rows - lst['visible']))
            tw = max(3, round(4 * s))
            image.alpha_composite(ui.panel(tw, th, tw / 2, (120, 146, 226), (120, 146, 226), (120, 146, 226), 0, 0), (w - pad - tw + round(1 * s), ty))
        lst['photo'] = ImageTk.PhotoImage(image)
        if lst['item'] is None:
            lst['item'] = self.view.create_image(lst['x'], lst['y'], anchor='nw', image=lst['photo'])
        else:
            self.view.itemconfigure(lst['item'], image=lst['photo'])
        self.view.tag_raise(lst['item'])

    def close_list(self) -> None:
        lst, self.listing = self.listing, None
        if not lst:
            return
        if lst['item'] is not None:
            self.view.delete(lst['item'])
        for item in lst['hidden']:
            self.view.itemconfigure(item, state='normal')
        self.paint(lst['c'])
