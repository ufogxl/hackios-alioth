#!/bin/bash
# One run of the backlight experiment, from whatever state the phone is in.
#
#   tools/backlight-test.sh [extra boot args...]
#
# What is being watched: the kernel publishes a stand-in AppleBacklight service (class names
# AppleBacklight and AppleARMBacklight, iokit/Kernel/MiniAppleBacklight.cpp). The Control Centre
# brightness slider goes SpringBoard -> backboardd -> CoreBrightness, and CoreBrightness has nowhere
# to put the level without such a service, which is why the slider does nothing. The kernel says,
# every twenty seconds,
#
#   AppleBacklight: 2 services published, the user client asked for 0 times
#
# and the number after "asked for" is the answer: 0 means CoreBrightness never found it (look up
# what it does look for), anything else means the client is open and the "selector ..." lines that
# follow are the protocol to implement.
#
# The script waits for the phone (loader, fastboot or Android), brings the loader up, makes sure
# the resident system image is in its RAM (after a reset it is gone; without it the kernel panics
# on `Process 1 exec of /sbin/launchd failed`), then runs: backlight service published, panel at
# 120 Hz, surface at 120. After HOME_WAIT seconds it takes a screenshot and reports. The run is
# left alive afterwards unless STOP=1.
#
#   HOME_WAIT   seconds to the home screen before the screenshot (default 150)
#   PHONE_WAIT  seconds to wait for the phone to be reachable (default 1800)
#   STOP=1      ask the kernel to come back at the end (the loader paints the screen green)
#   XL_SECS     when the kernel returns on its own (default 600 here, not xl's 0): a run nobody
#               ends is a phone nobody can reach -- on 31 October one with no deadline left the
#               loader's core inside the kernel and the phone answered nothing until someone held
#               the power button. 600 s is past the reading below (HOME_WAIT + 240 s), and the
#               loader now holds the kernel to it: still running at XL_SECS + 120 s, the phone is
#               reset by the loader itself (xnuloader/loader.c, OTHER_CPU_OVERRAN).
set -u
cd "$(dirname "$0")/.." || exit 1
export DEVICE=alioth

ARGS="gl=1 adreno=22 adreno_keep=1 touchscreen=1 bootcpu=7 cpus=8 rd=md0 battery=1 \
backlight=1 backlight_arm=1 display_hz=120 display_hz_delay=5 fb_hz=120 amp_step=2 ${*:-}"

HOME_WAIT=${HOME_WAIT:-150}
PHONE_WAIT=${PHONE_WAIT:-1800}

say() { echo "backlight-test: $*"; }

# ---- the phone: wait for it, and get the loader up ---------------------------------------------

deadline=$(( $(date +%s) + PHONE_WAIT ))
while :; do
	case "$(./xl state 2>/dev/null)" in
	loader)
		break ;;
	fastboot)
		# A slot marked unbootable makes ABL refuse every `fastboot boot` ("Load Error"), the
		# stock image and TWRP included. Clearing the flag is one command and touches no partition.
		if fastboot getvar slot-unbootable:b 2>/dev/null | grep -q ': yes'; then
			say "slot b is marked unbootable; fastboot boot would be refused, clearing it"
			fastboot --set-active=b >/dev/null 2>&1
		fi
		./xl enter >/dev/null 2>&1 && continue
		say "the loader did not come up from fastboot, retrying" ;;
	android)
		./xl enter >/dev/null 2>&1 && continue
		say "the loader did not come up from Android, retrying" ;;
	*)
		say "waiting for the phone (state: $(./xl state 2>/dev/null))" ;;
	esac
	if [ "$(date +%s)" -ge "$deadline" ]; then
		say "the phone is not reachable; power-cycle it (long-press power) and run this again"
		exit 1
	fi
	sleep 10
done

# ---- the resident system image ----------------------------------------------------------------

record=${XDG_RUNTIME_DIR:-/tmp}/darwin-hakios-phone.ramdisk
if [ ! -s "$record" ]; then
	say "no resident system image in the loader's RAM; uploading (about 90 s)"
	./xl ramdisk userland/system.apfs || { say "the upload failed"; exit 1; }
fi

# ---- the run ----------------------------------------------------------------------------------

stamp=$(date +%H%M%S)
log=logs/$stamp-backlight.log
started=$(date +%s)
say "running: $log"
say "boot arguments: $ARGS"
# XL_FOLLOW: the reading below happens at about HOME_WAIT + 240 s, before the run's own end (the
# kernel returns at XL_SECS=600 unless the caller says otherwise), and the run's log is only
# fetched when the run ends -- so without this the reading would have nothing to read. Every 20 s
# the loader's log comes back as $log.live, and that file (made by this run, one per fetch) is what
# the reading below reads. Without it, the 31 October run above read an empty file and said so.
# The boot arguments at the top of this file have to be *given* to xl: it reads them from XL_CMDLINE
# and falls back to its own "-v", which is not an argument at all. This line did not exist until
# 31 October, so every run this script made up to then used the loader's own smallest configuration
# -- `cmdline: debug=0x8 cpus=1 rd=md0 loader_return_after=0 ... -v` in the log -- with no GPU, no
# bootcpu=7, no battery, and no backlight service at all. That is why none of those runs could
# answer the question they were made for.
XL_FOLLOW=${XL_FOLLOW:-20} XL_SECS=${XL_SECS:-600} XL_CMDLINE="$ARGS" ./xl run > "$log" 2>&1 &
runpid=$!

# Give the kernel time to reach the window server, then look at the screen and at what it said.
# (A healthy run prints nothing here: the log fills at the end. Anything in it means the run is
# over already -- a panic, or a loader that hung.)
for ((i = 0; i < HOME_WAIT; i += 10)); do
	sleep 10
	[ -s "$log" ] && break
done
shot=logs/$stamp-backlight.png
# SCREENSHOT=0 skips it. A screenshot is a ten-megabyte transfer taken while the loader is also
# polling, and on 31 October the phone reset about a minute after one was asked for (10 MB going out
# with the run's own polling every two seconds); the watchdog is a likelier explanation than the log
# that was never fetched. The reading below is what this run is for -- the screen is checked in the
# runs that are about the screen.
if [ "${SCREENSHOT:-1}" = 1 ]; then
	# Thirty seconds, not xl's ten: the run's own polling holds the phone's lock every other second,
	# and a getvar that waits behind it is not a loader that has no display (which is what the
	# failure says).
	FB_TIMEOUT=30 ./xl screenshot "$shot" >/dev/null 2>&1 && say "screen: $shot"
fi

# The battery survey and the backlight probe in the SpringBoard shim (inject_sb.c) are said at
# 90 s / 95 s and 240 s / 245 s after SpringBoard starts -- and SpringBoard starts a minute or so
# into the run, not at t=0. So the log is read HOME_WAIT + 240 s in, which is past the second
# survey however late SpringBoard was; 130 s was too early by the margin that matters.
say "waiting for the SpringBoard surveys (240 s after SpringBoard's start)"
sleep 240
# Only a log this run wrote, never a leftover from an earlier one: a stale .live would answer with
# yesterday's numbers as if they were this run's, which is worse than no answer at all.
live=$(find logs -maxdepth 1 -name '*.live' -newermt "@$started" -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
echo
if [ -n "$live" ] && [ -s "$live" ]; then
	# The arguments the kernel actually got, not the ones this script meant to send: `xl` passes
	# XL_CMDLINE through, and until 31 October this script never set it (see the run line above).
	say "the kernel's command line (from $live):"
	grep -aE "cmdline:" "$live" | head -2
	echo
	# A run whose arguments never reached the kernel is not a reading (that is what 08:22 was): the
	# numbers below would be about a kernel with no backlight service and no bootcpu. Say it here,
	# before they are read, rather than leave it to be noticed later.
	if ! grep -aq "cmdline:.*backlight=1.*backlight_arm=1" "$live"; then
		say "WARNING: the kernel did not get backlight=1 backlight_arm=1 -- nothing below can answer the slider"
	fi
	if ! grep -aq "cmdline:.*bootcpu=" "$live"; then
		say "WARNING: no bootcpu= -- the loader handed its core to the kernel, so it cannot be asked anything while this runs"
	fi
	echo
	say "what the kernel said about the backlight (from $live):"
	grep -aE "AppleBacklight" "$live" | tail -25
	echo
	say "the panel and the surface:"
	grep -aE "display: (the panel was asked|TE:)|fb_hz" "$live" | tail -6
	echo
	# amp_step=2 asks the two CS35L41s on I2C who they are and writes nothing at all; it rides
	# along because a boot costs two minutes and this is the other half of "no sound".
	say "the amplifiers (amp_step=2, read-only):"
	grep -aE "amp: " "$live" | tail -15
	echo
	# The battery, as the kernel's own poll saw it over the run (one line a minute): whether the
	# status bar's number is moving with it is the other half of "the battery looks odd".
	say "the battery over the run:"
	grep -aE "battery: SOC=" "$live" | tail -8
	echo
	# Which methods the views that draw the battery have (the colour question, inject_sb.c).
	say "the battery view survey:"
	grep -aE "battery survey:" "$live" | tail -40
	echo
	# ... and what the view is made of: its subviews, and the colours each one is drawn in
	# (battery_dump_view, said once three seconds after the first level is set). The line that
	# says "fill ..." on a CAShapeLayer is the one the charging colour would be changed in.
	say "the battery view's tree (the colours it draws with):"
	grep -aE "battery view:" "$live" | tail -30
	echo
	# What iOS itself sees of the kernel's stand-in service, and where it sits (inject_sb.c,
	# backlight_probe): a match here with "asked for 0 times" in the kernel line above would put
	# the problem on CoreBrightness's side of the registry, not on the service.
	say "the backlight service as iOS sees it:"
	grep -aE "backlight probe:" "$live" | tail -10
else
	say "no live log yet; the run's own said:"
	tail -20 "$log"
fi

if [ "${STOP:-0}" = 1 ]; then
	./xl stop >/dev/null 2>&1 && say "asked the kernel to come back"
else
	say "the run is still up (pid $runpid); it returns by itself at ${XL_SECS:-600} s, ./xl stop ends it sooner"
fi

# Read the verdict:
#   2 ... asked for 0 times      the service is in the registry and nobody in iOS looks for it
#   2 ... asked for N times      CoreBrightness opens it; read the "selector"/"CLIENT OPENED" lines
