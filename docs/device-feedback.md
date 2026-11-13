# LED 与震动设备后端（M4-LED/Rumble）

> **状态：宿主验证通过，未在 Brick Pro / TG4040 真机验证。**
> 宿主单元测试、故障注入测试、LED 分区/马达假 sysfs 写入和官方 aarch64 工具链
> 交叉编译已通过；真实 `/sys/class/leds`、TG4040 的 `/sys/class/led_anim`、
> evdev FF 节点和马达都还没有真机证据。本文不声称任何未经真机验证的硬件行为。
>
> 本文覆盖 `include/brickdesk/device_feedback.h`、`src/device/feedback*`、
> `include/brickdesk/led.h`、`src/app/led.c`、`include/brickdesk/haptics.h`、
> `src/app/haptics.c`、`tools/bd-feedback.c` 和 `tests/device_feedback_test.c`。

## 1. 结论与范围

这次实现的是一个小的、可拒绝、可诊断的设备能力层：

- **LED**：只写标准 Linux LED class
  `/sys/class/leds/<device>/brightness`，范围来自 `max_brightness`。
  读/写后回读；活动 trigger 不是 `none` 时拒绝，避免把“trigger 控制中”误报成
  写入成功。自动发现必须显式开启，默认要求选择已知节点。
- **震动**：只写标准 evdev force feedback。打开显式指定的
  `/dev/input/eventN`，要求 `EV_FF`、`FF_RUMBLE` 和至少一个效果槽；用
  `EVIOCSFF` 上传 `FF_RUMBLE`，用一次 `EV_FF` 写入播放，用 `EV_FF` 0 +
  `EVIOCRMFF` 停止并删除。`close()` 自动停止和删除，避免效果残留。
- **TG4040 厂商 LED**：写 `/sys/class/led_anim/*` 的五个分区
  （`m`、`lr`、`f1`、`f2`、`rear`）：`max_scale*` 亮度、`effect_*` 效果、
  `effect_rgb_hex_*` 颜色、`effect_cycles_*` 循环次数、`effect_duration_*`
  时长。写入顺序与 NextUI 一致：亮度、cycles、duration、颜色，最后写
  `effect_*` 应用。
- **TG4040 厂商马达**：写 `/sys/class/motor/voltage`（微伏）和
  GPIO227 `value`。Brick Pro 上限 2.5 V，最小 0.5 V；0 写最大电压后
  GPIO 0，与 NextUI 的 `PLAT_setRumble()` 一致。GPIO 未导出时尝试
  export 并设为 output。
- **产品层**：`bd_led` 把 `[led]` 配置和系统状态映射为 default/off/
  low-battery/critical/charging/sleep/ambient 七种 profile；`bd_haptics`
  只负责脉冲时间线，不阻塞事件循环。
- 所有写操作都有配置上限。超过上限返回 `out-of-range`，**不做静默截断**，
  也不写硬件。缺节点、缺能力或未选择节点都是显式 `unsupported`，不会假成功。

明确不做：

- 不实现 `/dev/i2c-*`、AXP2202 寄存器、PMIC 关机/上电顺序。
- 不写 `frame_hex` / `anim_frames_enable` 帧动画；分区 effect 已覆盖需求。
- 不实现 FF_PERIODIC/FF_CONSTANT 等其他效果，也不设置 `FF_GAIN`。
- 不做事件节点自动猜测。选择错误的 event 节点可能让外接手柄震动，因此必须
  由调用方给出路径。
- 不把 `effect_*` 编号猜成更多语义；产品层只用 NextUI 已使用的 1..15。

## 2. 能力矩阵

| 能力 | 状态 | 条件 | 说明 |
|---|---|---|---|
| 标准 LED 读取 | 已实现，宿主测试 | `/sys/class/leds/<dev>/brightness`、`max_brightness` | 读原始值并换算 0..100 |
| 标准 LED 设置 | 已实现，宿主测试 | 同上，且 `brightness` 可写 | 写入后回读，必须完全一致 |
| LED 0% | 已实现，宿主测试 | 同上 | 写 raw 0；不自动加最小可见值 |
| LED trigger 门禁 | 已实现，宿主测试 | `trigger` 可读 | probe 时及每次写前复查；活动 trigger 不是 `none` 时 `unsupported` |
| LED 自动发现 | 已实现，宿主测试 | `allow_auto_discover=true` | 按名称排序，选第一个可用项；默认关闭 |
| 标准 LED 多色 | 明确不支持 | — | 4.9.191 无 multicolor 文档；独立颜色 LED 可分别打开 |
| `/sys/class/led_anim` 分区 | 已实现，宿主假 sysfs | 目录和 `effect_<zone>` 存在 | 写亮度/效果/颜色/cycles/duration |
| LED 分区 f1/f2 | 已实现，宿主假 sysfs | `max_scale_f1f2` 存在 | 共用亮度；f2 只写效果/颜色 |
| 厂商 LED 关闭 | 已实现，宿主假 sysfs | 任一分区存在 | 所有分区 `max_scale*` 写 0 |
| evdev FF 探测 | 已实现，宿主测试 | 显式 event 节点 | `EV_FF` + `FF_RUMBLE` + 效果槽 |
| evdev FF 播放 | 已实现，宿主测试 | 同上 | 强度 0..100，时长 ms，效果为 `FF_RUMBLE` |
| evdev FF 停止/清理 | 已实现，宿主测试 | 同上 | `EV_FF` 0 + `EVIOCRMFF`；`close()` 也执行 |
| `FF_PERIODIC` 回退 | 明确不支持 | — | 只接受 `FF_RUMBLE`，避免猜波形语义 |
| `FF_GAIN` | 明确不支持 | — | 不改变设备全局增益 |
| `/sys/class/motor/voltage` | 已实现，宿主假 sysfs | 节点可写 | 0.5–2.5 V 线性映射，写后 GPIO |
| GPIO227 马达开关 | 已实现，宿主假 sysfs | `value` 可写 | 未导出时写 `export`，再写 `direction=out` |
| 马达脉冲时间线 | 已实现，宿主测试 | 马达可用 | 8 步 pattern；shell 负责 deadline |

## 3. 公开 API、单位与配置

头文件：`include/brickdesk/device_feedback.h`。

### 3.1 返回值

```c
typedef enum bd_feedback_result {
  BD_FEEDBACK_OK = 0,
  BD_FEEDBACK_ERR_ARGUMENT,   /* 参数/配置非法 */
  BD_FEEDBACK_ERR_UNSUPPORTED,/* 缺节点、缺能力、厂商路径未验证 */
  BD_FEEDBACK_ERR_IO,         /* 打开、读写、ioctl、回读、清理失败 */
  BD_FEEDBACK_ERR_RANGE,      /* 超出 0..100 或配置上限 */
  BD_FEEDBACK_ERR_STATE       /* 未 probe、已 close 等状态错误 */
} bd_feedback_result;
```

每个操作可传 `bd_feedback_error *`；失败时 `code` 非 OK，`message` 给出路径和
原因。`bd_feedback_result_name()` 用于日志。`unsupported` 是明确拒绝，不是
“成功但没做事”。

### 3.2 LED

```c
bd_feedback_result bd_feedback_led_probe(
    bd_feedback_led *led, const bd_feedback_led_config *config,
    bd_feedback_error *error);
bd_feedback_result bd_feedback_led_get(
    const bd_feedback_led *led, int *percent, bd_feedback_error *error);
bd_feedback_result bd_feedback_led_set(
    bd_feedback_led *led, int percent, bd_feedback_error *error);
```

配置字段：

| 字段 | 默认 | 用途 |
|---|---|---|
| `sysfs_root` | `""`（真实 `/sys`） | 测试/备用根前缀；路径规则 `<root>/sys/class/leds` |
| `led` | NULL | 显式 LED 目录；绝对路径按 `sysfs_root` 解析，相对路径拼接 |
| `name` | NULL | 精确的 `/sys/class/leds/<name>`，必须是单段名称 |
| `allow_auto_discover` | false | 未选节点时按排序扫描；默认关闭 |
| `max_percent` | 0（按 100） | 写入上限 1..100；请求超限直接拒绝 |
| `backend` | `AUTO` | `AUTO`/`SYSFS` 都走标准 class；`NONE` 显式禁用 |

`led` 和 `name` 同时设置会返回 `invalid-argument`；显式路径找不到时不会回退到
其他 LED。探测成功时 `backend=SYSFS`，`raw_max=max_brightness`，初始
`brightness` 必须在 0..raw_max；否则 `unsupported`。

百分比到 raw 的映射是
`round(percent * raw_max / 100)`，读取是反向
`round(raw * 100 / raw_max)`。`max_brightness=1` 的开关型 LED 也适用：
非零百分比映射到 1，0 映射到 0。

LED 没有 `close()`，也不会在进程退出时自动恢复。原因是节点可能被系统或其他
进程共享，自动写回可能覆盖别人的状态。调用方如果“拥有”该 LED，应在退出前
显式 `bd_feedback_led_set(led, 0)` 或写回保存的值；本文和测试都按这个契约执行。

### 3.3 TG4040 厂商 LED 分区

```c
typedef enum bd_feedback_led_zone {
  BD_FEEDBACK_LED_ZONE_M = 0,  /* 顶栏 / Logo */
  BD_FEEDBACK_LED_ZONE_LR,     /* Brick Pro 摇杆环 */
  BD_FEEDBACK_LED_ZONE_F1,
  BD_FEEDBACK_LED_ZONE_F2,
  BD_FEEDBACK_LED_ZONE_REAR,   /* 扳机 */
  BD_FEEDBACK_LED_ZONE_COUNT
} bd_feedback_led_zone;

bd_feedback_result bd_feedback_led_anim_probe(
    bd_feedback_led_anim *led, const bd_feedback_led_anim_config *config,
    bd_feedback_error *error);
bd_feedback_result bd_feedback_led_anim_apply(
    bd_feedback_led_anim *led, bd_feedback_led_zone zone,
    const bd_feedback_led_anim_settings *settings, bd_feedback_error *error);
bd_feedback_result bd_feedback_led_anim_apply_all(
    bd_feedback_led_anim *led,
    const bd_feedback_led_anim_settings settings[BD_FEEDBACK_LED_ZONE_COUNT],
    bd_feedback_error *error);
bd_feedback_result bd_feedback_led_anim_off(bd_feedback_led_anim *led,
                                            bd_feedback_error *error);
```

| 字段 | 范围 | 说明 |
|---|---|---|
| `brightness` | 0..100 | 写 `max_scale` / `max_scale_lr` / `max_scale_f1f2` / `max_scale_rear` |
| `effect` | 0..255 | 厂商编号；产品层只用 1 linear / 2 breathe / 3 interval-breathe / 4 static 及 5..15 |
| `cycles` | -1..65535 | `-1` 无限循环 |
| `duration_ms` | 1..60000 | 写 `effect_duration_*` |
| `rgb` | 0x00RRGGBB | 写 `effect_rgb_hex_*` |

`f2` 不写亮度，因为 Brick Pro 的 f1/f2 共用 `max_scale_f1f2`；f2 的颜色、
效果、cycles、duration 仍然独立。probe 只检查 `effect_<zone>` 是否存在，
不写任何节点。缺分区返回 `unsupported`；值越界返回 `out-of-range` 且不写。

产品层 `bd_led`（`include/brickdesk/led.h`）负责 profile：

| profile | 行为 |
|---|---|
| default | 按 `[led.<zone>]` 的 brightness/color/effect 应用 |
| off | 所有分区亮度 0 |
| low battery | interval-breathe 0xFF3300，无限 |
| critical battery | interval-breathe 0xFF0000，无限 |
| charging | breathe 0x00FF00，无限 |
| sleep | breathe，用户颜色，5 个 cycle |
| ambient | static，调用方颜色，按 `led.ambient_brightness` |

### 3.4 TG4040 厂商马达

```c
bd_feedback_result bd_feedback_motor_open(
    bd_feedback_motor *motor, const bd_feedback_motor_config *config,
    bd_feedback_error *error);
bd_feedback_result bd_feedback_motor_set(bd_feedback_motor *motor,
                                         int percent,
                                         bd_feedback_error *error);
bd_feedback_result bd_feedback_motor_stop(bd_feedback_motor *motor,
                                          bd_feedback_error *error);
bd_feedback_result bd_feedback_motor_close(bd_feedback_motor *motor,
                                           bd_feedback_error *error);
```

默认 `min_voltage_uv=500000`、`max_voltage_uv=2500000`、`max_percent=100`、
`gpio=227`。`percent` 0..100 线性映射到电压；0 和 100 都写最大电压，
GPIO 分别写 0/1。写入顺序是先电压、后 GPIO，避免第一次脉冲沿用上一个
电压。`bd_haptics` 在其上实现最多 8 步的 pattern，shell 用
`bd_haptics_next_deadline()` 唤醒并在 deadline 写 0。

### 3.5 震动

```c
bd_feedback_result bd_feedback_rumble_probe(
    bd_feedback_rumble *rumble, const bd_feedback_rumble_config *config,
    bd_feedback_error *error);
bd_feedback_result bd_feedback_rumble_play(
    bd_feedback_rumble *rumble, int strength, int duration_ms,
    bd_feedback_error *error);
bd_feedback_result bd_feedback_rumble_stop(
    bd_feedback_rumble *rumble, bd_feedback_error *error);
bd_feedback_result bd_feedback_rumble_close(
    bd_feedback_rumble *rumble, bd_feedback_error *error);
```

配置字段：

| 字段 | 默认 | 用途 |
|---|---|---|
| `event` | NULL | 显式 `/dev/input/eventN`；NULL 直接 `unsupported`，不自动猜 |
| `sysfs_root` | `""`（真实 `/sys`） | 仅用于 `bd_feedback_rumble.vendor_*` 的只读探测；写马达用 `bd_feedback_motor` |
| `max_percent` | 0（按 100） | 强度上限 1..100；超限直接拒绝 |
| `max_duration_ms` | 0（按 1000） | 时长上限 1..60000 ms；超限直接拒绝 |
| `backend` | `AUTO` | `AUTO`/`EVDEV` 走 evdev；`NONE` 显式禁用 |

单位：

- API 强度是 0..100 百分比。`0` 等价于 `stop()`，不上传效果。
- 映射到 `ff_rumble_effect.strong_magnitude` 和 `weak_magnitude`：
  `round(strength * 65535 / 100)`；两个马达使用相同值。
- API 时长是毫秒，写入 `ff_effect.replay.length`；内核按该长度停止 replay。
  本模块不启动定时器，也不在播放后自动认为“已停”；`active` 只表示已经成功
  写入 `EV_FF` 1。
- 每次 `play()` 会先停止并删除旧效果，再上传新效果。`stop()` 在打开且没有活动
  效果时幂等；已 close 的句柄返回 `invalid-state`。`close()` 会停止、删除并
  关闭 fd。
- 库本身没有内部线程或定时器；它只负责上传/播放/停止。`bd-feedback` CLI 的
  `rumble play` 会在播放成功后保持 event fd 打开，等待请求的 `MS`，然后才
  `close()` 停止/删除。播放失败（包括参数/能力/上传/EV_FF 写失败）不会等待，
  但仍会 close 清理；SIGINT/SIGTERM 会中断等待并走同一清理路径。0 强度是
  stop，不等待。

evdev 探测规则：

1. 以 O_RDWR 打开显式节点。`ENOENT`/`ENODEV` 是 `unsupported`，其他打开错误是
   `io-error`。
2. `EVIOCGBIT(0)` 必须包含 `EV_FF`；`EVIOCGBIT(EV_FF)` 必须包含 `FF_RUMBLE`。
3. `EVIOCGEFFECTS` 必须返回大于 0 的效果槽。
4. `EVIOCGNAME` 只用于诊断；失败不影响能力。
5. 探测阶段不上传、不播放、不写任何文件。

写入与清理不是事务：`EVIOCSFF` 成功但 `EV_FF` 播放失败时，仍可能已经分配了
效果；本实现会尝试停止并 `EVIOCRMFF`。如果清理也失败，返回 `io-error`，
`close()` 会再尝试一次；最终仍失败时，fd 仍会关闭，但本文不声称输出一定停止。
这正是 `close()` 返回结果而不是静默 void 的原因。

## 4. 标准 Linux 证据

### 4.1 LED class

- Linux v4.9.191
  [`Documentation/leds/leds-class.txt`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/Documentation/leds/leds-class.txt?h=v4.9.191)：
  LED 出现在 `/sys/class/leds/`，`max_brightness` 给出上限，`brightness` 接受
  `0..max_brightness`；非零值对没有硬件调光的 LED 就是“开”，0 是“关”。
  trigger 是可选的内核事件源；把 brightness 设为 0 也会停止 timer trigger。
- 同一文档说明 LED 命名是 `devicename:colour:function`。本实现把 `name` 当作
  精确的 class entry，不做模糊匹配。
- 4.9.191 没有 multicolor LED 文档或 `multi_intensity` ABI，因此本实现只做
  `brightness`/`max_brightness`；需要 RGB 时按颜色分别选择独立 LED，或等真机
  确认厂商接口后再加。

实现中的 trigger 门禁不是内核文档强制要求，而是“不假成功”的选择：当
`trigger` 显示 `[heartbeat]` 等活动触发器时，直接写 `brightness` 可能被内核
覆盖。本实现因此在 probe 时检查，并在**每次 `set()` 写 brightness 之前重新
读取** `trigger`；如果探测后被其他服务切到活动 trigger，`set()` 返回
`unsupported` 且不写 brightness。缺失 `trigger` 属性仍然支持，表示该 LED 没有
trigger 控制。本模块**不写** trigger。

### 4.2 evdev force feedback

- Linux v4.9.191
  [`Documentation/input/ff.txt`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/Documentation/input/ff.txt?h=v4.9.191)：
  - `EVIOCGBIT(EV_FF, ...)` 返回 `FF_RUMBLE` 等能力位；
  - `EVIOCGEFFECTS` 返回效果槽数量；
  - `EVIOCSFF` 上传效果，用户把 `ff_effect.id` 设为 -1，驱动回填 id；
  - `EVIOCRMFF` 删除效果并停止它；
  - `EV_FF` 的 `value` 是 replay 次数，0 停止；示例 `play.value = 3` 播放三次，
    `stop.value = 0` 停止；
  - `FF_GAIN` 是设备全局增益，范围换算为 `0xFFFF * gain / 100`。
- Linux v4.9.191
  [`include/uapi/linux/input.h`](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/include/uapi/linux/input.h?h=v4.9.191)：
  `struct ff_effect`、`struct ff_rumble_effect`、`FF_RUMBLE=0x50`、
  `EVIOCSFF`、`EVIOCRMFF`、`EVIOCGEFFECTS`、`EV_FF`。
- Linux v4.9.191 `drivers/input/evdev.c` 把 `EVIOCSFF` 转给
  `input_ff_upload()`，把 `EVIOCRMFF` 转给 `input_ff_erase()`；
  `drivers/input/ff-core.c` 的 `input_ff_upload()` 为 `id=-1` 分配槽位并回填 id，
  `erase_effect()` 先 `playback(..., 0)` 再释放槽位。这支持本实现的
  “上传-播放-停止/删除”顺序。

本实现只接受 `FF_RUMBLE`。`ff.txt` 指出多数设备同时支持 `FF_PERIODIC`，但在
没有真机确认波形/周期语义前，不回退到 periodic，也不猜 `FF_GAIN`。

## 5. TG4040 厂商路径：证据与 blocker

### 5.1 证据版本

| 来源 | 版本/提交 | 用途 |
|---|---|---|
| TrimUI Brick Pro 固件 | `v1.1.1-20260717`（2026-07-18） | `TG4040`、厂商 `runtrimui.sh`、`keymon`、LED/马达节点 |
| brickdesk 调研 | `docs/research/device-features.md`，2026-11-12 | 固件/源码事实汇总，证据 E2/E24/E41 |
| NextUI | `LoveRetro/NextUI`，`main`=`b6ec1343c0a2636ddaf2502fde52cff237c67377` | LED/震动接口与单位；PolyForm Noncommercial 1.0.0 |
| Knulli | `knulli-cfw/knulli-linux`，`knulli-main`=`57afa662009641d181f57241bcc480119b131530` | GPIO227 开关式震动；GPL 系 |
| CrossMix | `cizia64/CrossMix-OS`，`main`=`206d612fdef83972ed54a17de955125d0c0b3528` | `lcd_off`/`lcd_on` 使用 `max_scale`；GPL-3.0 |
| MinUI | `shauninman/MinUI`，`main`=`dbf89435a5179788418ff3573042cf9c7e75d9e0` | 仅确认 tg5040 背景；无正式许可证 |

固件发布页：
<https://github.com/trimui/firmware_brickpro/releases/tag/v1.1.1-20260717>。
本文没有重新分发固件或厂商文件。

### 5.2 NextUI 的接口事实

固定提交 `b6ec1343` 的
[`workspace/tg5040/platform/platform.c`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/tg5040/platform/platform.c)：

- 约 286–307 行：
  `MAX_STRENGTH=0xFFFF`、`MIN_VOLTAGE=500000`、
  `MAX_VOLTAGE = is_brickpro ? 2500000 : 3300000`、
  `RUMBLE_PATH="/sys/class/gpio/gpio227/value"`、
  `RUMBLE_VOLTAGE_PATH="/sys/class/motor/voltage"`。
  `PLAT_setRumble(strength)` 在 `0 < strength < 0xFFFF` 时把 strength 线性映射到
  voltage（单位微伏），否则写最大电压；然后 GPIO227 写 1/0。
- 约 478–598 行：`/sys/class/led_anim/max_scale`、`max_scale_lr`、
  `max_scale_f1f2`、`max_scale_rear`；每区还有
  `effect_<zone>`、`effect_rgb_hex_<zone>`、`effect_cycles_<zone>`、
  `effect_duration_<zone>`。Brick Pro 的 `lr` 是摇杆环，`rear` 是扳机。
  NextUI 的默认 `brightness` 是 100，但代码没有证明 `max_scale` 的合法范围是
  0..100 还是 0..255，也没有证明 effect 编号语义。

这些事实足以确认 TG4040 的节点路径和 NextUI 的实际取值。本实现直接采用：

- 分区：`m`、`lr`、`f1`、`f2`、`rear`；Brick Pro 的 `lr` 是摇杆环，
  `rear` 是扳机。
- 亮度：NextUI 默认 `brightness=100`、`inbrightness=100`，本实现按 0..100
  写入；如果真机范围是 0..255，`max_scale` 会按比例偏暗，需在真机验收时确认。
- 效果：NextUI 的 profile 明确使用 2=breathe、3=blink/interval-breathe、
  4=static；其余 1、5..15 来自 LedControl 的名称表，作为可选效果。
- 马达：Brick Pro 使用 2.5 V 上限（`is_brickpro ? 2500000 : 3300000`），
  最小 0.5 V；这是 NextUI 在 tg5040 上的实际策略，不是硬件安全证书。

真机仍需确认：`max_scale` 实际范围、`effect_*` 编号 0–N 的视觉效果、
`frame_hex` 语义、GPIO227 极性和长时间震动的热/掉压。完成前不把本文
状态改成“已验证”。

### 5.3 其他项目

- Knulli `57afa662` 的
  [`knulli-rumble`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/allwinner/a133/fsoverlay/usr/bin/knulli-rumble#L33-L42)
  直接写 GPIO227 1、sleep、写 0，没有电压控制；这说明“开关式短震”是存在的，
  但也说明它不能提供安全电压曲线。
- CrossMix `206d612` 的 `lcd_off.sh`/`lcd_on.sh` 只把 LED `max_scale` 当作
  现有状态保存/恢复，不是 LED 效果 API。
- MinUI 没有正式许可证；本实现没有使用其代码。

### 5.4 许可

NextUI 使用 PolyForm Noncommercial 1.0.0；MinUI 没有正式许可证；Knulli 为
GPL 系；CrossMix 为 GPL-3.0。本文和实现只记录接口/行为事实，不复制上述项目的
代码、脚本、寄存器序列或资源。本仓库的 C 实现是独立编写的。

## 6. 诊断工具

`bd-feedback` 随 target 构建进入 `dist/bin`，发布包安装到
`/mnt/SDCARD/brickdesk/bin/bd-feedback`。

```sh
# 只读探测：默认没有选节点时会明确 unsupported
bd-feedback probe

# 标准 LED class
bd-feedback --led /sys/class/leds/<device> led get
bd-feedback --led /sys/class/leds/<device> --led-max 40 led set 30
bd-feedback --led /sys/class/leds/<device> led off

# 或按 class entry 名称选择
bd-feedback --led-name <name> led get

# evdev FF：必须显式给 event 节点
bd-feedback --event /dev/input/eventN rumble probe
bd-feedback --event /dev/input/eventN --rumble-max 30 --rumble-max-ms 200 \
  rumble play 20 100
bd-feedback --event /dev/input/eventN rumble stop
```

`rumble play` 在 `bd_feedback_rumble_play()` 成功后不会立即 close：它保持 event
fd 打开并等待 `MS`（默认 200ms），然后停止/删除并 close。播放失败不会等待，
但仍会 close 清理；Ctrl-C/SIGINT/SIGTERM 会中断等待并清理。实际等待/清理顺序
由宿主回归测试用假 evdev 执行验证，不由源码检查代替。

宿主无硬件时的实际输出：

```text
bd-feedback: led probe: unsupported (no LED selected; set led or name (auto-discovery is opt-in))
bd-feedback: rumble probe: unsupported (no rumble event node selected; set event to the FF-capable /dev/input/eventN)
led.available=no
led.vendor_led_anim=absent
rumble.available=no
rumble.vendor_motor_voltage=absent
rumble.vendor_motor_enable=absent
```

真机预期（未验证）应出现 `led.backend=sysfs`、`led.raw_range=...`、
`led.current_percent=...`、`rumble.backend=evdev`、`rumble.ff_rumble=yes`、
`rumble.effects=...` 等行。若出现 `vendor_...=present` 和
`vendor_blocker=...`，说明厂商节点存在但本模块仍不会写它。

## 7. 构建与测试

### 7.1 宿主

```sh
cmake -S . -B build-host -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBRICKDESK_WERROR=ON
cmake --build build-host --parallel
ctest --test-dir build-host -R device_feedback --output-on-failure
```

本次宿主结果：`device_feedback` 通过；随后完整
`ctest --test-dir build-host --output-on-failure` 为 40/40 通过（RelWithDebInfo，
`BRICKDESK_WERROR=ON`）。同一测试用
`-fsanitize=address,undefined -fno-omit-frame-pointer` 单独编译运行也通过，
没有 ASan/UBSan 报告。

`bd_device_feedback_test` 使用真实临时 sysfs 文件、假文件 I/O 和假 evdev ioctl，
覆盖：

- LED 显式路径、名称选择、排序自动发现、默认拒绝自动发现；
- `max_brightness` 缺失/为 0、brightness 初始值越界/坏值、trigger 缺失/
  活动 trigger；
- raw/percent 往返、0/100 边界、配置上限拒绝且不写、写失败、回读不一致、
  只读 LED、缺文件；
- 厂商 `led_anim` 目录只读探测和 blocker，验证 `max_scale` 未被修改；
- probe 后 trigger 切到活动值，`set()` 复查拒绝且 brightness 不变；缺 trigger
  时 `set()` 仍可写；
- CLI `rumble play` 生命周期：fake evdev 上验证 wait 时 fd 仍打开、请求时长、
  wait 完成后才 stop/erase/close、事件顺序、播放失败不等待、0 强度不等待、
  中断仍清理；真实 `bd_feedback_cli_wait_ms` 的 40ms 等待和 SIGALRM 中断；
- `device_feedback_cli` 用 test-only LD_PRELOAD 假 evdev 运行**真实
  `bd-feedback` 进程**：200ms 播放期间不 close，stop/erase/close 顺序和时长；
  上传失败立即退出且不等待；SIGINT 返回 130 并完成 stop/erase/close；
- evdev 探测的 `EV_FF`/`FF_RUMBLE`/效果槽缺失、event 缺失/权限失败/ioctl 失败；
- 效果上传参数（type、id、strong/weak、replay.length、trigger）、播放、
  停止、删除、重复播放、0 强度；
- 强度/时长上限、上传失败、播放写失败后的清理、删除失败时 close 的 cleanup
  失败、重复 close、未打开状态；
- 厂商 motor/gpio 节点只读探测和 blocker，验证文件内容未被修改。

### 7.2 target 交叉编译

本 orb 没有 Docker daemon；未使用 `ci/Dockerfile.toolchain`，而是在 `/tmp` 手动
安装同一组官方 20231018 工具链、SDK 和 patched SDL2，然后运行完整 target 构建：

```sh
export TRIMUI_TOOLCHAIN=/tmp/trimui-toolchain
export TRIMUI_SDK=/tmp/trimui-sdk
export TRIMUI_SYSROOT=/tmp/trimui-sysroot
scripts/build-target.sh --build-dir /tmp/bd-target-build
```

结果：152 个构建步骤全部成功，`dist/bin/bd-feedback` 存在；
`bd-feedback` 为 aarch64 ELF，max GLIBC 2.17（限制 2.23），只 NEEDED
`libc.so.6`。同一个 target 构建也编译了 `tests/bd_device_feedback_test`、
`tests/bd_feedback_cli_process_test` 和 test-only fake evdev preload（不进入发布包）。
`bd-feedback` 已加入 `scripts/build-target.sh` 的 `dist/bin` 与 GLIBC 检查、
`.github/workflows/target.yml` 的 `test -x`/`file`/`check-glibc` 检查，以及
发布包和离线包测试。

单独交叉编译本模块的最短命令：

```sh
/tmp/trimui-toolchain/bin/aarch64-linux-gnu-gcc \
  -std=c11 -O2 -g -Wall -Wextra -Werror \
  -Iinclude -Isrc/device \
  src/device/feedback.c src/device/feedback_linux.c tools/bd-feedback.c \
  -o /tmp/bd-feedback-cross/bd-feedback
```

实际结果：

```text
bd-feedback: ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV),
  dynamically linked, interpreter /lib/ld-linux-aarch64.so.1,
  for GNU/Linux 3.7.0
max GLIBC 2.17
NEEDED: libc.so.6
```

## 8. 真机 blocker 与验收清单

设备到货后按顺序执行；每项记录实际输出。未完成前不要把本文状态改成“已验证”。

### 8.1 标准 LED

- [ ] `ls -l /sys/class/leds` 是否存在？列出所有 entry、`max_brightness`、
      `brightness`、`trigger` 的实际值。
- [ ] `cat /sys/class/leds/<entry>/trigger` 的活动 trigger 是什么？若不是
      `none`，先记录，不要由 brickdesk 改 trigger。
- [ ] `bd-feedback --led /sys/class/leds/<entry> led get` 是否显示 sysfs、
      raw range 和当前百分比。
- [ ] 依次 `led set 100/50/10/0`，确认实际亮度、是否闪、0 是否全黑；
      对 `max_brightness=1` 的 LED 记录 0/1 语义。
- [ ] 标准 class 不存在时，确认 `led.vendor_led_anim=present`，然后从设置页
      “灯光”逐区测试：亮度 0/50/100、颜色 white/red/green/blue、效果
      static/breathe/interval-breathe；记录 `max_scale*` 实际范围（0..100 还是
      0..255）和每个 effect 编号的视觉效果。
- [ ] 关闭 brickdesk 回原厂 UI 后，LED 是否停在原厂状态；是否与
      `/tmp/system/enable_led`、原厂 `hardwareservice` 冲突。

### 8.2 evdev FF

- [ ] `cat /proc/bus/input/devices`、`ls -l /dev/input/event*`，记录哪个 event
      节点属于内置马达/手柄，而不是外接蓝牙手柄。
- [ ] 用 `evtest`/`fftest` 或 `bd-feedback --event ... rumble probe` 确认
      `EV_FF`、`FF_RUMBLE`、效果槽数量。若只有 `FF_PERIODIC`，本模块按设计
      返回 unsupported。
- [ ] 从 `--rumble-max 20 --rumble-max-ms 100` 开始，记录实际震动强度、噪音、
      电流/掉压、是否影响按键/屏幕；不要直接用 100%/长时长。
- [ ] 验证 `rumble play` 后 `rumble stop` 立即停；`close`、Ctrl-C、进程被杀
      后都不持续震动。若清理失败，记录 `bd-feedback` 的 `io-error` 和 close
      返回码，并把该 event 节点视为不安全。
- [ ] 连续短震 20 次、长时间 2 秒、低电量下各记录一次；确认没有重启/掉压。
- [ ] 如果内置马达没有 evdev FF 节点，记录 `unsupported`；这是明确的真机
      blocker，不要改写成 GPIO227/电压路径。

### 8.3 厂商马达路径

- [ ] 确认 `/sys/class/motor/voltage` 和 GPIO227 存在；记录是否已 export、
      `direction` 当前值。
- [ ] 从设置页把菜单触觉强度设为 20、时长 20 ms，触发一次导航；记录实际
      强度/噪音。逐步升到 60、100，确认 2.5 V 下的发热和掉压。
- [ ] 用万用表/示波器确认 GPIO227 极性；如果 0/1 反了，记录后修正
      `bd_feedback_motor_set()` 的 value 写入。
- [ ] 验证 8 步 pattern（启动/睡眠双脉冲）在最后一步后 GPIO 归 0；
      进程被杀、桌面退出、重启后不持续震动。
- [ ] 连续短震 20 次、2 秒长震、低电量下各记录一次；确认没有重启/掉压。
- [ ] 与原厂 `keymon`/`hardwareservice` 同时运行时，确认谁最后写值；必要时
      在安装器里停用原厂马达/LED 服务。

### 8.4 生命周期与共存

- [ ] brickdesk 退出应用回桌面、息屏/唤醒、重启后，LED 和震动是否恢复/停止。
- [ ] 与原厂 `keymon`、`hardwareservice`、其他前端同时运行时，谁最后写值；
      是否出现 LED 被改回、震动残留。
- [ ] 记录所有失败路径的 `bd-feedback` 输出，作为后续修复证据。

## 9. 文件

- `include/brickdesk/device_feedback.h`
- `src/device/device_feedback_internal.h`
- `src/device/feedback.c`
- `src/device/feedback_linux.c`
- `tools/bd-feedback.c`
- `tools/bd-feedback-play.c` / `tools/bd-feedback-play.h`
- `tests/device_feedback_test.c`
- `tests/bd_feedback_cli_process_test.c`
- `tests/fake_evdev_preload.c`
- `docs/device-feedback.md`（本文）
