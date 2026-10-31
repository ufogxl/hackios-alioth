# HakiOS

English · [中文](README.zh.md) · [Українська](README.uk.md)

HakiOS runs the open-source XNU kernel (xnu-8796.121.2) with my own boot loader and my own drivers
on an Android phone with a Qualcomm SoC, and boots the stock iOS 16.5 user space on top of it:
launchd, backboardd, SpringBoard, the apps. The same thing runs in QEMU (`virt`, aarch64) on a
Linux PC.

Nothing is flashed. The loader goes into RAM through `fastboot boot`, the system lives in RAM for
the length of a session, and a reboot brings the phone back to its own Android. This is a research
bench, not a ROM.

The first phone is the **POCO X5 Pro 5G** (`redwood`, Snapdragon 778G). The kernel knows nothing
about it in advance: every address, interrupt, power rail and firmware comes from the phone's own
device tree and its own Android, so a port to another Qualcomm phone is a new directory under
`devices/`, not a rewrite.

## What is in the repository

Only my code and my data:

- the kernel: a patch to Apple's open-source XNU (`xnu/hakios.patch`, APSL 2.0) and my driver
  files (`xnu/files/`), laid over a fresh clone by `xnu/apply.sh`;
- the boot loader (`xnuloader/`), my programs and libraries (`userland/src/`), tools (`tools/`),
  build and run scripts;
- `prebuilt/`: the loader and libraries built from this repository (not the kernel: `xnu-build.sh` makes
  that, and `xl` runs only the one built in this tree);
- `devices/poco-x5-pro/`: what one phone needs (its UEFI port, device tree, firmware script).

Not in the repository, never:

- any file of iOS: no binary, image, cache, font, picture or screenshot. You build the system
  image on your machine from the iOS 16.5 IPSW, which `sysimage/fetch.sh` downloads from Apple's
  public servers for you, without decrypting or circumventing anything;
- the phone's firmware (GPU, Wi-Fi, audio DSP, modem, haptics, cameras): it is the vendor's and
  is pulled from your own phone into `fw-*/`, which git ignores;
- keys, certificates, activation, or anything that would make the phone pass as an iPhone to
  Apple's services. Signing in to an Apple account is impossible here and not a goal.

## Status

| Works | Not yet |
|---|---|
| Lock screen, home screen, Control Center, gestures, the stock apps | Cellular: the modem boots, sees the network, the SIM is bound, the IPA data path and `en1` are in place; registration, the data session and the status bar bars are in progress (`docs/cellular.md`) |
| Touch screen, multitouch, power and volume buttons | Bluetooth |
| All 8 CPU cores, the big core at 2.4 GHz | Charging under this kernel |
| GPU rendering on Adreno (`gpu=1`, OpenGL ES through Mesa) | Metal (there is a shader compiler, no Metal) |
| Speakers and microphone (`adsp=1 sound=4 mic=4`) | Apple account: no Secure Enclave |
| Battery level in the UI (through the audio DSP, `adsp=1`) | |
| Vibration (`haptic=5`) | |
| Wi-Fi: radio, driver, network connection (`wifi=1`) | |
| Cameras in Camera.app (`cam=5`) | |
| `/private/var` kept across reboots (`var_store=1 ufs_write=1`) | |
| Sileo, Procursus packages, SSH into the phone | |

### The Redmi K40 (`alioth`) port

The second phone is the **Redmi K40** (`alioth`, Snapdragon 870, 12 GB). The kernel, loader and user
space are the same; what differs is collected in `devices/alioth/`. It boots the same iOS 16.5 user
space to a home screen with a wallpaper, widgets and working touch, on a 120 Hz panel.

| Works on the K40 | Not yet on the K40 |
|---|---|
| The kernel boots to SpringBoard; lock screen, home screen, stock apps | Wi-Fi: the QCA6390 is a PCIe part and `hakios.wifi.c` is still the WCN6750 version; the PCIe link has never trained |
| All 8 cores; `bootcpu=7` on the big core; EPSS frequency scaling | Microphone: the WCD938x hangs off SoundWire behind the ADSP and has not been done |
| Touch (FT3658U), multitouch, coordinates verified against the panel | Brightness (the slider moves, then nothing) and volume-up (not a TLMM pin; the firmware leaves it unpulled) -- two separate causes |
| GPU: a real A650 through freedreno, CoreAnimation rendered by it (`gl=1 adreno=22 adreno_keep=1`) | Metal: this port has none |
| Display: 120 Hz (`display_hz=120`); DCS brightness (`display_bl=N`) | Battery colour under the status-bar number |
| Wallpaper and widgets (`SYSTEM=1` in the image build: 407 launchd jobs instead of 25) | Camera |
| `/private/var` kept across reboots (`var_store=1 ufs_write=1`) | |
| ADSP firmware loaded (steps 1-2, read-only); audio and amp are wired but not initialised | |

Two things are worth knowing before reading the port notes, because each of them has cost a night:

- **The root file system is a 2.3 GB `userland/system.apfs` uploaded into the loader's RAM**, not a
  partition on the phone. A cold start empties that RAM, but the note file `xl` keeps beside the phone
  does not empty with it, so `xl run` silently uploads nothing and the kernel panics at
  `Process 1 exec of /sbin/launchd failed, errno 2`. That is never a bad image. `xl enter` now drops the
  note; a `fastboot boot` that sidesteps `xl enter` does not.
- **`mkxnuimg.py` defaults to `poco-x5-pro`**, whose UEFI lives at a different address. An image built
  without `DEVICE=alioth` will not boot here. A good one is 1,992,704 bytes; a POCO one is 1,470,464.

The day-by-day log of the port, including the hypotheses that turned out to be wrong, is `PROGRESS.md`;
its head carries an index. `devices/alioth/README.md` is the map of the port and
`devices/alioth/hardware.md` the register-level facts.

## Quick start in QEMU

Linux x86-64, about 12 GB of disk, 7-Zip 23 or newer, `lz4`, `qemu-system-aarch64`, `edk2-aarch64`.

```sh
git clone https://github.com/oosexgang/hakios-os.git hakios
cd hakios
sysimage/fetch.sh             # the iOS 16.5 IPSW from Apple's servers, the needed parts unpacked (7.4 GB)
REST=1 userland/build.sh      # the APFS images: userland/root.apfs and userland/system.apfs
mkdir -p logs
qemu/run-ios.sh 1200 -display gtk > logs/qemu.log     # a 20-minute session
```

The lock screen appears after two or three minutes. The mouse with the button held is a finger;
`H` is Home, `L` is the power button, `↑`/`↓` are volume.

## On the phone

A phone with a port (`devices/poco-x5-pro/`, POCO X5 Pro 5G), its boot loader unlocked, `adb` and
`fastboot` on the PC, the images from the previous section, and the phone's own firmware:

```sh
devices/poco-x5-pro/pull-firmware.sh    # GPU, Wi-Fi, audio/battery, haptics, modem, cameras -> fw-*/
./xl enter                          # Android -> fastboot -> our loader in RAM
./xl ramdisk userland/system.apfs   # the system image into the phone's RAM (~80 s), once per loader session
XL_SB_DELAY=8 XL_SECS=600 \
XL_CMDLINE="bootcpu=7 cpus=8 cpufreq=1 ufs=2 touchscreen=1 gpu=1 adsp=1 sound=4 wifi=1 haptic=5 modem=1" \
./xl run                            # a 10-minute session, the log goes to logs/
./xl reboot                         # back to Android
```

`tools/modem-cycle.sh "ARGS" SECS` does all of that in one command and returns the phone to Android.

The main kernel arguments (the full list is in `docs/ru/notes.md`):

| Argument | What it does |
|---|---|
| `cpus=8 bootcpu=7 cpufreq=1` | all CPU cores and their clocks |
| `touchscreen=1` | the touch screen |
| `gpu=1` | rendering on Adreno (an image built with `GL=1`, firmware in `fw-gpu/`) |
| `adsp=1 sound=4 mic=4` | the audio DSP: speakers, microphone; battery and charging come with it |
| `haptic=5` | the vibration motor |
| `wifi=1` | Wi-Fi (the network from `fw-wifi/network`) |
| `cam=5` | the cameras |
| `modem=1` | the modem beside the system: its firmware, file systems, IPA, SIM, NAS |
| `modem_step=N` | the modem step by step, for debugging (`docs/cellular.md`) |
| `var_store=1 ufs_write=1` | keep `/private/var` across reboots |

On the Redmi K40 (alioth, `devices/alioth/`) two of these differ, and the difference is not a
mistake: **`gpu=1` is refused there by design** -- that shorthand is the steps of the POCO's
Adreno 642L, and the SM8250's Adreno 650 has only the standalone steps 1 to 22. What works is

```sh
DEVICE=alioth XL_CMDLINE="gl=1 adreno=22 adreno_keep=1 touchscreen=1 cpufreq=1 bootcpu=7 cpus=8 rd=md0 battery=1"
```

(`adreno_keep=1` is what keeps the GPU for the system to draw with; without it step 22 is the end
and the display falls back to drawing without a GPU). The rest of that port is in
`devices/alioth/hardware.md`.

## Building from source

The kernel (Linux: theos with its cctools and `iPhoneOS16.5.sdk`; macOS: Xcode or cctools, the SDK
is fetched for you):

```sh
xnu/apply.sh             # XNU sources from Apple's GitHub + my changes -> xnu-src/
xnu-deps/setup.sh        # dependencies, tools and the SDK
./xnu-build.sh -j8 -k    # -> xnu-src/.../BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
```

The loader: `xnuloader/build.sh` (clang + lld-link, an EFI application without EDK2). The header
`xnuloader/identity.h`, the properties by which iOS tells which iPhone it is, is not kept in the
repository: `xnuloader/mkidentity.sh` makes it at build time from the device tree in your own IPSW
(one 34 KB file; LZFSE is decoded by `lzfse`, macOS's `compression_tool` or the Python `lzfse`
module, or the open lzfse library gets built). User space and the images: `REST=1 userland/build.sh`
(`MODEM=1 SYSTEM=1` put the modem's firmware into the system image). Details: `xnu/README.md`,
`docs/ru/notes.md`.

## Layout

| Directory | Contents |
|---|---|
| `xnu/` | the kernel patch and files; `apply.sh`, `export.sh`; a README about the kernel |
| `xnu/files/bsd/dev/hakios.*.c` | my drivers: display, touch, UFS, GPU, Wi-Fi, audio, modem (`hakios.mpss.c`), its packets (`hakios.ipa.c`, `hakios.cell.c`), SIM/NAS, haptics, cameras |
| `xnuloader/` | the boot loader (a UEFI application) |
| `userland/` | my programs and libraries (`src/`), the image build, launchd jobs |
| `sysimage/` | scripts that fetch and unpack the OS files from the IPSW (none of the files themselves) |
| `devices/poco-x5-pro/` | one phone: its UEFI port, device tree, prebuilt UEFI images, firmware script, tools |
| `devices/alioth/` | a second phone (Redmi K40): the same list for a different SoC, plus `hardware.md`, the register-level facts |
| `tools/` | APFS, firmware, QMI tests, run scripts |
| `qemu/` | running in QEMU |
| `docs/` | `notes.md`: the detailed notes (Russian); `cellular.md`: cellular; `air.md`: the GPU; `metal.md`: Metal |
| `prebuilt/` | the built loader and libraries (the kernel is not kept here) |
| `PROGRESS.md` | the port log, day by day, newest last; its head says how to read it |

## Devices

The kernel, loader and user space are shared. Everything about one phone lives in
`devices/<name>/`: its UEFI, device tree, firmware list and tools. The POCO's README
(`devices/poco-x5-pro/`) is the map of what a port to another Qualcomm phone must bring, and
`devices/alioth/` is that map followed a second time on a different SoC: compare the two before
starting a third.
The phone's drivers are the files `xnu/files/bsd/dev/hakios.*.c`; the head of each says where its
numbers came from.

## Cellular

The modem (the Hexagon MPSS) is brought up from XNU: firmware through TrustZone, shared memory,
SMP2P, GLINK, QRTR, QMI; its file systems are served from the UFS (read-only, writes stay in kernel
memory), TFTP and the service registry are mine. The IPA packet engine runs with its firmware,
clock and SMMU context, and the handshake with the modem goes through. The SIM is bound to the
subscription and NAS reads the signal. The current state and the whole story are in
`docs/cellular.md`.

## Independent, and Apple-free at the core

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**;
"iOS", "iPhone", "XNU" and "Apple" are named only to describe what the code works with. The core
kernel, the boot loader and the open tools build and run with nothing of Apple's:

```sh
make qemu-open      # the open kernel + a placeholder loader + a tiny raw-syscall user space, in QEMU
```

No IPSW, no iOS user space, no firmware. Booting the stock iOS user space (`make qemu`,
`make phone`) is an optional path that needs the IPSW and the phone's firmware, which you supply
from your own device.

## Licenses and the legal side

My code is under the license of the files it changes: the XNU patch under the Apple Public Source
License 2.0 (Apple publishes XNU under it; the terms are met: changes are published as source,
notices are kept), the rest as the file headers say. iOS, its images, Apple's trademarks and the
phone's firmware belong to their owners, are not in this repository and are not distributed by me;
everyone takes them from their own phone and their own IPSW. "iOS", "iPhone" and "Apple" are
named only to describe what the code works with. If you believe a file here infringes anyone's
rights, open an issue and I will remove it.

The detail is in separate files: `LEGAL.md` (what is and is not distributed, and the limits of
these statements), `COMPLIANCE.md` (how the APSL 2.0 and other terms are met, and the items still
to verify), `THIRD_PARTY_LICENSES.md` (every third-party component with origin and license), and
`NOTICE` (the short canonical list). None of this is legal advice.
