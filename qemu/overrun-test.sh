#!/bin/bash
# What the loader does with a kernel that promised to come back and does not (loader.c,
# OTHER_CPU_OVERRAN): load it in QEMU with `loader_return_after=30`, tell the kernel not to keep
# the promise (boot-arg loader_return_skip=1), and wait.
#
# Expect, in this order:
#
#   loader_return_after=30 was asked for, and not armed (loader_return_skip)
#   THE KERNEL DID NOT RETURN: it said 30 seconds, its log follows; the phone is restarted in 60 seconds
#   overrun 0 0                       ("result: overrun", what `xl run` sees)
#   ... 60 seconds ...
#   restarting the phone              (then the loader calls reset_system and the machine goes)
#
# In QEMU the staged log is empty (0 bytes): the kernel's own log into loader_log is fed by the
# hakios drivers, and none of them runs on a machine with no phone in it. On the phone it is the
# kernel's log as far as it got, the same one `oem log` shows during a run.
#
# usage: qemu/overrun-test.sh [seconds of QEMU timeout, default 260]
# env: QEMU_EFI, QEMU_VARS -- the aarch64 UEFI for this machine (default: AAVMF's 64 MB pair,
#      which is what QEMU's pflash takes; qemu-efi-aarch64's 2 MB QEMU_EFI.fd is refused)
cd "$(dirname "$0")/.." || exit 1
T=${1:-260}
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
[ -f "$K" ] || { echo "no kernel at $K -- ./xnu-build.sh first"; exit 1; }
export QEMU_EFI=${QEMU_EFI:-/usr/share/AAVMF/AAVMF_CODE.fd}
export QEMU_VARS=${QEMU_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}
[ -f "$QEMU_EFI" ] || { echo "no firmware at $QEMU_EFI (set QEMU_EFI and QEMU_VARS)"; exit 1; }
xnuloader/mkpayload.py -o qemu/payload.bin kernel="$K" \
	--cmdline "loader_return_after=30 loader_return_skip=1 bootcpu=1 cpus=2" || exit 1
QEMU_SMP=2 qemu/run.sh "$T" > qemu/overrun-test.log 2>&1
echo "qemu exit: $?"
echo "--- the lines that matter ---"
grep -n "THE KERNEL\|restarting the phone\|not armed\|loader_return_after expired\|did not start\|never arrived\|panic" qemu/overrun-test.log | tail -30
echo "--- the tail ---"
tail -15 qemu/overrun-test.log
