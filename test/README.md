# Test scripts

All scripts read `env.sh`. Set these first if the defaults do not fit:

    export DEV=/path/to/Exvius-revival-ios      (the server: a checkout of github.com/CreamySeaman6/Exvius-revival-ios)
    export DATA=/path/to/data                   (game data: the folder with native/ and static/)

A display is needed: start `Xvfb :99 -screen 0 1280x1024x24` as a background task. Output goes to `test/out/`.

Game data: `python3 app/launcher.py --headless --apk <apk>` with `EXVIUS_DESKTOP_HOME=<layout>` and
`EXVIUS_DESKTOP_DATA=$DATA` downloads it (14.6 GB) through the server's `fetch_data.py`.

| Script | What it does |
|---|---|
| `make_linux_layout.sh [apk]` | Package-shaped folder with the Linux runner in `build/linux-layout`; with an APK, also installs the game from it |
| `fresh_l.sh` | Make that layout a new install again |
| `play_l.sh <log> [--env N=V ...]` | Launcher without a window + Linux runner. Runs until the game closes: start it in the background |
| `ctl_l.sh <command>` | Game pixels (the picture is 640x960): `tap X Y`, `drag x1 y1 x2 y2`, `key back`, `quit`. Window pixels (640x997, the bar is the top 37): `click X Y`, `mdrag x1 y1 x2 y2`, `wheel X Y N`. Also `size 0\|1\|2`, `resize W H`, `fullscreen`, `picture`, `shot NAME` |
| `shot_l.sh <name>` | Screenshot to `test/out/control_l/<name>.png` |
| `seq_l.sh <name> "x y wait" ...` | Taps with waits, then one screenshot |
| `auto_l.sh <minutes>` | Taps through first run and the tutorial; a screenshot per round in `test/out/auto_l` |
| `story_l.sh <minutes>` | Keeps playing story quests (Auto battles, skips scenes, closes help pages); screenshots in `test/out/story_l` |
| `ab_l.sh <name> <cores>` | Fixed 3-minute scenario; totals frames, late frames, sound gaps from the runner's 5-second reports |
| `play_p.sh`, `ctl_p.sh`, `shot_p.sh`, `seq_p.sh`, `auto_p.sh <minutes>` | The same against the Windows package (`dist/ExviusRevivalDesktop`, from `OUT=dist/ExviusRevivalDesktop pack.sh`) under Wine |

Useful tap positions at 640x960: title "tap to start" 320 740; account linking "None" 320 435; "Yes" 440 612;
download prompt "OK" 320 630. `auto_p.sh` holds the spots that get through the tutorial battles.

Reading the 5-second report (`client/runner.log`, needs `FFBE_VERBOSE=1`):
`300 frames, 1 late, longest 26 ms; 0 sound gaps, least in hand 153 ms`. 300 frames is full speed. "Late" is a frame
shown more than 25 ms after the one before. A "sound gap" is a playing stream that ran dry and then carried on, which
is what is heard as a hiccup. "Least in hand" is the smallest amount of sound waiting to be played; about 150 ms is
healthy, near 0 means gaps are about to happen.

Useful spots in game pixels at 640x960, after the tutorial: Home tab 85 918, Summon tab 460 918, World 407 790, the
News board 70 643, a help page's close button 590 138, the in-game Back button 80 193 (80 70 on the summon screens),
first banner 320 420, Drop Rates 92 494. In window pixels: the bar's Back 17 17, speaker 315 17, camera 558 17, full
screen 590 17, gear 621 17.

`FFBE_PROFILE=1` (pass it with `--env`, or `EXTRA="--env FFBE_PROFILE=1"` for `ab_l.sh`) adds to each 5-second report
the functions the game's main thread was in while frames were overdue.

## Added for test5

- Background tasks end at their time limit and take a shared Xvfb with them. Give each run its own display: a small
  script that starts `Xvfb :99 ...`, runs `test/play_l.sh` (or `play_p.sh`) in it, and stops it afterwards.
- The start window and Settings on Linux, without Wine: `apt-get install -y python3-tk python3-pil.imagetk`, then
  `EXVIUS_DESKTOP_HOME=build/linux-layout EXVIUS_DESKTOP_DATA=<data> /usr/bin/python3.12 app/launcher.py` on a virtual
  display; pictures with `import -window root`, clicks with `xdotool`. `EXVIUS_DESKTOP_OPEN_SETTINGS=1`,
  `EXVIUS_DESKTOP_SETTINGS_PAGE=N` and `EXVIUS_DESKTOP_SCALE=1.5` (as on a display scaled to 150%) help.
- Control channel: `maximize`, `minimize` (the bar's window buttons), besides `click X Y` on the buttons themselves.
- Is the music playing: start with `FFBE_AUDIO_DUMP` (the play scripts do) and look at the loudness of the last
  seconds of `test/out/audio_l.raw` (16-bit stereo). Silence between tap sounds means the music has stopped.
- What the game calls: `--env TRACE_FUNCS=@<hex offset>,...` with offsets from `nm -D libgame.so`; functions of fewer
  than four instructions, or whose first four depend on their address, cannot be traced (the log says so).

## Added for 0.2.0

- `session_l.sh <display number> <log> [--env NAME=VALUE ...]`: one game run in a display of its own (Xvfb and
  openbox), stopped when the game closes. Start it as a background task; the control scripts work as before.
- `mouse.py`: real mouse events through xdotool (`move`, `click`, `wheel`, `drag`, `flick`, `passing`), in game
  pixels of a 640x960 picture. They go the whole way a player's mouse goes: SDL, the runner's finger, the game. The
  control channel's `tap` and `drag` skip the runner's mouse handling.
- `scroll.sh <runner.log>`: fourteen gestures on the list that is on the screen (wheel in the middle, on the list's
  scroll bar and at its edges, drags with and without a rest, flicks, presses made in passing, the scroll bar
  dragged by the button), each judged from the game's own account in the log. The game must run with
  `--env FFBE_TOUCH_LOG=1 --env FFBE_SCROLL_LOG=1`. A list already at its end reports "did not move": read the line.
- `engine.py <server checkout> <data> <a COPY of a save folder>`: Python from standard input with the server engine
  loaded as the launcher loads it. For looking things up without the game.
- `update_p.sh`: the updater under Wine. Needs `dist/ExviusRevivalDesktop` packed with a lower version
  (`VERSION=0.1.99 OUT=dist/ExviusRevivalDesktop sh pack.sh`) and a release folder (`sh release.sh 0.2.0`;
  `RELEASE=<folder>` for another). Five cases: `good`, `checksum`, `fail`, `stop`, `foreign`; name some to run only
  those. Each also checks that `client`, `save`, `data`, `pictures` and `settings.json` are untouched.
- `updater_kit.sh <release folder> [label]`: the package for trying the updater on a real PC: the program labelled
  0.1.99 with the release's `update.json` and update zip in `update-test\` and a `.cmd` that points the check there.
- The updater without the window: `python app/launcher.py --headless --check-update` and `--update`, with
  `EXVIUS_DESKTOP_UPDATE_URL=<folder or http address>`.
- Do not send control commands while `auto_l.sh` or `story_l.sh` run: both use the one `cmd` file. Look at their own
  pictures in `test/out/auto_l` and `test/out/story_l` instead.
