# brickdesk 配置参考

本文对应 M1-04 配置核心，schema 版本 1。配置用 INI（inih）书写，机器数据
仍用 JSON。配置加载后是只读快照；UI 和 `brickdesk-config` 修改值时通过
`include/brickdesk/config.h` 的接口校验并原子写回 `local.ini`。

## 1. 文件与覆盖层级

从低到高：

| 层 | 文件 | 谁写 | 说明 |
|---|---|---|---|
| 1 | 编译期默认值 | 开发者 | 定义在 `src/config/schema.c`，每个 key 都有默认值或标记为可选 |
| 2 | `brickdesk.ini` | 固件 | 主题、界面、显示、输入、电源、插件、会话 |
| 3 | `perf.ini` | 固件 | 性能 profile |
| 4 | `hotkeys.ini` | 固件 | 组合键 |
| 5 | `input.ini` | 固件 | 设备/按键映射 |
| 6 | `local.ini` | UI / CLI | 只写用户改过的 key，覆盖前面所有层 |

`brickdesk.ini`、`perf.ini`、`hotkeys.ini`、`input.ini` 都属于“系统层”；
同一 key 在后面的文件出现时覆盖前面的文件。`local.ini` 是唯一用户层。

目录解析顺序：

1. 命令行 `--config-dir DIR`（`brickdesk` 和 `brickdesk-config` 都支持）；
2. 环境变量 `BRICKDESK_CONFIG_DIR`；
3. 编译期默认目录。宿主机构建默认是仓库里的 `config/`；交叉构建可用
   `-DBRICKDESK_CONFIG_DIR=/mnt/SDCARD/brickdesk/config` 覆盖。

设备上推荐：

```text
/mnt/SDCARD/brickdesk/config/
  brickdesk.ini
  perf.ini
  hotkeys.ini
  input.ini
  local.ini
```

不在设备上时，`brickdesk --config-dir ./config` 直接使用仓库里的默认文件。
配置文件缺失不是错误：该层跳过，使用更低的层。配置目录不存在会报错，但
主程序仍用默认值启动（安全模式），`brickdesk-config check` 返回非 0。

## 2. 语法、类型与校验

- 注释：行首 `;` 或 `#`；`;` 也可以作为行内注释。
- section：`[section]`，key：`name = value`；值两侧空白会去掉。
- 空值 `key =` 合法；字符串空值就是空字符串。
- 未知 section/key：警告，不删除；`local.ini` 里的未知 key 在保存时原样保留。
- 单个 key 校验失败：报 `file:line`，该值不生效，下一层有效值继续生效；
  如果没有任何低层值，则使用编译期默认值。加载不会因为单个坏值而失败。
- 语法错误：报 `file:line`，继续加载该文件里已经解析出的内容。

schema 类型：

| 类型 | 写法 | 规则 |
|---|---|---|
| `bool` | `true` / `false` | 也接受 `yes/no/1/0/on/off`，不区分大小写 |
| `int` | `600` | 十进制整数，必须在 schema 的 min..max 内 |
| `enum` | `dark` | 必须在枚举表中，不区分大小写，输出规范化为小写/标准拼写 |
| `string` | `zh-CN` | 长度上限见各 key；必填 string 不能为空；不接受换行；不做通配或路径展开 |
| `accent` | `persimmon` / `#e07a5f` | 预设名或 `#RRGGBB`；非法值报行号 |
| `duration` | `180s` | 支持 `ms`、`s`、`min`；不带单位时按 key 的默认单位 |
| `combo` | `L1+R1+Start` | 组合键/序列语法，见第 6 节 |

没有实现的架构类型：`path`、`frequency`。主题/资源路径暂由上层自己校验；
频率是否在设备可用表中由 M1-07 的 perfd 探测并校验。

## 3. `[meta]` 与迁移

每个文件都可以有：

```ini
[meta]
schema_version = 1
```

当前支持版本是 1。

- 缺失 `[meta]` 或版本低于 1：视为版本 0。主程序、`get`、`set`、`dump`
  会先迁移，把原文件备份为 `<file>.bak.v0`，再原子写回带
  `schema_version = 1` 的文件；只做结构迁移，不改变用户值。
- `brickdesk-config check` 不迁移，只报告“migration needed”警告；CI 用它
  校验默认文件时不会修改仓库。
- 版本高于当前：报错，进入只读安全模式；已知 key 仍可读，`set`/`save` 拒绝写入。
- 迁移失败：保留原文件，报错并进入只读模式。
- 迁移会重写文件，原文件的注释不保留（备份里有原始内容）。

迁移钩子表在 `src/config/config.c` 的 `migrations[]`；当前只有 `0 -> 1`。
新增结构变化时加一个 `N -> N+1` 钩子并把 `BD_CONFIG_SCHEMA_VERSION` 加一。

## 4. `brickdesk.ini`

### 4.1 `[theme]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `mode` | enum | `dark` | `dark` | 桌面为深色专用；旧配置中的 `light`/`auto` 会按 `dark` 加载 |
| `accent` | accent | `persimmon` | 预设名或 `#RRGGBB` | 强调色；焦点、开关、进度、当前标记使用 |
| `ambient` | enum | `baked` | `baked`、`solid` | 背景：预烘焙封面底图 + 纱层，或纯色 |

强调色预设（深色桌面，可用 `bd_config_accent_hex()` 解析）：

| 预设 | 值 |
|---|---|
| `persimmon`（柿红，默认） | `#e07a5f` |
| `coral` | `#fd5c47` |
| `teal` | `#4fb3a9` |
| `blue` | `#6c9bd1` |
| `green` | `#86b36b` |
| `violet` | `#a98bd6` |

自定义 `#RRGGBB` 原样使用；用户自己负责对比度。

### 4.2 `[ui]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `language` | enum | `zh-CN` | `zh-CN`、`en-US` | 界面语言 |
| `animations` | bool | `true` | — | 过渡/焦点动画；关闭可降低 CPU |
| `blur` | bool | `true` | — | 顶栏/提示栏的窄条模糊；关闭后退化为渐变 |
| `time_format` | enum | `24h` | `24h`、`12h` | 时钟格式 |
| `clock_seconds` | bool | `false` | — | 时钟是否显示秒 |
| `cover_cache_mb` | int | `96` | 8..512 | 封面缩略图缓存上限（MB） |

### 4.3 `[display]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `width` | int | `1024` | 320..4096 | 逻辑宽度；Brick Pro 为 1024 |
| `height` | int | `768` | 240..4096 | 逻辑高度；Brick Pro 为 768 |
| `refresh_hz` | int | `60` | 30..120 | 刷新率 |
| `brightness` | int | `70` | 0..100 | 背光百分比 |
| `color_temperature` | int | `0` | -200..200 | 厂商 `color_temperature`；负冷正暖 |
| `contrast` | int | `50` | 10..100 | 厂商 `enhance_contrast` |
| `saturation` | int | `50` | 0..100 | 厂商 `enhance_saturation` |
| `exposure` | int | `50` | 10..100 | 厂商 `enhance_bright` |
| `white_point` | bool | `true` | — | 是否启用 `/dev/disp` gamma 白点 |
| `white_point_red` | int | `100` | 0..200 | 白点红增益，100 中性 |
| `white_point_green` | int | `94` | 0..200 | 白点绿增益，Brick Pro 预设 |
| `white_point_blue` | int | `54` | 0..200 | 白点蓝增益，Brick Pro 预设 |

### 4.4 `[audio]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `volume` | int | `70` | 0..100 | 系统输出音量百分比 |
| `mute` | bool | `false` | — | 静音；保留音量值，取消静音后恢复 |

音量、静音和背光由 `bd_settings_system` 接口应用到硬件；M3 使用记录状态的
stub，M4 接入设备后端。

### 4.5 `[input]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `repeat_delay_ms` | int | `400` | 0..2000 | 方向键首次重复前延迟 |
| `repeat_interval_ms` | int | `80` | 10..1000 | 方向键重复间隔 |
| `chord_window_ms` | int | `50` | 10..500 | chord 判定窗口 |
| `long_press_ms` | int | `500` | 100..3000 | 应用内长按阈值；热键用各自的 `hold_ms`，不读这个值 |
| `double_tap_ms` | int | `300` | 100..1000 | 双击判定窗口 |
| `haptics` | bool | `true` | — | 菜单/系统触觉总开关 |
| `haptic_strength` | int | `60` | 0..100 | 菜单触觉强度（马达百分比） |
| `haptic_duration_ms` | int | `35` | 5..500 | 单次菜单触觉时长 |
| `fn_switch_action` | enum | `mute` | `none`/`mute`/`led_off`/`performance`/`powersave`/`turbo`/`dpad_joystick` | FN 拨动开关动作 |
| `f1_action` / `f2_action` | enum | `none` | `none`/`screenshot`/`settings`/`quick`/`recent`/`return_home`/`menu` | F1/F2 动作 |
| `f1_app` / `f2_app` | string | `""` | 应用 ID | 非空时优先启动该应用 |
| `home_action` | enum | `recent` | 同 F1 动作表 | Home 键动作 |
| `dpad_to_joystick` | bool | `false` | — | 写 `input_dpad_to_joystick` |
| `turbo` | bool | `false` | — | 写 `turbo_*` 控制文件 |

### 4.6 `[led]`

Brick Pro 的厂商 LED 分区。`m` 是顶栏/Logo，`lr` 是摇杆环，`f1`/`f2`
是 FN 键（共用亮度），`rear` 是扳机。所有亮度为 0..100。

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `enabled` | bool | `true` | — | 灯光总开关；关闭后所有分区熄灭 |
| `duration_ms` | int | `1000` | 100..60000 | 效果单次时长，也是睡眠脉冲 5 次的单次时长 |
| `indicator_brightness` | int | `100` | 0..100 | 低电量/充电/睡眠指示亮度 |
| `ambient` | bool | `false` | — | 按前台应用推送/末帧颜色驱动灯光 |
| `ambient_brightness` | int | `100` | 0..100 | 氛围灯亮度 |
| `m.brightness` / `m.color` / `m.effect` | int/enum/enum | `100`/`white`/`static` | 0..100 / 颜色表 / 效果表 | 顶栏分区 |
| `lr.brightness` / `lr.color` / `lr.effect` | 同上 | 同上 | 同上 | 摇杆环分区 |
| `f1.brightness` / `f1.color` / `f1.effect` | 同上 | 同上 | 同上 | FN 1 分区；F2 共用亮度 |
| `f2.color` / `f2.effect` | enum/enum | `white`/`static` | 同上 | FN 2 分区 |
| `rear.brightness` / `rear.color` / `rear.effect` | 同上 | 同上 | 同上 | 扳机分区 |

颜色表：`white`、`red`、`orange`、`yellow`、`green`、`cyan`、`blue`、
`violet`、`magenta`、`off`。效果表：`linear`、`breathe`、
`interval_breathe`、`static`、`blink1`、`blink2`、`blink3`、`rainbow`、
`twinkle`、`fire`、`glitter`、`neonglow`、`firefly`、`aurora`、`reactive`，
对应厂商 effect 编号 1..15。

按应用关闭氛围灯：在 `local.ini` 增加 `[led.app.<app-id>] ambient = false`。
应用也可以通过 IPC 通知 `led.ambient` / `led.set` 推送颜色（见
[docs/app-protocol.md](app-protocol.md)）。

### 4.7 `[power]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `sleep_after` | duration | `60s` | 0..24h | 无操作进入轻睡前的时间；不带单位按秒；0 禁用自动轻睡 |
| `deep_sleep_after` | duration | `30s` | 0..24h | 轻睡多久后深睡；0 禁用自动深睡 |
| `long_press_action` | enum | `shutdown` | `shutdown`/`reboot`/`none` | 电源键按住 1 秒的动作 |
| `safe_shutdown` | bool | `true` | — | 关机前经 `/dev/i2c-6` 对 AXP2202 软断开，失败回退标准关机 |
| `low_battery_percent` | int | `5` | 0..100 | 低电量关机阈值 |
| `shutdown_on_low_battery` | bool | `true` | — | 临界电量倒计时结束后是否自动关机 |

### 4.8 `[plugins]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `enabled` | bool | `true` | — | 插件总开关 |
| `max_total_rss_mb` | int | `128` | 16..1024 | 所有运行中插件 RSS 总上限；超出后停止最久未活动的非可见插件 |
| `idle_stop` | duration | `60s` | 0..24h | 插件空闲停止时间；不带单位按秒 |
| `autostart_delay` | duration | `2s` | 0..10min | 开机后延迟启动插件；不带单位按秒 |

### 4.8 `[session]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `state_quota_mb` | int | `512` | 64..4096 | 每个应用的存档配额 |
| `quick_resume` | enum | `auto` | `auto`、`always`、`never` | 快速恢复策略 |
| `max_background` | int | `3` | 1..8 | 同时保留在后台的应用数量上限 |
| `memory_budget_mb` | int | `560` | 128..1024 | 后台应用的内存软上限（MB） |

## 5. `perf.ini`

profile 段是动态的：`[profile.<name>]`、`[app.<id>]`、`[core.<name>]`。
配置层校验类型和范围；profile 继承链、未知 profile、循环检测由 M1-07 的
perfd 在应用策略时完成。

### 5.1 `[perf]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `global_profile` | string | `default` | 任一 `[profile.<name>]` 名 | perfd 的全局档位；设置页从这里读取和写回 |

`global_profile` 只在设置页做成员校验；perfd 启动时仍以 `--profile` 参数
为准，读取该 key 由后续的接线任务完成。

### 5.2 `[profile.<name>]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `inherit` | string | 未设置 | profile 名，≤64 字符 | 继承另一个 profile |
| `governor` | enum | 未设置 | `performance`、`schedutil`、`ondemand`、`powersave` | CPU governor |
| `min_khz` | int | 未设置 | 0..3000000 | 最低频率（kHz） |
| `max_khz` | int | 未设置 | 0..3000000 | 最高频率（kHz） |
| `online_cores` | int | 未设置 | 2..4 | 在线核心数 |
| `swappiness` | int | 未设置 | 0..100 | 内核 swappiness |

未设置的字段由 `inherit` 链或 perfd 的策略决定；最终写入的频率必须是设备
`scaling_available_frequencies` 里的值。

### 5.3 `[app.<id>]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `profile` | string | 必填 | profile 名，≤64 字符 | 应用启动时使用的 profile |

`<id>` 是应用 id，例如 `org.libretro.retroarch`。

### 5.4 `[core.<name>]`

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `inherit` | string | 未设置 | profile 名，≤64 字符 | 继承 profile |
| `governor` | enum | 未设置 | 同 profile | 覆盖 governor |
| `min_khz` | int | 未设置 | 0..3000000 | 覆盖最低频率 |
| `max_khz` | int | 未设置 | 0..3000000 | 覆盖最高频率 |
| `online_cores` | int | 未设置 | 2..4 | 覆盖核心数 |
| `swappiness` | int | 未设置 | 0..100 | 覆盖 swappiness |

### 5.5 预设、继承与解析

随固件提供这些预设（都可在 `perf.ini` 里覆盖或删除）：

| profile | governor | min/max kHz | online_cores | 用途 |
|---|---|---|---|---|
| `default` | `schedutil` | 408000 / 1200000 | 4 | 桌面空闲，perfd 的全局 profile |
| `auto` | `schedutil` | 继承 `default` | 4 | NextUI 风格的自动调频预设 |
| `balanced` | 继承 `default` | 408000 / 1416000 | 4 | 轻负载应用 |
| `performance` | `performance` | 816000 / 1800000 | 4 | 游戏/模拟器 |
| `powersave` | `powersave` | 408000 / 816000 | 2 | 省电 |

`profile`、`app`、`core` 的名字都可以自定义。解析顺序从低到高：

```text
default（或显式 profile）
  < [app.<id>] profile
    < [core.<name>] 的 inherit 链与字段
```

- `inherit` 形成链，按从基类到子类的顺序覆盖字段。检测循环、超长链和
  未知 profile：perfd 拒绝本次 apply 并返回错误，不会悄悄应用半个 profile。
- `[core.<name>]` 没有 `inherit` 时叠加在应用/全局 profile 上；有
  `inherit` 时以该 profile 链为新的基类，再叠加 core 自己的字段。
- 未列出的 app id 不是错误，回退到 `default`；显式写了不存在的 profile
  或 core 名是错误。
- 频率单位是 kHz。`0` 表示“保持当前值”。请求先 clamp 到
  `cpuinfo_min_freq..cpuinfo_max_freq`，再向下取
  `scaling_available_frequencies` 中不超过请求值的最近一档。因此
  `min_khz` 可能略低于请求值，但不会低于硬件下限。任意阶段
  `min > max` 都拒绝，且不会写任何文件。
- `governor` 必须在 `scaling_available_governors` 中。唯一例外是
  `schedutil`：不可用时按 `ondemand` → `performance` 回退，并在结果里
  返回 `governor_fallback:true`；其他 governor 不可用直接报错。
- `online_cores` 只接受 2..4，且不能超过探测到的 CPU 数；cpu0 永远保持
  在线。降核按 cpu3→cpu2 的顺序写 `online`，升核按低编号到高编号。
- `swappiness` 可选，范围 0..100。节点不存在时该项标记为 unsupported 并
  跳过，不会让 CPU 配置整体失败。
- thermal zone 和 `/sys/class/devfreq/*` 只读展示；M1-07 不写 GPU 频率、
  thermal trip 或 cooling device。

### 5.6 `brickdesk-perfd`

```sh
brickdesk-perfd [--config-dir DIR] [--socket PATH] [--sysfs-root DIR]
                [--profile NAME] [--verbose]
brickdesk-perfd --dry-run [--profile NAME] [--app ID] [--core NAME] ...
brickdesk-perfd --status [--config-dir DIR] [--sysfs-root DIR]
```

- 默认 socket：`$BRICKDESK_PERF_SOCKET`，否则
  `/run/brickdesk/perfd.sock`；`/run/brickdesk` 不可创建时回退
  `/tmp/brickdesk-perfd.sock`。`SOCK_STREAM`，权限 0660，带 `.lock`
  `flock`，第二个实例会拒绝启动（单写者）。
- `--sysfs-root DIR` 是测试前缀：`/sys/...` 和 `/proc/...` 会拼到
  `DIR` 后面；默认空前缀表示真实的 `/sys`、`/proc`。
- `--dry-run` 只探测、解析、打印计划（包括要写的路径和值），然后退出，
  不创建 socket、不写 sysfs。`--status` 打印当前探测结果，不查询正在
  运行的 daemon。
- 启动时记录 governor/min/max/online/swappiness 原值，应用全局 profile；
  收到 SIGINT/SIGTERM 时按原值恢复。SIGKILL 无法恢复，这是进程被强杀
  时的已知边界。
- 静止时阻塞在 `poll()`；超时取最近一条租约的到期时间，没有租约时无限
  阻塞，不忙等。

#### 5.5.1 协议

Unix socket 上是换行分隔的 JSON，一个对象一行。请求上限 4096 字节，
回复上限 8192 字节。连接建立后 daemon 先发：

```json
{"type":"hello","version":1,"pid":123,"socket":"/run/brickdesk/perfd.sock","profile":"default","cpufreq":true}
```

请求格式（`id` 由客户端选择，回复原样带回）：

```json
{"jsonrpc":"2.0","id":1,"method":"perf.apply","params":{"session":"7f3c2a","profile":"performance","ttl_ms":10000}}
```

成功回复：

```json
{"jsonrpc":"2.0","id":1,"result":{"profile":"performance","app":"","core":"","governor":"performance","min_khz":816000,"max_khz":1800000,"online_cores":4,"swappiness":20,"swappiness_supported":true,"governor_fallback":false,"ttl_ms":10000}}
```

失败回复：

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"min_khz 1608000 is greater than max_khz 1200000"}}
```

方法：

| method | params | 说明 |
|---|---|---|
| `perf.apply` | `session`（必填）、`profile`、`app`、`core`、`ttl_ms` | 申请/更新租约并立即应用；`ttl_ms` 省略或 0 表示直到 restore/断开 |
| `perf.set` | 同上 | 运行中切换，必须已有该 session 的租约，否则 `-32002`；省略 `ttl_ms` 时保留原到期时间 |
| `perf.restore` | `session` | 删除该 session 的租约；若它是当前生效租约则恢复上一条或全局 profile |
| `perf.get` | — | 当前生效 profile、租约剩余时间、实际 governor/频率/在线核/swappiness、thermal、devfreq |
| `perf.list` | — | profile/app/core 名称，以及探测到的 governor/频率/CPU 数 |
| `perf.capabilities` | — | `cpufreq`、`governor_control`、`frequency_control`、`online_cores`、`swappiness`、`thermal`、`gpu_devfreq`、`cpu_count`、`policy_count` |
| `perf.ping` | — | `{"pong":true}` |

`perf.get` 的 `result`（`lease` 为 null 表示当前由全局 profile 生效）：

```json
{"jsonrpc":"2.0","id":2,"result":{"profile":"default","app":"","core":"","lease":null,"cpufreq":true,"governor":"schedutil","cur_freq_khz":1200000,"min_khz":408000,"max_khz":1200000,"online_cores":4,"cpu_count":4,"swappiness":20,"thermal":[{"zone":"thermal_zone0","type":"cpu-thermal","temp_mc":42000}],"devfreq":[{"name":"5000000.gpu","cur_freq":660000000,"available_frequencies":[400000000,660000000]}]}}
```

`perf.restore` 的 `result`：

```json
{"jsonrpc":"2.0","id":3,"result":{"restored":true,"profile":"default"}}
```

租约语义：

- 新租约的 order 最大，优先于旧租约；同一 session 重复 apply 会更新租约，
  带 `ttl_ms` 时按新的 ttl 续期。
- 客户端断开：该连接拥有的租约全部删除；若删掉的是生效租约，重新计算
  下一条租约或全局 profile 并恢复。
- `ttl_ms > 0`：`poll` 按最近到期时间醒来，到期后自动恢复。`ttl_ms = 0`
  不会自动过期，适合由 session 管理进程显式 restore。
- 多条租约同时存在时，最新的一条生效；它结束后由下一条最新租约接管，
  没有租约时回到全局 profile。
- 所有 apply/set/restore/断开/到期都会写日志（含 old→new 值）；`--verbose`
  额外打印启动/恢复细节，但不会把 profile 持久化到 `/etc`。

写事务顺序：读旧值 → 校验/白名单 → 写 governor → 按升降频方向选择
先写 max 还是 min（通常先写 max；目标 max 低于当前 min 时先写 min），
避免中间态出现 `min > max` → 写 online → 写 swappiness → 回读校验。
任一步失败按已写步骤的逆序恢复旧值；回滚失败会同时报告原始错误和
rollback 错误。客户端只能传 `session`/`profile`/`app`/`core`/`ttl_ms`，
不能传 governor、频率值或 sysfs 路径；所有路径都在 daemon 启动时固定。

错误码沿用 JSON-RPC 语义：`-32600` 缺 method，`-32601` 未知 method，
`-32602` 参数错误，`-32002` 没有租约，`-32000` profile 解析或 sysfs
写入失败。

与架构草案的差异：M1-07 只管理探测到的第一个 cpufreq policy（A133P
通常只有 `policy0`，`policy_count` 会在 `perf.capabilities` 中报告）；
thermal 只读展示、电量未接入，不实现 80 °C 降频或低电量禁
performance；未知 profile 采用拒绝而不是静默回退 default；日志写
stderr，未实现 512 KB × 3 文件轮转（交给 supervisor/journal 收集）；
VM 组只接线 `swappiness`，
`vfs_cache_pressure`、`dirty_writeback_centisecs`、IO scheduler/read_ahead
未接线；`perf.ini` 仅在启动时加载，没有 SIGHUP 热重载；这些留给后续
milestone 或上层策略。

## 6. `hotkeys.ini`

每个 `[hotkey.<id>]` 是一个绑定。同 id 的 section 按 key 合并：高优先级的
key 覆盖低优先级的同名 key，未写的 key 继续使用低层值。这样 `local.ini`
可以只写用户改过的一个字段，不会把系统默认的 `combo`/`action` 弄丢。

| key | 类型 | 默认 | 取值/范围 | 说明 |
|---|---|---|---|---|
| `combo` | combo | 必填 | 见下 | 触发键 |
| `hold_ms` | int | 未设置 | 50..5000 | 写成 hold 绑定；省略时是 tap/chord/sequence 绑定 |
| `window_ms` | int | `50` | 10..500 | chord 判定窗口 |
| `sequence_ms` | int | `800` | 100..5000 | 序列下一步的等待窗口 |
| `action` | string | 必填 | ≤64 字符 | 动作名，如 `menu.open`、`builtin.screenshot` |
| `scope` | string | `global` | ≤64 字符 | `global`、`app:<id>`、`mode:<mode>` |
| `suppress` | bool | `false` | — | 预留给 grab + uinput 独占转发；M1-05 只记录警告 |
| `confirm` | bool | `false` | — | 触发前是否二次确认 |
| `repeat` | bool | `false` | — | 按住时重复；会隐含 hold 语义 |
| `repeat_ms` | int | `100` | 20..1000 | 重复间隔 |
| `plugin` | string | 空 | ≤64 字符 | 插件 id；设置后该绑定的动作路由到插件，而不是普通动作 |
| `action_id` | string | 空 | ≤64 字符 | 插件菜单 id（`menu.activate`）或热键动作 id（`hotkey`） |

> `hold_ms` 的 schema 默认值是 500，但输入层只在文件里显式写了 `hold_ms`
> 时才把绑定当作 hold。`brickdesk-config dump` 会显示这个默认值，判断语义
> 时以“是否显式设置”为准；省略就是 tap/chord/sequence。

### 6.1 组合键语法

- `A+B+C`：chord。所有键必须在 `window_ms` 内按下并保持同时按下；最后一个
  键按下时触发一次。释放顺序不影响已触发的动作，所有键松开后才会重新武装。
- `A,B`：序列。每一步都可以是 chord。前一步的所有键必须先松开，下一步才
  开始计时；`sequence_ms` 从松开时算起。超时后回到第一步。
- `Key(hold:800)`：hold 绑定，覆盖 `hold_ms`。动作在按住达到阈值时以
  `phase=down` 发出，松开时以 `phase=up` 发出。提示栏用这个语义。
- `Key(double)`：双击。第一次松开后 `input.double_tap_ms`（默认 300 ms）
  内再次按下触发；与同键的 tap 绑定时，双击会取消延迟的单击。
- 普通单键（如 `A`）是 tap：松开时触发；如果同一键还是某个序列/双击的
  前缀，tap 会延迟到 `sequence_ms`/`double_tap_ms` 之后再触发。
- 普通 chord/序列在完成时立即触发，不会等松开。
- `*`：任意键，只能单独使用；配置层接受，但 `brickdesk-inputd` 把它当作
  诊断语法拒绝，不建立绑定。
- 键名来自 `include/brickdesk/input.h` 的逻辑键：`A`、`B`、`X`、`Y`、
  `L1`、`R1`、`L2`、`R2`、`SELECT`、`START`、`MENU`、`HOME`、`UP`、
  `DOWN`、`LEFT`、`RIGHT`、`POWER`、`VOLUME_UP`、`VOLUME_DOWN`；
  大小写不敏感，输出统一大写。
- `hold_ms`、`(hold:N)`、`(double)` 只适用于单键绑定；用在 chord 或序列
  上会在加载时报错并跳过该绑定。
- `repeat=true` 只能用于单键；如果没有写 `hold_ms`，隐含 500 ms。

### 6.2 动作名

`action` 是自由字符串，长度 1..64，配置层只要求非空；`brickdesk-inputd`
不执行动作，只把 `binding/action/combo/trigger/phase` 通过 socket 转发给
订阅者。约定前缀：

| 前缀 | 例子 | 执行者 |
|---|---|---|
| `builtin.` | `builtin.screenshot`、`builtin.volume_up` | 系统/输入层 |
| `menu.` | `menu.open` | 菜单 shim |
| `ui.` | `ui.hint` | 桌面/提示栏 |
| `session.` | `session.return_home` | session |
| `perf.` | `perf.cycle` | perfd |
| `app.` | `app.launch` | session |
| `plugin.` | `plugin.invoke` | 插件宿主 |
| `script.` | `script.run` | 受限 worker |

未知动作名不是配置错误：它会被原样转发，便于新 UI 先接动作、后补文档。

### 6.3 冲突与优先级

加载 `hotkeys.ini` 时按 scope 分组检查，并报告 `file:line`：

| 类型 | 例子 | 处理 |
|---|---|---|
| 完全相同 | 两个 `Menu`（scope 重叠） | 错误；保留先加载的，拒绝后一个 |
| 前缀/子集 | `Menu` 与 `Menu+X`、`Menu` 与 `Menu,X` | 警告；保留两者，运行时按优先级解析 |
| 首键重叠 | `L1+Start` 与 `L1+Select` | 警告；两个 chord 都完成时只会有一个先触发 |
| scope 不重叠 | `global` 与 `app:foo` 的两个 `Menu` | 不冲突；各自生效 |

运行时优先级：**完成的 chord > 到达阈值的 hold > 双击 > 序列 > 延迟的
tap**。chord 完成会取消同键的 hold/tap；hold 触发会取消同键的 tap；序列
或双击完成会取消为它们让路的 tap。释放所有键后状态复位，不会粘键或重复
触发。事件时间戳乱序时按已处理的最大时间钳制，重复的 down/up 被忽略。

默认配置故意保留两个前缀关系，并由上述规则解析：`Menu`（hold 600）对
`Menu+X`（chord），`Select`（hold 400，提示栏）对 `Select+Up`（chord）。
`brickdesk-inputd` 启动时会打印这两条 warning，`bd-input --check` 也会。

`scope` 目前只实现“global 或与当前 scope 完全相同”的激活；`app:`、`mode:`
字符串会被接受和检查冲突，但 M1-05 的桌面/inputd 还没有应用上下文切换。
`suppress=true` 也尚未实现按绑定 grab+uinput 转发；inputd 只在菜单订阅者
显式请求 `grab` 时独占设备。

默认绑定：

| section | combo | 语义 | action |
|---|---|---|---|
| `hotkey.menu` | `Menu` | hold 600 | `menu.open` |
| `hotkey.hint` | `Select` | hold 400；按住显示提示，松开隐藏 | `ui.hint` |
| `hotkey.screenshot` | `L1+R1+Start` | chord | `builtin.screenshot` |
| `hotkey.performance` | `Select+Up` | chord | `perf.cycle` |
| `hotkey.return_home` | `Menu+X` | chord，confirm | `session.return_home` |

## 7. `input.ini`

`[device.<name>]` 是动态 section，任意 key 都按字符串接受（≤64 字符）。
按 `vendor`/`product` 匹配设备，不按 `/dev/input/eventN` 编号。

| key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `vendor` | string | 必填（设备段） | USB vendor id，例如 `0x1a2c` |
| `product` | string | 必填（设备段） | USB product id，例如 `0x0001` |
| 其他任意 key | string | — | 逻辑键名到 evdev 的映射，例如 `A = BTN_EAST`、`Up = ABS_HAT0Y:-1` |

值支持 `BTN_*`、`KEY_*`、`ABS_*:<方向>`（方向 `-1`/`0`/`1`）和数字码
（`0x130`、`304`）。每个设备从内置映射开始，只覆盖 section 里写出的字段：
A/B/X/Y、L1/R1/L2/R2、SELECT/START/MENU/HOME、方向键（HAT、BTN_DPAD 和
键盘方向键）、POWER、VOLUME_UP、VOLUME_DOWN。这样设备到货前也能工作；
Power/Volume 通常属于独立的 `gpio-keys` 设备，即使 `input.ini` 没写它，
内置映射仍会识别。

配置层不校验 evdev 名称/轴方向是否真实存在；M1-05 的 `bd-input --list`
和 `--learn` 会在设备上确认，无法识别时回退到内置映射。

## 8. `local.ini`

- 只写用户改过的 key；不会复制整份 `brickdesk.ini`。
- 保存时保留 `local.ini` 里的未知 section/key，方便降级到旧版本。
- 已知 key 的非法值在加载时会被忽略；保存时不会写回该非法值。
- 保存会重写文件，不保留手写注释；旧文件先复制为 `local.ini.bak`。
- 写入流程：同目录临时文件 `local.ini.tmp.<pid>` → `write` → `fsync` →
  `rename` → 目录 `fsync`。失败时删除临时文件，原文件不变。
- 权限 0644；打开临时文件使用 `O_NOFOLLOW`，不跟随符号链接。
- 保存前对同目录的 `local.ini.lock` 加 `flock(LOCK_EX)`；同一进程/多进程
  写入按锁串行，仍然不会出现半截文件。
- 运行中的 `brickdesk` 监听配置目录（inotify）并在 200 ms 去抖后调用
  `bd_config_reload()`；`kill -HUP <pid>` 也会触发同一次重载。重载失败保留
  旧快照并在桌面显示通知，成功时主题、模糊、动画、LED、电源、按键表、
  后台应用上限和状态配额即时生效。设置页自己保存 `local.ini` 时静默重载，
  不弹提示。

## 9. 命令行

```sh
brickdesk-config check                      # 校验所有文件，打印 file:line
brickdesk-config get theme.mode             # 打印有效值
brickdesk-config set theme.accent coral     # 写 local.ini
brickdesk-config unset theme.accent         # 删除用户覆盖，回到低层/默认
brickdesk-config dump                       # 每个 key 的值和来源层/文件/行
brickdesk-config --config-dir ./config dump
```

`get`/`set`/`unset`/`dump` 会在需要时迁移旧文件；`check` 不会写文件。
`set`/`unset` 只能修改已经存在的 key（包括系统文件里定义的动态 section）；要新增
热键或设备映射，先在对应 INI 文件里写好 section，再用 `set` 覆盖字段。

退出码：

- `0`：成功；
- `1`：配置错误、未知 key、保存失败，或 `get`/`dump` 遇到配置错误；
- `2`：命令行用法错误。

`dump` 输出示例：

```text
theme.mode = dark (system:/path/config/brickdesk.ini:11)
display.brightness = 80 (user:/path/config/local.ini:5)
hotkey.menu.window_ms = 50 (compiled)
```

`dump --json` 在 M1-04 未实现；当前是稳定的纯文本格式。

## 10. C 接口

公共头文件 `include/brickdesk/config.h`：

```c
bd_config *bd_config_load(const bd_config_options *options, char *error,
                          size_t error_size);
void bd_config_free(bd_config *config);

const char *bd_config_get_string(const bd_config *, const char *key,
                                 const char *fallback);
long bd_config_get_int(const bd_config *, const char *key, long fallback);
bool bd_config_get_bool(const bd_config *, const char *key, bool fallback);

bool bd_config_set(bd_config *, const char *key, const char *value,
                   char *error, size_t error_size);
bool bd_config_save(bd_config *, char *error, size_t error_size);
bool bd_config_reload(bd_config *, char *error, size_t error_size);
bool bd_config_unset(bd_config *, const char *key, char *error,
                     size_t error_size);

void bd_config_set_change_callback(bd_config *, bd_config_change_fn fn,
                                   void *user);

bool bd_config_describe(const bd_config *, const char *key,
                        bd_config_schema_info *out);
```

- key 是 section 和字段用 `.` 连接的完整路径，匹配时不做再次分割，所以
  section 名可以带空格/点：`device.TrimUI Gamepad.A` 是合法 key。
- `bd_config_set()` 只改内存并触发变化回调；`bd_config_save()` 才写
  `local.ini`。两者分开，UI 可以先批量改再保存。
- `bd_config_reload()` 从同一目录重读所有层，保留变化回调；新文件加载失败
  时旧快照不变。`bd_config_unset()` 删除某个用户覆盖，回到低层或编译默认，
  下一次 `bd_config_save()` 写入结果。
- 变化回调只在有效值真的变化时触发；把 key 设成当前值不会触发。
- `bd_config_get()` 返回 `bd_config_value`，可拿到类型和原始规范值；
  `duration` 的 `integer` 单位是毫秒。
- `bd_config_source_layer()`/`bd_config_source_file()` 用于 UI 显示来源。
- `bd_config_accent_hex(accent, dark)` 解析预设；自定义 `#RRGGBB` 原样返回，
  未知值返回 `NULL`。
- `bd_config_describe()` 返回 key 的 schema 元数据（类型、min/max、枚举表、
  默认值、长度），也支持 `hotkey.*`、`profile.*`、`app.*`、`core.*`、
  `device.*` 动态 key。设置页用它生成控件，不逐行硬编码范围或枚举。

## 11. M1-04 的边界与后续

已完成：

- 编译期默认 → 四个系统文件 → `local.ini` 的按 key 覆盖；
- 类型、范围、枚举、accent、duration、combo 校验和 `file:line`；
- schema_version 0→1 迁移、备份、只读安全模式；
- 临时文件 + fsync + rename + 目录 fsync 的原子保存，`flock` 串行化；
- `check`/`get`/`set`/`dump`；
- 主程序启动加载和 `--config-dir`；
- inotify + SIGHUP 热重载、`bd_config_reload()`、`bd_config_unset()`。

明确留给后续 milestone：

- 热键冲突/前缀/scope 合并：M1-05 已实现，见 `docs/input.md` 和
  `bd-input --check`；应用级 scope 切换仍待 M1-09；
- profile 继承链、循环、未知 profile：M1-07 已实现，见 5.5/5.6；
- `/usr/share`、`/etc`、环境变量白名单、逐 key 命令行覆盖；
- `path`、`frequency` 类型和 `restart_required` 标记；
- `dump --json`；
- 主题渲染接线：`[theme]` 值已经可读，`src/gfx/` 的主题系统在后续里程碑消费。

与 `docs/research/architecture.md` 第 7 节相比的差异：

1. 架构草案用 `[ui] theme = default`；产品最终确定 `[theme] mode = dark`，
   旧 `light`/`auto` 只作为兼容别名加载为 `dark`，并新增 `[theme] accent`、
   `[theme] ambient`。
2. 架构草案的 8 层覆盖只实现到“编译期 → 系统文件 → local.ini”；其余层
   在 M1-04 没有对应目录/生命周期，先不引入。
3. 单个坏值采用“忽略该值、保留下一层有效值”而不是无条件重置编译默认值；
   没有低层值时结果与架构草案一致。
4. 热键 section 采用逐 key 合并，而不是整个 section 覆盖；这样 `local.ini`
   只写改过的字段也能工作。
5. 迁移重写文件会丢注释；原文件在 `.bak.vN` 里保留。
6. `flock`、inotify 和 `unset` 已实现；仍未实现 `path`/`frequency` 类型、`dump --json`。

## 12. 测试

```sh
BRICKDESK_WERROR=ON ./scripts/build-host.sh
ctest --test-dir build-host --output-on-failure
```

`config` 单测覆盖：覆盖层级（含反向优先级）、坏行行号、范围/枚举/combo、
accent 预设与自定义十六进制、变化回调、`local.ini` 只写改动、未知 key 保留、
原子写失败不破坏原文件、0→1 迁移、未来版本只读、`bd_config_reload()` 保留
回调、`bd_config_unset()` 回到默认及保存后删除覆盖、保存生成 `.lock`。

`perf_policy` 单测用 `tests/fixtures/sysfs/` 假 sysfs 和
`tests/fixtures/perf/perf.ini` 覆盖：inherit 链、循环/未知 profile、
cpuinfo clamp 与频率下取整、min>max 拒绝、governor 白名单与 schedutil
回退、online cores/swappiness 写入、写一半失败后的逆序回滚，以及租约
到期后恢复 default。测试会复制假 sysfs 到 `/tmp`，不修改仓库文件。

`config_check` 直接运行：

```sh
./build-host/brickdesk-config check --config-dir ./config
```

默认配置必须保持零错误；CI 在 `ctest` 里执行同一条检查。
