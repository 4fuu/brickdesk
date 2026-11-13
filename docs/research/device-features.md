# brickdesk 设备特性调研

- 调研对象：TrimUI Brick Pro（官方固件标识 `TG4040`），并对照 TrimUI Brick（`TG3040`）、TrimUI Smart Pro（`TG5040`）。
- 调研日期：2026-11-12。
- 范围：设备与系统基线、全部用户可见的电源/显示/音频/按键/LED/震动/电池/网络/时间/性能特性、全局按键与前台程序共享输入、启动与退出回桌面、游戏相关能力的扩展接口。
- 方法：阅读 NextUI、MinUI、Knulli、CrossMix 源码；从 TrimUI 官方 Brick Pro 1.1.1 固件包中提取 rootfs、`sys_config.fex`、DTB，核对脚本与二进制字符串；阅读 RetroArch 官方 Network Control Interface 文档。本文只记录事实、路径、命令和行为，不复制上述项目的代码。
- 许可提醒：NextUI 使用 PolyForm Noncommercial 1.0.0；MinUI 没有正式许可证；Knulli 为 GPL 系；CrossMix 为 GPL-3.0。brickdesk 不应把它们的代码复制进来，只能依据接口事实独立实现。

> 说明：本文的“原厂”指 TrimUI 官方 Brick Pro 1.1.1（2026-07-17）固件；“Brick”指官方 Brick 1.1.1（2025-11-26）。固件包内的 rootfs 路径用 `rootfs:/...` 表示，提取方法见附录 A。所有涉及硬件行为的结论都以源码/固件为证据；无法从源码确认的，统一放入“必须在真机上确认的问题”。

---

## 0. 结论摘要

1. Brick Pro 的官方型号是 **TG4040**，不是 Brick 的 TG3040，也不是 Smart Pro 的 TG5040。部分媒体写成 TG4070，与官方固件 `rootfs:/etc/model` 和 `firmware_brickpro` 仓库不一致，需以真机 `cat /etc/model` 为准。
2. Brick Pro 与 Brick、Smart Pro 共用 Allwinner A133P 平台。NextUI 用 **`tg5040`** 平台目录同时支持三者，通过环境变量 `DEVICE=brick|brickpro|smartpro` 区分；MinUI 也只有 **`tg5040`** 目录，但当前源码没有 `brickpro` 分支；Knulli 在 `board/allwinner/a133/` 下分别有 `trimui-brick`、`trimui-brick-pro`、`trimui-smart-pro`；CrossMix 是原厂 MainUI 的增强版，当前主要面向 Smart Pro。
3. 设备的关键接口是：`/dev/input/event*` + 厂商 `trimui_inputd`/`keymon`、`/dev/disp` 亮度 ioctl、`/sys/class/disp/disp/attr/*` 显示增强、`/sys/class/led_anim/*` RGB、GPIO227 + `/sys/class/motor/voltage` 震动、AXP2202 电源/电池 sysfs、`/sys/power/state` 休眠、`wpa_supplicant`/`udhcpc` Wi-Fi、`hciattach` + `bluealsa` 蓝牙、`cpufreq/policy0` 调频。
4. v1 的 P0 应覆盖：启动到桌面、应用启动/退出回桌面、GLES2 显示与亮度、全局按键（音量/亮度/电源）、扬声器与耳机、轻睡眠/深睡眠/唤醒、电池与充电、设置持久化、安全关机/重启、CPU 自动/性能/省电、LED 基础与震动基础。Wi-Fi 连接 UI、蓝牙配对/音频、FN 开关自定义、色温/对比度、LED 氛围、电池历史等放 P1。
5. 存档位、游戏切换器、快速恢复、游戏最近游玩/收藏、着色器/遮罩、金手指、RetroAchievements 属于**扩展协议，不内置**，不列入 P0。NextUI 的实现方式是“保存状态 + 截图 + 退出到启动器 + 重新启动并读状态”，RetroArch 可用 UDP Network Control Interface 做等价控制；brickdesk 只应提供协议和通用应用菜单，不应接管游戏内存或存档格式。
6. 最大风险不在 UI，而在三处：厂商 4.9.191 内核没有公开源码、显示/亮度是私有 `/dev/disp` ioctl、休眠/关机涉及 AXP2202 PMIC 且存在“关机后进入 limbo”问题。必须用真机把这三条路径先跑通，再谈自定义。

---

## 1. 设备型号、平台与差异

### 1.1 型号关系

| 项目 | Brick Pro | Brick | Smart Pro |
|---|---|---|---|
| 官方型号 | **TG4040**（固件 `etc/model`） | **TG3040** | **TG5040** |
| 官方固件仓库 | `trimui/firmware_brickpro` | `trimui/firmware_brick` | `trimui/firmware_smartpro` |
| 本次核对固件 | 1.1.1 / 2026-07-17 | 1.1.1 / 2025-11-26 | 1.1.1 / 2025-11-28 |
| SoC | Allwinner A133P，4×Cortex-A53 | 同左 | 同左 |
| GPU | PowerVR GE8300，最高 660 MHz | 同左 | 同左 |
| 内存 | 1 GB LPDDR3 | 1 GB | 1 GB |
| 屏幕 | 3.95" IPS，1024×768，60 Hz，无触屏 | 3.2" IPS，1024×768 | 5" IPS，1280×720 |
| 电池 | 5000 mAh（官方标称） | 3000 mAh | 以官方规格为准 |
| 摇杆 | 双 Hall 摇杆，L3/R3 可按下 | 无摇杆 | 双摇杆 |
| 额外按键 | Home、F1/F2（NextUI 当作 L4/R4） | F1/F2（NextUI 当作 L3/R3） | 无 Home/F1/F2 的同等布局 |
| LED 区域 | 5 区：`m`、`lr`、`f1`、`f2`、`rear` | 4 区：`m`、`lr`、`f1`、`f2` | 3 区：`l`、`r`、`m` |
| 平台目录 | `tg5040` | `tg5040` | `tg5040` |

证据：官方 Brick Pro 固件包中的 `rootfs:/etc/model`（`TG4040`）、`rootfs:/etc/version`（`1.1.1`）；`rootfs:/usr/trimui/bin/MainUI` 内字符串 `Trimui Brick Pro`；NextUI 的 `DEVICE` 判定与 `FIXED_WIDTH/HEIGHT`；NextUI `hasAnalogSticks()` 明确把 Brick Pro 与 Smart Pro 归为有摇杆、Brick 归为无摇杆。[E2][E4][E8][E41][E50]

> 注意：有评测把 Brick Pro 写成 TG4070，但官方固件和仓库均写 TG4040。这个差异不影响软件，但会影响按型号分包的脚本，真机必须确认。

### 1.2 NextUI / MinUI 用哪个平台目录

- **NextUI**：`workspace/tg5040/` 和 `skeleton/SYSTEM/tg5040/` 同时服务 Brick、Brick Pro、Smart Pro。安装脚本通过读取原厂 `MainUI` 里的 `Trimui ...` 字符串设置 `DEVICE`：
  - `Trimui Brick` → `DEVICE=brick`
  - `Trimui Brick Pro` → `DEVICE=brickpro`
  - 其他 → `DEVICE=smartpro`
  代码见 `skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh` 第 50–57 行、`workspace/tg5040/platform/platform.c` 第 29–34 行。[E5][E2]
- **MinUI**：同样只有 `workspace/tg5040/` 和 `skeleton/SYSTEM/tg5040/`，但当前 `MinUI.pak/launch.sh` 只判断 `Trimui Brick`，平台代码只有 `is_brick`，没有 `is_brickpro`。按源码推断，Brick Pro 会落入 Smart Pro 的 1280×720 分支，不能视为可用的 Brick Pro 支持；需要真机或上游后续版本确认。[E18][E19]
- **Knulli**：`board/allwinner/a133/trimui-brick/`、`trimui-brick-pro/`、`trimui-smart-pro/` 分开；Brick Pro 已有独立 board 目录与内核配置，但发布包是否已正式列出 Brick Pro 需查其 releases。[E31]
- **CrossMix**：不是独立平台移植，而是原厂 MainUI 的增强层；当前 README/发布说明主要面向 Smart Pro，v1.3 明确不兼容 Brick；其 `inputd_switcher.sh` 对 Brick 直接提示 “Feature not supported yet on brick”。[E36][E49]

### 1.3 固件、内核与外设差异

- 三台机器都是 Allwinner A133P（DTB `compatible = "allwinner,a133"`，`model = "sun50iw10"`），内核为厂商 **4.9.191**（rootfs 的 `/lib/modules/4.9.191/`，Knulli 同样使用 4.9.191 模块）。厂商没有公开内核/U-Boot 源码，Knulli 文档也明确这一点。
- Brick Pro 的 rootfs 与 Brick 的 rootfs 结构几乎相同，主要差异是：
  - `/etc/model` 为 `TG4040`，`MainUI` 型号字符串为 `Trimui Brick Pro`；
  - LED 多一个 `rear` 区，`PLAT_getNumLeds()` 返回 5（Brick 4，Smart Pro 3）；
  - 有双摇杆、L4/R4 与 Home 的映射；
  - 震动马达限压不同：NextUI 对 Brick Pro 用 2.5 V，对 Brick/Smart Pro 用 3.3 V；NextUI 的 FN 静音震动对 Brick Pro 用 900000，对 Brick 用 1500000；
  - 屏幕更大但分辨率和亮度表相同（NextUI 的 `scaleBrightness()` 对 Brick/Brick Pro 使用同一张 0–10 → 0–255 表）；
  - Brick Pro 额外有双扬声器、3.5 mm 耳机/麦克风、USB-C DP Alt Mode（官方标称最高 1080p60）和底部 Reset 按键；这些不是桌面 v1 的必需项，但属于设备外设差异，需在能力探测中识别。[E50]
- 外设相同点：AXP2202 PMIC、`/sys/class/led_anim/*`、GPIO227 马达、`/sys/class/speaker/mute`、耳机插入 `SW_HEADPHONE_INSERT`、Wi-Fi `wlan0`、蓝牙 `hci0`、`/dev/disp`。
- 外设不确定点：Wi-Fi/蓝牙芯片与 `hciattach` 变体在官方 rootfs 内自相矛盾（见 3.7），这是 Brick Pro 最需要在真机确认的硬件差异。

---
### 1.4 原厂 MainUI 的可见功能清单（矩阵补全）

以下直接来自原厂 `rootfs:/usr/trimui/res/lang/en.lang` 的键值，矩阵只挑与桌面实现相关的项；完整清单可用于检查“设备该有的功能”是否漏项。[E46]

- 首页/内容：Games、Favorites、Apps、Recent、File、Store、Themes、Bests、Ports。
- 设置主项：Language、Volume、BGM Volume、Brightness、Refresh Roms、Factory Reset、Flash Mode、Device Info、Hardware Test、ADC Info、Keymap。
- 网络：Wi-Fi、Hotspot、Bluetooth、Netplay、Reset Bluetooth Service、SSH。
- 显示：Color Temperature、Contrast、Saturation、Exposure、LED（Top / Taillight / F1/F2 / Joystick scale、LED Policy）。
- 系统：Date And Time、24-hour clock、Network Synchronization Time、Time Zone、USB Mode（Host/Dock/Data）、Homepage（Tab Visibility、Focus on Tab Bar）、OSD Shortcut、Confirmation Button、UI Use Joystick、Dead Zone、Lock Screen Time、Rumble、Volume&Rumble、Performance Settings（Mode Presets / Power Saving / Balanced / Performance / GPU Max Frequency / min-max frequency / Fan）、Enable SSH、Enable Keytone、Speaker/Headphone Channel Swap、Left/Right Channel Volume、Speaker/Headphone Gain、Microphone Switch。
- 游戏内：Continue、Save、Load、Volume、Exit Game、Record、Slot saved、Fullscreen On/Off、Delete ROM、Add To Favorites、Clear Recents/Favorites。

### 1.5 原厂后台进程

| 进程 | 作用 | 证据 |
|---|---|---|
| `MainUI` | 启动器、游戏列表、设置、Wi-Fi/蓝牙/商店/主题；退出后由 `runtrimui.sh` 执行应用命令再重启。 | `rootfs:/usr/trimui/bin/MainUI`；`runtrimui.sh:129-274`。[E41] |
| `keymon` | 全局按键、电源/休眠、亮度/音量、FN 开关、低电量/充电提示、OSD、马达脉冲。 | `rootfs:/usr/trimui/bin/keymon` 字符串；`low_battery_led.sh`。[E41] |
| `trimui_inputd` | 读物理 evdev，创建 `/dev/uinput` 虚拟输入，处理 grab、DPad→摇杆、Turbo、校准。 | `rootfs:/usr/trimui/bin/trimui_inputd` 字符串；`/tmp/trimui_inputd/*`。[E41] |
| `trimui_scened` | 场景/性能状态（与 `nativepower` 配置配合）。 | `rootfs:/usr/trimui/bin/trimui_scened`；`etc/config/nativepower`。[E41] |
| `trimui_btmanager` | 蓝牙管理/配对/音频。 | `runtrimui.sh:168`、`bt_init.sh`。[E41] |
| `hardwareservice` | 设置持久化、LED/马达/静音/Wi-Fi/BT 开关、亮度、电池温度。 | `rootfs:/usr/trimui/bin/hardwareservice` 字符串；`/mnt/UDISK/system.json`。[E41] |
| `trimui_osdd` | OSD 绘制/提示（音量、亮度、电量、FN 状态）。 | `runtrimui.sh:171-172`；`rootfs:/usr/trimui/osd`。[E41] |
| `musicserver` | 背景音乐播放。 | `runtrimui.sh:170`、`en.lang` BGM Volume。[E41][E46] |

---

## 2. 特性矩阵

优先级定义：

- **P0**：v1 必须；缺失会导致桌面不可用或无法安全关机/休眠。
- **P1**：v1 应该有；影响体验和“设备该有的功能”，但可在 P0 稳定后补齐。
- **P2**：可后置；偏个性化、诊断、外部显示或高阶扩展。
- **扩展协议（不内置）**：游戏/应用专属能力；桌面只提供通用菜单与协议，不实现存档位、游戏切换器、快速恢复等。

> 表中“原厂”列同时给出 MainUI 和后台进程的做法；“Knulli/CrossMix”只在有差异时展开。证据编号见文末索引。

| 特性 | NextUI / 原厂 / Knulli / CrossMix 的做法 | 设备接口与证据 | 优先级 |
|---|---|---|---|
| **启动** 开机到桌面 | NextUI 由官方 `runtrimui.sh` 包装器/更新流程进入 `MinUI.pak/launch.sh` 的 `while` 循环；原厂 `init.d/runtrimui` 启动 `runtrimui.sh`，后者拉起 keymon/inputd/scened/btmanager/hardwareservice/osdd，再运行 MainUI；Knulli 由 `S31emulationstation` 启动 ES；CrossMix 沿用原厂。 | `rootfs:/etc/init.d/runtrimui`、`rootfs:/usr/trimui/bin/runtrimui.sh:129-274`；NextUI `MinUI.pak/launch.sh:182-209`；Knulli `S31emulationstation`。[E5][E13][E41][E52] | P0 |
| **启动** 应用/游戏启动与退出回桌面 | NextUI：`nextui.elf` 把命令写入 `/tmp/next` 后退出，`MinUI.pak` 循环执行命令；游戏 Pak 执行 `minarch.elf core.so ROM`，退出后循环重新启动 `nextui.elf`。原厂：MainUI 退出后，`runtrimui.sh` 执行 `/tmp/cmd_to_run.sh`，然后回到 MainUI。Knulli：ES 直接拉起 RetroArch/独立模拟器，退出由 ES 接管。CrossMix：原厂循环 + `common_launcher.sh`。 | NextUI `nextui.c:1086-1090`、`MinUI.pak/launch.sh:182-209`；原厂 `runtrimui.sh:265-274`；CrossMix `common_launcher.sh:68-76`。[E5][E13][E37][E41] | P0 |
| **启动** 启动器/应用列表 | NextUI：Games、Favorites、Recents、Tools、Collections、搜索、文件夹背景、游戏封面；原厂：Games、Favorites、Apps、Recent、File、Store、Themes；Knulli：ES 游戏列表；CrossMix：原厂 UI + 增强启动器。 | NextUI `nextui.c` 目录/入口逻辑、`settings.cpp` 的显示选项；原厂 `en.lang` 键 0–20、139–143；NextUI README。[E8][E46][E47] | P0（外壳）/P1（封面与美化） |
| **启动** 通用应用菜单 | 原厂没有“桌面覆盖在游戏上”的菜单；RetroArch 有自己的菜单；Knulli 用 ES 的 HTTP 接口控制前台。建议 brickdesk 只提供“应用主动调用”的通用菜单，不做跨进程覆盖。 | 见 3.8；Knulli `dpad-toggle`/`softreset` 使用 `localhost:1234`。[E29][E30] | P1（协议） |
| **启动** 启动画面/主题/字体 | NextUI 支持 bootlogo Pak、主题色、字体、调色板、菜单动画；原厂有主题、语言、首页 Tab、OSD 皮肤；Knulli/CrossMix 有 ES/原厂主题。 | NextUI `settings.cpp:477-556`、`skeleton/EXTRAS/Tools/tg5040/Bootlogo.pak`；原厂 `en.lang` 键 108、211–218；`rootfs:/usr/trimui/res`。[E8][E46] | P2 |
| **电源** 短按电源键休眠/息屏 | NextUI：短按进入 hybrid sleep（先息屏，超时后深睡），1 秒长按关机；原厂：keymon 处理电源键，息屏/休眠/长按关机（有 6 秒关机字符串，阈值需真机确认）；Knulli：`power-button` 短按 `knulli-suspend`，2 秒长按关机；CrossMix 沿用原厂。 | NextUI `api.c:4170-4210`；原厂 keymon 字符串 `keymon power value`、`6seconds poweroff`；Knulli `power-button:24-55`。[E1][E20][E41] | P0 |
| **电源** 轻睡眠（息屏） | NextUI：`PWR_enterSleep()` 暂停音频、LED 进入 sleep profile、背光 0、STOP keymon/batmon/audiomon，等待唤醒；原厂：keymon 先 dim 再关 LCD；Knulli：`10-hide-ui` 息屏+静音；CrossMix 用 `lcd_off.sh`。 | NextUI `api.c:4341-4374`；原厂 keymon 字符串 `dim lcd`、`turn off lcd`；Knulli `10-hide-ui:5-9`；CrossMix `lcd_off.sh:6-14`。[E3][E32][E41][E51] | P0 |
| **电源** 深睡眠（suspend-to-RAM） | NextUI：`suspend` 脚本停止 wpa_supplicant/bluetoothd、保存 ALSA、写 `mem > /sys/power/state`，失败重试 5 次；原厂：keymon 写 `/tmp/system_suspend` 并 `echo mem > /sys/power/state`；Knulli：`pm-suspend`，可配置 fake suspend；CrossMix 沿用原厂。 | NextUI `skeleton/SYSTEM/tg5040/bin/suspend:16-99`；原厂 keymon 字符串 `system suspend`、`echo mem`；Knulli `knulli-suspend:123-157`。[E6][E21][E41] | P0 |
| **电源** 唤醒 | NextUI：`PAD_wake()` 等电源键，恢复背光/音量/音频、LED profile、CONT 被 STOP 的守护进程；原厂：keymon 处理 `power key up, turn on lcd`；Knulli：pm-utils resume hook 恢复亮度/音量/Wi-Fi；CrossMix 同原厂。 | NextUI `api.c:4375-4404`；原厂 keymon 字符串；Knulli `10-hide-ui:10-28`、`99-xradio-wlan`。[E3][E22][E25][E41][E51] | P0 |
| **电源** 关机/重启 | NextUI：写 `/tmp/poweroff` 或 `/tmp/reboot`，由 `MinUI.pak` 调 `poweroff_next`/`reboot_next`；原厂：`poweroff`/`reboot`；Knulli：`knulli-shutdown -s/-r`；CrossMix 同原厂。 | NextUI `platform.c:235-256`、`MinUI.pak/launch.sh:201-207`；Knulli `knulli-shutdown:13-58`。[E2][E5][E28] | P0 |
| **电源** 安全关机（避免 limbo） | NextUI 提供 “Safe poweroff” 设置：卸载 SD、杀进程、通过 `/dev/i2c-6` 对 AXP2202 写寄存器做软断开，失败再走标准关机；原厂没有该保护；Knulli 用 `shutdown -Ph` + 20 秒兜底。 | NextUI `poweroff_next.c:279-317`、`settings.cpp:702-710`；Knulli `knulli-shutdown:41-57`。[E17][E8][E28] | P0（目标）/P1（可先标准关机） |
| **电源** 屏幕超时/休眠超时 | NextUI：屏幕超时默认 60 s、休眠超时默认 30 s（README 又说深睡在 2 分钟后，存在版本差异）；原厂：`dimtime` 设置 + keymon 的 dim/睡眠状态机；Knulli：idle watcher/battery saver；CrossMix 同原厂。 | NextUI `config.h:230-231`、`api.c:4191-4210`；原厂 `hardwareservice` 字符串 `dimtime`；Knulli `knulli-suspend:72-113`。[E2][E41][E48] | P1 |
| **电池** 电量/充电状态 | NextUI：`capacity` + `time_to_full_now` + `usb/online` 判断充电，UI 显示电量/充电；原厂：MainUI 读 `axp2202-battery/capacity`、`status`、`axp2202-usb/online`；Knulli/CrossMix 同原厂或 batteryplus。 | NextUI `platform.c:180-190`；原厂 `hardwareservice` 字符串；Knulli `15-batteryplus-wakeup`。[E2][E41] | P0 |
| **电池** 低电量提示/自动关机 | NextUI：低电量 LED/提示、电池历史与剩余时间（Battery Pak）；原厂：keymon 调 `low_battery_led.sh`、`show_battery_low_msg.sh`，并有 6 秒关机字符串；Knulli：低电量提示；CrossMix 同原厂。 | 原厂 keymon 字符串 `low_battery_led.sh`、`show_battery_low_msg.sh`、`6seconds poweroff`；`rootfs:/usr/trimui/bin/low_battery_led.sh`。[E41] | P1（提示）/P0（安全关机） |
| **电池** 电池历史/剩余时间 | NextUI Battery Pak 有历史图和时间预测；原厂只有百分比/温度；Knulli batteryplus；CrossMix 原厂。 | NextUI `skeleton/EXTRAS/Tools/tg5040/Battery.pak`；原厂 `en.lang` 键 40–43、185–189。[E41][E46] | P2 |
| **显示** 1024×768 输出/缩放 | NextUI 固定 `1024×768`、`FIXED_SCALE=3`，用 SDL2 + OpenGL ES；原厂 MainUI 也按 1024×768；Knulli 用 ES + SDL2；CrossMix 原厂。 | NextUI `platform.h:140-146`；原厂 DTS `lcd_x=0x400`、`lcd_y=0x300`；NextUI `generic_video.c:652-692`。[E2][E4][E14] | P0 |
| **显示** 亮度 | NextUI：0–10，写 `/dev/disp` ioctl `0x102`，参数 `[0, raw, 0, 0]`；原厂：keymon/hardwareservice 同样 `setLCDBrightness`；Knulli：`knulli-brightness` 优先 `/sys/class/backlight/*`，否则 `brightness` 助手；CrossMix：`dispdbg setbl`。 | NextUI `msettings.c:1207-1228`、`scaleBrightness():1055-1102`；原厂 keymon 字符串 `/dev/disp`；Knulli `knulli-brightness:11-42`；CrossMix `lcd_on.sh:8-27`。[E3][E32][E41] | P0 |
| **显示** 色温/对比度/饱和度/曝光 | NextUI 设置里可调，写 `/sys/class/disp/disp/attr/*`；原厂 “Display” 菜单同样四项；Knulli 有色彩管理；CrossMix 原厂。 | NextUI `msettings.c:1220-1228、1393-1418`；原厂 `hardwareservice` 字符串 `/sys/devices/virtual/disp/disp/attr/...`。[E3][E41] | P1 |
| **显示** 白点校正 | NextUI 有 “White point correction” 与 RGB gain（DisplayCal）；原厂没有；Knulli 有色彩管理；CrossMix 无。 | NextUI `settings.cpp:597-635`、`displaycal.h`。[E8] | P2 |
| **显示** 屏幕关闭/恢复 | NextUI 背光 0；原厂 `dispdbg setbl 0`；Knulli `fb0/blank` 兜底；CrossMix `lcd_off.sh`/`lcd_on.sh`。 | CrossMix `lcd_off.sh:6-14`、`lcd_on.sh:18-27`；Knulli `knulli-brightness:177-210`。[E23][E32] | P0 |
| **显示** 旋转/撕裂/帧率 | NextUI 目标 60.235 Hz、严格 vsync；原厂 `MainUI` 有 `rotate 270` 痕迹；Knulli 用 `setalpha 0`、SDL compat；CrossMix 原厂。 | NextUI `platform.h:159-160`、`generic_video.c:652-692`；原厂 MainUI 字符串 `rotate 270`；Knulli `S13trimuialpha`。[E2][E14][E41] | P0（旋转/撕裂必须真机确认） |
| **显示** USB-C DP 外接 | Brick Pro 官方支持 DP Alt Mode，最高 1080p60；NextUI/原厂桌面未发现外接显示 UI；Knulli 有 HDMI/DRM 相关支持（其他机型）；CrossMix 无。 | 官方规格 [E50]；需真机确认 `/sys/class/drm`、DP 音频/热插拔。 | P2 |
| **音频** 扬声器音量 | NextUI：0–20，写 tinyalsa `digital volume`（反向映射）与 `DAC volume`，并在 0 时写 `/sys/class/speaker/mute`；原厂：MainUI/`shmvar vol`，`tinymix set 9 1`、`tinymix set 1 0`；Knulli `knulli-audio`；CrossMix 原厂。 | NextUI `msettings.c:1309-1391`；原厂 `runtrimui.sh:209-212`、`rc.local:4-7`；Knulli `knulli-audio`。[E3][E41] | P0 |
| **音频** 静音/扬声器底噪 | NextUI：`/sys/class/speaker/mute` 1/0；原厂 FN “Silent Mode” 也写该节点；Knulli 音量服务；CrossMix 原厂。 | 原厂 `com.trimui.silent.sh:5-13`；NextUI `msettings.c:1389`。[E41][E3] | P0 |
| **音频** 耳机插入/切换 | NextUI：keymon 监听 `EV_SW` code 2，`SetJack` 后切换耳机/扬声器音量；原厂：`SW_HEADPHONE_INSERT` 由 keymon 处理，`rc.local` 初始化 Headphone 控件；Knulli：udev/triggerhappy 调 `trimui-volume-attenuate`；CrossMix 原厂。 | NextUI `keymon.c:150-155`、`msettings.c:780-803`；原厂 `rc.local:4-7`；Knulli `multimedia_keys.conf:15-16`、`trimui-volume-attenuate`。[E1][E3][E20][E41] | P0 |
| **音频** 蓝牙音频 | NextUI：`bluealsa` + `amixer -D bluealsa` A2DP 控制；原厂：`bluealsa` + `trimui_btmanager`；Knulli：PipeWire/bluealsa；CrossMix 原厂。 | NextUI `msettings.c:1230-1269`、`bt_init.sh:54-71`；原厂 `bt_init.sh`；Knulli `S31emulationstation`/`knulli-audio`。[E15][E41][E48] | P1 |
| **音频** USB DAC | NextUI：寻找第一个非 `audiocodec` 的声卡，按 `PCM/Playback + Volume` 控件设音量；原厂无专门 UI；Knulli 可切音频设备；CrossMix 无。 | NextUI `msettings.c:1271-1357`。[E3] | P2 |
| **音频** 声道交换/增益/麦克风 | 原厂 “Speaker Channel Swap / Headphone Channel Swap / Left-Right Volume / Speaker Gain / Headphone Gain / Microphone Switch”；NextUI 没有全部暴露；Knulli/CrossMix 原厂或 ALSA。 | 原厂 `en.lang` 键 236–242；`hardwareservice` 字符串。[E41][E46] | P2 |
| **按键** 音量+/- | NextUI：keymon 读 `KEY_VOLUMEUP=115`/`KEY_VOLUMEDOWN=114`，按 `GetVolume/SetVolume` 调整并显示 OSD；原厂：keymon 同样处理；Knulli：triggerhappy 调 `volume-button`；CrossMix 原厂。 | NextUI `keymon.c:27-33、166-227`；Knulli `multimedia_keys.conf:1-8`；CrossMix `button_state.sh:31-43`。[E1][E20][E32] | P0 |
| **按键** Menu+音量调亮度 | NextUI：`Menu`(316)+音量，`BTN_MOD_BRIGHTNESS=BTN_MENU`；keymon 在 `menu_pressed` 时调亮度；原厂：keymon 的 `menu_pressed`/`select_pressed` 分支；Knulli：`KEY_VOLUMEUP+BTN_MODE` → `volume-button briup`；CrossMix：OSD 快捷键列表含 MENU+…。 | NextUI `keymon.c:160-227`、`platform.h:132-136`；Knulli `multimedia_keys.conf:5-8`；CrossMix `osd_hotkey.sh:3-27`。[E1][E4][E20][E35] | P0 |
| **按键** Select+音量调色温 | NextUI：`BTN_MOD_COLORTEMP=BTN_SELECT`，keymon 的 `menu2_pressed` 分支；原厂：keymon 的 select 分支；Knulli 无直接色温组合；CrossMix 原厂。 | NextUI `keymon.c:160-227`、`platform.h:134`；原厂 keymon 字符串 `select_pressed`。[E1][E4][E41] | P1 |
| **按键** Menu 打开游戏内菜单 | NextUI：minarch `PAD_justReleased(BTN_MENU)` 打开菜单；Menu+Select 截图+存档+退出到切换器；原厂：MainUI/游戏内 OSD；Knulli：RetroArch 自己的菜单；CrossMix：原厂 OSD。 | NextUI `ma_input.c:17-38、178-180`；原厂 `en.lang` 键 91、126；CrossMix `osd_hotkey.sh`。[E10][E35][E46] | P1（通用菜单协议） |
| **按键** 电源键 | NextUI：`BUTTON/CODE/JOY_POWER`，短按睡眠、1 秒长按关机，唤醒后 1 秒内忽略误触；原厂：keymon 电源状态机 + PMU；Knulli：短按 suspend、2 秒长按 shutdown；CrossMix 原厂。 | NextUI `platform.h:43-45、129-131`、`api.c:4170-4210`；Knulli `power-button`；原厂 keymon 字符串。[E2][E4][E20][E41] | P0 |
| **按键** FN 拨动开关 | NextUI：`/sys/class/gpio/gpio243/value` 作为 mute 状态，触发备用音量/亮度/色温、DPad→摇杆、Turbo；原厂：keymon 读同一 GPIO，FN Editor 可把开关映射为 LED 关、Quiet、Silent、CPU 省电/性能、Turbo；Knulli：`SW_TABLET_MODE` → `toggle-switch`，可配置为静音/RGB/飞行模式；CrossMix 原厂。 | NextUI `keymon.c:42、68-99`、`msettings.c:814-845`；原厂 `runtrimui.sh:43-45`、`fnswitch_scripts.json`；Knulli `S99toggle-switch:28-55`、`multimedia_keys.conf:13-14`。[E1][E3][E20][E26][E41] | P1 |
| **按键** F1/F2/Home/L4/R4 | NextUI：Brick 的 F1/F2 映射为 L3/R3；Brick Pro 的 F1/F2 映射为 L4/R4，Home 为 BTN_FN3，且可分配“启动某个 Pak”；原厂：F1/F2 有独立 `f1key.json`/`f2key.json` 与 `f1key_launch`/`f2key_launch`，可执行 FN Editor 脚本；Knulli：无同等按键盘位；CrossMix 原厂。 | NextUI `platform.h:110-115`、`fnbuttonmenu.cpp:123-179`；原厂 keymon 字符串 `KEY_F1/KEY_F2`、`f1key.json`、`f2key.json`。[E4][E9][E41] | P1 |
| **按键** 自定义组合键/快捷键 | NextUI：只有 FN 按钮→Pak 的简单分配；原厂：FN switch/F1/F2 的动作注册表（CPU、亮度、LED、Turbo、DPad）；Knulli：triggerhappy 规则可扩展；CrossMix：`osd_hotkey` 只覆盖 RetroArch OSD。 | 原厂 `fnkey_scripts.json`、`fnswitch_scripts.json`、`com.trimui.*.sh`；Knulli `multimedia_keys.conf`；CrossMix `osd_hotkey.sh`。[E20][E35][E41] | P1（brickdesk 应做成通用动作注册表） |
| **按键** A/B/X/Y/L/R/Select/Start/Menu 原始码 | CrossMix `button_state.sh`（沿用原厂事件码）：A=305、B=304、X=308、Y=307、L=310、R=311、SELECT=314、START=315、MENU=316，设备节点 `/dev/input/event3`；NextUI platform 用 SDL joystick 映射 A=1、B=0、X=3、Y=2、L1=4、R1=5、SELECT=6、START=7、MENU=8；Knulli triggerhappy 用 `BTN_MODE` 等；CrossMix 同原厂事件码。 | CrossMix `button_state.sh:28-43`；NextUI `platform.h:84-106`；Knulli `multimedia_keys.conf`。[E4][E20][E32] | P0 |
| **按键** 摇杆/十字键/死区/校准 | Brick Pro 有双 Hall 摇杆；NextUI 通过 SDL joystick 读轴，Brick 无摇杆；原厂有 `ADC INFO`、`Calibrate Joystick`、`Dead Zone`；Knulli 有 `joypad.config` 校准；CrossMix 原厂。 | NextUI `hasAnalogSticks()`；原厂 `en.lang` 键 20、152–155、228–231；Knulli `joypad.config`。[E8][E41][E46] | P1 |
| **按键** DPad→摇杆/Turbo | 原厂 `trimui_inputd` 的 `/tmp/trimui_inputd/input_no_dpad`、`input_dpad_to_joystick`、`turbo_*`；NextUI 也写这些文件；Knulli `dpad-toggle` 用同样文件；CrossMix 原厂。 | 原厂 `com.trimui.dpad*.sh`、`com.trimui.turbo_*.sh`；NextUI `msettings.c:825-844`、`platform.c:604-658`；Knulli `dpad-toggle:46-61`。[E2][E3][E29][E41] | P1 |
| **按键** 键位重映射（ABXY 等） | 原厂 “Keymap” 写 `/sys/module/gpio_keys_polled/parameters/button_config`；NextUI 没有同等全局重映射；Knulli 有 ES 输入配置；CrossMix 原厂。 | 原厂 MainUI 字符串 `/sys/module/gpio_keys_polled/parameters/button_config`、`setup_keymap`；`KeymapHandler`。[E41] | P2（风险高，v1 不做） |
| **LED** 开关/亮度 | NextUI：`/sys/class/led_anim/max_scale`、`max_scale_lr`、`max_scale_f1f2`、`max_scale_rear`；原厂：`max_scale`、`ledswitch`、`/tmp/system/enable_led`；Knulli：RGB 守护进程/`knulli-power-led`；CrossMix：原厂。 | NextUI `platform.c:478-598`；原厂 `runtrimui.sh:94-103`、`init_leds.sh`、`hardwareservice` 字符串；Knulli `toggle-switch`。[E2][E41] | P0（基础开关/亮度） |
| **LED** 分区/颜色/效果 | NextUI：`effect_rgb_hex_*`、`effect_*`、`effect_cycles_*`、`effect_duration_*`；原厂：同族节点 + `frame_hex`、`anim_frames_enable`；Knulli：RGB 模式；CrossMix：原厂。 | NextUI `platform.c:547-598`；原厂 `runtrimui.sh:94-103`、`low_battery_led.sh`、`init_leds.sh`；Knulli `knulli-rumble`/RGB。[E2][E41] | P1（效果与分区动画） |
| **LED** 低电量/充电/睡眠指示 | NextUI：低电量/充电/standby 指示，可单独设亮度；原厂：低电量红色闪烁、充电提示；Knulli：power LED / battery 指示；CrossMix：原厂。 | 原厂 `low_battery_led.sh`；NextUI README “LED indicators”；Knulli `99-powerled`。[E41][E47][E48] | P1 |
| **LED** 氛围灯（Ambient） | NextUI 支持按屏幕颜色驱动 LED，可按模拟器配置；原厂无；Knulli 有 RGB 模式；CrossMix 有 Ambilight。 | NextUI README；Knulli changelog；CrossMix release notes。[E47][E48][E49] | P2 |
| **震动** 菜单/系统反馈 | NextUI：菜单/睡眠/开机震动，强度可配；原厂：`rumblelevel` + hardwareservice；Knulli：`knulli-rumble`；CrossMix 原厂。 | NextUI `platform.c:286-307`、`api.c:236-238`；原厂 `hardwareservice` 字符串；Knulli `knulli-rumble:33-42`。[E2][E20][E41] | P0（基础） |
| **震动** 游戏内震动 | NextUI：minarch 把核心震动传到 `PLAT_setRumble`；原厂：输入/马达；Knulli：部分设备/模拟器支持；CrossMix：RetroArch。 | NextUI `platform.c:293-307`；原厂 GPIO227/马达；Knulli `knulli-rumble`。[E2][E20][E41] | P1 |
| **网络** Wi-Fi 开关/连接 | NextUI：`wpa_cli` + `wpa_supplicant` + `udhcpc`，Network 菜单；原厂：MainUI Wi-Fi/Hotspot；Knulli：ConnMan；CrossMix 原厂。 | NextUI `generic_wifi.c:24-258`、`wifi_init.sh:6-37`；原厂 `etc/init.d/wpa_supplicant`、`wpa_supplicant.conf`；Knulli `connman`。[E16][E41] | P1（开关/状态可 P0） |
| **网络** Wi-Fi 热点 | 原厂支持 Hotspot；NextUI 没有热点菜单；Knulli 有网络共享；CrossMix 原厂。 | 原厂 `en.lang` 键 114–116、125；`hostapd`、`dnsmasq`/`udhcpd`。[E41][E46] | P2 |
| **网络** 蓝牙开关/配对 | NextUI：`bluetoothctl` + `bluealsa`，Bluetooth 菜单；原厂：`trimui_btmanager`；Knulli：BlueZ/PipeWire；CrossMix 原厂。 | NextUI `generic_bt.c`、`bt_init.sh`；原厂 `bt_init.sh`、`trimui_btmanager`；Knulli `S03modules`/`knulli-audio`。[E15][E41][E48][E52] | P1（开关/状态可 P0） |
| **网络** SSH/ADB/文件共享 | 原厂有 SSH 开关、ADB、USB 模式；CrossMix 有 SSH/FTP/Samba；NextUI 没有 SSH 菜单；Knulli 有 SSH/SFTP/Samba。 | 原厂 `en.lang` 键 197–204、232–233；`etc/init.d/adbd`；CrossMix README；Knulli changelog。[E41][E46][E49] | P2 |
| **时间** 时钟/日期/时区/NTP | NextUI：时钟、24h、NTP、时区，写 `date`/`hwclock`/`uci`；原厂：Date And Time、24-hour、Network Synchronization Time、Time Zone；Knulli/CrossMix 系统级。 | NextUI `platform.c:662-826`、`settings.cpp:660-683`；原厂 `en.lang` 键 171–182；`sysfixtime`/`sysntpd`。[E2][E8][E41] | P1（P0 显示时钟） |
| **性能** CPU 调速器/频率 | NextUI：`auto`=schedutil，`performance`=performance，`powersave`=conservative；原厂：ondemand/performance/interactive，FN Editor 用 1008/1200/1416/1608/1800/2000；Knulli：电源管理；CrossMix：`cpufreq.sh` 0–8 档 + 核心上线。 | NextUI `governor.sh:40-57`；原厂 `com.trimui.*.sh`、`preload.sh:3-5`；CrossMix `cpufreq.sh:8-34、100-122`。[E7][E33][E41] | P0（auto/performance/powersave） |
| **性能** CPU 频率表 | DTS OPP：408/600/816/1008/1200/1320/1416/1464/1512/1608/1800/2000 MHz；CrossMix 只用 408/600/816/1008/1200/1416/1608/1800/2000；NextUI 注释 408–1800/2000。 | 官方 DTB `opp-hz`；CrossMix `cpufreq.sh`；NextUI `governor.sh`。[E7][E33][E45] | P1（自定义档位） |
| **性能** 温度/风扇/GPU | NextUI：CPU `thermal_zone0`、GPU `thermal_zone2`，GPU 固定 660 MHz；原厂：电池温度、Performance Settings、Fan（仅其他型号）；Brick/Brick Pro 无风扇；Knulli：fan control（其他机型）。 | NextUI `platform.c:123-138`；原厂 `static_temperature/refresh.sh`、`hardwareservice` 字符串；NextUI `hasActiveCooling()` 仅 tg5050。[E2][E8][E41] | P2（监控）/N/A（风扇） |
| **存储/设置** 配置持久化 | NextUI：`msettings.bin` + `config` 文件在 `.userdata`；原厂：`/mnt/UDISK/system.json`；Knulli：`/userdata/system`；CrossMix：原厂。 | NextUI `msettings.c:333-550`；原厂 `hardwareservice` 字符串 `/mnt/UDISK/system.json`、`lcd_on.sh:9`；Knulli `/userdata`。[E3][E41] | P0 |
| **工具** 截图/录屏 | NextUI：Menu+Select 截图、`Menu_screenshot`；原厂：OSD/Record；Knulli：`knulli-screenshot`；CrossMix：`screenshot.sh`。 | NextUI `ma_input.c:30-38`、`ma_menu.c:1659-1715`；Knulli `knulli-screenshot`；CrossMix `screenshot.sh`。[E10][E30][E41] | P1 |
| **工具** Paks/Apps/Tools | NextUI：`Tools/*.pak`、Pak Store；原厂：Apps/Emus；Knulli：Ports/ES；CrossMix：Apps/PortMaster。 | NextUI `settings.cpp:1138-1153`、`fnbuttonmenu.cpp`；原厂 `en.lang` 键 2、136–139、196；CrossMix README。[E8][E9][E46][E49] | P1 |
| **游戏专属** 存档位 | NextUI：minarch 1–8 用户槽（内部 0–7），另有自动槽 9；原厂/RetroArch 自己管理；Knulli/CrossMix 用 RetroArch。 | NextUI `ma_menu.c:1598-1760`、`ma_saves.c:159-197、371-385`。[E11][E12] | **扩展协议，不内置** |
| **游戏专属** 游戏切换器 | NextUI：Menu+Select 保存状态+截图+退出，NextUI 显示 recents 轮播，选择后设置 resume slot 再启动；原厂无；Knulli 未发现 NextUI 式切换器；CrossMix 未发现。 | NextUI `ma_input.c:30-38`、`nextui.c:2460-2510`；`GAME_SWITCHER_PERSIST_PATH`。[E10][E13] | **扩展协议，不内置** |
| **游戏专属** 快速恢复/自动续玩 | NextUI：`Menu_beforeSleep` 写 `AUTO_RESUME_PATH` + 自动槽 9，下次启动 `autoResume()` 直接拉起；原厂无；CrossMix “Resume at boot”（基于 RetroArch）；Knulli 未发现同类。 | NextUI `ma_menu.c:221-232`、`nextui.c:1182-1216`；CrossMix wiki/release notes。[E12][E13][E49] | **扩展协议，不内置** |
| **游戏专属** 最近游玩/收藏 | NextUI：Recents 最多 24 条、Favorites；原厂：Recent/Favorites；Knulli/CrossMix 有游戏列表收藏。 | NextUI `nextui.c:479-512`；原厂 `en.lang` 键 1、18、44、55、77–82。[E13][E46] | **扩展协议，不内置**（通用“最近应用”可 P2） |
| **游戏专属** 着色器/遮罩/金手指/RA | NextUI：shader/overlay/cheat/RetroAchievements；原厂：RetroArch 自带；Knulli：RetroArch/Bezel；CrossMix：overlay/RA。 | NextUI README、`settings.cpp:891-1093`；RetroArch 自身功能。[E8][E47] | **扩展协议，不内置** |
| **游戏专属** 通用应用内菜单 | 建议由前台应用绘制或通过 Unix socket 请求桌面显示；桌面不要尝试覆盖 GLES 全屏画面。RetroArch 可用 `MENU_TOGGLE`；Knulli 用 ES HTTP 接口。 | 见 3.8；RetroArch NCI；Knulli `softreset`/`dpad-toggle`。[E29][E30][E39] | P1（协议）/不内置游戏逻辑 |

---

## 3. 关键接口与实现

### 3.1 启动、应用生命周期与退出回桌面

**NextUI 的循环**

1. `runtrimui.sh` 在 SD 卡挂载后执行更新/安装，最后调用 `.system/tg5040/paks/MinUI.pak/launch.sh`。
2. `launch.sh` 建立环境变量（`PLATFORM=tg5040`、`SDCARD_PATH`、`SYSTEM_PATH`、`USERDATA_PATH` 等），启动 `trimui_inputd`、`keymon.elf`、`batmon.elf`、`audiomon.elf`，初始化 Wi-Fi/蓝牙，然后 `touch /tmp/nextui_exec` 并进入循环。
3. 循环体：运行 `nextui.elf`；如果 `nextui.elf` 写了 `/tmp/next`，读取命令、跑 `pre-launch.d` hooks、`eval` 命令、跑 `post-launch.d` hooks，然后删除 `/tmp/next`；循环再次启动 `nextui.elf`。
4. `nextui.elf` 的 `queueNext()` 只是 `putFile("/tmp/next", cmd)` 并 `quit=1`；真正启动发生在 shell 循环里。游戏 Pak 的 `launch.sh` 执行 `minarch.elf <core.so> <ROM>`；原生应用 Pak 执行自己的 `launch.sh`。**NextUI 默认不是启动 RetroArch，而是运行自己的 libretro 前端 minarch；原厂/CrossMix 才是 `ra64.trimui`/RetroArch 路线。**
5. 退出游戏后 `minarch.elf` 返回，shell 循环重新启动 `nextui.elf`，所以“回到桌面”不是由游戏进程直接画桌面，而是由启动循环重启前端。

证据：`skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh:114-209`、`workspace/all/nextui/nextui.c:1086-1090`、`skeleton/SYSTEM/tg5040/paks/Emus/GBA.pak/launch.sh`。[E5][E13]

**原厂的循环**

1. `/etc/init.d/runtrimui`（START=99）执行 `/usr/trimui/bin/runtrimui.sh`。
2. `runtrimui.sh` 导出 GPIO（107 5V、227 马达、243 FN 开关），启动 `keymon`、`trimui_inputd`、`trimui_scened`、`trimui_btmanager`、`hardwareservice`、`musicserver`、`trimui_osdd`，然后 `premainui.sh` → `MainUI` → `preload.sh`。
3. `MainUI` 退出后，`runtrimui.sh` 检查 `/tmp/cmd_to_run.sh`（MainUI 使用的命令文件），`chmod +x` 后执行；执行完删除并回到循环，再次启动 MainUI。
4. `preload.sh` 把 governor 重置为 `ondemand`、`408000–2000000`，并删除 `/tmp/stay_alive`、`/tmp/stay_awake`；`premainui.sh` 删除 DPad 模式文件。
5. 还有 `/tmp/.cmdenc` / `/tmp/program_to_run` 路径，说明原厂还有另一条程序/编码命令启动路径；brickdesk 不需要兼容它。

证据：`rootfs:/etc/init.d/runtrimui`、`rootfs:/usr/trimui/bin/runtrimui.sh:129-274`、`rootfs:/usr/trimui/bin/preload.sh:3-7`、`rootfs:/usr/trimui/bin/premainui.sh`。[E41]

**对 brickdesk 的直接结论**

- 公开的 TrimUI Smart Pro rootfs 说明指出：`/usr/trimui/bin/runtrimui.sh` 会执行
  `/mnt/SDCARD/System/starts` 下的脚本；CrossMix 也把固件检查和更新脚本放在这个
  目录（`System/starts/_FirmwareCheck.sh`、`°post_starts.sh` 等）。
- 本文从 Brick Pro 1.1.1 固件提取的 `runtrimui.sh` 没有单独保留/反编译该循环，
  因此以下语义仍需真机确认：目录遍历顺序、脚本是否同步执行、脚本返回 0/非 0 后
  是否继续启动 MainUI、以及脚本阻塞时后续脚本是否被延后。
- brickdesk 的安装器因此只新增 `/mnt/SDCARD/System/starts/zz_brickdesk.sh`，不修改
  原厂 `runtrimui.sh`；回退依赖“钩子返回后原厂继续启动 MainUI”这一未验证假设。

- 采用与 NextUI/原厂相同的“桌面进程 → 写命令 → 退出 → 启动器循环执行 → 重新启动桌面”模型，避免桌面和 GLES 游戏争用 framebuffer/EGL 上下文。
- 启动应用前保存桌面状态（选中项、滚动位置、设置），退出后恢复；不要假设应用会返回某个退出码。
- 应用退出后必须做清理：恢复背光/亮度、音量、LED profile、输入模式、CPU governor；否则会出现“回到桌面黑屏/无声/按键错乱”。
- 需要处理 `/tmp/poweroff`、`/tmp/reboot` 与“应用被杀”三种退出路径。

### 3.2 全局按键与前台程序共享输入

这是问题 4 的核心。结论：**全局按键由独立守护进程读同一批 evdev 节点实现，默认不独占输入；前台程序通过 SDL joystick 或自己的 evdev 读取仍然能看到这些按键。** 不同系统的差异在于是否再套一层 `trimui_inputd` 虚拟设备。

**NextUI**

- `keymon.elf` 在 `MinUI.pak` 中启动，打开 `/dev/input/event0..4`，`O_RDONLY|O_NONBLOCK|O_CLOEXEC`，没有 `EVIOCGRAB`。
- 它只处理：
  - `EV_SW` code 2（耳机插入）→ `SetJack()`；
  - `EV_KEY` 114/115（音量−/+）→ 调整音量；按住 316（Menu）时调亮度；按住 314（Select）时调色温；
  - `CODE_MUTE`/`CODE_JACK` 常量存在；实际 FN 状态主要靠 `/sys/class/gpio/gpio243/value` 轮询，耳机靠 `EV_SW` code 2。
  - 后台线程每 200 ms 读 `/sys/class/gpio/gpio243/value`，变化时 `SetMute()`，并触发一次双脉冲震动。
- 它不消费事件，所以前台 `nextui.elf`/`minarch.elf` 通过 SDL joystick 仍能收到同样的按键。`minarch` 用 `PAD_isPressed(BTN_MENU)` 判断组合键，并在组合键被消费时设置 `ignore_menu`，避免 Menu+音量同时打开菜单。
- 电源键不归 keymon：`nextui.elf`/`minarch.elf` 的 `PWR_update()` 处理短按睡眠、1 秒长按关机；睡眠时 keymon 被 `SIGSTOP`，唤醒后 `SIGCONT`。

证据：`workspace/tg5040/keymon/keymon.c:27-33、104-116、140-233`、`workspace/all/common/api.c:4170-4210、4341-4383`、`workspace/all/minarch/ma_input.c:17-38`。[E1][E3][E10]

**原厂**

- `keymon` 是常驻守护进程；`trimui_inputd` 也常驻。`keymon` 负责电源/音量/亮度/FN 键/低电量/OSD/休眠；`trimui_inputd` 负责输入复用：读物理 evdev，创建 `/dev/uinput` 虚拟设备，支持 `/tmp/trimui_inputd/grab`、`input_no_dpad`、`input_dpad_to_joystick`、`turbo_*` 等控制文件。
- 游戏/应用启动时，`MainUI` 退出，但 `keymon`/`trimui_inputd`/`hardwareservice`/`trimui_osdd` 不退出；因此游戏运行时音量/亮度/电源/FN 仍由后台处理。应用读的是 `trimui_inputd` 暴露的虚拟设备（或直接读 evdev，取决于应用）。
- `trimui_inputd` 是否对物理设备做 `EVIOCGRAB`，仅凭字符串不能 100% 确认；`/tmp/trimui_inputd/grab` 的存在说明它支持“抓取/转发”模式。真机应验证：游戏运行时全局键是否仍到达游戏、是否出现双份输入。
- `keymon` 的二进制字符串还显示它按 `KEY_F1`/`KEY_F2`、`/usr/trimui/fnkeys/f1key.json`/`f2key.json` 执行 F1/F2 动作，并在 `/sys/class/gpio/gpio243/value` 变化时处理 FN 开关。

证据：`rootfs:/usr/trimui/bin/runtrimui.sh:160-176、241-255`、`rootfs:/usr/trimui/bin/keymon` 字符串（`/dev/input`、`/tmp/trimui_inputd/grab`、`KEY_F1`、`gpio243`）、`rootfs:/usr/trimui/bin/trimui_inputd` 字符串（`/dev/uinput`、`/tmp/trimui_inputd/*`）。[E41]

**Knulli**

- 用 `triggerhappy` 读 evdev 并按 `multimedia_keys.conf` 执行命令；规则里明确有：
  - `KEY_VOLUMEUP`/`KEY_VOLUMEDOWN` → `volume-button volup/voldown`；
  - `KEY_VOLUMEUP+BTN_MODE`/`KEY_VOLUMEDOWN+BTN_MODE` → `volume-button briup/bridown`；
  - `KEY_POWER` → `power-button`；
  - `BTN_START+BTN_SELECT` → `knulli-screenshot`；
  - `BTN_SELECT+BTN_MODE` → `dpad-toggle`；
  - `SW_TABLET_MODE` → `toggle-switch`；
  - `SW_HEADPHONE_INSERT` → `trimui-volume-attenuate`；
  - `BTN_START+BTN_MODE+` → `knulli-softreset`。
- 这些规则用于旁路动作；`triggerhappy` 是否对设备做 grab 需真机确认（若 grab，前台将收不到原始事件）。Knulli 还用 ES 的本地 HTTP 接口（1234）做“运行中/退出/通知”控制。

证据：`board/allwinner/a133/fsoverlay/etc/triggerhappy/triggers.d/multimedia_keys.conf:1-17`、`board/fsoverlay/usr/bin/power-button`、`dpad-toggle`、`knulli-softreset`。[E20][E21][E29][E30]

**CrossMix**

- 完全沿用原厂 `keymon`/`trimui_inputd`；额外脚本用 `button_state.sh` 查询 `/dev/input/event3` 的按键状态（`evtest --query`），用 `keywatch.sh` 读 `event3`，用 `osd_hotkey.sh` 列出 OSD 组合键。它没有实现独立的全局热键守护进程。

证据：`System/usr/trimui/scripts/button_state.sh:28-50`、`keywatch.sh:9-30`、`osd_hotkey.sh:3-27`。[E32][E35]

**按键码表（CrossMix/原厂证据）**

| 功能 | evdev 码 | 备注 |
|---|---|---|
| A | 305 | `button_state.sh` |
| B | 304 | 同上 |
| X | 308 | 同上 |
| Y | 307 | 同上 |
| L1 | 310 | 同上 |
| R1 | 311 | 同上 |
| SELECT | 314 | NextUI keymon 也把 314 当作 color-temp modifier |
| START | 315 | NextUI keymon 常量存在，但亮度 modifier 用 316 |
| MENU | 316 | NextUI keymon 的 `menu_pressed` |
| 音量−/ + | 114 / 115 | `KEY_VOLUMEDOWN`/`KEY_VOLUMEUP` |
| 电源 | 116（`KEY_POWER`）或 joystick 102 | NextUI 同时定义 `BUTTON_POWER=116`、`CODE_POWER=102`、`JOY_POWER=102` |
| 耳机插入 | `EV_SW` code 2 | `SW_HEADPHONE_INSERT` |
| FN 拨动开关 | `EV_SW` code 1 或 GPIO243 | Knulli 用 `SW_TABLET_MODE`；原厂/NextUI 直接读 `gpio243` |
| Home / F1 / F2 / L4 / R4 | 未在公开脚本中给出 | NextUI 映射为 joystick 15/9/10/11/12（Brick Pro）；必须 `evtest` 确认 |

> 关键差异：Brick 没有摇杆，NextUI 把 F1/F2 当作 L3/R3；Brick Pro 有摇杆和 L4/R4，NextUI 把 F1/F2 当作 L4/R4，并额外有 Home。不要把 Brick 的映射直接套到 Brick Pro。

> 命名注意：NextUI README 把亮度组合写成 “Start + volume”，但代码里 `BTN_MOD_BRIGHTNESS` 是 `BTN_MENU`，keymon 用的亮度 modifier 是 316（Menu），315 才是 Start。两者可能是同一物理键的不同叫法，也可能 README 已过期；真机要以丝印和 `evtest` 为准。

**对 brickdesk 的建议**

- 第一版用“旁路监听”即可：打开所有 `/dev/input/event*`，不 grab；全局组合键用“按下并保持”判定，动作执行后不吞事件。这样音量/亮度等按键游戏也会收到，可能与游戏自身热键冲突，但实现简单、风险低。
- 若要做到“组合键不泄漏给游戏”，需要输入代理：独占物理设备，读入后按规则决定转发到虚拟 uinput 还是只执行桌面动作。这会影响所有前台程序，必须作为 P1 的独立架构决策，不能一边旁路一边 grab。
- 不要把电源键完全交给前台：桌面应在自己的电源守护进程中处理短按/长按，并在应用运行时也能收到；否则游戏卡死时无法休眠/关机。
- 组合键配置建议：`modifier + key`、长按阈值、重复间隔、动作 ID、是否吞键、仅在桌面/仅在应用/全局。原厂的 FN Editor 已经证明“动作注册表 + JSON”可行，brickdesk 可以把它泛化，而不是为每个功能写死代码。

### 3.3 电源、休眠、唤醒、关机

**电源树与关键节点**

- PMIC：AXP2202，I2C 地址 0x34；Brick Pro 的 `sys_config.fex` 里 `power_mode=0`（注释说 0=axp81X，但实际 DTB 是 `x-powers,axp2202`；以 DTB 为准）。
- 电源键：DTB `powerkey@0`，`pmu_powkey_off_time=0x2710`（10 s）、`pmu_powkey_long_time=0x5dc`（1.5 s）、`pmu_powkey_on_time=0x200`（512 ms）。原厂 keymon 又有“6 秒关机”的字符串，说明软件与 PMU 的阈值可能叠加，真机要测。
- 电池/充电：`/sys/class/power_supply/axp2202-battery/{capacity,status,temp,time_to_full_now}`、`/sys/class/power_supply/axp2202-usb/online`。
- 休眠：`/sys/power/state` 写 `mem`；`/sys/power/mem_sleep` 可能只有 `s2idle` 或 `deep`，需真机确认。
- 安全关机：NextUI 通过 `/dev/i2c-6` 对 AXP2202 写 0x40–0x44=0、0x48–0x4C=0xFF、0x22=0x0A、0x27=0x01，做 PMIC 软断开；失败再走 `reboot(LINUX_REBOOT_CMD_POWER_OFF)`。
- 充电指示：官方规格说电源键集成红/绿充电灯；这不在 `/sys/class/led_anim` 里，可能是 PMIC 硬件行为，需真机确认。

**NextUI 的状态机**

- `PWR_update()`：
  - `BTN_POWER` 按下记录 `power_pressed_at`；若在恢复后 1 秒内，忽略误触；
  - `BTN_POWER` 松开且 `power_pressed_at` 存在 → 手动睡眠；
  - 按住超过 1 秒 → `PWR_powerOff(0)`；
  - 屏幕超时 → 进入睡眠；
  - `pwr.requested_sleep`（硬件/其他进程请求）→ 睡眠。
- `PWR_sleep()`：`GFX_clear`、`PAD_reset`、`PWR_enterSleep`、`PWR_waitForWake`、`PWR_exitSleep`。
- `PWR_enterSleep()`：暂停音频（tg5040 不关闭设备）、LED sleep profile、背光 0、`killall -STOP keymon/batmon/audiomon`、降频、`sync`。
- `PWR_waitForWake()`：循环 `PAD_wake()`；如果 `suspendTimeout` 到期且不在充电/不需要保持唤醒，调用 `PWR_deepSleep()`；深睡失败则关机。
- `PWR_deepSleep()`：优先执行 `${BIN_PATH}/suspend`；该脚本停止 wpa_supplicant/bluetoothd、保存 ALSA、写 `mem > /sys/power/state`，失败重试 5 次，并用“睡眠超过 5 秒但返回非 0”绕过假失败。
- `PWR_exitSleep()`：恢复 CPU 频率、`SIGCONT` 守护进程、背光 1（Brick/Brick Pro 先设 raw 8 再设实际值）、恢复音量、`SND_resetAudio`。

证据：`workspace/all/common/api.c:4170-4210、4341-4493`、`skeleton/SYSTEM/tg5040/bin/suspend:16-99`、`workspace/tg5040/platform/platform.c:225-258`。[E3][E6][E2]

**原厂**

- `keymon` 是电源/休眠的实际处理者：字符串显示 `keymon dim seconds=...`、`dim lcd`、`turn off lcd`、`/tmp/system_suspend`、`system suspend`、`echo mem > /sys/power/state`、`/tmp/wakeup_from_suspend`、`system wakeup startbt ... startwifi ...`。
- 休眠前它会停止 `hciattach`、`bluetoothd`、`bluealsa`，把 `rfkill0/state` 设 0；唤醒后重启蓝牙、`hciconfig hci0 up`、`bluetoothctl power on`、`trimui_btmanager`。Wi-Fi 侧会 `ifconfig wlan0 down`、`killall wpa_supplicant`、`killall udhcpc`，唤醒后重新拉起。
- 原厂 `runtrimui.sh` 里 `tinymix set 9 1`、`tinymix set 1 0`，Brick Pro 还注释了 `tinymix set 7 145 145`（扬声器增益），说明音频初始化与休眠恢复强相关。

证据：`rootfs:/usr/trimui/bin/keymon` 字符串；`rootfs:/usr/trimui/bin/runtrimui.sh:209-212`。[E41]

**Knulli**

- `power-button`：短按（2 秒内松开）→ `knulli-suspend`；长按 2 秒 → 震动 + 关机；用 `inotifywait` 监视状态文件，避免电源键释放脚本竞争。
- `knulli-suspend`：读 `system.suspendmode`；可 `fakesuspend`；用 `pm-suspend` 进入睡眠；按 `system.batterysaver.mode`/`idlewatcher.timer` 配置 RTC 唤醒，并可自动关机；`pm-utils` hooks 处理亮度、音量、Wi-Fi 模块卸载/重载。
- `99-xradio-wlan`：suspend 时解绑 USB1、`rfkill block wifi`、`rmmod xradio_wlan`；resume 时重新 `insmod`、`rfkill unblock wifi`。这是 A133 上 Wi-Fi 休眠稳定性的重要参考。

证据：`board/fsoverlay/usr/bin/power-button:24-55`、`knulli-suspend:123-157`、`99-xradio-wlan:14-36`、`10-hide-ui`。[E21][E22][E25][E51]

**CrossMix**

- 沿用原厂 keymon/休眠；额外脚本 `lcd_off.sh` 用 `dispdbg` 关背光，`lcd_on.sh` 用 `/mnt/UDISK/system.json` 的 brightness × 23 恢复，并把 LED `max_scale` 恢复为 `ledset × 4`。它不是电源管理实现，只是显示开关。

**对 brickdesk 的建议**

- P0 先实现“轻睡眠”：背光 0 + 暂停音频 + 停全局键重复 + 记录时间；唤醒后恢复背光/音频/输入。这样即使深睡不稳定，也不会卡死。
- P1 再实现深睡：严格按“停 Wi-Fi/BT → 保存 ALSA → sync → `mem` → 恢复”的顺序；必须处理 `wpa_supplicant`/`bluetoothd` 不在运行、USB 插入、充电中、应用拒绝退出等分支。
- 关机必须两条路：标准 `poweroff`（兼容原厂）和 PMIC 软断开（避免 limbo）。默认可以先用标准关机，真机验证 limbo 后再启用 PMIC 路径；不要在未验证时直接写 I2C 寄存器。
- 不要在充电时自动深睡；NextUI 的 `PWR_preventAutosleep()` 在充电/HDMI/保持唤醒时阻止自动睡眠，可作为行为参考。

### 3.4 显示与背光

**软件栈**

- SoC/GPU：A133P + PowerVR GE8300；内核模块 `pvrsrvkm.ko`、`dc_sunxi.ko`；用户态 `libEGL.so`、`libGLESv2.so`、`libIMGegl.so`、`libPVRScopeServices.so`；启动工具 `pvrsrvctl --start`（Knulli 在 `rcS` 中调用）。
- SDL2：原厂 `/usr/trimui/lib/libSDL2-2.0.so.0.3000.8`（2.30.8）包含 **Mali EGL Video Driver**（`SDL_malivideo.c`、`SDL_maliopengles.c`）和 `opengles`/`opengles2` renderer。NextUI 用 SDL2 + `SDL_GL_CreateContext` 创建 OpenGL ES 3.2 上下文，`SDL_HINT_RENDER_DRIVER="opengl"`，`SDL_RENDERER_ACCELERATED|PRESENTVSYNC`。
- 结论：brickdesk 可以走 SDL2 + GLES2；但不能假设标准 `kmsdrm`/`/dev/dri` 可用，必须使用厂商 SDL2 的 `mali` 视频驱动（或直接 EGL/fbdev）。真机先跑 `SDL_GetNumVideoDrivers()` 确认驱动名。

证据：NextUI `generic_video.c:652-692`；原厂 `rootfs:/usr/trimui/lib/libSDL2-2.0.so.0.3000.8` 字符串；Knulli `rcS:10-13`；rootfs `/usr/lib/libEGL.so`、`libGLESv2.so`、`/lib/modules/4.9.191/pvrsrvkm.ko`。[E14][E41][E48]

**分辨率、旋转、帧率**

- 原厂 DTB：`lcd_x=0x400`（1024）、`lcd_y=0x300`（768）、`lcd_dclk_freq=0x39`（57 MHz）。
- NextUI：`FIXED_WIDTH=1024`、`FIXED_HEIGHT=768`、`FIXED_SCALE=3`、`SCREEN_FPS=60.235`，并使用严格 vsync。
- 原厂 `MainUI` 字符串有 `ScreenInit %dx%d %d, rotate=%d` 和 `rotate 270`，说明面板可能是纵向原生、由软件旋转；NextUI 直接用 1024×768，可能由 SDL/mali 驱动处理。brickdesk 必须在真机确认实际 `fb0` 的 `virtual_size`、`rotate` 和触摸/坐标方向（虽无触屏，但输入坐标/截图方向会受影响）。
- 撕裂：NextUI README 明确说修复了 MinUI 的 tearing/sync stutter；实现上要避免“SDL renderer 与自建 GL 混用”，统一用一个 GLES2 上下文和 `eglSwapBuffers`/`SDL_GL_SwapWindow`，并开启 vsync。

**亮度**

- NextUI：`SetRawBrightness(val)` 打开 `/dev/disp`，`ioctl(fd, 0x102, &param)`，`param[4]={0,val,0,0}`，val 0–255。0–10 的 UI 值经 `scaleBrightness()` 映射：0→1、1→8、2→16、3→32、4→48、5→72、6→96、7→128、8→160、9→192、10→255（Brick/Brick Pro 同一张表；Smart Pro 不同）。
- 原厂：`keymon`/`hardwareservice` 字符串也有 `/dev/disp`、`setLCDBrightness %d`；`/sys/class/disp/disp/attr/*` 处理色温/对比度/饱和度/曝光。`lcd_on.sh` 用 `brightness × 23`（0–10 → 0–230）并保证 0 时至少 4，说明原厂 UI 值与 raw 值不是简单线性。
- Knulli：优先 `/sys/class/backlight/*`，没有则用 `brightness` 助手；`do_dim`/`do_undim`/`dispoff`/`dispon` 做动画。CrossMix：`dispdbg` 的 `name=lcd0`、`command=setbl`、`param=0` 关背光。

**显示增强**

- `/sys/class/disp/disp/attr/color_temperature`（NextUI）或 `/sys/devices/virtual/disp/disp/attr/color_temperature`（原厂 `hardwareservice`）；
- `enhance_contrast`、`enhance_saturation`、`enhance_bright`（曝光）；
- 这些节点的取值范围不是 0–255 统一：NextUI 的 `scaleColortemp` 0–40、`scaleContrast` -4..5→10..100、`scaleSaturation` -5..5→0..100、`scaleExposure` -4..5→10..100；原厂 UI 的范围以 `en.lang`/MainUI 为准。真机要用 `cat`/`echo` 测边界。

**屏幕关闭**

- 首选 `/dev/disp` 亮度 0；备选 `dispdbg`（CrossMix）或 `/sys/class/graphics/fb0/blank`（Knulli 兜底）。
- 恢复时先设一个可见的最小 raw 值（NextUI 对 Brick/Brick Pro 先 `SetRawBrightness(8)`），再设保存的亮度；否则可能出现“亮度 0 恢复后仍然黑屏”。

### 3.5 音频

**设备与控件**

- 内置 codec 声卡名包含 `audiocodec`；`/proc/asound/cards` 用于找卡号。
- 原厂 `rc.local` 初始化：
  - `amixer -D hw:audiocodec cset name='Headphone Switch' 1`
  - `amixer -D hw:audiocodec cset name='Headphone Volume' 3`
  - `amixer -D hw:audiocodec cset name='HpSpeaker Switch' 1`
  - `amixer -D hw:audiocodec cset name='Headphone Volume' ...`
- NextUI `SetRawVolume`：
  - 扬声器：tinyalsa 打开 `audiocodec` 卡，`digital volume` 用 `mixer_ctl_set_percent(digital, 0, 100 - val)`（反向映射）；`DAC volume` 在 val=0 时设 0，否则 160；最后写 `/sys/class/speaker/mute`（val==0 → 1）。
  - 蓝牙：`amixer scontents -D bluealsa` 找第一个含 `A2DP` 的 simple control，`amixer -D bluealsa sset "<name>" -M val%`。
  - USB DAC：找第一个非 `audiocodec` 的声卡，遍历控件名含 `PCM`/`Playback` 且含 `Volume` 的 INT 控件，按百分比线性映射。
- 原厂 `runtrimui.sh` 还有 `tinymix set 9 1`、`tinymix set 1 0`；Brick Pro 注释 `tinymix set 7 145 145`（扬声器增益）。`tinymix` 的控件编号与名称在不同固件可能变化，brickdesk 应优先按名字找控件，而不是硬编码编号。

**耳机**

- 耳机插入是 `EV_SW` code 2（`SW_HEADPHONE_INSERT`）。NextUI keymon 在读到 `EV_SW` code 2 时 `SetJack(val)`；`GetVolume()` 在 `jack` 为真时返回耳机音量；`SetJack` 会重新应用音量。
- Knulli 用 udev/triggerhappy：插入 → `trimui-volume-attenuate headphone` → `amixer -c 0 sset Headphone 3`；拔出 → `speaker` → `amixer -c 0 sset Headphone 0`。
- 原厂 `MainUI` 字符串 `headphone found`/`headphone removed`，`keymon` 处理 `EV_SW` code 2。

**静音/底噪**

- `/sys/class/speaker/mute` 写 1 可真正关掉扬声器（包括底噪）；NextUI 在音量为 0 时写 1，否则写 0。
- 原厂 FN “Silent Mode” 也写 `/sys/class/speaker/mute` 并 touch `/tmp/system/muted`；`hardwareservice` 会同步该状态。

**对 brickdesk 的建议**

- P0：按名字找 `digital volume`/`DAC volume`，不要硬编码 `tinymix` 编号；初始化时把 `Headphone`、`digital volume`、`DAC Swap` 设到已知状态（NextUI 的做法是 `Headphone 0`、`digital volume 0`、`DAC Swap Off`）。
- P0：耳机插入/拔出必须实时切换音量与路由；不要等用户进设置。
- P1：蓝牙 A2DP 音量与 USB DAC；P2：声道交换/增益/麦克风。

### 3.6 LED 与震动

**LED 节点**

- 基路径：`/sys/class/led_anim/`。
- 全局：`effect_enable`、`anim_frames_enable`、`enable`、`frame_hex`、`max_scale`。
- 分区：`m`（顶部灯条/Logo）、`lr`（Brick 的肩键/扳机，Brick Pro 的摇杆环）、`f1`、`f2`（FN 键）、`rear`（Brick Pro 的 L/R 扳机）。
- 每区：`effect_<zone>`（效果编号）、`effect_rgb_hex_<zone>`（RRGGBB）、`effect_cycles_<zone>`、`effect_duration_<zone>`、`max_scale_<zone>`。
- Brick Pro 的 `max_scale_lr` 是摇杆环，`max_scale_rear` 是扳机；Brick 的 `max_scale_lr` 是扳机；Smart Pro 只有 `max_scale`（全局）。
- 原厂 `init_leds.sh` 用 `frame_hex` 直接写多帧颜色；`low_battery_led.sh` 用 `effect_rgb_hex_lr/f1/f2` + `effect_cycles` + `effect_duration` + `effect` 做红色闪烁。
- `effect_*` 的编号是厂商定义（原厂脚本用 5/6 等），不要猜测语义；brickdesk 应把“效果编号→名称”做成可配置表，真机逐个试。

**震动**

- 使能：GPIO227（`/sys/class/gpio/gpio227/value`，先 export，direction=out）。
- 强度：`/sys/class/motor/voltage`，单位微伏；NextUI 范围 500000–3300000，Brick Pro 限 2500000；原厂 `hardwareservice` 字符串含 2000000/2500000/3000000/3300000/3500000。
- NextUI `PLAT_setRumble(strength)`：0 < strength < 0xFFFF 时按线性映射写 voltage，否则写最大值；然后 GPIO227 写 1/0。
- 原厂 `keymon` 在 FN 静音切换时做双脉冲，Brick 用 1500000、Brick Pro 用 900000，说明 Brick Pro 马达更强，必须限压。
- Knulli `knulli-rumble` 直接 GPIO227 写 1、sleep、写 0；没有电压控制，所以只能做“开关式”短震。

**对 brickdesk 的建议**

- P0：LED 只做“开/关 + 全局亮度 + 低电量/充电提示”；震动只做“短震/强度 0–100”并保守限压（Brick Pro ≤2.5 V）。
- P1：分区颜色/效果、睡眠呼吸、按键反馈；把 `effect_*` 编号做成设备配置文件，避免跨固件写错。
- 不要在未确认安全电压前把 `/sys/class/motor/voltage` 开到 3.3 V 以上；Brick Pro 的官方马达驱动是 3.3 V，但 NextUI 实测高占空比“很烦人”，限到 2.5 V。

### 3.7 Wi-Fi、蓝牙、时间、性能

**Wi-Fi**

- 接口 `wlan0`；驱动大概率 `xradio_wlan`（rootfs `/lib/modules/4.9.191/xradio_wlan.ko`；Knulli `S03modules` 对非 Smart Pro 机型 insmod `xradio_mac/core/wlan.ko`）。
- 工具：`wpa_supplicant`（`-D nl80211 -i wlan0 -c /etc/wifi/wpa_supplicant.conf -O /etc/wifi/sockets`）、`wpa_cli`、`udhcpc`、`hostapd`（热点）、`dnsmasq`/`udhcpd`。
- NextUI `generic_wifi.c` 用 `wpa_cli -p /etc/wifi/sockets -i wlan0` 做 scan/status/connect；`wifi_init.sh` 负责 rfkill unblock、wpa_supplicant start、udhcpc。
- 原厂 `etc/init.d/wpa_supplicant` 是 procd 服务；`runtrimui.sh` 根据 `systemval wifi` 启停。
- Knulli 用 ConnMan；`99-xradio-wlan` 在休眠时卸载 `xradio_wlan`，唤醒时重载。
- 冲突点：Knulli 的 Brick Pro 内核配置 `CONFIG_RTL8821CS=m`，而 fsoverlay 和 rootfs 都是 XR829 固件/模块。这可能是 Knulli 配置复用/未完成，必须真机 `lsmod`、`/sys/bus/sdio/devices/*/modalias`、`dmesg` 确认。

**蓝牙**

- 工具链：`bluetoothd`、`bluetoothctl`、`bluealsa`（A2DP）、`hciattach`、`hciconfig`、`rfkill`。
- 原厂 `bt_init.sh` 使用 `hciattach_aic -n -s 1500000 /dev/ttyS1 aic`；但 `/etc/init.d/hciattach` 又启动 `/usr/bin/hciattach -n ttyS1 xradio`；NextUI 的 `bt_init.sh` 使用 `hciattach -n ttyS1 xradio`；Knulli 同时带 `hciattach`/`rtk_hciattach` 和 XR829 固件。
- 这说明 Brick/Brick Pro 的蓝牙可能是 AIC 或 XR829，官方 rootfs 自身存在两套脚本。不要根据文件名猜；真机看 `hciconfig -a`、`lsmod`、`dmesg | grep -i -E 'aic|xradio|rtl|bluetooth'`、`/sys/class/bluetooth/hci0`。
- 电源控制：`/sys/class/rfkill/rfkill0/state` 写 0/1；`/proc/bluetooth/sleep/btwrite`（NextUI）用于低功耗；`bluealsa -p a2dp-source` 提供 A2DP。
- 原厂 `runtrimui.sh` 启动 `trimui_btmanager`；NextUI 用 `bluetoothctl` + `bluealsa`；Knulli 用 BlueZ/PipeWire。

**时间**

- 设置时间：`date -s 'YYYY-MM-DD HH:MM:SS'; hwclock -u -w`。
- 时区：`uci get/set system.@system[0].zonename`，`uci commit system`，`ln -sf /usr/share/zoneinfo/<tz> /tmp/localtime`，`date -k`。
- NTP：`uci set system.ntp.enable=1/0`，`uci commit system`，`/etc/init.d/ntpd reload/stop`。
- 原厂还有 `sysfixtime`、`sysntpd` 服务；RTC 由 AXP2202 提供。
- NextUI 的 `PLAT_setDateTime`/`PLAT_getTimezones`/`PLAT_setCurrentTimezone`/`PLAT_getNetworkTimeSync` 是上述命令的封装。

**性能**

- 调频节点：`/sys/devices/system/cpu/cpufreq/policy0/`（部分原厂脚本用 `/sys/devices/system/cpu/cpu0/cpufreq/`，两者可能都可用）。
- 可用频率（DTB OPP）：408、600、816、1008、1200、1320、1416、1464、1512、1608、1800、2000 MHz。
- 原厂 FN Editor：
  - 省电：ondemand，1008–1200 MHz；
  - 平衡：ondemand，1200–2000 MHz；
  - 高性能：performance，2000 MHz；
  - 预置：Power Saving / Balanced / Performance。
- CrossMix `cpufreq.sh` 用 0–8 档：408/600/816/1008/1200/1416/1608/1800/2000，并可通过写 `/sys/devices/system/cpu/cpuN/online` 控制核心数。
- NextUI `governor.sh`：auto=schedutil（max 取第二高档）、performance=performance（max 2000）、powersave=conservative（max 取中间档）。
- 注意：原厂/CrossMix 使用 `ondemand`/`interactive`，NextUI 使用 `schedutil`/`conservative`；内核实际支持的 governor 列表必须真机 `cat scaling_available_governors` 确认。不要假设 `schedutil` 一定存在。
- GPU：NextUI 只读/固定 660 MHz；原厂 “GPU Max Frequency” 设置的实际节点未在公开脚本中出现；brickdesk 第一版不要做 GPU 调频。
- 温度：NextUI CPU `thermal_zone0`、GPU `thermal_zone2`；原厂 OSD 读 `axp2202-battery/temp`（电池温度），不是 CPU 温度；两者不要混用。

### 3.8 游戏专属能力与扩展协议

**NextUI 的机制（事实）**

- 游戏内 `Menu+Select`：`ma_input.c` 设置 `newScreenshot=1`、`quit=1`、`Menu_saveState()`，写 `GAME_SWITCHER_PERSIST_PATH`；NextUI 回到前台后打开 game switcher，显示 recents 轮播。
- 普通存档：`Menu_saveState()` 把 `state_slot` 设为当前 UI 槽（1–8 用户可见，内部 0–7），写状态文件和 `.bmp` 预览；`Menu_loadState()` 读同一槽。
- 自动续玩：`Menu_beforeSleep()` 写 `AUTO_RESUME_PATH` 并把状态存到自动槽 9；下次启动 `autoResume()` 读取路径，设置 `RESUME_SLOT_PATH=9`，直接启动对应模拟器；`State_resume()` 在 minarch 启动时读状态。
- 切换游戏：NextUI 的 game switcher 选择最近游戏后，设置 `RESUME_SLOT_PATH` 为上次槽位，退出当前游戏并启动新游戏；它不是“同时挂起多个模拟器”，而是“保存/退出/重启/读档”。
- 存档文件在 `SHARED_USERDATA_PATH/.minui/<EMU>/<rom>.stN`（或配置的 RetroArch 格式），预览 `<rom>.N.bmp`；这是 NextUI 的格式，brickdesk 不应假设兼容。

证据：`workspace/all/minarch/ma_input.c:17-38、140-161`、`workspace/all/minarch/ma_saves.c:159-197、371-385`、`workspace/all/minarch/ma_menu.c:221-232、1598-1760、1934-1938`、`workspace/all/nextui/nextui.c:1182-1216、1286-1313`。[E10][E11][E12][E13]

**RetroArch 的可编程接口（事实）**

- RetroArch 提供 UDP Network Control Interface：
  - 配置：`network_cmd_enable = "true"`、`network_cmd_port = "55355"`（默认）；
  - 发送：`retroarch --command "COMMAND;HOST;PORT"`，或 `echo -n "COMMAND" | nc -u -w1 localhost 55355`；
  - 关键命令：`GET_STATUS`、`SHOW_MSG`、`MENU_TOGGLE`、`QUIT`、`CLOSE_CONTENT`、`RESET`、`PAUSE_TOGGLE`、`SAVE_STATE`、`LOAD_STATE`、`STATE_SLOT_PLUS`、`STATE_SLOT_MINUS`、`LOAD_STATE_SLOT <n>`、`SCREENSHOT`、`SET_SHADER`、`SAVE_FILES`、`LOAD_FILES`、`FAST_FORWARD`、`REWIND`、`MUTE`、`VOLUME_UP/DOWN`。
  - `SAVE_STATE`/`LOAD_STATE` 走“当前槽”，槽位由 `STATE_SLOT_PLUS/MINUS` 或 `LOAD_STATE_SLOT` 控制；这足以实现“存档位/读档/快速恢复”的 RetroArch 适配器，但需要 RetroArch 自身开启 NCI。
- RetroArch 配置还有 `savestate_auto_save`、`savestate_auto_load`、`autosave_interval`、`savestate_auto_index`、`savestate_max_keep`，可用于“退出自动存/启动自动读”；这些是 RetroArch 行为，不是桌面行为。
- 命令行 `--command` 是“向已运行实例发 UDP 命令并退出”，不是启动新实例；`-L <core>`、`--appendconfig` 等用于启动。不要把 `--command` 和 `-L` 混在一个进程里期待它同时启动和发命令。

证据：Libretro Docs Network Control Interface；RetroArch 默认 `retroarch.cfg` 的 savestate 注释。[E39][E40]

**Knulli 的 ES HTTP 接口（事实）**

- Knulli 的脚本通过 `http://localhost:1234` 与 EmulationStation 通信：
  - `GET /runningGame`：201 表示没有游戏在运行，非 201 表示在游戏中；
  - `/notify`：发送通知；
  - `/emukill`：杀当前模拟器；
  - `/quit`：退出 ES；
  - `/messagebox`：显示消息框（`S28usbmode` 注释里出现）。
- 另一个 RGB 守护进程在 1235 端口，`knulli-brightness` 用 `/update-screen-state`，`knulli-shutdown` 用 `/kill`。
- 这证明“本地 HTTP + 小 JSON/文本命令”足以做前台/后台控制；但它依赖 ES 自己的实现，不能直接拿到 brickdesk。

证据：`board/fsoverlay/usr/bin/dpad-toggle:11-20`、`knulli-softreset:22-39`、`knulli-shutdown:6-11、34-38`、`knulli-brightness:50-59`、`S28usbmode:67`。[E23][E28][E29][E30]

**brickdesk 建议的扩展协议（设计建议，不是现有实现）**

目标：不接管游戏内存、不规定存档格式、不实现游戏切换器；只让应用/游戏主动告诉桌面“我能做什么”，桌面提供通用菜单和生命周期。

1. **启动约定**
   - 桌面启动应用时设置环境变量：`BRICKDESK_CONTROL_SOCKET=/run/brickdesk/control.sock`、`BRICKDESK_APP_ID=<id>`、`BRICKDESK_PROTOCOL=1`。
   - 应用如果支持协议，启动后连接 Unix socket，发送 `hello`：
     ```json
     {"v":1,"app":"my-game","caps":["menu","save","load","quit","screenshot","status"]}
     ```
   - 不支持协议的应用忽略环境变量，桌面只提供“强制退出/系统菜单”，不提供存档/恢复。
2. **桌面 → 应用**
   - `menu.show`：请求应用打开自己的应用内菜单（应用负责绘制/暂停；桌面不覆盖 GLES 画面）。
   - `state.save {"slot":N}` / `state.load {"slot":N}`：应用自己写/读自己的存档；桌面只记录元数据（时间、截图路径、应用 ID），不解析格式。
   - `state.query`：返回可用槽位、当前槽、是否有自动存档。
   - `app.quit {"save":true|false}`：应用自己决定是否保存；桌面等待退出并清理。
   - `status.ping`：健康检查/超时。
3. **应用 → 桌面**
   - `ready`、`menu.request`（应用请求桌面显示“系统菜单”，例如退出/设置）、`state.saved/loaded`、`app.exit`、`error`。
   - 桌面只信任同 UID/同 socket 的对端；socket 权限 0600；默认不监听 TCP。
4. **RetroArch 适配器**
   - 如果检测到 RetroArch 且 NCI 开启，桌面可用 `SAVE_STATE`/`LOAD_STATE`/`STATE_SLOT_PLUS`/`LOAD_STATE_SLOT`/`QUIT`/`SHOW_MSG`/`MENU_TOGGLE` 实现同一套 UI；如果 NCI 未开启，退化为“只退出/只截图”（截图可用 framebuffer 或 `SCREENSHOT` 若可用）。
   - 不把 RetroArch 的存档路径写死；通过 `GET_CONFIG_PARAM savestate_directory`/`savefile_directory` 查询。
5. **通用应用内菜单的边界**
   - 桌面是独立进程，应用是全屏 GLES/EGL 时，桌面无法可靠地“覆盖”一帧；除非引入合成器/共享纹理，否则不要做跨进程 overlay。
   - 正确做法：应用绘制菜单，或应用暂停并让出显示，由桌面绘制；协议里必须明确“谁拥有当前 framebuffer”。
   - 这也是为什么“桌面最多提供一个通用的应用内菜单”应理解为“协议 + 可选的应用侧菜单”，而不是桌面强制注入。

**对游戏功能的优先级**

- 存档位、游戏切换器、快速恢复、游戏最近/收藏、着色器/遮罩/金手指/RA：**扩展协议，不内置**。
- 通用应用菜单：P1（协议），只在应用声明 `menu` 能力时可用。
- 截图：P1 可作为系统能力（framebuffer 截图），但“游戏内截图预览/切换器”仍属扩展。

---

## 4. 必须在真机上确认的问题

以下问题无法仅凭源码/固件确定，必须在 Brick Pro 真机上逐条验证。建议按顺序做，先解决“能启动、能显示、能关机”，再解决体验。

### 4.1 型号与固件

1. `cat /etc/model` 是 `TG4040` 还是 `TG4070`？`strings /usr/trimui/bin/MainUI | grep '^Trimui'` 是否输出 `Trimui Brick Pro`？
2. `cat /etc/version`、`uname -a`、`cat /proc/version`：内核是否 4.9.191，是否为 `aarch64`，构建时间/厂商分支。
3. `cat /proc/device-tree/model`、`compatible`：是否 `allwinner,a133` / `sun50iw10`。
4. 电池标称 5000 mAh，但 DTB 的 `pmu_battery_cap` 字段为 3000（0xBB8，可能是模板默认值）；电量计读数是否准确？满充/放空各测一次，记录 `capacity` 曲线。
5. 屏幕原生方向/旋转：`cat /sys/class/graphics/fb0/virtual_size`、`fbset`、`cat /sys/class/graphics/fb0/rotate`（若存在）；截图方向是否 1024×768。

### 4.2 输入

6. `cat /proc/bus/input/devices`、`ls -l /dev/input/event*`：哪些 event 节点对应 `gpio_keys_polled`、`trimui_inputd` 虚拟设备、摇杆、电源键？
7. `evtest` 逐个按键：A/B/X/Y/L/R/Select/Start/Menu/Home/F1/F2/L4/R4、电源、音量±、十字键、双摇杆、L3/R3；记录 `type/code/value`。特别确认 Menu 是 316 还是 315，Home 是 102/15 还是其他。
8. FN 拨动开关：`cat /sys/class/gpio/gpio243/value` 在拨到 ON/OFF 时的值；`evtest` 是否同时产生 `SW_TABLET_MODE` (EV_SW code 1)。Knulli 和原厂的极性是否一致？
9. 耳机插入：`evtest` 是否产生 `SW_HEADPHONE_INSERT` (code 2)；插入后 `amixer -c 0 sget Headphone` 是否从 0 变 3，扬声器是否静音。
10. 游戏运行时，keymon/trimui_inputd 是否 grab 物理设备？全局音量/亮度键是否同时到达游戏（双份输入）？`/tmp/trimui_inputd/grab` 是否存在、由谁创建？
11. 长按/组合键：Menu+音量、Select+音量、Menu+Select、Menu+Start、电源 1 s/2 s/6 s 的实际行为；是否有按键重复/粘键。

### 4.3 显示

12. `SDL_GetNumVideoDrivers()` / `SDL_GetCurrentVideoDriver()`：厂商 SDL2 是否暴露 `mali`；`SDL_VIDEODRIVER=mali` 是否必需；`SDL_Renderer` 是否 `opengles2`。
13. `/dev/disp` 是否可打开；ioctl `0x102` 的 raw 范围是否 0–255；0/1/8/255 的实际亮度；是否会出现“0 仍然微亮”或“1 全黑”。
14. `/sys/class/backlight/*` 是否存在；`/sys/class/disp/disp/attr/*` 与 `/sys/devices/virtual/disp/disp/attr/*` 是否指向同一组节点；`dispdbg` 的 `lcd0/setbl` 是否可用。
15. 屏幕旋转：原生 panel 是否 768×1024 纵向；`rotate 270` 是必须还是遗留；GLES2 视口/截图/输入坐标是否一致。
16. 帧率/撕裂：`SCREEN_FPS=60.235` 是否与实际 panel 匹配；`eglSwapInterval(1)` 是否生效；快速横向滚动是否撕裂。
17. 亮度恢复：从息屏/深睡恢复时，先写 raw 8 再写保存值是否必要；是否会闪黑/闪白。
18. 色温/对比度/饱和度/曝光的取值范围与方向（越大越暖/越亮？）；写 0 是否关屏。

### 4.4 电源与电池

19. `cat /sys/power/state`、`cat /sys/power/mem_sleep`：支持 `mem` 吗？是 `s2idle` 还是 `deep`？一次深睡/唤醒的耗时、电流、发热。
20. 深睡 20 次循环：Wi-Fi/BT/音频/背光/输入是否每次恢复；是否出现 `wpa_supplicant` 卡死、蓝牙 `hci0` 不见了、ALSA 无声。
21. 休眠时 Wi-Fi/BT 是否必须停止/卸载模块；不停止是否更稳定？Knulli 卸载 `xradio_wlan`，NextUI 只停 `wpa_supplicant`/`bluetoothd`，两种都要试。
22. 充电/USB 插入是否阻止自动休眠；`axp2202-usb/online` 与 `time_to_full_now` 的组合是否能区分“充电”和“USB 连接电脑”。
23. 标准 `poweroff` 是否会出现 limbo（屏幕黑但电池未断开、插 USB 无反应）；PMIC 软断开序列是否有效；连续关机/开机 10 次是否稳定。
24. 电源键阈值：短按/1 s/2 s/6 s/10 s 的实际动作；PMU `pmu_powkey_off_time=10 s` 与 keymon 的 6 s 谁先生效。
25. 低电量阈值/自动关机电压；`capacity` 到 0% 时是否还能安全关机；低电量 LED/提示是否准确。
26. 充电灯：电源键红/绿是否由硬件自动控制；`/sys/class/led_anim` 是否能覆盖；关机充电是否亮。

### 4.5 音频、LED、震动

27. `amixer -c 0 contents` / `tinymix contents`：`digital volume`、`DAC volume`、`Headphone`、`HpSpeaker Switch`、`DAC Swap` 的实际名称、范围、方向；`tinymix set 9 1`/`1 0` 对应哪些控件。
28. 扬声器 0 音量时 `/sys/class/speaker/mute` 是否必须写 1 才能消除底噪；耳机插入后扬声器是否物理静音。
29. 蓝牙 A2DP：`bluealsa` 的 simple control 名称；`amixer -D bluealsa` 是否可调；切换回扬声器是否恢复。
30. USB DAC：`/proc/asound/cards` 的卡名；`PCM/Playback + Volume` 控件是否存在；OTG 供电是否足够。
31. `/sys/class/led_anim/` 完整节点列表；`m/lr/f1/f2/rear` 在 Brick Pro 上分别对应哪个物理灯；`effect_*` 编号 0–N 的实际效果；`max_scale_*` 的 0–255/0–100 范围。
32. `frame_hex` 的格式（多少组 RGB、多少帧）；`anim_frames_enable` 的语义；是否与 `effect_*` 冲突。
33. 马达：`/sys/class/motor/voltage` 的安全范围；2.5 V/3.3 V 的震动强度/噪音/发热；GPIO227 的极性；长时间震动是否掉压/重启。
34. 低电量/充电/睡眠 LED 是否与 `led_anim` 冲突；是否有硬件充电灯不受软件控制。

### 4.6 网络、时间、性能

35. Wi-Fi 芯片：`lsmod`、`/sys/bus/sdio/devices/*/modalias`、`dmesg | grep -i xradio`；是 XR829 还是 RTL8821CS？Knulli 配置与 rootfs 冲突，必须以真机为准。
36. 蓝牙芯片/固件：`hciconfig -a`、`dmesg | grep -i -E 'aic|xradio|rtl|hci'`；`hciattach` 与 `hciattach_aic` 哪个能起 `hci0`；`/sys/class/rfkill/rfkill0/state` 的极性。
37. `wpa_cli -p /etc/wifi/sockets -i wlan0 status` 是否可用；`wpa_supplicant` 的 `ctrl_interface` 路径；`udhcpc` 是否自动拿 IP；热点 `hostapd` 是否可用。
38. 蓝牙 A2DP 与 BLE HID 是否能同时工作；休眠恢复后是否需要重新 `bluetoothctl power on`/`agent`。
39. 时间：`hwclock -u -w` 是否写进 AXP2202 RTC；断网重启后时间是否保持；`uci system.ntp.enable` 是否真的控制 `ntpd`。
40. CPU：`cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies`、`scaling_available_governors`；是否支持 `schedutil`/`conservative`/`interactive`；写 `scaling_min_freq`/`max_freq` 的顺序是否会导致临时超频。
41. 核心上线：`/sys/devices/system/cpu/cpuN/online` 是否可写；关核心是否影响音频/GPU/输入。
42. 温度：`thermal_zone0/1/2` 分别是什么；CPU/GPU 温度读数是否合理；长时间满载是否降频。
43. GPU：`/sys/class/devfreq/*` 是否存在；厂商 “GPU Max Frequency” 设置实际写哪个节点；固定 660 MHz 是否真的固定。

### 4.7 系统/启动/存储

44. 原厂启动链：`/etc/init.d/runtrimui` 是否由 rcS 调用；`MainUI` 退出码；`/tmp/cmd_to_run.sh` 的写入者/格式；`/tmp/program_to_run`、`/tmp/.cmdenc` 是否仍使用。
45. `/mnt/SDCARD` 的文件系统（FAT32/ext4）、挂载选项、写入权限；`/mnt/UDISK` 的可用空间；`system.json` 的 schema/默认值。
46. 替换 MainUI 后，原厂 OSD/按键守护进程是否仍能工作；是否必须先启动 `keymon`/`trimui_inputd`/`hardwareservice`/`trimui_osdd`。
47. ADB/SSH：`adbd` 是否默认启动；`enablessh` 如何控制；串口/日志在哪里。
48. 断电/拔卡/只读 SD 的恢复：`runtrimui.sh` 有 `recover from readonly SD` 注释和 `e2fsck` 逻辑；brickdesk 需要自己的恢复/备份策略。

---

## 5. 主要风险与实施建议

### 5.1 风险

1. **厂商内核/显示私有接口**：4.9.191 没有公开源码，`/dev/disp` ioctl、`dispdbg`、`led_anim`、`motor` 都是厂商定义；固件升级可能改语义。必须做“能力探测 + 回退”，不要把路径/编号写死。
2. **显示栈脆弱**：SDL2 的 `mali` 驱动、EGL/GLES2、面板旋转、vsync 任何一处错都可能黑屏/撕裂/花屏。建议先用厂商 `sdltest`/`sdl2play` 验证 SDL2 视频驱动，再接入自己的 GLES2。
3. **休眠/关机数据损坏**：深睡时 SD 卡、SQLite/设置文件、音频状态都可能未同步；PMIC limbo 会导致“黑屏但不断电”。所有设置在写盘后 `sync`，关机前卸载 SD，默认提供安全关机路径。
4. **输入冲突**：全局键旁路监听会与游戏热键重复；grab + uinput 又会改变所有前台程序的行为。必须明确“v1 旁路、v2 输入代理”的路线，不要在同一个版本混用。
5. **Wi-Fi/BT 芯片不确定**：官方 rootfs 同时存在 AIC/XR829 两套蓝牙脚本，Knulli 的 Brick Pro 配置又启用 RTL8821CS；如果选错 `hciattach`，蓝牙完全不可用。必须在真机锁定芯片/固件后再写死。
6. **电池计**：官方标称 5000 mAh，但 DTB 的 PMU 配置是 3000 mAh；电量百分比可能不准，低电量关机阈值也可能不可靠。不要用容量做“剩余分钟”承诺。
7. **许可**：NextUI PolyForm Noncommercial、MinUI 无正式许可证；不能复制它们的代码/脚本/资源。本文只保留接口事实；实现时用独立代码和自有 UI 资源。
8. **只验证 Brick Pro 不够**：Brick、Smart Pro 的屏幕/摇杆/LED/FN 映射不同；如果 brickdesk 声称支持多机型，必须分别做能力探测，不能用一个 `tg5040` 分支硬编码。

### 5.2 建议的实施顺序

1. **P0-1 显示与启动**：厂商 SDL2 + `mali` 驱动 + GLES2 清屏/三角形/文字；确认 1024×768、旋转、vsync；再用 `/dev/disp` 调亮度。
2. **P0-2 输入与全局键**：读 evdev，打印所有键码；实现音量/亮度/电源；验证游戏运行时按键不丢/不双份。
3. **P0-3 音频与耳机**：按名字找 mixer 控件，扬声器/耳机切换，静音底噪。
4. **P0-4 电源**：先轻睡眠（背光/音频/输入），再深睡（`mem` + Wi-Fi/BT 停启），最后安全关机（标准 + PMIC）。
5. **P0-5 启动循环**：桌面 → 应用 → 退出 → 桌面；清理显示/音频/输入/CPU/LED。
6. **P0-6 设置与电池**：`/mnt/UDISK` 或 SD 的原子配置；电量/充电 UI；低电量提示。
7. **P1**：Wi-Fi 连接 UI、蓝牙配对/音频、FN 开关动作、F1/F2/Home/L4/R4、色温/对比度、LED 效果、震动强度、CPU 档位、OSD/截图、时间/NTP。
8. **P1/P2 协议**：先做“应用主动连接”的 Unix socket 协议和 RetroArch NCI 适配器；不要先做游戏切换器/存档格式。

---

## 附录 A：原厂固件提取与证据方法

官方 Brick Pro 固件包：`trimui/firmware_brickpro` 的 `v1.1.1-20260717`，文件 `trimui_tg4040_20260717_v1.1.1.7z` → `trimui_tg4040.awimg`。[E41]

`awimg` 是 Allwinner `IMAGEWTY` 容器，不是普通磁盘镜像。可用 `imagewty-tool info` 查看条目；关键偏移（Brick Pro）：

- `sys_config.fex`：offset `0x9C00`，size 24583；
- `sunxi.fex`（board DTB）：offset `0x19800`，size 153088；
- `boot.fex`：offset `0x5E6C00`，size 11882496；
- `rootfs.fex`：offset `0x113C000`，size 566231040。

提取 rootfs：

```sh
dd if=trimui_tg4040.awimg of=rootfs.img \
  iflag=skip_bytes,count_bytes skip=$((0x113C000)) count=$((566231040))
mkdir -p rootfs && debugfs -R 'rdump / /path/to/rootfs' rootfs.img
```

Brick（TG3040）1.1.1 的 `rootfs.fex` offset 为 `0x1131000`，size 同样 566231040；其余条目用 `imagewty-tool info` 查。

DTB 反编译：

```sh
dtc -I dtb -O dts -o sunxi.dts sunxi.fex
```

本文引用的原厂路径均相对于解包后的 rootfs 根目录，例如 `rootfs:/usr/trimui/bin/runtrimui.sh`。本文不附带固件镜像或提取出的厂商文件，只保留可复现的提取方法与接口事实。

## 附录 B：证据索引

> 链接均指向调研时的上游 `main`/`knulli-main`。行号以链接中的 `#L...` 为准；固件证据没有公开源码，使用官方 release 链接 + 解包路径。

- [E1] NextUI keymon：<https://github.com/LoveRetro/NextUI/blob/main/workspace/tg5040/keymon/keymon.c#L27-L233>（按键码、输入打开、音量/亮度、耳机、FN）
- [E2] NextUI tg5040 platform：<https://github.com/LoveRetro/NextUI/blob/main/workspace/tg5040/platform/platform.c#L180-L598>（电池、背光、CPU、震动、LED、时间）
- [E3] NextUI tg5040 msettings：<https://github.com/LoveRetro/NextUI/blob/main/workspace/tg5040/libmsettings/msettings.c#L800-L1418>（亮度 ioctl、音量 mixer、显示增强、FN/mute）
- [E4] NextUI tg5040 platform.h：<https://github.com/LoveRetro/NextUI/blob/main/workspace/tg5040/platform/platform.h#L43-L160>（按键/摇杆/分辨率/修饰键）
- [E5] NextUI MinUI.pak 启动循环：<https://github.com/LoveRetro/NextUI/blob/main/skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh#L50-L209>
- [E6] NextUI suspend 脚本：<https://github.com/LoveRetro/NextUI/blob/main/skeleton/SYSTEM/tg5040/bin/suspend#L16-L99>
- [E7] NextUI governor：<https://github.com/LoveRetro/NextUI/blob/main/skeleton/SYSTEM/tg5040/bin/governor.sh#L40-L57>
- [E8] NextUI 设置菜单/设备能力：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/settings/settings.cpp#L276-L390>、<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/settings/settings.cpp#L558-L853>
- [E9] NextUI FN 按钮分配：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/settings/fnbuttonmenu.cpp#L123-L179>
- [E10] NextUI minarch 输入/组合键：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/minarch/ma_input.c#L17-L180>
- [E11] NextUI minarch 存档：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/minarch/ma_saves.c#L159-L197>、<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/minarch/ma_saves.c#L371-L385>
- [E12] NextUI minarch 菜单/自动续玩：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/minarch/ma_menu.c#L221-L232>、<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/minarch/ma_menu.c#L1598-L1760>
- [E13] NextUI 启动/恢复：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/nextui/nextui.c#L1086-L1090>、<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/nextui/nextui.c#L1182-L1313>
- [E14] NextUI SDL/GLES 视频：<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/common/generic_video.c#L652-L692>
- [E15] NextUI 蓝牙初始化：<https://github.com/LoveRetro/NextUI/blob/main/skeleton/SYSTEM/tg5040/etc/bluetooth/bt_init.sh#L14-L134>
- [E16] NextUI Wi-Fi 初始化/通用实现：<https://github.com/LoveRetro/NextUI/blob/main/skeleton/SYSTEM/tg5040/etc/wifi/wifi_init.sh#L6-L37>、<https://github.com/LoveRetro/NextUI/blob/main/workspace/all/common/generic_wifi.c#L24-L258>
- [E17] NextUI 安全关机：<https://github.com/LoveRetro/NextUI/blob/main/workspace/tg5040/poweroff_next/poweroff_next.c#L279-L317>
- [E18] MinUI README 支持设备：<https://github.com/shauninman/MinUI/blob/main/README.md#L65-L91>
- [E19] MinUI tg5040 启动脚本：<https://github.com/shauninman/MinUI/blob/main/skeleton/SYSTEM/tg5040/paks/MinUI.pak/launch.sh#L25-L64>
- [E20] Knulli triggerhappy 全局键：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/triggerhappy/triggers.d/multimedia_keys.conf#L1-L17>
- [E21] Knulli 电源键：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/usr/bin/power-button#L24-L55>
- [E22] Knulli 休眠：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/usr/bin/knulli-suspend#L123-L157>
- [E23] Knulli 亮度：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/usr/bin/knulli-brightness#L11-L42>、<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/usr/bin/knulli-brightness#L177-L210>
- [E24] Knulli 震动：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/usr/bin/knulli-rumble#L33-L42>
- [E25] Knulli Wi-Fi 休眠钩子：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/pm/sleep.d/99-xradio-wlan#L14-L36>
- [E26] Knulli FN 开关：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/init.d/S99toggle-switch#L28-L55>
- [E27] Knulli 输入守护：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/init.d/S12trimuiinput#L6-L22>
- [E28] Knulli 关机：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/usr/bin/knulli-shutdown#L13-L58>
- [E29] Knulli DPad 切换：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/usr/bin/dpad-toggle#L46-L61>
- [E30] Knulli 软复位/HTTP：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/usr/bin/knulli-softreset#L22-L39>
- [E31] Knulli Brick Pro board：<https://github.com/knulli-cfw/knulli-linux/tree/knulli-main/board/allwinner/a133/trimui-brick-pro>
- [E32] CrossMix 按键码/背光：<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/button_state.sh#L28-L50>、<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/lcd_off.sh#L6-L14>
- [E33] CrossMix CPU 频率表：<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/cpufreq.sh#L8-L34>、<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/cpufreq.sh#L100-L122>
- [E34] CrossMix 背光恢复：<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/lcd_on.sh#L8-L27>
- [E35] CrossMix OSD 组合键：<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/osd_hotkey.sh#L3-L27>
- [E36] CrossMix 输入守护切换（Brick 不支持）：<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/inputd_switcher.sh#L14-L19>
- [E37] CrossMix 通用启动器：<https://github.com/cizia64/CrossMix-OS/blob/main/System/usr/trimui/scripts/common_launcher.sh#L68-L76>
- [E38] CrossMix RetroArch 启动：<https://github.com/cizia64/CrossMix-OS/blob/main/Apps/RetroArch/launch.sh#L7-L31>
- [E39] RetroArch Network Control Interface：<https://docs.libretro.com/development/retroarch/network-control-interface/>
- [E40] RetroArch 默认配置的 savestate 选项：<https://github.com/libretro/RetroArch/blob/master/retroarch.cfg>
- [E41] TrimUI 官方 Brick Pro 固件 1.1.1：<https://github.com/trimui/firmware_brickpro/releases/tag/v1.1.1-20260717>（`rootfs:/etc/model`、`rootfs:/usr/trimui/bin/runtrimui.sh`、`rootfs:/usr/trimui/bin/keymon` 等）
- [E42] TrimUI 官方 Brick 固件 1.1.1：<https://github.com/trimui/firmware_brick/releases/tag/v1.1.1-20251126>
- [E43] TrimUI 官方 Smart Pro 固件 1.1.1：<https://github.com/trimui/firmware_smartpro/releases/tag/v1.1.1>
- [E44] TrimUI 官方 Brick Pro SD 基础包：<https://github.com/trimui/assets_brickpro/releases/tag/v1.0.0-20260626>
- [E45] 原厂 DTB `sunxi.fex` 的 OPP/PMIC/按键节点（从 E41 固件提取；`opp-hz`、`powerkey@0`、`lcd_x/lcd_y`）
- [E46] 原厂 `rootfs:/usr/trimui/res/lang/en.lang`、`rootfs:/usr/trimui/features.json`、`rootfs:/usr/trimui/osd/widgets/*`（从 E41 提取）
- [E47] NextUI README（特性列表/深睡/LED/显示/网络）：<https://github.com/LoveRetro/NextUI/blob/main/README.md>
- [E48] Knulli Changelog（设备设置/电源/RGB/开关）：<https://github.com/knulli-cfw/distribution/blob/knulli-main/knulli-Changelog.md>
- [E49] CrossMix README/发布说明（Smart Pro、Brick 兼容性、功能）：<https://github.com/cizia64/CrossMix-OS>、<https://github.com/cizia64/CrossMix-OS/releases>
- [E50] TrimUI 官方 Brick Pro 产品页（5000 mAh、双 Hall 摇杆、16 键、Home/FN、LED 区域）：<https://www.trimuistore.com/products/trimui-brick-pro-handheld-game-console>
- [E51] Knulli 显示/电源 hooks：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/etc/pm/sleep.d/10-hide-ui#L5-L28>、<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/etc/pm/sleep.d/15-batteryplus-wakeup#L7-L12>、<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/fsoverlay/etc/pm/sleep.d/99-powerled#L8-L17>
- [E52] Knulli 启动/输入/Wi-Fi 模块：<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/init.d/S31emulationstation#L16-L47>、<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/init.d/S03modules#L5-L18>、<https://github.com/knulli-cfw/knulli-linux/blob/knulli-main/board/allwinner/a133/fsoverlay/etc/init.d/S12trimuiinput#L6-L22>

## 附录 C：本文没有做的事

- 没有把 NextUI/MinUI/Knulli/CrossMix 的代码或脚本复制进 brickdesk；没有采用它们的 UI 资源、主题、字体或存档格式。
- 没有实现产品代码；没有修改 `prototype/`。
- 没有在真机上执行任何命令；所有硬件行为结论都来自源码/固件，真机验证项见第 4 节。
- 没有把“游戏切换器/存档位/快速恢复”设计成桌面内置功能；它们属于第 3.8 节的扩展协议。
