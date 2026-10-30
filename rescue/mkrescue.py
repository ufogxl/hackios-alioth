#!/usr/bin/env python3
"""The v4 rescue image, from the stock TWRP image.

    rescue/mkrescue.py twrp-alioth.img -o twrp-alioth-rescue-v4.img

The image is TWRP 3.7.1_12 for alioth (dl.twrp.me/alioth) with three changes, and nothing else:

  kernel   the __setup string "skip_initramfs" becomes "skip_initramfX" -- one byte -- so the
           argument ABL passes when it boots a slot from flash no longer skips our ramdisk;
  ramdisk  init.recovery.service.rc starts /fixwrap.sh instead of /system/bin/recovery;
  ramdisk  /fixwrap.sh, /fixb.sh and /gpt_region_b.bin are added; the first runs the second
           and then recovery, and the second puts slot b's GPT entries back and reboots.

The header, the padding and the AVB block are the stock image's, copied as they are. That is
also why this image cannot be tried with "fastboot boot": its AVB no longer matches what it
signs, and fastboot checks. It can only be flashed to a slot and left to ABL.

It did not work: PROGRESS.md, 2026-10-26 late night, "补丁未生效或有别的问题" -- the phone
still stopped at the logo. The files are kept for the next attempt, not as a rescue.
"""
import argparse
import gzip
import hashlib
import io
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# The stock TWRP the files below were taken from, and the kernel this produces. A different
# TWRP gives a different kernel; the script says so rather than pretending.
EXPECTED_TWRP = 'bf5bf3300da28abbef0493829a35fcd703424c1255172f5613789a7e2cfed2e3'
EXPECTED_KERNEL = '6cea071a165e5ce523bc07fb2138ee8bec423e0fb4e1715586b254aa298d9776'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def boot_parts(image):
    """(kernel offset, kernel size, ramdisk offset, ramdisk size), boot header v2."""
    if image[:8] != b'ANDROID!':
        sys.exit('mkrescue.py: not an Android boot image')
    version = struct.unpack('<I', image[40:44])[0]
    if version != 2:
        sys.exit(f'mkrescue.py: boot header v{version}; this is written for v2, which is what '
                 'TWRP 3.7.1_12 for alioth uses')
    page = struct.unpack('<I', image[36:40])[0]
    ksize = struct.unpack('<I', image[8:12])[0]
    rsize = struct.unpack('<I', image[16:20])[0]
    if page not in (2048, 4096, 8192, 16384):
        sys.exit(f'mkrescue.py: implausible page size {page}')
    koff = page
    roff = (koff + ksize + page - 1) // page * page
    return koff, ksize, roff, rsize


def patch_kernel(kernel):
    if b'skip_initramfX' in kernel:
        return kernel, None
    at = kernel.find(b'skip_initramfs')
    if at < 0:
        sys.exit('mkrescue.py: the kernel has neither "skip_initramfs" nor "skip_initramfX": '
                 'not the kernel these files were taken from')
    at += len(b'skip_initramfs') - 1                     # the final 's'
    return kernel[:at] + b'X' + kernel[at + 1:], at


def gunzip(data):
    return zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(data)


def cpio_parse(raw):
    """The entries of one newc archive, in order. Stops at TRAILER!!!; the rest must be zero."""
    entries, off = [], 0
    while True:
        if raw[off:off + 6] != b'070701':
            sys.exit(f'mkrescue.py: no cpio header at {off}')
        f = [int(raw[off + 6 + k * 8:off + 6 + k * 8 + 8], 16) for k in range(13)]
        ino, mode, uid, gid, nlink, mtime, size = f[:7]
        devmajor, devminor, rdevmajor, rdevminor = f[7:11]
        namesize = f[11]
        name = raw[off + 110:off + 110 + namesize - 1].decode('latin1')
        data_at = (off + 110 + namesize + 3) // 4 * 4
        if name == 'TRAILER!!!':
            if any(raw[data_at:]):
                sys.exit('mkrescue.py: the bytes after the cpio trailer are not padding')
            return entries
        entries.append(dict(ino=ino, mode=mode, uid=uid, gid=gid, nlink=nlink, mtime=mtime,
                            devmajor=devmajor, devminor=devminor,
                            rdevmajor=rdevmajor, rdevminor=rdevminor,
                            name=name, data=raw[data_at:data_at + size]))
        off = data_at + (size + 3) // 4 * 4


def cpio_emit(entries):
    out = io.BytesIO()

    def header(ino, mode, uid, gid, nlink, mtime, size, devmajor, devminor,
               rdevmajor, rdevminor, namesize):
        return b'070701' + b''.join(b'%08X' % v for v in (
            ino, mode, uid, gid, nlink, mtime, size, devmajor, devminor,
            rdevmajor, rdevminor, namesize, 0))

    for e in entries:
        name = e['name'].encode('latin1') + b'\0'
        out.write(header(e['ino'], e['mode'], e['uid'], e['gid'], e['nlink'], e['mtime'],
                         len(e['data']), e['devmajor'], e['devminor'], e['rdevmajor'],
                         e['rdevminor'], len(name)))
        out.write(name)
        out.write(b'\0' * (-out.tell() % 4))
        out.write(e['data'])
        out.write(b'\0' * (-out.tell() % 4))
    name = b'TRAILER!!!\0'
    out.write(header(0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name)))
    out.write(name)
    out.write(b'\0' * (-out.tell() % 4))
    return out.getvalue()


def gzip_bytes(data):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as g:
        g.write(data)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description='Rebuild the v4 rescue image from the stock TWRP image.')
    ap.add_argument('twrp', help='the stock TWRP for alioth, twrp-alioth.img')
    ap.add_argument('-o', '--output', default='twrp-alioth-rescue-v4.img')
    ap.add_argument('--kernel-out', default=None,
                    help='write the patched kernel here as well (48 MB; not needed to make the image)')
    args = ap.parse_args()

    image = open(args.twrp, 'rb').read()
    if sha256(image) != EXPECTED_TWRP:
        print('mkrescue.py: this is not the TWRP image these files were taken from\n'
              f'            got      {sha256(image)}\n'
              f'            expected {EXPECTED_TWRP}\n'
              '            carrying on, but the kernel will not match the recorded one',
              file=sys.stderr)

    koff, ksize, roff, rsize = boot_parts(image)

    kernel, at = patch_kernel(image[koff:koff + ksize])
    if args.kernel_out:
        with open(args.kernel_out, 'wb') as f:
            f.write(kernel)
    got = sha256(kernel)
    print(f'kernel: {ksize} bytes, "skip_initramfs" patched at {at if at is not None else "already"}, '
          f'sha256 {got}' + ('' if got == EXPECTED_KERNEL else '  (DIFFERS from the recorded kernel)'))

    entries = cpio_parse(gunzip(image[roff:roff + rsize]))
    by_name = {e['name']: e for e in entries}
    for need in ('init.recovery.service.rc',):
        if need not in by_name:
            sys.exit(f'mkrescue.py: the ramdisk has no {need}: not the ramdisk expected')

    with open(os.path.join(HERE, 'init.recovery.service.rc'), 'rb') as f:
        by_name['init.recovery.service.rc']['data'] = f.read()

    ino = max(e['ino'] for e in entries) + 1
    for name, mode, executable in (('fixb.sh', 0o100755, True),
                                   ('fixwrap.sh', 0o100755, True),
                                   ('gpt_region_b.bin', 0o100644, False)):
        if name in by_name:
            continue
        with open(os.path.join(HERE, name), 'rb') as f:
            data = f.read()
        by_name[name] = dict(ino=ino, mode=mode, uid=0, gid=0, nlink=1, mtime=0,
                             devmajor=0, devminor=0, rdevmajor=0, rdevminor=0,
                             name=name, data=data)
        entries.append(by_name[name])
        ino += 1

    packed = gzip_bytes(cpio_emit(entries))
    if len(packed) > rsize:
        sys.exit(f'mkrescue.py: the new ramdisk is {len(packed)} bytes, the image has room for '
                 f'{rsize}; the stock ramdisk would need to be rebuilt with more room')
    packed += b'\0' * (rsize - len(packed))
    print(f'ramdisk: {len(entries)} entries, {len(packed)} bytes in the image (slot {rsize})')

    out = bytearray(image)
    out[koff:koff + ksize] = kernel
    out[roff:roff + rsize] = packed
    with open(args.output, 'wb') as f:
        f.write(out)
    print(f'wrote {args.output}: {len(out)} bytes, sha256 {sha256(out)}')


if __name__ == '__main__':
    main()
