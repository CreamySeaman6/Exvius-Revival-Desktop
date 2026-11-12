#!/bin/sh
# fresh_l.sh : make the Linux layout a new install again (keeps the game library and assets from the APK)
. "$(dirname "$0")/env.sh"
rm -rf "$LAYOUT/client/game/files" "$LAYOUT/client/game/prefs.tsv" "$LAYOUT/client/game/files-removed.txt" "$LAYOUT/save" "$LAYOUT/logs"
mkdir -p "$LAYOUT/client/game/files"     # as the launcher's install step leaves it; without it the game's Firebase start-up aborts
