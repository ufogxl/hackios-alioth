# Redmi K40（alioth）技术规格 —— iOS 16 移植用

任何一个值动手之前先在这里找一遍：这张表存在的意义是**不再重复踩同一个坑**。
每一条都注了来源，来源不同可信度不同：

- **[本仓 DTS]** = `devices/alioth/dts/` 里入库的设备树（`alioth-sm8250.dtsi`、`kona-*.dtsi`）
- **[真机 FDT]** = 本机 dump 出来的 `twrp-fdt.dts`，反编译自手机自己的 dtbo（**优先级最高**——
  入库的 DTS 是 LineageOS 的，与本机批次可能不同）
- **[厂商内核]** = 高通的 vendor 内核源码，见下面「厂商源码从哪来」
- **[实测]** = 在这台手机上跑出来的，`PROGRESS.md` 里有对应的日志
- **[公开]** = 公开规格；**[TBD]** = 还没确认

真机 dump、旧工具、备份都不在仓库里，在 `<主机>\Develop\alioth-local`
（`dump/twrp-fdt.dts` 是其中最有用的一份，36,301 行）。设备本地的大文件（固件、`soc.args`、
日志）另见 `~/alioth-device-state`，同样不入库。

## 厂商源码从哪来：**[厂商内核]**

设备树只说了「是什么」，没说「怎么驱动」。寄存器位定义、时序常数、初始化序列这些，
在设备树里没有，在高通放出来的 vendor 内核里有——**它是公开的**，不用拆机、不用逆向。

用的是 LineageOS 维护的那棵树（SM8250 同 SoC，正是本机）：

```
https://github.com/LineageOS/android_kernel_xiaomi_sm8250    分支 lineage-22.2
```

**按子系统找**（路径都在上面这个仓库里逐条验证过）：

| 要找什么 | 路径 |
|---|---|
| DSI / 面板 / 背光 | `techpack/display/msm/dsi/` |
| GPU（KGSL） | `drivers/gpu/msm/` |
| 触摸（FT3658U） | `drivers/input/touchscreen/focaltech_touch/` |
| 音频 codec（WCD938x） | `techpack/audio/asoc/codecs/wcd938x/` |
| SPMI 仲裁器（按键、PMIC） | `drivers/spmi/spmi-pmic-arb.c` |
| GENI I2C / SPI | `drivers/i2c/busses/i2c-qcom-geni.c`、`drivers/spi/spi-geni-qcom.c` |
| ADC（电池温度等） | `drivers/iio/adc/qcom-spmi-adc5.c` |
| 充电 / 电量计 | `drivers/power/supply/qcom/` |
| RPMh（时钟、供电投票） | `drivers/soc/qcom/rpmh.c` |
| **Wi-Fi** | `drivers/staging/qcacld-3.0/`（高通自家 WLAN 驱动） |

**Wi-Fi 这条要注意**：这棵树里**没有 ath11k**（`drivers/net/wireless/ath/` 只到 ath10k）。
K40 的 QCA6390 走的是高通的 `qcacld-3.0`，不是主线 ath11k。所以想照 ath11k 移植这条路在
本机 vendor 树里没有参考；反过来，`qcacld-3.0` 才是这台机器实际在用的驱动。

显示相关的几个文件最常查：

| 文件 | 里面有什么 |
|---|---|
| `dsi_ctrl_reg.h` | 所有 DSI 寄存器的偏移（`DSI_RDBK_DATA_CTRL 0x01D4` 这类） |
| `dsi_ctrl_hw_cmn.c` | 每个寄存器的**位定义和用法**：读回、FIFO 发包、时钟、PHY |
| `dsi_ctrl.c` | 上层流程：什么时候清读回寄存器、什么时候取数据 |
| `dsi_catalog.c` | 控制器版本到实现的映射（本机 `qcom,dsi-ctrl-hw-v2.4`） |

**不要 clone 整棵树**：Linux 内核树在 Windows 上 checkout 会踩路径长度和大小写问题
（`git sparse-checkout` 会让 `git status` 显示七万多个 `D`，`git reset` 甚至段错误）。
按需下载单个文件即可：

```bash
BASE=https://raw.githubusercontent.com/LineageOS/android_kernel_xiaomi_sm8250/lineage-22.2
curl -O $BASE/techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c
```

（国内网络取 raw.githubusercontent 可能要代理。）

---

## SoC 与内存

| 项目 | 值 | 来源 |
|---|---|---|
| SoC | Qualcomm SM8250-AC（骁龙 870）：1×A77@3.2 GHz + 3×A77@2.42 + 4×A55@1.8 | [公开] |
| 架构 | ARMv8.2；支持 16K 页转换粒度；无 PAC 依赖（用户态用 iPhone X 的） | [公开]/推导 |
| 核编号 | 0–3 = A55 小核（`small_cores=0xf`），4–7 = A77；`bootcpu=7` 让内核跑在大核上 | [实测] |
| RAM | **12 GB**（SKU pro，hwc=CN，cert M2012K11AC）：System RAM 0x80880000–0x37FFFFFFF | [实测] |
| GIC Distributor | 0x17a00000，0x10000 | [实测] 同 SoC |
| GIC Redistributor | 0x17a60000，0x100000，stride 0x20000 | [实测] 同 SoC |
| 定时器 | virtual/physical INTID 27/30；counter 19.2 MHz（always-on 复测 19,198,365 Hz） | [实测] |
| PSCI | SMCCC 1.1，走 SMC | [实测] |
| restart reason（IMEM） | 0x146bf65c（IMEM 基址 0x14680000，Cookie 0x146BF000） | [实测] |
| apps watchdog | 0x17c10000，EN 寄存器 +0x08 | [实测] |
| SPMI | core 0xc440000 / obs 0xe600000 / cnfg 0xc40a000 | [实测] |
| UFS 控制器 | 0x01d84000，LUN0，4 KB 块 | [实测] |

## 显示（这台手机上最容易被误解的一块）

| 项目 | 值 | 来源 |
|---|---|---|
| 面板 | 6.67" AMOLED 1080×2400，**DSC + CMD 模式**，24 bpp，burst mode | [真机 FDT] |
| 实际面板节点 | **天马 k11a**：`qcom,mdss_dsi_k11a_38_08_0a_dsc_cmd`（`msm_drm.dsi_display0=…`，`dtbo_idx=23`）；完整时序在 `dump/twrp-fdt.dts:26971` | [真机 FDT] |
| 入库 DTS 写的面板 | `dsi-panel-sw43404.dtsi`（三星 E4），**与本机不是同一个面板**——不要拿它的时序上机 | [本仓 DTS] vs [真机 FDT] |
| 帧缓冲 | **0x9C000000**，36 MB（"Display Reserved"）；1080×2400，**stride 4320**，像素 BGRX | [实测] |
| 谁在扫这块内存 | **MDP/DSI 由 bootloader 配好**；我们的代码只往 0x9C000000 里写。但 `hakios.display.c` 现在能发 DCS（`display_hz` / `display_bl`），前提是从**bootloader 留下的那页命令缓冲区**发（见下） | [实测] |
| 刷新率现状 | **120 Hz 实测 121.333/s**（`display_hz=120`，5 秒后数 TE：`display_te=6 display_te_delay=25` → 「728 pulses in 6 s = 121.333 a second」）。不开参数时是 bootloader 的 `timing@0`，60.666/s | [实测] |
| 面板控制脚 | reset = TLMM **gpio12**；TE = TLMM **gpio66**；ESD 中断 = TLMM **gpio51**（上升沿） | [真机 FDT] |
| 显示供电 | `qcom,panel-supply-entries = <&dsi_panel_pwr_supply_vci>`；`display_panel_avdd` 与 `pm8150a_amoled` 都是 **disabled** | [真机 FDT] |
| 亮度 | `bl_ctrl_dcs`：DCS `0x53`（0x20 关 dim / 0x28 开 dim / 0xe8 HBM）+ `0x51` 亮度值（0–0x7ff） | [真机 FDT] |

### 三档时序（k11a，`dump/twrp-fdt.dts:27095` 起）

| | timing@0 | timing@1 | timing@2 |
|---|---|---|---|
| framerate | **60** (0x3c) | **90** (0x5a) | **120** (0x78) |
| v-back-porch | 0x230 | 0x118 | 0x08 |
| v-front-porch | 0x258 | 0x12c | 0x04 |
| v-pulse-width | 0x20 | 0x10 | 0x04 |
| h porches | 0x10 / 0x08 / 0x08（三档相同） | | |
| panel-clockrate | 0x4190ab00（≈1.1 GHz，三档相同） | | |
| mdp-transfer-time-us | 14000 | 9500 | 7000 |
| 切档时写的 DCS | `60 00`，`d1 2e` | `60 10`，`d1 6e` | `60 10`，`d1 2e` |

读法：**刷新率是面板自己按 DCS `0x60` 的参数字节走的**（`00`=60，`10`=90/120，靠 `f0 5a 5a` 之后的
`b0 16` / `d1` 再分 90 与 120）。`mi,mdss-dsi-smart-fps-max_framerate = <0x78>` 也说明这颗面板的
上限是 120。三档的 `qcom,mdss-dsi-timing-switch-command` 都是 `dsi_lp_mode`，所以切档要在 LP 下发。

**已通**：`display_hz=120` 把上表 timing@2 那 7 个包按顺序发下去，TE 从 60.666 变成 121.333。
关键在于**命令 DMA 从哪一页读**：DSI 命令 DMA 是总线主设备，走显示侧的地址窗口，内核
`kmem_alloc_contig` 出来的普通页它读不到（`CMD_DMA_DONE`、`status0 0`、`ack err 0`，但面板毫无反应）；
**为什么会这样**：DSI 和 MDP 用的是**同一个地址空间**——`apps_smmu` 的非安全 CB（`0x820`），
`dsi_ctrl.c:282-288` 和 `sde_plane.c:692` 拿的都是 `kms->aspace[MSM_SMMU_DOMAIN_UNSECURE]`；
驱动的语义是「有 aspace 就用 IOMMU 映射后的地址，没有才用物理地址」(`sde_formats.c:1044-1048`)。
bootloader 那一页之所以能读，是因为 splash 被显式做了**恒等映射**
（`sde_kms.c:790-793` → `msm_smmu.c:164` `iommu_map(domain, 同址, 同址, ...)`，DT 上有
`qcom,iommu-earlymap`）——不是因为它是一页特殊的页。`kmem_alloc_contig` 的页从来没进过这套
页表，写进去的地址没有条目；而 `qcom,iommu-faults = "non-fatal"`、fault handler 只打日志就返回
（`msm_smmu.c:398-422`），所以**非法地址不 hang、不报错，只读到全 0**。
**这条对 MDP 直通同样成立**：`SSPP_SRC0_ADDR` 里现在那个 `0x9c000000` 能工作，同样只是因为它
落在恒等映射里；把普通内核页的物理地址写进去，下场和命令 DMA 完全一样。

从 bootloader 留在 `DSI +0x048` 的那页（`0x9aead000`）发就立刻生效。默认值已改为那一页：
`display_dma_at=ADDR` 可换，`display_dma_own=1` 是失败的那个反例。

注意面板 120 Hz ≠ 画面 120 fps：MDP 侧（`mdp-transfer-time-us` 14000/9500/7000）和帧驱动的
`fb_hz`（默认 60）都还没跟着改。

### 帧怎么上屏（`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`）

这是显示驱动，参数不少，全部是 boot-arg：

| 参数 | 默认 | 作用 |
|---|---|---|
| `fb_hz` | **60**（`kFBRefreshHz`） | 上屏节拍，也是 CoreAnimation 读到的速率（1–240） |
| `fb_beat` | 1 | 0 = 不按节拍，GPU 一画完就上屏 |
| `fb_threads` | 3 | 几个核一起把一帧缩放着拷进面板内存（1–8）；3 时约 3 ms，1 时 9–10 ms |
| `fb_queue` | 2 | 允许几帧等面板（1–2） |
| `touch_lat` | 0 | 1 = 每帧一行 `tl:`（`tools/latency.py` / `tools/frames.py`） |
| `display_te` | — | N = 数 N 秒 TE（gpio66）的脉冲并报 Hz；`display_te_delay=N` 先等 N 秒再数（要在切档之后量就用它） |
| `display_hz` | — | `60` / `90` / `120`：发 k11a 对应档的 `timing-switch-command`（7 个 DCS 包，LP）。`display_hz_delay=N` 默认 20 秒后发 |
| `display_bl` | — | 1..0x7ff：发一次 DCS `0x51` 背光；`display_bl_ramp=1` 是 0x7ff→0x20→0x7ff 三轮渐变，约 9 秒，用来肉眼判定"面板到底收没收到" |
| `display_hs` | 0 | 1 = 用 HS 而不是 LP 发（vendor 的普通 DCS 走 HS，切档走 LP；实测 LP 就够） |
| `display_read` | — | 0x01..0xff：发一次 DCS 读并打印 RDBK 四个字（面板不回话时读到全 0） |
| `display_dma_at` | — | 指定命令缓冲区的物理地址；**默认用 `DMA_BASE` 里 bootloader 留的那页**，因为只有显示侧能读到的页才有效 |
| `display_dma_own` | 0 | 1 = 用内核自己 `kmem_alloc_contig` 的页（会静默失败的反例，仅作对照） |
| `fb_filter` | 0 | 1 = 缩小时做混合（更好看，3.8 倍贵） |

## 触摸

| 项目 | 值 | 来源 |
|---|---|---|
| 触摸 IC | **FocalTech FTS**（`compatible = "focaltech,fts"`，10 点） | [本仓 DTS] |
| 总线 | SPI：QUPv3 **SE4 @ 0x990000**（size 0x4000，GIC 0x25d）；gpio8=CLK / 9=MISO / 10=MOSI / 11=CS；最高 **6 MHz** | [本仓 DTS][实测] |
| IRQ / Reset | IRQ = TLMM **gpio39**（0x2008，下降沿）；Reset = TLMM **gpio33** | [本仓 DTS][实测] |
| 供电 | **vdd = PM8150A L10（L10C）@ 3.0 V，vcc_i2c = PM8150A L1（L1C）@ 1.8 V** | [本仓 DTS]，代码 `hakios.goodix.c` 的 `io_supply_on()` 用同一组 |
| 逻辑坐标 | 8640×19200（= 1080×2400 ×8） | [本仓 DTS] |
| 固件 | FTS 固件已烘进源码：`xnu/files/bsd/dev/hakios.focaltech_fw.h`（116,448 字节）；原始 `fw-touch/focaltech_ts_fw_m20.bin` 不在仓库 | [实测] |

⚠️ 老版本的这张表写的是「L11 @ 3.0 V / L2」，**是错的**，已按 DTS 与代码改正。另外驱动文件名
`hakios.goodix.c` 是历史遗留：它同一份代码里既有 POCO 的 Goodix GT9916 分支，也有这里用的
FocalTech 分支，alioth 走的是后者。别被文件名骗了。

## 按键 / PMIC

| 项目 | 值 | 来源 |
|---|---|---|
| PMIC 家族 | PM8150（按键/基础）、PM8150A（显示/触摸供电）、PM8150B（充电/USB-PD） | [本仓 DTS] |
| 电源键 / 音量下 | PM8150 SID0 PON @ 0x810，bit0 / bit1 | [实测] |
| 音量上 | **PM8150 的 SPMI GPIO6**（不是 TLMM gpio6！真机 FDT：`gpios = <&pm8150_gpios 6 GPIO_ACTIVE_LOW>`，node `gpio_keys/vol_up`）。`hakios.buttons.c` 只读不写 PMIC：若固件没把它留成「带上下拉的输入」，驱动会明说 *"volume up is left alone"* 并放弃这个键 | [真机 FDT]/[实测] |
| 实现方式 | 无 `gpio_keys` 电源节点 → 走 qpnp-power-on | [本仓 DTS] |

## GPU（Adreno 650）

| 项目 | 值 | 来源 |
|---|---|---|
| chipid | **0x06050002**（A650），speed bin **2**（QFPROM 0x00784000，offset 0x19b，bit5 起 3 位） | [真机 FDT] |
| kgsl-3d0 | **0x03d00000**，0x40000 | [真机 FDT] |
| GMU | **0x03d6a000**，0x30000（dt 节点名叫 `qcom,gmu@3d69000`，但它的 `kgsl_gmu_reg` 是 0x3d6a000——按 reg 走） | [真机 FDT] |
| GPUCC | **0x03d90000**，0x9000 | [真机 FDT] |
| kgsl SMMU | **0x03da0000**，0x10000（**不是** 0x20000——POCO 才是）；stream 0 GPU / 2 secure / 4 GMU user / 5 GMU kernel | [真机 FDT] |
| RSCC | 0x03de0000 | [真机 FDT] |
| 频率档 | GMU index 1–7 = **305 / 400 / 441.6 / 490 / 525 / 587 / 670 MHz**；默认 1（305） | [实测] |
| 固件 | `firmware/fw-gpu/a650_gmu.bin`、`a650_sqe.fw`、`a650_zap.*`（设备本地） | [实测] |

**启动参数怎么给**（这一条最值钱，见 PROGRESS「GPU 的正确参数」）：

- `gpu=1` 在 alioth 上**一定被拒**（`hakios.adreno.c:4815`：SM8250 只有独立的 1–22 步）。
- 能用的是 `gl=1 adreno=22 adreno_keep=1`：跑到最后一步，并且**把 GPU 留下来给系统用**
  （服务 `HakiosGPU`，`iokit/Kernel/hakios.gpu.cpp`）。少了 `adreno_keep=1` 就等于跑完收摊。
- `adreno_speed=N`（1–7）挑档位，`adreno_speed=7` 是 670 MHz。频率是发给 GMU 的投票
  （`HFI_GX_BW_PERF_VOTE`），之后 GMU 自己向 RPMh 投票，我们没有 devfreq，所以**选了就一直停在那档**。
- 成功的标志：`OpenGL ES 3.2 Mesa 25.0.7; FD650`、`THE GPU STAYS ON`，且日志里
  `Software renderer` / `no GPU to draw with` / `vibrant views plain` 都是 0 次。

## UEFI 引导（已落实）

| 项目 | 值 | 来源 |
|---|---|---|
| UEFI 镜像 | `devices/alioth/prebuilt/SILICIUM_UEFI.fd`（Mu-Silicium aliothPkg），3 MB | [实测] |
| FD_BASE / FD_SIZE | **0x9FC00000 / 0x00300000**（POCO 是 0x9FD00000 / 0x200000，**所以每条 `xl` 都要 `DEVICE=alioth`**） | [实测] |
| PAYLOAD_BASE | 0xF0000000 | [实测] |
| 栈 / Info Blk | 0x9FF90000 / 0x9FFFF000 | [实测] |

### SM8250 DDR 布局（K40）

| 区域 | 地址 | 大小 |
|---|---|---|
| HYP | 0x80000000 | 0x00600000 |
| AOP CMD DB | 0x80860000 | 0x00020000 |
| GPU PRR | 0x80880000 | 0x00010000 |
| SMEM | 0x80900000 | 0x00200000 |
| QTEE | 0x80B00000 | 0x01900000 |
| TZApps | 0x82400000 | 0x03A00000 |
| PIL Reserved | 0x86200000 | 0x0C500000 |
| ADSP RPC | 0x92700000 | 0x00800000 |
| DXE Heap | 0x98900000 | 0x03300000 |
| **Display Reserved（帧缓冲）** | **0x9C000000** | 0x02400000 |
| UEFI FD | 0x9FC00000 | 0x00300000 |
| UEFI Stack | 0x9FF90000 | 0x00040000 |
| Info Blk | 0x9FFFF000 | 0x00001000 |

### TLMM 三片：基址已确认，引脚分配是**交错**的

基址（**以能工作的驱动为准**，不是按编号猜）：

```
WEST   0x0F100000    pins 133..179（TDM 133-137 在这里；hakios.amp.c 的探针也是这么认出来的）
SOUTH  0x0F500000    pins 0..3, 24..27, 32..65, 69..76, 85..91, 125..132（触摸 33/39、马达 60/61）
NORTH  0x0F900000    pins 4..23, 28..31, 66..68, 77..84, 92..124（SPI 8-11、TE 66、功放 114/119/120）
```

每个引脚的寄存器间隔是 **0x1000**：`cfg` 在 `+0`（[1:0] pull，[5:2] function，[8:6] drive-1，[9] output），
电平在 `+4` 的 bit0（输入）/ bit1（输出）。

⚠️ 早期文档把这三个基址的名字写反过（说 NORTH=0x0F500000）。判断依据只有一条：**能跑的驱动在哪个基址
上读写**——`hakios.goodix.c` 用 0x0F900000 配 gpio8-11、用 0x0F500000 配 33/39，触摸是通的。

**但引脚属于哪一片不是连续分段**，必须照抄这张表，不能按区间猜：

```
SOUTH   0..3      NORTH   4..23     SOUTH  24..27    NORTH  28..31
SOUTH  32..65     NORTH  66..68     SOUTH  69..76    NORTH  77..84
SOUTH  85..91     NORTH  92..124    SOUTH 125..132   WEST  133..179
```

老宏 `GPIO_BASE(n) = (n>=4 && n<=23) ? north : south` 只在本机用到的几个脚上碰巧对。音频要用的
133–137 属于 **WEST**，照旧宏会写到 SOUTH 去，读回全 0，看起来像「引脚没配」。

| 引脚 | 用途 | tile |
|---|---|---|
| 8–11 | 触摸 SPI（SE4 @ 0x990000） | NORTH |
| 12 | 面板 reset | SOUTH |
| 20 | WiFi `wlan-en` | NORTH |
| 23 / 24 | 指纹 IRQ / reset（Goodix） | NORTH |
| 33 / 39 | 触摸 reset / IRQ | SOUTH |
| 51 | 面板 ESD 中断 | SOUTH |
| 60 / 61 | 马达 I2C（SE11） | SOUTH |
| 66 | 面板 TE | NORTH |
| 114 | 两颗 CS35L41 的共用 reset | NORTH |
| 119 / 120 | 功放 I2C（SE3 @ 0x98c000） | NORTH |
| 133–137 | 三级 TDM：SCK / DIN / WS / DOUT（`mi2s2_*`，function 1） | **WEST** |

## 其余外设

| 项目 | 状态 | 说明 |
|---|---|---|
| GPU | ✅ 真机跑起来 | 见上 |
| Wi-Fi | ⚠️ | **QCA6390**，走 PCIe。主机侧已经跑通到"BHI 寄存器可读写、端点枚举成功"，卡在端点不响应 BHI 门铃——见下面的 WLAN 小节 |
| 音频 | ⚠️ 部分 | 两颗 CS35L41 在 `i2c@98c000`（SE3，gpio119/120，reset gpio114），DEVID `0x035a40`，能读能写；出声还差 ADSP + 三级 TDM（0x9020 / 0x9120）。麦克风走 SoundWire（WCD938x） |
| 振动 | ❌ | aw8697 在 I2C 上认得出（地址 `0x5a`、ID `0x97`）、原厂波形 RAM 回读一致、内部 ADC 约 4.3 V，但 **f0 校准和 RAM 播放都没 PASS**，iOS 的 haptic 服务没接上，也没有真实震感验收——见 `PROGRESS.md`「AW8697 振动：已到硬件驱动阻塞点」 |
| 相机 | ❌ | IMX582 等，最复杂，最后做 |
| 指纹 | — | 侧边 Goodix（gpio23/24），iOS 无对应能力，忽略 |

**`soc.args`**（`devices/alioth/soc.args`，由 `pull-soc.sh` 从真机 DTB 生成，`modem-cycle.sh` 自动拼进命令行）：

```
soc_ipa_base=0x1e40000   soc_gsi_base=0x1e04000   soc_ipcc=0x408000
soc_rsc=0x18220000       soc_apps_smmu=0x15000000 soc_smem=0x80900000
soc_imem=0x14600000      soc_ipa_stream=0x5c0     soc_ipa_stream_uc=0x5c2
soc_pas_ipa=0xf          soc_ipa_fw=ipa_fws
```

## WLAN（QCA6390，PCIe）

节点是 `qcom,cnss-qca6390@b0000000`（`dump/twrp-fdt.dts:8159`；`twrp-fdt.dts` 就是这台手机的 fdt）。

| 项目 | 值 |
|---|---|
| 根桥 | `qcom,pcie0@1c00000`（`qcom,wlan-rc-num = <0>`），PERST# = tlmm **gpio79**，WAKE = gpio81 |
| 使能脚 | `wlan-en-gpio = <&tlmm 20 0>`：高有效、8 mA、上拉（`cnss_wlan_en_active`）。**上电序列里拉高，不是事后补的**：Android 自己 0.595 s 上电、0.697 s 拉 EN、0.771 s 才 probe PCIe |
| 电源 | `vdd-wlan-aon` 与 `vdd-wlan-dig` 是**同一路**（phandle 0xa9），`vdd-wlan-io` 0xab、`vdd-wlan-rfa1` 0xac、`vdd-wlan-rfa2` 0xad——四路，和 `hakios.pcie.c` 里投的四路一致 |
| 电压 | aon/dig 950 mV（`0xe7ef0`）、io 1.8 V、rfa1 1.9 V、rfa2 1.35 V |
| 关键 flag | `qcom,smmu-s1-enable`（SMMU stage-1 必须开；bypass 被硬件拒绝，写 type1 读回 type2）、`qcom,set-wlaon-pwr-ctrl`、`qcom,cmd_db_name = "smpf2"`、`mhi,m2-no-db-access`、`qcom,no-l0s-supported`、`qcom,use-19p2mhz-aux-clk` |
| IOVA 池 | `qcom,iommu-dma-addr-pool = <0xa0000000 0x10000000>`；**MSI 门铃也落在这里**（实测 address `0xa0000040`、data `0x320`） |
| 枚举 ID | `17cb:1101`，BAR0 = 1 MB，链路 **Gen2 x1**，端点命令寄存器 `0x0406`（memory+busmaster+serr） |

### 能直接照抄的一组参考读数（同一颗芯片，`BHI_SERIALNU = 0xc4e10d95`）

厂商栈在两种状态下的实测值（用 `alioth-local/tools/regpeek` 从 Android root 读的）：

| 地址 | 含义 | 固件运行（AMSS） | 停在 PBL 等固件 |
|---|---|---|---|
| 0x1e0e228 | BHI_EXECENV | 0x2 | 0 |
| 0x1e0e22c | BHI_STATUS | 0x9aae8427 | 0 |
| 0x1e0e21c | BHI_RSVD2（主线误当 INTVEC） | — | 每次启动不同的随机值 |
| 0x1e0e240 | BHI_SERIALNU | 0xc4e10d95 | 0xc4e10d95 |
| 0x1e08000 | PARF_PHY_CTRL | 0x2000321c | 0x2000361c |
| 0x1e08174 | PARF_MHI_CLOCK_RESET_CTRL | 0x1 | 0x3 |
| 0x1e081b0 | PARF_LTSSM | 0x111 | 0x111（L0） |
| 0x1f8002c | WL_PWR_STATUS | 0x9 | 0xf |
| 0x1f806b0 / 0x1f806ac / 0x1f80318 | 三个时钟切换字 | 0 | 0 |

**0x1e00000 以上的寄存器怎么读**：BAR 只有 1 MB，芯片内部寄存器在 0x1e0e200 这类地址上，
靠 `BAR+0x310c`（`QCA6390_PCIE_REMAP_BAR_CTRL_OFFSET`）开窗——写
`0x40000000 | ((addr >> 19) & 0x3f)`，然后访问 `BAR + 0x80000 + (addr & 0x7ffff)`。

**寄存器地址和 bring-up 顺序的权威来源**是厂商 `cnss2` 驱动，本机副本在
`<主机>\Develop\alioth-local\cnss2-src\`（取自 LineageOS `android_kernel_xiaomi_sm8250`
的 `drivers/net/wireless/cnss2/`，`main.c` `bus.c` `pci.c` `reg.h` `pci.h`）：

- `reg.h` 是 QCA6390 的完整地址表：`BHI_EXECENV = 0x1e0e228`、`PCIE_BHI_ERRDBG2/3 = 0x1e0e238/23c`、
  WLAON 一整片 `0x1f80000`~`0x1f80700`、WCSS/Q6 那片 `0xb6*` `0xc3*` `0xdb*` `0xd0*`、
  `PBL_WLAN_BOOT_CFG = 0x1e22b34`、`PBL_BOOTSTRAP_STATUS = 0x1910008`、
  PBL 日志 SRAM `0x1403d58`（80 B）。
- `pci.c` 是顺序：`cnss_power_on_device`（四路电源）→ `cnss_resume_pci_link` →
  `cnss_pci_set_wlaon_pwr_ctrl`（读改写 `QFPROM_PWR_CTRL` @0x1f8031c，清 shutdown/vdd4blow 位）→
  `cnss_pci_start_mhi`（`mhi_prepare_for_power_up` + `mhi_sync_power_up`，BHI 下载在里面）。
  它在 BHI 之前**没有**别的芯片寄存器写。

## 启动参数速查（本机常用）

```sh
# 一个能用的系统：GPU + 触摸 + 电池，120 s 后自己回 loader
DEVICE=alioth XL_CMDLINE="gl=1 adreno=22 adreno_keep=1 touchscreen=1 cpufreq=1 bootcpu=7 cpus=8 rd=md0 battery=1" \
  XL_SECS=120 ./xl run

# 放着不动，直到 xl stop
DEVICE=alioth XL_SECS=0 XL_CMDLINE="…同上…" ./xl run

# 量帧率（配 RC_JOBS=rc.jobs.fps 构建的小镜像）
… touch_lat=1 glframes=1 …   # 然后 tools/frames.py logs/<run>.log
```

按功能分类的常用参数（完整名单见各处 `TUNABLE`/`PE_parse_boot_argn`）：

| 目的 | 参数 |
|---|---|
| 内核在哪跑 | `bootcpu=7`（大核）、`cpus=8`、`cpufreq=1` |
| 超时 | `XL_SECS=N`；`0` = 永不（`loader_return_after=0`） |
| 系统 | `rd=md0`（用常驻 RAM 里的系统镜像）、`battery=1`、`touchscreen=1` |
| 显示 | `fb_hz` `fb_beat` `fb_threads` `fb_queue` `fb_filter` `display_step` `display_probe` `display_te`(`_delay`) `display_hz`(`_delay`) `display_bl`(`_ramp`) `display_hs` `display_read` `display_dma_at` `display_dma_own` |
| 绘制 | `gl=1` `glwait` `glcopy` `glblit` `glframes=1` `gltrace=A-B` `glskip` |
| GPU 驱动 | `adreno=N` `adreno_speed=N` `adreno_keep=1` `adreno_only=N` `adreno_skip=0xMASK` `adreno_pause=MS` `adreno_fine=1` |
| 触摸 | `touchscreen=1` `touch_power` `touch_spi_hz` `touch_flip` `touch_swap` |
| 测量 | `touch_lat=1`（`tools/frames.py`/`latency.py`）、`cpuload=MS`、`rec_secs=N`（录屏） |

## 踩过的坑（按"疼"排序）

1. **`DEVICE=alioth` 不能忘**。`xl` 和 `mkxnuimg.py` 默认是 POCO，UEFI 地址不同，刷进去进不了 loader。
2. **`gpu=1` 在这台机器上必然被拒**，别浪费时间看它为什么"没生效"。用 `adreno=22 adreno_keep=1`。
3. **触摸供电是 L10/L1**，不是 L11/L2（老文档错了）。写错的表现是「引脚都配了但读不到芯片」。
4. **`tools/modem-cycle.sh` 曾经无条件把 `tools/macos-bin` 放进 PATH**，Linux 上 `timeout`/`stat`
   是 MacPorts 的壳，于是 `fastboot` 全部失败、loader 明明在却报 `loader did not come up on USB`。
   现在只有 Darwin 才加。
5. **loader 重启过就必须重新 `xl ramdisk userland/system.apfs`**（2.4 GB，约 90 s），否则根分区是空的，
   内核 panic 在 `Process 1 exec of /sbin/launchd failed, errno 2`。
6. **常驻镜像不认识工作目录**：`xl run` 会打印它是从哪个 worktree 传的、多大。看到了就读一眼——
   上一轮残留的镜像会让这一轮以完全无关的方式失败。
7. **`XL_CMDLINE=` 给空串等于 `-v`**：`${XL_CMDLINE--v}` 只在变量**未设**时用默认值。
8. **给 TLMM 引脚算错 tile** 时读回全 0，看起来像「硬件没响应」，其实是地址错（见上面的交错表）。
9. **改文件用 UNC 路径（`\\wsl.localhost\…`）会把 0755 变成 0644**，`./xl`、`tools/*.sh` 会
   `Permission denied`。改完 `chmod 755`，或者按 `git ls-files -s` 里的模式恢复。
10. **绿屏不是坏了**：loader 交还控制权时整屏涂色表示结局——绿=跑完、琥珀=停在 checkpoint、
   红=早期异常、紫=panic、蓝=selftest。看到绿屏说明这一轮**正常结束**了；要它一直跑用 `XL_SECS=0`。
11. **改内核要改 `xnu-src/xnu-8796.121.2/`，不是 `xnu/files/`**（后者是 `export.sh` 的产物）；
    增量构建没动静时删掉 `BUILD` 重编（19.8 秒）。
