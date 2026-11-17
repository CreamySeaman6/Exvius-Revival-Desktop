#!/bin/sh
# scroll.sh <runner.log> : real mouse gestures on the long list that is on the game's screen, one line each with
# where the list stood before and after. The game must run with FFBE_TOUCH_LOG=1 and FFBE_SCROLL_LOG=1 (play_l.sh
# ... --env FFBE_TOUCH_LOG=1 --env FFBE_SCROLL_LOG=1; under Wine the log is <package>/client/runner.log), and
# $DISPLAY must be the game's display. Put a list on the screen first (the Vortex, a shop).
# The list's place in window pixels, if it is not the Vortex's: X (320), BARX (612: its scroll bar), TOP (300), BOT (985).
# "ok": the list went the way it should and did not start from somewhere else than where it stood.
# The wheel must move the list the same way wherever the pointer is, also on the scroll bar; a drag with a rest
# must leave the list where it was put; a flick must let it glide on; a press made in passing must still scroll.
R=${1:?give the runner.log of the game}
HERE=$(cd "$(dirname "$0")" && pwd)
m() { python3 "$HERE/mouse.py" "$@"; }
bad=0
ask() {   # ask <label> <down|up|bar> <mouse.py words...>
  label=$1; want=$2; shift 2
  n0=$(wc -l < "$R")
  m "$@"
  sleep "${SETTLE:-2.2}"
  seg=$(tail -n +$((n0+1)) "$R")
  y0=$(echo "$seg" | grep -m1 '\[scroll\] began' | sed -E 's/.*: y ([0-9-]+) -> ([0-9-]+).*/\1/')
  yb=$(echo "$seg" | grep -m1 '\[scroll\] began' | sed -E 's/.*: y ([0-9-]+) -> ([0-9-]+).*/\2/')
  y1=$(echo "$seg" | grep '\[scroll\] \(moved\|ended\)' | tail -1 | sed -E 's/.*: y ([0-9-]+) -> ([0-9-]+).*/\2/')
  bar=$(echo "$seg" | grep -c 'ON-THE-SCROLL-BAR')
  glide=$(echo "$seg" | grep '\[scroll\] ended' | tail -1 | grep -c gliding)
  downs=$(echo "$seg" | grep -c '\[touch\] down'); moves=$(echo "$seg" | grep -c '\[touch\] move')
  verdict=ok
  case $want in
    down) [ -n "$y0" ] && [ -n "$y1" ] && [ "$y1" -gt "$y0" ] && [ "$yb" = "$y0" ] || verdict=FAILED ;;
    up)   [ -n "$y0" ] && [ -n "$y1" ] && [ "$y1" -lt "$y0" ] && [ "$yb" = "$y0" ] || verdict=FAILED ;;
    bar)  [ "$bar" -gt 0 ] || verdict=FAILED ;;
  esac
  [ "$want" != bar ] && [ "$bar" -gt 0 ] && verdict="FAILED (took the scroll bar)"
  [ "$verdict" = ok ] || bad=1
  printf '%-7s %-44s y %5s -> %5s  %s touches down, %s moves%s\n' "$verdict" "$label" "${y0:--}" "${y1:--}" "$downs" "$moves" \
    "$([ "$glide" -gt 0 ] && echo ', glides on')"
}
X=${X:-320}; BARX=${BARX:-612}; TOP=${TOP:-300}; BOT=${BOT:-985}
MID=$(( (TOP + BOT) / 2 )); A=$(( TOP + (BOT - TOP) * 3 / 4 )); B=$(( TOP + (BOT - TOP) / 4 ))
m move "$X" "$MID"; sleep 0.3
ask "wheel down 1 notch, middle of the list"   down wheel -1
ask "wheel down 3 notches"                     down wheel -3 70
ask "wheel up 2 notches"                       up   wheel 2 70
m move "$BARX" "$MID"; sleep 0.3
ask "wheel down 2, pointer on the scroll bar"  down wheel -2 70
ask "wheel up 1, pointer on the scroll bar"    up   wheel 1 70
m move "$X" $((TOP + 8)); sleep 0.3
ask "wheel down 2, pointer at the list's top"  down wheel -2 70
m move "$X" $((BOT - 8)); sleep 0.3
ask "wheel up 2, pointer at the list's bottom" up   wheel 2 70
ask "drag up with a rest"                      down drag "$X" "$A" "$X" "$B" 400
ask "drag down with a rest"                    up   drag "$X" "$B" "$X" "$A" 400
ask "flick up in 0.25 s, no rest"              down flick "$X" "$A" "$X" "$B" 250
SETTLE=3 ask "flick down in 0.2 s, no rest"    up   flick "$X" "$B" "$X" "$A" 200
ask "pressed in passing, then dragged up"      down pass $((X - 120)) $((A + 60)) "$X" "$A" "$X" "$B" 300
ask "quick flick up made in passing (0.1 s)"   down pass $((X - 120)) $((A + 60)) "$X" "$A" "$X" "$B" 100
SETTLE=3 ask "the scroll bar itself, dragged down" bar drag "$BARX" $((TOP + 40)) "$BARX" $((BOT - 60)) 500
[ $bad = 0 ] && echo "== all gestures ok (a list already at its end cannot go further: start in its middle)" || echo "== SOME GESTURES FAILED"
exit $bad
