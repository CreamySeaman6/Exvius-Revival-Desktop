#!/bin/sh
# make_linux_layout.sh [apk] : build a package-shaped folder that uses the Linux runner, for testing without Wine.
# Needs build/linux/runner and build/game.elf (build_linux.sh, guest/build.sh) and the server repository (DEV).
# With an APK it also copies the game out of it, as the launcher's first step does.
set -e
. "$(dirname "$0")/env.sh"
[ -d "$DEV/source/server" ] || { echo "set DEV to the server repository checkout (now: $DEV)"; exit 1; }
mkdir -p "$LAYOUT/runner"
ln -sfn "$ROOT/app" "$LAYOUT/app"
cp "$ROOT/build/linux/runner" "$ROOT/build/game.elf" "$LAYOUT/runner/"
cp "$ROOT/fonts/Roboto-Regular.ttf" "$LAYOUT/runner/font.ttf"
rm -rf "$LAYOUT/server"
mkdir -p "$LAYOUT/server/engine"
(cd "$DEV/source/server" && tar --exclude='__pycache__' -cf - .) | tar -xf - -C "$LAYOUT/server/engine"
cp "$DEV/ios-server/serve.py" "$DEV/ios-server/fetch_data.py" "$DEV/ios-server/docker-compose.yml" "$LAYOUT/server/"
cp "$DEV/data/latest.json" "$LAYOUT/server/latest.json"
if [ -f "$DEV/ios-server/README.md" ]; then cp "$DEV/ios-server/README.md" "$LAYOUT/server/README.md"; else cp "$DEV/README.md" "$LAYOUT/server/README.md"; fi
if [ -n "$1" ]; then
  EXVIUS_DESKTOP_HOME="$LAYOUT" EXVIUS_DESKTOP_DATA="$DATA" python3 "$ROOT/app/launcher.py" --headless --apk "$1" --skip-data
fi
echo "layout ready in $LAYOUT"
