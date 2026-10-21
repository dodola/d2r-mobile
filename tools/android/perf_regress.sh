#!/bin/bash
# Regression of the 2026-10-22 PPG work on another device (written for TNA-AN00 / Turnip, works on any serial).
#   usage: ANDROID_SERIAL=<serial> tna-regress.sh [rounds=2]
# Needs the debuggable APK of this branch installed (analysis/d2r-mobile-port/release-94012/apk-port-*/D2R-Mobile-debuggable.apk,
# built with analysis/perf-runs/ppg-1022/pack.sh) and a character in the character-select screen.
#
# For every optimisation that has a switch it runs  everything on  vs  that one switch off  and prints the camp FPS and whether
# the log is clean, then a visual comparison against the all-off baseline. A switch whose FPS is not lower than "all on" costs
# nothing; look hardest at any run with errors or a screenshot difference above ~1.0.
export MSYS_NO_PATHCONV=1
: "${ANDROID_SERIAL:?set ANDROID_SERIAL to the device under test}"
export ANDROID_SERIAL
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"; mkdir -p shots/regress
ROUNDS=${1:-2}
SWITCHES="disable-fault-around disable-runs-cache disable-shader-stamp disable-shader-get-cache disable-set-recycle disable-storage-bias disable-present-60fps"
state() { adb shell "echo bat=\$(dumpsys battery | grep -m1 level | tr -d ' ') cpu0max=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq)" | tr -d '\r'; }
fps() { ./perf_run_world.sh "$1" NOPROF render-scale.txt=1.0 ${@:2} </dev/null 2>&1 | tail -3 | sed -E 's/.*fps=([0-9.]+).*/\1/' | awk '{f+=$1} END {printf "%.1f", f/NR}'; }
errors() { adb exec-out run-as org.d2r.mobile cat files/d2r-mobile/diagnostics/game-startup.log 2>/dev/null | grep -a -c -E "STOP|DEVICE_LOST|fatal|SIGSEGV|SIGABRT|BUSY timeout|descriptor pool"; }
shot() { sleep 3; adb exec-out screencap -p > "shots/regress/$1.png"; }
echo "device: $(adb shell getprop ro.product.model | tr -d '\r') $(adb shell getprop ro.soc.model | tr -d '\r')  start: $(state)"
for r in $(seq 1 "$ROUNDS"); do
  line="round $r  all-on=$(fps rg$r-on)  errors=$(errors)"; shot rg$r-on
  for sw in $SWITCHES; do line="$line  $sw=$(fps rg$r-$sw $sw)/e$(errors)"; done
  echo "$line   ($(state))"
done
# every optimisation off, then compare the picture with everything on
fps rg-alloff $SWITCHES >/dev/null; shot rg-alloff
python -I - <<'EOF'
import cv2, numpy as np
a = cv2.imread('shots/regress/rg1-on.png').astype(np.float32); b = cv2.imread('shots/regress/rg-alloff.png').astype(np.float32)
d = np.abs(a - b)
print('screenshot all-on vs all-off: mean abs diff %.2f  p99 %.1f  (run-to-run noise on PPG is 0.45-0.56)' % (d.mean(), np.percentile(d, 99)))
EOF
adb shell am force-stop org.d2r.mobile
