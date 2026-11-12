# Sourced by the scripts in this folder. Set the variables before running a script to point somewhere else.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${DEV:=$HOME/Exvius-revival-ios}"              # checkout of the server repository (github.com/CreamySeaman6/Exvius-revival-ios)
: "${DATA:=$ROOT/build/data}"                     # game data folder (the one that holds native/ and static/)
: "${LAYOUT:=$ROOT/build/linux-layout}"           # package-shaped folder with the Linux runner (make_linux_layout.sh)
: "${PACKAGE:=$ROOT/dist/ExviusRevivalDesktop}"   # the Windows package from pack.sh, for the Wine tests
: "${OUT:=$ROOT/test/out}"                        # logs, screenshots, control folders
: "${DISPLAY:=:99}"
export DISPLAY
mkdir -p "$OUT/control_l" "$OUT/control_p"
