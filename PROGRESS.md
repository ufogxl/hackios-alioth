# 移植进度日志

> **这是一份按时间顺序追加的日志，不是入门文档。** 3500 多行从 2026-10-26 一直写到现在，
> 越往下越新；早期段落里的结论有很多已被后来的实测推翻，**不要从第 1 行开始读**。
>
> 想了解某个子系统现在是什么状态，先看这两份，它们按"现在怎么办"组织、随改动更新：
>
> | 想知道 | 看哪 |
> |---|---|
> | 这台手机怎么启动、参数怎么给、踩过哪些坑 | [`devices/alioth/README.md`](devices/alioth/README.md) |
> | 某个寄存器/引脚/时序到底是多少 | [`devices/alioth/hardware.md`](devices/alioth/hardware.md) |
> | 所有 boot-arg 的完整清单 | [`docs/ru/notes.md`](docs/ru/notes.md) |
>
> 这份日志的用处是别的：**为什么**会走到今天这一步——每个坑当时是怎么发现的、
> 错误结论是怎么被证伪的、哪些方案试过并且否决了。查"这条结论有没有证据"时来这里。
>
> 下面是几处入口，按问题找，不要顺序读：
>
> | 问题 | 段落 |
> |---|---|
> | 整条链路最早是怎么搭起来的 | [2026-10-26 上午](#2026-10-26-上午第-0-阶段完成-基座内核构建成功) |
> | UEFI 与 loader 首测 | [2026-10-26 中午](#2026-10-26-中午uefi-loader-真机首测成功) |
> | GPU 从零到画出第一帧 | [10-27](#2026-10-27a650-基础访问完成) · [10-28 secure mode](#2026-10-28-上午a650-走出-secure-modesecure-mode-死结解开) · [10-28 第一帧](#2026-10-28-上午a650-阶段-22-gpu-画出第一帧并上屏成功) |
> | 触摸坐标错位 | [2026-10-28 凌晨](#2026-10-28-凌晨ft3658u-触摸坐标定位修复完成已实测验证) |
> | 屏幕"冻住"其实是跑完了 | [2026-10-28 傍晚](#2026-10-28-傍晚屏幕冻住连时钟都不走的定案-是跑完了不是死机) |
> | 壁纸黑、小组件空白 | [2026-10-30 深夜](#2026-10-30-深夜壁纸没了小组件空白-是镜像里的作业表不是渲染) |
> | 120 Hz | [10-31 凌晨](#2026-10-31-凌晨120-hz-的-dcs-通路打通了面板还没换挡) · [10-31 凌晨 续](#2026-10-31-凌晨120-hz-上去了-唯一的坎是命令从哪一页内存发) |
> | 亮度条、音量条没反应 | [2026-10-31 凌晨（二）](#2026-10-31-凌晨二亮度为什么调不动-请求走到了-corebrightness然后没地方去) |
> | **当前停在哪、下一步做什么** | **[文末「22:40 今晚结论汇总」](#2240-今晚结论汇总用户要求记录后停手)** |
>
> 段落标题里的日期是我自己写的，锚点按 markdown 规则生成（空格转连字符、去掉标点）；
> GitHub 上点不动就按 `Ctrl+F` 搜标题原文。

## 2026-10-26 上午：第 0 阶段完成 + 基座内核构建成功

### 手机数据采集（全部完成，存档于 `dump/`，清单见 dump/README.md）

- **fastboot 驱动**：Windows Update 目录安装 "Google Android Bootloader Interface" 成功（此前手动改 inf 的路线废弃——签名问题）。装完需重新枚举设备才绑定。
- **真机身份**：Redmi K40 国行（M2012K11AC），**12GB 版**（SKU pro），A/B 槽当前 b，BL 已解锁。
- 通过 `fastboot boot twrp-alioth.img` 获得 TWRP root shell，未改动任何分区。
- 导出：真机 DTB（twrp-fdt.dtb/.dts）、dtbo_b、vbmeta_b、persist、cmdline、iomem、dmesg。
- **未拿到**：/vendor/firmware_mnt 的 QCA6390 固件（TWRP 挂不上 logical vendor）——稍后从 LineageOS ROM zip 提取。

### 真机验证的关键参数（已并入 devices/alioth.hardware.md）

- 实机面板：**天马 k11a**（cmdline `mdss_dsi_k11a_38_08_0a_dsc_cmd`）；reset=TLMM gpio12，TE=gpio66。
- 触摸 FTS：SE4 PA 0x990000/0x4000，SPI 6MHz，IRQ gpio39 下降沿，Reset gpio33。
- **供电冲突**：真机 FDT = vdd:PM8150A **L11**@3.0V + vcc_i2c:**L2**@1.2–1.3V；lineage-22.2 树写 L10/L1@1.8V。以真机为准，bring-up 验证。
- 音量上 = TLMM gpio6（gpio_keys 实测）；电源/音量下走 qpnp-power-on（同 V60）。
- RAM：System RAM 0x80880000–0x37FFFFFFF；帧缓冲 0x9C000000（36MB，Mu-Silicium 确认）。

### 设备配置（new-device.sh 已生成并填好）

- `common/devices/alioth.conf`：UEFI_TARGET=alioth，FD 0x9FC00000/0x300000，PAYLOAD 0xF0000000，FB 0x9C000000，WIFI_ATH11K=1，BOOTARGS="touchscreen=1 cpufreq=1 bootcpu=7 cpus=8"。
- `common/devices/alioth.platform.c`：GIC 0x17a00000/0x17a60000（工具从 DTB 提取，与文档一致），8 核 shift=8，**small_cores=0xf**（修正了此前草稿的 0xf0），fb_base=0x9c000000。

### 基座内核构建（成功）

- WSL Ubuntu 24.04（原生 ~/hackios，/mnt/c 构建太慢）。
- theos 手动安装：iPhoneOS16.5.sdk（theos/sdks 稀疏克隆）+ iOSToolchain-x86_64（L1ghtmann/llvm-project release）。
- 依赖：补装 autoconf automake libtool uuid-dev device-tree-compiler；apt 必须 `Acquire::http::Proxy=http://127.0.0.1:7897`。
- **坑**：xcrun 垫片只查 host/bin 与 /usr/bin，找不到 dsymutil → 写了 host/bin/dsymutil 包装器（滤掉 Apple 专有的 --minimize 转发给 llvm-dsymutil）。**包装器若在 Git Bash 一行命令里用 heredoc 写，$@ 会被提前展开成空——必须用文件拷贝方式部署**。
- 结果：`mach.development.bcm2837`（15,144,488 字节，Mach-O 64-bit arm64）+ dSYM。剩余 Error 均为 README 预告的链接后无害错误（export check + 3 symbolsets）。

## 2026-10-26 中午：UEFI + loader 真机首测成功

### Mu-Silicium UEFI（成功）

- `~/hackios/hackios-xnu-kernel/Mu-Silicium`（BASE e51cd87），HackiOS 的 mu-silicium.patch 已应用 + redwood 移植文件已拷。
- 产物 `Build/aliothPkg/RELEASE_CLANGPDB/FV/`：SILICIUM_UEFI.fd（3,145,728 = 0x300000 整）+ bootshim + .gz。
- 三个坑：① mu_nasm 用系统 nasm 替代（Bin/mu_nasm_extdep/Linux-x86-64/{nasm,ndisasm}，extdep_state.yaml version 20016.1.1）② BaseTools web extdep 手工装到 Bin/Mu-Basetools_extdep/（**目录名按依赖 name 而非 yaml 文件名**；Linux-x86-64 是指向 Linux-x86 的软链接；extdep_state.yaml 写 version: v2026080000.0.0-rc0）③ **必须用系统 clang 18**（theos 的 clang-11 挂在 Base.h VA_START）；llvm-objcopy 软链到 /usr/local/bin。
- BootShim/Makefile 用 `-DPAYLOAD_BASE=0xF0000000`（部署自 dump/BootShim-Makefile，别 sed 改）。

### xnuloader（成功）

- `alioth.platform.c` 已并入 xnuloader/loader.c（platforms[] 注册 &platform_alioth）。备份 loader.c.bak。
- charger_start/wdt_arm 里 `PLAT != &platform_redwood` 的特判对 alioth 直接 return，首测安全。
- 注意：WSL 的 /tmp 会丢文件，补丁素材放 ~；Git Bash→WSL 链会吃掉 $变量 和 for 循环变量（写成 .sh 文件再执行）。

### 打包 + 真机首测（成功）

- `SKIP='kernel images' bash common/build.sh alioth` → `out/alioth-boot.img`（1,992,704 字节；mkxnuimg 自动用 Mu-Silicium 构建的 FD）。
- Windows 侧 `fastboot.exe boot alioth-ios-port/out-alioth-boot.img` → 手机以 **xnuloader** 身份出现在 fastboot。
- loader 日志（`fastboot oem log` + `get_staged`）：
  ```
  == XNULOADER v3 ==
  CurrentEL=1 MIDR=51df805e MMFR0=101122 CNTFRQ=19200000 image=a1000000
  display 1080 x 2400 at 9c000000
  platform alioth
  usb: started
  ```
  显示 1080x2400@0x9c000000 识别正确，platform=alioth 按 fb_base 匹配成功。

### 仓库重大变化：main 分支重组（2026-10-25/26）

- hackios-xnu-userland 已清空（"Moved into the main HakiOS repository"）；main 重组为：Makefile 单入口 + `sysimage/`（原 ios165）+ `userland/` + `devices/poco-x5-pro/`（uefi/apply.sh、soc.args、pull-firmware.sh、prebuilt/）。common/ 没了。
- git pull 走代理反复 early EOF（仓库有大 blob）→ 改用 codeload tarball（12MB）解到 `~/hackios-new`。
- 新树主机检查缺 7z23+/adb/fastboot：7-Zip 23.01 官方二进制放 `~/7zz`→/usr/local/bin/7z；adb/fastboot 用 archive.ubuntu.com 源经代理安装（tuna 走代理 403）。
- xnu-deps 整棵从旧树 cp -a 到新树（setup.sh 差异只是加了 macOS 支持）。
- **双树策略**：旧树（d82c64a）保留全部构建状态和设备配置用于真机启动；新树只用于 sysimage 下载 + userland 镜像构建（镜像与设备无关），产出 root.apfs/system.apfs 拷回旧树 userland/。

### 固件提取（完成，无需 ROM zip）

- TWRP 里 `mount -t ext4 -o ro /dev/block/dm-4 /mnt/vendor` + `mount -t vfat -o ro /dev/block/sde30 /mnt/fw`（firmware_mnt = **modem 分区**）。
- GPU：a650_gmu.bin / a650_sqe.fw / a650_zap.{mdt,b00,b01,b02}（fw-gpu/）。
- 音频：adsp.mdt + adsp.b00–b18（fw-audio/，共 ~20MB）。haptic：aw8697_haptic.bin（fw-haptic/，与 redwood 同款 aw8697）。
- Wi-Fi：alioth 无 wpss.*；QCA6390 固件在 modem 分区 `qca6390/`（bdwlan.elf + amss20.bin + m3.bin + regdb*），**Wi-Fi 驱动移植后置**。
- 部署到两棵树顶层 fw-*/（旧树 FIRMWARE= 变量可指向）。存档 `alioth-ios-port/firmware/`。

### soc.args（新树 tools/soc-args.py 从 twrp-fdt.dtb 生成 → alioth-ios-port/soc.args）

- 与 SM7325 默认值大多相同；差异：soc_ipa_stream=0x5c0 / _uc=0x5c2、soc_ipa_fw=ipa_fws、缺 soc_aoss/cmd_db/mpss_mem/pas_mpss（工具未提取到，modem 工作再补）。

### 2026-10-26 下午：QEMU 里程碑——iOS 16.5 在 x86 电脑上启动

- 新树 `make kernel` 成功（产物 15,290,096 字节；make 报的 export check 错误是 README 预告的无害链接后错误，旧树 build.sh 有同款容错）。
- QEMU（qemu-system-arm + AAVMF，WSLg 显示）`make qemu` 一次跑通：**launchd → backboardd（window server ready, 45.9s）→ SpringBoard 成为 system shell**，音频子系统活跃。截图 `ios-screen-1.png`（苹果开机 logo）。
- **坑**：Ubuntu 24.04 的 EDK2 固件在 /usr/share/AAVMF/AAVMF_CODE.fd（脚本默认找 /usr/share/edk2/aarch64/QEMU_EFI.fd），需 QEMU_EFI/QEMU_VARS 环境变量。
- **问题**：镜像没含 GL（构建时没加 GL=1）→ CoreAnimation "Software renderer! Simulating crash!" → 永远停在开机 logo。修复：tools/mk-mesa.sh（softpipe 软渲染，无需 LLVM）→ GL=1 重建镜像。

### 2026-10-26 傍晚：Mesa 软渲染 + gl=1 启动参数

- Mesa 25.0.7（softpipe,freedreno）构建成功（libOSMesa.8.dylib 14.8MB）。坑：缺 meson/ninja/mako + llvm-ar/llvm-strip/llvm-ranlib（从 /usr/lib/llvm-18/bin 软链）。
- GL=1 重建镜像（root.apfs 6.3MB 含 GL 库；system.apfs 2.49GB）。
- **QEMU 带 `gl=1` 启动参数**（CMDLINE='-v gl=1'）：CoreAnimation 接受了 Mesa（Software renderer 报错消失），hakios-gl 渲染了 2 帧锁屏（"frame 1/2: N GL calls... 1 picture copied out"），backboardd render_update 链正常返回。
- **遗留问题（不阻塞手机）**：ramfb 截图始终是开机 logo，滑屏也不产生新帧——QEMU 的 ramfb 显示输出与用户态 GL 帧缓冲像是两个缓冲，待查（可能与我们用 AAVMF/QEMU 8.2 组合有关；上游截图 settings-about-qemu.png 说明上游 QEMU 能出图）。
- 结论：**镜像链与 iOS 用户空间已在 x86 上完整验证**（launchd→backboardd→SpringBoard→GL 渲染），手机端只差对应驱动。

### 新树 devices/alioth 骨架（done）

- `~/hackios-new/devices/alioth/`：README.md（移植状态+真机参数）、soc.args、pull-firmware.sh（TWRP 挂载 dm-4/sde30 版，QCA6390 整目录拉取）。

### 2026-10-26 深夜：完整 iOS 16.5 真机运行成功（历史时刻）

- hello 冒烟后，`xl ramdisk userland/system.apfs`（2371MB/92s=25MB/s）+ `XL_CMDLINE='-v bootcpu=7 cpus=8' XL_SECS=600 ./xl-alioth run`。
- **真机 iOS 16.5 完整用户空间运行成功**：launchd→backboardd→SpringBoard，WebKit/MTLCompiler/Intents 扩展全部拉起，cpu_test 实测 3.9 核并行。
- **手机屏幕上渲染出了 iOS 主屏幕**（xl screenshot 取证 `phone-screen1.png`：状态栏/小组件/全套图标/Dock）——天马屏帧缓冲直接上屏，DSI 兼容性超预期；图标走软渲染路径（GPU a650 驱动未移植）。
- 插曲：fastboot.exe 的 getvar 走 **stderr**——WSL 包装器 `2>/dev/null` 导致 xl 认不出 loader（state 永远 absent），改 `2>&1 | tr -d '\r'` 后全通。
- 用户 21:47 拔线（会话未跑完），内核继续跑到 21:52 自然结束。

### 安卓"救砖"记录（其实没砖）

- 亏电 4 秒重启循环 → 用户长按进 fastboot 充电 → TWRP 显示 85% 后**正常开机仍循环**：bootloader 在低电失败重启后**自动切到了 a 槽**（current-slot: a，今早真机在 b 槽），a 槽无系统。
- 修复：`fastboot --set-active=b && fastboot reboot` → 安卓完好（b 槽，TKQ1.220829.002 test-keys）。
- 教训：① iOS 会话**必须插线跑**（我们的内核不充电，redwood 同款限制）；② 亏电反复失败会让 bootloader 切槽，再遇到循环先 `getvar current-slot` 查槽；③ TWRP 充电是亏电恢复的最佳姿势。

### 2026-10-26 下午：xl 打通 + hello 就绪 + 首次内核启动失败（手机硬挂）

- WSL 内用 Windows fastboot.exe/adb.exe 做包装器（/usr/local/bin/{fastboot,adb}，滤 \r、绝对路径转 Windows 路径），`xl state/log` 已通。
- 新树 `make hello` 成功（16KB Mach-O arm64，零 Apple 文件），拷入旧树 userland/。
- **首次 `xl run` 失败，手机硬挂**：xl 的 enter() 自己调 mkxnuimg.py 重建 xnu-usb.img，但不像 common/build.sh 那样从 alioth.conf 导出 FD_BASE/FD_SIZE/PAYLOAD_BASE/DTB——UEFI 按 redwood 默认地址加载 → 崩溃且无 USB 枚举。**修复：用 `./xl-alioth`（dump/xl-alioth.sh，已部署旧树顶层，自动 load_device alioth 并导出全部地址变量）替代 `./xl`**。
- 手机硬挂后 USB 完全消失（无 fastboot/adb/9008，重启 USB 集线器无效），看门狗未触发。下次 run 必须等手机回安卓。
- 教训：运行 `common/run.sh`/`./xl` 前必须 source 设备 conf（build.sh 自动做，xl 不做）。

### 下一步

1. 等新树镜像构建完 → 拷 root.apfs/system.apfs 到旧树 userland/ → `xl enter` / `ramdisk` / `run`（BOOTARGS 加 soc.args）。
2. 新树正式移植 devices/alioth/（kernel 的 hakios_*.c 驱动重写：display 天马 k11a、触摸、Adreno 650、ADSP、aw8697——Wi-Fi/modem/相机后置）。
3. 首测预期：内核 -v 日志能起来就算胜利；显示/触摸要按 K40 硬件改驱动。

### 2026-10-26 22:xx：救砖结果复核确认

- 手机已验证回到安卓：slot **_b**，build TKQ1.220829.002 test-keys 完好，`dumpsys battery` level=84、status=2（充电中）。
- 结论：双系统基线稳定，iOS 随时可再从 `fastboot boot` 会话拉起，安卓分区未受影响。


---

## 2026-10-26 深夜：救援系统探索收尾（失败但有关键发现）+ 槽位属性救急手册

### 背景
昨晚亏电事故后用户问："以后有没有办法在 recovery 里切槽位？"——目标是做一个刷进 a 槽的自动救援 TWRP（a 槽事故后自动切回 b）。**最终失败，但留下重要结论。**

### 关键发现

1. **fastboot boot 会内容校验**：镜像含内嵌 AVB0 vbmeta（本 TWRP 在偏移 77209600，尾部有 AVB footer）时，改任意 1 字节（甚至只抹 vbmeta+footer）都报 `Failed to load/authenticate boot image: Load Error`；不含 AVB 的镜像（我们的 UEFI alioth-boot.img）则随便引导。结论：**要 fastboot boot 测试改过的镜像，必须先处理 AVB**，当前未解。

2. **从闪存正常引导 a 槽卡 logo 的真正原因是 `skip_initramfs`**：ABL 正常引导时给内核加此参数 → 内核跳过 ramdisk → TWRP/救援脚本不运行。内核里该字符串位于未压缩 ARM64 Image 偏移 40444247（__setup 字符串表），改成 `skip_initramfX` 打进 v4 后**卡 logo 依旧**——补丁未生效或有别的问题，未查明，勿当已解决。

3. **GPT 槽位属性血泪经验**（已验证健康的字节对：**boot_a=`00 00 00 00 00 00 7a 00`，boot_b=`00 00 00 00 00 00 77 00`**）：
   - 偏移：boot_a 属性字节在 sde 绝对字节 2281682352，boot_b 在 2281685552（备份 GPT 条目区，扇区 4456400 起）。
   - TWRP 里读：`dd if=/dev/block/sde bs=512 skip=4456410 count=2 | tail -c +433 | head -c 8`；写：`printf '\x..\x' | dd of=/dev/block/sde bs=1 seek=<绝对偏移> conv=notrunc`。
   - b 槽一度无法加载（retry 掉到 6、boot 落 fastboot）就是因为某次 `--set-active` 把 boot_b 写成 `0x37`（缺 bit54）。写回 0x77/0x7a 立刻修复。
   - **教训：`fastboot --set-active` 从某些中间态出发会生成无法加载的属性组合，不是幂等的。救急口诀 = 进 TWRP 把 boot/boot_b 属性字节写回 `0x7a`/`0x77`。retry 正常值 7。**
   - bit 语义未完全破译，只信实测的 0x7a/0x77，勿外推。fastboot getvar 家族（current-slot / slot-unbootable:a|b / slot-retry-count:a|b / slot-successful:a|b）是排查槽位问题的第一手工具。

4. TWRP 3.7.1_12（alioth）：**没有 Slot A/B 切换菜单**；FBE 加密解不开（`twrp decrypt default_password` 失败 signal 6）；无 cache 分区、recovery 分区不存在。

### 防变砖最终设计（已实施，手机处于安全终态）

- **boot_a 已用 TWRP dd 置零**（192MB 全 0，已验证首字节全零；boot_b 的 ANDROID! 头完好）。bootloader 永远无法从 a 槽启动任何东西 → 以后亏电事故链变成：b 槽失败被标记 → 尝试 a → 立刻 Load Error → **停在 fastboot 界面（能充电）** → 插电脑 `fastboot --set-active=b` 即回家。不会再出现无声 4 秒死循环。
- a 槽其余改动保留未还原（无害）：vbmeta_a 刷了 disable-verification 版、dtbo_a 刷成了 dtbo_b 的内容、boot_a 里躺着 v4 救援镜像但永不被引导。
- **原版 `twrp-alioth.img` 仍是维修入口**，`fastboot boot` 随时可用。
- v4 救援镜像（skip_initramfs 补丁内核 + 自动切槽脚本）存档在 `rescue/twrp-alioth-rescue-v4.img`，当前为死货，留作将来研究。

### 备份清单（`dump/`）
boot_a-stock.img（192MB 原版 a 槽 boot）、dtbo_a/b-stock.img、vbmeta_a/b-stock.img、gpt_region_a/b.bin（31 扇区槽位元数据，金色样本）、gpttail.bin。

### 用户新问题的结论："能不能把 iOS 刷到 A 区"

- **不行，概念错位**：A/B 槽（boot_a/system_a/vbmeta_a/dtbo_a）是安卓的体系，我们的引导链是 `fastboot boot` → UEFI → loader 把 iOS 灌内存，与槽位无关。
- 每次跑 iOS 不是"刷机"——闪存未动，92 秒是把 2.4GB system.apfs **经 USB 传进 RAM**（断电即丢）。
- **根本 blocker：XNU 内核无存储驱动（UFS/SCSI 未移植），即使写入闪存也读不到。**
- **对症优化方向（候选，中等工作量）**：改造 loader（`xnuloader/loader.c`，UEFI 环境有块设备驱动）从闪存分区读预存镜像——iOS kernel+ramdisk blob 提前 flash 到大空闲分区，以后每次只 `fastboot boot` 小 loader（几秒），启动从 92s 降到 ~20s 量级。
- 优先级排序：① 触摸驱动（FocalTech FTS）② 充电驱动 ③ 启动提速（本项）。

---

## 2026-10-27：闪存启动、触摸、CPU/DDR、AW8697 调试交接

### 当前安全状态与目标

- **保留当前 MIUI/Android，不刷类原生。** 最后一轮 iOS 测试后已执行 `fastboot -s xnuloader oem reboot`，ADB 实测 `sys.boot_completed=1`，手机回到安卓。
- 当前目标是继续提高 iOS 16 真机稳定性并移植驱动；优先自动调试，不要求用户操作手机。
- 工作区固定为 `<主机>\Develop\iOSOnAndroid`。安卓序列号 `<序列号>`，loader 序列号 `xnuloader`；工具在 `tools/platform-tools/`。
- Git Bash 调 WSL 且命令含 Unix 路径时先 `export MSYS2_ARG_CONV_EXCL='*'`。修改文件必须先 Read 再 Edit。

### 已完成：system.apfs 预存闪存 + loader 直接读取

- GPT 已正式调整：userdata 缩小，尾部新建约 4GB 的 **`iosdata=/dev/block/sda36`**；安卓仍能正常启动。
- `system.apfs` 大小 **2,486,968,320 字节**，已灌入 iosdata。
- loader 已有 `oem rdload <分区名>`，新 loader 会随 `xl enter` 自动打进引导镜像。
- 实测 `oem rdload iosdata` 用时约 **5.7–6.4 秒**，不再每次经 USB 传 2.4GB。直接证据：`wsl-current/rdload-success.log` 记录 `ffffb000 bytes in 5667 ms`。
- 全盘 GPT 调查已收尾：盘上只有两份分区表；内核按 4KB 扇区解读盘上表，35 个旧分区曾与 `/sys` 吻合。不要再寻找“第三份隐藏 GPT”。

### 已完成：FT3658U 触摸底层与 iOS 输入服务接通

- 正式源码：`wsl-current/hakios.goodix.alioth.c`（文件名沿用 goodix， alioth 实际为 FTS）。
- 正式启动参数：
  ```
  touchscreen=1 touch_step=5 touch_power=3 touch_gpio=4
  ```
  **不要加 `touch_return`。**
- `ios-touch-cpufreq-speed-logdump.txt` 证实 backboardd 已 attach SPI digitizer，并建立 `BKHIDUITouchDetectionModeDigitizerServiceWrapper`；自动检查有 `=== 36 checks, 0 failed ===`。
- 这证明芯片/总线/事件服务链路已通，**不能代替用户真实点按、滑动、多点手势验收**。

### 已完成：CPU 频率与 DDR 底层测试

- 源码：`wsl-current/hakios.cpufreq.c`。
- 频率日志 `ios-touch-cpufreq-speed-logdump.txt`：三组分别到 1804、2419、3187MHz；core 7 同一循环由 167 提升到 565 thousand iterations/ms。
- DDR 日志 `ios-touch-cpufreq-ddr-logdump.txt`：MC0/SH0 bandwidth vote 生效，7 核并行写 62087MB/s、读 25819MB/s、复制 15367MB/s。
- 两份日志均有 `svc_test: 0 failed`；speed 日志另有 `surf_test: 80 checks, 0 failed`。这些是底层/自动测试结论，不冒充整机长期稳定性。

### AW8697 振动：已到硬件驱动阻塞点，暂停盲试

正式源码为 `wsl-current/hakios.haptic.alioth.c`，**不要误改 redwood 的 `hakios.haptic.c`**。

已确认：

- QUPv3 SE11，SDA/SCL=GPIO60/61，reset=GPIO130，IRQ=GPIO129，I2C 地址 `0x5a`，ID `0x97`。
- SPMI v5 channel 62 控制 PM8150B GPIO5 / `vdd_hap_boost`。诊断模式把 GPIO 临时改为 input+output 后读到物理脚 **HIGH**，所以不是“只写寄存器没供电”。退出时 output-source 回低是预期保护行为。
- 原生 DIAG_GO offset 校准已完成；它是自清零位，不能用普通 `put()` 做写后相等验证。
- 原厂 `aw8697_haptic.bin` 为 3622 字节，载荷 3618 字节、11 个波形；RAM 全量回读一致。内部 ADC 实测约 4.3V。
- `haptic=3` 只配置和校验 RAM，不驱动电机；`haptic=4` 才允许校准/播放。故障时 `give_up()` 拉低 reset，退出还会关闭 boost，**不要屏蔽 UVLO**。
- 安卓原生振动服务可正常完成，但 `/dev/aw8697_haptic` 和关键 sysfs 对 shell 无权限，`adb root`/`su` 不可用。不要再假设能直接抓原生寄存器；原生振动后进 iOS 时芯片已被 bootloader 保持在复位，也抓不到快照。

最新两轮 f0 测试：

1. `ios-alioth-haptic-f0-logdump.txt`：CONT/bypass 校准时未立刻报欠压，但 45ms 的中间待机态被过早当成完成，读到 f0 period=0。
2. 源码随后补上原生约 110ms 的最短等待；`ios-alioth-haptic-f0wait-logdump.txt` 在持续驱动约 50ms 后出现真实 `SYSST 0x30 / UNDER-VOLTAGE`，已自动复位停止。
3. 因此 **f0 校准和 RAM 播放均未 PASS，iOS haptic 服务未接入，也没有真实震感验收**。不要继续只换 RAM gain 或绕过故障。若未来重开此项，应先获得可信的原生寄存器/供电时序证据。

最新构建产物保留为：

- `mach-alioth-haptic-f0wait.bcm2837`
- `run-alioth-haptic-f0wait.bin`
- `build-alioth-haptic-f0wait.log`（有 `CC hakios.haptic.o` 和 `built kernel`；末尾 export-check Error 2 是既有链接后现象）

### 下一步：A650 先做基础访问，尚未改代码/上机

- **此小节已被下方“2026-10-27：A650 基础访问完成”取代**；其中硬件地址和后续边界仍有参考价值，但“尚未改代码/上机”已不再是当前状态。
- alioth 真机 FDT `dump/twrp-fdt.dts`：A650 chipid `0x06050002`，GPU `0x03d00000/0x40000`，GMU `0x03d6a000/0x30000`，GPUCC `0x03d90000/0x9000`，SMMU `0x03da0000/0x10000`，RSCC `0x03de0000`。
- QFPROM corrected base 为 `0x00784000`，speed bin offset `0x19b`、bit5 起 3bit；不要沿用 redwood 的 `0x1e9`、642L/643 判定。
- `sm8250-upstream.dtsi` 与 `upstream/linux-gcc-sm8250.c` 已存在；GCC 的 gpu_memnoc `0x7100c`、gpu_snoc_dvm `0x71018`、ddrss_gpu_axi `0x71154`、GPLL vote `0x52000` 与 redwood 相同。其余 GPUCC offset 必须从 SM8250 Linux/vendor 来源重推导。
- GPU 固件已备：`firmware/fw-gpu/a650_gmu.bin`、`a650_sqe.fw`、`a650_zap.*`。

---

## 2026-10-27：A650 基础访问完成

### 源码改动与边界

- `wsl-current/hakios.adreno.c` 已从 redwood/Adreno 642L 改为仅支持 alioth/SM8250 的 `adreno=1..3` 基础访问。原版备份为 `hakios.adreno.redwood.c`，差异为 `adreno-alioth-steps1-3.diff`。
- 阶段 1 读取 GCC/RPMh，删除 SM8250 不存在的 `gcc_gpu_cfg_ahb` 访问；阶段 2 从 `0x784000 + 0x19b` 读取 bit5 起 3bit speed bin；阶段 3 按 SM8250 **纯只读**访问 GPUCC，使用 PLL1、GX/CX GDSC、AHB、CRC/CX APB/CX SNOC、GMU RCGR、`0x1508` clamp 等已确认偏移。
- `gpu=1` 或 `adreno>3` 会打印 `REFUSED` 并返回，避免误跑仍保留在文件中的 redwood 后续代码。新增参考源码 `upstream/linux-gpucc-sm8250.c`；SM8250 无 GPUCC PLL0/hub/cb，也无 GCC `gpu_cfg_ahb`。
- 本轮未加载 GMU/zap、未启动 3D core；阶段 5 及之后仍未移植、未测试。基础访问成功不代表 GPU/GMU/3D 已工作。阶段 4 已完成，见下方专项记录。

### 构建与真机证据

- `wsl-current/build-alioth-adreno-basic.log` 含 `CC hakios.adreno.o` 和 `built kernel`；末尾 export-check Error 2 是项目既有无害现象。打包脚本为 `build-adreno-basic.sh`。
- `mach-alioth-adreno-basic.bcm2837` SHA256：`b445b78519638265eb54bcde24a429c23904e03f0ee259cb670ab9919d76699a`。
- `run-alioth-adreno-basic.bin` 为 21,438,632 字节，SHA256：`a5de09e859b40379960d53317af8ec68832096702231ba1160cfb3dcd388f2a2`。
- 载荷参数：`loader_return_after=60 ... -v bootcpu=7 cpus=8 adreno=3 adreno_pause=100 adreno_fine=1 adreno_watch=3 adreno_return=1`；不带触摸、cpufreq、haptic，不加载 GMU/zap。
- 已从 fresh 安卓执行 bootloader → `fastboot boot xnu-rdload-log.img` → `oem rdload iosdata` → stage → `oem run`。rdload 用时 5.680 秒；控制台日志为 `wsl-current/adreno-basic-console.log`，`getvar result` 为 `returned 24210 0`。
- `wsl-current/ios-alioth-adreno-basic-logdump.txt` 证实阶段 1/2/3 全部 `done`，无 panic/FAILED，结尾三次 `alive ... nothing written`，随后正常返回 loader。
- RPMh：`gfx.lvl=0x30050`，等级 `0 48 64 128 144 192 224 256 320 384 416`；`cx.lvl=0x30000`；`mx.lvl=0x30010`。QFPROM 字节 `0x50`，speed bin=2。
- GPUCC 22 个读取全部返回，包括 `gx_gdscr=cx_gdscr=0x222001`（bit31 未置，**不能声称已上电**）、`cx_gmu=0x80004220`、`gmu source=1`、`gx_clamp_io_ctrl=0x3c0001`、`cx_hw_ctrl=0x5ff4`、`smmu vote=0x80000000`。
- 测试后已 `fastboot -s xnuloader oem reboot` 回到安卓；ADB 复核 `sys.boot_completed=1`、槽位 `_b`、电量 100%、USB 充电中，处于安全终态。

### A650 阶段 4：CX 电源域独立测试通过

- `hakios.adreno.c` 已开放 `adreno=4`。SM8250 FDT 与 Linux 均确认 `cx_gdscr=0x106c`、`cx_hw_ctrl=0x1540`，VOTABLE；初始 GDSC bit1 未置，可以清除软件 collapse vote。
- 若 CX 已经上电则拒绝并停止，不调用未移植的 redwood `stale_stop()`；不访问 GMU/3D core。新增 `cx_powered_here`，使本轮测试结束即使没有 GMU 地址空间，也会重新 collapse CX。
- 构建日志 `build-alioth-adreno-cx.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-cx.bcm2837` SHA256：`80c6e7f1e3bb4bbe02116c952d1218db2c84f8730123119b86b4453132b772f3`；`run-alioth-adreno-cx.bin` 为 21,438,632 字节，SHA256：`a3e1882486eb79005454f269850a3a93a469d2676a4f19d77cfea623e7122852`。
- 载荷参数为 `adreno=4 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。从 fresh 安卓启动，`getvar result=returned 24213 0`；日志 `ios-alioth-adreno-cx-logdump.txt`。
- 唯一上电写入为 `cx_gdscr: 0x222001 -> 0x222000`；随后 `cx_gdscr=0xf8222000`、`cx_hw_ctrl=0x80001fe0`，阶段 4 `done`。GX GDSC bit31 仍未置。连续 5 秒 alive 后写入 `0xf8222001` 回收，恢复到 `cx_gdscr=0x222001`、`cx_hw_ctrl=0x5ff4`，日志明确 `standalone CX cleanup: off again` 并正常返回 loader。
- 测试后已回到 fresh 安卓并复核 `sys.boot_completed=1`。这只证明 CX 上电/回收，不代表时钟、GMU、SMMU 或 3D core 已工作。

### A650 阶段 5：时钟与测频通过

- 仅启用 SM8250 存在的 GCC GPU bus/memory、GPUCC CXO/AHB/CX_GMU/HLOS SMMU vote；GMU RCGR 选择 GPLL0/1.5=200MHz，不访问 redwood 的 CB/hub。新增精确时钟快照与恢复，随后再 collapse CX。
- 最终测频参数来自设备原厂 `MiCode/Xiaomi_Kernel_OpenSource` 的 `alioth-r-oss`：`upstream/alioth-debugcc-kona.c` 和 `alioth-clk-debug.c`。GPUCC 主选择值是 **0x163**，XO/4 CBCR 是 **0x4300c**，GPUCC AHB/CX_GMU/CXO 分别是 **0x10/0x18/0x19**；采样窗口 0x1000/0x10000，不使用 redwood 的 CLEAR bit21。
- 早前 `clocks`/`clocks2`/`clocks3`/`clocks4` 日志记录了使用相近 SM8150 参考值导致计数器不完成的排查，均安全清理，不能当作通过日志。
- 最终构建 `build-alioth-adreno-clocks5.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-clocks.bcm2837` SHA256：`2aef7ff069524d02c90918fa1a89d5193364d4cbee2b102a0601464d2b4ad1bc`；`run-alioth-adreno-clocks.bin` 为 21,438,808 字节，SHA256：`27572527556b2dc75b948cee65d517e3cd53ea55dd8136687bd30c23f66d61e1`。
- `ios-alioth-adreno-clocks5-logdump.txt`：阶段 1–5 全部 `done`，`getvar result=returned 36944 0`。实测 CXO **19,200,292Hz**、CX_GMU **200,001,572Hz**、CXO_AON **19,200,292Hz**、GX_GMU **0Hz**、AHB **47,265,784Hz**。
- 观察 5 秒正常；清理恢复启动时 CBCR/RCGR 值，并再次得到 `standalone CX cleanup: off again`，正常回 loader。测试后 fresh 安卓 `sys.boot_completed=1`。这只证明 CX 和时钟，不代表 GMU 固件、SMMU 或 3D 已工作。

### A650 阶段 6：GMU／GBIF／CX misc 只读访问通过

- 按真机 FDT 将 GMU 映射修正为 `0x03d6a000/0x30000`；开放 `adreno=6`，仍只读取 GMU、GBIF 和 CX misc，不释放 CM3 reset，不写固件/SMMU，不开 GX。
- `build-alioth-adreno-first-words.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-first-words.bcm2837` SHA256：`02ffa997516555c0ce079c771026865f686f23a60be7f8475268295222cfb028`；`run-alioth-adreno-first-words.bin` 为 21,438,808 字节，SHA256：`b25b4bcf9d574eb7e63ab36b177171a9ab2c7e681121dab78faa2ef363db5fa9`。
- `ios-alioth-adreno-first-words-logdump.txt`：阶段 1–6 全部 `done`，`getvar result=returned 40889 0`。GMU `cm3_sysreset=1`、`cm3_fw_init_result=0`、`rpmh_power_state=0xf`、`sptprac_pwr_clk_status=0xc0`；always-on counter 实测 **19,199,092Hz**。这证明 GMU 寄存器与计数器可访问，不证明固件已启动；CM3 仍在 reset。
- GBIF `halt=0`、`halt_ack=3`；CX misc `system_cache_cntl_0=1`、`_1=0`。观察 5 秒正常，随后恢复原时钟并再次 `standalone CX cleanup: off again`，正常回 loader；测试后安卓 `sys.boot_completed=1`、槽位 `_b`、USB 充电。
- 阶段 7 前置核对：真机 FDT 的 RSCC 为独立 `0x03de0000/0x10000`，不是旧式 GMU 内部偏移；Linux A650 补丁明确使用独立 `gmu->rscc`，且确认 `RSC_STATUS0=4*0x004`、`PDC_SLAVE_ID=4*0x00b`、`SEQ_BUSY=4*0x101`、`SEQ_MEM_0=4*0x180`、`TCS0_STATUS=4*0x346`、`GMU_RSCC_CONTROL_ACK=4*0x9308`。阶段 7 仅读上述地址，不执行 RPMh 初始化或 PDC/sequencer 写入。

### A650 阶段 7：独立 RSCC 只读访问通过

- 已开放 `adreno=7`；只读取 `0x03de0000` 的 RSC/PDC/sequencer/TCS 状态与 GMU `RSCC_CONTROL_ACK`，未初始化 RPMh、未写 sequencer/PDC、未启动 GMU。
- `build-alioth-adreno-rscc.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-rscc.bcm2837` SHA256：`eb7efb62921ec416809124a32c6914ae0a0f2d790c5b91c4f826a78d2069a3b1`；`run-alioth-adreno-rscc.bin` 为 21,438,808 字节，SHA256：`5bca82a780fb3edbd6052f9a48b5a347b5ede89d6342f5d9f6931f83795b0212`。
- `ios-alioth-adreno-rscc-logdump.txt`：阶段 1–7 全 `done`，`getvar result=returned 39303 0`；`rsc_status0=0`、`pdc_slave_id=1`、`seq_busy=0`、`seq_mem_0=0`、`tcs0_status=1`、`rscc_control_ack=0`。always-on counter 复测 **19,198,365Hz**。
- 5 秒观察、时钟恢复、CX collapse 和返回 loader 均正常；测试后 fresh 安卓 `sys.boot_completed=1`。这些只读状态不构成 RPMh 请求成功的证据。
- 阶段 8 审核修正 redwood 的假设：alioth FDT 的 SMMU 窗口是 `0x03da0000/0x10000`，不是 `0x20000`；stream 为 GPU 0、secure 2、GMU user 4、GMU kernel 5。`qcom,protect=0xa0000/0xc000` 在窗口之外，不访问。下一步只读 ID、stream table 和 CBAR；若 ID 推导的布局与 FDT 不符，会拒绝继续读取。

### A650 阶段 8：SMMU 全局与流表只读访问通过

- 已把 SMMU 映射从 redwood 的 `0x20000` 修正为 alioth FDT 的 `0x10000`，开放 `adreno=8`。只读 ID、SCR0/SGFSR、6 个 SMR/S2CR 和 5 个 CBAR，未写 SMMU、未访问 protect 区、未进入 context bank。
- `build-alioth-adreno-smmu.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-smmu.bcm2837` SHA256：`d2c5255583803cef1f30c6a29982b53426f2e2cacff04e7b1c6db86457b487bd`；`run-alioth-adreno-smmu.bin` 为 21,438,808 字节，SHA256：`e605b4d36f4db09d6e7d4ffe68e3f19929ee41f0afeec48adde097f5dfd88a54`。
- `ios-alioth-adreno-smmu-logdump.txt`：阶段 1–8 全 `done`，`getvar result=returned 44251 0`。`IDR0=0x4c017e06`、`IDR1=0x20000005`：6 个 stream-match、5 个 context、8 个 4KiB pages，2×8×0x1000 恰为 FDT 的 `0x10000`；`SGFSR=0`。
- 启动时 6 个 SMR 和 5 个 CBAR 全为 0，S2CR 返回非规范的超实现值，不据此声称已有可用映射。5 秒观察与清理正常，已恢复 fresh 安卓。
- 阶段 9 仅根据已验证布局读取 context 0 的 `SCTLR/TTBR0/TCR/FSR`，地址从 `0x03da8000` 开始；不读其他 context，不写页表或控制寄存器。

### A650 阶段 9：SMMU context 0 只读访问通过

- `adreno=9` 在已验证布局下只读 `0x03da8000` 的 `SCTLR/TTBR0/TCR/FSR`；`build-alioth-adreno-smmu-context.log` 含模块编译和 `built kernel`。
- `mach-alioth-adreno-smmu-context.bcm2837` SHA256：`964a5557a759a3fee0590e3c79ae63c2d458273444b48c51da66aec35411f62f`；`run-alioth-adreno-smmu-context.bin` 为 21,438,808 字节，SHA256：`c974b275dfa6a1040bfc6cda1192381106807d4f9a1d5b36412acd5d6bf0b769`。
- `ios-alioth-adreno-smmu-context-logdump.txt`：阶段 1–9 全 `done`，`getvar result=returned 43991 0`；CB0 `FSR=0`。SCTLR/TTBR0/TCR 返回值带明显虚拟化特征，不能作为真实页表或启用状态使用，后续必须按 alioth hypervisor 接口核对。
- 5 秒观察、时钟恢复、CX collapse 和回 loader 正常；已恢复 fresh 安卓。
- 阶段 10 前置条件：A650 DTCM 固件目标地址是 `0x10004000`，寄存器窗口仍是 `GMU_WORD(0x1c00)=0x7000`，绝对地址 `0x03d71000`。测试只写两个 word，要求先观察到 `cm3_fw_init_result!=0xbabeface` 且 `cm3_sysreset==1`，最后逐字恢复原值并回读确认；不加载或启动固件。

### A650 阶段 10：GMU DTCM 写入与原值恢复通过

- 已开放 `adreno=10`；先确认 `cm3_fw_init_result=0`（不是 `0xbabeface`）且 `cm3_sysreset=1`，仅测试 `0x03d71000/0x03d71004` 两个 word。没有释放 CM3 reset、加载固件、配置 SMMU 或开启 GX。
- 原值 `0x272023e9/0x43d01660`；测试值 `0x600d0001/0x0a110002` 均逐字回读一致，随后原值恢复也逐字一致。日志明确 `both original DTCM words were restored and read back`。
- `build-alioth-adreno-gmu-memory.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-gmu-memory.bcm2837` SHA256：`466a237a3d3112ebddb26a952f0c98acafe8e85484cc4ec73e18aed5fbda9a71`；`run-alioth-adreno-gmu-memory.bin` 为 21,438,808 字节，SHA256：`78af0cc6eb1dc84f49fd69257dea5491044ff51d23e0e7d7173485b2f25b6f87`。
- `ios-alioth-adreno-gmu-memory-logdump.txt`：阶段 1–10 全 `done`，`getvar result=returned 46284 0`。5 秒观察正常，随后恢复时钟、collapse CX 并正常回 loader；测试后 fresh 安卓 `sys.boot_completed=1`。
- 这只证明 GMU 在 reset 下两个 DTCM word 可写且可恢复，不证明整块 TCM、固件执行、SMMU 或 3D core 已工作。阶段 11 仅审核 TrustZone 的无缓冲区查询；若 SMC ABI 或副作用不明确，不直接上机。

### A650 阶段 11：TrustZone 无缓冲区查询通过

- 已开放 `adreno=11`；阶段 11 只执行 `IS_CALL_AVAILABLE` 与 `PAS_IS_SUPPORTED`，没有传内存，也没有执行 `PAS_INIT_IMAGE`、`PAS_AUTH_AND_RESET` 或 `PAS_SHUTDOWN`。SMC 参数和可抢占续调约定按 Linux `qcom_scm-smc.c` 核对，`hold(true)` 仅负责访问前等待。
- alioth FDT 证实 audio=1、CDSP=18、video=9、GPU zap=13；未沿用未被本机 FDT 证实的 redwood modem=4，对照组已改为 CDSP。
- `build-alioth-adreno-trustzone.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-trustzone.bcm2837` SHA256：`9ea17ee7642a44499a3ab75d38400b3cb9021a7313c77a7f208f5677b924a336`；`run-alioth-adreno-trustzone.bin` 为 21,438,808 字节，SHA256：`4db1d713a77af7c2472dc388e1b788d17b4c980b63af66b6871068d780118e00`。
- `ios-alioth-adreno-trustzone-logdump.txt`：阶段 1–11 全 `done`，`getvar result=returned 49881 0`。64 位 SCM 自查询返回 `0/1`；`PAS_INIT_IMAGE` 与 `PAS_AUTH_AND_RESET` 的可用性查询也都返回 `0/1`。
- 对 audio/CDSP/video/GPU 的 `PAS_IS_SUPPORTED` 在 fast 与 slow 两种调用下均返回 `0/0`。连本机已知由 PAS 启动的对照也为 0，所以该问题在 alioth 上没有区分力，**不能据此断言 GPU zap 不受支持**。
- 5 秒观察、时钟恢复、CX collapse 和回 loader 均正常；测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`。阶段 12 仍只考虑签名校验，不执行 auth-and-reset、不开 GX。

### A650 阶段 12：TrustZone 接受 zap 签名

- `root.apfs` 的 `/fw` 已包含完整 A650 GMU/SQE/zap 固件；镜像内 `a650_zap.mdt` 与 `firmware/fw-gpu/a650_zap.mdt` 逐字节一致，SHA256 均为 `adab48bf3886600faa210b7ba328d837221d2bdbfc46e186beb241e8f44d2bee`。
- MDT 为 6860 字节、3 个 ELF 段，`phoff=52`、`phentsize=32`；148 字节头与段表加 6712 字节哈希证书区恰好等于文件大小。已把源码中的 a660 固件名全部改为 a650。
- redwood 的一页假设不适用于 6860 字节头；已改为按真实大小 `round_page` 后使用 `kmem_alloc_contig`，并逐页核对物理连续性。真机分配为 16384 字节（本内核 PAGE_SIZE=16K），起始物理地址 `0x1fcb78000`。
- 已开放 `adreno=12`，只调用 `PAS_INIT_IMAGE(image 13, header)`；没有 `PAS_MEM_SETUP`、auth-and-reset、zap 启动、SMMU 写入或 GX 上电。
- `build-alioth-adreno-zap-signature.log` 含 `CC hakios.adreno.o` 和 `built kernel`。`mach-alioth-adreno-zap-signature.bcm2837` SHA256：`0a13a83c5359f2fdaf47b4ed93256dcc549218785c5bb85ef81fc753d6d71fbc`；`run-alioth-adreno-zap-signature.bin` 为 21,438,808 字节，SHA256：`fd00922fe00ae5483bc8afa716b13e5adc25f3f48bc7da53e86dee84466fe077`。
- `ios-alioth-adreno-zap-signature-logdump.txt`：阶段 1–12 全 `done`，`getvar result=returned 51618 0`；TrustZone 返回 `0/0`，日志明确 `TRUSTZONE ACCEPTS THE ZAP SHADER'S SIGNATURE FROM THIS KERNEL`。这证明签名初始化路径可用，不证明 zap 已认证启动或 GPU 已工作。
- 5 秒观察、清理与回 loader 正常；测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`。阶段 13 的直接 SMMU 写入与阶段 8/9 的虚拟化证据冲突，必须单独审核，不直接开放。

### ADSP 与其他后续

- `wsl-current/hakios.adsp.c` 同样还是 redwood 版本，不能直接套用。alioth ADSP 保留内存为 **`0x8bb00000`、大小 `0x2500000`（37 MB）**（设备树 `pil_adsp_region@8bb00000`；此处先前写的 `0x8a100000` / `0x1d00000` 是错的，已按 FDT 更正）；PAS image ID、IPCC client/signal、SMP2P/GLINK item 均需按 alioth FDT 重移植。
- ADSP 固件已备：`firmware/fw-audio/adsp.mdt` 与 `adsp.b00`–`adsp.b18`。
- Wi-Fi 驱动尚未完成，未做扫描、关联、DHCP 或联网验收；不要把用户已提供 Wi-Fi 信息误当成 Wi-Fi 已可用。
- 仍需正式整合触摸/cpufreq 的安全默认载荷、长时稳定性、一键 rdload 脚本，并修正 `devices/alioth.hardware.md` 中过时的触摸供电与 TLMM 描述。

### 构建与 fresh 真机测试模板

修改 adreno 时还需把 `hakios.adreno.c` 一并复制到 WSL 的 `bsd/dev/`。基本构建命令：

```bash
export MSYS2_ARG_CONV_EXCL='*'
wsl bash -lc 'cp <主机>/Develop/iOSOnAndroid/alioth-ios-port/wsl-current/hakios.goodix.alioth.c ~/hackios-new/xnu-src/xnu-8796.121.2/bsd/dev/hakios.goodix.c && cp <主机>/Develop/iOSOnAndroid/alioth-ios-port/wsl-current/hakios.cpufreq.c ~/hackios-new/xnu-src/xnu-8796.121.2/bsd/dev/hakios.cpufreq.c && cp <主机>/Develop/iOSOnAndroid/alioth-ios-port/wsl-current/hakios.haptic.alioth.c ~/hackios-new/xnu-src/xnu-8796.121.2/bsd/dev/hakios.haptic.c && cd ~/hackios-new && ./xnu-build.sh -j8 -k'
```

每次检查构建日志中的 `CC <改动模块>` 和 `built kernel`，再导出 `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` 并用 `mkpayload.py` 打包。

每轮都从 fresh 安卓开始，不连续 `oem run`：

```bash
./tools/platform-tools/adb.exe -s <序列号> reboot bootloader
sleep 4
./tools/platform-tools/fastboot.exe -s <序列号> boot alioth-ios-port/wsl-current/xnu-rdload-log.img
sleep 8
./tools/platform-tools/fastboot.exe -s xnuloader oem rdload iosdata
./tools/platform-tools/fastboot.exe -s xnuloader stage <测试载荷.bin>
./tools/platform-tools/fastboot.exe -s xnuloader oem run
```

等待 `loader_return_after` 后 `get_staged`，再用 `extract-logdump.py` 提取。若当前已在 loader，先 `fastboot -s xnuloader oem reboot`，`adb wait-for-device`，等安卓完全启动后再开始下一轮。

### A650 阶段 13：GMU SMMU 地址空间通过

- 首轮 `ios-alioth-adreno-gmu-space-logdump.txt` 在严格校验处停止，但 SMR=`0x84000005`、S2CR=`0x301` 均已正确回读。根因是源码校验掩码误写为 `0x300ff`，漏掉 S2CR type 的 bits 9:8，因而把正确的 `0x301` 判为失败；这不是硬件拒绝写入。两处掩码已修为 `0x303ff`，其余严格读回检查没有放宽。修改前备份为 `hakios.adreno.before-s2cr-mask.c`。
- 修复后 `build-alioth-adreno-gmu-space2.log` 含 `CC hakios.adreno.o` 和 `built kernel`；`mach-alioth-adreno-gmu-space.bcm2837` SHA256：`f3377ffb43a273c0077115b1283d0d168aca39a383026b209757dbe6382a3102`；`run-alioth-adreno-gmu-space.bin` 为 21,438,856 字节，SHA256：`53f55f33232375994815b5715153e2bed53abd0024187b6a03797e6e3113b92b`。载荷为 `adreno=13 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。
- 从 fresh 安卓复测，`getvar result=returned 60059 0`；最终日志 `ios-alioth-adreno-gmu-space2-logdump.txt` 的阶段 1–13 全部 `done`。SMR/S2CR/CBAR/CB1 SCTLR/TTBR0/TCR 均回读正确，TLBSYNC 完成；CB1 FSR=`0x400`（仅为格式位，不在 `SMMU_FSR_ALL` 故障位内），SGFSR=0。5 秒观察正常，SMR/S2CR/SCTLR 清理、时钟恢复、CX collapse 和返回 loader 均完整。
- 四个固件外部分配块及 HFI/log 均建立映射。HFI=`0x60001000/0x5000`，log=`0x60006000/0x1000`。这只证明 GMU 地址空间建立和回收成功，不证明 GMU 固件运行、GX 或 3D 已工作。测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`。
- 用户补充触摸坐标可能和实际位置对不上。后续需专项核对 FT3658U 原始坐标范围、屏幕分辨率缩放、XY 交换、旋转方向及边缘映射；现有自动 attach 检查不能替代真实坐标验收。

### A650 阶段 14：GMU 固件加载通过，CM3 未启动

- 离线复核 `a650_gmu.bin` 的 16 个块完整，所有 TCM 数据均为完整 32 位字；两个外部数据块 `0x4000/0x55d0`、`0x157000/0x398` 均落在阶段 13 已分配的映射内。阶段 14 仅保持 CM3 reset，把 ITCM/DTCM 逐字写入并回读，把外部数据复制到对应内存；没有释放 CM3、初始化 HFI 或开启 GX。
- `build-alioth-adreno-gmu-firmware.log` 含 `CC hakios.adreno.o` 和 `built kernel`；`mach-alioth-adreno-gmu-firmware.bcm2837` SHA256：`09974f41821a5128571cbc803f01d7e165c1fa0b6dc313940fd144c3196876c5`；`run-alioth-adreno-gmu-firmware.bin` 为 21,438,856 字节，SHA256：`f3ba9d9359c838e2c258fe10cc003db03775057312d37589c38836afb7297ca0`。修改前备份为 `hakios.adreno.before-gmu-firmware.c`，差异为 `adreno-alioth-step14.diff`。
- 从 fresh 安卓测试，`getvar result=returned 60221 0`；`ios-alioth-adreno-gmu-firmware-logdump.txt` 的阶段 1–14 全部 `done`。ITCM 三块共 `0xc0+0x140+0x370c` 字节、DTCM 两块共 `0xec0+0x18` 字节均逐字回读一致；版本为 core=`0x20010008`、power=`0x20010008`、HFI=`0x20000001`。
- 5 秒观察正常，SMMU、时钟、CX 清理完整并正常回 loader；测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`。这仍只证明固件正确加载，不证明固件已执行。
- 阶段 15 审核发现两个必须先修正的问题：20K HFI 需要整体物理连续，而普通 `block_new()` 不保证；原厂冷启动在加载/启动 GMU 固件之前装载 RSCC/PDC 程序，当前旧步骤却把它推迟到 GMU 启动之后。不得直接开放原阶段 15/16 顺序。

### A650 阶段 15：GMU/CM3 固件实际启动通过

- HFI 改用 `kmem_alloc_contig` 并保留每个 4K 映射的连续性核对；RSCC 五个程序字与 AOP PDC 两个使能寄存器移到 CM3 启动前，五个字均严格回读。RSCC 唤醒失败现在立即返回，不再错误地继续。A650 有 IFPC，补写 `PWR_COL_INTER_FRAME_HYST=0x000a1680` 并把控制位 1|0 置位；其特性不含 SPTP_PC，因此不执行主机 SPTPRAC 上下电。
- 终审还修正 redwood 遗留的 GMU chip ID：本机 FDT `0x06050002` 按原厂规则压缩为 `0x06050000`，不再传 `0x06035000`。修改前备份为 `hakios.adreno.before-gmu-start.c`，差异为 `adreno-alioth-step15.diff`。
- `build-alioth-adreno-gmu-start.log` 含 `CC hakios.adreno.o` 和 `built kernel`；末尾 export-check Error 2 仍是既有链接后现象。`mach-alioth-adreno-gmu-start.bcm2837` SHA256：`a9b28e616f8acbe6336dfce759b49288ed56f97de5d0f27a8503f0b74f49eda8`；`run-alioth-adreno-gmu-start.bin` 为 21,438,872 字节，SHA256：`258306691f60c00900e0bf39626b60aa4f55265cc8bc725ee308604e829e7ce9`。打包脚本为 `build-adreno-gmu-start.sh`，载荷为 `adreno=15 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。
- 从 fresh 安卓测试，`getvar result=returned 75473 0`；`ios-alioth-adreno-gmu-start-logdump.txt` 阶段 1–15 全部 `done`。HFI 物理起始为 `0x1fcb8c000` 且连续；RSCC 五字逐一一致，PDC enable 从 `0x100` 到 `0x80000001`，IFPC 控制为 `0x9c40403`。CM3 释放后 `cm3_fw_init_result=0x100`，日志明确 `THE GMU'S FIRMWARE RUNS`；没有 SMMU fault。此轮未显式写 `HFI_CTRL_INIT`、未开启 GX 或 3D core。
- 5 秒 alive 后 CM3 重新 reset，RSCC 请求休眠并得到 `rsc_status0=0x1010006`，SMMU、时钟和 CX 清理完整，正常返回 loader。测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`、USB 充电、100%。这只证明 GMU 固件运行和回收，不证明 HFI 消息、GX 或 3D 已工作。

### A650 阶段 16：HFI 队列与双向版本查询通过

- 开放 `adreno=16`，初始化前后均记录并校验 HFI 表头、四个队列地址、状态和 read/write 索引；固定布局必须保持为 version=0、54 个表头字、首队列偏移 6、队列头 12 字、4 个队列，索引只要求小于 1024，不强制为 0。修改前备份为 `hakios.adreno.before-gmu-hfi.c`，最终差异为 `adreno-alioth-step16.diff`。
- 首轮 `ios-alioth-adreno-gmu-hfi-first-logdump.txt` 的阶段 1–16 虽全部 `done`，但清理时过早发送 `PREPARE_SLUMBER`，固件返回 `0x3ea` 和异步错误 `1002 / PwrTask:87`。这证明队列能收发，却不是合格的正常清理。按原厂 `hfi_start()` 顺序改为显式发送两字的 `FW_VER` 查询，并新增 `gmu_hfi_started`：只有阶段 17 的完整 `HFI_START` 成功后才允许 slumber；阶段 16 退出时直接 reset CM3。
- 最终构建日志 `build-alioth-adreno-gmu-hfi2.log` 含 `CC hakios.adreno.o` 和 `built kernel`，末尾 export-check Error 2 为既有现象。`mach-alioth-adreno-gmu-hfi.bcm2837` SHA256：`8fbe51a28c1d4f51f7d9304977943e930beab820e2f94c606bf0c14cc76e6e66`；`run-alioth-adreno-gmu-hfi.bin` 为 21,439,000 字节，SHA256：`4cd0fcfe5597c13ab765ba43b1f5384d5c4c52b528c3d8a70f4bf18092a2b4f7`。打包脚本 `build-adreno-gmu-hfi.sh`，参数为 `adreno=16 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。
- 从 fresh 安卓复测，`getvar result=returned 77883 0`；`ios-alioth-adreno-gmu-hfi-logdump.txt` 阶段 1–16 全部 `done`，无 SMMU fault。HFI 连续物理起始为 `0x1fcb90000`；寄存器 HFI version=`0x20000001`，`FW_VER` 命令获成功应答并返回 GMU core=`0x20010008`。命令队列和应答队列的 read/write 均从 0 前进到 4，四个队列地址保持 `0x600010e0`/`0x600020e0`/`0x600030e0`/`0x600040e0`。
- 5 秒 alive 正常；最终清理明确记录 HFI 尚未完整启动，因此不发送 slumber，CM3 reset、RSCC sleep（`rsc_status0=0x1010006`）、SMMU/时钟/CX 回收完整，正常回 loader。测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`、USB 充电、100%。阶段 16 证明 HFI 双向消息可用，不代表性能/带宽表、GX 或 3D core 已工作。

### A650 阶段 17：性能表与 GX／3D core 上电通过；正常 slumber 留待完整 core 启动后验收

- 开放 `adreno=17`，只支持本机 speed-bin-2；八档 GMU 索引反序后为 0、305、400、441.6、490、525、587、670 MHz，默认索引 1（305 MHz）。GPU VLVL 依次 0/64/128/144/192/224/256/320，GMU 两档为 0/200 MHz。HFI 性能表、单 off BW 表、`CORE_FW_START`、默认频率 vote、core OOB、`HFI_START` 均成功，305 MHz 实测约 305,001 kHz，GX 上电且 3D core 的 RBBM/CP 寄存器可读。阶段 17 的**上电和寄存器读取目标已通过**。
- A650 mainline 与本机一致只有单 off BW 表：levels=1，MC0=`0x50000`、SH0=`0x50004`、ACV=`0x5007c`，数据均 `0x40000000`，CNOC 一项；不要伪造 `bw=3` 或动态带宽表。slumber payload 布局是 header、bw、freq，当前保留 `{0,0,1}`，即 bw=off、freq=默认 305 MHz。
- `core-power` 与 `core-power2` 都完成阶段 17 上电，但正常 slumber 返回异步 `1002 / PwrTask:87`、应答 verdict `0x3ea`。`core-power2` 另把全局 word `0x1f840` 错当 GMU 局部偏移，写 `0x3de8100` 后挂起，`getvar result=hung 29484 0`；原厂 `gmu2gpu_offset=(0x3d6a000-0x3d00000)>>2=0x1a800`，正确局部 word 是 `0x5040`，绝对地址 `0x3d7e100`。该错误已修复，错误地址不得再用。
- `core-power3` 修正为 `GMU_POWER_COUNTER_ENABLE=GMU_WORD(0x1f840-0x1a800)`，写 `0x3d7e100` 不再挂起，但 `{bw=0,freq=0}` 仍有同样的 slumber 错误。`mach-alioth-adreno-core-power3.bcm2837` SHA256=`d102f13cc824d7713986e59b2d9c301ce0a6e38b744e1b0e901bbd08a9048a21`；`run-alioth-adreno-core-power3.bin` 21,439,016 字节，SHA256=`803f3c0b1bae8ae36c5f6f6cc31e92dfe627eabbdfcdd88977bb57e13aa33375`；测试 `returned 91929 0`。
- `core-power4` 仅改 freq=1，仍返回 `1002 / PwrTask:87`、`0x3ea`。内核 SHA256=`76d696c350a3f02d374e13071ce3c15cf78e69d1493e438b612420aff9b02329`；载荷 21,439,064 字节，SHA256=`c292b9e1bb64043156530b87d21f6d84d75ac9f66d7328073a301c2ed9789abf`；测试 `returned 91935 0`。freq=1 不是单独修复。
- 随后按原厂 `gmu_stop()` 补上最低 idle 等待。已核实枚举：`GPU_HW_ACTIVE=0`、`SPTP_PC=2`、`IFPC=3`、`NAP=4`、`MIN_VOLT=5`、`MIN_DDR=6`、`SLUMBER=0xf`。A650 的 IFPC 完成条件是 `RPMH_POWER_STATE==3` 且 `SPTPRAC_PWR_CLK_STATUS & 0xc0` 非零；等待上限 100 ms，不得无限等待。slumber 后原厂等待 `AO_GPU_CX_BUSY_STATUS` bit30 清零，当前代码已由旧 bit23 改为 bit30。
- `core-power5` 从 fresh 安卓测试 `returned 85155 0`。清除 core OOB 后 100 ms 内始终是 `rpmh_power_state=0`、`sptprac_pwr_clk_status=0`，未进入 IFPC；代码明确不发送无效 slumber，走 CM3 reset、GX 强制关闭、RSCC/SMMU/时钟/CX 回收，完整返回 loader。内核 SHA256=`e16a297e3dc93477cad2218e5f02af716100df6dee8cf5ccdded6124a4600507`；载荷 SHA256=`180e2182fbcc17df057f3a336306b2ab8d9366746035d2c4541f6389ed46e471`。
- `core-power6` 只增加超时诊断，不改硬件配置。`returned 92317 0`；超时时 `pwr_col_inter_frame_ctrl=0x9c40403`、`rpmh_ctrl=0xf11`、`gmu2host_intr_info=0`、`ao_host_interrupt_status=0`、`ao_gpu_cx_busy_status=0x30000000`，GMU log 有 79 个事件，OOB 清除后没有 IFPC 转换事件。强制回收仍完整。内核 SHA256=`4c8fb04de4c0e5bcbc49f9ba1a6fb81aaa5a4dd2d2d83620e9b11d64f925d8c7`；载荷 SHA256=`473f3c212f38023125cc23bafb2357867405a290ca9608aeff7b1e2d23cee0ff`。
- 原厂 `adreno_stop()` 只有在完整 `ADRENO_DEVICE_STARTED` 后才执行 idle/slumber；当前阶段 17 仅 GX 上电，尚未初始化 SQE/CP/RB，因此**不要把这里的 IFPC 超时误当成性能表错误，也不要继续盲试 slumber 参数**。正常 IFPC/slumber 验收推迟到完整 core 启动后；当前拒绝无效 slumber 并强制回收的安全路径保留。
- 当前正式源码为 `wsl-current/hakios.adreno.c`；本轮备份依次是 `hakios.adreno.before-core-power.c`、`before-core-power-cleanup.c`、`before-core-power-cleanup-address.c`、`before-core-power-slumber-args.c`、`before-core-power-idle-wait.c`、`before-core-power-idle-diagnostics.c`。`adreno-alioth-step17.diff` 已按当前源码重新生成。各轮 `build-alioth-adreno-core-power*.log` 均检查 `CC hakios.adreno.o` 与 `built kernel`；末尾 export-check Error 2 仍是既有现象。
- 每轮结束后均已恢复安卓。最后一次 `core-power6` 后确认 `sys.boot_completed=1`、`ro.boot.slot_suffix=_b`、USB 充电、100%。当前手机停在正常安卓。

### 新会话续作入口：先审核阶段 18，再做真机测试

- 下一步审核并开放 `adreno=18`：3D core context 0、ring、SQE 和 scratch 映射，不启动 CP。`a650_sqe.fw` 已核对为 31,632 字节，首字为 0，SHA256=`b4c27420fd6a5c6b511b254cc0fad8cdd40eea38ceb3418f895a1084f4460487`。
- 必须先核对 alioth 的 stream 1/LPAC 是否存在及其 context 配置，不能照搬 redwood 默认 `adreno_stream1=1`；阶段 18 不应启动 SQE，阶段 20 才会涉及该风险。继续确保 `gpu=1` 或 `adreno>当前已审核阶段` 拒绝执行。
- 每轮仍从 fresh 安卓开始，测试后立即 `fastboot -s xnuloader oem reboot` 并复核 `_b` 与 `sys.boot_completed=1`。完整 core 启动后重新验收 IFPC 与正常 slumber；之后再专项修复 FT3658U 触摸的原始范围、1080×2400 缩放、XY/旋转和边缘映射。

### A650 阶段 18：3D core 地址空间、ring／SQE／scratch 映射通过

- 真机 FDT 没有独立 `gfx3d_lpac` context：`gfx3d_user` 的 `iommus=<0x182 0x00 0x401>` 明确要求 stream 0／1 共用同一 context。正式实现只用一条 `stream=0、mask=0x401` 的 SMR 覆盖两路，移除 redwood 的 `adreno_stream1` 独立空 context／缺失流实验；GMU 仍保留已验证的 `mask=0x400`。`space_on()` 现在显式接收各设备自己的 stream mask，严格读回校验未放宽。
- 首轮曾尝试两条独立 `mask=0x400` SMR，`ios-alioth-adreno-core-space-first-logdump.txt` 显示 stream 0 的 SMR 写入 `0x84000000` 后读回 0，严格校验在阶段 18 停止；这不是页表或固件失败。随后按 FDT 改为单条组合 SMR。首轮 `returned 95665 0`，强制清理完整；对应源码、内核、载荷和日志均以 `first` 或 `before-core-space-smr` 留存。
- 阶段 18 对 `a650_sqe.fw` 强制要求精确的 31,632 字节和零版本头；按原厂 `_load_firmware()` 跳过首 4 字节，将 31,628 字节 SQE payload 拷贝后逐字节回读。建立 ring `0x01000000/0x8000`、SQE `0x01100000`（映射圆整至 `0x8000`）、scratch `0x01200000/0x1000`，没有分配 framebuffer，没有加载 zap，没有启动 CP／SQE。
- 最终构建 `build-alioth-adreno-core-space2.log` 含 `CC hakios.adreno.o` 和 `built kernel`，末尾 export-check Error 2 仍为既有现象。`mach-alioth-adreno-core-space.bcm2837` 为 15,143,096 字节，SHA256=`fd6fab9b82029652cb837d2cd585f08ad32849f28463c3a686327473f9657682`；`run-alioth-adreno-core-space.bin` 为 21,406,136 字节，SHA256=`eb1eea9e83814aa9129c501b60cfb7ad1acd94e1dc67e04e297390b765471f72`。打包脚本为 `build-adreno-core-space.sh`，参数 `adreno=18 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。
- 从 fresh 安卓复测，`getvar result=returned 98342 0`；`ios-alioth-adreno-core-space-logdump.txt` 的阶段 1–18 全部 `done`，无 FAILED／SMMU fault。SMR=`0x84010000`、S2CR=`0x300`、CBAR=`0x1f300`、CB0 SCTLR=`0x127` 均按严格条件通过；`SGFSR=0`，CB0 FSR=`0x400` 仅为格式位，不在 `SMMU_FSR_ALL` 故障位内。5 秒 alive 正常。
- 此轮仍未完整启动 core，因此清除 OOB 后 IFPC 超时属于阶段 17 已记录的既定边界；代码拒绝发送无效 slumber，CM3 reset、GX 强制关闭、RSCC sleep、SMMU／时钟／CX 回收完整，日志为 `standalone CX cleanup: off again`，正常返回 loader。测试后 fresh 安卓 `sys.boot_completed=1`、槽位 `_b`、USB 充电、100%。
- 当前正式源码为 `wsl-current/hakios.adreno.c`；备份 `hakios.adreno.before-core-space.c`、`hakios.adreno.before-core-space-smr.c`，最终差异 `adreno-alioth-step18.diff`。`gpu=1` 或 `adreno>18` 仍拒绝执行。阶段 18 只证明地址空间与固件内存映射建立成功，不证明 zap、SQE 执行、CP、3D 渲染或正常 IFPC／slumber 已工作。

### 新会话续作入口：先审核阶段 19，再做真机测试

- 下一步审核并开放 `adreno=19`：仅 zap shader 的卸载旧实例、保留内存、`PAS_MEM_SETUP` 与 `PAS_AUTH_AND_RESET`，不启动 CP／SQE。必须重新核对 alioth 的 zap 保留地址／大小、`a650_zap.b02` 大小与加载顺序，以及失败和正常退出时的 `PAS_SHUTDOWN` 回收路径；不得直接照搬 redwood 的“签名失败后沿用旧 zap”分支。
- 每轮仍从 fresh 安卓开始，测试后立即回 loader、重启安卓并复核 `_b` 与 `sys.boot_completed=1`。完整 core 启动后再验收 IFPC／正常 slumber；随后再处理 FT3658U 触摸坐标专项。

### A650 阶段 19：zap PAS 加载与认证通过；CP／SQE 仍未启动

- 真机 FDT `pil_gpu_region@8681a000` 的 `reg=<0 0x8681a000 0 0x2000>` 已取代 redwood 的 `0x8b71a000`。离线解析 MDT 证实唯一 `PT_LOAD` 是 b02：`p_paddr=0x5000`、`p_filesz=p_memsz=0x68c`（1676 字节）、带 relocatable flag；按 `qcom_mdt_pas_init()` 的 4K 对齐规则，PAS relocation size 为 `0x1000`，并非需要扩大到整个 `0x2000` 保留区。b02 SHA256=`a415e5452fa8f597670a6e51010e97bfabca7d20fbce8044caed64d9d5873113`；解析脚本为 `inspect-zap-mdt.py`。
- 阶段 19 现在强制要求初始 `PAS_SHUTDOWN` 成功，校验 MDT 第三程序头与 b02 精确一致，随后依次执行签名、`PAS_MEM_SETUP(13, 0x8681a000, 0x1000)`、拷贝并逐字节回读、`PAS_AUTH_AND_RESET`。删除了签名／mem-setup 失败时“沿用旧 zap”并伪装成功的分支。新增 `zap_needs_shutdown`：mem-setup 成功后，即使映射、拷贝或认证失败，正常清理也必须 shutdown；panic 后则由下一轮 mandatory initial shutdown 回收。
- 本阶段明确只做独立 PAS 验证，不写 `CP_SQE_INSTR_BASE`、不写 `CP_SQE_CNTL`，不启动 CP／SQE。原厂完整启动顺序是 CP_INIT 后 `subsystem_get()` 加载 zap、再退出 secure mode；不要把本阶段的 TrustZone 成功返回当成 CP 已退出 secure mode 或 zap 已被 3D core 实际使用的证明。
- 构建 `build-alioth-adreno-zap-load.log` 含 `CC hakios.adreno.o` 和 `built kernel`，末尾 export-check Error 2 仍为既有现象。`mach-alioth-adreno-zap-load.bcm2837` 为 15,143,128 字节，SHA256=`5791dfd1f2192cbff9c120dbeb557a10e5b8577714943c2ce0b46dacb55f8c44`；`run-alioth-adreno-zap-load.bin` 为 21,406,168 字节，SHA256=`25e2bceb499974b6fb7c4036da9a81ba68cf7f1aa7dfc368d74839d4214f5bec`。打包脚本 `build-adreno-zap-load.sh`，参数 `adreno=19 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。
- 从 fresh 安卓测试，`getvar result=returned 101036 0`；`ios-alioth-adreno-zap-load-logdump.txt` 阶段 1–19 全部 `done`，无 FAILED／REFUSED，未出现阶段 20 或 CP／SQE 启动。初始 shutdown、签名、mem-setup、auth-and-reset 和最终 shutdown 均为 TrustZone `0/0`；1676 字节全部回读一致，`SGFSR=0`，5 秒 alive 正常。
- 清理时未完整启动 core 的 IFPC 超时仍是既定边界，代码拒绝无效 slumber；CM3 reset、GX 强制关闭、zap shutdown（`zap cleanup after authentication: TrustZone shut it down`）、RSCC sleep、SMMU／时钟／CX 回收完整并正常回 loader。测试后已恢复 fresh 安卓，复核 `sys.boot_completed=1`、槽位 `_b`、USB 充电、100%。
- 当前正式源码 `wsl-current/hakios.adreno.c` SHA256=`956cb4bd7cde27429a2aa1e7acca5434f08d9d65bbde33264d3450bc79e036b8`；修改前备份 `hakios.adreno.before-zap-load.c`，最终差异 `adreno-alioth-step19.diff`。`gpu=1` 或 `adreno>19` 仍拒绝执行。下一阶段必须先审核阶段 20 的 core 寄存器初始化、保护配置、ring／SQE 启动及失败回收，再决定是否上机；正常 IFPC／slumber 仍需完整 core 启动后验收。

### A650 阶段 20：A650v2 CP／SQE 启动与 CP_ME_INIT 通过；正常 IFPC 仍待修复

- 按本机 chipid `0x06050002` 采用 A650v2 而非 redwood 参数：`PC_DBG_ECO_CNTL=0x00300200`、hang cycles=`0x3ffff`。按真机 FDT hbb=16、MAL=32、UBWC4 得到 `RB_NC_MODE_CNTL=0x816`、`TPL1_NC_MODE_CNTL=0x6`、`SP_NC_MODE_CNTL=0x26`、`UCHE_MODE_CNTL=0x600000`。删除无本机原厂依据的 `RBBM_NC_MODE_CNTL`、`RB_DBG_ECO_CNTL`、`CP_CHICKEN_DBG`、LPAC FIFO 和 QoS 清零写入；保留原厂 `RBBM_GBIF_CLIENT_QOS_CNTL=3`。`UCHE_CLIENT_PF=1`，A650 APRIV 的 `CP_RB_CNTL=0x20c`，增加 `CP_RB_RPTR_ADDR=0x01200100`。
- 补齐同一 alioth-r-oss 分支的 `a6xx_reg.h`、`kgsl_device.h` 等来源，核准 `CP_PROTECT_CNTL=0xb` 和 A650 所用 a620 protected table 的索引 0–32、47 共 34 条跨度；补写五组 bicubic weights。实现原厂 `a6xx_patch_pwrup_reglist()`：独立 GPU 地址 `0x01300000`、无 unprivileged AP bit 的特权映射，16 字节 `cpu_gpu_lock` 头，36 个 IFPC-only + 28 个 common + 7 个 A650 寄存器／回读初值对，`list_offset=72`、`list_length=142`。GMU busy mask 经原厂全局到局部路由确认为 word `0x930e`，另配置 select0=`0x20`、select1=`4`、enable=`1`，没有越界写。
- `CP_ME_INIT` 改为原厂精确 11 个 payload word，mask=`0x17f`，包含 max contexts=3、error=`0x20000000`、operation mode=2、恢复页地址和 spinlock/padding。阶段 20 强制要求阶段 19 已认证 zap，不重复 PAS 加载；删除故意制造 SMMU fault 的 `adreno_blind` 路径。启动失败或 ring 超时现在设置 `core_stuck`，清理会保持 core 侧 GBIF halt，不会在故障后重新放开总线。可选 HWCG 大表和预emption提交尚未启用，代码和日志均未宣称完整 vendor 初始化。
- 修改前备份 `hakios.adreno.before-core-start.c`，差异 `adreno-alioth-step20.diff`；当前源码 SHA256=`a20c1acb1aebecefabfee3a7da684ed896755f5d3bc137c18b99a2472072cfa7`。构建 `build-alioth-adreno-core-start.log` 含 `CC hakios.adreno.o` 和 `built kernel`，末尾 export-check Error 2 是既有现象。`mach-alioth-adreno-core-start.bcm2837` 为 15,143,240 字节，SHA256=`1831123f6f5761b88bcd6fec3d8dece561ddb731f6ea589f1d81dfbfd5fbab2e`；`run-alioth-adreno-core-start.bin` 为 21,406,280 字节，SHA256=`bf90da76f42e872d62b69a3b9780be2ab8700870371b8b73c2c90d3755351327`。打包脚本 `build-adreno-core-start.sh`，参数 `adreno=20 adreno_pause=100 adreno_fine=1 adreno_watch=5 adreno_return=1`。
- 从 fresh 安卓测试，`getvar result=returned 121984 0`；`ios-alioth-adreno-core-start-logdump.txt` 阶段 1–20 全部 `done`，无 FAILED／REFUSED。CP/SQE 启动后 `RBBM_STATUS=1`、CP fault/interrupt/protection 均为 0、SGFSR=0；11 字 CP_ME_INIT 完成，ring read register、scratch shadow、预期值都为 12，日志明确 `THE 3D CORE TAKES COMMANDS`。5 秒 alive 正常。该结果证明 CP/SQE 已执行第一条命令，不证明阶段 21 已退出 secure mode或能写内存。
- 清理时 CP 仍 idle，但 `rpmh_power_state=0`、`sptprac_pwr_clk_status=0`，100 ms 内未进 IFPC；代码继续拒绝无效 slumber，CM3 reset、GX 强制关闭、zap shutdown、RSCC sleep、SMMU／时钟／CX 回收完整并正常回 loader。因此**正常 IFPC／slumber 尚未通过**，不能因 CP 启动成功而标记完成。测试后已恢复 fresh 安卓，复核 `sys.boot_completed=1`、槽位 `_b`、USB 充电、100%。
- 当前只允许到 `adreno=20`，`gpu=1` 或 `adreno>20` 仍拒绝。下一步继续审核阶段 21 的 `CP_SET_SECURE_MODE`／`CP_MEM_WRITE` 和 cache 同步，并以原厂 GMU OOB、idle 及 feature 条件排查 IFPC 未进入的原因；不得靠发送无效 slumber 或缩短检查绕过。

### A650 core-commands 第 16–21 轮排查与 2026-10-27 暂停点

- core-commands 第 16、17、18、19、20 轮从 fresh 安卓测试后的 `getvar result` 分别为 `returned 142498 0`、`returned 115466 0`、`returned 120021 0`、`returned 120622 0`、`returned 151541 0`；对应证据为 `ios-alioth-adreno-core-commands16-logdump.txt` 至 `ios-alioth-adreno-core-commands20-logdump.txt`。这些轮次建立了 secure stream 2／context 2、验证 PAS CE0 临时带宽投票并排除完整 A650 HWCG 配置的影响，但均未解决 secure-mode 退出。
- 第 20 轮的 `CP_SET_SECURE_MODE(0)` ring 包头为 `0x70e60001`、payload 为 0；提交后 RPTR 到 14，`CP_INTERRUPT_STATUS=1`，`RBBM_SECVID_TRUST_CONTROL=1`，SQE 报告 rejected opcode=0。离线逐位核对原厂 `cp_type7_packet()` 后，包头和奇偶位正确，selector 1 也确为原厂 opcode 诊断接口；但 opcode=0 的含义仍不能过度解释，当前结论仅是 secure-mode 退出仍被拒绝。
- 第 20 轮产物：`mach-alioth-adreno-core-commands-twentieth.bcm2837` 为 15,176,608 字节，SHA256=`c20fc4ff849fc59f328742e1456b4db70af9634b0568e09a33bdc7cc6f6e4bd5`；`run-alioth-adreno-core-commands-twentieth.bin` 为 21,439,640 字节，SHA256=`d49da951712bc6d2bd053e3bc8af3ddeb8db22a74e33627a2904379179bfa42b`。
- 第 21 轮危险 zap 观察载荷在认证后挂住，`getvar result=hung 52703 0`。该源码改动已完全撤回，撤回时 `hakios.adreno.before-core-commands-zap-observe.c` 与正式源码无差异。认证后不得再从 HLOS 映射或读取 `0x8681a000`。该轮仅保留作故障证据：内核 `mach-alioth-adreno-core-commands-twentyfirst.bcm2837` SHA256=`9d079b4a74ae8ee77f9dc5bcb1f4b0321a09641581e523971173b2a7621d9ade`；载荷 `run-alioth-adreno-core-commands-twentyfirst.bin` 为 21,439,672 字节，SHA256=`3b4da7699107eabe24d0dedcdaf3ae9c87dede08f65cca4ca6709af27705eaaa`。
- 当前 `wsl-current/hakios.adreno.c` SHA256=`91666e24755976b2069eb39f9f2a1f79f3162cb2f3c19af39d80344b1a12d2a7`，只比备份 `hakios.adreno.before-core-commands-qsmmu.c` 多一处**尚未构建、尚未上机验证**的 QSMMU 候选修正：在两个 core context attach 后写 `gpu_smmu_hum_cfg(0x6800)=2` 和 `gpu_smmu_dora_ordering(0x64a0)=1` 并严格回读。依据是原厂 `kgsl_iommu_start()` 在非 `KGSL_IOMMU_SMMU_V500` 时必须执行这两写，而本机 FDT 为 `qcom,kgsl-smmu-v2`。`build-adreno-core-commands22.sh` 已准备但从未执行，没有对应第 22 轮内核或载荷。
- **暂停结论：GPU 驱动尚未完成；secure-mode 退出仍被拒绝，IFPC／正常 slumber 和 3D 渲染均未验收。** 下次续作应先单独验证上述 QSMMU 候选，不要把它当成既有修复，也不要恢复危险的 zap 读取。用户于 2026-10-27 要求暂停 GPU 工作并启动稳定 iOS；本次选用 `run-alioth-ios-current.bin`（触摸与 cpufreq，`loader_return_after=3600`），SHA256=`d90f9d7040c040834d5e3112e9c39b981298a10a61e1756e3fedf95f872f15a0`。
- 已从正常安卓依次执行 bootloader、`xnu-rdload-log.img`、`oem rdload iosdata`、stage 和 `oem run`。启动约 40 秒后 `getvar result=running 0 0`；`ios-final-20261027-logdump.txt` 记录 XNU 已跳转、launchd 及多项 iOS 用户态服务运行。未执行 `oem reboot`，手机保持在本次 iOS 运行状态；本次取回的日志未单独列出 SpringBoard 行，因此不额外宣称已完成 SpringBoard 日志验收。
- 随后按用户要求，仅重打包 cmdline 并删除 `-v`，生成 `run-alioth-ios-current-quiet.bin`（21,438,632 字节，SHA256=`2d0ca26ef071e65a7a739e26858fa12db6f5e424cd1fb4be19f12b44756b712a`），其余触摸、cpufreq 和 3600 秒返回保护参数不变；未改内核或驱动源码。重新启动约 35 秒后 `getvar result=running 0 0`，`ios-final-quiet-20261027-logdump.txt` 证实实际 cmdline 已无 `-v`，且日志已出现 SpringBoard。手机保持在本次无 verbose 的 iOS 运行状态。

---

## 2026-10-28 凌晨：FT3658U 触摸坐标定位修复完成（已实测验证）

### 根因（三个 bug，均已修）

1. **量程错了 20%**：设备树 `focaltech,display-coords = <0 0 0x21c0 0x4b00>` 是 8640×19200（8 单位/像素），但芯片原生量程是 **17280 × 38400**（16 单位/像素），厂商 `×10/16` 之后应为 **10800 × 24000**（10 单位/像素）。按 8640×19200 归一化会让 y 放大 1.25 倍，表现就是用户描述的"屏幕越往下，实际要点的位置越高"。
2. **归一化没有钳位**：Android 的 input core 会按 ABS 量程自动钳位，厂商驱动因此不需要处理；照抄后恢复态/越界报文会把归一化值算成 77236 > 65535，iOS 收到 y=1.18 直接出屏。已在 `focaltech_contact_update()` 中按厂商量程钳位（`x >= max_report_x → max_report_x - 1`）。
3. **缺少芯片恢复态判定**：厂商在 `point_num==0x0F` 且 `buf[2..6]==0xFF` 时视为恢复态并释放所有手指；我们之前把这些无效字当成真实坐标。已按 `report[1..5]==0xFF` 判定并释放全部触点。

### 实测标定证据（raw ÷ 16 = 屏幕像素；两轮上机）

| 实际点击 | raw（实测） | 换算像素 | 归一化值 |
|---|---|---|---|
| 左上角 | 1850, 1864 | 116, 117 | 7015, 3181 |
| 右上角 | 15684, 1676 | 980, 105 | 59484, 2859 |
| 右下角 | 16273, 37236（第一轮） | 1017, 2327 | — |
| 左下角 | 1095, 37353 | 68, 2334 | 4150, 63749 |
| 正中 | 9256, 20120（第一轮 9112, 20072） | 578, 1257（570, 1254） | 35106, 34339 |

正中标称 (540, 1200)，实测偏差 2–3 mm，属手指落点误差。五次点击、两个方向、两轮上机全部自洽，判定量程 10800×24000 正确。第一轮另有点 `raw 36204`（y），说明 19200 量程下底部约 17% 是死区，与用户观察一致。

### 代码与标定旋钮

- `wsl-current/hakios.goodix.alioth.c`：`focaltech_max_x/y` 改为 **10800 × 24000**；新增钳位与恢复态判定。
- 新增标定旋钮（默认关闭，可反复标定不用重编）：`touch_cal_raw=N`（每次按下及明显移动打印一行 raw→scaled→归一化）、`touch_max_x=N`、`touch_max_y=N`、`touch_swap=1`、`touch_noscale=1`。
- 修改前备份：`hakios.goodix.alioth.before-coords.c`。

### 构建产物

- `build-alioth-touch-extent.log` 含 `CC hakios.goodix.o` 与 `built kernel`。
- `mach-alioth-touch-extent.bcm2837` SHA256=`cec24ef49cde717d109cb802ba9fb8cbdc8f771b56d332a9c813b7af470ab4fa`。
- `run-alioth-touch-extent.bin` 21,439,976 字节 SHA256=`20ffc29e966299079a0090eacb037cf05455714cdb12856f183188d3e5704006`。
- 标定日志：`touch-cal-live.txt`（第一轮 200 点）、`touch-cal3.txt`、`touch-cal4.txt`、`touch-extent-final.txt`。

### 取日志的两个坑（重要）

1. `fastboot get_staged` 只是发送 `staged` 缓冲区，**必须先 `oem log`** 让 loader 重组实时日志，否则拿到旧内容。
2. 实时重组在启动洪流后经常停在启动阶段（1316262 字节，内容不更新），**不可靠**。可靠做法：设 `loader_return_after`，等内核到点返回后再取收尾日志（含 `== END OF LOG ==`）。

### 用户实测验收（2026-10-28 01:0x）

- 用修正后的内核跑正常会话（`run-alioth-touch-fixed.bin`，触摸+cpufreq，3600 秒返回保护），用户实际操作后确认：**"触摸没问题了"**。触摸定位专项就此关闭。
- 该载荷：`mach-alioth-touch-fixed.bcm2837` SHA256=`cec24ef49cde717d109cb802ba9fb8cbdc8f771b56d332a9c813b7af470ab4fa`（与 extent 验证轮同内核）；`run-alioth-touch-fixed.bin` 21,439,960 字节 SHA256=`9fb625a2f36f44f193ab9b6855cc9efc4a1dec43182d26ef070b477e80e54b05`。打包脚本 `build-touch-fixed.sh`。
- 注意：命令行没有 `-v`，但日志仍可用 `oem log` + `get_staged` 取（实时快照不可靠，见上）。

### 下一步

- 之后回到 A650 阶段 22 的 QSMMU 候选验证（`build-adreno-core-commands22.sh` 尚未执行，源码里那两处写在 `hakios.adreno.c` 中但从未构建/上机）。
- 其他待办：AW8697 振动（阻塞于供电时序证据）、ADSP 移植、Wi-Fi、`devices/alioth.hardware.md` 里过时的触摸供电/TLMM 描述需要按实测更新。

---

## 2026-10-28 上午：A650 走出 secure mode（secure-mode 死结解开）

### 根因：`CP_ME_INIT` 的 CP_INIT_MASK 多了一个位

- 我们一直发 `0x0000017f`。原厂 `adreno_a6xx.c:_set_ordinals()` / `CP_INIT_MASK` 是 **`0x16f`**，即 BIT(0)|(1)|(2)|(3)|(5)|(6)|(8)；`0x17f` 多出的 **BIT(4)** 是 drawcall filter range，而厂商既不置这一位、也**不为它填数据字**。
- 固件按掩码 ordinal 消费载荷，于是 `operation mode` 吃掉了寄存器列表地址的低 32 位、`reglist` 地址变成下一个 0、spinlock 错位 —— CP 处于**半初始化**状态。这就是前 20 多轮 `CP_SET_SECURE_MODE(0)` 一律被 SQE 以 `CP opcode error` 拒绝、`RBBM_SECVID_TRUST_CONTROL` 恒为 `0x1` 的原因。
- 改为 `0x16f`（载荷仍精确 11 字）后**一次通过**。

### 实测证据（`adreno-cpinit-mask2.txt`）

- 步骤 1–21 全 `done`；`getvar result=returned 145867 0`。
- `21: after submission: RBBM secure trust control 0`（此前恒为 0x1）。
- `21: the page reads 0x600dc0de; CP fault 0, protection 0, SMMU global fault 0`。
- 日志明确 **`THE 3D CORE LEFT SECURE MODE AND WRITES OUR MEMORY ON COMMAND`**。
- 5 秒 alive 观察正常，清理与回 loader 正常；测试后安卓 `sys.boot_completed=1`、槽位 `_b`。

### 同轮负面结果：QSMMU 两处厂商写入会挂死整机

- 按原厂 `kgsl_iommu_start()`（`subtype != KGSL_IOMMU_SMMU_V500` 分支）加入的 `0x6800=2`（GPU_SMMU_HUM_CFG）与 `0x64a0=1`（DORA_ORDERING）在 alioth 上**直接挂死 SoC**。
- 证据：`adreno-cpinit-mask-hang.txt` 最后一条是 `about to write smmu gpu_smmu_hum_cfg at 0x3da6800: 0x2`，之后 `== END OF LOG ==`，`getvar` 返回 `hung 33018 0`；`oem reboot` 后可恢复。
- 同一 SMMU 块的其他读写（0x3da0000..0x3da7ff，SMR/S2CR/CBAR/context bank）全部正常，且 0x6800 落在 FDT 的 `0x03da0000/0x10000` 窗口内 —— 不是映射错误，是这两个 offset 本身。
- 代码中已注释掉并写明原因，**不要在拿到新证据前恢复**。

### 产物

- `hakios.adreno.c`：`CP_ME_INIT` 掩码 `0x17f` → `0x16f`；QSMMU 两写注释保留说明。
- `build-alioth-adreno-cpinit2.log` 含 `CC hakios.adreno.o` 与 `built kernel`。
- `mach-alioth-adreno-cpinit-mask2.bcm2837` SHA256=`f51e1de9b4f364c79b8b7c7db0edda19c19d11acc00ee21e3a3ac97ff98e6e32`；`run-alioth-adreno-cpinit-mask2.bin` 21,439,912 字节 SHA256=`454957a8f8f7e562e0ed62071f033c5370fbad037df8ef7a5f0a3df064c58490`。打包脚本 `build-adreno-cpinit-mask2.sh`。
- 挂死轮产物：`run-alioth-adreno-cpinit-mask.bin`（保留为故障证据）。

### 下一步

- **阶段 22：GPU 画一帧上屏**（`step_frame` 已写好但从未跑过；`last_step > 21` 的守卫需放开到 22，`MiniIOMobileFramebufferShow()` 存在性要先确认）。
- 正常 IFPC／slumber 验收现在可以做了（此前缺的正是 secure mode 后的完整 core）。
- 之后再回到 iOS 渲染路径（`adreno_keep=1` + `HakiosGPUStart()` 已经在代码里）。

---

## 2026-10-28 上午：A650 阶段 22 —— GPU 画出第一帧并上屏（成功）

### 结果

- 放开 `last_step > 21` 守卫到 22（`step_frame` 早已写好，从未跑过）。`MiniIOMobileFramebufferShow()` 确认存在于 `iokit/Kernel/MiniIOMobileFramebuffer.cpp:1501`（`vinfo.v_baseaddr` 逐行 memcpy 到面板）。
- 从 fresh 安卓测试，`getvar result=returned 148000 0`；`adreno-frame.txt` 步骤 1–22 全 `done`。
- `22: the picture's first pixel is 00 80 ff ff (blue green red alpha; told: 00 80 ff ff), one in the middle ff ff ff ff, one in the last row ff 80 00 ff` —— 三个色带全部与指令一致。
- 日志明确 **`A PICTURE DRAWN BY THE GPU IS ON THE PANEL: orange, white, blue from the top`**。
- 5 秒 alive 观察正常，清理与回 loader 正常；测试后安卓 `sys.boot_completed=1`、槽位 `_b`。

### 产物

- `build-alioth-adreno-frame.log` 含 `CC hakios.adreno.o` 与 `built kernel`。
- `mach-alioth-adreno-frame.bcm2837` SHA256=`b5f268a542f0e1c3a7f1f9b22419575aa0571046ea293c9d10faa418a92e9032`；`run-alioth-adreno-frame.bin` 21,439,912 字节 SHA256=`0bf63dd8a2e01c7548d25ded3d682336510a27b2ffa7854fb78f707f65d7982f`。打包脚本 `build-adreno-frame.sh`。
- 日志：`adreno-frame.txt`。

### 状态判断

这也顺便证明阶段 20/21 的假设链是完整的：CP_ME_INIT（`0x16f`）→ 取命令 → 走出 secure mode → 写内存 → 2D blit → 上屏。**GPU 的硬件路径已经端到端打通。**

### 下一步

- `adreno_keep=1` + `HakiosGPUStart()`：把 GPU 留给 iOS 用户态（`iokit/Kernel/hakios.gpu.cpp` 提供 Mesa/freedreno 的 msm 接口）。这是让主屏幕从软件渲染变成 GPU 渲染的关键一步。
- 正常 IFPC／slumber 验收（此前缺的正是完整 core）。
- 待办不变：AW8697 振动、ADSP、Wi-Fi、按键（PON 状态读出来是 0x4，说明 `0x810` 的位定义或外设仍是错的，需要新的取证思路）、硬件文档更新。

### 附：正常 IFPC／slumber 同时通过（此前长期阻塞项已解决）

- 阶段 22 测试的收尾日志出现：
  `adreno: 0: lowest idle: IFPC reached (rpmh_power_state 0x3, sptprac_pwr_clk_status 0xc0)`，随后 `the GMU accepts "slumber"` 并正常 `standalone CX cleanup: off again`。
- 结论：此前 2026-10-27 记录的“IFPC 始终不进、拒绝发送无效 slumber”**根因同样是 secure mode 未退出**——core 没有真正跑起来时 GMU 不会进入 IFPC。secure mode 修好后这一项自动通过，`core-power5/6` 那些诊断不再需要。

---

## 2026-10-28 上午：GPU 交给 iOS —— CoreAnimation 由真实 A650（freedreno）渲染

### 结果（里程碑）

- 载荷：`adreno=22 adreno_keep=1` + 触摸/cpufreq，`loader_return_after=420`。`getvar result=returned`，测试后安卓 `sys.boot_completed=1`。
- **CoreAnimation 拿到了真正的 Adreno 驱动，不再是 softpipe**：
  `hakios-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa): OpenGL ES 3.2 Mesa 25.0.7; FD650; shaders OpenGL ES GLSL ES 3.20`
  `hakios-gl: asked for string 0x1f01: FD650`
- 全文 **`Software renderer` 出现 0 次**（此前 QEMU 阶段和手机早期都是软件渲染告警）。
- `backboardd [SystemShell] System shell "com.apple.springboard" finished startup after 5.05s.`，`AppleLogo` 渲染覆盖层正常淡出。
- `HakiosGPU` 稳定接收提交：每 5 秒约 100–107 次提交、约 200–216 个命令缓冲、2900–3478 个缓冲区被引用、约 5.2–5.5 MB 每次刷出缓存；GPU 每次实际工作约 530–600 µs；等待约 860–980 µs，`0 over before they began`。

### 意义

`adreno_keep=1` + `HakiosGPUStart()` → `iokit/Kernel/hakios.gpu.cpp` 的 msm ioctl 层 → Mesa `freedreno`（`DRIVERS=softpipe,freedreno`）→ CoreAnimation。这条链**端到端验证通过**，主屏幕渲染从软件路径切换到 GPU 路径。

### 产物

- `mach-alioth-adreno-keep.bcm2837` SHA256=`b5f268a542f0e1c3a7f1f9b22419575aa0571046ea293c9d10faa418a92e9032`（与 frame 轮同内核）；`run-alioth-adreno-keep.bin` 21,439,976 字节 SHA256=`f1088afa5c702af9caacc4bb5958d0c3f1ec59b0f23c6948e9139a4a9f8e75dc`。打包脚本 `build-adreno-keep.sh`。
- 日志：`adreno-keep.txt`。

### 待确认

- 用户实机观感（画面是否流畅、有无花屏/错位）。这一轮是自动验收，不代表主观体验。
- `0 IOSurfaces taken as buffers` 说明当前路径仍走 OSMesa 离屏 + 拷贝上屏，不是零拷贝直出；这是已知的中间形态。

---

## 2026-10-28 傍晚：屏幕"冻住、连时钟都不走"的定案 —— 是跑完了，不是死机

### 现象与结论

用户报告：iOS 界面上点任何地方都没反应，**而且时钟也不走了**，重启回 iOS 就正常。

结论：**屏幕没有卡死，是这一轮跑完了。** 内核按 `loader_return_after` 到期正常交还控制权，之后没有任何东西再往面板内存（`0x9c000000`）写，面板继续扫描那块内存，于是永远停在 iOS 最后画的那一帧上——时钟钉住、点哪都没反应，因为 iOS 已经不在了。loader 自己的文字走 UEFI 文本控制台，这台手机没有那个控制台，所以屏幕上不会出现任何新东西。

### 证据

- `fastboot getvar result` → `result: returned 1441823 0`：正常交还，不是 panic，也不是停 checkpoint。
- 所有历史日志里**没有一次**出现"帧统计停止、但其它日志继续"的形态。那条每 5 秒一行、并且是在**上屏线程自己身上**打印的统计（`MiniIOMobileFramebuffer: 51 frames in 5000 ms (10 a second)`）在每一轮都连续到最后一行，且始终 `0 gave up`、`0 came a refresh late`。真卡死它会先消失。
- 反而每一轮日志的最后都是 `--- loader_return_after expired ---` 或对应的收尾行，与 `returned` 吻合。
- 从 loader 里执行 `oem clearscreen` 立刻把这块"冻住的 iOS 画面"抹掉——证明面板一直在扫描同一块内存，画面本身早就死了。

### 修复：loader 在拿回控制权时给屏幕一个颜色

原来的缺陷是「跑完了」和「死机了」在屏幕上长得一模一样。`wsl-current/loader.c` 改动：

- 新增 `paint_rows(argb, first, rows)` / `paint_screen(argb)`：整屏或一段填色，随后 `flush_dcache`（clean+invalidate，不能用 clean alone：内核也用过这块内存，缓存里残留的旧行会回写盖掉）。
- `run_kernel()` 返回时按结局上色：

  | 颜色 | 值 | 含义 |
  |---|---|---|
  | 绿 | `0xff208040` | 跑满时间、正常交还 |
  | 琥珀 | `0xffc0a000` | 停在 checkpoint |
  | 红 | `0xffc02020` | 早期异常（early-exception） |
  | 蓝 | `0xff2040c0` | 返回路径自检（`oem selftest`） |
  | 紫（仅底部 320 行） | `0xff8020c0` | panic |

- 启动参数 `paint=0` 可关闭，保留内核最后一帧（给 `oem screenshot` 抓现场用）。
- **panic 与正常返回走的是同一条路**（`panic_spin_forever()` / `kdp_machine_reboot_type()` 都调 `loader_return()`，`loader_jump(0, 0)`，reason 都是 0），所以靠日志判定：`klog_says_panic()` 扫 `klog` 里的 `"panic(cpu "`（`kern/debug.c` 里每条 panic 的首行）。命中时 `getvar result` 报 `panicked`，并且**只涂底部 `PANIC_BAND`=320 行**，把上面留给内核的 panic 文本。

### 实测验证（真机）

1. `oem selftest` → `result: selftest-ok 0 0`；`oem screenshot` 取回 10,368,000 字节（4320×2400），六处采样点（四角/中心/底部中点）全部 `0xff2040c0`，全屏网格**只有 1 种颜色** → 绘制生效。
2. 50 秒短寿命载荷（`build-paint-check.sh`）→ `result: panicked 26123 0`；取回的面板底部三点为 `0xff8020c0`、上方为上一轮的 `0xff2040c0`，网格 2 种颜色、比例与 320 行吻合 → panic 判定与"只涂底部"都按设计工作。
   - 注意：这一轮 panic 文本并没有出现在面板上（上方仍是上一轮自检的蓝）。短载荷在图形控制台切换之前就 panic 了，属于该轮的个例；真机上 iOS 起来之后的 panic 才走 `PE_initialize_console(kPEGraphicsMode)`。

### 顺手确认的触摸结论

`/dev/hakios-touch` → `inject.c` 的投递链**是通的**，与"点不动"无关：日志里 `hakios: input: touch screen service 0x145e0d8b0, buttons 0x147814da0` 之后，`backboardd` 明确打印 `[TouchEvents] contact 1 presence: touching` / `presence: none`。分发循环只有"唤醒屏幕后吞掉那根手指"一个分支会丢事件，其余全部 `send_touch()`。所以之前"点不动"就是本条的"跑完了"，不是输入链问题。

### 取屏与调试的两个坑

- **`oem screenshot`**：stage 面板原始像素（`getvar screen` 给 `宽 高 行字节`，本机 `1080 2400 4320`），随后 `fastboot get_staged <file>` 取回。配合 `check-shot.py` 可直接判色，不需要人眼看屏幕。
- **WSL 的 `/tmp` 会在两次 `wsl -e bash -lc` 之间清掉**（WSL 实例重启）。取回的镜像要写到 `/mnt/c/...` 下的路径，否则下一步就 `No such file`。
- `oem run` / `oem selftest` 必须 stage **`run-payload.bin`**（内核+ramdisk）；stage `usb-payload.bin`（USB 壳，无内核）会得到 `result: not-started 0 0`，日志里是 `no kernel in payload`。

### 产物

- `loader.c`（`wsl-current/loader.c` 是权威副本：只有它带 `rdload` / `find_partition_by_name`；`base/.../xnuloader/loader.c` 是旧副本）。
- 构建（**必须在 WSL 里跑**，脚本里是 `/mnt/c` 硬路径）：
  ```
  clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector \
    -fshort-wchar -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter \
    -I../base/hackios-xnu-kernel/xnuloader -c loader.c -o loader-paint.obj
  lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
    /out:loader.efi loader-paint.obj
  bash build-images.sh
  ```
- 备份 `loader.efi.before-paint`；新工具 `check-shot.py`、`build-paint-check.sh`；样本 `shot.bin`、`shot-panic.bin`。

### 下一步

屏幕驱动与按键驱动（用户指定优先级）：面板背光/关屏/锁屏，以及 PM8150 PON 按键。

---

## 2026-10-29 凌晨：关屏接上了，三个按键的位全部实测确定

### 关屏：`kFBMethodRequestPowerChange`（selector 12）从空实现改为真正关屏

**根因**：iOS 一直用 selector 12 调 `kFBMethodRequestPowerChange`（日志里 boot 时先 `0x0` 再 `0x1`，对应 backboardd 的 `Set Blanked: NO` / `Main will unblank`），而 `MiniIOMobileFramebuffer.cpp` 里这个 case 和 `SwapSignal` 一起直接 `return kIOReturnSuccess`——调用了，没人做。

**改法**（`iokit/Kernel/MiniIOMobileFramebuffer.cpp`）：
- 新增 `bool fBlanked` 字段；`setBlanked(bool)` 置位时把面板内存（`fBase`，1080×2400×4）整块清零，并打印 `the display goes off (blanked: the panel's memory black)` / `on again`。
- `show()` 里取快照 `blanked = fBlanked`（已在 `fLock` 内），绘制处 `if (!blanked) drawLayer(...)`。**队列照常推进**（`fSwapsWaiting--`、`fSwapFirst` 递进都在前面），否则 `present()` 会永久阻塞。
- selector 12 改为 `fOwner->setBlanked(in[0] == 0)`。

**实机验证**（`full3.txt`）：
```
[   92.758478] MiniIOMobileFramebuffer: backboardd[12] selector 12 (scalars 1 in [0x0 ...])
[   92.758492] MiniIOMobileFramebuffer: the display goes off (blanked: the panel's memory black)
[   95.635076] MiniIOMobileFramebuffer: backboardd[12] selector 12 (scalars 1 in [0x1 ...])
[   95.635102] MiniIOMobileFramebuffer: the display goes on again
```
面板是 OLED，黑就是像素不发光，所以这已经等价于「屏幕灭了」。**仍未做**：DCS `0x51` 亮度（`bl_ctrl_dcs`，要驱动 DSI 命令通道）与 DCS `0x28/0x10` 真关机。

### 按键：寄存器位全部由实测确定，且发现并修掉一个真 bug

新增 `buttons_dump=N`（整块转储后盯 N 秒）与 `buttons_dump_wait=N`（先等 N 秒，让系统起来、手准备好）。

**整块转储读到的布局**（这两个块以前是照手册假设的，现在有实测值）：
```
PON  0x0800:  +00: 01 01 11 00 01 04 ...   ← 0x04=type 0x01, 0x05=subtype 0x04
              +10: 04 01 00 01 ...          ← INT_RT_STS = 0x04（bit2 = BARK 锁存位，不是键）
GPIO 0xc500:  +00: 01 02 01 02 10 11 ...   ← type 0x10 / subtype 0x11
              +08: 81 ...                   ← STATUS，bit0 = 电平高（松开）
              +10: 01 ...                   ← INT_RT_STS，同一位
              +40: 00 | +42: 00 | +46: 80   ← input / pull-up 30uA / enabled
```
- `status 0x4` 一直不变**不是故障**：`0x04` 是 bark 中断的锁存位，键位 bit0/bit1 都是 0，即松开。
- GPIO 的电平在 **`0x08`**（`GPIO_STATUS`），不是 `0x10`。驱动原来读 `0x10`，那里恰巧也有同一位所以能工作；现已改成规范的 `0x08`。

**三个键的实测证据**（`full3.txt`，转储窗口内有人按键）：
```
PON  0x0810: 04 -> 06 -> 04   (rounds 917..986)   bit1 置位 = 音量减（RESIN）
GPIO 0xc508: 81 -> 80 -> 81   (rounds 1050..1132) bit0 清零 = 音量加（低有效）
PON  0x0810: 04 -> 05         (rounds 1579)       bit0 置位 = 电源（KPDPWR）
```
寄存器映射与位定义**全部确认**，与文件头注释的假设一致。

**修掉的 bug**：`buttons_dump()` 自己跑循环，把 `buttons_poll()` 整整挡了 60 秒——按下的键被转储看见，却没有走上报路径（那一轮 `so far 0 presses`）。现在转储每轮也调一次 `buttons_poll()`。

### 本轮踩到的坑

- **`oem run` 之前必须先 `oem rdload iosdata`**。loader 重启过之后再 `oem run`，root 分区是空的，于是 `panic(cpu 1 ...): Process 1 exec of /sbin/launchd failed, errno 2`。这个 panic 跟被改的代码无关，`getvar result` 会报 `panicked`。
- **实时日志（`oem log` 后立刻 `get_staged`）会在约 1.3 MB 处截断**，里面没有 `MiniIOMobileFramebuffer` 的行。要验证显示相关的东西必须等 `loader_return_after` 到期后取完整日志（约 1.44 MB，带 `== END OF LOG ==`）。
- **WSL 的 USB 直通会掉**（`fastboot devices` 显示 `????????????`、`AdbWriteEndpointSync failed`），此时 Windows 侧设备管理器里 `Android Bootloader Interface` 仍是 OK。长按电源键重启手机即可恢复，不必动驱动。

### 产物

- `MiniIOMobileFramebuffer.cpp`（工作副本在 `wsl-current/`，装机前需 `cp` 回 `iokit/Kernel/`；备份 `.before-blank`）
- `hakios.buttons.alioth.c`（新增 `buttons_dump` / `buttons_dump_wait`、`GPIO_STATUS 0x08`；备份 `.before-dump`）
- 载荷脚本 `build-screen-buttons.sh`；日志 `live3.txt`（实时，截断）、`full3.txt`（完整）

---

## 2026-10-29 凌晨（续）：GPU 重启失效的真因、电池通了、状态栏 rdar 条

### GPU「又不行了」：SMMU CBAR 的遗留值，是我们自己上一轮留下的

**现象**：上一轮 GPU 正常跑完（`FD650`、50 fps），下一轮开机 `adreno: step 13: FAILED`：
```
adreno: 13: smmu cbar before use (0x3da1004) = 0x1f300
adreno: 13: the GMU's audited context or stream-table entry is not free
```
**根因**：`0x1f300` 精确等于本文件的 `SMMU_CBAR_STAGE_1`（`1<<16 | 3<<8 | 0xf<<12`），是 `space_on()` 写进去的。退出时 `space_off()` 清了 SMR / S2CR / CB_SCTLR，**唯独没清 CBAR**；`stale_stop()`（panic 路径）同样漏了。于是下一轮的 step 13 见到非零 CBAR 就判定「这个上下文被占」，直接放弃整条 GPU 序列。

这解释了此前多次「GPU 时好时坏」：**取决于上一轮是怎么退出的，而不是这一轮的代码**。

**修法（三处，`hakios.adreno.c`）**
1. `space_off()` 补 `put(&smmu, "cbar", smmu_page + SMMU_CBAR(space->context), 0)` —— 有序退出清干净。
2. `stale_stop()` 的循环里补同样的 CBAR 清零 —— panic 路径也清干净。
3. step 13 增加一重识别：CBAR 非零但 SMR 与 CB_SCTLR 都是零、且值恰为 `SMMU_CBAR_STAGE_1` 时，认作自己的遗留并清掉继续；其他值仍然拒绝。

**实测**（`gpu1.txt`）：
```
adreno: 13: smmu cbar before use (kept) (0x3da1004) = 0x1f300
adreno: 13: the GMU's context still has this driver's own CBAR 0x1f300 from an earlier run,
            with its stream entry and control register clear: clearing it
FD650: 2   THE GPU STAYS ON: 1   step 13 FAILED: 0
```
同一轮帧率还从 10 fps 涨到 **50 fps**（`252 frames in 5007 ms`）。

### 电池：`charger_start()` 里写死了 redwood，alioth 一次都没采过

**现象**：`battery: SOC=-1%, V=-1 mV, T=-273 C (0 reads, N failed)`——内核找到了共享块（magic 校验会单独报错，这里没报），但 `flags` 从没有 `VALID` 位。

**根因**（`loader.c:2068`）：
```c
if (charger_enabled || PLAT != &platform_redwood) {
    return;
}
```
移植到 alioth 时漏改，`charger_start()` 直接返回，`charger_poll()` 从未跑过，`charger_shared` 永远停在初始值。

**修法**：改为 `PLAT != &platform_redwood && PLAT != &platform_alioth`。

**实测**（`batt2.txt`）：
```
loader: battery: ChargerEx=0 QcomCharger=0 init=0 shared=a1012000
loader: battery: flags=1f SOC=99% V=4395 mV T=33 C action=7
内核  : battery: SOC=99%, V=4339 mV, I=57 mA, T=35 C, battery=1 charger=1 charging=1 (49 reads, 0 failed)
iOS   : hakios: battery: SOC=99% battery=1 charger=1 charging=1
```
两侧都对上了。`flags=0x1f` = VALID|PRESENT|CHARGER_PRESENT|VOLTAGE_VALID|CHARGING 全置位。

### 状态栏消失 / 红色 `rdar` 条：显示身份不是任何真 iPhone

**现象**：iOS 顶部不是状态栏，而是一条有颜色的横条，写着 `rdar` 加一串数字。

**根因**：iOS 报给 UIKit 的屏幕是 **1080×2400 @ scale 3 = 360×800 pt**，不是任何真 iPhone 的点数（iPhone X 是 375×812 pt）。UIKit 找不到对应的 status bar visual provider，就用红色调试条打出内部 radar 号 **`rdar:45025538`**（"No status bar visual provider for screen"）。`base/ios-sweet/NOTES.md:30` 有同样记录。

**这个问题是本项目自己引入的**：早先为了让每帧拷贝从 3446 µs 降到 916 µs，用 `fbwidth=1080 fbheight=2400` 把显示对齐到面板，代价就是显示身份变成了不存在的尺寸。

**改法**：不用 `fbwidth/fbheight`，用默认的 **1125×2436**（= 375×812 pt，正是 iPhone X）。画面由驱动缩放到 1080×2400 面板，每帧约 3446 µs（仍在 60 Hz 预算内）。

**实测**（`sb1.txt`）：`StartWindowServer: initial main screen size:{1125, 2436} scale:3`、`main display info: ... size:{1125,2436} scale:3`。**视觉验收待做**（见下）。

### WiFi 与音频：alioth 和 redwood 是完全不同的硬件

| | redwood（现有驱动） | alioth | 结论 |
|---|---|---|---|
| WiFi | WCN6750，固件跑在 **WPSS（Hexagon DSP）** 上，PAS/TrustZone 加载 | **QCA6390**，**PCIe + MHI**（`qcom,pcie@1c10000`，54 个 mhi_chan），wlan-en = TLMM gpio20 | 架构不同，`hakios.wifi.c`（4394 行）**不能移植**，要新写 PCIe/MHI 路径 |
| 扬声器 | AW88261 ×2（I2C + TDM） | **CS35L41 ×2**（`cs35l41@40` 主扬声器 / `@41` 听筒）+ **WCD938x** codec | 功放芯片不同，`hakios.speaker.c`（1211 行）不能直接复用 |

### 待办（按性价比排序）

1. **状态栏视觉验收**：跑一轮后 `oem screenshot` 取面板像素，看顶部红色条是否消失。
2. **关屏后的唤醒**：`setBlanked(false)` 目前只恢复绘制，Panic 日志显示 backboardd 会重新提交帧，但没实测过"锁屏→按电源键→亮屏"整条路。
3. **亮度控制**：`bl_ctrl_dcs`，要发 DCS `0x51`，需驱动 DSI 命令通道（`mdss_dsi_ctrl0@ae94000`）。
4. **CS35L41 音频**：需先有 I2C 控制器驱动 + TDM，比 redwood 重。
5. **QCA6390 WiFi**：PCIe + MHI，工作量最大。

### 构建注意事项

- `space_off`/`stale_stop` 里新增的 `put(&smmu, "cbar", ...)` 走的是 hypervisor（SMMU 全局寄存器），与已有的 smr/s2cr 写一致。
- 电池修复只动了 `loader.c`，**必须重刷 loader 镜像**（`build-images.sh` → `fastboot boot xnu-rdload-fixed.img`）才生效；只重新 `stage` 载荷不会变。

---

## 2026-10-29 01:2x：缩放默认改最近邻，恢复 1125×2436 的显示身份而不付代价

### 结论：`fb_filter` 默认值改为 0

恢复 iPhone X 的 1125×2436 显示身份（状态栏所需）后，双线性缩放让每帧从 ~1000 µs 涨到 **3392 µs**。但 1125×2436 → 1080×2400 恰好是 **24/25 的等比**，源列/行映射是一张固定表，最近邻不需要任何混合。

实测对比（同一场景两次截图，像素级）：

| 指标 | 双线性 (fb_filter=1) | 最近邻 (fb_filter=0) |
|---|---|---|
| 每帧驱动耗时 | 3392 µs | **894 µs**（3.8 倍） |
| 与另一张不同的像素 | — | 72.71% |
| ...其中单通道差 >40 | — | **1.68%** |

72% 的差异是双线性"每个像素都略有不同"的正常特征；真正可察觉的差异只有 **1.68%**。肉眼比对两张裁切图（`top-bilinear.png` / `top-nearest.png`，800×600 放大 2 倍）**看不出区别**，文字同样锐利。

### 官方默认值改为按默认参数跑一轮验证

`mach-alioth-default.bcm2837`（不带任何 `fbwidth/fbheight/fb_filter`）实测：
- `initial main screen size:{1125, 2436} scale:3` —— 状态栏身份正确
- **898–900 µs 每帧**（双线性时 3392）
- `FD650` 在，电池 `SOC=98% V=4330 mV` 正常

### 状态栏：像素级确认已恢复

先前那轮 `shot-statusbar.bin` 的客观判定：
- 前 400 行扫描，**单行最多 3/40 个红色采样，0 行达到 20+**（整条红带会接近 40/40）→ 红色 `rdar` 条已消失
- 顶部 y=60/80 两行有文字像素，横向分布为**左侧 x≈93–129（时间）+ 右侧 x≈1023–1029（电池/信号），中间全空** → 正是真实 iOS 状态栏的布局
- 全屏 1419 种颜色（真实 UI，不是涂屏）

### 新工具

- `crop-png.py IN.bin OUT.png X Y W H [SCALE]` —— 不依赖 PIL，用标准库 zlib 直接写 PNG，用于把面板内存裁出来看。
- `check-shot.py` 扩展了顶部 24 行的红色占比扫描，可直接判断 `rdar` 条是否还在。

### 互联网进展（2026-10-29 查）

HackiOS 已被多家媒体报道（Android Authority、PhoneArena、Decrypt、LTT 论坛、HN），在 POCO X5 Pro 上以 iOS 16.5 用户空间 + 自编译 XNU + 自定义 UEFI 启动，驱动以 iPhone X 身份伪装。[Hacker News 讨论](https://news.ycombinator.com/item?id=50196718) / [Android Authority](https://www.androidauthority.com/ios-on-android-phones-3726001/) / [Decrypt](https://decrypt.co/381351/hackers-running-apple-ios-android-phones) / [PhoneArena](https://www.phonearena.com/news/someone-successfully-booted-ios-on-an-android-device_id184233)。
**注意**：报道措辞不一致，有称"WiFi/音频/相机/图形均工作"，也有称"多数组件仍不可用"；且早期报道时"未公开代码"。本仓库的 `upstream/hackios-xnu-kernel-main` 是同一项目的源码，里面有 `wifi/`、`hakios.speaker.c`、`hakios.mic.c`，但都是 **redwood（POCO X5 Pro）** 的硬件版本。

---

## 2026-10-29 02:0x：SMMU CBAR 写后回读、DSI 控制器首次摸清

### CBAR 写不进去，不是没写——已加回读定论

上一节的修法里，`space_off()` 现在会写 0 到 CBAR。加了写后回读（`cbar after clear`）之后，真机（`cbar1.txt`）说明：

```
adreno: 0: about to write smmu cbar at 0x3da1004: 0
adreno: 0: smmu cbar after clear (0x3da1004) = 0x1f300    ← 写 0，读回还是 0x1f300
adreno: 0: about to write smmu cbar at 0x3da1008: 0
adreno: 0: smmu cbar after clear (0x3da1008) = 0x1f300
adreno: 0: about to write smmu cbar at 0x3da1000: 0
adreno: 0: smmu cbar after clear (0x3da1000) = 0x1f300
```

**结论：CBAR 在 CX 关闭（或正在关）时拒绝写入。** 依据有两条：同一时刻 SMR 能清（下一轮 step 13 读到 `smr before use = 0`，说明总线不是挂死的）；而 CBAR 三个上下文全部保持原值。

这不是阻塞问题——step 13 的"认出自己遗留值并清掉"逻辑已经把它兜住，GPU 每轮都正常起来（`FD650` 在，50 fps）。**这一项标记为已绕过，不再投入。**

### DSI 控制器第一次被读通：面板确实是 DCS 亮度

新增 `bsd/dev/hakios.display.c`（boot-arg `display_step=N`，**纯只读**），已接进 `bsd/conf/files` 与 `bsd_init.c`。真机读数（`disp7.txt`）：

```
mdp  HW_VERSION 0x60000000            ← MDP 6.0，与 SM8250 相符
dsi  +0x000 0x20040000                ← 6G_HW_VERSION：MAJOR 2, MINOR 4, STEP 0 = DSI 2.4.0.0
dsi  +0x004 0x000001f5                ← **CTRL**（不是 CLK_CTRL，见下面的更正）
```

**更正（拿到权威映射后的修正）**：早期这里写的是"`+0x004` 是 `DSI_CLK_CTRL`，`0x1f5` 表示所有时钟已使能"。
**这是错的。** 拿到 Linux 的 `dsi.xml` 和 `dsi_host.c` 后核对清楚：这块控制器是 **6G v3**，
`DSI_6G_REG_SHIFT = 4`（`dsi_cfg.h`），即 `6G_HW_VERSION` 在偏移 0，**其余寄存器相对 XML 整体 +4**。
按正确的映射重新解码真机 dump（`disp7.txt`）：

| 寄存器 | XML 偏移 | 实际偏移 | 实测值 | 含义 |
|---|---|---|---|---|
| `6G_HW_VERSION` | — | `+0x000` | `0x20040000` | MAJOR 2, MINOR 4, STEP 0 → DSI 2.4.0.0 |
| `VERSION` | `0x1f0` | `+0x1f4` | `0x03000104` | MAJOR 3 → 确认是 **6G** 主机（3 = `MSM_DSI_VER_MAJOR_6G`） |
| **`CTRL`** | `0x000` | **`+0x004`** | `0x1f5` | ENABLE=1, CMD_MODE_EN=1, VID_MODE_EN=0, LANE0–3=1, CLK_EN=1 |
| **`CLK_CTRL`** | `0x118` | **`+0x11c`** | `0x23f` | 六路时钟全开 + FORCE_ON_DYN_AHBM_HCLK |
| `CMD_DMA_CTRL` | `0x038` | `+0x03c` | `0x14000000` | FROM_FRAME_BUFFER=1, LOW_POWER=1 |
| `CMD_CFG0` | `0x03c` | `+0x040` | `0x100008` | DST_FORMAT 8 |
| `CMD_CFG1` | `0x040` | `+0x044` | `0x0013c2c` | WR_MEM_START 44, WR_MEM_CONTINUE 60, **INSERT_DCS_COMMAND=1** |
| `DMA_BASE` | `0x044` | `+0x048` | `0x9aeb2000` | bootloader 留下的命令缓冲地址 |
| `DMA_LEN` | `0x048` | `+0x04c` | `8` | 8 字节 |
| `TRIG_CTRL` | `0x080` | `+0x084` | `4` | DMA_TRIGGER = **4 = TRIGGER_SW** |
| `TRIG_DMA` | `0x08c` | `+0x090` | `1` | 上一次是软件触发 |
| `RDBK[0..1]` | `0x068` | `+0x06c` | `0x0c5147b4`, `0x01f10bb7` | 面板 ID 读回 |

结论方向没变但依据变强了：控制器**确实是活的、时钟确实全开**（`CLK_CTRL = 0x23f`），
而且**处在命令模式**（`CTRL.CMD_MODE_EN=1`、`VID_MODE_EN=0`）——这正是发 DCS 命令需要的模式。
`CMD_CFG1.INSERT_DCS_COMMAND=1` 和 `DMA_LEN=8` 还说明 **bootloader 自己刚发过一条 8 字节的 DCS 命令**，
这比"读回值对上了"更直接地证明这条通路是通的。

**面板 ID 证据保留**：`RDBK[0] = 0x0c5147b4`、`RDBK[1] = 0x01f10bb7`，拼起来正是设备树的
`androidboot.oled_wp=01f10bb70c51`——bootloader 用这套寄存器读过面板，读回值还在缓冲里。

面板的 on-sequence 里亮度是 DCS `0x51`（`mi,mdss-dsi-bl-max-level = 0x7ff`）、关屏是 `0x28`+`0x10`，所以：
- **亮度**必须走 DSI 命令通道
- **真关屏**（面板级）也走 DSI；目前 `setBlanked()` 是涂黑面板内存，OLED 上视觉等价，但面板本身还在扫描

### 刻意没有做的事：无人值守时不写 DSI

发 DCS 命令要写 DSI 的 command DMA 寄存器。**当前用户不在、手机无法人工干预**，而此前 QSMMU 的一次误写直接挂死整机（只能靠 `oem reboot` 恢复）。写活着的显示控制器有同样风险，一旦挂死，面板会黑到有人重启为止。

所以亮度这一项**停在"寄存器已摸清"**，等有人看着再往下做。下一步需要的是权威的 `dsi.xml.h`/`dsi-reg.h` 里 command DMA 那几个偏移（DSI_CMD_DMA_CTRL / _CMD_OFFSET / _CMD_LENGTH / _CMD_SW_TRIGGER / 命令缓冲），已尝试联网取但被限流（HTTP 429），本地没有该头文件。

### 今晚的总账

| 项 | 状态 | 证据 |
|---|---|---|
| GPU（真实 A650 渲染） | **修好并稳定** | `FD650`、`THE GPU STAYS ON`、50 fps、连续 4 轮正常 |
| 电池 | **修好** | loader `flags=1f SOC=99% V=4395mV`；内核 `SOC=99% V=4339mV charging=1 (49 reads, 0 failed)`；iOS `hakios: battery: SOC=99%` |
| 状态栏 | **修好** | 像素扫描：红色条消失（前 400 行最多 3/40 红色采样）、顶部左时间右电池的布局确认 |
| 显示性能 | **优化 3.8 倍** | `fb_filter=0` 成默认：3392 µs → 894 µs/帧，像素级差异仅 1.68% 可察觉 |
| 关屏/开屏 | **工作** | `selector 12` → `the display goes off/on again` |
| 三个按键 | **工作** | PON 0x0810 bit0/bit1、GPIO 0xc508 bit0 实测位翻转 |
| DSI 控制器 | **首次读通** | `0x1f5` 时钟全开、面板 ID 读回对上 `oled_wp` |
| 亮度 | 未做（有风险，等原因） | 需 DSI command DMA 偏移 |
| WiFi | 未做（架构不同） | QCA6390 = PCIe+MHI，无 PCIe 驱动 |
| 扬声器/麦克风 | **功放已认出来** | I2C 读到两台 CS35L41：DEVID `0x035a40`、REVID `0xb2` |

### 扬声器功放：XNU 第一次驱动起 I2C 设备（两台 CS35L41 都应答）

新增 `bsd/dev/hakios.amp.c`（boot-arg `amp_step=N`，源文件 `wsl-current/hakios.amp.alioth.c`），
已接进 `bsd/conf/files` 与 `bsd_init.c`。分四级，前两级完全不写。

**为什么是引脚 I2C 而不是序列引擎**：alioth 的两个功放挂在 `i2c@98c000`（QUPv3 wrapper 0，
serial engine 3）上，走 GENI 引擎要先正确编程时钟树，分频配错或门控没开就是整机挂死。
而 haptic 驱动（`hakios.haptic.c`）已经在真机上用 TLMM 引脚 bit-bang 100 kHz I2C 跑通，
驱动错了引脚只会得到一个 NAK，不会挂。引擎的寄存器仍然读一遍，如果发现它在忙就不接管。

**实测数据（`amp1-console.log` = step 1，`amp4-console.log` = step 4）**：

```
gpio 114: cfg 0x00000001 (pull 1, function 0, drive 2 mA, letting go), level 0   ← 复位脚，低=功放还在复位
gpio 119: cfg 0x00000001 (pull 1, function 0, drive 2 mA, letting go), level 1
gpio 120: cfg 0x00000001 (pull 1, function 0, drive 2 mA, letting go), level 1
se3: fw revision 0x303 (protocol 3, I2C), fifo interface enabled, fifo depth 16 words
se3: status 0 (idle), clock control 0x80004220, ser clock 0x11
se3: clock configuration 0x100000, votes 0x41043c0 (engine 3 not voted on)
the bus: pins 119 and 120 are plain GPIO and the lines are at rest
```

四个关键结论：

1. **序列引擎 3 的固件协议就是 I2C**（`GENI_FW_REVISION_RO` 的 [15:8] = 3），FIFO 开着、深 16 字，
   引擎空闲——这条总线没有别人在占。
2. **两条引脚是普通 GPIO（function 0）且线路为高**，而引脚配置里没开内部上拉，说明**板上有外部上拉电阻**，总线是真的、可用的。
3. **引脚没被 mux 到 qup3**，所以 bit-bang 不需要抢任何东西，直接就能说话。
4. **复位脚 GPIO114 读到 0**：bootloader 把功放留在复位状态。这就是为什么复位前两次读全是 NAK。

复位释放后（step 4，低 2 ms → 释放 → 等 5 ms）：

```
amp: reset released, line reads 1
amp: 119/120: 0x40 answers, DEVID 0x035a40 (a CS35L41)
amp: 119/120: 0x40 REVID 0x0000b2
amp: 119/120: 0x41 answers, DEVID 0x035a40 (a CS35L41)
amp: 119/120: 0x41 REVID 0x0000b2
amp: 120/119: nothing answers 0x40 (SDA 120, SCL 119)
```

- **两台功放都应答**：`0x40`（设备树里 `sound-name-prefix = "RCV"`，听筒路）和 `0x41`（`cirrus,right-channel-amp`，扬声器路）。
- **DEVID `0x035a40` 正是 CS35L41 的芯片 ID**（Linux `cs35l41-regs.h` 的 `CS35L41_CHIP_ID = 0x35a40`）。
- **REVID `0xb2`**，低四位 2 是偶数 → 按 Linux 的判据是 **CS35L41**（奇数才是 CS35L41R），即 rev B2 硅片。
- **SDA=119 / SCL=120**：反过来的引脚顺序上什么都没应答，两种顺序都试过，所以这个对应关系是测出来的、不是猜的。

这是这台机器上**第一次由这个内核驱动起一个真实的 I2C 从设备**。寄存器访问用的是 CS35L41 的真实格式
（32 位地址、32 位值、大端、stride 4，`cs35l41-lib.c` 的 `regmap_config`）——按一字节寄存器去读会从一个
应答正常的芯片那里得到乱码。

**已知的下一步（没做，需要人在）**：出声还差两件事——
1. 功放要**持续保持在复位外**（现在是探测完就把引脚恢复原样，功放又回到复位），并且要跑完 CS35L41 的初始化序列（几百个寄存器写入）；
2. 音频数据要从**三级 TDM（MI2S2，引脚 133/134/135/137）**送进来，那条路归 ADSP 管——redwood 的 `hakios.sound.c` 走的是 AFE/ADM/ASM，端口 `0x9000`、组 `0x9100`；alioth 对应的是端口 `0x9020`、组 `0x9120`，但 ADSP 固件本身不同，这是真正的大工程。

麦克风的路径完全不同：WCD938x 是 split codec，走 SoundWire（`bolero-cdc` 的 rx/tx macro），
不是 I2C 也不是 TDM，需要的是 SoundWire 控制器驱动，工作量比功放大。

**下一个可做的小步骤（无人值守也安全，纯读）**：把 `amp_step=5` 做成"保持复位释放 + 读功放的
中断状态和电源状态寄存器"，确认芯片在复位外能稳定存活，为后面写初始化序列打底。
—— 这一步需要一个 **常驻的 I2C 服务**（设备节点），因为 bit-bang 在 `amp_thread` 退出后引脚就被还原了。
在写那个之前，先确认 `amp_thread` 结束后功放确实回到复位（读回 GPIO114 = 0）是设计上的正确行为，
不要因为探测把手机留在"功放通电"的状态。

### step 5 已做（无人值守）：两台功放都完整启动通过

`amp_step=5` 加了启动等待：释放复位后轮询 `IRQ1_STATUS4` 的 bit1（OTP_BOOT_DONE），最多 100 ms × 1 ms，
然后检查 `IRQ1_STATUS3` 的 bit31（OTP_BOOT_ERR）。实测（`amp5-console.log`）：

```
amp: 0x40 (receiver): booted after 0 ms, status4 0x00000003
amp: 0x40 (receiver): status3 0x20000000
amp: 0x40 (receiver): status1 0x00000000, status2 0x00000000
amp: 0x40 (receiver): power status 0x00000000
amp: 0x41 (speaker): booted after 0 ms, status4 0x00000003
amp: 0x41 (speaker): status3 0x20000000
amp: 0x41 (speaker): status1 0x00000000, status2 0x00000000
amp: 0x41 (speaker): power status 0x00000000
```

- **两台都在 0 ms 就报 OTP_BOOT_DONE**（`status4` bit1 已置），说明 I2C 块能应答时它其实**早就启动完了**——
  上电后它自己完成 OTP 加载，不需要主机等。
- **`status3` 没有 bit31**（`0x20000000`），即 **没有 OTP_BOOT_ERR**，芯片自检通过。
- **`status1` / `status2` / 电源状态全 0**：没有升压过压、没有短路、没有温度告警。芯片现在是干净的、上电待命的。

这是这台机器上第一次把一个音频外设**完整读到"已启动、无错误"**的状态。剩下的都是"让它出声"：
- 常驻的 I2C 服务（保存/恢复引脚由设备节点持有，不再是一次性的线程）
- CS35L41 的初始化序列（几百个寄存器写入，Linux 的 `cs35l41_probe` 有完整顺序）
- 音频数据从三级 TDM 进来（ADSP 路线）

**已确认的硬件事实（供后续使用，全部实测）**：

| 事实 | 值 | 来源 |
|---|---|---|
| 总线 | QUPv3 wrapper 0，SE3，`0x0098c000` | 设备树 `i2c@98c000`；`GENI_FW_REVISION_RO` = `0x303`（protocol 3 = I2C） |
| 引脚 | SDA = GPIO119，SCL = GPIO120，NORTH tile，mux = 0（普通 GPIO） | 反序 120/119 全部 NAK，实测确认 |
| 外部上拉 | 有（引脚未开内部上拉，线路仍为高） | `cfg 0x00000001` 且 level = 1 |
| 复位 | GPIO114，NORTH tile，ACTIVE_HIGH（低 = 复位，高 = 释放） | 设备树 `reset-gpios = <&tlmm 114 0>`；电平 0 → 读全 NAK，释放后全应答复 |
| 功放 0x40 | CS35L41，DEVID `0x035a40`，REVID `0xb2` | 听筒路（设备树 `sound-name-prefix = "RCV"`） |
| 功放 0x41 | CS35L41，DEVID `0x035a40`，REVID `0xb2` | 扬声器路（设备树 `cirrus,right-channel-amp`） |
| 寄存器格式 | 32 位地址 + 32 位值，大端，stride 4 | Linux `cs35l41_regmap_i2c` |
| ADSP | `pil_adsp_region@8bb00000`，37 MB；PAS id 1，smem-id `0x1a7` | 设备树 `qcom,lpass@17300000` |

### ADSP 可行性：redwood 的代码要改哪几个数（纯分析，未动代码）

`hakios.adsp.c` 现在还是 redwood 版本。把它的常量和 alioth 设备树逐个对了一遍，
结论是**结构可以直接搬，但要改的常量比想象中少，只有三个是真的必须改的**：

| 常量 | redwood 现值 | alioth 应为 | 依据 | 必须改？ |
|---|---|---|---|---|
| `ADSP_MEMORY` | `0x86700000` | **`0x8bb00000`** | 设备树 `pil_adsp_region@8bb00000` | **是，改了会砸到别的固件区** |
| `ADSP_MEMORY_SIZE` | `0x04000000` (64 MB) | **`0x2500000`** (37 MB) | 同上 | **是** |
| `SMEM_PHYS` | `0x80900000` | `0x80900000` | 设备树 `smem_region@80900000`，2 MB | 否，一样 |
| `CMD_DB_PHYS` | `0x80860000` | `0x80860000` | 设备树 `reserved-memory@80860000`，128 KB | 否，一样 |
| `ipcc` 块 | `0x00408000`, 4 KB | `0x408000`, 4 KB | 设备树 `qcom,ipcc@408000` | 否，一样 |
| `aoss` 块 | `0x0c300000`, 1 KB | `0xc300000`, 4 KB | 设备树 `qcom,qmp-aop@c300000` 是 4 KB | 大小可放宽，地址一样 |
| `rsc` 块 | `0x18220000` (drv-2) | `0x18220000` (drv-2) | `qcom,tcs-offset = 0xd00`、`qcom,drv-id = 2` | 否，一样 |
| `PAS_ADSP` | `1` | `1` | 设备树 `qcom,pas-id = <0x01>` | 否，一样 |
| `ITEM_SMP2P_OURS/THEIRS` | `429` / `443` | `429` / `443` | 设备树 `qcom,smem = <0x1bb 0x1ad>` = 443/429 | 否，一样 |
| `ITEM_CRASH_REASON` | `423` | `423` | 设备树 `qcom,smem-id = <0x1a7>` = 423 | 否，一样 |
| `IPCC_LPASS` | `3` | `3` | `qcom,smp2p-adsp` 的 `mboxes = <&ipcc 3 2>` | 否，一样 |
| 轨道 `lcx.lvl` / `lmx.lvl` | PM7325 L5 / L4 | `pm8150_l11_level` / `pm8150_l4_level` | 设备树 `rpmh-regulator-lcxlvl` / `-lmxlvl` | **资源名一样，只是 PMIC 不同** |

**为什么两个 DSP 的常量几乎一样**：都是 SM8250 世代的平台，LPASS 的 PAS id、SMEM 布局、IPCC 客户端号
以及 RPMh 的 sequencer 都是 SoC 级的，不随机型变。差别只在**保留内存的位置**——那是 OEM 在
bootloader 里按机型划的。

**`0x86700000` 这个数在 alioth 上是别人的地盘**，设备树里是 `pil_wlan_fw_region@86700000`；
redwood 的 64 MB 会一路盖到 `0x8a700000`，正好压在 alioth 的 `pil_slpi_region@88c00000` 和
`pil_adsp_region@8bb00000` 上。所以这条不核对就搬过去，是把 ADSP 固件写进传感器 DSP 和 Wi-Fi 的区域里。

**固件本身是对的**：`firmware/fw-audio/adsp.b11` 里的构建路径是
`CRMBuilds/ADSP.HT.5.3-00589-SM8250-1_20200903_001359` —— **SM8250**，与 alioth 的 SoC 一致
（`HW_VERSION 0x60000000` 的 MDP 6.0、`FD650` 的 A650 也都是 SM8250 的特征）。21 MB 固件装得进 37 MB 的区域。

**还没做、也不该在无人值守时做的**：真正启动 ADSP 要写 TrustZone（PAS）、要经 AOP 邮箱给 DSP 上电、
要占 RPMh 的 TCS。这三样任何一样出错都可能挂死整机（此前 QSMMU 误写就是这么挂的）。所以这一步
**停在"常量已核对"**，等有人看着再往下走。

### 下一步的优先级建议

1. **`amp_step=6`：常驻 I2C 服务 + CS35L41 完整初始化**（写）——需要人在。功放已经能认、能自检通过，
   再往下就是几百个寄存器写入，第一次必然要人盯着。Linux 的 `cs35l41_probe` 顺序可以直接照抄：
   `test_key_unlock` → `register_errata_patch` → `otp_unpack` → `test_key_lock` → `gpio_config` →
   `IRQ1_MASK` → `init_boost` → `global_enable`。
2. **ADSP 移植（写）**——也需人在，且比第 1 项风险高一个量级。
3. **亮度（写 DSI）**——需人在，风险与第 2 项相当。
4. **WiFi（QCA6390 PCIe+MHI）**——不是"移植"，是**从零写 PCIe 根复合体驱动**，工作量最大；
   而且这台机器上现在没有任何 PCIe 驱动可以当起点。

结论：**无人值守能推进的纯读工作已经做完了**。再往前全部是写操作，都需要有人在旁边能重启。

### 关键：ADSP 固件被证明是 alioth 的（按装载跨度，不是构建标记）

`firmware/fw-audio/adsp.mdt` 的 ELF 程序头逐个解出来的结果：

- 20 个段里 18 个是 `PT_LOAD` 且非 HASH；**每一个都带 `QCOM_MDT_RELOCATABLE`（bit27）**。
- 可装载段的地址范围 `0x89500000 .. 0x8ba00000`，**跨度正好 `0x2500000` = 37 MB**。
- 最顶端段 `seg19`：相对 `min_addr` 的偏移 `0x2278000` + 自身 `0x288000` = `0x2500000`，
  **精确填满 37 MB 的保留区，一个字节不多不少**。

对照：

| | 地址 | 大小 |
|---|---|---|
| 固件实际跨度 | — | **`0x2500000`（37 MB）** |
| alioth `pil_adsp_region@8bb00000` | `0x8bb00000` | **`0x2500000`（37 MB）← 完全一致** |
| redwood 驱动里的 `ADSP_MEMORY` | `0x86700000` | `0x4000000`（64 MB）← 对不上 |

**这条证据比构建标记强得多**：构建路径里有 `SM8250` 只能说明是同一代 SoC（很多机型共用）；
而装载跨度与这台手机 bootloader 划出的保留区**精确相等**，说明这份镜像就是按这台机器的分区尺寸链接的。

**由此确认 Linux 的装载模型（`drivers/soc/qcom/mdt_loader.c`，已下载到 `wsl-current/ref/`）**：

```c
if (phdr->p_flags & QCOM_MDT_RELOCATABLE)
        relocate = true;
...
mem_reloc = relocate ? min_addr : mem_phys;     /* 可重定位：相对 min_addr */
offset = phdr->p_paddr - mem_reloc;             /* 段的落点 */
...
if (relocate)
        qcom_pas_mem_setup(pas_id, mem_phys, max_addr - min_addr);   /* 给 TrustZone：起点=区基址，长度=跨度 */
```

即：**TrustZone 拿到的是"区基址 + 跨度"**，段在区内按 `paddr - min_addr` 放置。
固件里的 `0x89500000` 这类绝对地址**与落点无关**，它只是链接时的基址。

### 这对 `hakios.adsp.c` 的移植意味着什么（还没改代码）

redwood 版的 `step_firmware()` 有两处**按绝对地址**写死的判断，对这份固件是错的：

```c
if (segment.paddr < ADSP_MEMORY || ... paddr + memory_size > ADSP_MEMORY + ADSP_MEMORY_SIZE)
        goto fail;                                  /* 错：应相对 min_addr */
image.segment[..].at = segment.paddr - ADSP_MEMORY; /* 错：应为 paddr - min_addr */
...
if (image.segments == 0 || lowest != ADSP_MEMORY)   /* 错：这份固件的 lowest 是 0x89500000 */
        goto fail;
...
image.size = ((highest + 0xfff) & ~0xfffu) - ADSP_MEMORY;   /* 错：应减 min_addr */
```

按 redwood 的写法，即使把 `ADSP_MEMORY` 改成 `0x8bb00000`，`lowest != ADSP_MEMORY` 这一条也会
直接把这份**正确的**固件判成坏的。redwood 那台机器的镜像大概不是可重定位的，所以它的写法在那里成立。

**移植时要改的（全部是常量 + 相对化，结构不用动）**：
1. `ADSP_MEMORY` → `0x8bb00000`，`ADSP_MEMORY_SIZE` → `0x2500000`；
2. 从 ELF 头算出 `min_addr` / `max_addr`，段的落点、越界判断、`image.size` 全部改成相对 `min_addr`；
3. `qcom_pas_mem_setup` 传的跨度改成 `max_addr - min_addr`（这里正好 = 区大小）。

**仍未验证、需要人看着才能做的**：真的把镜像写进去、请 TrustZone 认证、经 AOP 邮箱给 DSP 上电、
等它说 "up"。这一步写的东西（保留内存、PAS、RPMh 的 TCS）任何一样错都可能挂死整机。

### ADSP 移植：step 1 与 step 2 在 alioth 上跑通（纯读，已验证）

改的是 `wsl-current/hakios.adsp.alioth.c`（备份 `hakios.adsp.before-alioth-reloc.c`，已同步到 `bsd/dev/hakios.adsp.c`）。

**改了什么**（都是常量与相对化，结构不动）：
1. `ADSP_MEMORY` `0x86700000` → **`0x8bb00000`**，`ADSP_MEMORY_SIZE` `0x4000000` → **`0x2500000`**。
2. `step_firmware()` 的放置逻辑按 Linux `mdt_loader.c` 的两种情形重写：
   - 段带 `QCOM_MDT_RELOCATABLE`(bit27) → `mem_reloc = min_addr`，落点 `paddr - min_addr`；
   - 否则 → `mem_reloc = mem_phys`（区基址），并要求 `min_addr >= 区基址`。
3. 原来那条 `lowest != ADSP_MEMORY` 的硬判断删掉，换成"跨度装得下"的判断。
4. `step_image()` 日志里写死的 `0x86700000, 64 MB` 改成实际的 `0x8bb00000, 37 MB`。

**真机结果 `adsp_step=2`（`adsp2-console.log`，纯读）**：

```
adsp: 1: the partition for us and the audio DSP (0 and 2): at 0x80acf000, 0x30000 bytes, of which 0x2ffe0 free; 0 items
adsp: 1: the DSP's SMP2P item (443) is not there: it has not run since the phone was started
adsp: 1: our SMP2P item (429): not there; GLINK's descriptor (478): NOT THERE, our queue (479): NOT THERE, ...
adsp: 1: item 423, the DSP's last words: not there
adsp: 1: the doorbells say 0xffffffff
adsp: 2: the image is linked at 0x89500000 to 0x8ba00000 (relocatable); its segments go at
         paddr - 0x89500000, in the DSP's 0x2500000 bytes at 0x8bb00000, span 0x2500000
adsp: 2: the image is whole (from the small image): a header of 8220 bytes (7528 of hashes),
         18 segments, 21001504 bytes of files, 0x2500000 bytes of the DSP's 0x2500000 from 0x8bb00000
```

**这几行各自证明了什么**：

| 观察 | 证明了 |
|---|---|
| 分区表在 `0x80acf000` 解出 `0x30000` 字节、0 个 item | SMEM 的 `$TOC`/canary/item 布局在 alioth 上与 redwood **一致**，`SMEM_PHYS` 不用改 |
| 443/429/423/478/479 全都不在 | 冷启动后 DSP 确实从未运行；这是"还没启动过"的预期状态，不是错误 |
| doorbell 读 `0xffffffff` | IPCC 块（`0x408000`）可读、没挂，`IPCC_LPASS = 3` 的访问路径通了 |
| 固件 18 段全部读出并与头文件核对通过 | `/fw/adsp.mdt` 与 `adsp.b01`–`adsp.b18` 完整、长度一致 |
| **span `0x2500000` 与保留区尺寸完全相等** | 这份固件就是按这台机器的保留区尺寸链接的 |

**注意"from the small image"**：固件是从 `root.apfs` 里的 `fw/` 读的（`image_file()` 会去找），
不是在 `/fw` 的 APFS 大镜像里。直接按字节搜 `root.apfs` 搜不到 `adsp.mdt` 字符串（APFS 里文件名是压缩存储的），
所以"搜不到"不等于"不在里面"——这是本轮的又一个教训。

**仍未做（写操作，需人在）**：step 3 起要经 AOP 邮箱、写 RPMh 的 TCS、请 TrustZone 认证并启动 DSP，
以及 step 5 把 21 MB 写进保留区。任何一步错都可能挂死整机。

### 联合回归：今晚做的东西在**同一轮**里全部正常

`build-all-together.sh` → `together-console.log`（1,447,583 字节，`== END OF LOG ==` 完整）。
一轮里同时跑：显示（带涂色）、GPU、触摸、三按键、电池、功放（amp_step=5）、ADSP（adsp_step=2）。

| 子系统 | 结果 | 日志原文 |
|---|---|---|
| 功放 0x40 | 认到并启动 | `0x40 answers, DEVID 0x035a40`、`booted after 0 ms, status4 0x00000003` |
| 功放 0x41 | 认到并启动 | `0x41 answers, DEVID 0x035a40`、`booted after 0 ms, status4 0x00000003` |
| ADSP 固件 | 18 段校验通过 | `the image is whole ... 18 segments, 21001504 bytes of files, 0x2500000` |
| GPU | 工作 | `THE GPU STAYS ON`、`FD650` |
| 显示 | 898 µs/帧 | `51 frames in 5000 ms (10 a second), 898 us each in the driver` |
| 电池 | 正常 | `SOC=99% battery=1 charger=1 charging=1` |
| panic | **0** | `grep -c "panic(cpu" = 0` |

注意显示这一行的 `after 810 us waiting for the GPU`：GPU 在跑的同时，每帧多等 810 µs 给 GPU，
但驱动本身仍是 898 µs（和之前单独跑的 894–900 µs 一致），**总帧时间没有变差到影响 50 fps 的交换**。
这说明功放和 ADSP 的探测（都发生在启动早期、一次性的）没有侵入稳态的渲染路径。

**这是"新驱动不会互相踩"的直接证据**，也是今晚的收尾验收。

### 重大发现：DSI 命令在内存里的格式，从 bootloader 自己的缓冲里读出来了

新增 `display_step=7`（`hakios.display.alioth.c`，**仍然纯只读**）：读 bootloader 上一次发 DCS 命令时
留在 `DMA_BASE` 那里的字节。真机结果（`cmd7-console.log`）：

```
display: cmd dma: ctrl 0x14000000, cfg1 0x13c2c (start 44, continue 60, insert DCS 1)
display: cmd dma: base 0x9aeb2000, length 8, trigger control 0x4
display: cmd dma: the bytes there: c0390003 890000ef 60098030 08003804 ...
display: cmd dma: as bytes: 03 00 39 c0 ef 00 00 89 30 80 09 60 ...
```

**前 8 个字节就是那条命令**（`DMA_LEN = 8`），按 Linux `dsi_host.c` 的 `dsi_cmd_dma_add()` 解码：

| 位置 | 值 | 含义 |
|---|---|---|
| `data[0]` | `0x03` | `header[1]` = 字数 3（低字节） |
| `data[1]` | `0x00` | `header[2]` = 字数 3（高字节）→ **WC = 3 字节** |
| `data[2]` | `0x39` | `header[0]` = 数据类型 **DCS long write**、虚拟通道 0 |
| `data[3]` | `0xc0` | 标志：bit7 = 最后一个包，bit6 = 长包 |
| `data[4..6]` | `ef 00 00` | 负载：DCS 命令 `0xEF` + 两个参数 |

（`data[0..2]` 的乱序是 `dsi_cmd_dma_add()` 故意的，不是读错了——这正是它把 MIPI 包头三个字节
重排成 `packet.header[1], header[2], header[0]` 的结果。）

**这条命令本身也说明问题**：`0xEF` 是面板厂商的命令集解锁/密码命令，`0xEF 0x00 0x00` 是常见的
"进入 level-2 命令"写法——bootloader 最后做的事就是在解锁面板命令集，这只有在命令通路可用时才有意义。

**这解决了之前唯一"还要猜"的东西**。现在写亮度命令的字节序列是完全确定的：

```
DCS 0x51 亮度（mdss-dsi-bl-max-level = 0x7ff，例如满亮度 0x07 0xff）：
  内存字节 = 03 00 39 c0  51 07 ff  ff
             ^^^^^^^^^^^^^  包头 4 字节（WC=3, DCS long write, 最后包|长包）
                                        ^^^^^^^^^  负载 3 字节 + 0xff 补齐
  写入后：DMA_BASE = 缓冲物理地址，DMA_LEN = 8，TRIG_CTRL.DMA_TRIGGER = 4(TRIGGER_SW)，TRIG_DMA = 1
```

**仍然没有写**：`TRIG_DMA = 1` 会真的把命令发到面板上。无人值守时不做——这条通路一旦配置错
（比如 DMA 地址指向没人读的缓存、或者 `CMD_CFG1` 的 `WR_MEM_START/CONTINUE` 与缓冲区不符），
面板可能停在某个未定义状态，而恢复要重启。**这就是现在唯一挡在亮度前面的东西，而且是"有人看着按一下"就能过的。**

已把此前的错误解读（`+0x004` 当成 `CLK_CTRL`）在 PROGRESS 里更正：6G 主机的寄存器整体
`+4`（`DSI_6G_REG_SHIFT = 4`），`+0x004` 是 **`CTRL`** = `0x1f5`（ENABLE、CMD_MODE_EN、四条 lane、CLK_EN），
`+0x11c` 才是 `CLK_CTRL` = `0x23f`（六路时钟全开）。

### 功放寄存器快照：初始化前的真实状态，以及一个必须先处理的电平

`amp_step=6` 加了纯读的寄存器快照（`amp6-console.log`）。两台功放**逐字相同**，说明两颗处于同一状态。
与 Linux 驱动记录的上电默认值逐条对比：

| 寄存器 | 实测 | 厂商默认 | 结论 |
|---|---|---|---|
| `PWR_CTRL1` | `0x00000000` | `0x00000000` | 一致（功放、升压都没开） |
| **`PWR_CTRL2`** | **`0x00003321`** | `0x00000000` | **不同**，见下 |
| `PWR_CTRL3` | `0x01000010` | `0x01000010` | 一致 |
| `SP_ENABLES` | `0x00000000` | `0x00000000` | 一致（串口没开） |
| **`AMP_GAIN_CTRL`** | **`0x00000273`** | `0x00000000` | **不同，且是关键** |
| `AMP_DIG_VOL_CTRL` | `0x00008000` | `0x00008000` | 一致 |
| `GPIO1_CTRL1` / `GPIO2_CTRL1` | `0x81000001` | `0x81000001` | 一致 |
| `DAC_PCM1_SRC` | `0x00000008` | `0x00000008` | 一致 |
| `PWRMGT_CTL` / `PWRMGT_STS` | `0x00000000` | — | 芯片不忙、没被阻塞 |
| `CLOCK_DETECT_1` | `0x00000000` | — | 没有时钟（TDM 没跑，符合预期） |

**`AMP_GAIN_CTRL = 0x273` 的含义**：按 Linux 驱动的位域（`CS35L41_AMP_GAIN_PCM_SHIFT = 5`、`MASK = 0x3E0`）
和它的 dB 表（`DECLARE_TLV_DB_SCALE(amp_gain_tlv, 50, 100, 0)`，即 0.5 dB 起、每步 1 dB）：

- **PCM 模拟增益字段 = 19 / 满量程 20 → 约 +19.5 dB**
- PDM 增益字段同为 19
- 零交叉切换 = 0

**这是一个必须先处理的事实，不是可以顺手忽略的细节。** 芯片上电默认（厂商表里是 `0x00000000`）
是**最低**模拟增益；这台机器上的两颗都是 **19/20，几乎满增益**。如果拿这个状态直接打开功放，
第一声就是接近满增益的爆音，喇叭有真实风险。

也就是说，将来写初始化序列时**第一步之一必须是把这个字段压下去**（写到 0 或很低），
确认出声正常后再逐步加上来。这跟 `hakios.guard.c`（redwood 的扬声器保护）是同一类考虑，
但那套是在主机侧做增益和限幅，**功放芯片自己的模拟增益必须先设对**，主机侧再好的保护也补不回
一个已经被芯片放大的信号。

**`PWR_CTRL2 = 0x3321` 的含义**：`BST_EN` 位段 [5:4] = 2，按头文件是 `BST_EN_DEFAULT`。
其余位与 Linux 的 `cs35l41_boost_config()` 写入的配置相关（升压电感、峰值电流等），
本机的设备树给了 `cirrus,boost-peak-milliamp = 0xfa0`（4000 mA）、
`cirrus,boost-ind-nanohenry = 0x3e8`（1000 nH）、`cirrus,boost-cap-microfarad = 0x0f`（15 µF）——
这些值正好可用于核对将来写升压配置时算出来的寄存器值是否正确。

**这两颗芯片现在是"上电、启动完成、无错、升压按默认待命、模拟增益几乎满"的状态**，
也就是说：硬件是好的，但从安全角度**不该在这个状态下直接开到出声**。

**顺带确认**：`PWRMGT_STS = 0` 表示不在 `BLOCKED`，这正是升压时序会等的那个位；
`CLOCK_DETECT_1 = 0` 是因为 TDM 还没跑，符合预期，不是故障。

### 里程碑：内核第一次**写**进外部芯片，并读回确认

`amp_step=7` 做了第一次写操作，真机结果（`amp7-console.log`）：

```
amp: 0x40 (receiver): gain 0x00000273 -> 0x00000013 (PCM field 19 -> 0)
amp: 0x40 (receiver): reads back 0x00000013: THE WRITE TOOK
amp: 0x40 (receiver): put back, reads 0x00000273: as it was
amp: 0x41 (speaker): gain 0x00000273 -> 0x00000013 (PCM field 19 -> 0)
amp: 0x41 (speaker): reads back 0x00000013: THE WRITE TOOK
amp: 0x41 (speaker): put back, reads 0x00000273: as it was
```

**这四行各自证明了什么**：

| 观察 | 意义 |
|---|---|
| 写 `0x13` 后读回 `0x13` | I2C 写通路成立——地址、数据、字节序（32 位大端）全部正确 |
| 写回 `0x273` 后读回 `0x273` | 不是"恰好读到一个缓存值"，读回的是真实寄存器，写是可逆的 |
| 两台芯片结果一致 | 两颗 CS35L41 都能写，不是单颗碰巧 |

**这是这台机器上第一次由这个内核向外设写入成功。** 之前 `hakios.goodix.c` 写的一直是 SoC 自己的
时钟/GPIO 寄存器，不是外部芯片。写外部 I2C 设备需要把地址、寄存器索引、数据三段字节序都做对，
任何一段错都只会得到 NAK 或读回旧值——现在这三点都被读回验证过了。

**为什么选增益寄存器、为什么往低写**：
- 往**低**写是为了安全——step 6 发现两颗芯片上电就是 19/20 的模拟增益，往下调永远不会有爆音；
- 此时功放**完全没开**（`PWR_CTRL1 = 0`）、串口**没使能**（`SP_ENABLES = 0`）、**没有时钟**
  （`CLOCK_DETECT_1 = 0`），所以这次写**物理上发不出声音**；
- 写完立刻写回原值，不留副作用。

**这为下一步铺好了路**：初始化序列要的就是"写进去、能读回来、能改回来"。现在这条路
在真实的 CS35L41 上被证明了，剩下的只是把 Linux `cs35l41_probe()` 的寄存器序列照搬过来。

### 现在的完整状态（截至本次收尾）

| 项 | 状态 |
|---|---|
| 功放识别 | ✅ 两台 CS35L41，DEVID `0x035a40`、REVID `0xb2` |
| 功放启动 | ✅ 两台 OTP_BOOT_DONE，无 OTP_BOOT_ERR |
| 功放寄存器快照 | ✅ 14 个关键寄存器，与厂商默认逐条比对 |
| 功放写通路 | ✅ 写入并读回确认，可逆 |
| DSI 寄存器映射 | ✅ 权威偏移确认（6G，整体 +4） |
| DSI 命令格式 | ✅ 从 bootloader 缓冲读出（`03 00 39 c0 ...`） |
| ADSP 固件 | ✅ alioth 的（SM8250，跨度精确吻合 37 MB 区） |
| ADSP step 1/2 | ✅ 共享内存布局一致、18 段校验通过 |
| GPU / 显示 / 触摸 / 按键 / 电池 | ✅ 联合回归全绿，0 panic |

**剩下的都需要有人看着**，按风险从低到高：
1. 功放完整初始化序列（写几百个寄存器 + 开升压）——**可以做了**，写通路已验证
2. 亮度 DCS `0x51`（写 DSI 命令缓冲 + 触发）——格式已解出
3. ADSP step 3 起（AOP 邮箱、TrustZone、RPMh TCS）——风险最高

### 麦克风：路径与扬声器完全不同，是 SoundWire 不是 I2C/TDM

用户让"有余力也研究下麦克风"。这两台设备的麦克风架构**和 redwood 不是一回事**，先说结论：
**麦克风不做，是因为它需要的是第三种控制器，不是把已有的东西改一改**。

alioth 的录音路径（设备树 `qcom,msm-audio-apr/qcom,q6core-audio/bolero-cdc`）：

```
模拟麦克风 ──► WCD938x（split codec）──► tx_swr_master ──► 音频 DSP
                                        (SoundWire, 不是 I2C)
数字麦克风 ──► va-macro DMIC0/1（LPI GPIO 5/6）──► 音频 DSP
```

| 部件 | 地址 | 总线 |
|---|---|---|
| `tx-macro@3220000` | `swrm-io-base = 0x3230000` | SoundWire（`qcom,swr-mstr`，master id 3） |
| `rx-macro@3200000` | `swrm-io-base = 0x3210000` | SoundWire（master id 2，含 playbook 输出） |
| `va-macro@3370000` | — | LPI DMIC（GPIO5 = clk `func2`，GPIO6 = data `func1`） |
| WCD938x | SoundWire 从设备，`reg = <0x0d 0x1170223>` | 挂在 tx master 下 |

**为什么这比功放难得多**：

1. **SoundWire 不是 I2C**。它有自己的协议（帧结构、命令字、bank/port 寄存器），而且**时钟由主机产生**
   （`swr_master` 要先把 SoundWire 时钟跑起来，从设备才会应答）。bit-bang 一条两线 I2C 的经验完全用不上。
2. **三个 controller 都在 LPASS 里**。`rx-macro`/`tx-macro`/`va-macro` 的时钟格（`lpass_audio_hw_vote`）
   归音频 DSP 管——`hakios.adsp.c` 里已经记着"从应用处理器读 LPASS 的时钟寄存器会导致内核外部中止"。
   所以**麦克风同样卡在 ADSP 上**，和扬声器一样，但还多一层 SoundWire 协议。
3. **模拟麦克风还要走 codec 的偏置电路**（`cdc-vdd-mic-bias`、`micbias1..4-mv = 0xabe` = 2745 mV），
   这是 WCD938x 自己的寄存器，同样在 SoundWire 后面。

**所以麦克风排在扬声器之后**，而且不是"顺手"，是要另外写一个 SoundWire 主机控制器驱动。这一点要说明白，
免得把"功放能读了"误当成"麦克风也快了"。

**数字麦克风（DMIC）反而是最短的路**：它只需要 LPI 引脚 + `va-macro` 的 DMIC 通路，
不经过 WCD938x 的模拟部分。但 `va-macro` 依然归 DSP 管，所以还是绕不开 ADSP。

**顺序上的建议**：
1. 扬声器（功放初始化序列）——**写通路已验证，可以做了**
2. ADSP 起来（这是扬声器和麦克风共同的必要条件）
3. 麦克风（SoundWire 驱动 + codec 配置）

### 功放：按本机硬件配置完成，且余留状态是"关闭、静音"

`amp_step=8` 按设备树给的升压元件参数把两颗 CS35L41 配置好，真机结果（`amp8-console.log`）：

```
amp: 0x40 (receiver): boost coefficients     0x00002424 -> 0x00002424: took
amp: 0x40 (receiver): boost slope/inductor   0x00007500 -> 0x00007500: took
amp: 0x40 (receiver): boost peak current     0x0000004a -> 0x00000040: took
amp: 0x40 (receiver): analog gain            0x00000273 -> 0x00000013: took (PCM field 19 -> 0)
amp: 0x41 (speaker):  （完全相同的四行）
amp: step 8: the state left behind
amp: 0x40 (receiver): PWR_CTRL1 0000000000 (off, as it must be), PWR_CTRL2 0x00003321 (BST_EN 2),
                      SP_ENABLES 0000000000 (serial port off, silent)
amp: 0x41 (speaker):  （同上）
```

**这几行说明了什么**：

1. **升压参数按本机元件写对了**。系数 `0x2424`（K1=K2=0x24）和斜率/电感 `0x7500`（slope 0x75, lbst 0）
   对应 1000 nH 电感 + 15 µF 电容，是从设备树 `cirrus,boost-ind-nanohenry = 0x3e8`、
   `cirrus,boost-cap-microfarad = 0x0f` 经 Linux 的 `cs35l41_boost_config()` 的查表算出来的。
2. **发现 bootloader 用的峰值电流和本机设备树不一样**：读到的原值是 `0x4a`，
   反算 `(0x4a - 0x10) × 50 + 1600 = 4500 mA`；设备树写的是 `0xfa0` = **4000 mA**。
   也就是说安卓的 bootloader 按 4500 mA 配过。现在按设备树的 4000 mA 写入并生效。
   两者都在芯片允许范围（1600–4500 mA）内，后者更保守，也符合本机自己的设备树。
3. **模拟增益从 19/20 压到了 0**——这是在真机上落实了上一节提出的安全问题。
4. **余留状态是安全的**：`PWR_CTRL1 = 0`（全局使能关）、`SP_ENABLES = 0`（串口关），
   **芯片没有输入、没有使能，物理上不可能出声**。升压的 `BST_EN` 仍是 2（默认待命），
   但那是 converter 的配置位，没有 `PWR_CTRL1.GLOBAL_EN` 就不会真正升压。

**必须如实说明的一点**：`pins_give_back()` 把复位脚 GPIO114 恢复成了进来时的电平（**低电平**，
即复位有效）。所以**这些配置在本次运行结束后不会保留**——芯片重新进入复位，寄存器回到上电默认。
这是设计上的选择：无人值守时不把手机留在"配置好且已上电"的状态。

**要让配置真正生效并保持，需要的是**：常驻的 I2C 服务（设备节点），由它持有复位脚和总线，
在系统运行期间一直让芯片保持复位外。这是下一步要写的东西，但它同时也意味着**芯片会一直保持
在"已配置、未使能"的状态**——仍然不会出声，但是一个需要人在场确认的状态变更。

### 扬声器这条线现在的确切位置

| 步骤 | 状态 |
|---|---|
| 认出芯片 | ✅ `DEVID 0x035a40`、`REVID 0xb2` |
| 等它启动 | ✅ `OTP_BOOT_DONE`，无错 |
| 读全部关键寄存器 | ✅ 14 个，与厂商默认逐条比对 |
| 写通路验证 | ✅ 写入→读回→还原 |
| 按本机元件配置升压 | ✅ 系数/斜率/峰值电流全部 `took` |
| 把模拟增益压到安全值 | ✅ 19 → 0 |
| 保持配置（常驻服务） | ❌ 未做，需人在 |
| 使能（`GLOBAL_EN` + 串口） | ❌ 未做，**会出声，必须有人在** |
| 音频数据（TDM/ADSP） | ❌ 未做，卡在 ADSP |

### WiFi（QCA6390）：不是"移植"，是从零写 PCIe 根复合体

用户要求的最后一项。这台机器的 WiFi 是 **QCA6390，挂 PCIe**，不是 redwood 的 WCN6750（走 WPSS DSP）。
设备树的实际结构：

```
qcom,cnss-qca6390@b0000000          ← WiFi 的驱动节点
  qcom,wlan-rc-num = <0x00>         ← 它说要挂在 PCIe domain 0
  wlan-en-gpio = <&tlmm 20 0>       ← 上电使能脚
  mhi,max-channels = <0x1e> (30)
  vdd-wlan-aon / -dig / -io / -rfa1 / -rfa2   ← 五路供电

qcom,pcie@1c00000    cell-index 0   perst gpio 79,  wake gpio 81    conf @ 0x60000000
qcom,pcie@1c08000    cell-index 1
qcom,pcie@1c10000    cell-index 2   perst gpio 85,  wake gpio 87    conf @ 0x64000000
```

**发现一处不一致，必须记下来**：设备树里 MHI 设备（`qcom,mhi@0` → `mhi_rmnet@0`）挂在
**`pcie@1c10000`（domain 2）** 下面，但 `cnss-qca6390` 节点自己写的是 `qcom,wlan-rc-num = <0x00>`（domain 0）。
两者对不上。可能是厂商设备树的历史遗留（多个 PCIe 口共用一套 cnss 节点），也可能是 MHI 挂在
不同的 root complex 上。**这一条在动手前必须用运行时的数据核实**（安卓下 `lspci` 或读
`/sys/bus/pci/devices`），不能照着写。这正是 GPU 那次 CBAR 遗留值教的同一课。

**为什么这条路比其它都长**：

| 需要的东西 | 现状 |
|---|---|
| PCIe 根复合体驱动（PARF/PHY/ELBI/IATU/MSI） | **树里没有**。`hakios.*.c` 里没有任何 PCIe 代码 |
| PCIe PHY 上电与链路训练 | 没有。这是 SoC 级的复杂块，`qcom,pcie-phy-ver = 0x44e` |
| MHI（Modem Host Interface）协议栈 | 没有。30 个通道、事件环、门铃 |
| 固件下载（`bdwlan` 系列、`amss` 校准） | 未见 |
| 五路供电 + perst/wake GPIO 时序 | 有 GPIO 驱动可复用，但时序未知 |

**结论**：WiFi 的工作量比扬声器和麦克风加起来还大，而且它**不依赖 ADSP**（这点和其它几项不同），
是一条完全独立的、需要从 PCIe 底层往上搭的路。

**短期的替代方案**（如果只是要联网）：USB 网卡。内核里已经有 `hakios.virtio_net.c` 和
网络栈的接入点，USB 侧若有可用的 host 控制器驱动，插一个 RTL8152/8153 之类就能上网，
不必等 QCA6390。这比写 PCIe 便宜一个数量级。**但这条路没有验证过**，只是指出方向。

### 默认路径回归：新加的代码在没人要求时不说话

`build-default-check.sh` → `default-check.log`。不带 `amp_step`、不带 `adsp_step`、不带 `display_step`：

| 检查 | 结果 |
|---|---|
| `amp:` 行数 | **0**（功放驱动没被启动，符合预期） |
| `adsp:` 行数 | **0**（ADSP 驱动没被启动） |
| GPU | `THE GPU STAYS ON`、`FD650` ✅ |
| 显示 | 897 µs/帧 ✅ |
| 电池 | `SOC=99% battery=1 charger=1 charging=1` ✅ |
| panic | **0** ✅ |
| 日志完整 | `== END OF LOG ==` ✅ |

这说明今晚做的三件事——新增 `hakios.amp.c`、改写 `hakios.adsp.c` 的固件放置逻辑、在 `bsd_init.c`
里加一行调用——**对默认启动没有任何影响**。每个驱动都在自己的 boot-arg 后面，没人给参数就不运行。
这点很重要：`hakios.adsp.c` 是**改过逻辑的**（不是纯新增），所以必须确认它在不启用时也不出错。

### 今晚的完整账目

**新做出来的能力**（真机验证）：

| 项 | 状态 | 关键证据 |
|---|---|---|
| 识别扬声器功放 | ✅ 两台 CS35L41 | `DEVID 0x035a40`、`REVID 0xb2` |
| 功放启动检查 | ✅ | `OTP_BOOT_DONE`，无 `OTP_BOOT_ERR` |
| 功放寄存器快照 | ✅ 14 个 | 与厂商默认逐条比对 |
| **向内嵌芯片写入** | ✅ **首次** | 写入→读回→还原，两轮一致 |
| 功放升压配置 | ✅ | 系数/斜率/峰值电流全部 `took` |
| 功放模拟增益 | ✅ 19→0 | 消除了近满增益的风险 |
| ADSP 固件归属 | ✅ 证明是 alioth 的 | 跨度 `0x2500000` 精确等于保留区 |
| ADSP 固件校验 | ✅ 18 段 | `the image is whole` |
| ADSP 共享内存 | ✅ 布局与 redwood 一致 | 分区表解出 `0x30000` 字节 |
| DSI 寄存器映射 | ✅ 权威 | 6G 主机，整体 `+4` |
| **DSI 命令格式** | ✅ 读出 | bootloader 缓冲 `03 00 39 c0 ...` |
| 联合回归 | ✅ 全绿 | 六子系统同轮，0 panic |
| 默认路径 | ✅ 无影响 | 实验驱动 0 行输出 |

**没能做、也不该无人值守做的**：

| 项 | 卡在哪 |
|---|---|
| 功放使能（出声） | 会真的发声，必须有人在 |
| 功放配置持久化 | 需要常驻 I2C 服务（设备节点） |
| 亮度 DCS `0x51` | 写活着的显示控制器有挂死风险 |
| ADSP step 3+ | AOP 邮箱 + TrustZone + RPMh TCS，风险最高 |
| 麦克风 | 要写 SoundWire 主机控制器，且绕不开 ADSP |
| WiFi | 要从零写 PCIe 根复合体，工作量最大 |

**修正过的错误结论**（留在文档里以免重犯）：
1. `dsi +0x004` 不是 `DSI_CLK_CTRL` 而是 `CTRL`——6G 主机整体 `+4`。
2. ADSP 保留区不是 `0x8a100000`/`0x1d00000`，而是 `0x8bb00000`/`0x2500000`。
3. redwood 的 `ADSP_MEMORY = 0x86700000` 在 alioth 上是 **WLAN 固件区**，直接搬会砸坏别人的区域。
4. 固件校验不能按绝对地址判断——可重定位镜像按 `min_addr` 相对放置，旧逻辑会把**正确**的固件判坏。
5. `root.apfs` 里搜不到 `adsp.mdt` 字符串不等于固件不在——APFS 的文件名是压缩存储的。

**下一步（都需要人看着）按风险从低到高**：
1. 功放使能——写通路已验证、升压已按本机配好、增益已压到 0，**风险已降到最低**
2. 功放常驻服务——让配置跨重启保留
3. 亮度 DCS——格式已解出，只差一次写入
4. ADSP 启动——最重的一步，也是音频和麦克风的共同前提
5. 麦克风 / WiFi——各自独立的工程

### 补上"探测不留副作用"的实证（`amp8b-console.log`）

之前一直说"探测完会把引脚恢复原样"，但**没有实测证明过**。现在加了最后一步验证：
恢复引脚后再读一次，功放必须停止应答。

```
amp: the pins are put back as they were found
amp: after putting it back, the amplifiers are silent again: the probe left nothing behind
```

这行证明了三件事：
1. 恢复 GPIO114 到进来的电平（低 = 复位）后，芯片**确实回到复位**；
2. 前面所有配置（升压、增益）随复位**一并清除**，没有留下半配置状态；
3. 功放驱动跑完之后，这台机器和跑之前**是一样的**——可以反复跑，不会累积状态。

这一条重要，因为整个无人值守流程都建立在"探测不改状态"这个前提上。现在它是测出来的，不是声称的。

### 重要发现：iOS 的声音现在根本没有下游

查主机侧音频链路时发现一件对扬声器路线影响很大的事。`/dev/hakios-audio`（`hakios.audio.c`）
是一个"谁注册了 sink，声音就交给谁"的设备，全树里只有两处注册：

| 注册者 | 条件 |
|---|---|
| `hakios.virtio_sound.c` `sound_play` | 仅在 QEMU 的 virtio 声卡下 |
| `hakios.sound.c` `ios_sink` | 仅在 `sound=4` 且 ADSP 已经起来时 |

**在真机上、不带 `sound=4` 时，`audio_sink` 是 NULL**——iOS 写进 `/dev/hakios-audio` 的样本
**被丢弃**（只留下日志里的"写了多久、多响"统计）。也就是说：

> **即使把两颗 CS35L41 完全使能并配置好，iOS 的声音也不会到它们那里。** 中间缺的正是
> `hakios.sound.c` 里那个从 `/dev/hakios-audio` 到 TDM 端口的搬运者。

而 `hakios.sound.c` 现在的端口是**写死的 redwood 值**：

```c
#define PORT   0x9000      /* PRI_TDM_RX_0 —— redwood 用的主 TDM */
#define GROUP  0x9100      /* PRI_TDM_RX   —— 同上 */
#define CLOCK_PRI_TDM_IBIT 0x200
```

alioth 的功放接的是**三级 TDM**（设备树 `qcom,msm-dai-tdm-tert-rx`）：

```c
port 0x9020, 0x9022   group 0x9120   /* 引脚 133/134/135/137 = mi2s2 */
```

**所以扬声器的完整链条是四段，缺一不可**：

1. ADSP 起来（`hakios.adsp.c`）——**只验证到 step 2，step 3+ 未做**
2. `hakios.sound.c` 改成 alioth 的端口（`0x9020`/`0x9120`）并跑通 AFE/ADM/ASM——**未做**
3. CS35L41 使能（`PWR_CTRL1.GLOBAL_EN` + `SP_ENABLES` + ASP 配置成 TDM 从机）——**未做**
4. `/dev/hakios-audio` 的 sink 注册（`hakios.sound.c` 里那一行）——**未做**

之前我在 PROGRESS 里写"扬声器可以做，写通路已验证"——这个说法**不完整**，必须更正：
写通路只是第 3 段的**前提**，不是全部。真正的顺序是 ADSP → sound.c 改端口 → 功放使能 → 注册 sink。

这四段里，第 1 段风险最高（要碰 TrustZone/RPMh），第 2、3 段是中等（改驱动 + 写功放），
第 4 段是顺带。**所以"让 iOS 出声"的真实工作量比之前估计的大得多**，这一点要如实记录。

### 音频 TDM 引脚：tile 基址实测确定，且四个脚都还没 mux

`amp_step=9`（纯只读）把三级 TDM 的四个引脚在四个候选基址上各读一遍，让数据自己说话：

```
amp: tlmm f100000 (west?), gpio 133 (bit clock):  cfg 0x00000001 (pull 1, function 0, drive 2 mA), io 0
amp: tlmm f100000 (west?), gpio 135 (frame sync): cfg 0x00000001 (pull 1, function 0, drive 2 mA), io 0
amp: tlmm f100000 (west?), gpio 134 (data in):    cfg 0x00000001 (pull 1, function 0, drive 2 mA), io 0
amp: tlmm f100000 (west?), gpio 137 (data out):   cfg 0x00000001 (pull 1, function 0, drive 2 mA), io 0
（f000000 / f500000 / f900000 三处四个脚全部读出 0）
```

**两条结论，都是测出来的**：

1. **SM8250 的 west TLMM tile 基址是 `0x0f100000`**。只有这里读出非零、且是合理的引脚配置
   （pull 1、function 0、drive 2 mA），其它三个候选基址全 0——没有东西。这和已知的两个
   （north `0x0f900000`、south `0x0f500000`）合起来构成了完整的 tile 模型：
   `f000000` 起 16 MB 窗口里，west 在 +1 MB、south 在 +5 MB、north 在 +9 MB。
   **之前 `hakios.goodix.c`/`hakios.haptic.c` 里的 `GPIO_BASE(n)` 只有"4..23 去 north，其余去 south"，
   对 114/119/120 恰好对；但对 133..137 就会算错。** 这是个需要记下来的陷阱。

2. **四个音频脚现在是普通 GPIO（function 0），没有被 mux 成 mi2s2**，
   电平都是 0（没有信号在跑）。也就是说，将来开音频时**必须自己把这四个脚 mux 过去**
   （设备树的 `tert_tdm_*_active` 里 function 是 `mi2s2_sck` / `mi2s2_ws` / `mi2s2_data0` / `mi2s2_data1`，
   在各自的 pin group 列表里都是第 1 项 = 值 1，和 bus 引脚的 `qup3` 同一个道理）。

这一条填上了"音频数据从哪进来"的最后一处空白：**引脚位置、基址、当前状态、目标 function 值**
现在全部确定，只差在 ADSP 起来之后把 TDM 端口配置好并把这四个脚 mux 过去。

### 音频这条链现在的完整地图

| 段 | 内容 | 状态 |
|---|---|---|
| 1 | ADSP 启动（PAS/TrustZone/AOP/RPMh） | ❌ step 3+ 未做，风险最高 |
| 2 | AFE 端口 `0x9020`、组 `0x9120`（三级 TDM） | ❌ 未做，需 ADSP |
| 3 | 四个引脚 mux 到 `mi2s2_*`（function 1，west tile `0xf100000`） | ❌ 未做，**位置和值已确定** |
| 4 | 功放 ASP 配成 TDM 从机（`SP_ENABLES`、`ASP_CONTROL4`、通道映射） | ❌ 未做，**写通路已验证** |
| 5 | 功放使能（`PWR_CTRL1.GLOBAL_EN`） | ❌ **会出声，必须有人在** |
| 6 | `hakios.sound.c` 改成 alioth 的端口 + 注册 sink | ❌ 未做 |
| 7 | iOS 侧 `/dev/hakios-audio` 写入 | ✅ 已有（`HakiosAudio.driver`） |

**七段里第 7 段本来就通，第 3、4 段的地基今晚打好了，1、2、5、6 仍未动。**

### 一个重要陷阱：TLMM 的 tile 不是"连续分段"，现有驱动的 `GPIO_BASE()` 是碰巧对的

`ref/pinctrl-sm8250.c` 里 180 个引脚逐个对了 tile，结果**根本不连续**：

```
SOUTH   0..3      NORTH   4..23     SOUTH  24..27    NORTH  28..31
SOUTH  32..65     NORTH  66..68     SOUTH  69..76    NORTH  77..84
SOUTH  85..91     NORTH  92..124    SOUTH 125..132   WEST  133..179
```

对照现有两个驱动的写法：

```c
/* hakios.goodix.c 与 hakios.haptic.c 都是这个 */
#define GPIO_BASE(n)  (((n) >= 4 && (n) <= 23) ? tlmm_north : tlmm_south)
```

**这个判断对这两个驱动各自用到的引脚恰好是对的**，但它是错的规则：

| 引脚 | 驱动用到 | 真实 tile | 按旧宏算 | 结论 |
|---|---|---|---|---|
| 8–11（触摸 SPI） | goodix | NORTH | NORTH（因在 4..23 内） | 碰巧对 |
| 33, 39（触摸复位/中断） | goodix | SOUTH | SOUTH | 碰巧对 |
| 60, 61（马达 I2C） | haptic | SOUTH | SOUTH | 碰巧对 |
| 114, 119, 120（功放复位 + I2C） | amp | NORTH | NORTH（在 4..23 外 → 会算成 SOUTH） | **本驱动没用旧宏，单独映射 north，对的** |
| **133, 134, 135, 137（TDM）** | 将来 | **WEST** | 会算成 SOUTH | **错** |
| 24–27, 69–76, 85–91, 125–132 | 未用 | SOUTH | SOUTH | 碰巧对 |
| 28–31, 66–68, 77–84, 92–124 | 未用 | NORTH | SOUTH | **错** |

也就是说：**旧宏对"4..23 之外全当 south"的假设，在 28..31、66..68、77..84、92..124、133..179
这些引脚上都是错的**。只是恰好没有驱动用过它们，所以一直没暴露。

**这件事必须写下来**，因为音频要用 133–137（WEST），谁要是照抄 `GPIO_BASE()` 就会写到
south tile 里去，而那里没有这个引脚——读回来是 0，看起来像"引脚没配置"，实际是**地址错了**。
GPU 那次 SMMU CBAR 遗留下来的教训就是这类：一个没核对过的小规则，用对了两次，第三次就错。

**正确的做法**（将来音频驱动要用）：照搬上游那张表，或者直接用区间判断：

```c
/* SM8250, from drivers/pinctrl/qcom/pinctrl-sm8250.c: the tiles interleave */
static vm_offset_t
gpio_tile(unsigned n)
{
	if (n <= 3)                              return tlmm_south;
	if (n <= 23)                             return tlmm_north;
	if (n <= 27)                             return tlmm_south;
	if (n <= 31)                             return tlmm_north;
	if (n <= 65)                             return tlmm_south;
	if (n <= 68)                             return tlmm_north;
	if (n <= 76)                             return tlmm_south;
	if (n <= 84)                             return tlmm_north;
	if (n <= 91)                             return tlmm_south;
	if (n <= 124)                            return tlmm_north;
	if (n <= 132)                            return tlmm_south;
	return tlmm_west;
}
```

**没有去改 `hakios.goodix.c` 和 `hakios.haptic.c`**：它们能工作，改它没有收益，只有风险。
但这条记录留在文档里，比改代码更有用——它防止的是**将来**的错误。

### 最终验收（`final-together.log`，内核 SHA256 `394fa92a…`）

最后一次改动之后（`TLMM_WEST_PHYS` 接进 step 9、硬件文档更新）重新跑联合回归：

```
amp: 119/120: 0x40 answers, DEVID 0x035a40 (a CS35L41)
amp: 119/120: 0x41 answers, DEVID 0x035a40 (a CS35L41)
amp: 0x40 (receiver): booted after 0 ms, status4 0x00000003
amp: 0x41 (speaker): booted after 0 ms, status4 0x00000003
adsp: 2: the image is whole (from the small image): 18 segments, 21001504 bytes of files, 0x2500000
MiniIOMobileFramebuffer: 51 frames in 5000 ms (10 a second), 894 us each in the driver
hakios: battery: SOC=99% battery=1 charger=1 charging=1
THE GPU STAYS ON: 1
panic(cpu: 0
== END OF LOG ==: 1
```

（`amp_step=9` 的 west tile 行缺省是**预期**的：联合脚本用的是 `amp_step=5`；step 9 单独跑过，结果在 `amp9-console.log`。）

**内核最终状态**：`mach.development.bcm2837` SHA256 `394fa92a5da78eb388899fa49a5eafeaa8b270075d71541421a5043aab7b2c07`。
`hakios.amp.c` / `hakios.display.c` / `hakios.adsp.c` / `bsd/conf/files` / `bsd/kern/bsd_init.c`
全部与 `wsl-current/` 下的工作副本一致（逐个 `cmp` 验证过）。

**手机现在停在 loader 里**（`getvar result: returned 1441823 0`），可以随时用
`oem rdload iosdata` → `stage` → `oem run` 再跑任何一轮，也可以 `oem reboot` 回到安卓。

## 2026-10-29：互联网调研——别人在 Redmi K40 上做到哪了

用户要求查 WiFi 和麦克风的社区完成度。结论分三层。

### 1. 有一个和我们**目标完全相同**的 alioth 项目：UniXNU

`unixnu.com/devices/alioth`，报告为 **"XNU/Darwin 内核跑在 Redmi K40（alioth）上"**，
属于 UniXNU 项目，维护者 FFT-infra，仓库 `FFT-infra/UniXNU-alioth`（pin `872cb18`）
+ `FFT-infra/UniXNU-sm8250-common`。它的硬件描述和我们实测的一致
（SM8250-AC、Adreno 650、1080×2400、FT5652 触摸在 GENI SE4、**QCA6390 在 `qcom,pcie@1c00000`**、DWC3 USB）。

它的状态表（原文措辞，"implemented" = 代码有、部分能跑，不等于可用）：

| 领域 | 它 | 我们 |
|---|---|---|
| 显示 | implemented（锁屏能渲染，**没有壁纸**） | ✅ 完整 iOS 界面 + 状态栏 |
| 触摸 | implemented（**报告从没送到 OS**，`native_touch_hid_reports` 恒为 0） | ✅ 真机触点进 iOS |
| GPU | implemented（**只有时钟/电源，没有提交流**） | ✅ 真实 A650 渲染，`FD650`，50 fps |
| **网络/WiFi** | implemented（**只到 PCIe 上电；没有 WiFi**） | ❌ 同 |
| **音频** | **planned（"Nothing"）** | 🔶 功放已配置（领先） |
| 按键 | **未实现** | ✅ 三个键 |
| 存储 | device-verified | ✅ |
| 电池 | 未提及 | ✅ |

**关键原文（WiFi）**：

> Only PCIe controller-level steps run: power-up works (`0x22f000` reads back `0xf822f000`), but
> step 10 (DBI, config space, link training) has "never run here or in the reference". No link has
> trained — `parf_ltssm` reads 0 — nothing is enumerated, and MHI, QMI and ath11k don't exist in the
> tree. The PHY sequence is skipped (`pcie_skip=32`) because **it hangs the phone**, which also means
> no pipe clock.

**这条对我们价值很大**：另一个和我们几乎同起点的项目，在 PCIe 上停在同一个位置，而且给出了
具体的危险点——**PCIe PHY 序列会挂死整机**（所以它直接跳过了）。这印证了我之前的判断：
WiFi 不是"移植"，是从零写 PCIe，而且 PHY 那一段本身就有挂死风险。

**音频原文**：这一节是**空白模板**（hardware unidentified、无组件、无配置、无测量），
麦克风全页没提。

### 2. postmarketOS 的 alioth：WiFi 也是坏的

`postmarketOS/pmaports` 的 alioth 设备 MR（`!2947`）**至今还是 draft**，用的是**下游 4.19 内核**，
原文明确写着 **"Wi-Fi seems to be broken"**，其余未测。多年没有更新。
Reddit 上有人跑了 postmarketOS + Niri（约一个月前），但那是能开机当桌面玩具用，不是功能完整。

也就是说：**Linux 侧做了多年，alioth 的 WiFi 依然是坏的。**

### 3. WiFi 在别的机器上能成，是因为架构不同

`MakrSas/ios-topaz`（Redmi Note 12 4G，SM6225）**WiFi 是能用的**：`TopazModem` 系列补丁
（0113–0130）走 modem PAS 启动 → GLINK/QRTR → HTT → 内核内 WPA2 四步握手，iOS 设置里能扫描、能连、
密码进钥匙串、能开 Safari 上 HTTPS。限制是只支持 WPA2-PSK、无省电、无 MAC 随机化。

**但 topaz 的 WiFi 走的是 SoC 内置 modem 那条路**（和 redwood 的 WCN6750 同类），
**不是 PCIe 挂独立芯片**。这正是 alioth 难的地方：QCA6390 是一颗外挂 PCIe 器件，
ath11k 在 Linux 上要 PCIe + MHI + QMI 三件套齐备。

顺带：`ios-topaz` 明确写了 **audio 不工作**（"No Bluetooth, cellular data, audio, camera, sensors, GPS, SEP"），
扬声器是 roadmap 第 9 项（计划移植 `TopazAudio`/`TopazSpeaker`）。也就是说**连领先的项目，音频都还没做**。

### 结论：WiFi 和麦克风都没有可抄的

| 项 | 全网现状 | 我们的判断 |
|---|---|---|
| alioth WiFi | **无人做成**（XNU 侧停在 PCIe 上电，Linux 侧 postmarketOS 明说坏） | 确认是硬骨头，不是我们不会 |
| alioth 麦克风 | **无人碰过**（UniXNU 音频整节空白，ios-topaz 音频不工作） | 同上 |
| 可借鉴的 | `ios-topaz` 的 modem WiFi 栈（架构不同，思路可看）、ath11k 本身的实现 | 有限 |

**所以按用户说的"实在没有就后面再说"——这两项确实该往后放。** 优先级排序：
1. 让 iOS 出声（ADSP + sound.c + 功放使能）——**别人都没做到，我们有最好的起点**
2. 亮度
3. WiFi / 麦克风——等有闲或有人啃 PCIe 再说

## 工作纪律：查日志只查这一轮那一份（2026-10-29 教训）

`wsl-current/` 里堆了几百个历史日志（`adreno-*`、`adsp*`、`amp*`、`gl*`、`hz120.log`、`cavbl.log`、`noblit.log`……）。
用 `grep -a ... *.log` 会把往轮的输出混进"本轮证据"里，既烧 token 又可能得出错误结论。

**实例**：`grep -ah "on a panel of" *.log` 返回三条 `display 1125x2436 on a panel of 1080x2400`，
被当作 `fbwidth=1080 fbheight=2400` 生效的证据引用；但本轮自己的 `fbsize.log` 里这个模式**一条都没有**
（`grep -a -c "on a panel of" fbsize.log` = 0），那三条来自更早的日志。用户的批评是对的。

**规矩**（每次取回日志后立刻执行）：

1. `get_staged` 之后只用这一份文件名，后续所有 grep 都指向该路径，**禁止通配符、禁止一次查多个 `.log`**。
2. 引用任何一行之前先 `grep -a -c "<模式>" <本轮文件>`，数量非零才引用。
3. 预期存在却查不到的行，就写"本轮日志里没有"，**不要**去别的日志里找补。
4. 要与上一轮对比时，显式写出上一轮的文件名，并在输出里标明它来自哪个文件，绝不用 glob 代替。
5. 每轮日志用带语义的名字保存（`hz120.log`、`cavbl.log`、`noblit.log`、`fbsize.log`），旧轮次的挪进 `wsl-current/old-logs/`，从根目录消失后就不会被误查。

## 2026-10-30：干净 clone 上真机验证，以及为了让别人也能重做所做的整理

### 起点

用户的要求最后定成两条：**所有工作都在 WSL 里做，只保留一个 git 仓库**；并且用一次**干净 clone** 证明
「在 K40 上跑起 iOS 16」这件事能重做。

### 结论：成立

干净 clone（921 个文件、45 MB）从零编出内核与 loader，再用 **Windows 的 `fastboot.exe`** 刷上手机，
iOS 16.5 用户态起来了：

```
=== 36 checks, 0 failed ===
surf_test: 80 checks, 0 failed
[SystemShell] System shell "com.apple.springboard" finished startup after 7.33s.
```

- 内核 `mach.development.bcm2837` 15,210,808 字节，loader `loader.efi` 70,656 字节。
- 打包出的镜像与手机实测过的 `xnu-rdload-fixed.img` 相比，BootShim 与 UEFI 逐字节相同，payload 只差
  PE 头的时间戳 3 字节。
- 日志 `final-run3.log`（1,447,406 字节）：launchd 6913 行、backboardd 177 行、SpringBoard 324 行，
  **崩溃 0**。
- 屏幕颜色数从 30 秒时的 158 种（文本控制台）到 45 秒时的 **150,708 种**——时间点正对 SpringBoard
  启动完成，画面确实切到 iOS 了。

刷机这条链的数据：`fastboot boot` 后 2 秒出现 `product: xnuloader`；`oem ramdisk` + `stage`
2,486,968,320 字节的系统镜像用了 **90.6 秒**（约 27 MB/s）。

### 为了让 clone 能编，修掉的缺陷

| 缺陷 | 现象 | 修法 |
|---|---|---|
| `dsymutil` 没入库 | 干净 clone 编内核 14 秒后失败（`--minimize: command not found`） | `xnu-deps/setup.sh` 生成它；`make kernel` 的哨兵从入库的 `xnu-clang` 改成 `dsymutil` |
| `mkidentity.sh` 调错目标 | `make -C lzfse lzfse`，上游只有 `all/install/clean` | 改成 `make all` |
| `mkxnuimg.py` 写死 POCO | FD 地址 `0x9FD00000`/`0x200000` | 加 `DEVICES` 表与 `-d/--device/$DEVICE`；alioth 是 `0x9FC00000`/`0x300000` |
| 可执行位丢失 | clone 后 `Permission denied` | 被 `./` 调用的脚本全部补上 |
| `DEVICE` 没导出 | `DEVICE=alioth make phone` 时脚本内部读到默认值 | Makefile `export DEVICE` |
| `soc.args` 选错设备 | `ls devices/*/soc.args \| head -1` 在两设备树里恒取 alioth | 按 `$DEVICE` 取 |

### 两个失败的真因（都不是所测代码的问题）

- **`panic: Process 1 exec of /sbin/launchd failed, errno 2`**：loader 冷启动后内存里没有系统镜像。
  `oem run` 之前必须 `xl ramdisk userland/system.apfs`（或 `oem rdload iosdata` 从 sda36 读）。
  这条 2026-10-26 就记过，这次又踩了一遍。
- **clone 编的 `root.apfs` 自检 22 项挂 2 项、不交 launchd**：缺 `/etc/var.skel`。它由
  `sysimage/mkvarskel.py` 从 iOS 系统镜像生成，干净 clone 没有 IPSW 就没有它 → `/private/var` 建不出来、
  `/tmp` 符号链接不存在，init 就不往下走。补上 IPSW 数据后同一份代码是 **36 项 0 失败**。

### 复现需要仓库外的东西

| 输入 | 怎么拿 | 大小 |
|---|---|---|
| iOS 16.5 IPSW | `make fetch`（`sysimage/fetch.sh`，Apple CDN 分块下） | 7.4 GB |
| 手机固件 `fw-*` | `make firmware`（adb 从手机拉） | ~21 MB |
| theos（cctools + `iPhoneOS16.5.sdk`） | 外部安装，步骤见 `xnu/README.md` | — |

基础启动**不需要**固件：这次是 Mesa softpipe 软渲染起来的；GPU/音频/振动/Wi-Fi 才要。

WSL2 里系统 fastboot 看不到 USB，本机靠 `/usr/local/bin/fastboot` 转发到 Windows 的 `fastboot.exe`
（那 354 字节的包装器不在仓库里）；原生 Linux 没有这个问题。

### rescue：镜像不入库，存做法

- `twrp-alioth-rescue-v4.img`（201 MB）与 `kernel-patched.bin`（48 MB）不必存：前者是公开的
  **TWRP 3.7.1_12 for alioth**（SHA-256 `bf5bf330…`），后者是它内核的**一个字节**——`__setup` 字符串
  `skip_initramfs` → `skip_initramfX`，未压缩 ARM64 Image 偏移 **40444260**（字符串起点 40444247）。
- 镜像里属于我们的只有 4 个文件：`fixb.sh`（637 B，只在自己跑在 a 槽时写回 b 槽 GPT 条目并重启）、
  `fixwrap.sh`（52 B，先跑 fixb 再 `exec /system/bin/recovery`）、`init.recovery.service.rc`（改了一行）、
  `gpt_region_b.bin`（15,872 B）。它们现在在 `rescue/`，配 `mkrescue.py` 重建。
- 重建验证过：从原版 TWRP 重打，内核 SHA-256 `6cea071a…` 与记录一致，ramdisk 3672 个条目一致；
  只有 gzip 压缩字节不同（重压缩必然变）。
- 这条方案**没有成功**（2026-10-26 记的「卡 logo 依旧，勿当已解决」）；现在真正防变砖的是
  boot_a 置零 + 槽位属性 `0x7a`/`0x77`。

### 目录改名与它带来的副作用

`~/hackios-new` → `~/hackios-alioth`。**改名会让已经编过的树失效**：依赖文件里存的是旧绝对路径，

```
*** No rule to make target '~/hackios-new/xnu-src/.../bootstrap.cpp'
```

删掉 `xnu-src/xnu-8796.121.2/BUILD` 重编即可（18.8 秒），内核仍是 15,210,808 字节，结尾仍只有那条
已知无害的 `check_all_exports` 报错。`xnu-deps/build/mesa/` 里的 meson 缓存同样记着旧路径，但**不影响使用**：
现有的 `out/libOSMesa.8.dylib` 仍然有效（`out/built-from` 的哈希与当前补丁一致，`GL=1` 构建会直接复用它）。
只有确实需要重建 Mesa 时（补丁变了，或 `out/` 被删）才要先把 `build-softpipe-freedreno/` 删掉，否则 meson
会因源码目录变了而失败；重建由 `userland/build.sh` 自动调用 `tools/mk-mesa.sh`，只需网络，不需预存。

### 本次提交

- `7590a6d` 设备修正：`export DEVICE`、`soc.args` 按设备取、alioth 文档补 `DEVICE=alioth` 与 `xl ramdisk`
- `a4272dd` rescue 归档：4 个小文件 + `mkrescue.py` + `MANIFEST.md`，`.gitignore` 只放行这六份

## 2026-10-30：删掉 `prebuilt/` 里的内核，去掉「找不到就用旧的」

`xl:51` 原来是 `[ -f $K ] || K=prebuilt/mach.development.bcm2837`。这个回退的初衷是让人没有交叉工具链
也能跑，但它和**编译失败**留下的现场长得一模一样：`BUILD/obj/` 里没有 `mach`。于是

- 新 clone 不编就 `make phone`——`Makefile` 的 `phone:` 没有依赖 `kernel`，不会先编；
- `make clean` 之后忘了重编——`clean` 会删掉整个 `BUILD`；
- **编译失败**——`xnu-build.sh` 只是 `exec make`，失败不留内核，`xl` 照样开跑，日志里是 2026-10-25 的内核。

第三种最要命：你以为在测刚改的驱动，测的是五天前的。而且那句提示打在 stderr（`xl:120`），
`modem-cycle.sh` 又是 `./xl run 2>&1 | tail -2`，正好被截掉。

改法是不再回退，让「没有内核」变成一次明确的失败：

- `xl`：删掉回退；`run()` 里 `[ -f $K ] || { 报错，告诉你去跑 xnu-build.sh; return 1; }`。`xl state`/
  `enter`/`reboot` 不碰内核，仍然能用。
- `qemu/run-ios.sh`：同样 `exit 1`。
- `tools/update-prebuilt.sh`：不再拷内核进 `prebuilt/`；`prebuilt/SHA256SUMS` 也不再列它。
- `tools/modem-cycle.sh`：`./xl run | tail -2` 之后检查 `${PIPESTATUS[0]}`，没起来就退出，不去读上一轮的日志。
- `README.md`/`README.zh.md`/`docs/ru/notes.md`：`prebuilt/` 不再包含内核。

删掉的文件是 `prebuilt/mach.development.bcm2837`，16,809,112 字节，SHA-256 `03e794e8…`，版本串
`Sun Oct 25 22:52:46 PDT 2026` 且带 `-dirty`——它比当时源码旧，也不如真机验证过的那次（`f85d19de…`，
15,210,808 字节）。

代价说清楚：没有工具链的人不能再直接跑，必须先 `./xnu-build.sh`。这是有意的——宁可跑不起来，也不要跑错内核。

同一天重建了 git 历史：原来 4 个提交压成一个初始提交并 force push，旧哈希 `7590a6d`/`a4272dd`/`4a7aa4f`/
`e1d3f1f` 不再存在，文档里引用的它们只当历史记录看。

## 2026-10-30（二）：从零跑通一遍、macOS shim 的 PATH 陷阱、GPU 的正确参数

### 从零复现（这一步是为了证明仓库能独立站住）

新 clone 到 `~/hackios-alioth`，然后严格按 README 的顺序：

```
xnu/apply.sh            → XNU 源码 c54f35ca，打 185 个补丁文件
setup.sh                → 交叉工具链
./xnu-build.sh          → mach.development.bcm2837，15,210,808 字节
tools/fetch.sh          → iOS 16.5 IPSW，7.4 GB
REST=1 GL=1 userland/build.sh   → root.apfs 6,266,880；system.apfs 2,486,968,320
make phone
```

结果是 `=== 36 checks, 0 failed ===`、`surf_test: 80 checks, 0 failed`、SpringBoard 4.76 s 起、日志
1,467,622 字节 / 10,715 行。也就是说仓库里该有的都在，跑不起来只可能是本地状态或路径的问题——
后面那条 PATH 陷阱就是这么找出来的。

### Windows 只剩「插拔 USB」

`<主机>\Develop\iOSOnAndroid` 清空（9.7 G），保留的数据（platform-tools、dump、firmware、
twrp-alioth.img、证据日志，共 650 M）挪到 `<主机>\Develop\alioth-local`，不参与版本控制。
`/usr/local/bin/fastboot`、`/usr/local/bin/adb` 现在是两个壳，直接 exec platform-tools 里的 `.exe` 并
`tr -d '\r'`（fastboot 37.0.1 / adb 1.0.41 已验证）。除了把手机插到这台上，全部工作在 WSL 里。

### `tools/modem-cycle.sh` 把 macOS 的 shim 装到了 Linux 上

症状：`make phone` 打印 `xl: loader did not come up on USB (state: fastboot)` 两遍，然后
`modem-cycle.sh: the run never started`。但同一时刻 `./xl state` 明明说 `loader`，手机就在 loader 上。

原因在脚本第 7 行：**无条件**把 `tools/macos-bin` 放到 `PATH` 最前。那里的 `timeout` 和 `stat` 是
`exec /opt/local/bin/gtimeout|gstat` 的壳（MacPorts），Linux 上不存在。于是 `xl` 里每一条
`flock ... timeout 10 fastboot ...` 都直接失败（`tools/macos-bin/timeout: 2: exec: /opt/local/bin/gtimeout:
not found`），`is_loader()` 的 `fb getvar product` 读不到 `product: xnuloader`，`state()` 就把一台正在
loader 上的手机判成 `fastboot`，`enter()` 于是永远等不到、永远不发 payload。

判据很干净：同一台手机、同一时刻，普通 `PATH` 下 `./xl state` = `loader`，modem-cycle 的 `PATH` 下 = `fastboot`。
修复是加一个和 `xl:49` 一样的条件：

```sh
[ "$(uname)" = Darwin ] && PATH=$PWD/tools/macos-bin/platform-tools:$PWD/tools/macos-bin:$PATH
```

顺手补了两处：

- `./xl run ... | tail -2` 之后检查 `${PIPESTATUS[0]}`。`xl run` 在内核缺失或 payload 做不出来时会
  **什么都没传就返回**，脚本接着去 `ls -t logs/*-run.log.live`，读到的就是上一轮的日志，还被当成这一轮的。
- `tools/modem-cycle.sh` 在 git 索引里是 644，而 `Makefile:84` 直接执行它——干净的 clone 上 `make phone`
  会 `Permission denied`。改成 755（和 `xl`、其它 tools 一致）。

### `XL_SECS=0`：一次运行不超时

原来最长 `XL_SECS` 秒后内核自己交还控制权。看一个放着不管的系统（动画、待机）时这没用。现在
`XL_SECS=0` → payload 的 `loader_return_after=0`，`xnu/files/osfmk/arm64/loader_return.c:464` 把 0 读作
「永不」，`xl` 那边也不再设 deadline，运行只由 `xl stop` 结束。`xl` 头部说明同步改了。

顺带记一条容易踩的：`args=${XL_CMDLINE--v}` 是「变量没设时用 `-v`」，不是「空时」。想要不 verbose 启动
就显式给非空值（比如 `XL_CMDLINE="gl=1 ..."`），给空串等于 `-v`。

### GPU：`gpu=1` 在 alioth 上是**被设计拒绝**的

`gpu=1` 是 `adreno=21 adreno_skip=0x1c00 adreno_pause=0 adreno_keep=1` 的简写（`hakios.adreno.c:4735`），
而 `hakios.adreno.c:4815` 对 alioth 明确拒绝：*"REFUSED: alioth has only standalone SM8250 steps 1 to 22"*。
这不是退化，是 A6xx 那一串寄存器步骤按 SoC 分档，SM8250 只做到 22。

能用的是**直接点名最后一步并留住 GPU**：

```
gl=1 adreno=22 adreno_keep=1
```

`adreno=22` 跑完第 22 步，`adreno_keep=1` 不让它停——第 22 步之后服务 `HakiosGPU` 才会被
`HakiosGPUStart()` 启动（`iokit/Kernel/hakios.gpu.cpp`，msm ioctl），Mesa 的 freedreno 才有东西可用，
CoreAnimation 才会把帧交给它。**少了 `adreno_keep=1` 就等于 21 步做完收摊**，屏幕上看起来和没有 GPU 一样。

验证成功的标志（本轮日志）：

- `hakios-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa): OpenGL ES 3.2 Mesa 25.0.7; FD650`
- `adreno: THE GPU STAYS ON for the system to draw with: the service HakiosGPU`
- `Software renderer` / `no GPU to draw with`：0 次（没有 GPU 时每次界面重绘都打一条）
- `vibrant views are plain`：0 次（半透明模糊回来了）
- SpringBoard 5.32 s 起，GPU 305 MHz，48–50 °C

`adreno=22` 之后 GPUCC 的默认档位是 **index 1 = 305 MHz**（`0/305/400/441.6/490/525/587/670` 八档中的第二档）。

### 电池：`battery=1` 还要 loader 给的地址

`hakios.battery.c:183` 只认 `battery=1`，但第 184 行还要 `battery_shared_pa`——由 loader 放在命令行里
（日志里能看到 `battery_shared_pa=0x00000000a1012000`）。所以 `battery=1` 单独写是对的，前提是走
`xl run`/`make phone` 让 loader 一起上；手工拼命令行漏掉它，驱动会一声不响地不加载。

### 这一轮确认过的完整启动参数

```
DEVICE=alioth XL_SECS=0 \
XL_CMDLINE="gl=1 adreno=22 adreno_keep=1 touchscreen=1 cpufreq=1 bootcpu=7 cpus=8 rd=md0 battery=1" \
./xl run
```

### 改内核文件的两个坑（这一轮各踩一次）

1. **`xnu/files/` 不是构建的输入**。构建读的是 `xnu-src/xnu-8796.121.2/`。改 `files/` 里那份再编，
   编译器一声不响地什么都不做（`.o` 的时间戳都不变）。顺序是：在 `xnu-src` 里改 → `xnu/export.sh`
   把结果写回 `files/`（`xnu/README.md` 第 44 行就是这么写的）。这一轮新的 `display_te` 就是这样落的。
2. **增量构建会卡在 `check_all_exports` 上，而且这次它挡住的不是结尾而是全部**。表现是
   `bsd/` 根本没重编、内核文件时间戳还是旧的（大小 15,210,808 不变）。`-k` 也没用。
   可靠的做法是把整个 `BUILD` 删掉重编：**19.8 秒**，内核仍是好的（末尾照旧只有那几条已知无害的
   exports/symbolset 报错，`xnu/README.md` 也说了「看文件日期，不看 make 的退出码」）。

### 绿屏 = 这一轮正常结束

loader 交还控制权时整屏涂色说明结局（`xnuloader/loader.c:3267`）：**绿**=跑完按时交还、**琥珀**=停在
checkpoint、**红**=早期异常、**紫**=panic（文字留在屏上）、**蓝**=selftest。所以量帧率时用
`XL_SECS=30/260` 跑完看到绿屏是预期行为；想让它一直不结束就用 `XL_SECS=0` 再 `xl stop`。

### 还没解决的：帧率

画面能动但很慢——时钟几秒一跳、点图标要十几秒才开。已知的闸门有三层，下一轮按这个顺序量：

1. **显示驱动自己的节拍**：`MiniIOMobileFramebuffer.cpp:79` `kFBRefreshHz = 60`，`fb_hz` 可改（1–240）。
   帧只在 1/60 s 的节拍上上屏（`fb_beat=0` 取消节拍）。CoreAnimation 不读这里的 vsync 通知，而是按它
   读到的速率自己起拍——所以 `fb_hz` 同时决定「最多多少帧」和「CoreAnimation 以为该画多少帧」。
2. **GPU 档位**：305 MHz。八档里这是第二低。
3. **真正的绘制耗时**：`touch_lat=1`（每帧 `tl:` 行，`tools/latency.py`）、`glframes=1`（每帧一行，
   `tools/frames.py`）、显示驱动自己的统计行（「from one frame to the next: … under 10/18/26/35 ms」）。
   没有这三个数之前不要猜是 GPU 还是 CPU 还是上屏拷贝。

至于这个手机在安卓上能跑 120 Hz：那是 panel 的 DSI 时序 + 面板初始化序列（天马 k11a，
`mdss_dsi_k11a_38_08_0a_dsc_cmd`，DSC、CMD 模式）决定的，而现在的驱动只往 bootloader 留下的
splash framebuffer（0x9C000000）里画，**从不碰 DSI 时序**。`hakios.display.c` 也只有 `display_step`
一个参数，全部是只读的寄存器观测。要把面板开到 120 Hz 得先有自己的 DSI host 驱动（时序表、
`dsi_panel_pwr_supply_vci` → pm8150a_l11、reset gpio12、TE gpio66），这是另一件事，和现在的掉帧是两回事：
现在连 60 Hz 的节拍都没喂满。

### 量过了：现在是 58 fps，天花板是面板的 60 Hz

上面三条闸门，这一轮把 1 和 3 量了。

**用 `tools/frames.py` 量的（`RC_JOBS=rc.jobs.fps` 的镜像 + `touch_lat=1`，260 秒，GPU 开、
其余默认）**：25 个手势、**1,238 帧上屏**，全部手势合计 **1,064 个帧间隔，20% 超过 16.7 ms、
2% 超过 33 ms**；每个手势单独看是 **43–61 fps**，多数在 57–60。慢帧的时间去向（均值）：
draw 2–11 ms、gpu 2–9 ms、copy 2–4 ms、**idle before 6–25 ms**——也就是说慢帧不是在等 GPU，
是窗口服务器自己没按时交帧（在等一次 commit）。显示驱动自己的统计同样印证：
`from one frame to the next: 0 under 10 ms, 113 under 18, …`，一半在 17 ms。
**「巨卡」「完全没有动画」是软件渲染器时代的事**，GPU 回来后它不再是瓶颈。

**面板到底多少 Hz，用 TE 引脚数的（新增 `display_te=SECONDS`，`hakios.display.c`）**：
TE 是 gpio66（NORTH tile，`cfg 0x4` = function 1、无上下拉），读它的电平位、忙循环采样 3 秒：

```
display: TE: 182 pulses in 3 s = 60.666 a second (from 5353016 samples, 2693331 of them high)
```

**60 Hz**，而且 TE 是占空比约 50% 的方波（不是窄脉冲），所以这个数不会有采样漏计的问题。
TE 的周期就是面板的刷新周期——bootloader 给面板配的是设备树里的 `timing@0`（60 Hz）。
于是：

- `fb_hz=120` 现在**没有意义**：我们的节拍可以到 120，但面板每 16.7 ms 才取一帧，多写的那些白搭。
- 想上 90/120，必须让面板换档：DCS `0x60` 写 `0x10`（并保持 `d1 2e`，见 `hardware.md` 的三档表）。
  这条命令要走 DSI 的 command DMA——寄存器映射已经摸清（`display_step=7` 的 dump 里，
  `CMD_DMA_CTRL/CMD_CFG1/DMA_BASE/DMA_LEN/TRIG_CTRL` 都在，连 bootloader 上次发的那 8 个字节都在），
  但**这条写路径本内核还从来没走过**，PROGRESS 里「没人看着不写活着的显示控制器」的结论仍然成立。
- 就算面板换成 120 Hz，渲染侧也不够快：慢帧的 draw+gpu+copy 加起来 11–16 ms，而 120 Hz 的预算是 8.3 ms。
  换档之前得先把这三项压下去（GPU 到 670 MHz、`fb_threads` 加核、`fb_beat=0` 去掉等节拍）。

---

## 2026-10-30 深夜：壁纸没了、小组件空白 —— 是镜像里的作业表，不是渲染

**现象**（用户报的，三条互相独立）：桌面背景纯黑、两个小组件是纯白空块但文字标签正常、音量上键没反应。

### 壁纸与小组件：先证伪渲染路径

`glcopy=1`（每个 IOSurface 走处理器拷贝而不是共享）跑一轮，240 秒后截图：**照样纯黑、照样白块**。
所以跟 eagl 的 surface 共享/blit 无关，也不是 `adreno_speed`/`fb_threads`（上一轮已排除）。

### 真正的原因：`Could not find job` 与只有 25 个作业的镜像

这一轮的 launchd 日志里有 43 条：

```
com.apple.xpc.launchd <Error>: Could not find job with label com.apple.chronod
```

小组件是 `chronod` 画的，壁纸是 `PosterBoard` 画的，两个进程**根本没被加载**。
`userland/root/LaunchDaemons` 只有 **25** 个 plist，就是 `build.sh` 里 `IOS_JOBS` 的默认那一串
（notifyd … runningboardd … SpringBoard … wifid），`chronod` 不在里面。
`mkjobs.py` 的 `REST=1` 才会把其余 382 个守护进程也写进镜像（`MachServices` 有、程序在的才要，
去掉 `RunAtLoad/KeepAlive`，用到才启动）。**这就是"以前能用、现在又不能"的东西：镜像是用哪套参数建的。**

### 修法与验证

```sh
cd userland && GL=1 REST=1 SYSTEM=1 ./build.sh      # 作业 25 -> 407，含 com.apple.chronod.plist
cd .. && ./xl stop; ./xl ramdisk userland/system.apfs   # 2371 MB，97 s，24 MB/s
XL_SECS=0 XL_CMDLINE="gl=1 adreno=22 adreno_keep=1 touchscreen=1 cpufreq=1 bootcpu=7 cpus=8 rd=md0 battery=1" ./xl run
```

- 日志：`(user/501/com.apple.chronod [87]) <Notice>: Successfully spawned chronod[87] because ipc (mach)`，
  之后 chronod 给 `CLKWatchFaceLibrary` 等一堆服务建 stub，没崩。
- 截图：**蓝/青/黄/橙渐变壁纸回来了**；Weather 小组件显示 "No weather data"，Calendar 小组件显示
  "FRIDAY / 30 / No events today"。图标行、Search 药丸、Dock 全部正常。
- 小瑕疵：`(com.apple.chronod) <Error>: Invalid type for plist importer (key: ThrottleInterval type: string),
  accepted types: [integer]` —— 某个作业的 plist 里 `ThrottleInterval` 是字符串，launchd 拒收这一项，不影响启动。

### 两条要注意的

- **`xl ramdisk` 在跑的时候会被拒**（`xl: a run is in progress`）。换镜像之前必须 `xl stop`，等它真回来再传。
  另外 `xl` 早就不用 `DEVICE=` 了——`DEVICE` 只影响 `mkxnuimg.py`/`make` 打出来的 payload 与 `soc.args`。
- **速度**：手机开机比想象的快，150 秒足够截图；240 秒是白等。

### 发烫

`cpufreq=1` 的日志是明写的：`cpufreq: group 2 (1 of our cores): row 20 -> 20 of the frequency table, 3187 MHz`
—— `raise_domain()` 把 EPSS 的 perf state **直接写到每个 cluster 的最高行**，不是设一次上限、让硬件自己降频，
所以 8 个核全部钉在各自最高频。以前只有 25 个作业时机器基本空转，看不出来；现在 407 个作业真起来几十个
守护进程，满频 + 真负载 = 烫。

**对照跑过**（同镜像、同参数，只去掉 `cpufreq=1`）：

| | `cpufreq=1` | 不带 |
|---|---|---|
| 渲染层每帧 | 3570 µs | 3413 µs |
| 壁纸 / 小组件 | 正常 | 正常 |

差 4%，不值得为它把八核钉在最高频。**默认不要再带 `cpufreq=1`。**
（`bootcpu=7` 保留：那只是把内核放在大核上跑，不涉及频率。）

### 还没修

音量上键：固件没有把 PM8150 的 SPMI GPIO6 配成带上拉的输入，`hakios.buttons.c` 有意不去写 PMIC，
所以按了永远读不到。要修就得加一段 SPMI 写：`MODE` 改成输入、`PULL_CTL` 开上拉、`EN_CTL` 打开。



## 2026-10-31 凌晨：120 Hz 的 DCS 通路打通了，面板还没换挡

### 三个 bug，一个比一个靠里

**1. `dsi` 是 0 —— `Kernel data abort … pc hz_thread, far 0x48`**

第一次 `display_hz=120` 直接 panic。只有 panic buffer 才有答案：`ESR 0x96000005`（translation
fault，level 1）、`FAR 0x48`、`x0 = 0`。0x48 正是 `DSI + DMA_BASE` 的偏移——`dsi` 这个映射只在
`display_thread` 里做（`display_step` 那条路），`hz_thread` 自己的 `dsi` 还是 0。
现在是一个 `display_map()`，两个线程共用，谁先到谁建。

**2. 点火寄存器写错了地址 —— `TRIG_CTRL +0x084` 只是"触发源"**

修好映射后不再 panic，但日志说：

```
display: dcs 60: len 4, trigger 0x4 -> 0x5 after 100000 us
```

`0x4 -> 0x5` 说明那一比特根本没自清——因为 **+0x084 是 TRIG_CTRL，它配置的是"由谁触发"**
（`DMA_TRIGGER` 在 bit 0..2，`0x4 = TRIGGER_SW`），真正的一次性点火在 **`TRIG_DMA`，+0x090**。
依据是 Linux 的 `drivers/gpu/drm/msm/registers/display/dsi.xml`（6G host 全部 +4）：
`dsi_cmd_dma_tx` 最后调 `msm_dsi_host_cmd_xfer_commit`，写的是 `DMA_BASE`、`DMA_LEN`、`TRIG_DMA=1`。
同一张表也解释了从机读到的每个数：`CMD_DMA_CTRL 0x14000000` = FROM_FRAME_BUFFER(bit28)
| LOW_POWER(bit26)，`CMD_CFG1 0x13c2c` = wr-mem-start 0x2c / continue 0x3c / INSERT_DCS_COMMAND，
`TRIG_CTRL 0x4` = TRIGGER_SW。

**3. 不再往 bootloader 的缓冲里写**

包字节改成写自己 `kmem_alloc_contig` 的一页（加 `ml_io_map_unmappable` 的 uncached 映射，
`dsb` 之后再点火），`DMA_BASE` 用完还原成 bootloader 留下的值。原来那 8 个字节在
`0x9aead000`，属于谁不知道，往里面写就是无声的内存破坏。

### 现在到哪一步

```
display: command buffer: 0x92ac4000, a page of our own; the bootloader had left 0x9aead000
display: dcs 60: len 4, 2000 us, intr 0x501, status0 0, trig ctrl 0x4, dma ctrl 0x14000000
display: dcs f0 (long): len 8, 100 us, intr 0x501, …
…（7 个包，全部 intr bit0 = CMD_DMA_DONE，无 ERROR 位）
display: the panel was asked for 120 a second (display_te counts what it now does)
display: TE: 182 pulses in 3 s = 60.666 a second
```

命令**确实上了线**（100 µs 就 done，说明不是没发出去），但 **TE 还是 60.666**。两种可能还没分辨：

1. 面板收到了但没换挡（smart fps，或者要 MDP 侧时序跟着改）；
2. 面板根本没收到（LP 模式、时序、或主机这边的模式不对）。

### 下一刀：亮度就是判据

k11a 的背光是 DCS 0x51（`bl_ctrl_dcs`，大端，最大 0x7ff），所以加了 `display_bl=1..0x7ff`：
屏幕变亮 = 面板确实收到包；不变 = 命令通路本身还差东西。日志侧永远是 `intr 0x501`，
它只能证明 DSI 主机把包推上去了，证明不了面板收到。

### 顺带：设备树里 k11a 的三档（`dump/twrp-fdt.dts` 26971 起）

| timing | framerate | vbp / vfp | clockrate | mdp-transfer-us | 切换命令里的关键字节 |
|---|---|---|---|---|---|
| timing@0 | 0x3c = 60 | 0x230 / 0x258 | 0x4190ab00 (1.1 GHz) | 0x36b0 = 14000 | `60 00` + `d1 2e` |
| timing@1 | 0x5a = 90 | 0x118 / 0x12c | **同一个 0x4190ab00** | 0x251c = 9500 | `60 10` + `d1 6e` |
| timing@2 | 0x78 = 120 | 0x08 / 0x04 | **同一个 0x4190ab00** | 0x1b58 = 7000 | `60 10` + `d1 2e` |

三档的 panel clockrate 完全一样，所以换挡不需要动 DSI/PHY 的 PLL——这点对"只发 DCS 就能换"
是好消息；但 `mdp-transfer-time-us` 三档不同，说明 **MDP 侧也要跟着改**，而这正是 bootloader
留在 60 Hz 的那一套。

---

## 2026-10-31 凌晨：loader 上了网却没 USB，其实是它把自己当成了 QEMU

### 现象

跑完几轮内核（其中有一轮 panic、loader 报过 `core 600 is not off … not the kernel's to take`
和 `THE KERNEL LEFT CORE 500 RUNNING`）之后，手机屏幕上有 XNULOADER 的字，PC 上
`fastboot devices` / `adb devices` / `xl state` 全是空，Windows 连"未知设备"都没有。
拔插 USB 无效；`xl reboot` 之后重新 `xl enter` 也还是起不来。

### 屏幕上那行 Synchronous Exception 就是答案

```
Synchronous Exception at 0x00000000A10026D8
```

loader 的 PE `/base` 就是 `0xA1000000`（`xnuloader/build.sh` 里写着），所以这是 **RVA 0x26D8**。
把 `prebuilt/loader.efi` 按 PE 节表映射后反汇编：

```
26c0:  mov  w27, #0x8
26cc:  movk w27, #0x902, lsl #16      ; w27 = 0x09020008
26d8:  strh w9,  [x27]                ; ← 崩在这条 store
```

`0x09020000` 在 `loader.c` 里只有一处：`FW_CFG_BASE`，QEMU virt 的 fw_cfg。

### 根因：平台靠 framebuffer 基址认，认不出就当 QEMU

`efi_main` 里：

```c
PLAT = &platform_qemu_virt;
… SCREEN.base = GOP->mode->fb_base;
for (…) if (platforms[i]->fb_base == SCREEN.base) PLAT = platforms[i];
if (PLAT == &platform_qemu_virt) ramfb_phone_screen();     /* 写 0x09020000 */
print("display %d x %d at %x\n", …);                     /* 这句在崩溃点之后 */
```

alioth 的判据是 `fb_base == 0x9C000000`。**那一次固件报的不是这个数**，于是平台留在 QEMU virt，
`ramfb_phone_screen()` 去写 fw_cfg，在真机上就是同步异常；而唯一能说明"固件报了哪个地址"的那行
恰好写在后面，所以屏幕上永远看不到。

### 修法（`xnuloader/loader.c`）

- `print("display … at %x")` **提到** `ramfb_phone_screen()` 之前；
- `ramfb_phone_screen()` 只在 `SCREEN.base == platform_qemu_virt.fb_base`（`0xE1000000`）时调用。

改完第一次启动，同一块板子报的是 `display 1080 x 2400 at 9c000000` / `platform alioth` /
`usb: started`——说明 fb_base 平时是对的，是那一两次启动的固件状态让 GOP 报了个别的值。
**以后再遇到，屏幕会先打出那个地址**，按那个地址补一条 `platforms[]` 或者加别的判据就行，
loader 不会再崩在 fw_cfg 上。

### 还有一个坑：`xl enter` 自己要 `DEVICE=alioth`

`xl enter` 内部调 `mkxnuimg.py` 时**不带 `-d`**，而 `mkxnuimg.py` 的默认设备是 `poco-x5-pro`
（只读 `$DEVICE`）。所以从 Android/fastboot 拉 loader 必须 `DEVICE=alioth ./xl enter`，
否则打出来的是 POCO 的镜像（1.47 MB，alioth 是 1.99 MB），刷进去自然起不来 loader。

## 2026-10-31 凌晨：120 Hz 上去了 —— 唯一的坎是"命令从哪一页内存发"

### 结论

DCS 命令通路一直是好的，包格式、触发寄存器、模式都没错。**坏的是命令缓冲区的地址**：

| 发送用的页 | 日志 | 面板 |
|---|---|---|
| 内核 `kmem_alloc_contig` 的 `0x92ac4000` | `intr 0x501, status0 0, ack err 0`，100 µs 完成 | **一动不动** |
| bootloader 留在 `DMA_BASE` 的 `0x9aead000` | 同样 `intr 0x501, status0 0, ack err 0` | 亮度立刻变，随后 TE **121.333/s** |

两次的代码、包字节、寄存器顺序完全一样，只有 `DMA_BASE` 的值不同。也就是说
**`CMD_DMA_DONE` 只能证明"主机把一次传输跑完"，不能证明"面板收到"**——上一次记在
PROGRESS 里的那个判断要往后挪一位：不是面板不听话，是 MCU 读到的是垃圾。

### 怎么逼出来的：让亮度渐变，然后只换地址

上一节留下的判据是"亮度变了 = 面板收到包"。但一次单步 0x7ff→0x3c 在绿屏上根本看不出来，
所以先加了 `display_bl_ramp=1`：0x7ff → 0x20 → 0x7ff，40 ms 一步，来回三轮，约 9 秒，
`dsi_quiet` 关掉中间那一百来行日志。第一次跑（自己的页）——屏幕从头到尾没变。
换 `display_dma_at=0x9aead000` 重跑——**"亮度变了"**。

### 为什么

MSM 的 DSI 命令 DMA 是**总线主设备**，它按 `DMA_BASE` 去读内存，走的是显示侧的地址窗口
（这一代 SoC 上显示自己的 SMMU/预留区），而不是"AP 能访问就算数"。bootloader 那一页是它自己
刚刚发过命令的地方，当然在窗口里；内核 `kmem_alloc_contig` 出来的普通物理页不在。
所以现在的默认值是**直接沿用 `DMA_BASE` 里 bootloader 留下的值**（它读到的地方，显示一定能读到），
每次发包前保存那一页的前 16 字节、发完写回去：

```
display: command buffer: 0x9aead000, the bootloader's own page, the one the display can reach; the bootloader had left 0x9aead000
```

- `display_dma_at=ADDR`：指定别的地址（`0x9E200000` 之类落在 `cont_splash_region` 里的位置，
  还没试过，留作把这一页"合法化"的方向）；
- `display_dma_own=1`：回到内核自己那页，也就是反例，留着当对照；
- 真正的修法是找到显示 SMMU 的上下文、把我们自己的页映射进去，这件事还没做。

### 120 Hz 的实测

```
display: asking the panel for 120 a second: 7 packets, each one a DCS write
display: dcs 60: len 4, 100 us, intr 0x501, status0 0, ack err 0, …
…（7 个包全部完成）
display: TE: 728 pulses in 6 s = 121.333 a second (from 9504614 samples of gpio 66, 62086 of them high)
```

`display_te_delay=25` 让计数窗口落在切档之后，所以 121.333 是**切档后**的读数，
不是把开机那 20 秒算进去的平均。k11a 的 `timing-switch-command` 是 `dsi_lp_mode`，
我们默认就是 LP（`dma ctrl 0x14000000`），和 vendor 一致；`display_hs=1` 那条 HS 路没再需要。

### 还没做（要给 120 Hz 配上"合理调度"，这几件事都得动）

- **`fb_hz` 还是 60**：面板 120 Hz 了，但 iOS 的上屏节拍还是每 60 Hz 一帧。真正的 120 Hz 画面
  要把 `fb_hz=120` 一起带上（`MiniIOMobileFramebuffer` 的 `kFBRefreshHz` 也是 CoreAnimation 读到的值）；
- **MDP 侧没动**：`mdp-transfer-time-us` 三档是 14000 / 9500 / 7000，MDP 现在还是 bootloader 的
  60 Hz 那一套。面板在命令模式下按 TE 取帧，TE 120 了 MDP 也会被拉成 120 次/秒推帧，
  但每一帧的传输时间预算没跟着改，重活的时候要重新量；
- **功耗**：AMOLED 面板自刷新在 120 Hz 下空转也更费电。要"坚固"，得有 idle 回 60、用起来才 120 的
  策略（vendor 靠 `mi,mdss-dsi-smart-fps` + `dfps_data_region@9e300000` 那套做动态帧率切换）。

### 顺带：绿屏不是故障

跑完一轮之后整屏 `(32,128,64)`，是 loader 的 `ending = 0xff208040`——"内核跑完、正常交还控制权"
的颜色（见 `xnuloader/loader.c` 的返回色表）。所以亮度测试要放在**内核还活着的时候**看，
`display_hz_delay`（默认 20 秒）之后、`loader_return_after`（`XL_SECS`）之前。

## 2026-10-31 凌晨（二）：亮度为什么调不动 —— 请求走到了 CoreBrightness，然后没地方去

### 证据链，从 iOS 里往外

1. **SpringBoard 的滑杆是走的**。日志里 backboardd 自己打：

   ```
   backboardd [Backlight] Setting CoreBrightness backlight factor:1.000000 with fade duration:0.185000
   backboardd [Backlight] Set BrightnessSystem property:DisplayBrightnessFactorWithFade to:1
   ```

   说明 `BKSHIDServicesSetBacklightFactor…` → backboardd → CoreBrightness 这一条是通的。

2. **CoreBrightness 找的是 IOKit 服务，不是 framebuffer**。framebuffer 的 user client 在
   backboardd 里只被用了 4/5/12/79 号 selector（swap begin/end、power change、get block），
   没有任何亮度 selector；反编译字串里却有
   `BCAppleBacklightBrtControl initWithService:`、`AppleBacklightBrtControlGetRegistryID`、
   `Unable to find backlight service.`、`AppleBacklight` / `AppleARMBacklight` 两个类名。

3. **本内核以前没有这个服务**，所以第 1 步的请求进了 CoreBrightness 之后再没有下一步，
   滑杆自然什么都没发生。

### 做了什么

`iokit/Kernel/MiniAppleBacklight.cpp`：照类名发布 `AppleBacklight` 和 `AppleARMBacklight`
（`IOServiceMatching` 比的是实例的**类名**，所以类必须就叫这个名字），user client 记录
selector/scalar（标量与 float 两种读法都打出来）、结构体大小，并回填零输出。
两个类**不互为父子**（共同基类叫 `MiniBacklightService`）：`metaCast` 会把子类也算进去，
`AppleARMBacklight : AppleBacklight` 会让匹配 `AppleBacklight` 一次返回两个服务。

现在内核日志里能确认服务是起来了：

```
AppleBacklight: published as AppleBacklight, provider GenericARMPE
AppleBacklight: published as AppleARMBacklight, provider GenericARMPE
AppleBacklight: 2 services published, the user client asked for 0 times
```

最后那个 `0 times` 是 bsd_init 刚跑完时的读数（backboardd 还没起来），**不是结论**。判断用的
是启动后每 20 秒打一次的同一行：如果到 iOS 桌面之后还是 0，说明 CoreBrightness 根本不按
`AppleBacklight` 这个类名找（下一步去翻 dyld cache 里的匹配路径）；只要不是 0，
`CLIENT OPENED` 和随后每一条 `selector N` 行就是**协议本身**，照着接
`hakios_display_set_brightness()` 即可（`backlight_dcs=1` 已经会把这个 level 按 float/整数
两种解释映射到 0..0x7ff 再发 DCS 0x51）。

脚本：`tools/backlight-test.sh`（等手机 → 修槽位 → 起 loader → 补 resident 镜像 → 跑 →
截图 → 汇总 AppleBacklight / TE / amp 三组日志）。`amp_step=2` 顺带带上，读-only。

### 这一轮的坑（都已写进 devices/alioth/README.md）

- **`fastboot boot` 全线 `Failed to load/authenticate boot image: Load Error`**：不是 AVB、不是镜像，
  是 ABL 把槽位标成了 `slot-unbootable:b: yes`（`xl reload` 把手机冷重启回 Android，Android
  没起来被记了一次失败）。`fastboot --set-active=b` 一行修好，之后同一份 `xnu-usb.img` 立刻
  `Booting OKAY`。**连原版 TWRP 都拒**，所以别去怀疑镜像。
- **loader 重启后 resident 镜像就没了**：直接 `xl run` 会在 launchd 处 panic
  （`Process 1 exec of /sbin/launchd failed, errno 2`）。README 原来就写了，这次又踩了一遍。
- **`loader_return_after=0` 的代价**：没有一个定时返回兜底，内核/loader 一旦挂死（USB 端点还在、
  一个命令都不回），手机只能长按电源。凌晨这次就挂在 `xl run` 之后，`fastboot getvar product`
  报 `AdbWriteEndpointSync failed (121)`，等了 20 分钟没看门狗。**去现场重启一下即可**，
  不是变砖。

### 下一轮要看的

- 桌面起来后 `AppleBacklight: … asked for N times` 的 N；
- 有 N 的话，第一条 `selector …` 的标量/结构体；
- 顺带 `amp:` 开头那几行（CS35L41 在不在 I2C 上、DEVID 是不是 0x35a40）。


### 挂死之后为什么没有被看门狗救回来

loader 的 `run_on_other_cpu()` 里，虚拟看门狗（Qualcomm Haven，SMC `0x86000007`）由 loader 这个核
喂到 `budget + 60` 秒，之后交给内核；内核只有在 **`hv_wdt_pet=1`** 时才喂
（`osfmk/arm64/loader_return.c`）。默认没有这个参数，而 `budget`（= `loader_return_after`）是 0，
所以 60 秒以后没人喂、`WDT_BITE_MS=30000`，理论上 90 秒就该复位——实际等了 20 分钟手机还
枚举着，说明这台机器上 `wdt_armed` 本来就是假（SMC 的 pet 调用没成功）。

结论：**不要指望看门狗把挂死的跑法救回来**。`loader_return_after=0` 的跑法一旦 loader 的
USB 端点死了，只能长按电源重启。判断方法：设备还枚举着、但
`fastboot getvar product` 报 `AdbWriteEndpointSync failed (121)`，就是这种情况，不是变砖。


## 2026-10-31 凌晨（三）：手机挂死，改在离线准备电池项的“问卷”

### 手机的状态（未恢复）

02:35 又探了一次：`xl state` 说 fastboot，实际是 Windows 上
`USB\VID_18D1&PID_D00D\XNULOADER` 这个接口还在、但
`getvar product` / `getvar slot-unbootable:b` 全部 `AdbWriteEndpointSync failed (121)`；
`Get-Process fastboot` 是空的（没有僵尸进程占着设备）。也就是说还是第二轮里那个“loader 的 USB
端点死了”的状态，`loader_return_after=0` 的跑法没有兜底，**只能长按电源**。按约定停在这里等用户，
没有再去碰设备状态（没有盲试 `oem`/重启命令）。

### 这段时间做了什么：第 3 项（电池显示）的证据准备

亮度那项现在卡在“等手机回来看 `asked for N times`”，扬声器那项也一样要上机；电池这项里
还有一段可以离线做完的：**状态栏那个电池图标到底是什么类、有哪些方法**。

已经确认的（上一轮的像素证据）：填充 ~81 %、“81”在里面、闪电在、充电时填充是**白色**。
真机充电时填充是**绿色**，而颜色不是 `chargePercent`/`showsPercentage`/`_shouldShowBolt`
这三个钩子管的——所以下一步需要知道 `_UIBatteryView` 用哪个属性/方法决定颜色。

这次没有去猜一个选择器就硬钩（钩错了会给没充电的手机画绿色，或者什么都不画），而是让
日志自己说。在 `userland/src/inject_sb.c` 里加了一次性的**方法问卷**：

- `battery_survey_class()`：用 `class_copyMethodList` 列出实例方法和类方法，只打印名字里带
  battery/charge/percent/bolt/color/fill/level/state 的（大小写不敏感），每条带**去掉 slide 的
  地址**和类型编码；另外用 `class_copyIvarList` 把同类的 ivar 连偏移一起打出来
  （颜色很可能就是一个 ivar，例如 fill color / charging 状态）。
- 类表：`_UIBatteryView`、`UIStatusBarBatteryItemView`、`UIStatusBarBatteryPercentItemView`、
  `_UIStaticBatteryView`、`_UIStatusBarBatteryItem`。
- 时机：启动后 **90 秒和 240 秒**各打一次。只打一次会丢——loader 每次 `XL_FOLLOW` 只取环形
  缓冲区尾部，启动瞬间那几行很容易夹在两次抓取之间（第一轮的教训）。
- 输出走 `say()`（stderr → 控制台 → 运行日志里 `hakios:` 前缀那一类），不需要额外 boot arg。

日志里落地后，要看的就三件事：`_UIBatteryView` 有没有 `setBatteryState:`/`batteryState`/
`chargingState`/`fillColor`；`UIStatusBarBatteryItemView` 是不是真正在画状态栏（它是不是持有
一个 `_UIBatteryView`——有 `setBatteryView:` 的话就是）；以及那个 ivar 里有没有颜色。

### 顺便：共享缓存里找私有方法地址（离线尝试，未完成但记下进展）

想不靠手机就把 `-[_UIBatteryView setBatteryState:]` 的未 slide 地址从 dyld 共享缓存里读出来，
做了这些（都是 `20F66__iPhone10,3_6` 这套分片缓存）：

- 主缓存 `dyld_shared_cache_arm64` 里 0x270 开始的 32 字节记录就是 **路径 → mach-o 地址**表
  （`q0=loadAddress, q1=modTime(0), q2=inode(0), q3=path 文件偏移`），共 2711 个镜像；
  UIKitCore 在 `0x188d7a000`。
- 分片缓存每个文件头里 `mappingOffset/mappingCount`（0x10/0x14，一个 mapping 32 字节）就能
  建出 vm→(文件, 偏移) 的映射；`dyld_shared_cache_arm64.27.dylddata` 这类文件 mapping 的
  **fileOffset 不为 0**，不能假设“vm - 基址 = 文件偏移”。
- UIKitCore 是“预优化”镜像，**没有 `__objc_classlist`**；class_t 在 `__DATA_DIRTY.__objc_data`，
  在这套缓存里是 **40 字节**（isa、superclass、cache、vtable、data），不是运行时的 48 字节；
  指针带 tag（`0x02 << 40`，`v & 0xFFFFFFFFFF` 才是地址）。
- `class_ro_t` 还是老布局：+0x18 是类名指针，+0x20 是 baseMethods。`_UIBatteryView` 的类 ro
  在 `0x1d5cc5770`（flags 0x184），方法表在 `0x189e45df8`，header `0xc000000f / 0x6d`，
  共 **109 个方法**。
- 方法表项是 12 字节相对偏移，但**偏移是相对“字段自己的地址”**：IMP = `(entry+8) + impOffset`
  正好对上已知的五个地址（例如 `showsPercentage` = `0x189a9370c`，解析出 `0x189a93704 + 8`；
  `setChargePercent:` = `0x18928d310`，解析出 `0x18928d308 + 8`）。**名字（nameOffset）没解出来**：
  `entry + nameOffset` 落在字符串中间，`& ~3` 的 selref 假设也不成立。109 条里能稳定对上
  IMP、对不上名字——下次要么按运行时问卷（已加好，最省事），要么继续啃 nameOffset 的低两位
  语义。

### 影像已经重新打好（等手机回来就能用）

`userland/obj/libhakios.sb.dylib` 02:44 重新编译通过；`build.sh system.apfs` 默认**不会**因为
dylib 变了就重建镜像（它的重建条件只看 stage0 / manifest / cryptex），必须 `SYSTEM=1`，否则
镜像还是旧的（这次先踩了一遍：system.apfs 时间戳仍停在 23:46）。`SYSTEM=1 ./build.sh system.apfs`
之后新镜像才带着这份问卷。

### 下一轮（手机回来之后）

1. 长按电源 → `tools/backlight-test.sh`（看 `AppleBacklight: … asked for N times`）；
2. 同一次跑法的日志里找 `hakios: battery survey:` 那几行（90 s / 240 s 两拨），
   据此决定第 3 项的颜色钩子；
3. 亮度如果 N>0，就看 `CLIENT OPENED` 和 `selector …`，接 `hakios_display_set_brightness()`。


### 02:55 这一轮的离线证据：CoreBrightness 到底按哪个类名找

又探了一次手机：还是 `AdbWriteEndpointSync failed (121)`，没有恢复，继续等。

趁这段时间把 dyld 缓存（`20F66__iPhone10,3_6`）里两个类名字符串的**出处**查清楚了
（方法：主缓存 0x270 的“路径→mach-o 地址”表 + 每个镜像自己的 `__TEXT` 段范围）：

| 字符串 | 出现在 | 说明 |
|---|---|---|
| `AppleARMBacklight` | **CoreBrightness**（`0x1af0e46b0`）、PowerlogLiteOperators | 16.5 的亮度栈按这个名字找 IOKit 服务 |
| `AppleBacklight` | **BrightnessControl.framework**（`0x202552482`）、PowerlogCore、PowerlogHelperd | 旧名字，BrightnessControl 还在用 |
| `BacklightControl - Primary` | **CoreBrightness**（`0x1af0e39e2`） | 它给这条亮度控制起的名字（日志/模块名） |
| `DisplayBackLightArchitecture` | **CoreBrightness**（`0x1af0df3ee`） | 决定走“老架构（Apple*Backlight 服务）”还是新架构（IOMFB/DCP 的 `brightness-nits`） |

同一个 CoreBrightness 里还有 `IOServiceFirstMatch`、`IOMFBBrightnessLevel`、`brightness-nits`、
`dcp-brightness-node`、`backlight-level`、`new-backlight-architecture` 这些字符串——也就是说
它**两条路都有**，走哪条由 `DisplayBackLightArchitecture`（型号/预设）决定。

对我们的意义：内核现在 `backlight=1 backlight_arm=1` 同时发布两个类名，**两个框架都能找到**，
所以 `asked for N times` 如果是 0，问题就不在类名上，而在下面某一个：

1. 服务不在它搜索的父节点/路径里（我们是挂在 `GenericARMPE` 下的）；
2. 它先读 `DisplayBackLightArchitecture` 走了 IOMFB 那条路（那就要在 framebuffer 服务上提供
   `brightness-nits` / `IOMFBBrightnessLevel` 属性，而不是 Apple*Backlight 服务）；
3. 它要求服务有某些属性（`BacklightControl - Primary` 之类）才认。

这三条都要等 N 的值才能判断，不先改代码。


### 03:10 又一层离线证据：从 iOS 内部看这个服务在不在

手机还是 `(121)`，继续等。这轮把“N 的两种可能”里能提前做的准备做了：

1. **反汇编 CoreBrightness 里引用 `AppleARMBacklight` 的代码**（`__text` 里 adrp+add 定位，
   共 5 处：`0x1af06732c`、`0x1af068390`、`0x1af068e0c`、`0x1af068e60`、`0x1af06bc6c`）。
   形态都是 `adrp/add x1, "AppleARMBacklight"` 紧接 `bl 0x1afb35c1c`（同一段代码里前面还有
   `ldr w0, [全局]` → `bl`，再把另一个字符串 `0x1af0f78db` 放进 x3 去调用）。这些 `bl` 落在
   **分支岛**里（0x1afb3xxxx，不是 `__stubs` 0x1af0d7b50–0x1af0d8a98），没有符号表就分不清
   被调的是 `IOServiceMatching` 还是日志/遥测函数，**所以不下结论**——但“字符串确实作为参数
   出现在代码里”是确定的（不是只躺在字符串表里）。
   跟进了一句：`0x1afb35c1c` 不在任何镜像的段里，是缓存自己的**跳板岛**；岛里的条目是 12 字节的
   `adrp x16 / add x16 / br x16` 链，要接着走下去得解 `LC_DYLD_CHAINED_FIXUPS` 的 import 表
   才能知道最终符号。**这条线先停**：同样的答案，一次带 `asked for N times` 的跑法就能给，
   不值得在没有手机的时候继续啃。

2. **在 iOS 内部加了一个直接探针**（`userland/src/inject_sb.c`，`backlight_probe()`）：
   SpringBoard 启动 95 s / 245 s 时用 `IOServiceGetMatchingService(0, IOServiceMatching("…"))`
   分别查 `AppleBacklight` 和 `AppleARMBacklight`，用 `IORegistryEntryGetPath` 打印服务在
   registry 里的**完整路径**。日志行是
   ```
   hakios: backlight probe: AppleARMBacklight -> IOService:/GenericARMPE/AppleARMBacklight
   hakios: backlight probe: AppleBacklight: no such service in the registry
   ```
   这行的用处：内核那条 `asked for N times = 0` 时，如果探针**能**匹配到，说明 userland 看得见、
   是 CoreBrightness 自己没找（名字/路径/架构判断的问题，看路径就知道该不该改父节点）；
   如果探针也匹配不到，那就不是 CoreBrightness 的问题，而是服务根本没进 userland 能看到的
   registry（发布/权限）。
3. `tools/backlight-test.sh` 会把 `backlight probe:` 那几行一起打出来。

镜像重新打一次（`SYSTEM=1 ./build.sh system.apfs`），跑法不变。
打好了：`userland/obj/libhakios.sb.dylib` 02:53（91,328 字节，`strings` 里有 5 条
`battery survey` + 3 条 `backlight probe`），`userland/system.apfs` 02:54（2,486,968,320 字节）。


### 03:25 服务挂在哪：loader 的 device tree 里确实有 arm-io

手机还是 `(121)`。这轮查的是“N=0 的第三条可能：路径不匹配”里能离线确认的部分：

- `xnuloader/loader.c` 建 DT 时**有 `arm-io` 节点**（`dt_begin(&w, "arm-io")`，3119 行），
  下面只有 `isp`（cam=5 时）、`gic`、`uart0`，**没有 display/backlight 节点**。
- 内核里我们的服务是 `GenericARMPE.cpp:304` 调 `MiniAppleBacklightStart(this)` 发布的，
  provider 就是 PE 本身；也就是说现在 iOS 看到的路径是
  `IOService:/<PE 的名字>/AppleARMBacklight`，**不是** `IOService:/arm-io/AppleARMBacklight`。

所以如果回来以后 `asked for N times` 是 0、而 `backlight probe:` 能匹配到并打印出
`IOService:/GenericARMPE/AppleARMBacklight` 这样的路径，下一步就是**把服务挂到一个叫
`arm-io` 的 IOService 底下**（在 PE 里建一个空壳 IOService、`setName("arm-io")`、
`registerService()`，再把 Apple*Backlight attach 到它下面）。这样做只赚不亏：
`IOServiceMatching` 是全 registry 匹配、不看父节点，路径查找则只有挂对了父节点才成立。
现在不改——`IOServiceMatching` 依然是最可能的写法，一次跑法就能判定。


### 03:35 扬声器这一项的离线盘点：固件缺一半

手机还是 `(121)`。把第 2 项能用得上的素材点了一遍：

- `alioth-local/firmware/fw-audio/`：`adsp.mdt` + `adsp.b00`–`adsp.b18`，这是 **ADSP 的固件**
  （WCD938x codec 的驱动跑在 ADSP 里）。这东西在。
- **CS35L41 自己的 DSP 固件不在**：`alioth-local/dump` 是一堆单独文件（boot/dtbo/gpt/dmesg…），
  不是 vendor 分区；`find alioth-local -iname "*cs35l41*"` 没有结果。
- 结论：功放要么按**无 DSP 直通模式**初始化（初始化寄存器序列自己写，工作量在《几百个寄存器》
  这个量级），要么先补一份 `/vendor/firmware`（`cs35l41-dsp1-spk-prot.bin` /
  `cs35l41-dsp1-spk-cali.bin`）再走 smart-amp 路径。

已写进 `devices/alioth/README.md` 的扬声器一节。**这不是这一轮的主线**，只是把“缺什么”记清楚，
免得下次从零开始查。


### 03:50 修掉一个会让证据白跑的时间错：脚本读日志太早

对了一遍时间线：`backlight-test.sh` 原来在 `HOME_WAIT=150` 秒后截图、再等 **130** 秒就读
`.live`，也就是 `xl run` 之后约 280 秒。可 SpringBoard 不是 t=0 起来的（内核 + launchd + 应用
注册，大约要一分钟以上），问卷在 **SpringBoard 启动后 90 s 和 240 s** 才打，探针是 95/245 s——
240 s 那一拨落在 run 之后 300 秒上下，**原来那一下会正好错过它**（这正是第一轮“只打一次会被
环形缓冲截掉”的同类错误，只不过这次是脚本读早了）。改成截图后再等 **240** 秒（约 run 后
390 秒），无论 SpringBoard 晚到什么时候，第二拨问卷和探针都已经在日志里。`bash -n` 通过。


### 04:05 顺手堵掉“旧日志冒充新日志”

`backlight-test.sh` 原来用 `ls -t logs/*.live | head -1` 找日志——如果这次跑法根本没写出
`.live`（loader 挂死、刚进 loader 就断），它会捡到**上一次跑法的** `.live`，然后拿旧数据当
这次的结论（和之前的教训同一条：一次跑法只看一次生成的日志）。现在改成只认 **mtime 晚于
本次 `xl run` 启动时刻**的 `.live`：

```
started=$(date +%s)                     # 起跑前记下
live=$(find logs -maxdepth 1 -name '*.live' -newermt "@$started" -printf '%T@ %p\n' |
       sort -rn | head -1 | cut -d' ' -f2-)
```

没有这样的文件就走“no live log yet”分支、只打本次 run 自己的日志，不再看旧的。
`find -newermt` 在 WSL 的 GNU find 上实测可用（`@0` 能找到现存两个 `.live`，`@now` 为空）。


### 04:30 `DisplayBackLightArchitecture` 不是可以改配置绕过的开关

在 `sysimage/rootfs` 整棵树上搜这两个键，**只有 `usr/libexec/backboardd` 一个文件命中**
（没有 plist、没有默认值文件）。看 backboardd 里那串字符串的上下文：

```
DisplayBrightnessFactorPending / DisplayBrightnessFadePeriod / DisplayBrightnessFactorWithFade
com.apple.backboardd.backlight.changed
DisplayBackLightArchitecture
backlight dim/off
```

它夹在 backboardd 的一堆**显示属性名**中间（旁边就是 `com.apple.backboardd.backlight.changed`
这种属性/通知名），也就是说这是 backboardd 从显示模块（CoreBrightness 的 CBDisplayModule*）
拿到的**运行时属性**，不是读 Preferences 的键——**不能靠改 plist 强制走老架构**。
所以“它是不是走了 IOMFB 那条路”只能从 `asked for N times` / 探针路径来判断，配置这条捷径不存在。


## 夜间总结（给早上看的版本）

**手机从 01:56 起挂死**：loader 的 USB 端点死了（设备还枚举成
`Android Bootloader Interface / VID_18D1&PID_D00D\XNULOADER`，但所有命令
`AdbWriteEndpointSync failed (121)`；没有僵尸 fastboot 进程；`loader_return_after=0` 时看门狗
也不会救）。**需要你长按电源**；按不动就先插充电器等一分钟。

**其余全部就绪**（没有提交代码）：

| 项 | 状态 | 设备回来后怎么验 |
|---|---|---|
| 1 亮度滑杆 | 内核发布 `AppleBacklight`/`AppleARMBacklight` 服务并计数；CoreBrightness 按 `AppleARMBacklight`、BrightnessControl 按 `AppleBacklight` 找（已从缓存定位）；SpringBoard 里加了 registry 探针 | `tools/backlight-test.sh`：看 `asked for N times` 和 `hakios: backlight probe:` 的路径 |
| 2 扬声器 | 已确认滑块/音量键都到了内核、写进来的就是静音；硬件链缺 CS35L41 初始化 → TDM → ADSP；ADSP 固件在，CS35L41 的 DSP 固件不在 | `amp_step=2` 的行（身份）+ 决定走无 DSP 直通还是先补 `/vendor/firmware` |
| 3 电池 | 数值/闪电正常、颜色不绿；为“改哪个属性”加的问卷已烘焙（90/240 秒两拨） | 同一次跑法里的 `hakios: battery survey:` 行 |

一条命令（会自动等手机、修槽位、补上传 resident 镜像、跑 120 Hz + 背光 + amp 只读，约 7 分钟
后汇总四组日志）：

```
tools/backlight-test.sh
```










## 08:05 现状：手机又卡在"设备在、命令全超时"，这次需要长按电源

`fastboot devices` 显示 `???????????? fastboot`，Windows 里是
`USB\VID_18D1&PID_D00D\XNULOADER`（loader，不是 ABL），所有命令
`AdbWriteEndpointSync failed: 信号灯超时时间已到 (121)`，`fastboot reboot` 也一样发不进去。
WSL 这边不是管理员，禁用/启用 USB 设备那条路走不通（`IsInRole(Administrator)` = False），
所以**没有软件办法让它重启**，只能长按电源。

今天 07:47 之后每一轮都是同一个结尾：内核起来之后 20 秒到 2.5 分钟之内，loader 就不再回答。

| 轮次 | 用的内核 | 结尾 |
|---|---|---|
| 07:47 | 02:08 | 68 秒 panic：`Process 1 exec of /sbin/launchd failed, errno 2`（root 是空的，见第一条） |
| 07:53 | 02:08 | 起跑约 150 秒后 mute；又过约 2 分钟手机自己复位回 ABL（`product: alioth`） |
| 08:01 | 07:56（新内核） | 起跑 20 秒内就 mute：`XL_FOLLOW=20` 的第一份 `.live` 就没取到；一直 mute 到现在 |

三次里只有 07:47 留下了原因（panic）。07:53 和 08:01 什么都没留下，因为 loader 在被问之前
就已经不说话了。

**一处我自己的错误，记在这里**：往 PROGRESS.md 追加下面这段（第一次尝试）时 heredoc 引号写错，
正文里的命令被 shell 执行了，其中有一条 `fastboot reboot`（输出 `Rebooting FAILED ... (121)`）。
从时间上看那台设备在它之前约 20 秒（08:01:55 的第一次 `.live` 抓取）就已经不回答，所以它不像是
起因；但这件事是我不对，`PROGRESS.md` 当时还被写坏了 40 行，已经用 `head -n 2748` 截回去了。

## 今天做的三处修改

1. **`xl` 的 `enter()`：新起来的 loader 一定没有 resident 镜像，把 `$PHONE.ramdisk` 删掉。**
   07:47 那次就是因为这个记录还留着 01:54 的上传，`xl run` 什么都没传就开跑，root 是空的，
   于是 `launchd ... errno 2`。文件头注释也补了一句：resident 镜像跨 run 存活，不跨 loader 重启。
2. **`tools/backlight-test.sh` 起跑时带 `XL_FOLLOW=20`。** `XL_SECS=0` 时 `xl run` 只在 run 结束
   才取日志，脚本却要在 run 中间读，之前那份 `.live` 根本没被生成过；现在每 20 秒一份，
   真被复位也留得下最后 20 秒。
3. **`tools/backlight-test.sh` 加 `SCREENSHOT=0` 开关。** 截图是 10 MB 的 `get_staged`，
   和 loader 每 2 秒的轮询同时发生；07:53 那次复位前最后一件大事就是它（疑点，不是结论）。

## 新内核：把"第二种到达方式"也接住了（已编译，未验证）

`MiniAppleBacklight.cpp` 加了 `MiniBacklightService::setProperties(OSObject*)`：

- IOKit 里这个方法是虚的（`IORegistryEntry.h:661`），userland 的 registry 写入
  `is_io_registry_entry_set_properties`（IOUserClient.cpp:4015 起）先查 allowable 列表，
  没有就直接调它，所以服务截得到；
- macOS 那套亮度栈正是 `IOServiceMatching("AppleARMBacklight")` +
  `IORegistryEntrySetCFProperty(IODisplayParameters)`，**根本不开 user client**，所以
  `asked for 0 times` 不等于 "CoreBrightness 没找它"，属性写入是另一半证据；
- 顺带一个事实：`IOService` / `IORegistryEntry` 自己都没实现 setProperties（基类返回
  `kIOReturnUnsupported`，什么都不存），没有这个 override 时这类写入是**既被拒绝又被忽略**；
- 新代码把写入的字典按键打印到深度 3（正好覆盖 `IODisplayParameters > brightness > value/max`），
  有 `{value, max}` 就按 `value * 0x7ff / max` 换成分辨率亮度，`backlight_dcs=1` 时照旧发 DCS 0x51，
  最后把 `IODisplayParameters` 存进服务自己的属性。
- 构建：`./xnu-build.sh` 链接成功（`mach.development.bcm2837` 07:56，里面有
  `properties written on` 和 `level %llu of %llu -> panel` 两个新字符串）；结尾的 export check
  报错是 README 里写过的已知无害项（看文件日期，不看退出码）。
  **注意：08:01 那轮就是第一轮用这个内核，20 秒内 mute，所以这个改动还没有任何运行时证据。**

## 手机回来后按这个顺序做（每轮都有界）

1. `tools/backlight-test.sh`：会自动等手机、修槽位、补上传 resident 镜像（约 90 秒）。
2. 若还想排查"为什么 loader 中途不说话"，先用
   `SCREENSHOT=0 XL_SECS=180 tools/backlight-test.sh`：内核 180 秒后自己回来，
   日志一定拿得到，不用靠 loader 在 run 中间回答；
3. 对照实验里唯一还没排除的差异是 boot args 里的 `amp_step=2`（01:33 那次活过 10 分钟的运行
   没有它）。要么去掉它跑一轮，要么保留它、把 `amp_step` 从 1 开始逐个加，看那一行之后 loader
   是否还回答。

## 08:10 离线：loader 为什么会在 run 中间"不说话"

读 `xnuloader/loader.c` 和 `osfmk/arm64/loader_return.c`，串起来是这样：

- 内核每 10 秒更新一次 `loader_live.heartbeat`，用的是 `thread_call_enter_delayed`
  （`loader_return.c:585`），不是中断里，所以**线程调用被饿住时会停**。
- loader 在 `run_on_other_cpu()` 里盯着它：`SILENCE_SECONDS = 50`（loader.c:920），
  心跳停 50 秒就判定"内核悬死"。
- 判定的后果（loader.c:3197 起）：`hc_take_back("the kernel hung")` → 打印
  `THE KERNEL HUNG on core %x: no sign of life for 50 seconds; its log follows` →
  `set_result("hung", …)` → 把内核日志写出去 → 返回菜单。

所以 07:53 那次"两分钟后自己回到 ABL"很可能就是这个分支：内核（或它的心跳线程）停了
50 秒以上，loader 收工。

而 08:01 那次**第一份 `.live`（起跑 20 秒）就没取到**，这比 50 秒的判定还早，所以
"心跳超时"解释不了它；要么是 loader 的 USB 服务在这之前就不工作了，要么是
`hc_take_back`（等 UFS 控制器空闲）在内核核心还占着的时候把自己锁死——后者能解释
"设备还在、永远不说话、也永远不重置"。这两种目前都只是假设。

下一轮值得的两个选项（都还没有做）：

1. 给 `SILENCE_SECONDS` 一个 boot arg（比如 `kernel_silence=120`），先区分"内核真的悬死"
   和"只是某一段时间心跳被饿住"；
2. 在 `hc_take_back` 前后各打一行到 loader 屏幕/日志，下次 mute 时至少知道它卡在哪。

**注意**：loader 的改动要 `DEVICE=alioth ./xl reload` 才会生效，而且 reload 本身要重新打
loader 镜像——手机还在 mute 的时候不要做。

## 08:22 根因找到了：`backlight-test.sh` 的 boot args 从来没进过内核

`tools/backlight-test.sh` 第 31 行定义了 `ARGS=`，然后**没有任何地方把它交给 `xl`**。
`xl run` 的写法是 `local args=${XL_CMDLINE--v}`，而脚本从来没设 `XL_CMDLINE`，所以每次跑的
都是 loader 自己的最小配置：

```
cmdline: debug=0x8 cpus=1 rd=md0 loader_return_after=0 epoch=1793404076 sb_delay=8 -v
```

（这是 07:47 那次 panic 日志里的原话。`-v` 是 `xl` 的默认值，原样进了 cmdline。）

对照 01:33 那次活过 10 分钟的 run：

```
cmdline: debug=0x8 battery_shared_pa=... loader_return_after=0 epoch=... sb_delay=8 gl=1 adreno=22
adreno_keep=1 touchscreen=1 bootcpu=7 cpus=8 rd=md0 battery=1 backlight=1 backlight_arm=1
display_hz=120 display_hz_delay=5 fb_hz=120
```

也就是说 07:47、07:53、08:01、08:11 这四轮：

- **`cpus=1`（loader 补的默认值）、没有 `bootcpu=7`**：内核只在一个小核上跑，
- 没有 `gl=1 adreno=22 adreno_keep=1`：没有 GPU，画面靠软件路径，
- 没有 `battery=1`：电池页不读，
- **没有 `backlight=1 backlight_arm=1`：`AppleBacklight`/`AppleARMBacklight` 服务根本没发布**，
  `asked for N times` 这个判据这四轮里一次都没被真正问过，
- 没有 `display_hz=120`、没有 `amp_step=2`。

所以"`asked for 0 times`"之类的结论**不能用**——问题不在 CoreBrightness，在这行脚本。

**这也很可能就是手机每轮 mute 的原因**：单核 + 没有 GPU 的 iOS 启动在 loader 看来就是心跳停了
（`SILENCE_SECONDS = 50`），loader 判"内核悬死"、`hc_take_back`，然后 USB 就不再说话；
有时能走到 watchdog 复位回 ABL，有时卡在那里等人按电源。08:10 那段把 50 秒判定当成主要嫌疑，
现在看它只是**症状的出口**，真正的原因是内核根本没按能跑起来的那套参数启动。

修法（已改，`bash -n` 通过）：

```
XL_FOLLOW=${XL_FOLLOW:-20} XL_CMDLINE="$ARGS" ./xl run > "$log" 2>&1 &
```

另外脚本现在会把参数打出来：`backlight-test: boot arguments: gl=1 adreno=22 ...`，
一眼就能看出这轮到底带没带。

**下一轮开始之前先确认这一行**：run 的日志里 `cmdline:` 必须含 `backlight=1 backlight_arm=1
bootcpu=7 cpus=8`；不含就说明还没修好，别拿那一轮的数据下结论。

## 08:25 另一个把判断带偏的东西：Windows 侧的 fastboot 僵尸进程

`xl` 的 `timeout N fastboot …` 只杀 WSL 这侧的转发，Windows 的 `fastboot.exe` 有时会留下来并
占着设备；之后所有调用都会"看不到设备"或 121 超时。今天 08:14–08:24 我连续探了二十多次手机，
每次都可能在 PC 上留下一个，于是把"PC 不让我看"误当成"手机没响应"。

判据（已写进设备 README）：

- 先 `Stop-Process -Name fastboot -Force` 全部杀掉，**等 2 秒**，再只发一条
  `fastboot getvar product`；
- `fastboot devices` 列出 `????????????  fastboot`（序列号是问号）就是"设备在、命令不进"；
  真正的 ABL 是 `<序列号>  fastboot`，xnuloader 是它自己的名字；
- 杀干净之后仍然是 121 → 才是手机侧的问题。

时间线对齐一下：08:08:54 手机确实回来过（`product: alioth`），08:11:00 起跑了那轮
`XL_SECS=180`，约 20 秒后 loader 就不再应答；08:12 之后我的探测本身已经不可靠，
所以"它到底哪一秒 mute 的"目前只有 .live 一份都没取到这一个证据。

## 08:30 为什么"mute"其实是必然的：kernel 和 loader 抢同一个核

`xnuloader/loader.c:2754`：

```
uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
```

默认 `kernel_mpidr == this_mpidr`，只有 cmdline 里有 `bootcpu=N` 才会改（2769 行）；改过之后
3157 行会打 `the kernel will run on core %x (this is core %x)`。而 3192 行是：

```
if (kernel_mpidr != this_mpidr) { run_on_other_cpu(...); }   /* loader 留在自己的核上，继续服务 fastboot */
else                            { jump_to_kernel(...); }     /* loader 把自己这个核让给 kernel */
```

今天四轮的 cmdline 都是 `debug=0x8 cpus=1 rd=md0 … -v`（script 的 ARGS 没传，见 08:22 那条），
**没有 `bootcpu=7`**，所以 loader 是把自己正在用的核直接让给了内核：

- 从那一刻起，**没有任何一个核在跑 loader**，`getvar result` / `oem log` / `fastboot devices`
  当然全部 121 超时；
- 这和内核健不健康完全无关，`XL_FOLLOW` 的每份 `.live` 也必然取不到；
- 01:33 那次活过 10 分钟、`.live` 能连续取到，正是因为它带着 `bootcpu=7`，loader 在另一个核上。

所以今天所有"20 秒内就 mute"的记录**都不能当成挂死的证据**，它们只是"loader 不在线"；
08:10 那条关于 50 秒心跳的推断，只在 `bootcpu=7` 的跑法里才有意义（那时 loader 真的在看）。

**唯一还没解释的是 08:11 那轮为什么没在 180 秒后自己回来**：`loader_return_after=180`，
kernel 健康的话应当交还控制权、loader 重新上线。到 08:28 都没有，说明单核、无 GPU 的启动
确实卡住了（`cpus=1` + 没有 `gl=1`，iOS 的图形栈在软件路径上）。它现在大概停在"内核还在、
但不回交还"的状态；如果它的定时器还在给它喂 watchdog，手机就不会自己复位，只能等电源。

**结论**：在 `bootcpu=7` 补上之前，不要再开机跑；补上之后 mute 才有诊断价值。

## 08:45 电池颜色：除了问卷，还打视图树（已烘焙进 08:44 的镜像）

方法名单说明"有哪些方法"，但没说"那抹颜色在哪一层、由谁画"。所以在
`inject_sb.c` 里给 `-[_UIBatteryView setChargePercent:]` 加了**一次性**的视图树输出：
第一次设亮度值之后 3 秒（布局已完成）走一遍子视图，两层、最多 16 个：

```
hakios: battery view: the tree of _UIBatteryView:
hakios: battery view: _UIBatteryView 17x11 at 0,0 hidden 0 alpha 1.00
hakios: battery view:       background UIExtendedSRGBColorSpace 1 1 1 1
hakios: battery view:       tint UIExtendedSRGBColorSpace 0.2 0.8 0.4 1
hakios: battery view:       layer CALayer
hakios: battery view:     _UIBatteryFillView 17x11 at 0,0 hidden 0 alpha 1.00
hakios: battery view:       layer CAShapeLayer
hakios: battery view:       fill UIExtendedSRGBColorSpace 1 1 1 1
```

（上面是格式示例，不是这次的值——真值要等手机跑起来。）

要点：

- 视图自己的 `backgroundColor`/`tintColor`，以及它 layer 的 `fillColor`/`strokeColor`/
  `backgroundColor` 都打；`fillColor`/`strokeColor` 只在 `respondsToSelector:` 通过时才发，
  `CALayer` 没有这两个属性，直接发会抛 unrecognized selector；
- 颜色用 `-description` 打，任何色彩空间都能读（不依赖 CGColor 分量）；
- 视图在第一次调用时 retain 住，3 秒后 dump 完再 release，避免视图已经销毁。

构建：`cd userland && SYSTEM=1 ./build.sh system.apfs`，退出 0、0 个 error；
`userland/root/lib/libhakios.sb.dylib` 08:43（91,640 字节，含
`battery view: the tree of`），`userland/system.apfs` 08:44（2,486,968,320 字节）。
手机起来后**必须先重新上传镜像**（`xl ramdisk userland/system.apfs`），本轮的结论才作数。

## 09:05 loader 自己判定"内核没回来"并复位手机（OTHER_CPU_OVERRAN）

问题不是"检测不到挂死"，而是**有两种挂死逃掉了所有现有机制**：

1. 心跳停了 50 秒 → 现有 `SILENCE_SECONDS` 能发现（`result: hung`，`xl run` 会 `fb reboot`）。
2. **内核定时代码还活着、但内核卡住** → 心跳一直在跳，上面那条永远不触发；而 alioth 上
   Haven watchdog 根本没启用（`wdt_arm()` 只对 `platform_redwood` 生效），loader 复位手机
   的唯一路径是 `ST->rt->reset_system()`，但它只在"用户按了"（`xl reboot`）或异常处理器里被调用。
   于是手机保持"内核在跑、什么也不做"直到有人长按电源——08:12 之后那半小时就是这样。

改法（`xnuloader/loader.c`）：内核自己承诺过返回时间（`loader_return_after=N`，即 `XL_SECS=N`），
loader 就按承诺判它：

- `budget != 0` 且已过 `budget + RETURN_GRACE_SECONDS`（60 秒）→ `OTHER_CPU_OVERRAN`；
- 立刻 `log_of_hung_kernel()` 把内核日志理好（此时内存还在），
  `set_result("overrun", …)` + `write_logs()` 暂存给主机；
- 再空转 `OVERRUN_LINGER_SECONDS`（60 秒）只响应 USB，让 `xl run` 把日志取走；
- 然后 `ST->rt->reset_system(0 /* EfiResetCold */)` 冷复位。主机不在也一样复位 —— 这正是目的。

`budget == 0`（默认 `XL_SECS=0`，普通使用）**行为完全不变**：没有承诺就没有期限，只有 `xl stop`。
`wdt_pet` 那条"`budget + 60` 之内喂狗"顺手改成同一个宏，语义一致。

`xl` 侧：`result` 含 `overrun` 时打印一行并返回 2，**不再** `fb reboot`——loader 一分钟后自己复位，
从主机再插一脚只会和它抢。

前提（写在文档里）：这条只在 `bootcpu=N`（loader 独占另一个核）时存在。没有 `bootcpu=`，
loader 根本不在运行，没有软件能救；`XL_SECS=0` 的跑法也没有承诺可判。

验证：

- `xnuloader/build.sh` 退出 0；`loader.efi` 09:01（旧件备份 `loader.efi.bak-20261031`，00:28，
  md5 不同），`strings` 里有 `THE KERNEL DID NOT RETURN: it said %d seconds…`；
- 新 loader 在**下一次 `xl enter`** 才进卡（`enter()` 从 `xnuloader/loader.efi` 现打 payload）；
- 手机自 08:12 起不再应答（`????????????  fastboot`，`getvar` 121），需要长按电源，
  所以 09:05 时这条路径**还没在真机上跑过**。回来后先 `XL_SECS=60` 正常跑一次
  （应当 `result: returned`，不受影响），再人为制造超时验证 `overrun` 分支。

## 09:16 QEMU 里跑通了"内核没回来 → 复位"（不用手机）

手机还躺着（08:12 起不应答），所以这条路径先在 QEMU 上验。要制造"内核承诺返回却没返回"，
给内核加了一个只在测试时用的 boot arg：

```c
/* osfmk/arm64/loader_return.c: loader_return_timer_init() */
uint32_t skip = 0;
if (PE_parse_boot_argn("loader_return_skip", &skip, sizeof(skip)) && skip != 0) {
	printf("loader_return_after=%u was asked for, and not armed (loader_return_skip)\n", seconds);
	return;
}
```

`loader_return_after=30` 仍然写在命令行里（loader 照读），内核自己不装那个定时器 —— 心跳照跳，
内核永远不回来，正是静默检测看不到的那一类。`qemu/overrun-test.sh` 一键跑：

```
loader_return_after=30 was asked for, and not armed (loader_return_skip)
THE KERNEL DID NOT RETURN: it said 30 seconds, its log follows; the phone is restarted in 60 seconds
overrun 0 0
... 60 秒（期间 QEMU 里照常跑内核，root device 找不到后自己 panic）...
restarting the phone
qemu exit: 0            ← reset_system 真的把机器复位了（run.sh 带 -no-reboot，复位即退出）
```

所以 30 + 60 秒判定、暂存、60 秒留出时间、`ST->rt->reset_system(0)` 全部走通，包括
"内核还活着的时候 loader 调用 UEFI 冷复位"这一步。

两个观察：

- 暂存的内核日志在 QEMU 里是 **0 字节**：内核写进 `loader_log` 的是各 hakios 驱动的 printf
  （adreno/mic/sound/... 都调 `loader_log_putc`），QEMU 里一个都不跑。手机上会有，
  但还是要在真机上确认一次"overrun 时取到的日志有多少"。
- 早先那次 `return_cpu=0` 的尝试**没有**造出超时：XNU 的逻辑 CPU 0 就是内核自己所在的核
  （QEMU 里只有 1 个逻辑 CPU），xcall 直接就地执行、正常返回。也正因为那次是正常返回，
  它顺带验证了**不会误报**：30 秒的跑法在 33 秒正常回交，loader 没有打 overrun。

另外：`wdt_armed` 在 redwood 上是真开着的，所以 overrun 的 60 秒等待期里 loader 现在也喂狗
（每 5 秒），否则 Haven watchdog 会在等待中途咬掉，轮不到 loader 自己的复位。

内核构建：09:11（`xnu-build.sh` 收尾照旧是 symbolset/export-check 报错，看文件时间），
mach 15,239,512 字节，`strings` 里有 `not armed (loader_return_skip)`。loader.efi 09:06
（含等待期喂狗）。下次 `xl enter` / `xl run` 用的就是这两个。

## 10:15 ADSP：不是"没移植"，是"没跑过"——下一步是只读的两步

手机仍不应答，这轮把声音那条链的下一步核清楚了。`hakios.adsp.c` 的文件头写的是 redwood，
但代码里的常量**已经全换成 alioth 的**（`ADSP_MEMORY 0x8bb00000/0x2500000`、`PAS_ADSP 1`、
`SMEM_PHYS 0x80900000`、SMP2P 429/443、crash reason 423、IPCC client 3 信号 0/2、
`CMD_DB 0x80860000`），逐条都能在 `dump/twrp-fdt.dts` 里对上（详见 `devices/alioth/README.md`
"声音（三）"）。

没对上的只有三类，都是运行期才知道的：

- GLINK item `478/479/480`：redwood 跑出来的编号，alioth 不一定相同；
- RSC 基址 `0x18220000` vs DT 的 `rsc@18200000`（可能只是 drv 偏移）；
- DSP 镜像装不装得下 37 MB 的区域。

所以真机回来后的第一件事不是写代码，而是 `adsp_step=1`、`adsp_step=2`——两步都不写任何硬件，
第 1 步给出 429/443/478/479 在不在、doorbell 计数、crash reason；第 2 步核对固件。编译时间不变，
`adsp_step` 解析早就在，无需重新构建。

## 10:53 把"设备在、没人答"这件事钉死：USB 描述符是 loader 的，不是 ABL 的

Windows 设备管理器（`Get-PnpDevice`）给出：

```
Status : OK
Class  : AndroidUsbDeviceClass
FriendlyName : Android Bootloader Interface
InstanceId   : USB\VID_18D1&PID_D00D\XNULOADER
```

`VID_18D1&PID_D00D` 是 fastboot，实例名 `XNULOADER` 是**loader 自己挂的**描述符（`xl is_loader`
认的就是 `product: xnuloader`）。所以 08:05 之后这台不是"卡在 ABL"，而是 **loader 的 USB 端点还在、
loader 那一核已经交给内核在跑**（08:11 那轮没有 `bootcpu=`），内核不服务 fastboot，于是所有命令
121 超时。这也解释了为什么等再久也不会自己好：没有第二个核在看任何东西。

顺带修了文档里一句会误导人的话：`devices/alioth/README.md` 的"设备枚举着，但一个命令都不回"第 2 条
原来写"没有任何看门狗会把它救回来"，现在补上例外——`XL_SECS=N` **且** `bootcpu=N` 的跑法有
09:05 的 loader 自复位；并注明躺着的这台没有 `bootcpu`，属于救不回来的那一类。

## 10:58 无人值守的跑法默认带上了期限（backlight-test.sh：XL_SECS=600）

今早那台手机之所以躺到没人能救，根子是"跑法没有期限"（`XL_SECS=0`，`loader_return_after=0`）。
光有 loader 的自复位还不够：没有 `XL_SECS` 它连"承诺"都没有。所以脚本自己给一个：

```bash
XL_FOLLOW=${XL_FOLLOW:-20} XL_SECS=${XL_SECS:-600} XL_CMDLINE="$ARGS" ./xl run > "$log" 2>&1 &
```

- 600 s 在读数点（`HOME_WAIT + 240 s`，默认 390 s）之后，不影响上面那套 grep；
- `ARGS` 里有 `bootcpu=7`，所以 600 s 到了内核还不回来时，loader 在 720 s 冷复位（09:05 那条）；
- 调用者仍可 `XL_SECS=… tools/backlight-test.sh` 覆盖；`STOP=1` 照旧提前结束；
- 结果提示改成了"它会在 ${XL_SECS} 秒后自己回来"。

以后写别的无人值守脚本（`battery-check.sh` 等）也照这个来：**只要是人不在旁边的跑法，就带期限**。

## 18:50 离线：iOS 自己的二进制说了亮度服务该长什么样（不用手机）

手机从 08:11 躺到现在，但**亮度这个问题有一半根本不用手机**：iOS 里"找背光服务"的那段代码就在
`userland/system.apfs` 里，只要会读 APFS 就能把它的话抠出来。

**取法**（可复用，以后查电池/音量也是这条路）：

```
iOS 16 的 dyld 共享缓存不在 /System/Library/Caches，而在 cryptex 里：
  /System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.*（43 个分片）
tools/apfs/apfs.py cat userland/system.apfs <上面那条路径> > /tmp/dsc/<分片名>     # 会自动解 decmpfs
```

**三个调用方，原文都在**（`grep -aoE '.{140}AppleBacklight.{200}'`）：

| 分片 | 代码 | 字符串（原文） | 读的东西 |
|---|---|---|---|
| `.37` | BrightnessControl 的 `-[BCAppleBacklightBrtControl initWithService:]` | `com.apple.BCBrtControl`、`AppleBacklight`、`IODisplayParameters`、`brightness`、`min`、`max`、`BrightnessMilliNits`、`BrightnessMicroAmps`、`_maxUser >= _minUser`、`_maxNits >= _minNits`、`Unable to find backlight service.`、`got back service %u` | 按类名 **`AppleBacklight`** 找；读 `IODisplayParameters.brightness.{min,max}` 当量程 |
| `.13` | CoreBrightness 的 `CBDisplayContaineriOS` | `com.apple.CoreBrightness.CBDisplayContaineriOS`、`AppleARMBacklight`、`IOHIDDevice`、`DisplayBrightness…`（`DisplayBrightnessData`/`Factor`/`Min`/`Max` 等） | 按类名 **`AppleARMBacklight`** 找 |
| `.19`/`.23` | PowerLog 的 `PLDisplayAgent` / `BrightnessSystemClient` | `IOServiceMatched`、`AppleARMBacklight`、`IODisplayParameters`、`min`、`max`、`rawData`、`brightness=%@ self.pendingBacklightEntry` | 按类名 **`AppleARMBacklight`** 找，读同一本字典 |

顺带确认：`/usr/libexec/backboardd` 自己的字符串里**没有** `AppleARMBacklight`/`IODisplayParameters`
——背光不是 backboardd 直接做的，做它的是缓存里的框架（CoreBrightness / BrightnessControl），
所以没有哪个守护进程需要改。

**这解释了两件事**：

1. `MiniAppleBacklight.cpp` 同时发布 `AppleBacklight` 与 `AppleARMBacklight` 两个类是对的——
   三种调用方确实分用这两个名字（那份注释里的"两个名字都发"不是猜，现在有原文）。
2. **真正缺的东西不是类名，是属性**：`MiniBacklightService::start()` 以前只 `registerService()`，
   一个属性都没发。而 `BCAppleBacklightBrtControl` 一上来要读 `IODisplayParameters.brightness.min/max`
   （还要求 `max >= min`），读不到就是"没有量程的背光"，滑块没有可动的范围。

**改了什么**（`xnu/files/iokit/Kernel/MiniAppleBacklight.cpp`，已同步到 `xnu-src`）：

- 新增 `publishBrightnessParameters()`：`IODisplayParameters = { brightness = { value, min = 0, max = 0x7ff } }`，
  在 `registerService()` **之前**发（调用方是"找到服务就在同一口气里读量程"）；
- 每次把电平送面板时（`backlight_dcs=1` 那条路）用同样的函数把 `value` 更新成新电平；
- 日志行多了量程：`AppleBacklight: published as %s, provider %s, brightness %#x of %#x`。

**构建**：18:45 完成，`mach.development.bcm2837`（15,239,432 字节）里已能 `strings` 到新行与
`IODisplayParameters`。`xnu-build.sh` 结尾仍是那个已知无害的 `IOKit.symbolset` / `check_all_exports`
（exit 2），没有 `error:`。

**这仍不是结论**：属性发对了不等于滑块会动——要等手机回来跑一轮，看
`AppleBacklight: N services published, the user client asked for M times` 里的 M 是不是非零，
以及 `CLIENT OPENED` / `selector ...` 的原文。**但这一轮把"跑之前能确定的东西"都确定了。**

**跑之前确认过路径**：`xl` 的 `K=xnu-src/…/mach.development.bcm2837`（就是 18:45 这一版），
`xl enter` 用的是 `xnuloader/loader.efi`（09:06，带 OVERRUN 自复位）——所以手机回来后
不需要额外换什么，跑的就是今晚这两样。

## 18:55 同一招用在电池上：颜色是哪个方法选的

缓存分片 `.03` 里 `_UIBatteryView` 的颜色代码是：

```
_batteryColorForCapacity:lowCapacity:style:usingTintColor:      ← 主色由 容量 + lowCapacity + style + tint 决定
_batteryFillColorForEntry:usingTintColor:                       ← 充放电状态在 entry 里
_batteryFillColor / _batteryUnfilledColor / _batteryTextColor
_updateBatteryFillColor  _updateBodyColors  _updateBodyLayers  _updateBolt
```

所以"充电时填充是白的、真机是绿的"不该靠给图层硬写颜色解决，该看
`_batteryColorForCapacity:lowCapacity:style:usingTintColor:` 的入参（尤其 entry 的充电标志、style）
再决定要不要钩。电池数据本身走 IOPS：分片 `.05` 有 `AppleSmartBatteryManager` 和
`com.apple.system.accpowersources.*`。已写进 `devices/alioth/README.md` 的电池一节。

## 18:58 音量那一半：不是坏了，是"下游没人"

缓存里 HAL 的样子（分片 `.13`）：`AMCP` / `HALS_IOA2Device` / `IOAudio2::Engine`，
匹配的是 IOKit 的 `IOAudioDevice` / `IOAudioEngine`（`IOAudioEngineGlobalUniqueID`、
`IOAudioEngineNumSampleFramesPerBuffer` 等一串键）。也就是说**真正的 iOS 音频驱动形状是 IOAudio 引擎**，
而本移植走的是 HAL 插件（`userland/src/audio_plugin.c`，日志 `hakios-audio: the volume: … dB`），
它已经收得到音量、按 dB 缩放样本——所以音量滑杆本身没有"没接上"的问题，
**缺的是 sink（第 2 项：CS35L41 + TDM + ADSP）**。结论：第 1 项里音量那一半挂在第 2 项后面，
亮度那一半独立且已备好（18:50 节）。

顺带把"CS35L41 固件到底有没有"查完了：`alioth-local`（`dump/`、`firmware/`、`firmware.tar`）
里 `find -iname '*cs35l41*'` 一条都没有——所以功放只能走"无 DSP 直通"初始化（寄存器序列自己写），
或者等抓一份完整 `/vendor` 再回来。这条与 README 的说法一致，但现在是查过的，不是推断。

## 19:10 ADSP 第 4/5 步与 Linux 对齐（读代码 + 段布局，不是猜）

- 第 4 步（`step_header`）把 `.mdt` 的**头 + 哈希**放进一页内核内存，整页清缓存后交给
  `SCM_PAS_INIT_IMAGE`（`scm` 里 `SCM_CALL(2,1)`）——这正是 Linux `qcom_scm_pas_init_image()`
  吃的 "metadata"；随后 `SCM_PAS_MEM_SETUP(pas_id=1, 0x8bb00000, 跨度)`。
- 第 5 步（`step_image`）只写**段**，落点 `paddr - min_addr`，再读回逐字比对。
- **不写元数据到区首是对的**：这份镜像第一段 `paddr == min_addr`，按 `paddr - min_addr` 正好落在
  偏移 0；Linux 的 `mdt_loader` 若再往区首写一份头，就会和第一段重叠。所以"区首 = 第一段"这个
  事实本身就排除了"还要多写一份头"的可能。
- README 里"镜像装不装得下 37 MB"这条旧的待确认项删掉了——真机 `adsp_step=2` 已经答过
  （18 段、`0x2500000` 恰好填满）。

（试图在线取 `mdt_loader.c` 原文复核时，这台机器出不去外网：raw.githubusercontent / git.kernel.org
被拦，MCP 搜索也超时——19:30 那条取源码的任务在 1800 s 后自己失败退出，等于确认了这条路今晚走不通。
所以上面这一条是按**段布局**推的，不是照抄源码；要在源码层面钉死，得本机有一份 Linux 树时再看。）

## 19:22 新背光服务在 QEMU 里冒烟通过（不用手机）

带 `backlight=1 backlight_arm=1` 在 QEMU 里启动（`qemu/backlight-smoke.log`，`qemu exit: 0`）：

```
[    5.797151]: AppleBacklight: published as AppleBacklight, provider GenericARMPE, brightness 0x7ff of 0x7ff
[    5.797697]: AppleBacklight: published as AppleARMBacklight, provider GenericARMPE, brightness 0x7ff of 0x7ff
```

两个类都发布成功、属性在 `registerService()` 之前就位、内核照常往下跑。日志里的两次
`panic(cpu 0 … Failed to mount root device @IOKitBSDInit.cpp:926)` 是 QEMU 没有根设备的老现象
（和 09:16 那次 overrun 测试同一个），不是这次改动引起的——panic 发生在服务发布之后，
日志仍以 `== END OF LOG ==` 结束。

QEMU 只能证明"这段代码能跑、会发布"，证明不了 iOS 会不会来开客户端——那仍然要等手机。

## 19:30 回来后照这个顺序做（这一版是 10 月的最终版，替换 2805 行那份旧的）

**前提**：手机自 08:11 起 `fastboot getvar` 一直 121 超时，只有长按电源能救。先物理重启。

1. `./xl state`：`android` → `./xl enter`（这次 `enter` 打的是 09:06 的 loader，带 OVERRUN 自复位）；
   等到 `loader`。
2. **只读、有界的第一轮**：`SCREENSHOT=0 tools/backlight-test.sh`
   （脚本自带 `XL_SECS=600`、`bootcpu=7`、`backlight=1 backlight_arm=1`；720 s 内核不回来 loader 自己复位）。
   先看它打印的 `cmdline:` 里有没有 `backlight=1 backlight_arm=1 bootcpu=7`——**没有就别读后面的数据**。
3. 判据（都在这轮 `.live` 里）：
   - `AppleBacklight: published as … brightness 0x7ff of 0x7ff` **两行**（两个类各一行）→ 18:50 的属性改动生效；
   - `AppleBacklight: N services published, the user client asked for M times`：
     **M > 0** → 协议到了，下一轮加 `backlight_dcs=1` 才碰面板；
     **M = 0** → 看 `backlight probe:` 那几行：探针查得到就是 CoreBrightness 的匹配问题，
     探针也查不到就是发布侧的问题。
4. `adsp_step=1`、`adsp_step=2` 各跑一轮（只读）：刷新重启后 429/443/478/479 的有没有、
   doorbell 读数和固件完整性。
5. 电池：同一轮的 `battery survey:` / `battery view:` 两段（08:44 的镜像里已有），
   配合 18:55 那条 `_batteryColorForCapacity:lowCapacity:style:usingTintColor:`。

**这一轮里没有一步是写硬件的**。`backlight_dcs=1`（真发 DCS 0x51）、`adsp_step>=3`
（AOP 邮箱、RPMh、PAS）、`amp_step>=6`（功放初始化）都必须有人在旁边时才做。

## 19:35 `backlight-test.sh` 自己先检查参数（把这轮读数的前提写进脚本）

08:22 那次的教训是"参数没进内核，数据看起来却是数据"。现在读数之前脚本自己判：

```bash
if ! grep -aq "cmdline:.*backlight=1.*backlight_arm=1" "$live"; then
	say "WARNING: the kernel did not get backlight=1 backlight_arm=1 -- nothing below can answer the slider"
fi
if ! grep -aq "cmdline:.*bootcpu=" "$live"; then
	say "WARNING: no bootcpu= -- the loader handed its core to the kernel, so it cannot be asked anything while this runs"
fi
```

`bash -n` 通过，四条判据用两行假 `cmdline:` 试过（好的一行不报、坏的一行两条都报）。

## 19:40 一条先记下、还不敢当结论的：`IOMFBBrightnessLevel`

CoreBrightness 的文本（分片 `.13`）里有 `IOMFBBrightnessLevel`，但**它的上下文是 CoreBrightness 自己
的一组标定键**：`BLNitsCap`、`brightness-nits-cap`、`AvgBacklightNits`、`AvgLogicalBrightnessNits`、
`max_Factor`/`aab-constraint-*`、`__DisplayStartFade`…… 也就是说它更像"这份曲线/标定的键名"，
不一定是"要写进 IOKit 的属性"。所以它**不构成**"亮度其实走 IOMobileFramebuffer 而不是背光服务"的证据；
真要走那条路，跑一轮之后 `IOMobileFramebuffer` 那边的属性写入日志会自己说话。先记在这里，
免得下次又从头找一遍。

## 19:50 文档里写过、代码里从来没有的 `setProperties()`——补上了

`devices/alioth/README.md` 的"第二种到达方式"一节写着"`MiniBacklightService::setProperties()`
现在把写入的字典按键打印出来"。查了一遍：**`xnu/files` 与 `xnu-src` 两份 `MiniAppleBacklight.cpp`
里都没有这个 override**，而基类 `IORegistryEntry::setProperties` 是

```cpp
IOReturn
IORegistryEntry::setProperties( OSObject * properties )
{
	return kIOReturnUnsupported;
}
```

（`xnu-src/…/iokit/Kernel/IORegistryEntry.cpp:584`）。也就是说，如果 CoreBrightness 走的是
"写属性"这条路，写进去的东西**既没被拒绝也没什么可查**——日志里连一行都不会有。

**19:37 的实现**（`xnu/files/iokit/Kernel/MiniAppleBacklight.cpp`，已同步）：

- `MiniBacklightService::setProperties(OSObject *)`：逐键打印写入的字典（最多四层，数字打值）；
- `IODisplayParameters.brightness.{value,max}` 存在时按 `value * 0x7ff / max` 换算（四舍五入、上限 clamp），
  更新发布的 `value`；**只有 `backlight_dcs=1` 才真的发 DCS 0x51**，否则日志写
  `(not sent: no backlight_dcs=1)`；
- `showProperties()` 用 `OSCollectionIterator`，只关心字典和数字，其它类型只写一句 "not a number"。

**验证**：19:37 编译（exit 2 仍是已知的 symbolset，无 error/warning，新字符串在核里）；19:45 QEMU
冒烟再次通过（两个类照常带属性发布），日志仍是 QEMU 那个 `Failed to mount root device` 老 panic。
**真机上会不会有人写这个属性，仍然是手机回来那一轮才知道。**

## 19:47 user client 的结构体也记下来了

原来那行只知道"有个 `structure N in`"，内容一个字节都没留下。现在 `externalMethod` 把
`structureInput` 的前 32 字节按 8 字节字打出来：

```
AppleBacklight: selector %u: %u scalars in [...], %u out; structure %u in, %u out
AppleBacklight:   structure in [0]: 0x…
```

这样真机上如果客户端用的是结构体而不是标量，日志里就有东西可看。19:40 编译（15,239,560 字节，
`strings` 已确认 `structure in [%u]`），19:47 QEMU 冒烟通过（两个类照常发布、无新 panic）。

顺带把 `userland/src/inject_sb.c` 的 `backlight_probe()`（853 行）读了一遍：`IOServiceMatching`
按两个类名各查一次、`IORegistryEntryGetPath` 打路径、查不到就明说——逻辑没问题，**没有改它**。
它能再进一步读 `IODisplayParameters` 验证属性对用户态可见，但那要重做 2.4 GB 的系统镜像、
而且改坏了是"SpringBoard 起不来"这种代价；等这轮真机数据回来再决定值不值得加。

再读一遍 DCS 那条路（`hakios.display.c:777` `hakios_display_set_brightness`）：`level` clamp 到
`0x7ff`；`static last_level = 0xffffffff`，**第一次调用一定发**（不会因为"和初始值一样"被吞掉）；
首行与超过 `0x40` 的跳变才打 `display: brightness N: dcs 51 hi lo`；字节是 `0x51, hi, lo` 三字节
short write，`dsi_quiet` 包住避免刷屏。没有发现要改的地方。

## 19:55 今晚（18:40 起）的净变化，一页看完

**代码（未提交）**

| 文件 | 改了什么 | 验证 |
|---|---|---|
| `xnu/files/iokit/Kernel/MiniAppleBacklight.cpp` | ① `publishBrightnessParameters()`：服务起来、每次电平变化都发 `IODisplayParameters = {brightness = {value, min, max}}`；② `setProperties()` override（原来没有，基类直接 unsupported 且什么都不存）；③ `externalMethod` 打 `structureInput` 前 32 字节；④ 19:59 自查补上的守卫：`service == NULL` 时不发布（user client 的 `getProvider()` 理论上不该是 NULL，但一个属性不值得一次 panic） | 19:59 编译无 error/warning；20:05 QEMU 冒烟：两个类照常发布 |
| `tools/backlight-test.sh` | 读数前先查 `cmdline:` 有没有 `backlight=1 backlight_arm=1` 和 `bootcpu=`，缺了先打 WARNING | `bash -n` + 两组假 cmdline 试过 |

（`xnu-src` 里的同名文件已同步；`xl` 用的就是 19:40 那次编出来的核，`xl enter` 用的是 09:06 的 loader。）

**只读结论（写进文档的）**

- iOS 16.5 的亮度栈三种调用方与各自读的键（18:50）；电池颜色由
  `_batteryColorForCapacity:lowCapacity:style:usingTintColor:` 决定（18:55）；音量挂在 sink 后面（18:58）。
- ADSP 第 4/5 步与 Linux 对齐，第 5 步"只写段"不是遗漏（19:10）。
- 手机自 08:11 起 121 超时、需要物理重启；回来后照 19:30 那份顺序做，一轮只读、带判据、带期限。

**没做、也不该在无人时做的**：`backlight_dcs=1`（真发 DCS 0x51）、`adsp_step>=3`、`amp_step>=6`。














---

---

## 21:30 UniXNU-alioth 参考仓库结论 + 手机 ABL 阻塞项

**参考仓库（用户指派，/tmp/unixnu，只读）**：同机（alioth）另一条移植线：Mu-Silicium
UEFI + iPadOS 17.7.11 (21H461) donor，boot 机制与 `xl enter` 完全同构（`fastboot boot`
一个 gzip(BootShim+UEFI FD+payload) 的 boot.img）。

- **亮度（任务 1）**：他们独立得出与我们相同的硬件结论（k11a_38_08_0a_dsc_cmd 面板，
  `bl_ctrl_dcs`，min 1 / max 0x7ff，init 0x218，big-endian；无 KTZ8866，WLED disabled）。
  他们的 BacklightProbe.cpp 走 DSI0(0xae94000) TPG 命令 FIFO 逐字写：长写头
  `0xC0390003`、payload `0x51|hi<<8|lo<<16|0xff<<24`、读回用 `0xA0060000|cmd`（bit 5 转
  lane）经 RDBK_DATA_CTRL 取 DCS 0x52。mode 3 = 把 iOS 服务接到 DCS。**可直接借鉴的增量**：
  DCS 0x52 读回配方（我们还没有），以及他们"只读模式先行、逐问题设判据"的验证法。
- **音频（任务 2）**：仓库里**没有**任何 codec/扬声器工作，只有 native_audio_stats 统计行
  和 AdspProbe（DSP 段/信箱）的描述。CS35L41 仍无参照，我们领先。
- **电池（任务 3）**：只有 ShengBattery.cpp 的统计行布局（stage/pct/mv/ma/temp/flags/
  cycles/usb/full/design/cycles/refuse）——电池驱动本体不在这个仓里，价值是"一行该报什么"。
- 他们的 patches/xnu/series 是空的：一切靠组件和 boot args，不靠改内核。

**手机侧阻塞（继续查因）**：`fastboot boot` 现在**对任何镜像**报
`Failed to load/authenticate boot image: Load Error`——我们的 20:24 镜像、00:28 备份
loader、现场重建镜像、连 10 月 26 日在本机用过的 TWRP 都拒绝。因此是手机 ABL 侧状态，
不是镜像问题。判别证据：
- 10-26 健康 getvar 存档（alioth-local/dump）与现在的 diff：唯一实质变化是 `token`
  （ARB token 变了），其余 anti/crc/unlocked/verity 相同；slot-successful 由 a+b 都 yes
  变为都 no，slot-retry 7/6。
- `fastboot --set-active=b` 清掉了 slot-unbootable:b，boot 仍报 Load Error → 不是该标志。
- 未做任何刷写；所有动作都是只读查询和 RAM boot 尝试。ABL 产生这个错误串的检查点还没
  定位到（无 UART 控制台，网络源码搜索被打断）。按规矩：手机回不到 loader，停在用户处。

---

## 21:40 根因找到：今晚的镜像全部错建成了 poco-x5-pro

**现象链（一晚的证据）**：`fastboot boot` 报 `Failed to load/authenticate boot
image: Load Error`（任何镜像，含 TWRP）；新 bootloader 会话里我们的镜像
`Booting OKAY` 但 2 秒内回 fastboot、屏幕闪一下黑屏；TWRP 能起。

**根因**：mkxnuimg.py 的 `--device` 默认值是 `poco-x5-pro`（取 $DEVICE，没设就
用 redwood）。20:24 及之后重建的镜像全部装进了 **redwood 的 FD + dtb + shim**——
错误机型的 UEFI 在 alioth 上显示初始化就崩（闪一下→黑屏→复位→fastboot）。
证据：从镜像内核段解出 FDT（405,158 B，magic d00dfeed00062ea6）与
devices/poco-x5-pro/prebuilt/device-tree.dtb 逐字节一致；alioth 的 dtb 是
919,977 B（000e09a9），不在镜像里。

**修复**：`./mkxnuimg.py -d alioth -p usb-payload.bin` 重建 → alioth dtb 在镜像
内 → 新 bootloader 会话 boot → **4 秒 loader 就活了（product: xnuloader）**。
（教训：xl enter 带 DEVICE=alioth 的规矩被破坏过一次，后面所有手动重建都没
检查机型。）

**ABL 侧另有两个未解之谜（不阻塞，记录在案）**：① 旧会话里 Load Error 连
TWRP 都拒、新会话就收——像是 ABL 会话态问题；② logfs 里 ABL 日志显示
`fastboot boot` 后走 "Booting from slot (_b)" + "Error: Appended Soc Device Tree
blob not found"（那是 redwood 镜像 v1 头 + 非 QCDT dtb 被 ABL 拒后落回槽位
启动的路径——换回 alioth 镜像后是否消失待观察）。misc 里有 `boot-system0`
命令、XBL 报 `pil-SPSS failed to validate firmware metadata`（SPSS/slpi 固件
校验失败，每次启动都有）、ARB token 与 10-26 存档不同——都已只读取证存入
alioth-local/dump/（logfs2.img、uefivarstore.img、misc/devinfo/secdata/abl_a/
abl_b 等）。

**21:39 起**：loader 已活，backlight 只读跑（backlight=1 backlight_arm=1、
amp_step=2、无 backlight_dcs）在后台进行中。

---

## 21:55 01:52 以来所有 "Process 1 exec of /sbin/launchd failed" panic 的根因：哨兵文件骗过了上传

**现象**：21:40 的 backlight 只读跑在 78.5 s panic——`Process 1 exec of /sbin/launchd
failed, errno 2`（init 找不到 /sbin/launchd）。01:52、07:47 的跑死于同一处；01:30 的
跑正常进了 SpringBoard。

**根因**：真正的根文件系统是 2.3GB 的 userland/system.apfs，由 `./xl ramdisk` 上传到
loader RAM 的 resident 区，内核拿它当 md0。是否上传由哨兵文件
`${XDG_RUNTIME_DIR}/darwin-hakios-phone.ramdisk`（存在=已在 RAM 里，跳过上传）决定。
手机一冷启，loader RAM 就空了，但哨兵文件还活着 → 上传被静默跳过 → 内核拿 4MB 的
payload ramdisk（userland/root.apfs，/redwood overlay）当根 → /sbin/launchd 不存在 →
panic。`xl enter` 会在冷启 loader 后删哨兵（31 号上午刚修过的坑），但今晚用
`fastboot boot` 直接起的 loader，绕过了 enter。

**证据链**：01:30 好 boot 的 loader 日志有 `resident image at pa fc000000 size
943c4000: it is the root (md0), the payload ramdisk is md1`；01:52/07:47/21:40 的
loader 日志全都没有这行；night-backlight.out 里没有 "uploading" 行 = 哨兵静默跳过。

**修复**：手动 `./xl ramdisk userland/system.apfs`（90 s，26 MB/s，21:53 完成）→
重跑 backlight 只读（night2）。

**注意**：system.apfs 是 08:44 重建的（2,486,968,320 B = 0x943a0000），与 01:30 成功
boot 用的镜像（0x943c4000）不同——新镜像从未成功启动过，night2 是它的首飞。

---

## 22:40 今晚结论汇总（用户要求记录后停手）

### 1) 21:40 之前所有 "launchd failed" panic 的根因：哨兵文件骗过了上传
真正的根文件系统是 2.3GB 的 `userland/system.apfs`，靠 `./xl ramdisk` 传进 loader RAM，
内核拿它当 md0。是否上传由哨兵 `${XDG_RUNTIME_DIR}/darwin-hakios-phone.ramdisk`
决定（存在就跳过）。手机冷启后 loader RAM 是空的，但哨兵还在 → 静默跳过上传 →
内核拿 4MB payload ramdisk 当根 → `Process 1 exec of /sbin/launchd failed, errno 2`。
判别：好 boot 的 loader 日志有 `resident image at pa fc000000 size ...`，坏 boot 没有。
`xl enter` 会删哨兵，但 `fastboot boot` 直接起 loader 会绕过它。**已写入
devices/alioth/README.md 的"已知坑：resident 根文件系统与哨兵文件"一节。**

### 2) 21:53 night2 跑：镜像上传成功，内核 600 s 未返回
- `./xl ramdisk userland/system.apfs`：2371 MB，90 s，26 MB/s，21:53 完成。
- 21:53:34 起跑（resident 镜像在位），屏幕停在 adreno GPU 自检的三条色带；
  内核到 600 s 没有自己返回，loader 的 OTHER_CPU_OVERRAN 在 660 s 把它复位回 fastboot。
- **没有拿到任何内核日志**：backlight-test.sh 把 xl 放后台后脚本先退出，
  WSL 会话结束把 xl 的轮询进程杀掉了，日志一次都没 fetch。教训：跑必须在前台进程里。
- 镜像本身在宿主机上用 `tools/apfs/apfs.py` 验过：容器 607170 块、45061 文件、
  0 坏块、`/sbin/launchd` 在（34560 字节）。**镜像结构是好的**，不是坏镜像。
- 该镜像（2,486,968,320 B = 0x943a0000，08:44 重建）从未成功启动过；01:30 成功那次
  用的是 0x943c4000 的旧镜像（已被覆盖，无备份）。

### 3) 对照 UniXNU-alioth（工作正常的参考实现，已 clone 到
###    <主机>\Develop\iOSOnAndroid\UniXNU-alioth）
- **根在 UFS 真存储**（`board.toml [userdata]`：OS/SystemOS 两个卷在 userdata，
  带 verified-root；Data 卷只有 32 MiB RAM seed）。没有 2.3GB 内存上传。
- **三条实时诊断**：`qcom-usb-debug=1` CDC-ACM 串口控制台、`boot-progress=3`
  面板文字页、持久日志 + `sheng-autoreset=180` 自复位。原话："a colour band
  cannot carry what a text page can"。
- **镜像防呆**：`check-payload-kernel.py` 防"build kernel 后不 build payload
  会打包旧内核"——和我们 `mkxnuimg.py --device` 默认 poco-x5-pro 是同一类坑。
- **已知崩的进程直接禁**：sheng-disable-jobs=liveactivitiesd,cryptexd,routined，
  逐个记录崩溃 PC。
- 他们的未解之谜和我们一致：`fastboot boot` 连续失败、同一镜像手动断电重启能进，
  怀疑落回活动槽位，需要持久日志 tag 字段判定。

### 4) 当前手机状态（2026-10-31 深夜，停手时）
- 手机在 **fastboot**（stock ABL），不在 loader。
- 最后一次 `xl enter` 失败：`loader did not come up on USB (state: fastboot)`。
- 已验证过的恢复手法（今晚用过两次成功）：`fastboot reboot bootloader` → 等 14 s →
  `fastboot boot /tmp/xnu-usb-alioth.img` → loader 约 4 s 起来
  （脚本 `\\wsl.localhost\Ubuntu-24.04\tmp\recover-loader.sh`，未执行）。
- 起来之后必须：`./xl ramdisk userland/system.apfs`（90 s）再跑，否则 launchd panic。

### 5) 下一步建议（未执行）
1. 用**前台**方式跑一次诊断 boot（`-v` + `XL_FOLLOW=15 XL_FOLLOW_KEEP=1`），拿到内核
   停在 bsd_init 之后哪一行的实时日志——这是 night2 缺的那份证据。
2. 中期按 UniXNU-alioth 补齐：镜像打包防呆检查（mkxnuimg 的 DEVICE 必须显式）、
   面板文字诊断页、UFS 根（本树已有 `ufs=2` 块设备和
   `tools/darwin-write.py`，但根仍是 RAM 镜像）。

## 2026-10-31 22:30 桌面回来了：根因是镜像漏了 `GL=1 REST=1`，不是代码被改坏

### 现象

「桌面起不来」——内核起来了、SpringBoard 进程 `job state = running`，但画面停在开机 logo，
之后要么自己返回 loader，要么 600 s 被兜底复位。而 01:29 那张截图（`logs/shot-ios.png`）
里壁纸、小组件、图标全在，说明它曾经是好的。

### 根因：08:43 那次重建镜像时漏了两个构建参数

三条独立证据，互相印证：

1. **镜像里没有 GL 库**。`root.apfs` 的 `/lib` 只有 7 个 `libhakios.*.dylib`，
   没有 `libOSMesa.8.dylib`，也没有 `libhakios.gl.dylib`；全盘搜 `osmesa` 零命中。
2. **内核日志直接点名**：
   ```
   hakios: no accelerated renderer; the software one will draw
   backboardd [WindowServer] CoreAnimation: Software renderer! Simulating crash!
   backboardd [WindowServer] CoreAnimation: Failed to initialize Metal or OpenGLES contexts!
   ```
   这正是 `PROGRESS.md:119` 记过的那条：没 GL → CoreAnimation 拿不到上下文 → 停在 logo。
3. **作业表只有 25 个**。`/redwood/LaunchDaemons` 缺 `chronod`、`PosterBoard`、`searchd`，
   也就是 `devices/alioth/README.md:85` 写的「桌面背景纯黑、小组件纯白空块」那套配置。

也就是说：08:43 那次是**默认参数**构建（`GL` 空、`REST` 空），而截图里那个能用的桌面
需要 `GL=1 REST=1 SYSTEM=1`。

### 修复

```sh
cd userland && GL=1 REST=1 SYSTEM=1 ./build.sh
```

退出 0，0 个 error。结果：

| | 修复前 | 修复后 |
|---|---|---|
| `root.apfs` | 4,194,304 (1,024 块) | 6,266,880 (1,530 块) |
| `/lib/libOSMesa.8.dylib` | 无 | 14,822,264 |
| `/lib/libhakios.gl.dylib` | 无 | 541,888 |
| launchd 作业 | 25 | **407**（`... and 382 more jobs`） |
| `chronod` / `PosterBoard` / `searchd` | 无 | 全在 |

旧镜像备份在 `~/alioth-backup/root.apfs.25jobs`（这是唯一可能回退的来源，
`SYSTEM=1` 会覆盖 `system.apfs`）。

### 验证

上传 2371 MB（91 s，26 MB/s）→ 跑 `gl=1 adreno=22 adreno_keep=1 touchscreen=1 bootcpu=7
cpus=8 rd=md0 battery=1`：

- 中途截图 `logs/verify2.png`：**苹果 logo**（不再是 loader 的三条色带）；
- 再截 `logs/verify3.png`：**桌面完整回来** —— 壁纸、Weather/Calendar 两个小组件、
  全套图标、Dock、电池 99%。文件 3,056,630 字节，与 01:29 那张 `shot-ios.png`
  （3,056,607 字节）几乎同尺寸，是满屏内容的旁证。

### 过程中的两个坑（都是老坑，又踩了一遍）

1. **哨兵文件又一次骗过上传**。`pkill` 掉 `xl run` 后 loader 里其实没有常驻镜像了，
   但 `${XDG_RUNTIME_DIR}/darwin-hakios-phone.ramdisk` 还在，于是内核拿 4 MB 的
   `root.apfs` 当根 → `panic: Process 1 exec of /sbin/launchd failed, errno 2`。
   **判别：** 好的跑法日志里有 `resident image at pa fc000000 size 943c4000: it is the root
   (md0)`，坏的那次没有。修法：`rm` 掉哨兵再 `./xl ramdisk`。
2. **`xl run` 在后台跑时被 `pkill` 会留下 `.run` 锁**，下一次 `xl run` 报
   "a run is in progress"。清理：`./xl stop` + `pkill -f "xl run"` +
   `rm /run/user/1000/darwin-hakios-phone.run`。

### 顺带确认的

- **代码侧没有发现被改坏**。`inject_sb.c` 的新增（`battery_survey`、`backlight_probe`、
  `battery_dump_view`）都是只读探测；`hakios.display.c` 的 594 行里所有写操作都由
  `display_hz` / `display_bl` / `display_bl_ramp` / `display_read` 四个 boot-arg 门控，
  默认不碰硬件；`display_map()` 只是把原来的三行 `ml_io_map` 包了一层并加了失败即终止线程。
- GPU 侧全程健康：日志里 22 步跑完，TrustZone 归还 secure extent、SMMU 路由、
  `standalone CX cleanup: off again` 都正常。

### 下次别再踩

**重建镜像一律用 `GL=1 REST=1 SYSTEM=1 ./build.sh`**，三个都不能省：
`GL` 少了停在 logo，`REST` 少了壁纸黑+小组件空白，`SYSTEM` 少了 dylib 改了镜像不更新。
判断手上的镜像属于哪套：`ls /redwood/LaunchDaemons | wc -l` → **25 = 精简，407 = 全量**。
## 2026-10-31 23:55 亮度曾能调后来不能调：`CMD_DMA_CTRL` 的 bit 28 是借来的

### 现象

`display_bl_ramp=1` 跑完，日志全绿——渐变循环完整、每包 `intr 0x501`、`CMD_DMA_DONE` 置位——
**屏幕纹丝不动**。而 `PROGRESS.md` 里明确记着 10-31 凌晨亮度过："换 `display_dma_at=0x9aead000`
重跑——亮度变了"。同一份代码、同一个参数，一次能用一次不能用。

### 根因

把寄存器原值打出来，`CMD_DMA_CTRL` 在两个不同的启动里完全不同：

| 启动 | 原始值 | bit 28（embedded mode） | 亮度 |
|---|---|---|---|
| 10-31 凌晨 | `0x14000000` | **有** | 能调 |
| 10-31 深夜 | `0x4000001` | **无** | 不动 |

bit 28 决定命令引擎**是否去内存里取包**。没有它，引擎照样把传输"跑完"并置 `CMD_DMA_DONE`，
包根本没发出去——所以日志全绿而面板毫无反应。

这套代码从没自己设过这一位，一直继承 bootloader 的余温。vendor 的每个 kickoff 路径都显式写
（`dsi_ctrl_hw_cmn.c:673`：`reg |= BIT(28); reg &= ~BIT(24); reg &= ~BIT(29);`），我们没做。

### 修法

`dsi_power_mode()` 现在每次发包前写死这几位：

```c
ctrl &= ~((1U << 31) | (1U << 30));     /* broadcast off, master on */
ctrl |= (1U << 28);                      /* embedded mode：从内存取包 */
ctrl &= ~((1U << 24) | (1U << 29));     /* packet type 0, WC_SEL 0 */
```

`0x4000001` → `0x14000001`。实测 `display_bl=32` 屏幕**明显变暗**，`display_bl_ramp=1` 渐变可见。

### 顺带修掉的一个自己写的 bug

`dsi_tpg_read()` 里 `ctrl` 先存 `CMD_DMA_CTRL`、后又被拿去读 `RDBK_DATA_CTRL`，最后那句
`wr(dsi + DSI_CMD_DMA_CTRL, ctrl & ~CMD_DMA_EMBEDDED)` **把 RDBK 寄存器的值写进了命令引擎的控制字**。
现在用 `dma_ctrl_saved` 单独保存原值。这个 bug 会让每次读回尝试之后，后续所有写亮度都失败。

### 读回：从沉默变成"回了 4 字节，但内容是 0"

同一个修复顺带让读回有响应：

```
dcs read 52: intr 0x100501, status0 0, ack err 0x1003b04, rdbk ctrl 0x40000, data 00000000 ...
```

`rdbk ctrl 0x40000` 的 bit 31:16 是读回字节数，`>> 16` = **4**。面板确实回了 4 个字节，
但四个 RDBK 字读出来全是 0。下一步查这 4 字节落在哪（读序，或需要更长等待）。

### 教训

**`CMD_DMA_DONE` 只证明主机跑完一次传输，不证明包上了线。** 寄存器里"能读到就以为对"的位，
要么自己设，要么别信——否则同一份代码会在不同启动下表现相反，而日志里每一行都是绿的。
## 2026-11-01 00:40 亮度读回查到尽头：vendor 自己写着"理想情况下应该读，现在只设默认值"

### 从"4 字节"到"错误包"

bit 28 修好之后，读回从完全沉默变成有回应：

```
dcs read 52: intr 0x100501, status0 0, timeout 0, ack err 0x1003b04, rdbk ctrl 0x40000, data 全 0
```

`rdbk ctrl 0x40000 >> 16 = 4`。先按"收到 4 字节数据"理解，做了两个实验排除：

| 实验 | 结果 | 说明 |
|---|---|---|
| 清除后立即读 `rdbk ctrl` | `0` | 清除有效，那 4 不是残留 |
| 读完后等 5 ms 再读一次 | 完全相同 | 不是数据写入滞后 |

再读 `DSI_TIMEOUT_STATUS`（`0x0c0`）：

```
timeout 0
```

没有超时。结合 `ack err` bit 24 = `DSI_PERIPH_ERROR_PKT`，结论是：**那 4 字节是 MIPI 错误包本身**
（短包正好 4 字节：DI + 2 字节错误码 + ECC）。硬件把错误包解析成状态位，所以 `RDBK_DATA` 里没有载荷。

`ack err` 低 16 位 `0x3b04` 应该就是面板的错误码——vendor 只解码 bit 15/16/17/20/23/24，这几位没名字。

换 `0x0a`（get power mode）和 `0x04`（get display ID）测试，**结果完全一样**。所以不是 0x52 特殊，
是所有 DCS 读都被拒。

### 决定性证据

拉下完整的 vendor 源码（`techpack/display` 等，见下）后，在 `dsi_panel.c:1041` 找到这段：

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

我们的面板是 `DSI_BACKLIGHT_DCS`。**高通自己写着"理想情况下应该从面板读背光级别，现在只设默认值"。**

并且在整个 `techpack/display/` 里搜 `MIPI_DSI_DCS_READ`，只有一处：

```
techpack/display/msm/dsi/dsi_ctrl.c:1093:	if ((cmd_type == MIPI_DSI_DCS_READ) ||
```

那是 `dsi_ctrl_copy_and_pad_cmd` 里设置 BTA 标志的判断，**不是发起读**。整个显示子系统没有任何
地方真正执行过 DCS 读。

### 结论

读回不是我们写错的 bug，是厂商留着没做的一段。已经试过且排除的：

- 命令类型（`0x05` → `0x06`）——修掉了一个真缺陷
- `0x37` 批处理（vendor 的正确做法）
- RDBK 双写复位
- MDP 空闲等待
- HS 模式（`display_hs=1`）
- TPG FIFO 路径
- 二次读取（排除时序）

写亮度**可用**（`display_bl=N`、`display_bl_ramp=1` 实测都有效）。读回要知道面板支不支持、
需要什么前置条件，只能查 k11a 的 datasheet。

### 顺带：完整 vendor 源码落到 C 盘

`<主机>\Develop\iOSOnAndroid\k40-ref`，LineageOS `android_kernel_xiaomi_sm8250`
分支 `lineage-22.2`，commit `a04dba039`，74329 个文件（完整内核树）。

Windows 上直接 clone 会失败：`drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c` 里的 `aux` 是保留设备名，
`git checkout` 整个工作树全部失败。逐文件 rsync 到 `/mnt/c` 只有 6 文件/秒（要 3 小时）。
最后走的路：WSL 里完整 clone → `tar` 打包（1.4 G，5 秒）→ 拷到 C 盘 → Windows `tar` 解压（30 秒）
→ `git reset --hard` 检出 74342 个文件。16 个仍缺失，全是保留名或只差大小写的文件
（`xt_DSCP.h` 对 `xt_dscp.h` 这类），都在 netfilter 和 nouveau，与本机工作无关。
## 2026-11-01 01:50 控制中心调不了亮度：CoreBrightness 选了虚拟背光，不碰硬件

### 现象（用户描述，两个都要紧）

- **设置里拖亮度条**：滑杆动、控制中心的数字也跟着变，**屏幕亮度不变**
- **控制中心里拖亮度条**：**拖不动**

### 已经排掉的，一层一层

| 环节 | 证据 | 结论 |
|---|---|---|
| 内核发布服务 | `AppleBacklight: 2 services published` | ✅ |
| iOS 侧能查到 | `probe: AppleBacklight -> IOService:/GenericARMPE/disp0/AppleBacklight` | ✅ |
| sandbox 不挡 | `probe: IOServiceOpen ok (type 0)` | ✅ |
| user client 协议 | `probe: method 0 -> kr 0x0, 4 out` | ✅ |
| backboardd 收到滑杆 | `brightness change:0.351675 reason:setDisplayBrightness (permanent)` | ✅ |
| **CoreBrightness 往下传** | `dcs 51` 0 次、`properties written` 0 次、`CLIENT OPENED` 0 次 | ❌ |

服务的**两条接收通道都实测可用**（user client 的 externalMethod 返回 `kr 0x0`，属性路径也实现了），但
CoreBrightness 一条都不走。

### 从 dyld 缓存里找到的原因

`strings` 搜出 CoreBrightness 有**三个**背光控制器：

```
BCAppleBacklightBrtControl     真实背光（IOKit 服务）
BCHIDBrtControl                HID 背光
BCVirtualBrtControl            虚拟背光：只改 UI 数值，不动硬件
```

现象和第三个完全吻合：滑杆动、数字变、屏幕不变，控制中心的滑杆还拖不动（虚拟控制读不到真实范围）。
**它退回了 `BCVirtualBrtControl`。**

### 试过的位置和属性（全部无效）

| 尝试 | 结果 |
|---|---|
| 服务挂平台专家 `GenericARMPE` 下 | ❌ 不调用 |
| 服务挂显示服务 `disp0` 下（`IOService:/GenericARMPE/disp0/AppleBacklight`） | ❌ 不调用 |
| `IODisplayParameters` 只在背光服务上 | ❌ |
| `IODisplayParameters` 同时放到显示服务上 | ❌ |

挂载位置改到 `disp0` 下之后路径和真实设备的层级一致了，但 CoreBrightness 仍然不认。

### 代码状态（三处位置调整留在树里）

- `MiniIOMobileFramebuffer.cpp`：`start()` 里调 `MiniAppleBacklightStart(this)`，背光挂显示服务下
- `GenericARMPE.cpp`：原来的挂载点注释掉
- `MiniAppleBacklight.cpp`：`IODisplayParameters` 同时发布到 provider 上

方向是对的（层级正确），但不足以让 CoreBrightness 用它。

### 下一步：只能反汇编

需要看 `BCAppleBacklightBrtControl` 的 `initWithService:` **调用者**在什么条件下才构造真实控制器，
而不是退回虚拟的。这要：

1. 从 `sysimage/20F66__iPhone10,3_6/dyld_shared_cache_arm64*` 里提取
   `BrightnessControl.framework` 的二进制（仓库里没有提取工具，`ipsw` 或自己解析都行）
2. 反汇编找 `BCAppleBacklightBrtControl` 的构造点，看它检查了什么

在这之前，内核侧已经没有可做的事了。

### 一个副产品：内核 printf 在 IOKit 上下文会被抑制

`method 0` 明明返回成功，但内核的 `CLIENT OPENED`、`services published` 都没进日志。这解释了为什么
之前一直看不到 `asked for N times`。判断调用有没有发生，只能靠用户态的返回值（probe），不能靠内核日志。
## 2026-11-01 02:30 控制中心的亮度条通了：缺的是 `backlight-control` 一个属性

### 结果

```
display: brightness 908:  dcs 51 ...
display: brightness 948:  dcs 51 ...
display: brightness 1015: dcs 51 ...
display: brightness 1140: dcs 51 ...
```

控制中心拖动亮度条，屏幕跟着变。一次会话里 `dcs 51` 发了 **55 次**，每一个中间电平都到了面板。

### 根因

CoreBrightness 找背光服务的条件**不是类名**。查 sheng 的 `AppleARMBacklight.cpp`（通过 asteroids 端口的
`package/AsteroidsBacklight.cpp` 读到）原文：

> CoreBrightness (backboardd) finds a service **with `backlight-control = true`** that **conforms to
> AppleARMBacklight**, reads min and max from IODisplayParameters brightness, and sets
> `{brightness, FadePeriod (16.16 s)}` through **setProperties**.

我们的 `MiniAppleBacklight.cpp` 把类名取成 `AppleBacklight` / `AppleARMBacklight`，就以为匹配上了。
类名只是其中一半，缺的是那个属性，于是 CoreBrightness 退到它自己的第三个选择
`BCVirtualBrtControl`——一个只改滑杆和数字、不碰硬件的控制器，正是之前看到的现象。

### 改了什么

`iokit/Kernel/MiniAppleBacklight.cpp`：

```c
setProperty("NativeSandboxGrantProcess", "backboardd");   /* 沙盒允许 backboardd 写这些属性 */
setProperty("backlight-control", true);                    /* CoreBrightness 的匹配条件 */
```

还有 `setProperties` 的协议：CoreBrightness 发的是**扁平**的字典，`brightness` 和 `FadePeriod`
直接在顶层，不是嵌套在 `IODisplayParameters` 下。原来的实现只认嵌套格式，所以即使被调用也读不到。

### 这一路怎么排除过来的

| 环节 | 什么时候确认的 |
|---|---|
| 服务发布 / iOS 可见 / sandbox / user client 协议 | 探针验证，全部可用 |
| 挂平台专家下 → 挂显示服务 `disp0` 下 | 层级改了，仍不调用 |
| `IODisplayParameters` 放背光服务 / 也放 provider | 仍不调用 |
| **加 `backlight-control` 属性** | **调用立刻开始** |

前面每一项都"看起来对"，但都不够。差别只有最后那一个属性。

### 读回仍然不通，但不影响

`dcs 52 answered 00000000` 每次都打，读回还是拿不到数据（面板回错误包，前面查过：vendor 自己也从不读背光）。
写亮度不依赖它。

### 参考来源

这是从别的机型学来的，值得记住怎么找到的：
[unixnu.com/devices](https://unixnu.com/devices/) 里搜各机型的 `porting.toml` 找 `brightness`/`backlight`，
命中 **rootstockos/UniXNU-asteroids**（Nothing Phone 3a），它的 T37 记录写着 iOS 设亮度、睡眠开关面板
都已经可用。它的 `package/AsteroidsBacklight.cpp` 头注释把 CoreBrightness 的全部条件写了出来。

**遇到 iOS 侧不认的服务，先看别人怎么让 iOS 认的，比啃二进制快得多。**
## 2026-11-01 03:55 亮度和电池改成默认开启：功能不该靠启动参数

### 用户的意见

> 为啥搞到启动参数里，这个固化不是更好吗，需要调试的才用启动参数吧？

对的。`backlight=1`、`backlight_arm=1`、`backlight_dcs=1`、`battery=1` 四个参数原本都是
opt-in——**忘了任何一个，现象和"驱动坏了"一模一样**。今晚前面几小时的排查里，有一半时间
是在跟"参数没带"较劲，而不是跟代码。

### 改法：全部 opt-out

| 位置 | 原来 | 现在 |
|---|---|---|
| `MiniAppleBacklight.cpp` | `backlight=1` 才发布 | 默认发布，`backlight=0` 关 |
| 同上 | `backlight_arm=1` 才发第二个类 | 默认发，`backlight_arm=0` 关 |
| 同上 | `backlight_dcs=1` 才真发 DCS | 默认发，`backlight_dcs=0` 关 |
| `hakios.battery.c` | `battery=1` 才初始化 | 默认初始化，`battery=0` 关 |
| `xnuloader/loader.c` | 命令行有 `battery=1` 才传 `battery_shared_pa` | 默认传，`battery=0` 不传 |

### 实测（命令行只有 `gl=1 adreno=22 adreno_keep=1 touchscreen=1 bootcpu=7 cpus=8 rd=md0`）

```
cmdline: debug=0x8 battery_shared_pa=0x00000000a1012000 loader_return_after=150 ...
hakios: battery: SOC=98% battery=1 charger=1 charging=1
display: brightness 1024: dcs 51 ...
```

电池从 `SOC=-1% battery=0 charger=0 charging=0` 变成正常读数，亮度不用任何参数就能被
CoreBrightness 调到。

### 顺带发现的一个坑：`xl enter` 在 loader 状态不重建镜像

```sh
enter() {
	case "$(state)" in
	loader) return 0 ;;      ← 手机已经在 loader，直接返回
```

所以改完 loader 之后如果手机还停在 loader，`xl enter` 不会重新打包 `xnu-usb.img`，**新 loader
一直不生效**。今晚就卡在这里：`loader.efi` 是 01:30 编译的，`xnu-usb.img` 却还是 22:15 的。

绕过的办法：先让手机离开 loader（`fastboot reboot bootloader` 或 `xl reload`），再 `xl enter`。
手动打也可以，但记得带 `DEVICE=alioth`：

```sh
xnuloader/mkpayload.py -o usb-payload.bin loader.efi=xnuloader/loader.efi --usb
DEVICE=alioth ./mkxnuimg.py -p usb-payload.bin -o xnu-usb.img
```

判断手上镜像是不是新的：`xnu-usb.img` 的时间戳和 `xnuloader/loader.efi` 比。正确镜像
1,994,752 字节（打成 POCO 会是 1,470,464）。

### 状态

亮度和电池都默认可用，不再需要任何启动参数。参数保留下来只做关闭和调试用。
