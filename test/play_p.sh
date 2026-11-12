#!/bin/sh
# play_p.sh <log name> [--env NAME=VALUE ...] : the Windows package under Wine, launcher without its window.
# Needs Xvfb on $DISPLAY. Windows Python cannot write straight to a redirected file under Wine, hence "| cat".
. "$(dirname "$0")/env.sh"
log=$1; shift
rm -f "$OUT/control_p/cmd"
cd "$PACKAGE"
WINEDEBUG=-all SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} EXVIUS_DESKTOP_DATA="Z:$DATA" wine python/python.exe app/launcher.py --headless --play \
  --env "FFBE_CONTROL=Z:$OUT/control_p" --env FFBE_VERBOSE=1 --env VERBOSE=1 --env FFBE_QUIET_RESTART=1 --env FFBE_WINDOW=640x960 \
  --env "FFBE_AUDIO_DUMP=Z:$OUT/audio_p.raw" "$@" < /dev/null 2>&1 | cat > "$OUT/$log"
