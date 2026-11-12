#!/bin/sh
# story_l.sh <minutes> : keep tapping through story quests with AUTO battles in the Linux game (640x960 picture);
# a screenshot per round goes to $OUT/story_l. Start it on a quest list or mid-quest.
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT/story_l"
end=$(( $(date +%s) + $1 * 60 )); n=0
while [ $(date +%s) -lt $end ]; do
  for p in "590 138" "130 80" "333 560" "320 921" "320 300" "320 921" "92 885" "170 610" "170 720" "352 893" "320 884" "92 885" "320 352" "170 610" "170 720" "320 500"; do
    "$ROOT/test/ctl_l.sh" tap $p; sleep 2
  done
  [ $((n % 4)) -eq 3 ] && { "$ROOT/test/ctl_l.sh" drag 110 610 300 610; sleep 3; "$ROOT/test/ctl_l.sh" tap 460 610; sleep 2; }
  n=$((n+1)); "$ROOT/test/shot_l.sh" story_$n; mv "$OUT/control_l/story_$n.png" "$OUT/story_l/$(printf '%03d' $n).png"
  pgrep -f "linux-layout/runner/runner" > /dev/null || break
done
