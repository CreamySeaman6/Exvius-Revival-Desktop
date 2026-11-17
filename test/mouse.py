#!/usr/bin/env python3
"""mouse.py <command> ... : real mouse events (X11, through xdotool) on the game window in $DISPLAY.

Unlike the control channel's `tap` and `drag`, which hand the game ready-made touches, these go the whole way a
player's mouse goes: the X server, SDL, the runner's mouse handling. Positions are window pixels (the bar is the
top 37 of a 640x997 window). Works on the Linux game and on the Windows game under Wine.

    move X Y                          put the pointer there
    click X Y                         a click at rest
    wheel N [gap ms]                  N notches at the pointer; negative N turns down (the list goes on down)
    drag X1 Y1 X2 Y2 [ms] [steps] [rest ms]   press, slide, rest (250 ms), let go
    flick X1 Y1 X2 Y2 [ms]            the same without the rest: let go while moving
    pass X0 Y0 X1 Y1 X2 Y2 [ms]       the pointer travels X0,Y0 -> X1,Y1, the button goes down without a stop,
                                      and the slide goes on to X2,Y2: a press "made in passing"
"""
import os
import subprocess
import sys
import time


def x(*words):
    subprocess.run(['xdotool', *map(str, words)], check=True)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    found = subprocess.run(['xdotool', 'search', '--name', os.environ.get('GAME_WINDOW', 'FINAL FANTASY')], capture_output=True, text=True).stdout.split()
    if not found:
        print('no game window in this display')
        return 1
    place = dict(line.split('=') for line in subprocess.run(['xdotool', 'getwindowgeometry', '--shell', found[0]], capture_output=True, text=True).stdout.split())
    ox, oy = int(place['X']), int(place['Y'])

    def to(px, py):
        x('mousemove', ox + round(px), oy + round(py))

    def glide(x1, y1, x2, y2, ms, steps):
        for i in range(1, steps + 1):
            to(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps)
            time.sleep(ms / 1000 / steps)

    command, n = sys.argv[1], [float(v) for v in sys.argv[2:]]
    if command == 'move':
        to(n[0], n[1])
    elif command == 'click':
        to(n[0], n[1]); time.sleep(0.2); x('mousedown', 1); time.sleep(0.08); x('mouseup', 1)
    elif command == 'wheel':
        for _ in range(int(abs(n[0]))):
            x('click', 5 if n[0] < 0 else 4)
            time.sleep((n[1] if len(n) > 1 else 60) / 1000)
    elif command in ('drag', 'flick'):
        ms = n[4] if len(n) > 4 else 400
        steps = int(n[5]) if len(n) > 5 else max(4, int(ms / 12))
        rest = (n[6] if len(n) > 6 else 250) if command == 'drag' else 0
        to(n[0], n[1]); time.sleep(0.4); x('mousedown', 1); time.sleep(0.05)
        glide(n[0], n[1], n[2], n[3], ms, steps); time.sleep(rest / 1000); x('mouseup', 1)
    elif command == 'pass':
        ms = n[6] if len(n) > 6 else 400
        to(n[0], n[1]); time.sleep(0.3)
        glide(n[0], n[1], n[2], n[3], 120, 10); x('mousedown', 1)
        glide(n[2], n[3], n[4], n[5], ms, max(4, int(ms / 12))); time.sleep(0.25); x('mouseup', 1)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
