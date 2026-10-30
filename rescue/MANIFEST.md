# 救援镜像：存的是做法，不是镜像

`twrp-alioth-rescue-v4.img`（201 MB）和它的内核 `kernel-patched.bin`（48 MB）**不在这个仓库里**。
它们能从公开的 TWRP 镜像重新做出来——逐字节相同的部分只有内核的一个字节——所以这里存的是那
一个字节在哪、四个小文件、一个能重做的脚本。

重做：

```sh
python3 rescue/mkrescue.py twrp-alioth.img -o twrp-alioth-rescue-v4.img
```

脚本会打印内核的 SHA-256，与下面记录的 `6cea071a…` 不同就说明手上的 TWRP 不是同一版。

## 镜像是什么

原版是 TWRP 3.7.1_12 for alioth（<https://dl.twrp.me/alioth/>）。三处改动，其余（头、填充、
AVB 块）原样复制：

| 改动 | 内容 |
|---|---|
| 内核一个字节 | `__setup` 字符串 `skip_initramfs` → `skip_initramfX`，位于未压缩 ARM64 Image 偏移 **40444260**（字符串起点 40444247）。ABL 从闪存引导时会传这个参数，改名后内核不再跳过 ramdisk |
| ramdisk 一行 | `init.recovery.service.rc`：`service recovery /system/bin/recovery` → `/fixwrap.sh` |
| ramdisk 三个文件 | `fixwrap.sh`：跑 `/fixb.sh`，然后 `exec /system/bin/recovery`；`fixb.sh`：只在自己跑在 a 槽时，把 b 槽的 GPT 条目写回并重启；`gpt_region_b.bin`：要写回的 15,872 字节 |

因为内容改了而 AVB 块没改，**`fastboot boot` 会拒绝这个镜像**（`Failed to load/authenticate
boot image: Load Error`），它只能刷进槽位、由 ABL 引导。

## 它没有成功

PROGRESS.md（2026-10-26 深夜，「救援系统探索收尾」）记的是：打进 v4 后**卡 logo 依旧**——补丁
未生效或有别的问题，未查明，勿当已解决。这里留的是下一次尝试的起点，不是能用的救援。

现在真正防变砖的是另一套，同一节里：boot_a 已置零，槽位属性 `boot_a=0x7a`、`boot_b=0x77`，
事故会停在 fastboot，插电脑 `fastboot --set-active=b` 就回来。

## 记录

| 文件 | 大小 | SHA-256 |
|---|---|---|
| 原版 `twrp-alioth.img` | 201,326,592 | `bf5bf3300da28abbef0493829a35fcd703424c1255172f5613789a7e2cfed2e3` |
| `kernel-patched.bin`（脚本产出） | 48,597,004 | `6cea071a165e5ce523bc07fb2138ee8bec423e0fb4e1715586b254aa298d9776` |
| 原 v4 镜像（重建前的那一份） | 201,326,592 | `9cf05f71408a9461c044803e4bf074be14fa8b8a38809f16e04b125784df0239` |
| `fixb.sh` | 637 | `ddc640fd63aab030e552b1965e4f86f5f0de9406f5123878b09da721908beade` |
| `fixwrap.sh` | 52 | `b39b73128dc9fef7b305369f22bf949d5478789ab0d151c4a39dfa7af343e202` |
| `init.recovery.service.rc` | 153 | `45f90c3ddef2fdd69e6a651f8f93d2d345eea552aaee1cf7c2ecaa58917fb467` |
| `gpt_region_b.bin` | 15,872 | `ab36c5f6cb0d51fd1385f13dc7032371dc14315d1fd788f6b6986c24653e02c0` |

`gpt_region_b.bin` 是手机 sde 磁盘第 4456400 扇区起的 31 扇区 GPT 备份条目，即 `fixb.sh` 写回的
内容；原样备份在手机侧的 `dump/gpt_region_b.bin`，与本目录的这份相同。

## 重建到什么程度

- **内核**：逐字节相同，SHA-256 与记录一致（脚本会核对）。
- **镜像**：不保证逐字节相同——ramdisk 重新 gzip，压缩结果会变。ramdisk 的**内容**相同：3672 个
  条目，含上面四个文件。
- 实测一轮：`mkrescue.py` 从 `twrp-alioth.img` 重打，内核 SHA 一致，ramdisk 条目数一致（3672），
  镜像 201,326,592 字节，与记录的原 v4 镜像只差在 ramdisk 的压缩字节上。
