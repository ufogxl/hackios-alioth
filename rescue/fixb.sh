#!/system/bin/sh
# K40 rescue: when booted on slot a (accident state), restore slot b and reboot.
mkdir -p /tmp
LOG=/tmp/rescue.log
slot=$(getprop ro.boot.slot_suffix)
echo "rescue: slot=$slot" > $LOG
if [ "$slot" != "_a" ]; then
  echo "rescue: not slot a, noop" >> $LOG
  exit 0
fi
magic=$(dd if=/dev/block/by-name/boot_b bs=1 count=8 2>/dev/null)
if [ "$magic" != "ANDROID!" ]; then
  echo "rescue: boot_b invalid, stay in twrp" >> $LOG
  exit 0
fi
dd if=/gpt_region_b.bin of=/dev/block/sde bs=512 seek=4456400 conv=notrunc 2>>$LOG
sync
echo "rescue: switched to slot b, rebooting" >> $LOG
reboot
sleep 2
echo b > /proc/sysrq-trigger
