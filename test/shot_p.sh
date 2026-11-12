#!/bin/sh
# shot_p.sh <name> : screenshot of the running game as $OUT/control_p/<name>.png
. "$(dirname "$0")/env.sh"
"$ROOT/test/ctl_p.sh" shot "$1"
for i in $(seq 1 50); do [ -s "$OUT/control_p/$1.ppm" ] && break; sleep 0.1; done
python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$OUT/control_p/$1.ppm" "$OUT/control_p/$1.png"
rm -f "$OUT/control_p/$1.ppm"
