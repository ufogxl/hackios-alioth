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

## 亮度回读为什么读不到

写亮度正常（DCS 0x51）。**任何读命令都拿不到数据**，七种组合全部试过：

| 尝试 | `status0` | `ack err` | 数据 |
|---|---|---|---|
| 命令 DMA，`0x05` 当读命令 | `0x5` | 0 | 全 0 |
| 命令 DMA，改成 `0x06` | `0x0` | 0 | 全 0 |
| TPG FIFO（参考项目那条） | `0x0` | 0 | 全 0 |
| `0x37` 与读命令批处理 | `0x5` | `0x1000800` | 全 0 |
| `0x37` + RDBK 双写复位 | `0x5` | `0x1000800` | 全 0 |
| `0x37` + MDP 空闲等待 | `0x5` | `0x1000800` | 全 0 |
| `0x37` + HS 模式（`display_hs=1`） | `0x5` | `0x1000800` | 全 0 |

`ack err` 的 bit 24 是 `DSI_PERIPH_ERROR_PKT`——**面板回了错误包**，它收到了命令但拒绝执行。

换成 MIPI 最基础的读命令 `0x0a`（get power mode）测试，**一样被拒**。所以不是 0x52 不被支持，
而是这块面板在 cmd 模式下不接受任何 DCS 读。

**vendor 从不读背光**：`dsi_panel.c` 和 `dsi_ctrl.c` 里搜不到任何发 DCS 0x52 的地方。厂商自己
只写不读，所以“面板在什么条件下允许读”从来没有被验证过。参考项目的 `porting.toml` 里那条同样
写着 "Not verified on hardware"。

往下走需要 k11a 的 datasheet，或厂商初始化序列里有没有开启读的配置——两样手上都没有。

### 顺带修掉的一个真缺陷

`dsi_read()` 一直把 **`0x05`（DCS short write）当读命令**用，正确的是 **`0x06`**（DCS read）。
这影响所有走它的路径，包括 `display_read=N`。另外 `0x37` 不能单独发——vendor 的
`dsi_set_max_return_size` 清掉它的 last 标志，和读命令**批处理进同一次传输**。

代码里两条路都会走，失败时打 `no answer on either path`，不会假装读到了电平。
