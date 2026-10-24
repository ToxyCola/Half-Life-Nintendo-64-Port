#!/bin/bash
# Smoke-test every converted map on the PC harness: load, spawn, idle 20 s, then walk around 20 s.
# build: gcc -O1 -g -w -DHOST_SWAP_BE -Itest/stubs -Isrc -Ivalve_sdk/{common,engine,pm_shared,public} -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp -DMAX_PHYSENTS=128 test/host_main.c test/host_swap.c src/bsp.c src/entities.c src/pmove.c valve_sdk/pm_shared/pm_shared.c valve_sdk/pm_shared/pm_math.c -lm -o hosttest_be
# usage: test/smoke_all.sh <dir containing maps/*.hlb (uncompressed, big-endian)> <hosttest_be binary>
ROOT=$1; BIN=$2
S=$(mktemp); printf "0 20 0 0 0 0\n20 25 400 0 0 0\n25 26 0 0 90 0\n26 30 400 0 0 2\n30 31 0 0 -150 0\n31 40 400 120 0 0\n" > $S
pass=0; fail=0
for f in $ROOT/maps/*.hlb; do
  m=$(basename $f .hlb)
  out=$(timeout 120 $BIN $ROOT $m $S 40 2>&1); rc=$?
  ents=$(echo "$out" | grep -o "spawned [0-9]* entities" | head -1)
  last=$(echo "$out" | grep "^t=" | tail -1)
  stuck=$(echo "$out" | grep "^t=" | tail -20 | grep -c "hull1 -2")
  if [ $rc -ne 0 ]; then echo "FAIL $m rc=$rc"; fail=$((fail+1)); else pass=$((pass+1)); fi
  printf "%-8s rc=%d %-22s stuck=%2d  %s\n" $m $rc "$ents" $stuck "$(echo $last | cut -c1-70)"
done
echo "pass=$pass fail=$fail"
