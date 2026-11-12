#!/bin/sh
# ctl_p.sh <command...> : send one command to the running game (tap X Y, drag x1 y1 x2 y2, key back, shot NAME, quit)
# and wait until it has been picked up
. "$(dirname "$0")/env.sh"
C="$OUT/control_p"
echo "$*" > "$C/cmd.tmp" && mv "$C/cmd.tmp" "$C/cmd"
for i in $(seq 1 100); do [ -e "$C/cmd" ] || break; sleep 0.1; done
