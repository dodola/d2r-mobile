#!/bin/bash
# Cumulative A/B for the 2026-10-22 PPG CPU-side work, with the device state printed next to every number.
#   usage: bench-all.sh [rounds=3]
# "old" = every optimisation of that day disabled through its marker; "new" = defaults.
# Only trust the numbers when battery >= ~60% and scaling_max_freq of cpu0 is the hardware maximum (3532800).
export MSYS_NO_PATHCONV=1 ANDROID_SERIAL=${ANDROID_SERIAL:-AVAY025422000548}
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"
ROUNDS=${1:-3}
OLD="disable-fault-around disable-runs-cache disable-shader-stamp disable-shader-get-cache disable-set-recycle disable-storage-bias disable-present-60fps"
state() { adb shell "echo bat=\$(dumpsys battery | grep -m1 level | tr -d ' ') temp=\$(dumpsys battery | grep -m1 temperature | tr -d ' ') cpu0max=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) cpu6max=\$(cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_max_freq)" | tr -d '\r'; }
fps() { ./perf_run_world.sh "$1" NOPROF render-scale.txt=1.0 ${@:2} </dev/null 2>&1 | tail -3 | sed -E 's/.*fps=([0-9.]+).*/\1/' | awk '{f+=$1} END {printf "%.1f", f/NR}'; }
echo "start: $(state)"
case "$(state)" in *level:[0-5]?*|*level:[0-9]\ *|*level:[0-9]) echo "WARNING: battery below 60% - the system caps the CPU, numbers are only valid as same-state pairs";; esac
for r in $(seq 1 "$ROUNDS"); do
  o=$(fps bench$r-old $OLD)
  n=$(fps bench$r-new)
  echo "round $r: old=$o new=$n  ($(state))"
done
