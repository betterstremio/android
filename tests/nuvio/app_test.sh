#!/bin/bash
# tests/nuvio/app_test.sh BIN ROOT OUT: the whole app against fake_nuvio.py, driven with the remote's keys. Software rendering, about
# 45 frames a second; the frame numbers leave room for a slow machine.
BIN="$1"; ROOT="$2"; O="$3"; PORT=18765; H="$O/home"; D="$H/Library/Application Support/StremioOrigin"; LOG="$O/server.log"; FAILS=0
mkdir -p "$O"; rm -rf "$H" "$O"/*.log "$O"/*.png; mkdir -p "$D"; : > "$LOG"
seed() { echo '{"guideSeen":"1","whatsNewSeen":"9.9.9"}' > "$D/state.json"; }
seed
python3 -I "$ROOT/tests/nuvio/fake_nuvio.py" $PORT "$LOG" & SRV=$!; trap 'kill $SRV 2>/dev/null' EXIT; sleep 0.6
ctl() { python3 -I -c "import urllib.request,sys; urllib.request.urlopen('http://127.0.0.1:$PORT/ctl?'+sys.argv[1]).read()" "$1"; }
run() { # name script exit-frame
  : > "$LOG.part"; local n0=$(wc -l < "$LOG")
  ( cd "$ROOT" && env HOME="$H" SB_ASSETS=assets SB_NV_URL=http://127.0.0.1:$PORT SB_NV_URL2=http://127.0.0.1:$PORT SB_NV_KEY=k SB_SCRIPT="$2" SB_SHOT="$O/$1.png" SB_SHOT_FRAME="$3" SB_SHOT_EXIT=1 SB_ALARM=240 LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
    xvfb-run -a -s "-screen 0 1920x1080x24" "$BIN" > "$O/$1.log" 2>&1; rc=$?; [ $rc = 0 ] || { echo "FAIL $1: exit $rc"; } )
  tail -n +$((n0 + 1)) "$LOG" > "$LOG.part"
  grep -E 'AddressSanitizer|SUMMARY' "$O/$1.log" | cut -c1-200 | head -5
  if grep -q AddressSanitizer "$O/$1.log"; then echo "FAIL $1: sanitizer"; grep -E '^    #[0-9] ' "$O/$1.log" | head -14 | cut -c1-150; FAILS=$((FAILS + 1)); fi
}
# check "what" python-expression over s (state.json), acct (files in acct/), log (rows the server saw in this run)
check() { python3 -I - "$D" "$LOG.part" "$1" "$2" <<'PY' || FAILS=$((FAILS + 1))
import json,sys,os
d,lp,what,expr=sys.argv[1:5]
s=json.load(open(d+'/state.json')); acct=os.listdir(d+'/acct') if os.path.isdir(d+'/acct') else []
log=[json.loads(l) for l in open(lp) if l.strip()]; k=[r['k'] for r in log]
lib=s.get('lib') or {}; shown=sorted(i for i,v in lib.items() if not v.get('removed')); nadd=len(s.get('addons') or [])
ok=eval(expr)
print(('ok   ' if ok else 'FAIL ')+what+('' if ok else '   [profile %s, acct %s, shown %s, addons %d, files %s, server %s]'%(s.get('nvProfile'),s.get('acctKind'),shown,nadd,acct,sorted(set(k)))))
sys.exit(0 if ok else 1)
PY
}
# 1. a Stremio user: the sign-in as it always was; Nuvio's server is not called
run stremio "" 300; check "Stremio sign-in never calls Nuvio" "len(log)==0 and not s.get('acctKind')"
# 2. Nuvio tab, code approved by the phone, profiles, Right + OK = Kids (their addons and library come down)
run signin "100:up,130:right,620:right,660:ok" 1300
check "signed in as Kids" "s.get('nvProfile')=='2' and s.get('acctKind')=='nuvio'"
check "Kids' library and watched came down" "'tt0317219' in shown and lib.get('tt0114709',{}).get('state',{}).get('timesWatched')==1"
check "Kids' addons" "nadd==1"
check "nothing pushed back from a fresh sign-in" "not any(x.startswith('sync_push') or x.startswith('sync_delete') for x in k)"
# 3. start again: the picker over Home; Left + OK = Sam, new on this device: the loading screen
ctl "dead=1"
run sam1 "340:left,380:ok" 1200
check "Sam chosen" "s.get('nvProfile')=='1'"
check "Sam's library, none of Kids'" "'tt0111161' in shown and 'tt0317219' not in shown"
check "Sam's progress came down" "lib.get('tt0468569',{}).get('state',{}).get('timeOffset')==600000"
check "a dead manifest with no copy: the other addons stay" "nadd==2"
check "Kids' data set aside" "any(f.endswith('-p2.json') for f in acct)"
ctl "dead=0"
# 4. a manifest that dies after we have its copy keeps its addon (6 Nov fault)
python3 -I - "$D" <<'PY'
import json,sys
p=sys.argv[1]+'/state.json'; s=json.load(open(p))
s['addons'].append({"transportUrl":"http://127.0.0.1:18765/dead/manifest.json","manifest":{"id":"fake.dead","name":"Fake dead","version":"1.0.0","resources":["stream"],"types":["movie"],"catalogs":[]},"flags":{},"nvAt":0})
json.dump(s,open(p,'w'))
PY
ctl "dead=1"
run deadkeep "340:ok" 500
check "a manifest that fails keeps its addon" "nadd==3"
check "young manifests are not asked for again" "sum(1 for r in log if r['k']=='manifest' and r['b']!='dead')==0"
ctl "dead=0"
# 5. switching back to a kept profile downloads nothing before Home; the sync follows
run kids2 "340:right,380:ok" 800
check "back on Kids from the kept copy" "s.get('nvProfile')=='2' and 'tt0317219' in shown and nadd==1"
# 6. the PIN: Locked (Right), wrong then right
PIN="340:right,370:ok,420:ok,450:down,480:down,510:down,540:right,570:right,600:ok,  660:left,690:left,720:up,750:up,780:up,810:ok,840:right,870:ok,900:right,930:ok,960:down,990:left,1020:left,1050:ok,1080:down,1110:down,1140:right,1170:right,1200:ok"
run pin "${PIN// /}" 1700
check "a wrong PIN then the right one" "s.get('nvProfile')=='3' and [r['b'] for r in log if r['k']=='pin']==['1','1234']"
check "uses the primary's addons" "any(r['k']=='addons' and r['b']==1 for r in log)"
# 7. the session: 401 once, refreshed; 500 once, the second address
ctl "expire=1"; run refresh "340:ok" 500; check "401: refreshed and carried on" "'refresh' in k and 'pull_lib' in k"
ctl "fail=1"; run fallback "340:ok" 500; check "500: the second address answers" "'pull_wat' in k"
# 8. the server ends the session: back to sign-in, signed out, nothing left behind
ctl "expire=1"; ctl "refuse=1"; run lost "340:ok" 500; ctl "refuse=0"
check "session refused: signed out and forgotten" "not s.get('acctKind') and not s.get('nvRefresh') and not [f for f in acct if f.startswith('nv-')] and not lib"
echo "fails: $FAILS"; [ $FAILS = 0 ] && echo "APP TEST PASSED"
