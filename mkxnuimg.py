#!/usr/bin/env python3
"""
Pack a device's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

Which device, and therefore where the FD goes, is --device (or $DEVICE), and the values per
device are in DEVICES below. The UEFI pieces come from a Mu-Silicium tree built here
(Mu-Silicium/Build/<pkg>Pkg/, see devices/<device>/uefi/README.md) or, when there is none,
from devices/<device>/prebuilt/.

usage: mkxnuimg.py [-d device] [-p payload.bin] [-o out.img]

  --fd FILE --dtb FILE --prebuilt DIR   take these files instead of looking them up
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000

# Where each device's UEFI is to sit, and how big it is. From the device's own device tree and its
# Mu-Silicium package: the base is where the phone's firmware block leaves room for it, and the
# size must match the FD exactly (the shim copies it as one piece).
#   pkg  the Mu-Silicium package name, for Mu-Silicium/Build/<pkg>Pkg/
#   dtb  the file in the package's Resources/DTBs that this device boots with
DEVICES = {
    'poco-x5-pro': {'pkg': 'redwood', 'fd_base': 0x9FD00000, 'fd_size': 0x200000, 'dtb': 'redwood.dtb'},
    'alioth':      {'pkg': 'alioth',  'fd_base': 0x9FC00000, 'fd_size': 0x300000, 'dtb': 'alioth.dtb'},
}

ap = argparse.ArgumentParser()
ap.add_argument('-d', '--device', default=os.environ.get('DEVICE', 'poco-x5-pro'))
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
ap.add_argument('--fd', help='the UEFI FD to pack (instead of looking it up)')
ap.add_argument('--dtb', help='the DTB to append (instead of looking it up)')
ap.add_argument('--prebuilt', help='directory holding SILICIUM_UEFI.fd, device-tree.dtb, BootShim.bin, ramdisk, mkbootimg.py')
ap.add_argument('--shim', help='BootShim.bin to pack (instead of looking it up)')
ap.add_argument('--mkbootimg', help='mkbootimg.py to use (instead of looking it up)')
args = ap.parse_args()

if args.device not in DEVICES:
    sys.exit(f'mkxnuimg.py: no such device: {args.device} (known: {", ".join(DEVICES)})')
device = DEVICES[args.device]
FD_BASE, FD_SIZE = device['fd_base'], device['fd_size']
PREBUILT = args.prebuilt or os.path.join(HERE, 'devices', args.device, 'prebuilt')
if args.output is None:
    args.output = os.path.join(HERE, f'xnu-{args.device}.img')

fd_path = args.fd or os.path.join(MU, f'Build/{device["pkg"]}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': args.shim or os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': args.dtb or os.path.join(MU, 'Resources/DTBs', device['dtb']),
             'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': args.mkbootimg or os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': args.shim or os.path.join(PREBUILT, 'BootShim.bin'),
             'fd': args.fd or os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': args.dtb or os.path.join(PREBUILT, 'device-tree.dtb'),
             'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': args.mkbootimg or os.path.join(PREBUILT, 'mkbootimg.py')}
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
