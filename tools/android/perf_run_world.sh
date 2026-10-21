#!/bin/bash
# usage: run-world.sh <label> [marker[=value] ...]   (PPG; enters the world, waits, prints last RENDER_PROFILE windows)
# Always clears the optional markers run-mobile.sh clears, plus draw/present markers, then applies the given ones.
export MSYS_NO_PATHCONV=1 ANDROID_SERIAL=${ANDROID_SERIAL:-AVAY025422000548}
P=org.d2r.mobile; D=files/d2r-mobile/diagnostics; L=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
adb shell "input keyevent KEYCODE_WAKEUP" >/dev/null
adb shell "run-as $P sh -c 'cd $D && rm -f disable-fault-around protect-around.txt fault-around.txt enable-dynamic-ubo desc-diag disable-set-recycle disable-runs-cache disable-shader-stamp disable-shader-get-cache enable-yield-backoff enable-bc-passthrough finalize-wait.txt acquire-sync.txt hint-target-ns.txt translate-affinity.txt acquire-affinity.txt performance-hints guest-wait-diag present-60fps draw-timing wsi-diag render-scale.txt resolution-tier.txt lod-bias.txt skip-ps.txt profile-detail barrier-stats pass-timing disable-* private-vulkan private-vulkan-sysmem att-load-table.txt cov-table.txt enable-att-load-table enable-att-store-skip; rm -rf shader-override; true'"
for kv in "$@"; do f=${kv%%=*}; c=${kv#*=}; [ "$f" = "$kv" ] && c=""; adb shell "run-as $P sh -c 'cd $D && printf %s \"$c\" > $f'"; done
adb shell am force-stop $P; sleep 1; adb logcat -c
adb shell am start -W -n $P/org.d2r.binaryprobe.GameActivity >/dev/null; sleep ${PRE:-18}
# Enter the world from character select. Default: tap the "enter game" button at a fixed fraction of the landscape screen
# (0.524, 0.897 of width/height, measured on PPG-AN00), so it works on other panel sizes; ENTER="tap X Y" overrides, ENTER=gamepad
# presses A instead (only works in the gamepad UI).
if [ "${ENTER:-}" = "gamepad" ]; then adb shell "input gamepad keyevent BUTTON_A" </dev/null
elif [ -n "${ENTER:-}" ]; then adb shell input $ENTER </dev/null
else SZ=$(adb shell wm size </dev/null | tr -d '' | grep -o '[0-9]*x[0-9]*' | tail -1); W=${SZ%x*}; H=${SZ#*x}
  if [ "$H" -gt "$W" ]; then T=$W; W=$H; H=$T; fi
  adb shell input tap $((W*524/1000)) $((H*897/1000)) </dev/null; fi
sleep ${LOAD:-28}
adb exec-out run-as $P cat $D/game-startup.log > "$HERE/$L.log" 2>/dev/null
grep -m1 "driver id" "$HERE/$L.log" | cut -c1-60
grep -a "RENDER_PROFILE frame" "$HERE/$L.log" | tail -${NSHOW:-4} | sed -E 's/.*(frame=[0-9]+ frames=[0-9]+ fps=[0-9.]+ elapsed_ms=[0-9.]+).*(gpu_wait_ms=[0-9.]+).*(draws=[0-9]+)/\1 \2 \3/'
