#!/bin/sh
# release.sh <version> : makes the three files of a release in final/release-<version>/ from the built parts:
#   ExviusRevivalDesktop-win64-<version>.zip    the full package (a first install)
#   ExviusRevivalDesktop-update-<version>.zip   the program's own files, without python\: what the updater downloads, and
#                                               what people on a version without the updater copy over their folder by hand
#   update.json                                 what the updater reads: the version, the update zip's name, size and SHA-256
# Build first (guest/build.sh, build_windows.sh). DEV is the server repository checkout, as for pack.sh.
# The summary shown in the start window's offer is the first paragraph of docs/RELEASE-<version>.md.
# WITH_PYTHON=1 puts the whole python\ folder into the update zip as well: needed only when the Python runtime or
# one of its compiled libraries changed since the last release (a new toolchain zip).
# NEEDS=<version> says that installs older than that cannot take this update through the updater (they are told to
# install it by hand). Leave it out unless the way updates are put in place itself changed.
set -e
cd "$(dirname "$0")"
V=${1:?give the version, for example: sh release.sh 0.2.0}
echo "$V" | grep -Eq '^[0-9]+(\.[0-9]+){1,3}$' || { echo "the version must look like 0.2.0"; exit 1; }
NOTES=docs/RELEASE-$V.md
[ -f "$NOTES" ] || { echo "write $NOTES first: its first paragraph is the summary the updater shows"; exit 1; }
DIR=final/release-$V
NAME=ExviusRevivalDesktop
rm -rf "$DIR"
mkdir -p "$DIR"
VERSION=$V OUT="$DIR/$NAME" sh pack.sh > "$DIR/pack.log"
find "$DIR/$NAME" -name '__pycache__' -type d -prune -exec rm -rf {} +
FULL=$NAME-win64-$V.zip
UPDATE=$NAME-update-$V.zip
(cd "$DIR" && zip -q -r -X "$FULL" "$NAME")
if [ -n "$WITH_PYTHON" ]; then
  (cd "$DIR" && zip -q -r -X "$UPDATE" "$NAME")
else
  (cd "$DIR" && zip -q -r -X "$UPDATE" "$NAME" -x "$NAME/python/*" \
     && zip -q -r -X "$UPDATE" "$NAME"/python/Lib/site-packages/truststore* "$NAME"/python/Lib/site-packages/certifi*)
fi
python3 - "$DIR" "$V" "$FULL" "$UPDATE" "$NOTES" "$NEEDS" <<'PY'
import hashlib, json, re, sys, zipfile
from pathlib import Path
folder, version, full, update, notes, needs = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], Path(sys.argv[5]), sys.argv[6]

def describe(name):
    data = (folder / name).read_bytes()
    return {'file': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

# the first paragraph under the title, as one line
paragraphs = [' '.join(p.split()) for p in re.split(r'\n\s*\n', notes.read_text(encoding='utf-8')) if p.strip() and not p.lstrip().startswith('#')]
summary = paragraphs[0] if paragraphs else ''
if len(summary) > 400:
    sys.exit(f'{notes}: the first paragraph is the summary the start window shows; keep it under 400 characters (now {len(summary)})')
info = {'schema': 1, 'version': version, 'summary': summary, 'update': describe(update), 'full': describe(full)}
if needs:
    info['needs'] = needs
(folder / 'update.json').write_text(json.dumps(info, indent=1) + '\n', encoding='utf-8')
# No game files, no APK, no save, no working notes: the same check for both zips.
bad = []
for name in (full, update):
    for entry in zipfile.ZipFile(folder / name).namelist():
        low = entry.lower()
        top = low.split('/')[1] if '/' in low else low
        if (low.endswith(('.apk', '.xapk', '.apks', '.cpk', '.acb', '.awb', '.usm', '.so', '.rgba'))
                or top in ('client', 'save', 'data', 'logs', 'pictures', 'update', 'settings.json')
                or (low.endswith('.md') and top != 'server')):          # the package has no notes of its own, only the server's README
            bad.append(f'{name}: {entry}')
if bad:
    sys.exit('these must not be in a release:\n' + '\n'.join(bad))
print(json.dumps(info, indent=1))
PY
rm -rf "$DIR/$NAME" "$DIR/pack.log"
echo "== release $V: upload these three files to the release tagged v$V"
ls -l "$DIR"
