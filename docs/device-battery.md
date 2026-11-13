# 电池、充电状态与低电量策略（M4-Battery）

> **状态**：宿主构建、单元测试、ASan/UBSan 和 aarch64 交叉编译已通过；
> **Brick Pro 真机行为全部未验证**。设备到货后按第 10 节逐项记录，未记录前
> 不要把本文或代码注释改成“已验证”。
>
> 本文覆盖 `include/brickdesk/device_battery.h`、`src/device/battery*`、
> `tests/device_battery_test.c`、`tests/fixtures/battery/` 和
> `src/device/battery_probe_main.c` 中的 `bd-battery`。不修改
> `src/app/home`、`src/app/settings`、`src/config` 或现有 `bd_power` 状态机。

## 1. 范围与边界

实现：

- 只读探测 Linux `power_supply` class，发现电池、USB、Mains 等多个供电节点。
- 读取标准语义字段：容量百分比、充电状态、外部电源、电池存在性、电压、
  电流、温度、剩余/充满时间、健康状态。
- 缺失、不可读、格式错误、越界或电池已拔出的字段一律标为 unknown，不做
  电压→容量估算，不把缺失节点或 `present=0` 当成 0%。
- 可注入 `sysfs_root` 的假设备树，完整覆盖发现、选择、坏值、热插拔、
  节点消失/类型变化等读取竞态。
- 与 UI 无关的低电量策略：提醒/临界阈值、迟滞、去抖、防重复、插电抑制与
  恢复、unknown 保护、建议采样 deadline、事件和建议。
- 只读诊断 CLI `bd-battery`，已加入 target 产物和 GLIBC 检查。

明确不做：

- 不写任何 sysfs 文件、PMIC 寄存器、I2C 总线、LED 或背光。
- 不调用 `bd_power_*`，不强制关机/重启/休眠，不驱逐应用，不杀进程。
- 不实现电源键、自动休眠定时器或长按判定；这些属于 `m4/power` 和 shell。
- 不把策略字段全部暴露成首页选项；基本/高级配置的边界见第 6 节。
- 不与 home 分支已有的简易电池读数抢接线；本分支只提供 API，统一接线由
  协调线程完成并删除旧读取。

## 2. 能力矩阵

| 能力 | 状态 | 条件/语义 |
|---|---|---|
| `power_supply` 发现 | 已实现 | `<root>/sys/class/power_supply/*`；按名称排序 |
| 电池选择 | 已实现 | 显式名称；否则优先 present 的 `axp2202-battery`，再按名称序 |
| `type` | 已实现 | Battery/USB/USB_DCP/USB_CDP/USB_ACA/USB_C/USB_PD/USB_PD_DRP/Mains/UPS/Other |
| `present` | 已实现 | 电池存在性；`present=0` 时容量/状态/电压/电流/温度/时间/健康全部 unknown |
| `online` | 已实现 | 外部供电节点；只按 `online` 聚合，不使用 `present` 代替 |
| `capacity` | 已实现 | 0..100 整数百分比；越界/非数字 = unknown |
| `status` | 已实现 | Charging / Discharging / Not charging / Full / Unknown |
| 外部电源 | 已实现 | 任一 USB/Mains `online=1`；所有外部节点都可读且为 0 才是 known false |
| `voltage_now` | 已实现 | µV；负值 = unknown |
| `current_now` | 已实现 | µA，带符号；缺失 = unknown（AXP2202 BSP 未暴露该属性） |
| `temp` | 已实现 | 十分之一 °C（305 = 30.5 °C） |
| `time_to_empty_now` / `time_to_full_now` | 已实现 | 秒；负值 = unknown |
| `health` | 已实现 | Good/Overheat/Dead/Over voltage/Unspecified failure/Cold/Over current/Unknown/Other |
| 读取错误计数 | 已实现 | `read_errors` 只统计“文件存在但坏值/读失败”；可选字段缺失不算错误 |
| 节点消失/类型变化 | 已实现 | `BD_BATTERY_ERR_STALE`，清空电池测量值 |
| 无电池节点 | 已实现 | `BD_BATTERY_ERR_UNSUPPORTED`，容量 unknown，绝不返回 0% |
| 低电量策略 | 已实现，宿主测试 | 见第 5 节 |
| 策略执行关机 | **明确不做** | 只输出 `PREPARE_SHUTDOWN` 建议，消费者自己走安全流程 |
| PMIC/LED/休眠/UI | 不在范围 | 由 power/AV/UI 模块负责 |

## 3. 公开 API 与单位

头文件：`include/brickdesk/device_battery.h`。

### 3.1 返回值和错误

```c
typedef enum bd_battery_result {
  BD_BATTERY_OK = 0,
  BD_BATTERY_ERR_ARGUMENT,
  BD_BATTERY_ERR_UNSUPPORTED,
  BD_BATTERY_ERR_STALE,
  BD_BATTERY_ERR_IO
} bd_battery_result;
```

- `OK` 表示快照成功；某些字段仍可 unknown，用对应的 `*_known` 判断。
- `UNSUPPORTED`：没有 `power_supply` class，或没有 Battery 节点。
- `STALE`：打开时选中的电池节点消失，或 type 不再是 Battery。
- `IO`：无法打开/枚举 class 目录，或路径溢出。
- `ARGUMENT`：空指针、非法显式名称、配置/路径非法。

`bd_battery_error` 提供 `code` 和可读 `message`；`bd_battery_result_name()`
用于日志。

### 3.2 读取

```c
bd_battery_result bd_battery_open(bd_battery *battery,
                                  const bd_battery_config *config,
                                  bd_battery_error *error);
bd_battery_result bd_battery_read(const bd_battery *battery,
                                  bd_battery_status *status,
                                  bd_battery_error *error);
```

配置：

| 字段 | 默认 | 说明 |
|---|---|---|
| `sysfs_root` | `NULL`/`""` = `/` | 根前缀；假树形如 `/tmp/root` 会读 `/tmp/root/sys/class/power_supply` |
| `battery_name` | `NULL`/`""` = 自动 | 精确名称；显式名称必须是 `type=Battery` |

`bd_battery_open()` 只发现并记录选择，不写任何文件。`bd_battery_read()`
每次重新枚举供电节点，因此 USB/Mains 热插拔可见；它不会在已有选择后悄悄
切换到另一个电池。若打开时没有电池，之后的 `read` 可以重新发现新出现的
电池，但仍不会把 USB/Mains 当电池。

### 3.3 单位

| 字段 | 单位 | 说明 |
|---|---|---|
| `capacity_percent` | % | 0..100；只有 `capacity_known` 为真才可使用 |
| `voltage_uv` | µV | Linux power_supply ABI；例如 3.95 V = 3950000 |
| `current_ua` | µA | 带符号；AXP BSP 约定正=充电、负=放电；驱动不暴露时 unknown |
| `temp_deci_c` | 0.1 °C | 305 = 30.5 °C |
| `time_to_empty_s` / `time_to_full_s` | 秒 | 不是分钟 |
| `present` / `online` / `external_power` | 布尔 | 用 `*_known` 区分 unknown 与 false |

**不要把 mV 当 µV，也不要把 305 当 305 °C。** 真机必须先 `cat` 原始节点
确认单位，见第 10 节。

### 3.4 `bd_battery_status`

关键字段：

```c
bool battery_available;
char battery_name[BD_BATTERY_NAME_MAX];
bool present_known, present;
bool capacity_known; int capacity_percent;
bool status_known; bd_battery_charge_status status;
bool external_power_known, external_power;
bool voltage_known; int64_t voltage_uv;
bool current_known; int64_t current_ua;
bool temp_known; int32_t temp_deci_c;
bool time_to_empty_known; int64_t time_to_empty_s;
bool time_to_full_known; int64_t time_to_full_s;
bool health_known; bd_battery_health health;
unsigned read_errors;
unsigned supply_count;
bool supplies_truncated;
bd_battery_supply supplies[BD_BATTERY_MAX_SUPPLIES];
```

`supplies[]` 保存每个节点的名称、原始 `type` 文本、分类、selected、
present/online/status，供诊断和“多个供电节点”排查。默认最多 16 个；
超过时 `supplies_truncated=true`。即使被截断，选中电池仍会被直接读取，
不会误报 STALE。

`clear_measurements()` 在 STALE 时清空电池测量值，但保留供电节点列表和
外部电源聚合；外部电源是独立读取结果，不依赖电池节点。

## 4. 探测规则

### 4.1 枚举与选择

1. 打开 `<sysfs_root>/sys/class/power_supply`；不存在 = `UNSUPPORTED`，
   权限/IO 错误 = `IO`。
2. 读取每个目录的 `type`；只有 `type=Battery` 的节点才是电池候选。
   不按名称模糊匹配，不把 `USB`/`Mains` 当电池。
3. 显式 `battery_name`：必须存在且 `type=Battery`；否则 `UNSUPPORTED`。
4. 自动选择：
   - 优先 `present` 为真的候选；
   - 没有 present 真时，优先 `present` unknown 的候选；
   - 全部 known absent 时仍选一个用于诊断（读取时 `present=no`）；
   - 在候选集合内优先精确名称 `axp2202-battery`，否则按名称排序取第一个。
5. 每次 `read` 都重新枚举，所以 USB/Mains 插入/拔出可见。已选电池消失
   返回 `STALE`；不会自动切换到另一个电池。

### 4.2 外部电源聚合

- 只统计 `type=USB`（含 USB_DCP/CDP/ACA/C/PD/PD_DRP）和 `type=Mains`。
- **聚合扫描全部枚举节点，包括 `supplies[16]` 之外的节点。** 公开诊断表仍然
  只保留前 16 项并用 `supplies_truncated` 标记，但截断不能隐藏在线电源，也
  不能把未知变成 known false。
- 任一 `online=1` → `external_power_known=true`、`external_power=true`。
- 所有外部节点的 `online` 都可读且为 0 → known false。
- 任一外部节点的 `online` 缺失/坏值，且没有其他节点为 1 → known false 是
  不安全的，因此报 unknown。
- 表外节点无法读取/分类时按“可能外部且 unknown”处理，绝不输出 known false。
- 不用 `present` 代替 `online`；不用电池 `status` 反推外部电源字段
  （策略层有自己的受限推断，见 5.4）。

### 4.3 unknown 规则

| 情况 | 结果 |
|---|---|
| class/电池节点不存在 | `UNSUPPORTED`，容量 unknown，不是 0% |
| 电池 `present=0` | `present_known=true, present=false`；容量/状态/电压/电流/温度/时间/健康 unknown |
| 电池 `present` 缺失 | `present_known=false`；其余字段仍尝试读取 |
| 电池 `present` 坏值 | `present_known=false`，`read_errors++`；其余字段仍尝试读取（不假定拔出） |
| 可选属性缺失（如 `current_now`） | 对应 `*_known=false`；不算 read error |
| 数字格式错误/溢出 | 对应 `*_known=false`，`read_errors++` |
| `capacity` 不在 0..100 | unknown，`read_errors++` |
| `voltage_now` 为负 | unknown，`read_errors++` |
| `time_to_*` 为负 | unknown，`read_errors++` |
| 选中节点读取中消失 | `STALE`，清空电池测量值 |
| 选中节点 type 变为非 Battery | `STALE` |
| 读取中某属性 ENOENT | 该属性 unknown；结束时若目录仍在则返回 `OK` |

## 5. 低电量策略

策略完全独立于 UI、home、settings 和 `bd_power`。它只消费
`bd_battery_status` 和调用方提供的单调时间 `now_ms`，输出事件、建议和下一次
采样 deadline。

### 5.1 等级与阈值

| 等级 | 进入条件（默认，需连续确认） | 退出条件（默认，需连续确认） |
|---|---|---|
| `NORMAL` | 容量 > 20%，或从 unknown 首次得到正常读数 | — |
| `REMINDER` | 容量 ≤ 20% | 容量 ≥ 23%（reminder + 3 点迟滞） |
| `CRITICAL` | 容量 ≤ 5% | 容量 ≥ 7% 先回到 REMINDER；≥23% 回 NORMAL |
| `UNKNOWN` | 容量缺失/不可用/电池拔出 | 下一次有效容量；正常值立即生效，低值仍需确认 |

- 从 `NORMAL` 直接跳到 `CRITICAL` 是允许的，但必须连续确认，避免单个坏值。
- 从 `CRITICAL` 到 `REMINDER` 需要容量达到 `critical + critical_hysteresis`
  （默认 5+2=7），否则保持临界。
- 从 `REMINDER` 到 `NORMAL` 需要容量达到
  `reminder + reminder_hysteresis`（默认 20+3=23）。
- 迟滞区间内的抖动不会产生新事件。
- `UNKNOWN` 不触发 LOW/CRITICAL/RECOVERED/PREPARE_SHUTDOWN；已有等级重置为
  UNKNOWN，并发出一次 `UNKNOWN` 事件。

### 5.2 去抖与防重复

- `confirm_samples`（默认 2）要求连续样本进入同一候选等级；中间回到原等级
  会清零候选计数。
- 进入 REMINDER/CRITICAL 时各发一次事件；之后按 `reminder_repeat_ms`
  （默认 10 分钟）/`critical_repeat_ms`（默认 2 分钟）重复。设为 0 表示
  只发首次，不重复。
- `CRITICAL → REMINDER` 发出 LOW（提醒仍然存在，只是降级）；
  `REMINDER/CRITICAL → NORMAL` 发出 RECOVERED。
- 时间回退（`now` 小于上次事件时间）不会被视为“已到重复时间”。

### 5.3 采样 deadline

`bd_battery_policy_init()` 返回 `next_sample_ms = now_ms`，要求立即采一次。
每次 `update()` 返回绝对单调 deadline 和相对间隔；调用方应在 deadline 或
外部 uevent/电源事件时调用 `update`，不要忙轮询。

| 状态 | 默认间隔 |
|---|---|
| NORMAL | 60 s |
| REMINDER | 30 s |
| CRITICAL | 15 s |
| 插电/抑制中 | 60 s |
| 候选等级待确认 | 5 s（取更小者） |
| 容量 unknown | 5 s 起，连续 unknown 翻倍，封顶 120 s |

稳定时最多每 60 s 一次；unknown 退避避免在无电池设备上每秒轮询。deadline
用饱和加法，`now` 接近 `UINT64_MAX` 时返回 `UINT64_MAX`。

### 5.4 插电抑制与恢复

策略的 `plugged_in` 判定优先级：

1. `external_power_known=true`：直接用 `external_power`；
2. 外部电源 unknown，但电池 `status=Charging/Full`：视为 plugged；
3. 外部电源 unknown，但电池 `status=Discharging`：视为 unplugged；
4. `Not charging` 或 `status` unknown：plugged 仍 unknown。

说明：

- 2/3 是**策略层**的受限推断，只用于抑制/恢复提醒；不会写回
  `external_power_known`，也不会让 `PREPARE_SHUTDOWN` 通过安全门。
- 插电期间保留内部等级跟踪，但不发 LOW/CRITICAL，不建议关机；
  发出一次 `PLUGGED` 事件。
- 拔电时发出 `UNPLUGGED`；如果等级仍是 REMINDER/CRITICAL，立即恢复对应
  提醒（不需要重新等一个完整重复周期）。
- 如果在插电期间容量回到正常，仍会发 `RECOVERED`；之后拔电不会误报低电量。
- `suppress_while_plugged=false` 时不做抑制，但临界建议仍不会在外部电源
  为 1 时变成关机。

### 5.5 事件

| 事件 | 含义 |
|---|---|
| `BD_BATTERY_EVENT_LOW` | 进入/恢复/重复提醒等级 |
| `BD_BATTERY_EVENT_CRITICAL` | 进入/恢复/重复临界等级 |
| `BD_BATTERY_EVENT_RECOVERED` | 从 REMINDER/CRITICAL 回到 NORMAL |
| `BD_BATTERY_EVENT_PLUGGED` | 外部电源/充电状态从未插电变为插电 |
| `BD_BATTERY_EVENT_UNPLUGGED` | 从插电变为已知未插电 |
| `BD_BATTERY_EVENT_UNKNOWN` | 有效容量变为 unknown（只发一次转换） |

事件是 bitmask；一次 update 可能同时有 `UNPLUGGED | LOW`。

### 5.6 建议与安全门

| 建议 | 条件 | 消费者动作 |
|---|---|---|
| `NONE` | 正常、unknown、抑制中 | 无 |
| `SHOW_REMINDER` | REMINDER 且未被抑制 | 展示提醒，不关机 |
| `SHOW_CRITICAL` | CRITICAL 且未被抑制 | 展示强提醒；是否关机由消费者决定 |
| `PREPARE_SHUTDOWN` | CRITICAL + `recommend_shutdown` + 电池 known present + 外部电源 known false | 消费者走自己的用户确认/保存/`bd_power_poweroff()` 流程 |

硬性安全规则：

- 容量 unknown、`battery_available=false`、`present=0`、`present` unknown
  → 绝不产生 `PREPARE_SHUTDOWN`。
- 外部电源 unknown 或 known 为 1 → 绝不产生 `PREPARE_SHUTDOWN`。
- 策略本身不调用 `bd_power_*`、不写 PMIC、不杀进程、不驱逐应用。
- 现有 `[power] shutdown_on_low_battery` 应由消费者读取并作为
  `recommend_shutdown` 的门；策略不直接读取配置文件。

### 5.7 配置：基本与高级

`bd_battery_policy_config_default()` 给出默认值。基本字段可以将来出现在
设置页；高级字段只应出现在配置文件/开发者配置中，不把每个探测字段都变成
首页选项。

**基本**

| 字段 | 默认 | 范围/说明 |
|---|---|---|
| `enabled` | true | 策略总开关 |
| `reminder_percent` | 20 | 1..100 |
| `critical_percent` | 5 | 0..reminder-1 |
| `suppress_while_plugged` | true | 插电抑制/拔电恢复 |
| `recommend_shutdown` | true | 仅允许建议 `PREPARE_SHUTDOWN`，不执行 |

**高级**

| 字段 | 默认 | 说明 |
|---|---|---|
| `reminder_hysteresis_percent` | 3 | 退出提醒需要的高于阈值的点数 |
| `critical_hysteresis_percent` | 2 | 退出临界需要的高于阈值的点数 |
| `confirm_samples` | 2 | 连续确认样本数，1..100 |
| `reminder_repeat_ms` | 600000 | 10 分钟；0 = 不重复 |
| `critical_repeat_ms` | 120000 | 2 分钟；0 = 不重复 |
| `sample_normal_ms` | 60000 | 稳定正常采样 |
| `sample_reminder_ms` | 30000 | 提醒等级采样 |
| `sample_critical_ms` | 15000 | 临界等级采样 |
| `sample_charging_ms` | 60000 | 插电/抑制采样 |
| `sample_confirm_ms` | 5000 | 候选确认采样 |
| `unknown_initial_ms` | 5000 | unknown 首次退避 |
| `unknown_max_ms` | 120000 | unknown 退避上限 |

验证规则：reminder 1..100；critical 0..reminder-1；阈值+迟滞 ≤100；
confirm 1..100；采样/unknown 间隔 1 s..24 h；unknown_initial ≤ unknown_max；
重复间隔 0 或 1 s..24 h。`enabled=false` 时跳过其余验证。

### 5.8 与现有 `[power]` 配置的关系

现有 `config/brickdesk.ini` 已有：

```ini
[power]
low_battery_percent = 5
shutdown_on_low_battery = true
```

建议的统一接线（由协调线程完成，本分支不改 schema/settings）：

- `low_battery_percent` → `critical_percent`（语义是“低电量关机阈值”，
  不是提醒阈值）。
- `shutdown_on_low_battery` → 消费者的关机动作门；同时映射到
  `policy.recommend_shutdown`，但策略仍只给建议。
- 将来新增 `battery_reminder_percent` 及高级键到 `[power]`，只放在配置文件/
  高级设置，不放在首页。
- 首页只展示 `capacity`、`status`、`external_power` 三个字段；不展示
  voltage/current/temp 等探测细节。

## 6. 事件循环接线形状

下面是未来 shell 的接线形状，不是当前已实现的 home/settings：

```c
#include "brickdesk/device_battery.h"

bd_battery battery;
bd_battery_config bcfg = {.sysfs_root = NULL, .battery_name = NULL};
bd_battery_error err;
if (bd_battery_open(&battery, &bcfg, &err) != BD_BATTERY_OK) {
  /* Show unknown, never 0%.  Retry open later if the battery appears. */
}

bd_battery_policy_config pcfg;
bd_battery_policy_config_default(&pcfg);
pcfg.critical_percent = power_config.low_battery_percent;
pcfg.recommend_shutdown = power_config.shutdown_on_low_battery;

bd_battery_policy policy;
bd_battery_policy_output pout;
bd_battery_policy_init(&policy, &pcfg, now_ms(), &pout, &err);

/* At pout.next_sample_ms, a power_supply uevent, or an activity event: */
bd_battery_status status;
bd_battery_result rr = bd_battery_read(&battery, &status, &err);
bd_battery_policy_update(&policy, &status, now_ms(), &pout);

if (pout.events & BD_BATTERY_EVENT_LOW) { /* show reminder */ }
if (pout.events & BD_BATTERY_EVENT_CRITICAL) { /* show critical */ }
if (pout.events & BD_BATTERY_EVENT_PLUGGED) { /* hide low UI */ }
if (pout.events & BD_BATTERY_EVENT_UNPLUGGED) { /* re-evaluate low UI */ }
if (pout.recommendation == BD_BATTERY_RECOMMEND_PREPARE_SHUTDOWN) {
  /* Consumer policy: save, confirm, then bd_power_poweroff(...). */
}
```

- 读取失败时仍把 `status` 传给策略：`battery_available=false` / 容量 unknown
  会让策略进入 UNKNOWN，不会关机。
- 每个 home/settings 消费者应先用 `capacity_known` 判断，未知时显示占位符，
  不显示 0%。
- 同一 `bd_battery` 可在单线程内复用；多线程读取需要调用方串行化。

## 7. 证据、版本与许可

> 许可提醒：NextUI 使用 PolyForm Noncommercial 1.0.0；MinUI 没有正式许可证；
> Knulli 为 GPL 系（GitHub NOASSERTION）；CrossMix 为 GPL-3.0。本文和实现
> 只使用接口/行为事实，不复制上述项目的代码、脚本、寄存器序列或资源。
> brickdesk 保持本仓库原有许可证。

### 7.1 版本固定

| 来源 | 版本/提交 | 用途 |
|---|---|---|
| TrimUI Brick Pro 固件 | `v1.1.1-20260717`，2026-07-18 发布 | 设备路径、型号、AXP2202 节点 |
| brickdesk 调研 | `docs/research/device-features.md`，2026-11-12 | 固件/源码事实与 E1–E52 |
| Linux 稳定树 | `v4.9.191` | power_supply ABI、单位、状态/类型文本 |
| NextUI | `LoveRetro/NextUI` `b6ec1343c0a2636ddaf2502fde52cff237c67377` | 容量读取、充电判定、batmon 行为 |
| Knulli | `knulli-cfw/knulli-linux` `57afa662009641d181f57241bcc480119b131530` | 多节点选择、DTS pmu 参数 |
| CrossMix | `cizia64/CrossMix-OS` `206d612fdef83972ed54a17de955125d0c0b3528` | 只作对照；没有独立电池 API |
| MinUI | `shauninman/MinUI` `dbf89435a5179788418ff3573042cf9c7e75d9e0` | 只确认 Brick Pro 支持状态；无正式许可证 |
| Allwinner AXP2202 BSP | `kiramint/Avaota-linux` `7c2a33957cb82e522885992ec26331d1c1c6db28`（2025-02-21） | 公共 BSP 驱动的属性/单位/状态映射 |

固件发布：
<https://github.com/trimui/firmware_brickpro/releases/tag/v1.1.1-20260717>。
调研文档记录 `rootfs:/etc/model` 为 `TG4040`、内核为厂商 `4.9.191`、PMIC 为
AXP2202；本文没有重新分发固件或厂商文件。

### 7.2 Linux power_supply ABI

- `Documentation/power/power_supply_class.txt`（v4.9.191）：
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/Documentation/power/power_supply_class.txt?h=v4.9.191>
  - 所有电压/电流/电荷/能量/时间/温度单位分别是 µV、µA、µAh、µWh、秒、
    十分之一 °C。
  - `CAPACITY` 是百分比；`STATUS` 是 Charging/Discharging/Not charging/Full/
    Unknown；`PRESENT`/`ONLINE` 是标准存在/在线属性。
  - 驱动可以省略任意属性；缺失不是 0。
- `include/linux/power_supply.h`（v4.9）：
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/include/linux/power_supply.h?h=v4.9.191>
  - `POWER_SUPPLY_STATUS_*`、`POWER_SUPPLY_PROP_CAPACITY/PRESENT/ONLINE/
    VOLTAGE_NOW/CURRENT_NOW/TEMP/TIME_TO_*` 的枚举。
- `drivers/power/supply/power_supply_sysfs.c`（v4.9）：
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/power/supply/power_supply_sysfs.c?h=v4.9.191>
  - type 文本：`Battery`、`USB`、`USB_DCP`、`USB_CDP`、`USB_ACA`、`USB_C`、
    `USB_PD`、`USB_PD_DRP`、`Mains`、`UPS`、`Unknown`。
  - status 文本：`Unknown`、`Charging`、`Discharging`、`Not charging`、`Full`。
  - health 文本：`Unknown`、`Good`、`Overheat`、`Dead`、`Over voltage`、
    `Unspecified failure`、`Cold`、`Watchdog timer expire`、
    `Safety timer expire`；较新内核还会出现 `Over current` 等。

实现按这些文本精确/大小写不敏感匹配；未知文本映射为 `OTHER`/unknown 并计入
`read_errors`，不做猜测。

### 7.3 官方 Brick Pro 1.1.1 固件

调研文档（E41、第 3.3 节）记录的节点：

- `/sys/class/power_supply/axp2202-battery/{capacity,status,temp,time_to_full_now}`
- `/sys/class/power_supply/axp2202-usb/online`

这些是固件事实，不是本文实测；真机仍需确认完整节点列表和单位。调研还记录
电池标称 5000 mAh，但 DTB `pmu_battery_cap=3000`（0xBB8），所以不能用容量
做剩余时间承诺。

### 7.4 NextUI 行为

固定提交 `b6ec1343`：

- [`workspace/tg5040/platform/platform.c` 的 `PLAT_getBatteryStatusFine()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/tg5040/platform/platform.c#L180-L190)：
  - `is_charging = (axp2202-usb/online == 1) && (axp2202-battery/time_to_full_now > 0)`；
  - `charge = axp2202-battery/capacity` 原始百分比。
- 同文件 `PLAT_getBatteryStatus()` 把容量量化到 10/20/40/60/80/100，用于
  “少担心电池、多玩游戏”的粗略 UI。本实现**不复制**该量化；API 返回原始
  0..100。
- [`workspace/all/batmon/batmon.c`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/batmon/batmon.c)：
  1 秒循环、15 秒日志节流、充电开始/停止检测；本实现不复制其日志数据库，
  策略默认稳定 60 秒采样。
- 结论：NextUI 把“USB online + time_to_full>0”作为充电判定；本实现分开
  暴露 external_power 和 status，不把该启发式写死为唯一真相。

### 7.5 Knulli 行为

固定提交 `57afa662`：

- [`board/allwinner/a133/trimui-brick-pro/patches/batocera-emulationstation/001_fix_battery_path.patch`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/allwinner/a133/trimui-brick-pro/patches/batocera-emulationstation/001_fix_battery_path.patch)：
  在 `/sys/class/power_supply` 中优先选择路径含 `axp2202` 的节点，其次含
  `bat` 的节点。这证明 Brick Pro 存在多个供电节点，不能默认取第一个。
- [`package/boot/uboot-a133/trimui-brick-pro/boot_package/trimui-brick-pro.dts`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/package/boot/uboot-a133/trimui-brick-pro/boot_package/trimui-brick-pro.dts)：
  `pmu@34` 为 `x-powers,axp2202`；`bat-power-supply` 的
  `pmu_battery_cap = <0xbb8>`（3000），`pmu_battery_warning_level1 = <0x05>`、
  `warning_level2 = <0x00>`。这些是 PMIC 硬件告警阈值，本模块不写它们，
  真机可用来校准策略阈值。
- Knulli 的 `99-powerled` 只在 resume 时恢复电源 LED，不提供低电量策略；
  本模块不复制。

### 7.6 Allwinner AXP2202 BSP 驱动

公共 BSP `kiramint/Avaota-linux` 提交 `7c2a339`：

- `bsp/drivers/power/supply/axp2202_battery.c`：
  - `power_supply_desc.name = "axp2202-battery"`，`type = BATTERY`；
  - 属性包括 `PRESENT`、`VOLTAGE_NOW`、`STATUS`、`CAPACITY`、`TEMP`、
    `TIME_TO_EMPTY_NOW`、`TIME_TO_FULL_NOW`、`HEALTH`、`CHARGE_COUNTER`、
    `CHARGE_FULL`、`ENERGY_FULL_DESIGN` 等；
  - **没有 `CURRENT_NOW`**；因此 Brick Pro 上 `current_now` 很可能不存在，
    本实现把它当 unknown，不估算；
  - `axp2202_get_soc()` 返回百分比，并 clamp 到 SOC 上下限；
  - `axp2202_get_bat_status()` 把 TRI/NCHG 映射为 Discharging，
    PRE/CC/CV 映射为 Charging，DONE 映射为 Full，其余 Unknown；
  - `axp2202_get_bat_present()` 从 `COMM_STAT0` 的电池位返回 0/1；
  - `axp_vts_to_temp()` 返回十分之一 °C（例如 700 = 70.0 °C）；
  - `time_to_empty/full` 由分钟 ×60 得到秒。
- `bsp/drivers/power/supply/axp2202_usb_power.c`：
  - `power_supply_desc.name = "axp2202-usb"`，`type = USB`；
  - `ONLINE` 由 VBUS 在线状态返回；`PRESENT` 是 VBUS 状态，不等同 online。

这是 Allwinner 公共 BSP，不是 TG4040 厂商 4.9.191 内核的逐字副本；真机
必须确认实际属性集、单位、状态映射和 `present` 语义。

### 7.7 CrossMix / MinUI

- CrossMix `206d612` 沿用原厂 keymon/UI，没有发现独立的低电量策略 API；
  本文只把它的 `lcd_off`/`lcd_on` 当作“关屏不是休眠”的对照，不复制。
- MinUI `dbf8943` 没有正式许可证；只用于确认 Brick Pro 支持状态，不作为
  实现来源。

## 8. 构建、测试与交叉编译

### 8.1 宿主

```sh
cmake -S . -B build-host -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBRICKDESK_WERROR=ON
cmake --build build-host --parallel
ctest --test-dir build-host -R device_battery --output-on-failure
```

本分支实际结果：

```text
1/2 Test #6: device_battery ...................   Passed
2/2 Test #7: device_battery_cli ...............   Passed
100% tests passed, 0 tests failed out of 2
```

覆盖：

- full fixture：4 个供电节点、selected 标记、present/capacity/status/
  external/voltage/current/temp/time/health、单位、read_errors=0。
- limited fixture：只有 type+capacity；所有可选字段 unknown 且不算错误。
- 无电池 fixture：UNSUPPORTED，容量 unknown，不是 0%；usb0 online=0 仍可读。
- 晚到电池：open 时无电池，之后 read 自动发现；再移除回到 UNSUPPORTED。
- 多电池选择：present 优先、`axp2202-battery` 优先、显式选择、
  全部 absent 仍选一个用于诊断、USB 显式名称被拒绝。
- present=false：容量/状态/电压/电流/温度/健康全部 unknown；
  外部电源仍可读。
- 坏值：present=2、capacity 非数字/越界/空、status 未知文本、负电压、
  坏电流/温度/时间、未知 health；字段 unknown、read_errors 增长。
- 容量边界：0、100、101、-1、空、带空格 73。
- 外部电源：单 USB、USB+Mains、任一 online=1、known-off + unknown、
  online 坏值、present 不代替 online、无外部节点。
- STALE：电池节点删除、type 变为 USB、class 目录删除。
- 截断：20 个 USB + 1 个电池，selected 在表外仍可读，`supplies_truncated`；
  表外唯一在线节点、表外 unknown online、全部离线三种公开读 API 回归，
  并把每种结果喂策略两次：在线→抑制且不建议关机，unknown→SHOW_CRITICAL，
  全部离线→才允许 PREPARE_SHUTDOWN。
- 策略：默认/非法配置、阈值边界、迟滞、连续确认、抖动、插电抑制、拔电恢复、
  Charging/Full/Discharging 推断、unknown 事件与退避、关机安全门、
  deadline、时间回退、禁用策略。

### 8.2 ASan/UBSan

```sh
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude -Isrc/device \
  -DBRICKDESK_BATTERY_FIXTURE_DIR='"tests/fixtures/battery"' \
  tests/device_battery_test.c src/device/battery.c src/device/battery_linux.c \
  -o /tmp/bd_battery_asan
ASAN_OPTIONS=detect_leaks=1 /tmp/bd_battery_asan
```

结果：`device_battery_test: OK`，无 ASan/UBSan 报告。

### 8.3 aarch64 交叉编译

本地使用官方 TrimUI 20231018 工具链（Linaro GCC 7.5.0 2019.12）编译电池库、
`bd-battery` 和测试；不依赖 SDL/SDK：

```sh
CC=/tmp/trimui-toolchain/bin/aarch64-linux-gnu-gcc
$CC -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc/device \
  -c src/device/battery.c -o /tmp/bd-battery-cross/battery.o
$CC ... src/device/battery_linux.c ...
$CC ... src/device/battery_probe_main.c ...
$CC ... -DBRICKDESK_BATTERY_FIXTURE_DIR='"tests/fixtures/battery"' \
  tests/device_battery_test.c ...
file /tmp/bd-battery-cross/bd-battery
TRIMUI_TOOLCHAIN=/tmp/trimui-toolchain ci/check-glibc.sh --max 2.23 \
  /tmp/bd-battery-cross/bd-battery \
  /tmp/bd-battery-cross/bd_device_battery_test
```

实际结果：

```text
bd-battery:             ELF 64-bit LSB executable, ARM aarch64, ...
bd_device_battery_test: ELF 64-bit LSB executable, ARM aarch64, ...
bd-battery: max GLIBC 2.17 (limit 2.23) OK
bd-battery: NEEDED libc.so.6
bd_device_battery_test: max GLIBC 2.17 (limit 2.23) OK
bd_device_battery_test: NEEDED libc.so.6
```

`scripts/build-target.sh`、`.github/workflows/target.yml` 和
`scripts/package-target.sh` 已加入 `bd-battery` 的构建、`dist/bin`、发布包
`payload/bin` 和 GLIBC 检查。完整 target CMake 构建（含 SDL/SDK）由 target
workflow 验证；本 orb 内没有 SDK，只做了独立交叉编译。

### 8.4 全量宿主测试

同一配置的全量 CTest（2026-11-13，main `f669b08`）为 38/38 通过。新增的
`device_battery` 和 `device_battery_cli` 均在其中；本模块没有修改
home/settings/config，全量截图和场景测试仍通过。

## 9. 诊断 CLI `bd-battery`

只读，默认探测真实 `/sys`；写操作不存在。构建设置见第 8 节，target 产物在
`build-target/bd-battery`，发布包在 `payload/bin/bd-battery`。

```sh
# 真机只读探测
/mnt/SDCARD/brickdesk/bin/bd-battery

# 显式选择电池
/mnt/SDCARD/brickdesk/bin/bd-battery --battery axp2202-battery

# 宿主 fixture
./build-host/bd-battery --root tests/fixtures/battery/full
```

宿主 full fixture 输出（节选）：

```text
probe.read_only=yes
probe.open=ok
battery.available=yes
battery.name=axp2202-battery
battery.present=yes
battery.capacity=73
battery.status=discharging
battery.external_power=yes
battery.voltage_uv=3950000
battery.current_ua=-420000
battery.temp_deci_c=305
battery.time_to_empty_s=7200
battery.time_to_full_s=unknown
battery.health=good
battery.read_errors=0
supply.count=4
...
policy.level=normal
policy.suppressed=yes
policy.recommendation=none
policy.next_sample_ms=60000
policy.next_sample_after_ms=60000
```

无电池宿主输出会明确显示 `battery.available=no`、`battery.capacity=unknown`、
`probe.read=unsupported`，退出码 1；不会打印 0%。

## 10. 真机验收矩阵

以下全部是**待执行**项。每项记录：日期、固件 `/etc/version`、`/etc/model`、
内核 `uname -a`、操作者、命令输出。未记录前不要把本文状态改成“已验证”。

### 10.1 节点与单位

```sh
adb shell 'cat /etc/model; cat /etc/version; uname -a'
adb shell 'ls -l /sys/class/power_supply'
adb shell 'for d in /sys/class/power_supply/*; do
  echo "== $d";
  for a in type present online status capacity voltage_now current_now temp \
           time_to_empty_now time_to_full_now health charge_now charge_full \
           charge_full_design; do
    [ -f "$d/$a" ] && printf "%s=" "$a" && cat "$d/$a";
  done;
done'
adb shell '/mnt/SDCARD/brickdesk/bin/bd-battery'
```

- [ ] 实际节点名是否为 `axp2202-battery`、`axp2202-usb`，是否还有
      `axp2202-ac`/其他节点；`bd-battery` 的 `supply.*` 是否与 `ls` 一致。
- [ ] `type` 是否为 `Battery`/`USB`/`Mains`；如果厂商用了别的文本，记录并
      确认我们的分类是否仍为 unknown/other（不会误当电池）。
- [ ] `capacity` 是否为 0..100 整数；`0` 和 `100` 是否真实出现。
- [ ] `voltage_now` 是否是 µV（约 3,000,000..4,400,000），不是 mV（约
      3000..4400）。如果是 mV，说明厂商违反 ABI 或节点名不同，必须停用该
      字段并修文档。
- [ ] `current_now` 是否存在；存在时符号方向（充电正/放电负）是否与 ABI
      一致；不存在时 `bd-battery` 必须显示 unknown，不估算。
- [ ] `temp` 是否是十分之一 °C（约 250..450，即 25..45 °C），不是 °C
      整数或毫度。
- [ ] `time_to_empty_now`/`time_to_full_now` 是否是秒；充电时
      `time_to_full_now` 是否 >0，满电/放电时是 0、缺失还是其他值。
- [ ] `present` 在正常电池、拔出电池（如可拔）时的值；`present=0` 时
      `capacity` 是否仍返回旧值/0，`bd-battery` 必须仍报 unknown。
- [ ] `status` 在充电、放电、满电、插电脑 USB（可能 Not charging）时的
      文本；是否与 `online` 一致。
- [ ] 多节点下 `external_power` 聚合是否符合实际：只插 USB、只插 DC、
      同时插、都不插。

### 10.2 容量与 5000 mAh 冲突

- [ ] 满充到 100%，记录 `capacity`、`voltage_now`、`temp`、`charge_full`/
      `charge_full_design`（若存在）。
- [ ] 持续放电到设备自动关机，每 60 秒记录 `capacity`、`voltage_now`、
      `current_now`（若存在）；记录关机前最后几个百分比。
- [ ] 对比标称 5000 mAh 与 DTB `pmu_battery_cap=3000`；不要用容量承诺
      剩余分钟，不要用电压估算容量。
- [ ] 观察 `capacity` 是否在低电量时跳变（例如 7→3→0）；记录跳变点，
      用于第 10.4 节阈值校准。
- [ ] 记录 0% 时是否还能安全关机/是否提前关机；这决定 `critical_percent`
      的保守下限。

### 10.3 充电状态与插拔

- [ ] 插/拔充电器 20 次，记录 `axp2202-usb/online`、`status`、
      `time_to_full_now`、`capacity` 的每次变化。
- [ ] 对比 NextUI 的 `(online==1 && time_to_full>0)` 判定：Brick Pro 上
      `time_to_full_now` 在刚插入、涓流、满电时分别是多少；我们的
      `status`/`external_power` 是否更准确。
- [ ] 插电脑 USB（数据连接）与插充电器分别记录；`online` 是否能区分，
      `status` 是否变为 Not charging/Discharging。
- [ ] 满电后 `status` 是否为 Full；拔电后是否变 Discharging；是否有
      卡在 Charging 的状态。
- [ ] 充电时 `bd-battery` 的 `policy.suppressed=yes`、`recommendation=none`；
      拔电后若仍低电量，是否立即恢复 LOW/CRITICAL。

### 10.4 抖动与阈值校准

- [ ] 在 20% 附近每 5 秒采样 30 分钟，记录容量跳变；在 5% 附近同样采样。
- [ ] 根据跳变幅度调整 `confirm_samples`、`reminder_hysteresis_percent`、
      `critical_hysteresis_percent`；默认 2/3/2 只是起点，不是真机结论。
- [ ] 观察 `capacity` 在负载突变（启动游戏、Wi-Fi 扫描）时的瞬时跌落；
      如果单次跌落超过 3 个点，增加 confirm 或迟滞，而不是改读取层。
- [ ] 确认 AXP2202 的 `pmu_battery_warning_level1=5`/`level2=0` 是否产生
      中断或额外节点；本模块不依赖它们，策略不应与 PMIC 告警重复关机。
- [ ] 记录实际自动关机点与 `critical_percent` 的关系；确保提醒/临界阈值
      高于实际关机点，避免“刚提醒就关机”。

### 10.5 读取竞态与异常

- [ ] 在读取过程中快速重启/休眠/唤醒，重复 20 次；`bd-battery` 不应崩溃，
      不应把暂时缺文件打印成 0%。
- [ ] 用 `mv`/`rm` 临时移走 `axp2202-battery`（若可安全操作），确认
      `bd-battery` 报 `probe.read=stale` 且 `capacity=unknown`。
- [ ] 修改某个属性为坏值（仅测试环境），确认对应字段 unknown、
      `read_errors>0`，其余字段仍可读。
- [ ] 同时插拔 USB/Mains，确认 `external_power` 在 known true/false/unknown
      之间转换符合第 4.2 节，不会把未知当未插电。
- [ ] 在 `present=0` 时确认策略不产生 LOW/CRITICAL/PREPARE_SHUTDOWN。

### 10.6 策略与安全流程

- [ ] 用真实状态喂策略（可临时写一个宿主/设备测试程序），确认：
      20% 连续两次 → REMINDER；5% 连续两次 → CRITICAL；
      23% 连续两次 → RECOVERED；6% 不会离开 CRITICAL，7% 才回 REMINDER。
- [ ] 插电时低电量不弹提醒；拔电后立即恢复；插电期间等级仍正确跟踪。
- [ ] 容量 unknown、present unknown、外部电源 unknown 时，
      `recommendation` 只能是 NONE/SHOW_CRITICAL，不能是 PREPARE_SHUTDOWN。
- [ ] 只有 known present + known 未插电 + 连续确认的 CRITICAL 才出现
      `PREPARE_SHUTDOWN`；消费者仍需用户确认/保存，再调用 `bd_power_poweroff`。
- [ ] 记录策略在真实采样间隔下的 CPU 唤醒次数；稳定时应约 1 次/60 秒，
      而不是 1 次/秒。

### 10.7 与 home/settings 的统一接线

- [ ] 协调线程把首页/设置页的旧 `/sys/.../capacity` 读取替换为
      `bd_battery_read()`；旧读取删除后，未知状态显示占位符而不是 0%。
- [ ] 首页只显示容量、充电/外部电源状态；不显示 voltage/current/temp 等
      诊断字段。
- [ ] 设置页只暴露基本阈值；高级字段留在配置文件。
- [ ] 旧 `shutdown_on_low_battery` 行为改由消费者读取策略建议后执行；
      策略本身不直接关机。
- [ ] 应用运行/游戏前台时，策略事件仍由 shell 消费；不要在 home 绘制层
      直接关机。

### 10.8 中止条件

任一项出现以下情况，立即停止并恢复原厂 UI：

- 读取导致内核错误、PMIC 错误或文件系统错误；
- 设备在 unknown/0% 状态下自动关机或无法唤醒；
- 充电时策略仍建议关机；
- 连续 3 次读取崩溃/卡死；
- 旧 home 读取与新 API 同时写/读导致显示跳变或竞争。

恢复：按 `docs/device-testing.md` 的 `recover.sh` 流程；保留日志，不重复
失败操作。

## 11. 已知限制与未证实项

- **未证实**：Brick Pro 4.9.191 厂商内核的 AXP2202 属性集、单位、状态映射、
  `present` 语义、`current_now` 是否存在、`time_to_full_now` 的 0 值语义。
- **未证实**：AXP2202 `capacity` 在低电量/负载突变时的抖动、0% 行为、
  实际关机点；默认阈值/迟滞/确认次数只是保守起点。
- **未证实**：5000 mAh 标称与 DTB `pmu_battery_cap=3000` 的差异对百分比
  曲线的影响；本模块不估算剩余时间。
- **未实现**：PMIC 告警中断、LED 低电量指示、充电灯控制、自动休眠/关机；
  这些属于 power/LED/shell。
- **未实现**：多电池并联的容量加权；只选择一个电池节点，其余仅诊断。
- **未实现**：uevent 订阅；调用方负责在 uevent/电源事件时提前调用
  `bd_battery_policy_update`，策略只给 deadline。
- **未实现**：线程安全；多线程读取/策略更新需调用方串行化。
- **不声称**：`bd_battery_read()` 的多个属性是原子快照；sysfs 是逐文件
  读取，策略用确认/迟滞吸收抖动，读取层用 STALE 检测节点消失。
- **不声称**：`PREPARE_SHUTDOWN` 是关机命令；它只是建议，消费者必须自己
  完成用户确认、状态保存和安全关机。

## 12. 文件与集成点

| 文件/目标 | 作用 |
|---|---|
| `include/brickdesk/device_battery.h` | 公共读取/策略 API、状态、事件、配置 |
| `src/device/battery.c` | 结果名、错误、策略状态机、deadline |
| `src/device/battery_linux.c` | power_supply 枚举、选择、读取、坏值/STALE |
| `src/device/battery_internal.h` | 私有错误/字符串 helper |
| `src/device/battery_probe_main.c` | `bd-battery` 只读诊断 CLI |
| `tests/device_battery_test.c` | 读取/策略/边界/竞态测试 |
| `tests/fixtures/battery/` | full/limited/none 假设备树 |
| `brickdesk_battery` | CMake 静态库 |
| `bd-battery` | target/host 可执行文件；target `dist/bin`、发布包 |
| `bd_device_battery_test` | 测试可执行文件；CTest 名称 `device_battery` |

后续接线点：

1. 协调线程在 shell 中持有 `bd_battery` 和 `bd_battery_policy`，生命周期
   覆盖事件循环。
2. 首页/设置页用 `bd_battery_read()` 替换旧简易读取；只显示
   `capacity_known ? capacity_percent : unknown`、`status`、`external_power`。
3. 配置层把 `low_battery_percent` 映射到 `critical_percent`，
   `shutdown_on_low_battery` 作为消费者动作门；新增 reminder/高级键时只放
   配置文件/高级页。
4. 事件循环按 `next_sample_ms` 采样；在 power_supply uevent、插电、唤醒、
   应用切换时提前调用 update。
5. 收到 `PREPARE_SHUTDOWN` 后，消费者走 `bd_power_poweroff()` 的安全流程；
   在真机验收第 10 节通过前，不要自动执行。
6. 删除 home 分支旧 `/sys/.../capacity` 读取和旧的 `battery_percent`
   直接赋值；统一由本 API 提供 unknown 语义。
