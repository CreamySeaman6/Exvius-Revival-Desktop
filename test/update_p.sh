#!/bin/sh
# update_p.sh [scenario ...] : the updater under Wine, without clicking. Each scenario starts from a fresh copy of
# the Windows package in $PACKAGE (labelled with a lower version: VERSION=0.1.99 OUT=dist/ExviusRevivalDesktop sh
# pack.sh) and is offered the release in $RELEASE (sh release.sh 0.2.0 -> final/release-0.2.0).
# Scenarios (all of them when none is named):
#   good      download, check, unpack, replace, restart: the new version's start window is up
#   checksum  a zip that is not what update.json describes: refused, nothing changed
#   fail      a move made to fail half-way (EXVIUS_UPDATE_TEST_FAIL): everything put back, the old version starts
#   stop      the replacing cut off half-way (EXVIUS_UPDATE_TEST_STOP), as by a power cut: the next start puts it back
#   foreign   a zip that names something outside the program's own files: refused whole
# Every scenario also checks that client\, save\, data\, logs\, pictures\ and settings.json are as they were.
# It starts a display of its own (Xvfb + openbox). Results: one line per check, "ok" or "FAILED".
. "$(dirname "$0")/env.sh"
RELEASE=${RELEASE:-$ROOT/final/release-0.2.0}
WORK=${WORK:-/tmp/update-test}
INST=$WORK/ExviusRevivalDesktop
export WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-/tmp/wt/prefix} DISPLAY=:${DISP:-95}
pkill -f "Xvfb $DISPLAY " 2> /dev/null; sleep 0.5
Xvfb "$DISPLAY" -screen 0 1600x1200x24 -nolisten tcp > /dev/null 2>&1 &
xvfb=$!
sleep 1
openbox > /dev/null 2>&1 &
sleep 0.5
bad=0
check() { if [ "$2" = "$3" ]; then echo "  ok      $1"; else echo "  FAILED  $1: expected [$3], got [$2]"; bad=1; fi; }
version() { sed -n "s/^VERSION = '\(.*\)'/\1/p" "$INST/app/version.py" 2> /dev/null; }
# every file of the program's own items with its checksum: two installs with the same list are the same program
program() { (cd "$INST" && find app runner server source python "Exvius Revival Desktop.exe" README.txt LICENSES.txt "QUICK START.txt" \
              -name __pycache__ -prune -o -type f -print0 2> /dev/null | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16); }
mine() { (cd "$INST" && find client save data pictures settings.json -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16); }
fresh() {
  wineserver -k 2> /dev/null; sleep 1
  rm -rf "$WORK"; mkdir -p "$WORK"
  cp -r "$PACKAGE" "$INST"
  mkdir -p "$INST/client/game" "$INST/save/state" "$INST/data/native" "$INST/logs" "$INST/pictures"
  echo "the player's own" > "$INST/client/game/libgame.so"; echo "save" > "$INST/save/state/player.json"
  echo "data" > "$INST/data/installed.json"; echo "picture" > "$INST/pictures/one.png"; echo '{"volume": 30}' > "$INST/settings.json"
  old=$(version); before=$(program); kept=$(mine)
}
# The launcher without its window. Its output goes through a pipe into a file (Windows Python under Wine cannot write
# to a file directly), in the background: the programs it starts keep that pipe open for as long as they run.
launcher() {
  rm -f "$WORK/out.txt"
  (cd "$INST" && EXVIUS_DESKTOP_UPDATE_URL="Z:$1" wine python/python.exe app/launcher.py --headless $2 < /dev/null 2>&1 | cat > "$WORK/out.txt") > /dev/null 2>&1 &
  for i in $(seq 1 120); do sleep 1; grep -qE 'handed over|Stopped|no update to offer|is offered' "$WORK/out.txt" 2> /dev/null && { [ "$2" = --update ] && ! grep -qE 'handed over|Stopped|no update' "$WORK/out.txt" && continue; break; }; done
  cat "$WORK/out.txt"
}
settle() { for i in $(seq 1 "${1:-40}"); do sleep 1; grep -q "$2" "$INST/logs/update.log" 2> /dev/null && break; done; sleep 2; }
window_up() { for i in $(seq 1 30); do xdotool search --name '^Exvius Revival Desktop$' > /dev/null 2>&1 && { echo yes; return; }; sleep 1; done; echo no; }
new=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['version'])" "$RELEASE/update.json")
[ $# -gt 0 ] || set -- good checksum fail stop foreign
for scenario in "$@"; do
  echo "== $scenario"
  fresh
  case $scenario in
  good)
    launcher "$RELEASE" --check-update | grep -c "version $new is offered" | { read n; check "the newer version is offered" "$n" 1; }
    launcher "$RELEASE" --update | tail -2
    settle 60 "updated: $old -> $new"
    check "the installed version is the new one" "$(version)" "$new"
    check "update.log says so" "$(grep -c "updated: $old -> $new" "$INST/logs/update.log")" 1
    check "the old version's files are in the backup" "$(sed -n "s/^VERSION = '\(.*\)'/\1/p" "$INST/update/backup/$old/app/version.py")" "$old"
    check "the new version's start window opened" "$(window_up)" yes
    sleep 3
    check "the start window read the result" "$(grep -c "update: $old -> $new installed" "$INST/logs/launcher.log")" 1
    check "download and unpacked copy are cleared" "$(ls "$INST/update" | tr '\n' ' ')" "backup "
    ( cd "$WORK" && rm -rf expect && mkdir expect && cd expect && unzip -q "$RELEASE/ExviusRevivalDesktop-update-$new.zip" )
    check "app, runner, server are the release's files" \
      "$(cd "$INST" && find app runner server -name __pycache__ -prune -o -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)" \
      "$(cd "$WORK/expect/ExviusRevivalDesktop" && find app runner server -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"
    ;;
  checksum)
    cp -r "$RELEASE" "$WORK/release"; printf 'x' | dd of="$WORK/release/ExviusRevivalDesktop-update-$new.zip" bs=1 seek=4000000 conv=notrunc 2> /dev/null
    launcher "$WORK/release" --update | tail -1
    check "the version is unchanged" "$(version)" "$old"
    check "the program's files are unchanged" "$(program)" "$before"
    check "nothing is waiting to be put in place" "$(ls "$INST/update/staged" 2> /dev/null | wc -l)" 0
    ;;
  fail)
    export EXVIUS_UPDATE_TEST_FAIL=6; launcher "$RELEASE" --update | tail -1; unset EXVIUS_UPDATE_TEST_FAIL
    settle 60 "not updated"
    check "update.log names the failure" "$(grep -c "not updated: $old -> $new" "$INST/logs/update.log")" 1
    check "the version is the old one" "$(version)" "$old"
    check "the program's files are exactly as before" "$(program)" "$before"
    check "the old version's start window opened" "$(window_up)" yes
    sleep 3
    check "the start window was told why" "$(grep -c "update: $old -> $new not installed" "$INST/logs/launcher.log")" 1
    ;;
  stop)
    export EXVIUS_UPDATE_TEST_STOP=5; launcher "$RELEASE" --update | tail -1; unset EXVIUS_UPDATE_TEST_STOP
    settle 60 "test: stopping"
    check "it was cut off with the journal in place" "$(ls "$INST/update/journal.txt" 2> /dev/null | wc -l)" 1
    half=$(program)
    [ "$half" != "$before" ] && echo "  ok      the folder is half replaced at this point" || { echo "  FAILED  the folder should be half replaced here"; bad=1; }
    (cd "$INST" && wine "Exvius Revival Desktop.exe") < /dev/null > /dev/null 2>&1 &
    settle 60 "not updated"
    check "the next start put the old files back" "$(program)" "$before"
    check "the version is the old one" "$(version)" "$old"
    check "the old version's start window opened" "$(window_up)" yes
    ;;
  foreign)
    cp -r "$RELEASE" "$WORK/release"
    ( cd "$WORK" && mkdir -p x/ExviusRevivalDesktop/save/state && echo "not yours" > x/ExviusRevivalDesktop/save/state/player.json \
      && cd x && zip -q "$WORK/release/ExviusRevivalDesktop-update-$new.zip" ExviusRevivalDesktop/save/state/player.json )
    python3 - "$WORK/release" "$new" <<'PY'
import hashlib, json, sys
from pathlib import Path
folder, new = Path(sys.argv[1]), sys.argv[2]
info = json.loads((folder / 'update.json').read_text())
data = (folder / info['update']['file']).read_bytes()
info['update'].update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
(folder / 'update.json').write_text(json.dumps(info))
PY
    launcher "$WORK/release" --update | tail -1
    check "the version is unchanged" "$(version)" "$old"
    check "the program's files are unchanged" "$(program)" "$before"
    check "nothing is waiting to be put in place" "$(ls "$INST/update/staged" 2> /dev/null | wc -l)" 0
    ;;
  esac
  check "client, save, data, pictures and settings.json are untouched" "$(mine)" "$kept"
done
wineserver -k 2> /dev/null
kill "$xvfb" 2> /dev/null
[ $bad = 0 ] && echo "== all checks passed" || echo "== SOME CHECKS FAILED"
exit $bad
