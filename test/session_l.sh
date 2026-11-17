#!/bin/sh
# session_l.sh <display number> <log name> [--env NAME=VALUE ...] : one run of the Linux game in a display of its
# own (Xvfb plus the openbox window manager), stopped again when the game closes. Start it as a background task.
# The other scripts (ctl_l.sh, shot_l.sh ...) find the game through the control folder, not the display.
n=$1; shift
Xvfb ":$n" -screen 0 "${SCREEN:-1280x1100x24}" -nolisten tcp > /dev/null 2>&1 &
xvfb=$!
sleep 1
DISPLAY=":$n" openbox > /dev/null 2>&1 &
wm=$!
sleep 0.5
DISPLAY=":$n" sh "$(dirname "$0")/play_l.sh" "$@"
code=$?
kill "$wm" "$xvfb" 2> /dev/null
exit $code
