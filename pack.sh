#!/bin/sh
# Assembles the Windows package in dist/ExviusRevivalDesktop from the built parts.
set -e
cd "$(dirname "$0")"
DEV=${DEV:?set DEV to a checkout of the server repository, https://github.com/CreamySeaman6/Exvius-revival-ios (it must contain source/server, ios-server and data/latest.json)}
PY=${PY:-/tmp/pywin/python}
WHL=${WHL:-/tmp/whl}
OUT=${OUT:-dist/ExviusRevivalDesktop}
rm -rf "$OUT"
mkdir -p "$OUT/app" "$OUT/runner" "$OUT/server" "$OUT/source"
cp build/windows/stub.exe "$OUT/Exvius Revival Desktop.exe"
cp app/*.py "$OUT/app/"
mkdir -p "$OUT/app/fonts"
cp fonts/*.ttf fonts/Cinzel-OFL.txt "$OUT/app/fonts/"
cp build/windows/runner-stripped.exe "$OUT/runner/runner.exe"
cp build/windows/SDL2.dll build/game.elf "$OUT/runner/"
cp fonts/Roboto-Regular.ttf "$OUT/runner/font.ttf"
cp docs/README.txt docs/LICENSES.txt "docs/QUICK START.txt" "$OUT/"
# Python: the runtime without debug symbols, headers, the test suite and pip
cp -r "$PY" "$OUT/python"
find "$OUT/python" -name '*.pdb' -delete
rm -rf "$OUT/python/include" "$OUT/python/libs" "$OUT/python/Scripts" "$OUT/python/Lib/test" "$OUT/python/Lib/idlelib" \
       "$OUT/python/Lib/ensurepip" "$OUT/python/Lib/site-packages/pip" "$OUT/python/Lib/site-packages"/pip-* \
       "$OUT/python/Lib/site-packages/setuptools" "$OUT/python/Lib/site-packages"/setuptools-* "$OUT/python/Lib/lib2to3" \
       "$OUT/python/Lib/turtledemo" "$OUT/python/tcl/tk8.6/demos"
find "$OUT/python" -name '__pycache__' -type d -prune -exec rm -rf {} +
mkdir -p "$OUT/python/Lib/site-packages"
for w in "$WHL"/*.whl; do unzip -q -o "$w" -d "$OUT/python/Lib/site-packages"; done
# Server: the engine and start-up code of the iPhone release
mkdir -p "$OUT/server/engine"
(cd "$DEV/source/server" && tar --exclude='__pycache__' -cf - .) | tar -xf - -C "$OUT/server/engine"
cp "$DEV/ios-server/serve.py" "$DEV/ios-server/fetch_data.py" "$DEV/ios-server/docker-compose.yml" "$OUT/server/"
cp "$DEV/data/latest.json" "$OUT/server/latest.json"
# The full list of server settings: the server's own README (at the top of the public repository)
if [ -f "$DEV/ios-server/README.md" ]; then cp "$DEV/ios-server/README.md" "$OUT/server/README.md"; else cp "$DEV/README.md" "$OUT/server/README.md"; fi
git -C "$DEV" log -1 --format='%H %cd' > "$OUT/server/REVISION.txt"
# Source of the runner and the game stand-in
mkdir -p "$OUT/source/runner" "$OUT/source/guest" "$OUT/source/tools" "$OUT/source/stub" "$OUT/source/fonts"
cp runner/*.c runner/*.cpp runner/*.h runner/*.inc runner/runner.rc "$OUT/source/runner/"
cp guest/*.c guest/*.h guest/build.sh "$OUT/source/guest/"
cp tools/gen_gl.py tools/make_icon.py "$OUT/source/tools/"
cp stub/stub.c stub/stub.rc "$OUT/source/stub/"
cp fonts/* "$OUT/source/fonts/"
cp build_linux.sh build_windows.sh pack.sh "$OUT/source/"
cp docs/BUILDING.txt "$OUT/source/" 2>/dev/null || true
du -sh "$OUT" "$OUT"/*
