#!/bin/sh
# play_l.sh <log name> [--env NAME=VALUE ...] : the launcher (no window) with the Linux runner in the layout folder.
# Needs Xvfb on $DISPLAY. Runs until the game closes; start it as a background task.
. "$(dirname "$0")/env.sh"
log=$1; shift
rm -f "$OUT/control_l/cmd"
cd "$ROOT"
SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} EXVIUS_DESKTOP_HOME="$LAYOUT" EXVIUS_DESKTOP_DATA="$DATA" exec python3 app/launcher.py --headless --play \
  --env "FFBE_CONTROL=$OUT/control_l" --env FFBE_VERBOSE=1 --env VERBOSE=1 --env FFBE_QUIET_RESTART=1 --env FFBE_WINDOW=640x960 \
  --env "FFBE_AUDIO_DUMP=$OUT/audio_l.raw" "$@" > "$OUT/$log" 2>&1
