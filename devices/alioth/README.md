# Device: Redmi K40 (alioth)

骁龙 870（SM8250-AC），12 GB，代号 alioth。BL 已解锁。

本目录是 alioth 在**当前这棵树**里的全部内容。这份 README 只讲**资源在哪、怎么编、怎么跑**。

**动手改任何驱动之前，先去 [`hardware.md`](hardware.md) 查那个值从哪来。** 那张表每条都注了
来源，可信度不同（真机 dump > 入库 DTS > 公开规格）；寄存器位定义和初始化序列这类设备树里没有
的东西来自高通的 vendor 内核源码——**公开可下载**，路径和取法在 `hardware.md` 的「厂商源码从哪来」。
功能现状和踩坑过程在 `PROGRESS.md`。

## 资源在哪

| 项 | 位置 | 说明 |
|---|---|---|
| 内核驱动 | `xnu/files/bsd/dev/hakios.*.c` | display / adreno / adsp / amp / goodix / haptic / buttons / cpufreq 等 |
| loader | `xnuloader/loader.c` | 平台表在 `platform_alioth` |
| UEFI | `devices/alioth/prebuilt/SILICIUM_UEFI.fd` | Mu-Silicium aliothPkg，`0x300000 @ 0x9FC00000`，已编译入库 |
| 设备树 | `dts/` | `alioth-sm8250.dtsi`、`kona-*.dtsi`（LineageOS 的，末梢与本机批次可能有出入） |
| soc.args | `soc.args` | 由真机 DTB 生成 |
| 固件 | 顶层 `fw-audio` / `fw-gpu` / `fw-haptic` | **不入库**，从自己手机 TWRP 提取，见下 |
| 系统镜像 | `userland/root.apfs`、`userland/system.apfs` | **不入库**，由 `userland/build.sh` 生成 |

真机 dump、旧工具、备份在 `<主机>\Develop\alioth-local`（`dump/twrp-fdt.dts` 最有用，
36,301 行）。设备本地的大文件在 `~/alioth-device-state`。

## 取固件

手机在 TWRP 且 adb 连着时：

```sh
devices/alioth/pull-firmware.sh
```

它挂载 vendor 与 modem 分区，拉 GPU（`a650_*`）、音频（`adsp.*`）、haptic、Wi-Fi（`qca6390/`）
到顶层 `fw-*`。**Wi-Fi 还需要** `fw-wifi/mac`（本机 MAC，一行，冒号分隔十六进制）与
`fw-wifi/network`（Wi-Fi 名与密码），这两个自己写。

## 编译内核

```sh
xnu/apply.sh          # 把 xnu/files/ 铺到 XNU 源码上（幂等，可重复跑）
./xnu-build.sh -j8    # -> xnu-src/.../BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
```

**改了 `xnu/files/` 里的文件必须重新 `apply`**（或手动拷到 `xnu-src/` 对应位置），构建读的是
`xnu-src/` 那一份，不是 `files/` 那一份。

结尾的 `check_all_exports` / `IOKit.symbolset` 报错是**已知无害**的，判据是看内核文件的
时间戳有没有更新，不看 make 的退出码。

## 编译系统镜像

```sh
cd userland && GL=1 REST=1 SYSTEM=1 ./build.sh
```

**三个参数都不能省**：

| 少了 | 症状 |
|---|---|
| `GL` | 卡在苹果 logo，到不了桌面（CoreAnimation 拿不到 GLES 上下文） |
| `REST` | 桌面能起，但壁纸纯黑、小组件纯白空块 |
| `SYSTEM` | dylib 改了，镜像不重建 |

判断手上的镜像属于哪套，不用跑起来：

```sh
ls userland/root/LaunchDaemons | wc -l                      # 25 = 精简，407 = 全量
python3 tools/apfs/apfs.py ls userland/root.apfs /lib | grep -c gl   # 0 = 没 GL
```

`cpufreq=1` 默认不要带：它把所有核钉在最高频（3187 MHz），实测渲染只快 4%，代价是发烫。

## 装机与运行

```sh
DEVICE=alioth ./xl enter        # Android/fastboot -> loader（内部要打 USB 镜像）
./xl ramdisk userland/system.apfs   # 2.4 GB 系统镜像进手机 RAM，约 90 秒，每次 loader 重启后都要做
DEVICE=alioth ./xl run          # 上传内核、运行、取回日志
DEVICE=alioth make phone        # 或者一条命令做完上面三件
```

**镜像默认设备是 `poco-x5-pro`**，它的 UEFI 在 `0x9FD00000`、大小 `0x200000`；alioth 是
`0x9FC00000` / `0x300000`。刷错就进不了 loader。所以**凡是要打镜像的命令都要带 `DEVICE=alioth`**，
包括 `xl enter` / `xl reload` / `make phone`——它们内部都调 `mkxnuimg.py`。
判据：正确镜像是 1,992,704 字节，POCO 的是 1,470,464。

`xl` 只调 PATH 里的 `fastboot` / `adb`。WSL 里系统的 fastboot 看不到设备，需要
`/usr/local/bin/fastboot` 那个转发到 Windows 的包装器。

`xl run` 之后 90–150 秒截图就够，不用等更久：

```sh
./xl screenshot out.png
```

## 跑挂了怎么查

### `Process 1 exec of /sbin/launchd failed, errno 2`

根文件系统没挂上——**几乎总是忘了 `./xl ramdisk`**，不是镜像坏了。loader 冷启后 RAM 是空的。
判别：好的一轮日志里有 `resident image at pa ... size 943c4000: it is the root (md0)`，坏的那轮没有。

`xl` 用 `${XDG_RUNTIME_DIR}/darwin-hakios-phone.ramdisk` 判断要不要上传，这个文件会和真实状态
不同步。`xl enter` 会删它；`pkill` 掉的 run 不会。手动清理：

```sh
./xl stop
pkill -f "xl run"                                   # 返回 15 是正常的
rm -f /run/user/1000/darwin-hakios-phone.{run,ramdisk}
./xl ramdisk userland/system.apfs                   # 必须重传
```

顺带：`xl ramdisk` 收尾时设备会从 `fastboot devices` 里消失十来秒，不是掉线。

### `fastboot boot` 报 `Load Error`

连 TWRP 都拒绝，说明 ABL 把槽位标成 unbootable 了，不是镜像问题：

```sh
fastboot getvar slot-unbootable:b   # yes 就是它
fastboot --set-active=b             # 一行修好，不刷任何分区
```

### loader 不说话

| 现象 | 原因 | 处理 |
|---|---|---|
| 屏幕停在 `Synchronous Exception` | 旧 loader，没认出这台机器的 framebuffer 基址 | `DEVICE=alioth ./xl reload` |
| 命令全 121 超时，没带 `bootcpu` | loader 把自己那个核整个让给了内核 | 带 `bootcpu=7`；看 run 日志的 `cmdline:` 行确认 |
| 命令全 121 超时，带了 `bootcpu=7` | Windows 侧残留 `fastboot.exe` 占着设备，或端点真死了 | 先 `Stop-Process -Name fastboot -Force`；等 2–3 分钟看是否自复位；都不行才长按电源 |
| 报 `core N is not off` | 内核 panic 或 `xl stop` 之后没整机重启 | `xl reboot` 后再跑 |

### 整屏是绿的

没事。`0xff208040` 是 loader 返回色表里“内核跑完、正常交还控制权”的颜色。默认
`loader_return_after=0`（永不自动返回），所以绿屏只在三情况下出现：内核 panic 返回、
`xl stop` 让内核交还、或显式给了 `XL_SECS=N`。

要看显示相关的效果，得在内核还活着的时候看，绿屏上什么也看不出来。

## 常用 boot 参数

```sh
XL_CMDLINE="gl=1 adreno=22 adreno_keep=1 touchscreen=1 bootcpu=7 cpus=8 rd=md0 battery=1"
```

- `gl=1 adreno=22 adreno_keep=1`：GPU 要这三个。`gpu=1` 在这台按设计被拒（那是 POCO 的
  Adreno 642L 的步骤表）；少了 `adreno_keep=1`，第 22 步做完就收摊、显示退回软件路径
- `bootcpu=7`：内核跑在大核，loader 留在自己核上继续应答。**要在一轮中间读日志就必须带**
- `cpufreq=1`：钉最高频，默认不带（发烫）
- `backlight=1 backlight_arm=1`：发布 `AppleBacklight` / `AppleARMBacklight` 服务
- `display_hz=60|90|120`：面板刷新率；`display_bl=1..0x7ff`：直接发一次 DCS 0x51 背光

完整清单在 `docs/ru/notes.md`。

## 亮度：控制中心能调了

### 控制中心能调亮度：缺的是 `backlight-control` 一个属性

**现象**：设置里拖亮度条，滑杆动、控制中心的数字也变，屏幕亮度不变；控制中心里那条拖不动。

**原因**：CoreBrightness 找背光服务的条件**不是类名**。它有三种控制器可选——
`BCAppleBacklightBrtControl`（真实）、`BCHIDBrtControl`（HID）、`BCVirtualBrtControl`（虚拟：只改 UI
数值、不碰硬件）。缺条件时它退回第三个，所以滑杆动、屏幕不动。

真正的条件是（来自 sheng 的 `AppleARMBacklight.cpp`，通过 asteroids 端口的
`package/AsteroidsBacklight.cpp` 读到）：**服务要有 `backlight-control = true`，且 conforms to
`AppleARMBacklight`**；range 从 `IODisplayParameters` 读；电平通过 `setProperties` 以**扁平的**
`{brightness, FadePeriod}` 送来（不是嵌套在 `IODisplayParameters` 下）。

`xnu/files/iokit/Kernel/MiniAppleBacklight.cpp` 现在：

```c
setProperty("NativeSandboxGrantProcess", "backboardd");   /* 沙盒允许 backboardd 写这些属性 */
setProperty("backlight-control", true);                    /* CoreBrightness 的匹配条件 */
```

`setProperties` 也接受扁平格式。改完实测：控制中心拖动，`dcs 51` 一次会话发出 **55 次**，
每个中间电平都到了面板。

**教训**：`IOServiceMatching` 类名对上了不等于 CoreBrightness 会用——它还要属性。遇到 iOS 侧
不认的服务，先去 [unixnu.com/devices](https://unixnu.com/devices/) 里搜别的机型的 `porting.toml`
（搜 `brightness`/`backlight`），比啃二进制快得多。这次就是从 `rootstockos/UniXNU-asteroids`
（Nothing Phone 3a）的 T37 记录里找到的。

### 读回（DCS 0x52）仍然不通，但不影响

```
display: brightness 1140: dcs 52 answered 00000000 ... no level in it
```

写亮度不依赖读回。vendor 自己也从不读背光（`dsi_panel.c` 里写着"理想情况下应该从面板读，现在只设
默认值"），所以这块面板支不支持读回从未被验证过。详见下面一节。

### 曾经能调、后来不能调：`CMD_DMA_CTRL` 的 bit 28 是借来的

`display_bl=N` 和 `display_bl_ramp=1` 有一夜是好用的，第二天同一份代码同一份参数就纹丝不动。
根因不在包格式、不在地址、不在时序，而在**一个从没被显式设置过的寄存器位**：

| 启动 | `CMD_DMA_CTRL` 原始值 | bit 28（embedded mode） | 亮度 |
|---|---|---|---|
| 10-31 凌晨 | `0x14000000` | **有** | 能调 |
| 10-31 深夜 | `0x4000001` | **无** | 不动 |

bit 28 决定命令引擎**是否去内存里取包**。没有它，引擎照样置 `CMD_DMA_DONE`、日志全绿，
但包根本没发出去，面板什么也没收到。

这套代码一直继承 bootloader 留下的值。vendor 的每个 kickoff 路径都显式写
（`dsi_ctrl_hw_cmn.c`：`reg |= BIT(28); reg &= ~BIT(24); reg &= ~BIT(29);`）。现在 `dsi_power_mode()`
每次发包前把这几位写死：

```c
ctrl &= ~((1u << 31) | (1u << 30));     /* broadcast off, master on */
ctrl |= (1u << 28);                      /* embedded mode：从内存取包 */
ctrl &= ~((1u << 24) | (1u << 29));     /* packet type 0, WC_SEL 0 */
```

改后 `0x4000001` → `0x14000001`，**屏幕能变暗了**（`display_bl=32` 实测可见）。

**教训**：`CMD_DMA_DONE` 只证明主机跑完一次传输，不证明包上了线。寄存器里"能读到就以为对"的位，
要么自己设，要么别信它。

### 读回：面板回的是错误包，而 vendor 从没实现过读

同一个 bit 28 的修复顺带让读回从"完全沉默"变成"有回应"：

```
dcs read 52: intr 0x100501, status0 0, timeout 0, ack err 0x1003b04, rdbk ctrl 0x40000, data 全 0
```

`rdbk ctrl 0x40000 >> 16 = 4`。一开始以为收到了 4 字节数据，继续查才看清：

- **`timeout 0`**：不是总线超时，控制器认为传输正常完成
- **`ack err` 的 bit 24** 是 `DSI_PERIPH_ERROR_PKT`——**外设错误包**
- MIPI 错误包正好 4 字节（DI + 2 字节错误码 + ECC）

所以那 4 字节不是数据，是**面板回的错误包本身**；硬件把它解析成 `ack err` 的位，所以
`RDBK_DATA` 里没有载荷。`ack err` 的低 16 位 `0x3b04` 应该就是面板报的错误码——vendor 只解码
bit 15/16/17/20/23/24，这几位没有名字。

换 `0x0a`（get power mode）和 `0x04`（get display ID）测试，**结果完全一样**。不是 0x52 特殊，
是所有 DCS 读都被拒。

**然后看到了这句。** vendor 自己的 `dsi_panel.c:1041`：

```c
case DSI_BACKLIGHT_DCS:
case DSI_BACKLIGHT_EXTERNAL:
case DSI_BACKLIGHT_PWM:
default:
	/*
	 * Ideally, we should read the backlight level from the
	 * panel. For now, just set it default value.
	 */
	break;
```

我们的面板正是 `DSI_BACKLIGHT_DCS`（`bl_ctrl_dcs`）。**高通自己写着"理想情况下应该从面板读，
现在只设默认值"。** 而且在整个 `techpack/display/` 里搜 `MIPI_DSI_DCS_READ`，只有一处，是
`dsi_ctrl.c:1093` 设置 BTA 标志的判断——**没有任何地方真正发起过 DCS 读**。

结论：这不是我们能修的 bug，是厂商留着没做的一段。写亮度可用（`display_bl=N`、
`display_bl_ramp=1` 都实测有效），读回要知道面板到底支不支持、需要什么前置条件，
只能查 k11a 的 datasheet——手上没有。

### 顺带修掉的一个自己写的 bug

`dsi_tpg_read()` 里 `ctrl` 变量先存 `CMD_DMA_CTRL`、后又被拿去读 `RDBK_DATA_CTRL`，最后那句恢复
**把 RDBK 的值写进了命令引擎的控制字**。现在用 `dma_ctrl_saved` 单独保存。

### 一路试过的（留作记录）

| 尝试 | 结果 |
|---|---|
| `0x05` 当读命令 | `status0 0x5`，面板拒绝 |
| 改成 `0x06`（DCS read） | `status0 0x0`，接受但仍无数据 |
| TPG FIFO（参考项目那条） | 传输 `done`，仍无数据 |
| `0x37` 与读命令批处理（vendor 的正确做法） | `ack err 0x1000800` |
| `+ RDBK` 双写复位 / `+ MDP` 空闲等待 / `+ HS` 模式 | 同上 |
| **显式设置 bit 28** | **写亮度恢复；读回字节数 0 → 4** |

`ack err` 的 bit 24 是 `DSI_PERIPH_ERROR_PKT`。vendor 树里从不读背光
（`dsi_panel.c`、`dsi_ctrl.c` 无 `0x52`），参考项目同样标着 "Not verified on hardware"。

