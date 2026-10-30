#!/bin/bash
# Refresh prebuilt/ from what is built in this tree: the loader, our user-space programs and the pieces
# of the phone's UEFI. With them somebody without the cross toolchain can still make the images and run
# the system (QEMU or the phone); see README.md.
# The kernel is not among them. It was, and the copy went stale behind the sources while xl fell back to
# it whenever a build failed -- so a broken tree ran an old kernel with nothing saying so (see xl).
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
MU=Mu-Silicium
mkdir -p prebuilt/userland devices/poco-x5-pro/prebuilt
[ -f xnuloader/loader.efi ] && cp xnuloader/loader.efi prebuilt/loader.efi || echo "update-prebuilt.sh: no xnuloader/loader.efi here (lld-link); prebuilt/loader.efi kept"
cp userland/hello prebuilt/userland/
for f in init child stage0 hello_dyn svc_test surf_test rwtouch mg_test cpu_test sound_test libhakios.dylib libhakios.sb.dylib \
	libhakios.media.dylib libhakios.app.dylib libhakios.tweaks.dylib HakiosAudio net_test; do
	cp userland/obj/$f prebuilt/userland/
done
if [ -f $MU/Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd ]; then
	cp $MU/Build/redwoodPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd $MU/BootShim/BootShim.bin $MU/Resources/DTBs/redwood.dtb \
		$MU/Resources/ramdisk $MU/Resources/Scripts/mkbootimg.py devices/poco-x5-pro/prebuilt/
fi
{
	echo "Built $(date '+%Y-%m-%d %H:%M') from this tree."
	echo
	sha256sum prebuilt/loader.efi prebuilt/userland/* devices/poco-x5-pro/prebuilt/* 2>/dev/null
} > prebuilt/SHA256SUMS
ls -la prebuilt prebuilt/userland devices/poco-x5-pro/prebuilt
