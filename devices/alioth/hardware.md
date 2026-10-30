# Redmi K40（alioth）硬件事实表 —— iOS 16 移植用

> 来源标注：**[K40-DTS]** = LineageOS `android_kernel_xiaomi_sm8250` @ lineage-22.2 的
> `arch/arm64/boot/dts/vendor/qcom/alioth-sm8250.dtsi`（及下载到 `../../k40-dts/` 的附属文件）；
> **[V60]** = ios-v60 仓库 `docs/v60/HARDWARE.md` 中已在真机验证的 SM8250 常量（K40 同 SoC，可直接引用）；
> **[公开]** = 公开规格；**[TBD]** = 需要从你的真机 dump 确认，见 PORTING-PLAN.md。

## SoC 与内存

| 项目 | 值 | 来源 |
|---|---|---|
| SoC | Qualcomm SM8250-AC（骁龙 870，Kryo 585：1×A77@3.2 + 3×A77@2.42 + 4×A55@1.8） | [公开] |
| ARM 架构特性 | ARMv8.2，支持 16K 页转换粒度（**无 topaz 那种只能用 4 核的问题**）；无 PAC 依赖（用 iPhone X 用户态） | [公开]/推导 |
| RAM | **12 GB 版**（SKU pro，hwc=CN，cert M2012K11AC）：System RAM 实测 0x80880000–0x37FFFFFFF（含安全区裁剪）；DTB memory 节点与 iomem 已 dump 存档 | [实测] |
| 定时器 | virtual/physical INTID 27/30，counter 19.2 MHz | [V60] 同 SoC |
| PSCI | SMCCC 1.1，经 SMC 调用 | [V60] |
| restart reason（IMEM） | 0x146bf65c | [V60] 同 SoC |
| apps watchdog | 0x17c10000，EN 寄存器 +0x08 | [V60] 同 SoC |
| GIC Distributor | 0x17a00000，size 0x10000 | [V60] 同 SoC |
| GIC Redistributor | 0x17a60000，size 0x100000，stride 0x20000 | [V60] 同 SoC |
| SPMI | core 0xc440000 / obs 0xe600000 / cnfg 0xc40a000 | [V60] 同 SoC |
| UFS 控制器 | 0x01d84000，LUN0，4KB 块 | [V60] 同 SoC |

## 显示

| 项目 | 值 | 来源 |
|---|---|---|
| 面板 | 6.67" AMOLED 1080×2400，DSC，CMD 模式，驱动 IC **SW43404**（`dsi-panel-sw43404-amoled-dsc-fhd-plus-cmd.dtsi`） | [K40-DTS] |
| 供电 | `dsi_panel_pwr_supply_vci` → pm8150a_l11 | [K40-DTS] |
| 默认面板节点 | **实机 = 天马 k11a**（cmdline: `mdss_dsi_k11a_38_08_0a_dsc_cmd`，dtbo_idx=23）；DSC、cmd 模式、24bpp、burst、bl_ctrl_dcs，完整时序在 dump/twrp-fdt.dts 第 26971 行起 | [实测] |
| 面板控制脚 | reset = TLMM gpio12，TE = TLMM gpio66，ESD 中断 = TLMM gpio51，供电 = `display_panel_avdd`（PM8150A 系） | [实测] |
| splash framebuffer | **0x9C000000 已确认**（Mu-Silicium MemoryMapLib "Display Reserved" 区，36 MB），1080×2400、stride 4320、DSC **[TBD: 实机确认]** | [Mu-Silicium] |

K40 面板存在多供应商混用（三星 E4 / 天马），不同批次的面板 IC 可能不同；这影响第 4 阶段显示管线的适配范围，首批只需支持你手里这台。

## 触摸（关键差异项）

| 项目 | 值 | 来源 |
|---|---|---|
| 触摸 IC | **FocalTech FTS**（`compatible = "focaltech,fts"`，10 点触控） | [K40-DTS] |
| 总线 | **SPI**：QUPv3 SE4（gpio8=CLK, gpio9=MISO, gpio10=MOSI, gpio11=CS），SPI 最高 6 MHz；**SE4 PA = 0x990000**（kona-qupv3.dtsi `spi@990000`），TLMM PA = **0x0F000000**（kona-pinctrl.dtsi `pinctrl@f000000`） | [K40-DTS] |
| IRQ / Reset | IRQ = TLMM gpio39（flags 0x2008 = 下降沿），Reset = TLMM gpio33，**均已实测确认** | [实测] |
| 供电 | **实测（TWRP FDT）：vdd = PM8150A L11 @ 3.0V，vcc_i2c = PM8150A L2（1.2–1.3V）**，RPMh LDO。⚠️ lineage-22.2 树写的是 L10/L1@1.8V，与真机 4.19 FDT 冲突——bring-up 以 L11/L2 为准（本机 TWRP 实测可正常上电），点不亮再回退 | [实测] |
| SE4 寄存器 | PA **0x990000**，size 0x4000，中断号 GIC 0x25d；SE 自身 50 MHz，FTS 子节点限 **6 MHz** | [实测] |
| 逻辑坐标 | 8640×19200（= 1080×2400 ×8） | [K40-DTS] |

好消息成对出现：协议层可借鉴 **ios-topaz 的 FocalTech 驱动**（63 字节报文格式：buf[2]&15 为点数、
每点 6 字节，补丁 0060/0083），传输层可借鉴 **HackiOS 的 bit-bang SPI**（redwood_goodix.c，无需
GENI 时钟投票）。骨架已写好：`port/alioth/bsd/dev/alioth_touch.c`，剩 FTS-over-SPI 帧格式需真机确认。

## 按键 / PMIC

| 项目 | 值 | 来源 |
|---|---|---|
| PMIC 家族 | PM8150（按键/基础）、PM8150A（显示供电）、PM8150B（充电/USB-PD），K40 与 V60 同家族 | [K40-DTS][V60] |
| 电源键 / 音量 | 电源键 + 音量下 = PM8150 SID0 PON@0x810 bit0/1（V60 验证，K40 无 gpio_keys 电源/音量下节点）；**音量上 = TLMM gpio6（active-low），gpio_keys 节点实测确认** | [V60]+[实测] |
| 按键实现方式 | K40 设备树无 gpio_keys 节点 → 走 qpnp-power-on，与 V60 相同 | [K40-DTS] |

按键驱动骨架已写好：`port/alioth/bsd/dev/alioth_buttons.c`（SPMI 仲裁器/观察者代码直接复用
redwood_buttons.c，其地址 0xc440000/0xc40a000/0xe600000 与 V60 实测完全一致）。

## UEFI 引导（第 1 阶段，已落实）

| 项目 | 值 | 来源 |
|---|---|---|
| Mu-Silicium 目标 | **`Platforms/Xiaomi/aliothPkg` 已获取**（本地 `base/mu-silicium/`，sparse 检出），直接 `build_uefi.py -d alioth` | [实测] |
| FD_BASE / FD_SIZE | **0x9FC00000 / 0x00300000 已确认**（MemoryMapLib "UEFI FD" 区；FD 之上 0x9FF90000=Stack、0x9FFFF000=Info Blk，与 dsc 的 PcdCPUCoresStackBase 吻合） | [Mu-Silicium] |
| PAYLOAD_BASE | 安卓 boot 镜像加载惯例 0x8000000 段或按内存图 DXE Heap 0x98900000 区域安排；8GB 版 DDR 顶 0xA0000000 **[TBD: 构建时按 build_uefi.py 输出确认]** | [TBD] |

### SM8250 DDR 布局（K40，来自 Mu-Silicium MemoryMapLib，已确认）

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

寄存器区（GSI/TLMM 三片 0x0F100000/0x0F500000/0x0F900000、USB、AOSS、SMMU、APSS_HM 0x17800000 等）同表，供 loader 页表与驱动引用。IMEM 0x14680000（Cookie 0x146BF000，与 restart reason 0x146bf65c 一致）。

#### TLMM 三片 tile：基址已实测确认，但引脚分配是**交错的**

基址（上表的三片）已被两个独立来源确认：Mu-Silicium 的寄存器表，以及 XNU 内核里用 TDM 引脚
（gpio 133/134/135/137）逐个候选基址试读——**只有 `0x0F100000` 读出合理的引脚配置**
（`cfg 0x00000001`：pull 1、function 0、drive 2 mA），其余两处全 0。

**但引脚属于哪一片不是连续分段**，这一点必须照抄上游表，不能按区间猜：

```
SOUTH   0..3      NORTH   4..23     SOUTH  24..27    NORTH  28..31
SOUTH  32..65     NORTH  66..68     SOUTH  69..76    NORTH  77..84
SOUTH  85..91     NORTH  92..124    SOUTH 125..132   WEST  133..179
```

现有驱动里的 `GPIO_BASE(n) = (n >= 4 && n <= 23) ? north : south` **只在它们用到的引脚上碰巧正确**
（touch 的 8–11/33/39、haptic 的 60/61）。它对 28–31、66–68、77–84、92–124、133–179 全部算错。
音频要用的 133–137 属于 **WEST**，照抄旧宏会写到 south 去（读回全 0，看起来像"引脚没配"，
实际是地址错了）。

| 引脚 | 用途 | tile |
|---|---|---|
| 8–11 | 触摸 SPI（QUPv3 SE4 @ 0x990000） | NORTH |
| 20 | WiFi `wlan-en` | NORTH |
| 33 / 39 | 触摸 reset / IRQ | SOUTH |
| 60 / 61 | 马达 I2C（SE11） | SOUTH |
| 114 | 两颗 CS35L41 的共用 reset | NORTH |
| 119 / 120 | 功放 I2C（SE3 @ 0x98c000，SDA/SCL） | NORTH |
| 133 / 134 / 135 / 137 | 三级 TDM：SCK / DIN / WS / DOUT（`mi2s2_*`，function 1） | **WEST** |

## 其余外设

| 项目 | 状态 | 说明 |
|---|---|---|
| GPU | Adreno 650 | Mesa freedreno 支持成熟；HackiOS 的 Adreno 驱动基于 642L，同 a6xx 家族，需改地址/固件 |
| Wi-Fi | **QCA6390**（`qcom,cnss-qca6390@b0000000`，wlan-en = TLMM gpio20） | **ath11k 官方支持的芯片**——HackiOS 的 ath11k-via-LinuxKPI 路线可直接走，不再是"未知"。⚠️ 需先有 PCIe 根复合体驱动（`qcom,pcie@1c00000/1c08000/1c10000`），XNU 侧目前完全没有 PCIe 代码；设备树里 MHI 挂在 domain 2 而 cnss 节点写 `wlan-rc-num = 0`，**两者不一致，动手前必须用运行时 `lspci` 核实** |
| 音频 | 见 `alioth-audio-overlay.dtsi`（已下载） | **扬声器已实测到"可配置"**：两颗 CS35L41 挂在 `i2c@98c000`（QUPv3 SE3，SDA=gpio119/SCL=gpio120，复位=gpio114 低有效），DEVID `0x035a40`；内核能读能写，升压已按本机 1000 nH/15 µF/4000 mA 配好，模拟增益已从 19 压到 0。出声还差 ADSP + 三级 TDM（`0x9020`/`0x9120`）+ 使能。麦克风走 SoundWire（WCD938x），是另一套控制器 |
| 相机 | IMX582 等（`alioth-sm8250-camera-*.dtsi`） | 最复杂，最后做 |
| 指纹 | 侧边 Goodix 指纹 | iOS 无对应能力，忽略 |
| 振动 | aw8697（与 HackiOS haptic 同款） | 可直接借鉴 |
