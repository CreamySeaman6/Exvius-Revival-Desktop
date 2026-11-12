#!/bin/sh
# ab_l.sh <name> <cores> : start the game on Linux with that many cores, tap through the title, let it load
# (LOAD seconds, default 150), then add up the runner's 5-second reports. EXTRA adds launcher arguments,
# for example EXTRA="--env FFBE_AUDIO_RESERVE=0". Expects an install that is past its first run.
. "$(dirname "$0")/env.sh"
name=$1; cores=$2
(setsid "$ROOT/test/play_l.sh" run_$name.log --env FFBE_NO_SETUP_RESTART=1 --env FFBE_CORES=$cores $EXTRA > /dev/null 2>&1 &)
sleep 40
"$ROOT/test/ctl_l.sh" tap 320 740
sleep ${LOAD:-150}
"$ROOT/test/shot_l.sh" $name
"$ROOT/test/ctl_l.sh" quit
sleep 4
pkill -f 'launcher.py --headless'; pkill -f desktop_server.py
cp "$LAYOUT/client/runner.log" "$OUT/runner_$name.log"
grep '^\[runner\] 5 s' "$OUT/runner_$name.log" | python3 -c "
import sys,re
f=l=g=0; w=0; rows=[]
for s in sys.stdin:
    m=re.search(r'(\d+) frames, (\d+) late, longest (\d+) ms; (\d+) sound gaps, least in hand (-?\d+)',s)
    if m:
        a,b,c,d,e=map(int,m.groups()); f+=a; l+=b; g+=d; w=max(w,c); rows.append((a,b,c,d,e))
print('reports',len(rows),'frames',f,'late',l,'longest',w,'ms','sound gaps',g)
print('per 5 s (frames/late/longest ms/sound gaps/least sound in hand ms):')
print(' '.join('%d/%d/%d/%d/%d'%r for r in rows))
"
echo "crashes: $(grep -c 'guest stopped' "$OUT/runner_$name.log")"
