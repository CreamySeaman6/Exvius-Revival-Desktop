#!/bin/sh
# auto_p.sh <minutes> : tap the usual first-run and tutorial spots in the Wine game (640x960 window) and keep a
# screenshot about every 20 s in $OUT/auto_p. Stops when the game closes or the disk is nearly full.
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT/auto_p"
end=$(( $(date +%s) + $1 * 60 )); n=0
while [ $(date +%s) -lt $end ]; do
  for p in "320 120" "170 720" "170 610" "320 120" "470 610" "320 480" "320 630" "320 120" "170 830" "470 720"; do
    "$ROOT/test/ctl_p.sh" tap $p; sleep 2
  done
  n=$((n+1)); "$ROOT/test/shot_p.sh" auto_$n; mv "$OUT/control_p/auto_$n.png" "$OUT/auto_p/$(printf '%03d' $n).png"
  free=$(df --output=avail -m "$OUT" | tail -1)
  if [ "$free" -lt 1300 ]; then echo "disk nearly full ($free MB): closing the game"; "$ROOT/test/ctl_p.sh" quit; break; fi
  pgrep -f runner.exe > /dev/null || break
done
