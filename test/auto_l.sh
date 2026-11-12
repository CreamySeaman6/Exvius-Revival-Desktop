#!/bin/sh
# auto_l.sh <minutes> : tap the usual first-run and tutorial spots in the Linux game (640x960 game picture) and keep
# a screenshot about every 20 s in $OUT/auto_l. Stops when the game closes.
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT/auto_l"
end=$(( $(date +%s) + $1 * 60 )); n=0
while [ $(date +%s) -lt $end ]; do
  for p in "320 120" "170 720" "170 610" "320 120" "470 610" "320 480" "320 630" "320 120" "170 830" "470 720" "320 740" "320 435" "440 612"; do
    "$ROOT/test/ctl_l.sh" tap $p; sleep 1.5
  done
  n=$((n+1)); "$ROOT/test/shot_l.sh" auto_$n; mv "$OUT/control_l/auto_$n.png" "$OUT/auto_l/$(printf '%03d' $n).png"
  pgrep -f "linux-layout/runner/runner" > /dev/null || break
done
