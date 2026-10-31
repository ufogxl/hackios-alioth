# Device: Redmi K40 (alioth)

骁龙 870（SM8250-AC），12 GB，代号 alioth。BL 已解锁。

本目录是 alioth 在**当前这棵树**里的全部内容。下面这张表按实测更新过一次：
原表写于 main 重组后的迁移初期，其中「内核驱动 ❌ 未开始」「UEFI ⏳ 未迁移」已不成立。

| 项 | 状态 | 在哪 |
|---|---|---|
| 内核驱动 | ✅ 已在真机跑起来 | `xnu/files/bsd/dev/hakios.{adreno,adsp,amp,display,goodix,haptic,buttons,cpufreq}.c` |
| loader | ✅ 真机验证 | `xnuloader/loader.c` 的 `platform_alioth`，`xnuloader/build.sh` |
| UEFI | ✅ 二进制入库 | `prebuilt/SILICIUM_UEFI.fd`（Mu-Silicium aliothPkg，0x300000 @ 0x9FC00000） |
| 打包 | ✅ 一条命令 | `./mkxnuimg.py -d alioth -p <payload> -o <img>` |
| 固件 | ✅ 从真机 TWRP 提取，**不入库** | 顶层 `fw-audio` / `fw-gpu` / `fw-haptic`；重新提取用 `pull-firmware.sh` |
| soc.args | ✅ 由真机 DTB 生成 | `soc.args` |
| Wi-Fi | ❌ 未做 | QCA6390 是 PCIe 外挂芯片；`hakios.wifi.c` 仍是 WCN6750 版本 |
| 麦克风 | ❌ 未做 | WCD938x 走 SoundWire，依赖 ADSP，无人做过 |

## 真机关键参数

来源：`hardware.md`、`dump/` 里的原厂备份、以及真机日志。

- 帧缓冲 `0x9C000000`（36 MB），面板 1080×2400，行字节 4320
- UEFI FD `0x9FC00000` / `0x300000`；`PAYLOAD_BASE = 0xF0000000`
- GIC `0x17A00000` / `0x17A60000`；8 核，0-3 是 A55（`small_cores=0xf`）
- 触摸供电：PM8150A **L1 @ 1.8 V**（`vcc_i2c`）与 **L10 @ 3.0 V**（`vdd`），
  见 `hakios.goodix.c` 的 `io_supply_on()`，与真机 FDT 一致
- 音量上 = **PM8150 的 SPMI GPIO6**（`gpios = <&pm8150_gpios 6 GPIO_ACTIVE_LOW>`，不是 TLMM GPIO6；
  固件没把它配成带上拉的输入时 `hakios.buttons.c` 故意不碰它，所以现在按了没反应）；
  音量下 = PON 的 RESIN_N；电源 = PON 的 KPDPWR。面板 reset = gpio12；TE = gpio66
  （默认 60.666 Hz；`display_hz=120` 切档后实测 **121.333 Hz**，见文末）
- GPU 要用 **`gl=1 adreno=22 adreno_keep=1`**：`gpu=1` 在这台按设计被拒（SM8250 只有独立 1–22 步），
  少了 `adreno_keep=1` 第 22 步做完就收摊、显示退回软件路径

## 启动

```sh
./mkxnuimg.py -d alioth -p <payload.bin> -o boot.img   # UEFI + BootShim + payload
./xl enter                                             # 手机进 loader（从 Android 或 fastboot）
./xl run                                               # 上传内核、运行、取回日志
```

**`mkxnuimg.py` 的默认设备是 `poco-x5-pro`**，而这台手机的 UEFI 在 `0x9FC00000`、大小
`0x300000`（POCO 是 `0x9FD00000` / `0x200000`），刷错镜像进不了 loader。**凡是要打镜像的命令都要
带 `DEVICE=alioth`**，包括 `xl enter` / `xl reload` / `make phone`——`xl` 自己确实不读 `DEVICE`，
但这些命令内部会调 `mkxnuimg.py`，而它读（镜像大小是 1,992,704 字节；打成 POCO 的会是 1,470,464）：

```sh
DEVICE=alioth ./xl enter        # 从 Android/fastboot 把 loader 拉起来（内部要打 USB 镜像）
DEVICE=alioth ./xl run
DEVICE=alioth make phone        # 或者一条命令：进 loader、上传系统镜像、跑内核、取日志
```

`make phone` 走 `tools/modem-cycle.sh`：它先 `xl ramdisk userland/system.apfs` 把 2.4 GB 的系统
镜像放进手机内存（一次性，约 90 秒），再 `xl run` 跑内核；**loader 重启过一次之后必须先做这一步**，
否则根分区是空的，内核会以 `Process 1 exec of /sbin/launchd failed, errno 2` panic。

`xl` 现在自己知道这件事：`xl enter` 只要这一次真的把 loader 引导起来，就删掉
`$PHONE.ramdisk` 那条记录（resident 镜像跨 run 存活、不跨 loader 重启）。所以整机重启之后
`xl run` 会提示 "nothing here recorded who uploaded the resident image"，而不是拿着旧记录
什么都不传。**见到 `launchd ... errno 2` 先想这条，不要去怀疑镜像本身。**

`xl` 只调 PATH 里的 `fastboot` / `adb`。在 Linux 上直连手机时用系统的即可；在 WSL 里
系统的 fastboot 看不到 USB 设备，需要 `/usr/local/bin/fastboot` 那个转发到 Windows 的包装器。

### `fastboot boot` 报 `Failed to load/authenticate boot image: Load Error`

连 **TWRP、原版 boot.img 都拒绝**，不是镜像坏了，是 **ABL 把当前槽位标成了 unbootable
（`slot-unbootable:b: yes`）**：这个状态下 ABL 不加载任何 RAM 镜像，一律报这个错（看上去像
AVB 校验失败，其实是槽位标志）。实测原因链：手机被 `xl reload` 冷重启回 Android，Android
起不来被标记失败 → 落到 fastboot。修法就一行，不刷任何分区：

```sh
fastboot --set-active=b            # 立刻清掉 unbootable，retry-count 回到 7
fastboot getvar slot-unbootable:b  # 应为 no
DEVICE=alioth ./xl enter           # 然后 boot 就正常了
```

判断是不是这个原因：`fastboot getvar slot-unbootable:b` 是 `yes` 就直接修，别去怀疑镜像
（同一份 `xnu-usb.img` 修标志前 100% `Load Error`，修完立刻 `Booting OKAY`）。

另外注意 `xl enter` 的镜像和 `xl reload` 一样要带 `DEVICE=alioth`：不带会按 `poco-x5-pro`
打包，FD 落在 `0x9FD00000`，同样进不去。

## 系统镜像的作业表：壁纸和小组件在不在，全看这里

`build.sh` 默认只把 `IOS_JOBS` 那一串 **25 个**作业写进镜像（`notifyd` … `SpringBoard` … `wifid`），
小组件守护进程 `chronod` 和壁纸的 `PosterBoard` **都不在里面**。症状是：桌面背景纯黑、小组件是纯白空块
（位置和文字标签都在，就是没有内容），日志里反复出现

```
com.apple.xpc.launchd <Error>: Could not find job with label com.apple.chronod
```

这不是渲染问题：`glcopy=1`、`glblit=0`、`adreno_speed`、`fb_threads` 都改过，画面一点没变。

```sh
cd userland && GL=1 REST=1 SYSTEM=1 ./build.sh   # 作业 25 -> 407（mkjobs.py --rest，按需启动）
cd .. && ./xl stop && ./xl ramdisk userland/system.apfs   # 跑着的时候 xl 不收新镜像
XL_SECS=0 XL_CMDLINE="gl=1 adreno=22 adreno_keep=1 touchscreen=1 bootcpu=7 cpus=8 rd=md0 battery=1" ./xl run
```

`cpufreq=1` 可以不跟：它把 EPSS 的 perf state 直接写到每个 cluster 的最高行（`row 20 -> 20`，3187 MHz），
**所有核心钉在最高频**。实测去掉它，渲染层每帧 3413 µs 对 3570 µs（差 4%），壁纸和小组件都正常，
所以为了不发烫，默认就不要带它。

- 验证：日志里 `Successfully spawned chronod[87] because ipc (mach)`；截图里壁纸是蓝/青/黄/橙渐变，
  Weather 显示 "No weather data"，Calendar 显示 "FRIDAY / 30 / No events today"。
- `ls userland/root/LaunchDaemons | wc -l` 可以判断手上的镜像属于哪一套：**25 = 只有默认那套，407 = 全量**。
- 重建了镜像**一定要重新上传**：跑的是 RAM 里那一份，`xl` 认不出旧的会提示
  `nothing here recorded who uploaded the resident image`。
- 手机开机很快，`xl run` 之后 **90–150 秒**截图就够，不用等 240 秒。

### 少了 `GL=1`：停在苹果 logo，到不了桌面

`REST=1` 少了只是壁纸黑；`GL=1` 少了是**卡在 logo**（CoreAnimation 拿不到 GLES 上下文会主动
模拟崩溃，日志里的 `Failed to initialize Metal or OpenGLES contexts!`）。三者各少了会怎样：

| 少了 | 症状 |
|---|---|
| `GL` | 停在苹果 logo |
| `REST` | 桌面能起，壁纸黑、小组件空白 |
| `SYSTEM` | dylib 改了，镜像不重建 |

## loader 在屏幕上有字、PC 上却没有设备

症状：手机上能看到 `== XNULOADER v3 ==` 那一段，但 `fastboot devices` / `adb devices` /
`xl state` 全是空，Windows 里连"未知设备"都没有，拔插 USB 无效。

**先看屏幕**，尤其这两行（新版 loader 一定会打）：

```
display ____ x ____ at 0x________
platform ________
```

loader 认平台**只看 GOP 报的 framebuffer 基址**：等于 `0x9C000000` 才是 alioth，否则退回
`platform_qemu_virt`，然后去写 QEMU 的 fw_cfg（`0x09020000`）——在真机上就是

```
Synchronous Exception at 0x00000000A10026D8
```

（`0xA1000000` 是 loader 的 PE `/base`，所以那其实是 RVA `0x26D8`，一条 `strh` 到 `0x09020008`。）

现在这条路径已经收窄：`ramfb_phone_screen()` 只在基址等于 QEMU 自己的 `0xE1000000` 时才调用，
并且 `display … at 0x…` 打在它**之前**。所以再遇到同类问题：

- 屏幕的 `platform` 是 `alioth`、`usb: started` → 手机侧没问题，去查 PC（换口、换线、
  `dump/usb-restart.ps1` 重启 USB hub）；
- 屏幕报的基址不是 `0x9C000000` → 把那个地址记下来，给 `platforms[]` 补一条；
- 屏幕停在 `Synchronous Exception`（旧 loader）→ 用新 loader：`DEVICE=alioth ./xl reload`。

另外，内核 panic 或 `xl stop` 之后 loader 可能报 `core N is not off … not the kernel's to take`
或 `THE KERNEL LEFT CORE 500 RUNNING`，这时**必须整机重启**（`xl reboot`，或者长按电源关机后
电源+音量下进 fastboot）再 `xl run`，否则下一次启动会缺核、userland 起不来。

### `bootcpu=N` 决定 loader 还能不能回答

`xnuloader/loader.c:2754` 起：`kernel_mpidr` 默认就是 loader 自己这个核，只有 cmdline 里带
`bootcpu=N` 才会换（带了会打 `the kernel will run on core %x (this is core %x)`）。
不带的时候 loader 是 **`jump_to_kernel`：把自己正在用的核整个让给内核**——这一轮里就没有
任何核在跑 loader，`xl state`、`.live` 抓取、截图、`xl stop` 全部 121 超时。
**这是"loader 不说话"最常见的原因，不是挂死。** 01:33 那次能连续 10 分钟取到 `.live`，
就是因为带着 `bootcpu=7`（内核在 7 号核，loader 留在自己的核上继续服务 fastboot）。

推论：`tools/backlight-test.sh` 这类"要在一轮中间读日志"的脚本必须带 `bootcpu=7`；
判据是 run 日志里的 `cmdline:` 行——`cpus=1` 且没有 `bootcpu=`，那一轮的"mute"什么都不说明。

### 设备枚举着，但一个命令都不回

症状：Windows 设备管理器里 `Android Bootloader Interface` 状态 OK（序列号可能是 `XXXXXXXXXXXX`），
可 `fastboot devices` 空、`xl state` 是 `absent`，`fastboot reboot` 报

```
AdbWriteEndpointSync failed: 信号灯超时时间已到 (121)
```

这是 **loader 自己停了**（不是 USB、不是线）：端点还在，但没人往里读写。两种来源，区别对待：

1. **等 15 秒再看**。`xl` 的 `timeout N fastboot …` 只杀 Linux 这侧的转发进程，Windows 的
   `fastboot.exe` 有时会活下来并一直占着设备（`Get-Process fastboot`），别的调用就会"看不到设备"。
   杀掉它（`Stop-Process -Name fastboot -Force`）后重试。

   **这一条比看上去更容易踩**：`xl run` 在 loader 不说话的时候每 2 秒轮询一次，每次超时都可能
   留下一个 `fastboot.exe`；于是"设备不见了 / 永远 121"可能是 PC 自己的僵尸进程，不是手机上
   真的没响应。2026-10-31 08:14–08:24 我连续探了二十多次，就是把这个现象当成了 loader 挂死。
   判据：先全部杀掉、再等 2 秒、**只发一条** `fastboot getvar product`。
   - `fastboot devices` 列出 `????????????  fastboot`（序列号是问号）本身就是这个状态的典型样子；
     正常 ABL 会给出真序列号（`<序列号>  fastboot`），xnuloader 也给出它自己的名字。
   - 现在 `xl` 的 `fb()` 自己会收拾：在 WSL 里（`/proc/version` 含 `microsoft`）一次
     `timeout` 返回 124 之后就 `Stop-Process -Name fastboot -Force`，再返回那次的输出。
     别的平台上这条分支不生效（macOS 上没有 `/proc/version`）。
2. 杀掉之后还超时 → 内核把 loader 一起挂死了。**现在默认 `loader_return_after=0`（永不自动返回），
   所以没有任何看门狗会把它救回来**，只能长按电源。这是去掉定时返回的代价：以前
   `XL_SECS=8` 那种"到点回来"顺带兜住了挂死，现在挂死就挂死，去现场重启一下即可
   （用户 2026-10-31 明确要求去掉自动返回机制）。
   例外是带 `XL_SECS=N` **且**带 `bootcpu=N` 的跑法：09:05 起 loader 自己在 `N+60` 秒判"内核没回来"、
   暂存日志、`N+120` 秒冷复位（见文末"loader 自己发现…"一节）。08:05 之后躺着的这台没有
   `bootcpu`，loader 那一核早就交给内核了，任何软件都救不回来。

**判断是不是挂死**：`fastboot getvar product` 若是 `FAILED (AdbWriteEndpointSync failed …)` 而
不是 `product: xnuloader`，就是 loader 不响应了；`slot-unbootable:b` 一类的槽位状态这时读不到，
别据此下结论。

**2026-10-31 08:05 的这一版**：`fastboot devices` 显示 `????????????  fastboot`，
PnP 的实例是 `USB\VID_18D1&PID_D00D\XNULOADER`（还是 loader，不是 ABL），所有命令（含
`fastboot reboot`）都 121 超时。WSL 的 PowerShell 不是管理员（`IsInRole(Administrator)` =
False），禁用/启用 USB 设备那条路也走不了，**只能长按电源**。先前 07:53 那次同样的现象
约两分钟后手机自己复位回 ABL（`product: alioth`），所以遇到它先等 2–3 分钟再看一眼，
还没回来再找人按电源。

## 跑完一轮之后整屏是绿的，不是坏了

内核把控制权交还 loader 之后，loader 会把整块帧缓冲刷成 `0xff208040`（屏幕上就是 `(32,128,64)`
的纯绿）。这是 `xnuloader/loader.c` 返回色表里"**跑完了，正常返回**"的颜色，和其它几个：

| 颜色 | 值 | 含义 |
|---|---|---|
| 绿 | `0xff208040` | 内核跑完、正常交还控制权 |
| 紫（最下 320 行） | `0xff8020c0` | panic |
| 红 | `0xffc02020` | 早期异常（启动阶段就挂了） |
| 琥珀 | `0xffc0a000` | checkpoint |
| 蓝 | `0xff2040c0` | selftest |

**现在默认 `loader_return_after=0`（永不自动返回）**，所以跑完不会自己变绿：绿屏只在这三种情况下出现——
内核自己 panic 返回、`xl stop` 让内核交还，或者 `XL_SECS=N` 显式要求 N 秒后返回。也就是说
平时看到的应该是 iOS 桌面一直停留在那里；看不到 iOS 桌面而看到绿屏，先查 `XL_SECS` 有没有被带上。

要看"显示相关的实验有没有效果"，就在内核活着的时候看；**绿屏上什么都看不出来**，
亮度/刷新的实验一律在 iOS 还在跑的那段时间里看。

## 120 Hz

面板（天马 k11a）的刷新率是它自己按 DCS 走的，内核发一串写就能切：

```sh
# 改成两秒后就切档，然后再数 6 秒 TE（数的是切档之后的）
DEVICE=alioth XL_SECS=40 XL_CMDLINE="… display_hz=120 display_hz_delay=2 display_te=6 display_te_delay=4" \
  ./xl run
```

日志应当是：

```
display: asking the panel for 120 a second: 7 packets, each one a DCS write
display: the panel was asked for 120 a second (display_te counts what it now does)
display: TE: 728 pulses in 6 s = 121.333 a second
```

**踩过的坑（再遇到"命令都发了、面板没反应"先看这条）**：MSM 的 DSI 命令 DMA 是总线主设备，
只能读显示侧地址窗口里的内存。内核 `kmem_alloc_contig` 出来的普通页它读不到——日志照样
`intr 0x501, status0 0, ack err 0`，面板一动不动。现在的默认值是**沿用 bootloader 留在
`DSI +0x048` 的那页**（alioth 上是 `0x9aead000`），每次发包前保存那 16 字节、发完写回。
`display_dma_at=ADDR` 可以换页，`display_dma_own=1` 是那个静默失败的反例。

能不能肉眼确认"面板收到包"：`display_bl_ramp=1`，约 9 秒里亮度 0x7ff → 0x20 → 0x7ff 来回三轮。

面板 120 Hz ≠ 画面 120 fps：`fb_hz`（默认 60）和 MDP 侧的 `mdp-transfer-time-us` 都还没跟着改。

## 控制中心的亮度条和音量条为什么都没反应

两个滑杆坏在两个完全不同的地方，日志里的证据也不同。

**亮度**：请求一路都走到了。backboardd 自己会打

```
[Backlight] Setting CoreBrightness backlight factor:1.000000 with fade duration:0.185000
[Backlight] Set BrightnessSystem property:DisplayBrightnessFactorWithFade to:1
```

然后 CoreBrightness 要把 level 交给一个 IOKit 服务（类名 `AppleBacklight` / `AppleARMBacklight`，
user client 是 `BCAppleBacklightBrtControl` 包的那个）——**这个内核以前没有这个服务**，
所以到那里就断了。现在 `iokit/Kernel/MiniAppleBacklight.cpp` 会照类名发布这两个服务，
启动后每 20 秒打一行

```
AppleBacklight: 2 services published, the user client asked for N times
```

`N` 就是判据：0 = CoreBrightness 不按这个类名找（还没解决）；非 0 = 找到了，
紧接着的 `CLIENT OPENED` / `selector …` 行就是协议，照着接到 `hakios_display_set_brightness()`
（DCS 0x51）上即可。一键跑：`tools/backlight-test.sh`。

**（18:50 离线核对，不用手机）** iOS 自己的二进制里，找这个服务的代码有三处，读的都是
`IODisplayParameters.brightness`：

| 谁 | 按哪个类名找 | 读什么 |
|---|---|---|
| `BCAppleBacklightBrtControl`（BrightnessControl） | `AppleBacklight` | `brightness.min` / `brightness.max` 当量程（`_maxUser >= _minUser`），另有 `BrightnessMilliNits` / `BrightnessMicroAmps` |
| CoreBrightness 的 `CBDisplayContaineriOS` | `AppleARMBacklight` | 显示容器模块 |
| PowerLog 的 `PLDisplayAgent` | `AppleARMBacklight` | `min` / `max` / `rawData` / `brightness` |

所以两个类名都发是对的。**以前缺的是属性**：`MiniBacklightService::start()` 只
`registerService()`，一个属性都没发，`BCAppleBacklightBrtControl` 找不到量程。现在服务起来时就发
`IODisplayParameters = { brightness = { value, min = 0, max = 0x7ff } }`，每次送电平再更新
`value`（内核 18:45 那版）。取这组字符串的办法写在 `PROGRESS.md` 的 18:50 一节（读 cryptex 里的
dyld 共享缓存）。QEMU 里冒烟过（19:22）：两个类都发布、属性在 `registerService()` 之前就位、
日志 `AppleBacklight: published as AppleBacklight, provider GenericARMPE, brightness 0x7ff of 0x7ff`。

`N` 是 0 的时候，SpringBoard 里还有一条从 iOS 那一侧问的探针（`inject_sb.c` 的
`backlight_probe()`，启动后 95 s / 245 s）：

```
hakios: backlight probe: AppleARMBacklight -> IOService:/GenericARMPE/AppleARMBacklight
hakios: backlight probe: AppleBacklight: no such service in the registry
```

它用 iOS 自己的 `IOServiceGetMatchingService` 查同样的类名，并把服务在 registry 里的**路径**打出来：

- 探针能查到、内核说 `asked for 0 times` → 服务看得见，是 CoreBrightness 没找（看路径决定要不
  要把服务挂到别的父节点/改名）；
- 探针也查不到 → 问题在服务发布这一侧，跟 CoreBrightness 的匹配逻辑无关。

**第二种到达方式**：macOS 的亮度栈不是开 user client，而是
`IOServiceMatching("AppleARMBacklight")` 之后 `IORegistryEntrySetCFProperty` 写
`IODisplayParameters = { brightness = { value, max } }`。所以"`asked for 0 times`"只排除了
user client 这一条路。这一节原来写着"`MiniBacklightService::setProperties()` 现在把写入的字典按键打出来"
——**那句话当时是假的**：`xnu/files` 和 `xnu-src` 里都没有这个 override，而基类
`IORegistryEntry::setProperties` 返回 `kIOReturnUnsupported` 且一个键都不存
（`iokit/Kernel/IORegistryEntry.cpp:584`）。19:37 才真正写上（见 `PROGRESS.md` 19:50 一节）：
写入的字典逐键打印，有 `{value, max}` 就按 `value * 0x7ff / max` 换算；是否真发到面板仍是
`backlight_dcs=1` 决定，与 user client 那条路一致。日志里找：

```
AppleBacklight: 1 properties written on AppleARMBacklight
AppleBacklight:   1: IODisplayParameters = 1 keys
AppleBacklight:   2: brightness = 2 keys
AppleBacklight:   3: value = 12345
AppleBacklight:   3: max = 65536
AppleBacklight:   level 12345 of 65536 -> panel 0x60a (not sent: no backlight_dcs=1)
```

（顺带：`IOService`/`IORegistryEntry` 本身没实现 setProperties，基类返回
`kIOReturnUnsupported` 且什么都不存，没有这个 override 时这类写入既被拒绝又被忽略。）

一键跑：`tools/backlight-test.sh`。它起跑时带 `XL_FOLLOW=20`（否则 `XL_SECS=0` 的 run 中间
根本没有 `.live` 可读），`SCREENSHOT=0` 可以跳过那次 10 MB 的截图。

**跑法分两步**：第一轮**不带** `backlight_dcs=1`，只看协议（`CLIENT OPENED` / `selector …` /
`properties written on …`），面板不动；确认有东西到了之后，第二轮在命令后面加一个
`backlight_dcs=1`（`tools/backlight-test.sh backlight_dcs=1`），那时
`hakios_display_set_brightness()` 才真的发 DCS 0x51——它会把 0..1 的浮点因子换算成
0..0x7ff，日志里是 `AppleBacklight: … sent to the panel as level 0x…`。
（亮度发送在 `hakios.display.c:777`，clamp、去重、大跳变才打一行都在那里。）

**每轮先看 `cmdline:`**。`xl run` 不设 `XL_CMDLINE` 时，会给内核一条
`cmdline: debug=0x8 cpus=1 rd=md0 loader_return_after=0 … sb_delay=8 -v` —— 没有 GPU、
没有 `bootcpu=7`、**没有 backlight 服务**，`asked for N times` 就永远没有意义。
2026-10-31 08:22 之前的 `backlight-test.sh` 正是这个状态（定义了 `ARGS` 却没交给 `xl`），
那天的四轮数据全部作废。现在脚本写的是 `XL_CMDLINE="$ARGS"`，并且开头会打印
`backlight-test: boot arguments: …`；两条里任何一条不对，就别拿那轮的数据下结论。


**音量**：滑杆和音量键**其实都到了内核**——日志里有

```
hakios-audio: the volume: 0.000 dB under the loudest
buttons: volume down PRESSED at 2792.683 s
```

音量是 HAL 插件（`userland/src/audio_plugin.c`）里按 dB 缩放样本的，所以它"能调"；
问题在于**没有 sink，什么都没放出来**，调了也听不见。日志里的播放侧是

```
hakios-audio: 0.09 s written, 0.00 s of it sound, peak 0% of full scale
```

（iOS 写进来的就是静音。）手机上要出声，缺的是整条硬件链：

1. `hakios.speaker.c` 是 **redwood 的 AW88261**，引脚、地址、TDM 全不一样，alioth 上不装 sink；
2. alioth 的两颗 **CS35L41 已经能认出**（I2C `0x40` 听筒 / `0x41` 扬声器，DEVID `0x035a40`、
   REVID `0xb2`），但只做到"读身份"：复位一放回、初始化序列（几百个寄存器）没写；
3. 功放的音频输入是 **ADSP 驱动的三级 TDM（mi2s2，引脚 133/134/135/137）**，ADSP 没起来；
4. 麦克风走 SoundWire 的 WCD938x，同样没起。

所以顺序是：CS35L41 初始化 → TDM 时钟 → ADSP 路由，**这三步里前两步也不产生声音**，
必须 ADSP 起来才有信号。`amp_step=N` 那套只到第 4 步（复位 + 读状态），是这条链的起点。

现有的固件只凑齐了第一段的一半：`alioth-local/firmware/fw-audio` 是 **ADSP 的 `adsp.mdt` +
`adsp.b0x`**（WCD938x 的 codec 驱动跑在上面），但**没有 CS35L41 自己的 DSP 固件**
（`/vendor/firmware` 下的 `cs35l41*` 那几段没有抓到，dump 里也没有 vendor 分区）。也就是说
功放要么按“无 DSP 直通”模式初始化（寄存器序列得自己写），要么得先补抓 `/vendor`。

## 状态栏的电池：数字、闪电有了，颜色还没有

数值来自内核（`bsd/dev/hakios.battery.c` 每 5 秒读一次 loader 给的电池页，每分钟打一行
`battery: SOC=81%, V=4092 mV, I=315 mA, T=35 C, battery=1 charger=1 charging=1 action=3`），
SpringBoard 里的 shim（`userland/src/inject_sb.c`）把 `UIDevice.batteryLevel/batteryState`、
`_UIBatteryView` 的 `chargePercent` / `showsPercentage` / `_shouldShowBolt` 和锁屏充电环
都改成了这个值。截图上填充约 81 %、"81" 在图标里、闪电在，说明这条链是通的。

和真机不同的一点：**充电时填充是白的，真机是绿的**。颜色不是上面那几个属性决定的，
`_UIBatteryView` 内部按某个状态/颜色自己选。为了不猜着钩（钩错会给没插电的手机画成绿色），
shim 里加了一次性的方法问卷，启动后 90 s 和 240 s 各打一次，日志里找

```
hakios: battery survey: -[_UIBatteryView …]                             ← 方法名 + 未 slide 地址 + 类型编码
hakios: battery survey: [_UIBatteryView] ivar … at +152 "…"             ← 可能的颜色 ivar
hakios: battery survey: _UIBatteryView has 109 methods, instance size …  ← 实际数字以日志为准
```

只列名字里带 battery/charge/percent/bolt/color/fill/level/state 的，不需要额外 boot arg。
下一版照这些行补颜色钩子。看电池随时间的读数：`tools/battery-check.sh [分钟]`
（每分钟一条内核读数 + 一张截图）。

除了方法问卷，`setChargePercent:` 第一次被调用 3 秒后还会**一次性**打出视图树（两层、最多
16 个），每行是类名、frame、hidden、alpha、`backgroundColor`、`tintColor`，以及它 layer 的类名和
`fillColor`/`strokeColor`/`backgroundColor`：

```
hakios: battery view: the tree of _UIBatteryView:
hakios: battery view: _UIBatteryView 17x11 at 0,0 hidden 0 alpha 1.00
hakios: battery view:       layer CALayer
hakios: battery view:     _UIBatteryFillView 17x11 at 0,0 hidden 0 alpha 1.00
hakios: battery view:       fill UIExtendedSRGBColorSpace 1 1 1 1
```

充电填充若是 `CAShapeLayer` 画的，真正上色的就是那行 `fill`；把它和真机的绿色对比，
就是颜色钩子要改的地方（`fillColor`/`strokeColor` 只在 `respondsToSelector:` 通过时才发，
`CALayer` 没有这两个属性）。

**（18:55 离线核对，不用手机）颜色到底是哪个方法选的**：`system.apfs` 的 dyld 共享缓存
（分片 `.03`）里，`_UIBatteryView` 挑颜色用的方法名是

```
_batteryColorForCapacity:lowCapacity:style:usingTintColor:
_batteryFillColorForEntry:usingTintColor:          （配套 _batteryFillColor / _batteryUnfilledColor / _batteryTextColor）
_updateBatteryFillColor   _updateBodyColors   _updateBodyLayers   _updateBolt
```

也就是说颜色由 **容量 + lowCapacity + style + tintColor** 决定，不是由 `chargePercent` 直接决定；
充电的绿色在那个 entry 的充放电状态里。所以运行时那份视图树之外，还应看 `_batteryFillColor` 的取值
和 entry 的充电标志——钩子要改的正是 `_batteryColorForCapacity:lowCapacity:style:usingTintColor:`
这一处的入参/返回值，而不是给图层硬写颜色。电池状态本身走的是 IOPS
（`.05` 里的 `AppleSmartBatteryManager` / `com.apple.system.accpowersources.*`）。


## loader 自己发现"内核没回来"并复位（09:05 起）

手机上没有任何别的复位看门狗可用：`wdt_arm()` 只对 `platform_redwood` 生效，alioth 的 Haven
watchdog 从来没被 arm 过；UEFI 冷复位 `ST->rt->reset_system(0, …)` 原本只在 `xl reboot` 和
loader 自己的异常处理器里调用。所以设备"内核还在跑、但什么也不做"时，只能等人长按电源。

现在 loader 拿内核自己的承诺当依据（`loader_return_after=N`，就是 `XL_SECS=N`）：

| 时刻（从开跑算） | loader 做什么 |
|---|---|
| `N + 60 s` 还没回来 | 判定超时（`OTHER_CPU_OVERRAN`），把内核日志理好、`set_result("overrun")`、写盘 |
| 之后 60 s | 只响应 USB，让 `xl run` 把日志取走 |
| `N + 120 s` | `reset_system(0)` 冷复位，主机在不在都一样 |

主机侧能看到 `result: overrun`，`xl` 打印一行后返回 2，**不会**再补 `fb reboot`（会和 loader 自己
的复位抢）。`logs/*.log` 里是内核最后说的话。

前提与边界：

- **必须有 `bootcpu=N`**：loader 在另一个核上活着才有"谁来看"这回事。不带 `bootcpu` 的跑法
  loader 根本不在运行，出任何问题都没有软件能救（见上面 `### bootcpu=N` 一节）。
- **`XL_SECS=0`（默认）行为不变**：没有承诺，就没有期限；跑多久由 `xl stop` 决定。
  想让无人值守的跑法有这条保险，就带 `XL_SECS=NNN`。
- 内核只是**心跳停了**的挂死，走原来的 50 秒静默检测（`result: hung`），不等 `XL_SECS`。
- 复位之后手机回到 Android（ABL 的默认），不是回到 loader；`xl state` 看到 `android` 再 `xl enter`。

`xnuloader/loader.efi`（09:06）已经带上这条逻辑，旧件备份在 `xnuloader/loader.efi.bak-20261031`。
生效时机是**下一次 `xl enter`**：`enter()` 每次都用当前的 `xnuloader/loader.efi` 现打 payload。

QEMU 上验证过整条路径（`qemu/overrun-test.sh`，09:16）：判定、`result: overrun`、60 秒等待、
`reset_system` 冷复位都走通。QEMU 里假装超时的办法是内核的测试 boot arg
`loader_return_skip=1`（`loader_return.c`：命令行照留 `loader_return_after=N`，内核自己不装返回定时器，
心跳照跳）；只有测试会传它，手机上不传就没有任何影响。真机上还没触发过，回来后第一次
`XL_SECS=NNN` 的跑法就是它的第一次实战。

## 声音（三）：ADSP 驱动其实已经按 alioth 参数化，第一步是只读的

`xnu/files/bsd/dev/hakios.adsp.c` 的文件头还写着 redwood（POCO X5 Pro），但**代码里的常量已经是
alioth 的**，逐条对照 `dump/twrp-fdt.dts`（刷机机上的那份）：

| 代码里 | 值 | DT 依据 |
|---|---|---|
| `ADSP_MEMORY` / `ADSP_MEMORY_SIZE` | `0x8bb00000` / `0x2500000` | `pil_adsp_region@8bb00000`（dts 38893，no-map，37 MB） |
| `PAS_ADSP` | 1 | `qcom,lpass@17300000` 的 `qcom,pas-id = <0x01>`（7332） |
| 固件名 | `adsp` | 同节点的 `qcom,firmware-name` |
| `SMEM_PHYS` | `0x80900000` | kona 的 smem（DT 里没有单独节点，与 Android 一致） |
| `ITEM_SMP2P_OURS` / `THEIRS` | 429 / 443 | `qcom,smp2p-adsp: qcom,smem = <0x1bb 0x1ad>`（18031） |
| `ITEM_CRASH_REASON` | 423 | `qcom,smem-id = <0x1a7>`（7334） |
| `IPCC_LPASS` / `IPCC_GLINK` / `IPCC_SMP2P` | 3 / 0 / 2 | `smp2p-adsp` 的 `mboxes = <&ipcc 3 2>`、glink 的 `0`（18036 起）；IPCC 本身 `0x408000`（5946） |
| `CMD_DB_PHYS` | `0x80860000` | kona 的 cmd-db |

**还没确认的**（DT 里没有或对不上，只能真机读）：

- GLINK 的 item 号 `478/479/480`：这是运行期由共享内存分配器给的编号，redwood 上跑出来是这个数，
  alioth 上不一定；`adsp_step=1` 会打 `GLINK's descriptor (478): there / NOT THERE`。
- RSC 基址 `0x18220000`：DT 里 `apps_rsc` 是 `rsc@18200000`，差 `0x20000` 可能是 drv 偏移，
  第 7 步（RPMh 请求 DSP 的电源轨）之前要用第 1 步的输出交叉验证。

（"镜像装不装得下 37 MB"原来也列在这里，**已经不用猜了**：真机 `adsp_step=2` 打了个
`the image is whole … 18 segments, 21001504 bytes of files, 0x2500000 bytes of the DSP's 0x2500000`。）

**第 4、5 步与 Linux 的对应（18:20 核对）**：第 4 步把 `.mdt` 的**头 + 哈希**整页交给
TrustZone（`SCM_PAS_INIT_IMAGE`，就是 Linux `qcom_scm_pas_init_image()` 拿到的 "metadata"），
再 `SCM_PAS_MEM_SETUP(pas_id, 0x8bb00000, 跨度)`；第 5 步只写**段**，落点 `paddr - min_addr`。
Linux 的 `mdt_loader` 不会再多写一份元数据到区首——这份镜像的第一段 `paddr == min_addr`，
本来就落在偏移 0，区首没有第二份数据的位置。所以第 5 步"只写段"与 Linux 一致，不是遗漏。


所以声音这条链的**下一次真机行动**不是"移植"，而是：

```
XL_CMDLINE="... adsp_step=1"   → 看 429/443/478/479 在不在、doorbell 计数、crash reason
XL_CMDLINE="... adsp_step=2"   → 固件完整性与大小（仍不写）
再往后 3 步才第一次写（AOP 邮箱），4/5 步写 DSP 内存，7 步请 TrustZone 认证并启动
```

第 1 步纯读，失败也只是"读不到"；它同时决定后面要不要先补 `/vendor/firmware_mnt/image` 里的
`adsp.mdt`/`adsp.bNN`（`alioth-local/firmware/fw-audio` 里那份要确认是 alioth 自己的签名段，
不是别的机型的）。

## 已知坑：镜像机型

`mkxnuimg.py` 的 `--device` 默认是 `poco-x5-pro`（读 $DEVICE，未设则 redwood）。
不带 `-d alioth`（或 `DEVICE=alioth`）重建的镜像装的是 redwood 的 FD/dtb/shim：
在 alioth 上 FD 显示初始化即崩（闪一下黑屏、2 秒内回 fastboot），ABL 日志
（logfs，可经 TWRP 拉取）里对应 "Error: Appended Soc Device Tree blob not
found"。判别法：镜像内核段里 FDT 的 totalsize——alioth 919977、redwood
405158。2026-10-31 晚整晚的"boot 不进 loader"即此因，`-d alioth` 重建后
4 秒 loader 就活。

## 已知坑：resident 根文件系统与哨兵文件

根文件系统（2.3GB 的 userland/system.apfs）靠 `./xl ramdisk` 传进 loader RAM，一次
上传活到手机冷启为止。`tools/backlight-test.sh` 用哨兵文件
`${XDG_RUNTIME_DIR}/darwin-hakios-phone.ramdisk` 判断要不要上传：哨兵在就跳过。用
`fastboot boot` 直接冷启 loader 后 RAM 是空的，但哨兵还在 → 内核拿 4MB payload
ramdisk 当根 → `Process 1 exec of /sbin/launchd failed, errno 2` panic。判别：好 boot
的 loader 日志有 `resident image at pa fc000000 size 943c4000: it is the root (md0),
the payload ramdisk is md1`，坏 boot 没有。冷启 loader 后要么
`xl enter`（自动删哨兵），要么手动删哨兵或重传。

### 第二种进坑方式：`pkill` 掉一场跑

冷启之外，`pkill -f "xl run"` 也会让哨兵与 RAM 对不上，症状同样是
`exec of /sbin/launchd failed, errno 2`。而且它还会留下 `.run` 锁，下次 `xl run` 报
`a run is in progress` 看着像手机的问题。

```sh
./xl stop
pkill -f "xl run"                                  # 返回 15 是正常的
rm -f /run/user/1000/darwin-hakios-phone.{run,ramdisk}
./xl ramdisk userland/system.apfs                  # 必须重传，90 s
```

顺带：`xl ramdisk` 收尾时设备会从 `fastboot devices` 里消失十来秒，不是掉线。
