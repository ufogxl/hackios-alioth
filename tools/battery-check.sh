#!/bin/bash
# Watch the battery for a while: what the kernel reads, and what the status bar draws, side by side.
#
#   tools/battery-check.sh [minutes]
#
# The status bar's number comes from the SpringBoard shim in userland/src/inject_sb.c, which reads
# kern.hakios.battery_soc; that value is the Qualcomm firmware's, polled every 5 s into a shared page
# (bsd/dev/hakios.battery.c) and printed once a minute as
#
#   battery: SOC=81%, V=4092 mV, I=315 mA, T=35 C, battery=1 charger=1 charging=1 action=3
#
# So the question "does the status bar follow the battery" is answered by comparing that line with a
# screenshot taken at the same time. This leaves the run alone (xl log and xl screenshot are the two
# commands allowed while one is in progress) and writes logs/battery-*.png.
set -u
cd "$(dirname "$0")/.." || exit 1
export DEVICE=alioth

MINUTES=${1:-10}
say() { echo "battery-check: $*"; }

[ "$(./xl state)" = loader ] || { say "no run in progress (state: $(./xl state)); start one first"; exit 1; }

stamp=$(date +%H%M%S)
for ((i = 1; i <= MINUTES; i++)); do
	sleep 60
	./xl log >/dev/null 2>&1
	soc=$(grep -a "battery: SOC=" logs/loader.log 2>/dev/null | tail -1)
	shot=logs/$stamp-battery-$i.png
	if ./xl screenshot "$shot" >/dev/null 2>&1; then
		say "$(date +%H:%M:%S) $soc  -> $shot"
	else
		say "$(date +%H:%M:%S) $soc  (no screenshot: $(./xl state))"
	fi
	case "$soc" in
	*"charger=0"*) say "the charger is gone; the bolt in the status bar should be gone too" ;;
	esac
done

say "what the kernel read over the whole run:"
grep -a "battery: SOC=" logs/loader.log 2>/dev/null | tail -20
