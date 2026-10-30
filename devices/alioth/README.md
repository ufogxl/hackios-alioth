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
  见 `hakios.goodix.c:246`，与真机 FDT 一致
- 音量上 = TLMM gpio6；面板 reset = gpio12

## 启动

```sh
./mkxnuimg.py -d alioth -p <payload.bin> -o boot.img   # UEFI + BootShim + payload
./xl enter                                             # 手机进 loader（从 Android 或 fastboot）
./xl run                                               # 上传内核、运行、取回日志
```

**`xl` 和 `mkxnuimg.py` 的默认设备是 `poco-x5-pro`**，而这台手机的 UEFI 在 `0x9FC00000`、大小
`0x300000`（POCO 是 `0x9FD00000` / `0x200000`），刷错镜像进不了 loader。所以在这个端口上，每条
`xl` 命令都要带 `DEVICE=alioth`：

```sh
DEVICE=alioth ./xl enter
DEVICE=alioth ./xl run
DEVICE=alioth make phone        # 或者一条命令：进 loader、上传系统镜像、跑内核、取日志
```

`make phone` 走 `tools/modem-cycle.sh`：它先 `xl ramdisk userland/system.apfs` 把 2.4 GB 的系统
镜像放进手机内存（一次性，约 90 秒），再 `xl run` 跑内核；**loader 重启过一次之后必须先做这一步**，
否则根分区是空的，内核会以 `Process 1 exec of /sbin/launchd failed, errno 2` panic。

`xl` 只调 PATH 里的 `fastboot` / `adb`。在 Linux 上直连手机时用系统的即可；在 WSL 里
系统的 fastboot 看不到 USB 设备，需要 `/usr/local/bin/fastboot` 那个转发到 Windows 的包装器。
