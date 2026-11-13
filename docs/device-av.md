# 显示与音频设备后端（Brick Pro / TG4040）

> 状态：宿主构建与单元测试通过；aarch64 交叉编译和 GLIBC 检查通过；
> **真机行为全部未验证**。设备到货后按第 10 节校准，并把结果补回本文。
>
> 本文只覆盖 `src/device/brightness*`、`src/device/audio*` 和
> `include/brickdesk/device_av.h`。不涉及电源、PMIC、LED、马达、输入守护进程，
> 也不修改设置页和 menu 的实现。

## 1. 结论

这次实现的是一个小型、可拒绝、可诊断的设备能力层：

- 亮度：标准 `/sys/class/backlight` 后端，以及有源码证据的 `/dev/disp`
  ioctl 后端（SET `0x102`、GET `0x103`）。
- 音频：通过 ALSA control UAPI（`/dev/snd/controlC<N>`）枚举全部 MIXER 控件，
  按**精确名称**绑定音量、静音和扬声器/耳机路径；不链接 libasound，也不按
  `tinymix` 编号写死。
- 所有写操作都回读；回读不一致返回 I/O 错误，不把失败包装成成功。
- 探测阶段不写硬件。`/dev/disp` 只有在型号白名单匹配且 GET 成功时才可用；
  ALSA 只有在卡和控件都按名称确认后才绑定角色。未知设备/未知控件不会被盲写。
- 音量单位是 0..100；亮度单位是 0..100 和 raw 两种。设置页和 menu 可以共用
  同一组函数，不需要第二次单位换算。

真机上仍然未知、因此没有编造的部分集中列在第 9 节。最重要的两项是：

1. Brick Pro 的 `digital volume` 是 0..63 的反向衰减控制，这一点有 Brick Pro
   的 `asound.state` 证据；但“0..100 线性映射到 0..63”的听感曲线需要校准。
2. 耳机插入事件本身由输入层读取（`SW_HEADPHONE_INSERT`），本模块只提供
   `bd_av_audio_set_jack()` 和路由切换。谁调用它、是否与 keymon/inputd 重复，
   需要真机接线确认。

## 2. 能力矩阵

| 能力 | 状态 | 后端/条件 | 说明 |
|---|---|---|---|
| 亮度读取 | 已实现，宿主测试 | sysfs 或 `/dev/disp` | sysfs 读 `brightness`/`max_brightness`；disp 用 GET `0x103` |
| 亮度设置 | 已实现，宿主测试 | sysfs 或 `/dev/disp` | 百分比或 raw；写入后回读校验 |
| 亮度 0 的语义 | 已实现，未校准 | 两种后端 | 0% 映射 raw 0；NextUI 在 Brick/Brick Pro 上把 UI 0 映射到 raw 1，真机需决定是否设下限 |
| 亮度探测 | 已实现 | sysfs 优先，disp 回退 | 探测不写；disp 需型号白名单 + GET 成功 |
| ALSA 音量 | 已实现，宿主测试 | `digital volume` 精确名称 | 默认反向映射；范围来自控件，不写死 0..63 |
| 静音 | 已实现，宿主测试 | `/sys/class/speaker/mute`，或可选 ALSA 开关 | 默认 sysfs；配置 `mute_control` 可改用 ALSA BOOLEAN |
| 扬声器/耳机路径 | 已实现，宿主测试 | `HpSpeaker Switch` + `Headphone Switch` | 两个都绑定才具备路由能力；写入后回读 |
| 耳机状态 | API 已实现，接线未做 | 调用方喂 `SW_HEADPHONE_INSERT` | 本模块不读 evdev；`set_jack()` 记录并自动切换 |
| 耳机音量控件 | 只枚举，不管理 | `Headphone Volume` | 能力标志 `has_headphone_volume`；没有专用 setter |
| 蓝牙 A2DP / USB DAC | 明确不支持 | — | 不在本任务范围 |
| 色温/对比度/饱和度 | 明确不支持 | — | 不在本任务范围 |
| `dispdbg` debugfs 亮度 | 明确不支持 | — | CrossMix 使用；依赖 debugfs/root，未作为后端 |
| 电源/PMIC/LED/马达 | 不在范围 | — | 不实现 |

## 3. 公开 API

头文件：`include/brickdesk/device_av.h`。

### 3.1 返回值和错误

```c
typedef enum bd_av_result {
  BD_AV_OK = 0,
  BD_AV_ERR_ARGUMENT,   /* 参数为空、配置非法 */
  BD_AV_ERR_UNSUPPORTED,/* 设备/角色/ABI 不可用，拒绝执行 */
  BD_AV_ERR_IO,         /* 打开、读写、ioctl、回读失败 */
  BD_AV_ERR_RANGE,      /* 超出 0..100 或控件范围 */
  BD_AV_ERR_STATE       /* 状态不允许，例如 AUTO 路由没有 jack 状态 */
} bd_av_result;
```

每个操作都可以传 `bd_av_error *`；失败时 `code` 非 `OK`，`message` 给出可读原因。
`bd_av_result_name()` 用于日志。注意：`BD_AV_ERR_UNSUPPORTED` 不是“成功但没做事”，
而是明确的拒绝。

### 3.2 亮度

```c
bd_av_result bd_av_brightness_probe(bd_av_brightness *b,
                                    const bd_av_brightness_config *config,
                                    bd_av_error *error);
bd_av_result bd_av_brightness_get_raw(const bd_av_brightness *b, int *raw,
                                      bd_av_error *error);
bd_av_result bd_av_brightness_set_raw(bd_av_brightness *b, int raw,
                                      bd_av_error *error);
bd_av_result bd_av_brightness_get(const bd_av_brightness *b, int *percent,
                                  bd_av_error *error);
bd_av_result bd_av_brightness_set(bd_av_brightness *b, int percent,
                                  bd_av_error *error);
```

`bd_av_brightness` 保存探测结果：`backend`、`device`、`model`、`raw_min`、
`raw_max`、`readable`、`writable`。探测失败时 `backend` 为
`BD_AV_BRIGHTNESS_NONE`。

配置字段（NULL 使用默认值）：

| 字段 | 默认 | 用途 |
|---|---|---|
| `sysfs_root` | `""`（真实 `/sys`） | 测试或备用根前缀；路径规则是 `<root>/sys/class/backlight` |
| `backlight` | NULL，自动发现 | 显式 backlight 目录；相对路径相对 `sysfs_root` |
| `disp_path` | `/dev/disp` | disp 设备 |
| `model_path` | `/etc/model` | disp 型号门禁 |
| `allowed_models` | `TG4040` | 逗号分隔精确型号；不支持通配符 |
| `backend` | `AUTO` | `AUTO` 先 sysfs，再 disp |
| `disp_raw_max` | 0（按 255） | disp raw 上限 |

百分比到 raw 的映射是 `round(percent * (max-min) / 100)`，读取是反向
`round((raw-min) * 100 / (max-min))`；超出 raw 范围的值按 `BD_AV_ERR_RANGE`
拒绝，不做静默截断。

### 3.3 音频

```c
bd_av_result bd_av_audio_open(bd_av_audio *audio,
                              const bd_av_audio_config *config,
                              bd_av_error *error);
void bd_av_audio_close(bd_av_audio *audio);
bd_av_result bd_av_audio_get_volume(const bd_av_audio *a, int *percent,
                                    bd_av_error *error);
bd_av_result bd_av_audio_set_volume(bd_av_audio *a, int percent,
                                    bd_av_error *error);
bd_av_result bd_av_audio_get_muted(const bd_av_audio *a, bool *muted,
                                   bd_av_error *error);
bd_av_result bd_av_audio_set_muted(bd_av_audio *a, bool muted,
                                   bd_av_error *error);
bd_av_result bd_av_audio_get_route(const bd_av_audio *a, bd_av_route *route,
                                   bd_av_error *error);
bd_av_result bd_av_audio_set_route(bd_av_audio *a, bd_av_route route,
                                   bd_av_error *error);
bd_av_result bd_av_audio_set_jack(bd_av_audio *a, bool plugged,
                                  bd_av_error *error);
```

`bd_av_audio_open()` 只要求找到卡；某个角色缺失时 open 仍返回 `OK`，但
`has_volume` / `has_mute` / `has_route` 为 false，对应操作返回
`BD_AV_ERR_UNSUPPORTED`。这使诊断工具可以显示“卡在、但控件名不对”，而不是
直接失败或猜测。

控制表可通过以下函数读取，用于诊断：

```c
size_t bd_av_audio_control_count(const bd_av_audio *audio);
const bd_av_control_info *bd_av_audio_control_at(const bd_av_audio *audio,
                                                 size_t index);
```

默认配置：

| 字段 | 默认 | 证据 |
|---|---|---|
| `card_name` | `audiocodec` | `/proc/asound/cards`、官方 rc.local 的 `hw:audiocodec` |
| `snd_root` | `/dev/snd` | ALSA control 设备 |
| `proc_cards` | `/proc/asound/cards` | 卡发现 |
| `volume_control` | `digital volume` | NextUI `SetRawVolume()`、Brick Pro asound.state |
| `volume_map` | `DEFAULT`（反向） | `digital volume` 0 最大、63 最小 |
| `speaker_mute` | `/sys/class/speaker/mute` | 官方 keymon、NextUI |
| `mute_control` | 空（不用） | 可选 ALSA BOOLEAN 开关 |
| `speaker_switch` | `HpSpeaker Switch` | 官方 rc.local、Knulli Brick Pro asound.state |
| `headphone_switch` | `Headphone Switch` | 同上 |
| `headphone_volume` | `Headphone Volume` | 同上 |
| `manual_route` | false | 默认 jack 边沿自动切换 |

音量耦合规则：`set_volume(0)` 在绑定静音源时写入静音；之后 `set_volume(>0)`
会清除这个自动静音，但不会清除显式 `set_muted(true)` 的状态。`get_muted()`
始终读硬件，不返回缓存值。

`get_volume()` 读取控件第一个通道的 raw 值；`set_volume()` 把同一个 raw 写入
所有通道并逐个回读。如果其他进程单独改了某个声道，读回值仍以第一个通道为准；
需要区分声道时应直接看 `bd_av_audio_control_count/at()` 暴露的控制表。

写入不是事务：音量写成功但随后的静音写失败、或路由先写一路成功再写另一路失败
时，函数返回 I/O 错误，但硬件可能已经部分改变。调用方收到错误后应重新调用
`bd_av_audio_get_volume()` / `get_muted()` / `get_route()` 同步实际状态，不要把
错误当成“完全没动”。亮度同理：SET 成功但回读不一致时可能已经改变了背光。

路由状态是实际开关组合：`SPEAKER`、`HEADPHONE`、`BOTH`、`NONE`。Brick Pro 的
`asound.state` 初始两者都是 `true`，所以 `get_route()` 会返回 `BOTH`，这不是
错误。只有调用 `set_route(SPEAKER|HEADPHONE)` 才会强制互斥。

## 4. 探测与写入规则

### 4.1 亮度

sysfs 后端：

1. 自动扫描 `<sysfs_root>/sys/class/backlight`，按名称排序。
2. 第一个同时满足“`max_brightness` 可读且 > 0”“`brightness` 可读且
   0..max”的目录被选中。
3. 显式 `backlight` 路径无效时直接 `UNSUPPORTED`，不会悄悄改用 disp。
4. 写 `brightness` 后重新读取，必须等于写入值；否则 `BD_AV_ERR_IO`。

disp 后端：

1. 先读 `model_path`，必须精确匹配 `allowed_models` 中的一项（默认仅
   `TG4040`）。不匹配时不打开 `/dev/disp`。
2. 以 O_RDWR 打开；失败再以 O_RDONLY 打开并标记 `writable=false`。
3. 调用 GET `0x103`，参数为 4 个 `unsigned long`（[0]=screen 0）。返回值必须
   在 0..`disp_raw_max`。
4. 只有 GET 成功才把后端标记为可用；GET 失败或越界时拒绝写。
5. 写 SET `0x102`（[0]=screen 0，[1]=raw），成功后再 GET 回读；不一致时报错。

注意：GET 的返回值语义来自 Allwinner `dev_disp.c` 的 `ret = get_bright()`；
如果厂商 4.9.191 内核改了语义，探测会拒绝，而不是继续写。

### 4.2 音频

1. 从 `proc_cards` 按 `card_name` 精确匹配卡 id；或调用方显式设置
   `card_by_index` + `card_index`。
2. 打开 `<snd_root>/controlC<N>`，用 `SNDRV_CTL_IOCTL_CARD_INFO` 校验 card
   index 和 id。
3. 用 `SNDRV_CTL_IOCTL_ELEM_LIST` 枚举全部 MIXER 控件，再逐个
   `ELEM_INFO` 读取类型、访问权限、count、min/max/step。枚举失败则关闭并报错。
4. 角色只按 `strcmp` 精确名称绑定；类型、读写权限、count 和范围都必须满足。
   没有“尝试 card 0”“找名字包含 Headphone 的控件”这类猜测。
5. 写入后立即回读；音量检查所有通道，开关检查 0/1。不一致返回 I/O 错误。

`bd_av_audio_open()` 返回 `OK` 但 `has_*` 为 false 是合法的“能力缺失”状态；
调用方应先看能力标志，再暴露 UI 动作。

## 5. 证据与版本

> 许可说明：NextUI 使用 PolyForm Noncommercial 1.0.0；MinUI 没有正式许可证；
> Knulli 是 Batocera 派生树（GitHub 显示 NOASSERTION，GPL 系）；CrossMix 是
> GPL-3.0。本文和实现只使用接口/行为事实，不复制上述项目的代码。brickdesk
> 保持本仓库原有许可证。

### 5.1 官方 Brick Pro 1.1.1 固件

- 发布页：<https://github.com/trimui/firmware_brickpro/releases/tag/v1.1.1-20260717>
  （2026-07-18 发布；包 `trimui_tg4040_20260717_v1.1.1.7z`）。
- 本地解包核对（不随仓库分发厂商文件）：
  - `rootfs:/etc/model` = `TG4040`
  - `rootfs:/etc/version` = `1.1.1`
  - `rootfs:/usr/trimui/bin/keymon` 含 `/dev/disp`、`setLCDBrightness %d`、
    `/sys/class/speaker/mute` 字符串。
  - `rootfs:/etc/rc.local` 第 5–7 行：
    `Headphone Switch 1`、`Headphone Volume 3`、`HpSpeaker Switch 1`；第 10–12
    行是 LINEOUT 相关注释。
  - `rootfs:/usr/trimui/bin/runtrimui.sh` 第 209–212 行：
    `tinymix set 9 1`、`tinymix set 1 0`，注释 `#tg4040 speaker` 和
    `# tinymix set 7 145 145`。

这组证据确认了型号、`/dev/disp` 存在、扬声器静音节点存在，以及官方使用的
三个 mixer 名称。它**不**证明 `digital volume` 的映射方向；方向来自下一节的
Brick Pro 状态转储和 NextUI 行为。

### 5.2 NextUI

- 仓库：<https://github.com/LoveRetro/NextUI>
- 核对 commit：`b6ec1343c0a2636ddaf2502fde52cff237c67377`（2026-11-12）。
- 相关文件与事实：
  - `workspace/tg5040/libmsettings/msettings.c#L1209-L1218`：
    `SetRawBrightness()` 打开 `/dev/disp`，`unsigned long param[4]={0,val,0,0}`，
    `ioctl(fd, 0x102, &param)`，范围 0–255。
  - 同文件 `#L1055-L1102`：Brick 与 Brick Pro 共用 0–10 → raw 表
    （0→1，1→8，2→16，3→32，4→48，5→72，6→96，7→128，8→160，9→192，
    10→255）。
  - 同文件 `#L1309-L1391`：`SetRawVolume()` 按名称取 `digital volume`，
    `mixer_ctl_set_percent(digital, 0, 100 - val)`；`DAC volume` 在 0 时写 0、
    否则写 160；最后写 `/sys/class/speaker/mute`。
  - 同文件 `#L798-L803`：`SetJack()` 记录耳机状态并重新应用音量。
  - `workspace/tg5040/keymon/keymon.c#L150-L155`：`EV_SW` code 2
    （`SW_HEADPHONE_INSERT`）调用 `SetJack(val)`。
- 许可：PolyForm Noncommercial 1.0.0（`LICENSE`）。仅为接口事实参考。

### 5.3 内核 `/dev/disp` ABI

- 头文件：`linux-sunxi/libvdpau-sunxi`，commit
  `ebdf7844efbb997a1e858600ae76c90985ea865d`，
  `kernel-headers/sunxi_display2.h#L470-L471`：
  `DISP_LCD_SET_BRIGHTNESS = 0x102`、`DISP_LCD_GET_BRIGHTNESS = 0x103`。
- 驱动：`allwinner-zh/linux-3.4-sunxi`，`drivers/video/sunxi/disp2/disp/dev_disp.c`：
  - `#L1380-L1402`：ioctl 从用户态复制 4 个 `unsigned long`；
  - `#L1596-L1614`：SET 使用 `ubuffer[1]`；GET 返回
    `dispdev->get_bright(dispdev)` 的结果。
- 局限：Brick Pro 的 4.9.191 厂商内核没有公开源码；上述 3.4 源码给出 ioctl
  编号和参数布局，NextUI 在 TG5040 上实际使用同一编号。实现据此只在 GET
  成功时启用 disp，并在每次写入后回读；没有为未知 ioctl 编造参数。

### 5.4 Knulli（Brick Pro 状态转储）

- 仓库：<https://github.com/knulli-cfw/knulli-linux>
- 核对 commit：`57afa662009641d181f57241bcc480119b131530`（2026-10-01）。
- `board/allwinner/a133/trimui-brick-pro/boot/asound.state`：
  - `control.4` `digital volume`，INTEGER，count 1，range `0 - 63`；
  - `control.8` `DAC volume`，INTEGER，count 2，range `0 - 255`；
  - `control.10` `Headphone Volume`，INTEGER，count 1，range `0 - 7`；
  - `control.14` `Headphone Switch`，BOOLEAN，初始 `true`；
  - `control.15` `HpSpeaker Switch`，BOOLEAN，初始 `true`。
- `package/system/knulli-audio/alsa/knulli-audio#L79-L87`（a133 分支）：
  设置 `HpSpeaker`、`Headphone`、`Headphone` 音量、`ADC volume`、
  `LINEOUT volume`、`DAC Swap Off`。
- `board/allwinner/a133/fsoverlay/usr/bin/trimui-volume-attenuate`：
  耳机插入 `amixer -c 0 sset Headphone 3`，拔出 `amixer -c 0 sset Headphone 0`。
- 许可：仓库未声明统一许可证（GitHub `NOASSERTION`，Batocera/GPL 系）。
  只使用控件名称、范围和设备行为事实。

### 5.5 CrossMix

- 仓库：<https://github.com/cizia64/CrossMix-OS>
- 核对 commit：`206d612fdef83972ed54a17de955125d0c0b3528`（2026-08-20）。
- `System/usr/trimui/scripts/lcd_off.sh#L6-L14`：`dispdbg setbl 0`；
  `lcd_on.sh#L8-L27`：读取 `system.json` 的 0–10 亮度，乘 23 写
  `dispdbg`，0 时保底 4。
- 许可：GPL-3.0。`dispdbg` 路径依赖 debugfs/root，本实现不采用，仅作为
  真机对照。

### 5.6 MinUI

- 仓库：<https://github.com/shauninman/MinUI>，无正式许可证；不作为实现来源。
  仅用于确认 tg5040 平台目录和输入/音量行为的历史背景（见
  `docs/research/device-features.md`）。

### 5.7 ALSA control UAPI

- 使用内核 UAPI `<sound/asound.h>`：`SNDRV_CTL_IOCTL_ELEM_LIST`、
  `ELEM_INFO`、`ELEM_READ`、`ELEM_WRITE`、`CARD_INFO`。
- 目标工具链（官方 GCC 7.5 linaro，20231018）自带
  `aarch64-linux-gnu/libc/usr/include/sound/asound.h`，因此不需要链接
  libasound，也不需要在设备上依赖 `amixer`/`tinymix` 的编号。
- 控件方向没有单独的 UAPI 字段；本实现不根据类型猜方向，而是按精确名称和
  已记录的设备事实选择 `volume_map`。

## 6. 设置页与 menu 接线

`bd_settings_system` 和 `bd_menu_system_ops` 都使用 0..100。两者都有单个
`user` 指针，因此用一个小的后端对象同时持有亮度和音频句柄：

```c
#include "brickdesk/device_av.h"

typedef struct av_backend {
  bd_av_brightness brightness;
  bd_av_audio audio;
} av_backend;

static bool settings_brightness(void *user, int percent) {
  av_backend *av = (av_backend *)user;
  return bd_av_brightness_set(&av->brightness, percent, NULL) == BD_AV_OK;
}

static bool settings_volume(void *user, int percent) {
  av_backend *av = (av_backend *)user;
  return bd_av_audio_set_volume(&av->audio, percent, NULL) == BD_AV_OK;
}

static bool settings_muted(void *user, bool muted) {
  av_backend *av = (av_backend *)user;
  return bd_av_audio_set_muted(&av->audio, muted, NULL) == BD_AV_OK;
}
```

```c
static int menu_brightness_get(void *user) {
  av_backend *av = (av_backend *)user;
  int percent = 0;
  return bd_av_brightness_get(&av->brightness, &percent, NULL) == BD_AV_OK
             ? percent
             : -1;
}

static bool menu_brightness_set(void *user, int percent) {
  av_backend *av = (av_backend *)user;
  return bd_av_brightness_set(&av->brightness, percent, NULL) == BD_AV_OK;
}

static int menu_volume_get(void *user) {
  av_backend *av = (av_backend *)user;
  int percent = 0;
  return bd_av_audio_get_volume(&av->audio, &percent, NULL) == BD_AV_OK
             ? percent
             : -1;
}

static bool menu_volume_set(void *user, int percent) {
  av_backend *av = (av_backend *)user;
  return bd_av_audio_set_volume(&av->audio, percent, NULL) == BD_AV_OK;
}
```

说明：

- 设置页和 menu 在同一个 UI 线程调用时无需额外锁；本模块不做线程安全承诺，
  多线程调用由调用方串行化。
- 当前设置页的 `bd_settings_system` 只有 set 回调；`bd_av_*_get()` 可用于
  页面打开时同步实际值。
- 耳机事件建议由输入层在收到 `SW_HEADPHONE_INSERT` 后调用
  `bd_av_audio_set_jack()`；如果输入层选择自己做路由，则把
  `bd_av_audio_config.manual_route` 设为 true，只记录状态。

## 7. 诊断工具

`bd-av` 随 target 构建和发布包安装到 `bin/bd-av`。它只读探测，写命令需要显式
子命令。

```sh
# 只读探测：亮度 + 音频卡/控件/路由
/mnt/SDCARD/brickdesk/bin/bd-av probe

# 亮度
/mnt/SDCARD/brickdesk/bin/bd-av brightness get
/mnt/SDCARD/brickdesk/bin/bd-av brightness set 50
/mnt/SDCARD/brickdesk/bin/bd-av brightness set raw 128

# 音频
/mnt/SDCARD/brickdesk/bin/bd-av audio status
/mnt/SDCARD/brickdesk/bin/bd-av audio controls
/mnt/SDCARD/brickdesk/bin/bd-av audio volume
/mnt/SDCARD/brickdesk/bin/bd-av audio volume 50
/mnt/SDCARD/brickdesk/bin/bd-av audio mute on
/mnt/SDCARD/brickdesk/bin/bd-av audio route headphone
```

宿主上（没有 `/dev/disp`、`/etc/model` 或声卡）会看到明确的 unsupported：

```text
bd-av: brightness probe: unsupported (no brightness backend (sysfs: no readable backlight class at /sys/class/backlight; disp: cannot identify device: cannot read /etc/model))
bd-av: audio open: io-error (cannot read /proc/asound/cards: No such file or directory)
brightness.backend=none
audio.available=no
```

真机预期（未验证）应类似：

```text
brightness.backend=disp
brightness.device=/dev/disp
brightness.model=TG4040
brightness.raw_range=0..255
brightness.readable=yes
brightness.writable=yes
brightness.current_raw=128
brightness.current_percent=50
audio.card_index=0
audio.card_id=audiocodec
audio.volume_control=digital volume bound=yes range=0..63 step=1 inverted=yes
audio.mute=sysfs bound=yes
audio.route_bound=yes speaker=HpSpeaker Switch headphone=Headphone Switch
audio.headphone_volume_bound=yes
audio.jack_known=no plugged=no
audio.volume_percent=100
audio.muted=no
audio.route=both
control.0.name=digital volume
control.0.type=integer
control.0.count=1
control.0.range=0..63 step=1
control.0.access=rw
...
```

如果 `audio.controls` 显示的名称与默认值不同，先用
`--volume-control`、`--speaker-switch` 等参数临时验证，再把真机结果写回
默认值或配置文件；不要在代码里加入模糊匹配。

## 8. 构建、测试、交叉编译

### 8.1 宿主

```sh
cmake -S . -B build-host -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBRICKDESK_WERROR=ON
cmake --build build-host --parallel
ctest --test-dir build-host -R 'device_av|config_check' --output-on-failure
```

`bd_device_av_test` 使用真实临时 sysfs 文件、假 `/dev/disp` ioctl 和假 ALSA
mixer，覆盖：

- 亮度 sysfs 自动发现、排序、raw/percent 单位、范围、写回读、坏值、缺设备、
  写失败、回读失败；
- 亮度 disp 的型号门禁、GET 失败、raw 上限、SET 失败、回读不一致、只读打开；
- 音频卡缺失/打开失败/枚举失败、控件缺失、错误类型、inactive 权限；
- 反向音量映射、0/100 边界、0 音量自动静音、显式静音保持、写失败、读失败、
  回读不一致；
- sysfs 静音缺失/坏值/写失败，ALSA 静音开关；
- 扬声器/耳机路由、BOTH 初始状态、AUTO 无 jack、jack 自动切换、手动路由、
  单开关缺失、写失败、回读不一致；
- `/proc/asound/cards` 解析。

### 8.2 target 交叉编译

官方工具链 + SDK + patched SDL2 的完整流程与仓库现有 M1 文档一致；本模块只
新增一个静态库和一个 `bd-av` 可执行文件，不需要新的链接库。

```sh
export TRIMUI_TOOLCHAIN=/opt/trimui/toolchain
export TRIMUI_SDK=/opt/trimui/sdk
export TRIMUI_SYSROOT=/opt/trimui/sysroot

scripts/build-sdl2.sh --prefix "$TRIMUI_SYSROOT"
scripts/build-target.sh
file build-target/bd-av
aarch64-linux-gnu-readelf --version-info build-target/bd-av |
  grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1
ci/check-glibc.sh --max 2.23 build-target/bd-av
```

`scripts/build-target.sh` 会把 `bd-av` 放进 `dist/bin/` 并纳入 GLIBC 检查；
`scripts/package-target.sh` 会把它放进发布包 `payload/bin/bd-av`，并把本文放进
`docs/device-av.md`。

本次在 orb 内用官方 `20231018` 工具链（GCC 7.5.0 linaro）、SDK 和 patched
SDL2 实际验证：

```text
build-target/bd-av: ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV),
  dynamically linked, interpreter /lib/ld-linux-aarch64.so.1,
  for GNU/Linux 3.7.0
build-target/bd-av: max GLIBC 2.17 (limit 2.23) OK
NEEDED: libc.so.6
```

发布包 `brickdesk-0.1.0-aarch64.zip` 中包含
`payload/bin/bd-av` 和 `docs/device-av.md`；包测试（安装、升级、崩溃回退、
卸载、purge）通过。CI 使用相同的 target workflow 检查 `bd-av` 的 aarch64
格式和 `GLIBC <= 2.23`。

## 9. 仍未知 / 明确不支持

以下项目没有足够证据，代码不实现或明确返回 unsupported：

1. **Brick Pro 4.9.191 内核的 `/dev/disp` 实现**没有公开源码。GET 的返回值
   语义来自 Allwinner 3.4 源码；如果 4.9 驱动不同，探测会拒绝。真机必须验证
   GET 和 SET 的 raw 范围、返回值、0 的行为。
2. **`/sys/class/backlight` 是否存在**未确认。若不存在且 `/dev/disp` 不可用，
   亮度的所有操作返回 unsupported。
3. **亮度 0 的可见性**：NextUI 把 UI 0 映射 raw 1，CrossMix 的恢复脚本保底 4；
   本实现 0% 就是 raw 0。真机需要决定是否在设置层加下限，而不是在驱动层偷偷
   改值。
4. **耳机插入检测**不在本模块。输入层是否已经把 `SW_HEADPHONE_INSERT` 转成
   回调、是否与 keymon/inputd 重复处理，需要接线和真机确认。
5. **路由语义**：Brick Pro 状态转储中 `HpSpeaker Switch` 与 `Headphone Switch`
   初始都为 true。硬件可能在插入耳机时自行静音扬声器；本模块的互斥路由是
   “主动关闭另一路”，是否必要/是否有 pop 需要真机校准。
6. **音量曲线**：`digital volume` 的 raw 是衰减值，0..100 线性映射只是可预测
   的 API 单位，不是听感等响曲线。真机需要记录各百分比的响度，必要时在设置层
   做曲线，而不是改控件方向。
7. **`Headphone Volume`** 只枚举能力，不自动写入。官方 rc.local 写 3，
   Knulli 写 0 或设置值；本模块不替调用方决定。
8. **蓝牙 A2DP、USB DAC** 未实现；`bluealsa` 是独立 mixer 插件，不属于内置
   codec 控件。
9. **`dispdbg`** 未实现。它依赖 `/sys/kernel/debug/dispdbg` 和 debugfs 挂载，
   没有公开的稳定 ABI。
10. **挂起/恢复**：本模块不处理 PMIC 或 `mem`。恢复后如果 control fd 失效，
    调用方应重新 `bd_av_audio_close/open`；当前没有自动 refresh。
11. **多声卡/热插拔**：只按 `card_name` 或显式 index 选择一张卡；不处理运行中
    换卡。`card_by_index` 是显式选择，不是猜测。
12. **线程安全**：无内部锁；设置页和 menu 需要串行调用。
13. **持久化**：本模块不读写配置文件；亮度/音量/静音的持久化仍由
    `bd_config` 和设置页负责。
14. **电源/PMIC/LED/马达/色温/对比度**：不在本任务范围。

## 10. 真机校准清单

设备到货后按顺序执行；每项记录实际输出和结论，未完成前不要把本文状态改成
“已验证”。

### 10.1 身份与节点

- [ ] `cat /etc/model` 是 `TG4040`；若固件升级后变化，更新
      `bd_av_brightness_config.allowed_models` 前先确认。
- [ ] `ls -l /dev/disp`、`ls -l /dev/snd/controlC*`、`cat /proc/asound/cards`。
- [ ] `bd-av probe` 能打开亮度后端和 `audiocodec` 卡。
- [ ] `ls /sys/class/backlight` 是否存在；存在时 AUTO 是否选中了它，raw max
      是多少。

### 10.2 亮度

- [ ] GET `0x103` 的返回值与当前可见亮度大致对应；记录 0、1、8、128、255。
- [ ] 依次 `bd-av brightness set 100/75/50/25/0`，记录实际观感、是否闪屏、
      0 是否全黑、1 是否仍可见。
- [ ] `bd-av brightness set raw 0/1/8/255`，确认 raw 范围就是 0..255；若 GET
      返回值不等于 SET，记录差异和是否稳定。
- [ ] 息屏/恢复、重启后亮度是否保持；是否需要先写一个可见的最小值。
- [ ] 如果 sysfs 和 disp 同时存在，比较两者是否指向同一背光；AUTO 的优先级
      是否符合预期。

### 10.3 音量与静音

- [ ] `amixer -c 0 contents` 和 `tinymix contents` 保存完整输出；与
      `bd-av audio controls` 对照。
- [ ] 确认 `digital volume` 的 range 是 0..63、方向是 0 最大；如果固件不同，
      记录实际 range 和方向，再调整 `volume_map`。
- [ ] `bd-av audio volume 100/75/50/25/0`，记录响度、底噪、是否有跳变。
- [ ] 0 音量时 `/sys/class/speaker/mute` 是否为 1；非 0 时是否为 0；是否真正
      消除底噪。
- [ ] `bd-av audio mute on/off` 是否与 FN Silent Mode、原厂 keymon、
      hardwareservice 冲突；`/tmp/system/muted` 是否被其他进程改写。
- [ ] 关闭 brickdesk 回原厂 UI 后音量/静音是否恢复，是否出现“回桌面无声”。

### 10.4 耳机与路由

- [ ] 插入/拔出耳机时哪个 `/dev/input/event*` 产生 `EV_SW` code 2；用
      `evtest` 记录。
- [ ] 初始 `bd-av audio status` 的 route 是否为 `both`（两个开关都为 true）。
- [ ] `bd-av audio route speaker` / `headphone` 后，扬声器和耳机实际出声情况；
      是否有 pop、是否必须保持某一路开启。
- [ ] 插入耳机后原厂是否已经物理静音扬声器；如果是，评估是否还需要主动路由。
- [ ] `Headphone Volume` 的 0..7 实际效果；官方 3、Knulli 0/设置值哪个合适。
- [ ] 把 `SW_HEADPHONE_INSERT` 接到 `bd_av_audio_set_jack()`，验证自动切换；
      同时确认没有和 keymon/inputd 重复处理。
- [ ] 反复插拔 20 次，检查路由状态、音量、底噪、是否有粘滞。

### 10.5 生命周期与共存

- [ ] 启动、退出应用回桌面、息屏/唤醒、重启后分别检查亮度、音量、静音、路由。
- [ ] 深睡 20 次后重新 `bd_av_audio_open()`，确认 control 设备仍可用。
- [ ] 与原厂 keymon/hardwareservice 同时运行时，谁最后写值；是否出现“设置页
      改完又被原厂改回”。
- [ ] 记录所有失败路径的 `bd-av` 输出，作为后续修复证据。

## 11. 文件

- `include/brickdesk/device_av.h`
- `src/device/device_av_internal.h`
- `src/device/brightness.c`
- `src/device/brightness_common.c`
- `src/device/audio.c`
- `src/device/audio_alsa.c`
- `tests/device_av_test.c`
- `tools/bd-av.c`
- `docs/device-av.md`（本文）
