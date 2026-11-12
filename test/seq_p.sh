#!/bin/sh
# seq_p.sh <name> "x y wait" ... : tap, wait that many seconds, repeat; then take one screenshot called <name>
. "$(dirname "$0")/env.sh"
name=$1; shift
for step in "$@"; do set -- $step; "$ROOT/test/ctl_p.sh" tap $1 $2; sleep $3; done
"$ROOT/test/shot_p.sh" "$name"
