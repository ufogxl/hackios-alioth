# The kernel

Our changes to XNU `xnu-8796.121.2` (the kernel of iOS 16.5, Darwin 22.5.0):

- `files/`: every file of the work, whole, at its path in the XNU tree -- the 72 of Apple's that we
  changed and the 113 that are ours. Whole files rather than a patch: applying is a copy and cannot
  half-fail, and what is in the repository is readable as source;
- `apply.sh`: fetches the sources from Apple's GitHub and copies `files/` over them;
- `export.sh`: the reverse, run before committing.

It builds on Linux with stock clang (no macOS, no Xcode) and on macOS with Xcode.

## Build

Linux (Arch): `sudo pacman -S --needed base-devel git curl clang lld python perl xz`, and
[theos](https://theos.dev/docs/installation-linux) in `~/theos` (or `$THEOS`) for its cctools
(`ld64`, `nm`, `otool`, `strip`) and `sdks/iPhoneOS16.5.sdk`. macOS: Xcode and MacPorts coreutils;
`xnu-deps/setup.sh` lays out `xnu-deps/theos-mac` itself.

Debian/Ubuntu: `sudo apt install build-essential git curl clang lld python3 perl xz-utils`, and theos
the same way.

```sh
xnu/apply.sh             # the xnu-8796.121.2 tag from apple-oss-distributions/xnu + our changes -> xnu-src/
xnu-deps/setup.sh        # dependencies, mig/unifdef/tcsh, the SDK overlay, two libraries, dsymutil
./xnu-build.sh -j8 -k    # about a minute on 12 threads
```

`make kernel` does the same three in order, and runs `setup.sh` when `xnu-deps/host/bin/dsymutil` is
not there yet (that file, and not `xnu-clang`, is the mark of a finished setup: `xnu-clang` is
tracked in the repository).

The kernel: `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
`./xl run` and `qemu/run-ios.sh` take it from there and nowhere else: `prebuilt/` no longer carries a
copy, so a run without a build of this tree refuses instead of quietly running an old one.

Things to know:

- `-k` is required: the build always ends with an error in `check_all_exports` (export lists), after
  the kernel has been linked. Look at the file's date, not at make's exit code.
- No spaces in the repository's path.
- After changing kernel headers that the two external libraries see: `xnu-deps/setup.sh --rebuild-libs`.
- Apple's DriverKit generator (`iig`) is closed; `xnu-deps/iig-impl/` holds stubs generated from
  the linker's undefined symbols (`xnu-deps/gen-iig-stubs.py`).
- Edit `xnu-src/xnu-8796.121.2` directly: it is a git tree on branch `hakios`, one commit behind
  Apple's tag `xnu-8796.121.2` (`c54f35ca`). `xnu/export.sh` regenerates the whole of `files/` from
  what differs from that tag, so it picks up every change at once -- there is no patch to keep in
  step, and nothing to bring up to date by hand.

## What is added

| Files | What |
|---|---|
| `osfmk/arm64/gic_v3_irq.c`, `generic_arm64_support.c`, `psci_smp.c`, `loader_return.c` | the platform: GICv3, the timer, several CPUs through PSCI, the return to the loader with the log, the hypervisor watchdog |
| `osfmk/minicrypto/` | a replacement for corecrypto (MD5, SHA, HMAC, DRBG, AES) |
| `iokit/Kernel/GenericARMPE.cpp`, `MiniIOSurface.*`, `MiniIOMobileFramebuffer.cpp`, `MiniAppleKeyStore.cpp` | the platform expert and replacements for closed drivers: surfaces, the display, the key store |
| `bsd/miscfs/apfs/`, `bsd/miscfs/ramfs/` | read-only APFS and a writable RAM file system |
| `bsd/kern/kern_minimac.c`, `libkern/amfi/` | AMFI and Sandbox stubs |
| `bsd/dev/hakios.input.c`, `hakios.goodix.c` | input: `/dev/hakios-touch`, the Goodix GT9916 touch controller |
| `bsd/dev/hakios.cpufreq.c` | CPU clocks (Qualcomm EPSS) |
| `bsd/dev/hakios.net.c`, `hakios.virtio_net.c` | the `en0` network interface |
| `bsd/dev/hakios.adreno.c`, `iokit/Kernel/hakios.gpu.cpp` | the Adreno 642L GPU |
| `bsd/dev/hakios.wifi.c`, `hakios.wlfw.c` | Wi-Fi (WCN6750): shared memory, the radio's DSP, the driver |
| `bsd/dev/hakios.qmi.c`, `hakios.nas.c` | QMI and the NAS client |
| `bsd/dev/hakios.mpss.c` | the modem: firmware, file systems, IPA, SIM, registration |
| `bsd/dev/hakios.audio.c`, `hakios.adsp.c`, `hakios.speaker.c`, `hakios.mic.c` | sound through the audio DSP; the battery comes through it too |
| `bsd/dev/hakios.haptic.c`, `hakios.camera.c`, `hakios.ufs.c` | the vibration motor, the cameras, the UFS storage |
| `bsd/pthread/pthread_kext_static.c` | the pthread kext linked into the kernel |

The patch also fixes what keeps open XNU from building and running outside Apple: the makefiles
on Linux, the `BCM2837` configuration it is based on, bugs in abandoned code.

License: a derivative work of XNU, Apple Public Source License 2.0 (`LICENSE.APSL` at the top).
