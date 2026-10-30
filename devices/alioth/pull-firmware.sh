#!/bin/bash
# Redmi K40 (alioth) 的固件提取：TWRP 下挂载 vendor(dm) 与 modem 分区（fw_mnt=modem 分区）。
# 用法：手机在 TWRP 且 adb 连着时运行本脚本（新树顶层）。
# 与 redwood 的差异：Wi-Fi 是 QCA6390（固件在 modem 分区 qca6390/ 目录，非 wpss.*），GPU 是 a650。
set -e
cd "$(dirname "$(readlink -f "$0")")/../.."
pull() { # DIR SOURCE...
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not found"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}

# vendor 与 modem 分区在 TWRP 里需手动挂载（Android 下 /vendor/firmware 无权限）
adb shell 'mountpoint -q /mnt/vendor || mount -t ext4 -o ro /dev/block/dm-4 /mnt/vendor 2>/dev/null || mount -t ext4 -o ro $(ls /dev/block/bootdevice/by-name/vendor_* | head -1) /mnt/vendor'
adb shell 'mkdir -p /mnt/fw; mountpoint -q /mnt/fw || mount -t vfat -o ro $(readlink /dev/block/bootdevice/by-name/modem) /mnt/fw'

V=/mnt/vendor/firmware
I=/mnt/fw/image
pull fw-gpu $V/a650_gmu.bin $V/a650_sqe.fw $V/a650_zap.mdt $V/a650_zap.b00 $V/a650_zap.b01 $V/a650_zap.b02
pull fw-haptic $V/aw8697_haptic.bin
pull fw-audio $I/adsp.mdt $I/adsp.b00 $I/adsp.b01 $I/adsp.b02 $I/adsp.b03 $I/adsp.b04 $I/adsp.b05 $I/adsp.b06 $I/adsp.b07 $I/adsp.b08 $I/adsp.b09 $I/adsp.b10 $I/adsp.b11 $I/adsp.b12 $I/adsp.b13 $I/adsp.b14 $I/adsp.b15 $I/adsp.b16 $I/adsp.b17 $I/adsp.b18
# QCA6390：整目录拉（bdwlan.* 板级数据 + amss20.bin + m3.bin + regdb*）
mkdir -p fw-wifi && adb pull /mnt/fw/image/qca6390 fw-wifi/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-wifi/qca6390: $(ls fw-wifi/qca6390 | wc -l | tr -d ' ') files"
echo "pull-firmware.sh: done. Wi-Fi 还需要 fw-wifi/mac（本机 MAC，一行，冒号分隔十六进制）与 fw-wifi/network（Wi-Fi 名称和密码）"
