# HakiOS —— 红米 K40（`alioth`）移植

[English](README.md) · 中文

HakiOS 把苹果开源的 XNU 内核（xnu-8796.121.2）连同自己的引导程序和自己的驱动，跑在采用高通芯片的
Android 手机上，并在其上启动原版 iOS 16.5 的用户空间：launchd、backboardd、SpringBoard、各个 App。
同一个内核也能在 Linux 电脑的 QEMU（`virt`，aarch64）里运行，且不需要苹果的任何文件。

**什么都不刷。** 引导程序通过 `fastboot boot` 进内存，系统在一次会话期间只活在内存里，重启就回到手机
自己的 Android。这是研究用的台架，不是 ROM。

**这个仓库是红米 K40**（`alioth`，骁龙 870，12 GB）的移植。它起始于 POCO X5 Pro 5G（`redwood`，
骁龙 778G）的移植——那台是初号机，仍是参考，也仍保留着自己的目录；内核、引导程序和用户空间是共用的，
不同的部分都收在 `devices/` 下。

## 仓库里有什么

只有这一次移植的代码和数据：

- 内核：对苹果开源 XNU 的补丁（`xnu/hakios.patch`，APSL 2.0）和驱动文件（`xnu/files/`），由
  `xnu/apply.sh` 铺到一份全新的克隆上；
- 引导程序（`xnuloader/`）、程序与库（`userland/src/`）、工具（`tools/`）、构建与运行脚本；
- `devices/alioth/`：属于这台手机的一切——UEFI、设备树、固件脚本、`hardware.md`（寄存器级事实及每条
  的出处）、`status`（什么能用，以及是怎么看到的）、`boot-args`（这台手机需要的命令行）；
- `devices/poco-x5-pro/`：初号机的同一份清单；
- `prebuilt/`：由本仓库构建出的引导程序和库。**不含内核**——内核由 `xnu-build.sh` 现场构建，`xl` 只跑
  本树构建出来的那一个。

仓库里**永远不会有**：任何 iOS 文件（系统镜像在你的机器上从 iOS 16.5 的 IPSW 构建，`sysimage/fetch.sh`
从苹果公开服务器下载，不解密任何东西）；手机固件（从你自己的手机拉到 `fw-*/`，git 忽略）；密钥、证书、
激活相关的一切。

## 现状

下面这张表来自 `devices/alioth/status`，一行一个部件。只有**在手机上亲眼看到过**的才写 `works`；
写了代码但还没验证的是 `partial`，并注明缺什么。**这张表是生成的**
（`python3 tools/device-status.py update alioth README.md`），不要在这里手改。

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
| wifi | partial | the link trains to L0 and the QCA6390 (17cb:1101) enumerates behind pcie0: four rails voted through RPMh, PERST# released (it is active low), the LTSSM enabled with one plain write. Its configuration space, BAR0 (1 MB at 0x60400000), the half-BAR register window and its BHI block (version 1.2, exec env 0 = PBL) all read and write, and fw/qca6390/amss20.bin is in the image. What does not work yet is the first firmware download: BHI takes the descriptor, the endpoint attempts the transfer and fails (BHI_STATUS stays 0, ERRCODE 0xef128100, its error words echo our IMGADDR and IMGSIZE), so the secondary boot loader never starts and no MHI, QMI or ath11k exists. The driver is hakios.pcie.c, steps 1 to 43, each behind a boot argument |
| bluetooth | missing | behind the same QCA6390, so behind the same link |
| cellular | missing | the modem is not started; hakios.mpss.c in this tree is the POCO X5 Pro's |
| network | missing | follows wifi and cellular; nothing of iOS's networking has been reached |
| usb | partial | the loader's USB answers xl during a run; nothing of the kernel's own |
| sensors | missing | no accelerometer, gyroscope or ambient light |
<!-- status:alioth end -->

寄存器级的依据在 `devices/alioth/hardware.md`；每条结论是怎么来的、以及哪些后来被推翻，在 `PROGRESS.md`。

## 在手机上跑

一台已解锁 BL 的 K40、电脑上有 `adb` 和 `fastboot`、镜像已经构建好（见下）、手机固件已拉取：

```sh
devices/alioth/pull-firmware.sh        # 按这个手机自己的路径拉：GPU、Wi-Fi、音频、振动、相机 -> fw-*/
DEVICE=alioth ./xl enter               # Android -> fastboot -> 我们的 loader 进内存
./xl ramdisk userland/system.apfs      # 2.3 GB 的系统镜像进内存（约 90 秒），每次 loader 会话一次
./xl run                               # 跑一次会话，日志在 logs/
./xl stop                              # 结束，把手机留在 loader
```

`xl run` **不需要任何参数**：这台手机自己的命令行在 `devices/alioth/boot-args`。`make phone SECS=600`
把整条链路（`xl enter`、上传、运行）合成一条命令，`ARGS=...` 追加参数。

**两个各值一晚的坑，现象都像"镜像坏了"：**

- `./xl enter` 会重建小镜像，并**丢掉"resident 系统镜像是谁传的"那条记录**，于是下一次跑会 panic 在
  `Process 1 exec of /sbin/launchd failed, errno 2`。**这从来不是镜像坏了**，补传一次即可：
  `./xl ramdisk userland/system.apfs`。
- 小镜像由 `userland/build.sh` 构建，而**系统长什么样取决于构建那一刻的环境变量**：
  `GL=1 REST=1 SYSTEM=1` 才有 GPU 渲染路径、全量 launchd 作业和壁纸。少了这几个重建，壁纸会是黑的、
  停靠栏纯白、小组件不加载。

## 启动参数

只列这台手机会用到的。全部参数见 `docs/ru/notes.md`。

| 参数 | 默认 | 作用 |
|---|---|---|
| `gl=1 adreno=22 adreno_keep=1` | — | GPU：Adreno 650 引导的第 22 步，并保留给系统渲染用（`gpu=1` 是 POCO 的简写，这台机器按设计拒绝） |
| `touchscreen=1` | — | FT3658U 触摸 |
| `bootcpu=7 cpus=8` | — | 内核跑在大核（loader 留在自己核上，运行期间还能应答），八核全开 |
| `cpufreq` | **开** | 每个簇钉在最高频（1804 / 2419 / 3187 MHz）。`cpufreq=0` 就保留 bootloader 那个"modest"频率，慢 3.4 倍 |
| `touch_poll_us` | **500** | 多久看一眼触摸的中断线。芯片本身 240 Hz 上报，原来的 8000 等于每次触摸多等最多 12 ms |
| `fb_fetch_align` | **2** | 上屏拷贝何时起笔：2 = 等面板这一帧快取完，1 = 等它刚开始，0 = 来一帧写一帧 |
| `fb_fetch_lead` | **600** | `fb_fetch_align=2` 提前多少行起笔 |
| `fb_threads` | **8** | 分担拷贝的处理器数：每帧 2.0 ms，三线程 3.5 ms，单线程 9–10 ms |
| `fbwidth` / `fbheight` | 1125 / 2436 | 画面的尺寸，是 iPhone 的，不是面板的。`1080 2400` 不用缩放（1.3 ms），但 iOS 认不出那个尺寸，会退回调试排版 |
| `display_hz` | — | `60` / `90` / `120`：把面板对应档位的切换序列发下去 |
| `fb_kick` | **0** | 1 = 关掉 autorefresh，由内核自己启动每一帧。实测：面板此后就不再取帧，因为 **autorefresh 是唯一能启动取帧的东西** |
| `var_store=1 ufs_write=1` | — | `/private/var` 跨重启保留 |

只读、或者只在被点名时才动的探针：`display_step=N` 与 `display_te=N`（显示控制器、面板 TE 脚）、
`display_lines=1`（取帧到了第几行）、`fb_log_layers=N`（不是"单层全屏"的那些 swap）、
`pcie_step=N`（PCIe 根复合体，Wi-Fi 用）、`cpuload` / `ddr`。

## 已知边界

- **每一帧都是拷过去的。** 不拷贝直接上屏，需要显示控制器能读到它够得着的内存，而这条引导链里没有任何
  一环映射的超过 bootloader 留下的那一块帧：换地址不在 SMMU 的页表里，换更宽的 stride（4352，GPU 能写
  的那种）会越过映射边界并把机器挂死。两条都缺 `apps_smmu` 的 stage-1 域——**缺在整条引导链上，
  不只是显示**。
- **画面是 60 帧/秒。** 面板的 120 Hz 档位是通的、也稳；把它按在 60 的原因还没查实。
- **没有 Metal。** 渲染走 Mesa 的 OpenGL ES，CoreAnimation 通过它画。
- 没有声音、没有网络（Wi-Fi 和蜂窝都没有）、没有相机、没有振动。
- 界面动起来时拷贝仍可能撕裂。`fb_fetch_align` 减少了多少，在"没被实验写过"的干净状态下**还没测过**。

## 从源码构建

内核（Linux：theos 及其 cctools 与 `iPhoneOS16.5.sdk`；macOS：Xcode 或 cctools）：

```sh
xnu/apply.sh             # 从苹果的 GitHub 取 XNU + 本仓库的改动 -> xnu-src/
xnu-deps/setup.sh        # 依赖、工具和 SDK
./xnu-build.sh -j10      # -> xnu-src/.../BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
```

`./xnu-build.sh --clean` 会先扔掉整个对象树。这不是顺手：用旧对象链接出来的内核在手机上**和代码写坏了
一模一样**，已经栽过一晚。构建最后那几条报错（export 检查、symbol set）是无害的，**判断有没有编出来看
内核文件的时间戳**，不是看 make 的退出码。

引导程序：`xnuloader/build.sh`。用户空间与镜像：`GL=1 REST=1 SYSTEM=1 userland/build.sh`。
细节见 `xnu/README.md`、`devices/alioth/README.md`、`docs/ru/notes.md`。

## 目录结构

| 目录 | 内容 |
|---|---|
| `xnu/` | 内核补丁与文件；`apply.sh`、`export.sh` |
| `xnu/files/bsd/dev/hakios.*.c` | 驱动：显示、触摸、UFS、GPU、Wi-Fi、音频、基带、振动、相机、PCIe。每个文件头都写明它的数字从哪来 |
| `xnuloader/` | 引导程序（一个 UEFI 应用） |
| `userland/` | 程序与库（`src/`）、镜像构建、launchd 作业、`rc.jobs.*`（一次会话跑什么） |
| `sysimage/` | 从 IPSW 抓取并解包 OS 文件的脚本（不含任何那个文件本身） |
| `devices/alioth/` | 这台手机：UEFI、设备树、固件脚本、`hardware.md`、`status`、`boot-args` |
| `devices/poco-x5-pro/` | 初号机，同一份清单 |
| `tools/` | APFS、固件、QMI 测试、运行脚本、`device-status.py` |
| `qemu/` | 在 QEMU 里跑 |
| `docs/` | `notes.md`（启动参数全表，俄语）、`cellular.md`、`air.md`、`metal.md` |
| `prebuilt/` | 构建好的引导程序与库（内核不放在这里） |
| `PROGRESS.md` | 移植日志：为什么是现在这样，试过什么、否决了什么 |

## 许可与法律

HakiOS 是一个独立研究项目，与 Apple Inc. 无关联、未获其认可；"iOS"、"iPhone"、"XNU"、"Apple" 这些名字
只用来描述代码针对的对象。内核、引导程序和开放工具用不到苹果的任何东西也能构建运行（`make qemu-open`）。

代码遵循它所改动文件的许可：XNU 补丁以 Apple Public Source License 2.0 发布（改动以源码形式公开、
声明保留），其余按各文件头。iOS、其镜像、Apple 的商标以及手机固件属于各自所有者，**不在本仓库**
也不分发。细节见 `LEGAL.md`、`COMPLIANCE.md`、`THIRD_PARTY_LICENSES.md`、`NOTICE`。这些都不是法律意见。
