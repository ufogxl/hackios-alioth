# HakiOS — the Redmi K40 (`alioth`) port

English · [中文](README.zh.md)

HakiOS runs the open-source XNU kernel (xnu-8796.121.2) with its own boot loader and its own drivers
on an Android phone with a Qualcomm SoC, and boots the stock iOS 16.5 user space on top of it:
launchd, backboardd, SpringBoard, the apps. The same kernel runs in QEMU (`virt`, aarch64) on a Linux
PC, with nothing of Apple's.

Nothing is flashed. The loader goes into RAM through `fastboot boot`, the system lives in RAM for the
length of a session, and a reboot brings the phone back to its own Android. This is a research bench,
not a ROM.

**This repository is the port to the Redmi K40** (`alioth`, Snapdragon 870, 12 GB). It began as the
port to the POCO X5 Pro 5G (`redwood`, Snapdragon 778G), which is still the reference and still has
its own directory; the kernel, the loader and the user space are shared, and what differs is
collected under `devices/`.

## What is in the repository

Only this port's code and data:

- the kernel: a patch to Apple's open-source XNU (`xnu/hakios.patch`, APSL 2.0) and the driver files
  (`xnu/files/`), laid over a fresh clone by `xnu/apply.sh`;
- the boot loader (`xnuloader/`), the programs and libraries (`userland/src/`), the tools (`tools/`),
  the build and run scripts;
- `devices/alioth/`: everything that is one phone's — its UEFI, device tree, firmware script,
  `hardware.md` (the register-level facts and where each came from), `status` (what works, and how
  it was seen), `boot-args` (the kernel command line this phone needs);
- `devices/poco-x5-pro/`: the same for the first phone;
- `prebuilt/`: the loader and libraries built from this repository. Not the kernel: `xnu-build.sh`
  makes that and `xl` runs only the one built in this tree.

Never in the repository: any file of iOS (the system image is built on your machine from the iOS
16.5 IPSW that `sysimage/fetch.sh` downloads from Apple's servers, without decrypting anything); the
phone's firmware (pulled from your own phone into `fw-*/`, which git ignores); keys, certificates or
activation.

## Status

What works on this phone, one line per area, from `devices/alioth/status`. Only what has been seen on
the phone is `works`; written and not yet seen is `partial`, with a note. The table is generated —
`python3 tools/device-status.py update alioth README.md` — and must not be edited here.

<!-- status:alioth begin -->
| Area | State | Note |
|---|---|---|
| display | works | the panel's 60 Hz timing, or its 120 Hz one with display_hz=120; the picture stays at 60 frames a second, so the panel is not the limit. A frame is copied into the panel's memory in 2 ms and the copy is timed to the panel's own fetch (fb_fetch_align=2). A narrow band of what looked like tearing under the status bar turned out to be residue in the display controller from an earlier experiment and went away with a power cycle; how much tearing is left, in a state no experiment has written to, is not measured yet |
| touch | works | FT3658U, multitouch, coordinates checked against the panel |
| buttons | partial | power and volume-down come through the PMIC; volume-up is not a TLMM pin and the firmware leaves it unpulled, and the PON status word reads 0x4 |
| cpu | works | all eight cores; bootcpu=7 on the big core; EPSS frequency scaling |
| gpu | works | a real A650 through Mesa's freedreno, CoreAnimation rendered by it (gl=1 adreno=22 adreno_keep=1); no Metal |
| storage | works | /private/var survives a reboot with var_store=1 ufs_write=1 |
| audio | partial | the ADSP's firmware loads (steps 1-2, read-only); speaker, sound and microphone are wired but not initialised |
| battery | works | percentage, charging state and the charger flag from the PMIC, with no boot-arg |
| thermal | missing | no thermal driver; cpufreq=1 pins the top frequency instead |
| haptic | missing | the aw8697 answers on I2C and its waveform RAM reads back correctly, but f0 calibration and playback do not pass and no vibration has been felt |
| camera | missing | IMX582 and the rest |
| torch | missing | the flash module is on the PMIC's SPMI and has been read, not driven |
| wifi | missing | the QCA6390 is a PCIe endpoint behind pcie0 (stream ID 0x1c00) and the link has never trained on this SoC; hakios.wifi.c and wifi/lx in this tree are the POCO X5 Pro's (WCN6750, no PCIe) |
| bluetooth | missing | behind the same QCA6390, so behind the same link |
| cellular | missing | the modem is not started; hakios.mpss.c in this tree is the POCO X5 Pro's |
| network | missing | follows wifi and cellular; nothing of iOS's networking has been reached |
| usb | partial | the loader's USB answers xl during a run; nothing of the kernel's own |
| sensors | missing | no accelerometer, gyroscope or ambient light |
<!-- status:alioth end -->

The register-level facts behind all of this are in `devices/alioth/hardware.md`, and how each was
established — including the conclusions that were later overturned — is in `PROGRESS.md`.

## On the phone

A K40 with its boot loader unlocked, `adb` and `fastboot` on the PC, the images built (below), and
the phone's own firmware pulled:

```sh
adb pull /mnt/fw/image /some/where     # then: devices/alioth/pull-firmware.sh, which knows the paths
DEVICE=alioth ./xl enter               # Android -> fastboot -> our loader in RAM
./xl ramdisk userland/system.apfs      # the 2.3 GB system image into RAM (~90 s), once per loader session
./xl run                               # a session; the log goes to logs/
./xl stop                              # end it and leave the phone in the loader
```

`xl run` needs no arguments: the phone's own command line is `devices/alioth/boot-args`, and
`make phone SECS=600` runs the whole sequence (`xl enter`, the upload, the run) in one command,
adding `ARGS=...` to it.

**Two things cost a night each, and both look like a bad image:**

- `./xl enter` rebuilds the small image and **drops the note about the resident system image**, so the
  run after it panics with `Process 1 exec of /sbin/launchd failed, errno 2`. That is never a bad
  image: upload it again with `./xl ramdisk userland/system.apfs`.
- the small image is built by `userland/build.sh`, and **what the system is** is decided by the
  environment at that moment: `GL=1 REST=1 SYSTEM=1` gives the GPU path, the full set of launchd
  jobs, and the wallpaper. Rebuild it without those and the wallpaper is black, the dock is white
  and the widgets do not load.

## Boot arguments

Only what this phone needs. Everything below is a boot argument to the kernel; the full list is in
`docs/ru/notes.md`.

| Argument | Default | What it does |
|---|---|---|
| `gl=1 adreno=22 adreno_keep=1` | — | the GPU: step 22 of the Adreno 650's bring-up, and keep it for the system to draw with (`gpu=1` is the POCO's shorthand and is refused here) |
| `touchscreen=1` | — | the FT3658U |
| `bootcpu=7 cpus=8` | — | the kernel on a big core (the loader keeps its own and answers during a run), all eight cores |
| `cpufreq` | **on** | pin every cluster at its top row (1804 / 2419 / 3187 MHz). `cpufreq=0` leaves the boot loader's modest frequency, which is 3.4 times slower |
| `touch_poll_us` | **500** | how often the touch screen's interrupt line is looked at. The part reports 240 times a second, so 8000 (the old default) added up to 12 ms to every touch |
| `fb_fetch_align` | **2** | when the frame copy starts: 2 waits for the panel to be nearly done with its own fetch, 1 waits for the fetch to begin, 0 copies as the swap arrives |
| `fb_fetch_lead` | **600** | how many lines early `fb_fetch_align=2` starts |
| `fb_threads` | **8** | processors sharing the copy: 2.0 ms a frame, against 3.5 with three and 9–10 with one |
| `fbwidth` / `fbheight` | 1125 / 2436 | the picture's size, which is the iPhone's, not the panel's. `1080 2400` needs no scaling (1.3 ms) but iOS does not know that screen and falls back to a debug layout |
| `display_hz` | — | `60` / `90` / `120`: send the panel the timing-switch sequence for that rate |
| `fb_kick` | **0** | 1 turns autorefresh off and has this kernel start each frame itself. Measured: the panel then never fetches again, because autorefresh is the only thing that starts a fetch |
| `var_store=1 ufs_write=1` | — | keep `/private/var` across reboots |

Probes that only read, or that only run when asked: `display_step=N` and `display_te=N` (the display
controller and the panel's TE pin), `display_lines=1` (where in its frame the fetch is),
`fb_log_layers=N` (the swaps that are not one full-screen layer), `pcie_step=N` (the PCIe root
complex, for Wi-Fi), `cpuload`/`ddr`.

## Known limits

- **A frame is always copied.** Handing one over uncopied needs the display controller to read memory
  it can reach, and nothing in this boot chain maps more than the one frame the boot loader left: a
  new address is not in the SMMU's page table, and a wider stride (4352, which a GPU can write) runs
  past what is mapped and hangs the machine. Both need a stage-1 domain in `apps_smmu`, which is
  missing from the whole port, not just from the display.
- **The picture is 60 frames a second.** The panel's 120 Hz timing works and is stable; what holds the
  picture at 60 has not been established.
- **No Metal.** Rendering is OpenGL ES through Mesa; CoreAnimation draws through it.
- No sound, no network (neither Wi-Fi nor cellular), no camera, no vibration.
- The copy can still tear while the interface animates. `fb_fetch_align` reduced it; how much of it,
  in a state no experiment has written to, is not measured yet.

## Building from source

The kernel (Linux: theos with its cctools and `iPhoneOS16.5.sdk`; macOS: Xcode or cctools):

```sh
xnu/apply.sh             # XNU from Apple's GitHub + the changes -> xnu-src/
xnu-deps/setup.sh        # dependencies, tools and the SDK
./xnu-build.sh -j10      # -> xnu-src/.../BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
```

`./xnu-build.sh --clean` throws the object tree away first. It is not a convenience: a kernel that
linked stale objects looks on the phone exactly like broken code, and one evening went into that.
The build's last lines are harmless errors (export checks and symbol sets); the kernel's timestamp is
what says whether it was made.

The loader: `xnuloader/build.sh`. User space and the images:
`GL=1 REST=1 SYSTEM=1 userland/build.sh`. Details: `xnu/README.md`, `devices/alioth/README.md`,
`docs/ru/notes.md`.

## Layout

| Directory | Contents |
|---|---|
| `xnu/` | the kernel patch and files; `apply.sh`, `export.sh` |
| `xnu/files/bsd/dev/hakios.*.c` | the drivers: display, touch, UFS, GPU, Wi-Fi, audio, modem, haptics, cameras, PCIe. The head of each says where its numbers came from |
| `xnuloader/` | the boot loader (a UEFI application) |
| `userland/` | the programs and libraries (`src/`), the image build, the launchd jobs, `rc.jobs.*` (what a session runs) |
| `sysimage/` | the scripts that fetch and unpack the OS files from the IPSW (none of the files) |
| `devices/alioth/` | this phone: UEFI, device tree, firmware script, `hardware.md`, `status`, `boot-args` |
| `devices/poco-x5-pro/` | the first phone, the same list |
| `tools/` | APFS, firmware, QMI tests, the run scripts, `device-status.py` |
| `qemu/` | running in QEMU |
| `docs/` | `notes.md` (the boot arguments, in Russian), `cellular.md`, `air.md`, `metal.md` |
| `prebuilt/` | the built loader and libraries (the kernel is not kept here) |
| `PROGRESS.md` | the port log: why things are the way they are, and what was tried and rejected |

## Licenses and the legal side

HakiOS is an independent research project, not affiliated with or endorsed by Apple; "iOS",
"iPhone", "XNU" and "Apple" are named only to describe what the code works with. The kernel, the boot
loader and the open tools build and run with nothing of Apple's (`make qemu-open`).

The code is under the license of the files it changes: the XNU patch under the Apple Public Source
License 2.0 (changes published as source, notices kept), the rest as the file headers say. iOS, its
images, Apple's trademarks and the phone's firmware belong to their owners and are not in this
repository. The detail: `LEGAL.md`, `COMPLIANCE.md`, `THIRD_PARTY_LICENSES.md`, `NOTICE`. None of it
is legal advice.
