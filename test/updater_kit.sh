#!/bin/sh
# updater_kit.sh <release folder> [label] : a package for trying the updater on a real Windows PC before (and
# after) a release is published. It is the same program as the release, labelled with a lower version (0.1.99
# unless another label is given), so the updater has something newer to offer. Inside the package folder it adds
#   update-test\                              the release's update.json and update zip
#   Test the updater from this folder.cmd     starts the program with its update check pointed at update-test
# Started by the .cmd, the program is offered the release from that folder: nothing has to be published.
# Started by its .exe, it asks GitHub like any install, and offers the release once it is published.
# Needs the built parts and DEV, as pack.sh does. Writes <release folder>/../ExviusRevivalDesktop-win64-<label>-updater-test.zip.
set -e
cd "$(dirname "$0")/.."
REL=${1:?give the release folder, for example final/release-0.2.0}
LABEL=${2:-0.1.99}
[ -f "$REL/update.json" ] || { echo "$REL has no update.json: run release.sh first"; exit 1; }
REL=$(cd "$REL" && pwd)
ZIPNAME=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['update']['file'])" "$REL/update.json")
WORK=$(dirname "$REL")/updater-kit
rm -rf "$WORK"; mkdir -p "$WORK"
VERSION=$LABEL OUT="$WORK/ExviusRevivalDesktop" sh pack.sh > "$WORK/pack.log"
find "$WORK/ExviusRevivalDesktop" -name '__pycache__' -type d -prune -exec rm -rf {} +
mkdir "$WORK/ExviusRevivalDesktop/update-test"
cp "$REL/update.json" "$REL/$ZIPNAME" "$WORK/ExviusRevivalDesktop/update-test/"
printf '%s\r\n' '@echo off' \
  'rem Starts the program in this folder with its update check pointed at the "update-test" folder beside this' \
  'rem file instead of the release page on GitHub: for trying the updater before a release is published.' \
  'set "EXVIUS_DESKTOP_UPDATE_URL=%~dp0update-test"' \
  'start "" "%~dp0Exvius Revival Desktop.exe"' > "$WORK/ExviusRevivalDesktop/Test the updater from this folder.cmd"
OUT=$(dirname "$REL")/ExviusRevivalDesktop-win64-$LABEL-updater-test.zip
rm -f "$OUT"
(cd "$WORK" && zip -q -r -X "$OUT" ExviusRevivalDesktop)
rm -rf "$WORK"
ls -l "$OUT"
