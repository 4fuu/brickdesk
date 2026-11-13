# 电源、休眠与关机（M4-Power）

> **状态：未在 Brick Pro 真机验证。**
> 宿主状态机、故障注入测试和 aarch64 交叉编译已通过；真实休眠/唤醒、电源键、
> 标准关机、PMIC 和 20 轮连续休眠恢复都还没有真机证据。本文不声称任何未经
> 真机验证的恢复可靠性。真机步骤见第 8 节。

本文记录 `m4/power` 的电源后端：可注入操作的轻睡/深睡/唤醒/关机/重启状态机，
以及只使用公开 Linux 接口的后端。音频和亮度由 M4-A 负责，本文不实现也不写入
ALSA、`/dev/disp`、背光或 LED；home/app/settings 也不在范围内。

## 1. 范围与边界

实现：

- `include/brickdesk/device_power.h`：公共 API、hook 契约、报告结构。
- `src/device/power.c`：同步状态机、失败回滚、退化恢复、重复/取消语义。
- `src/device/power_linux.c`：只读能力探测；`/sys/power/state` 深睡；
  `reboot(2)` 标准关机/重启；PMIC 安全关机的显式 unsupported。
- `src/device/power_probe_main.c`：`bd-power` 探测/诊断 CLI，默认只读。
- `tests/device_power_test.c`、`tests/fixtures/power/`：宿主状态机和故障注入测试。

不做：

- 不实现音频暂停/恢复、背光、LED、震动。它们通过调用方 hook 接入。
- 不实现自动休眠定时器、电源键长按判定、充电策略。shell 拥有这些策略。
- 不实现输入守护进程、应用会话或服务监管；只定义调用顺序和失败语义。
- 不写 AXP2202 寄存器，不打开 I2C，不实现 PMIC 安全关机序列。
- 不把“关背光”当作休眠。`display_off` 只是轻睡的一个 hook；深睡必须由
  Linux 后端写 `mem`，失败时不会自动降级成轻睡。

## 2. 证据、版本与许可

### 2.1 版本固定

| 来源 | 版本/提交 | 用途 |
|---|---|---|
| TrimUI Brick Pro 固件 | `v1.1.1-20260717`，发布于 2026-07-18，`trimui_tg4040_20260717_v1.1.1.7z` | 设备型号、内核、电源节点、原厂脚本行为 |
| brickdesk 调研 | `docs/research/device-features.md`，2026-11-12 | 固件/源码事实汇总和证据索引 E1–E52 |
| Linux 稳定树 | `v4.9.191` | `/sys/power/state`、`reboot(2)` ABI |
| NextUI | `LoveRetro/NextUI`，`main` = `b6ec1343c0a2636ddaf2502fde52cff237c67377` | 电源键/混合休眠/深睡/关机的接口行为 |
| Knulli | `knulli-cfw/knulli-linux`，`knulli-main` = `57afa662009641d181f57241bcc480119b131530` | 电源键、suspend、resume hook、关机 |
| CrossMix | `cizia64/CrossMix-OS`，`main` = `206d612fdef83972ed54a17de955125d0c0b3528` | `lcd_off`/`lcd_on` 只关屏，不是休眠 |
| MinUI | `shauninman/MinUI`，`main` = `dbf89435a5179788418ff3573042cf9c7e75d9e0` | 仅用于确认 Brick Pro 支持状态；无正式许可证 |

固件发布：<https://github.com/trimui/firmware_brickpro/releases/tag/v1.1.1-20260717>。
调研文档记录 `rootfs:/etc/model` 为 `TG4040`、内核为厂商 `4.9.191`、PMIC 为
AXP2202；这些路径来自 `docs/research/device-features.md` 第 1.3、3.3 节和
E41。本文没有重新分发固件或厂商文件。

### 2.2 Linux ABI

- `Documentation/power/states.txt`（v4.9.191）说明 `/sys/power/state` 接受
  `freeze`/`standby`/`mem`/`disk`，其中 `mem` 是 Suspend-to-RAM 标签：
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/Documentation/power/states.txt?h=v4.9.191>
- `kernel/power/main.c`（v4.9.191）的 `state_show()`/`state_store()` 实现了该
  属性；`state_store()` 返回错误码或写入长度：
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/kernel/power/main.c?h=v4.9.191>
- **v4.9.191 主线的 `kernel/power/main.c` 没有 `mem_sleep` 属性。**
  `mem_sleep` 是后续内核/厂商 backport 的可选接口。因此本实现只把它当作可选
  探测项，不假设存在，也不依赖它来请求 `mem`。
- `reboot(2)` 的 `LINUX_REBOOT_CMD_POWER_OFF`/`LINUX_REBOOT_CMD_RESTART` 定义：
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/include/uapi/linux/reboot.h?h=v4.9.191>
  <https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/kernel/reboot.c?h=v4.9.191>
  v4.9 的 `reboot()` 在 `pm_power_off` 未注册时会对 `POWER_OFF` 返回 `EINVAL`；
  这是标准关机可能失败的一个具体路径，必须在真机确认。
- `wakeup_count` 在 v4.9 存在，但本实现没有使用它；`wakeup_count` 竞态处理是
  真机验收项，不是已实现能力。

### 2.3 NextUI 行为（只学习接口，不复制代码）

固定提交 `b6ec1343` 的关键事实：

- [`workspace/all/common/api.c` 的 `PWR_update()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/common/api.c#L4137-L4210)：
  短按电源键进入 sleep；按住约 1 秒调用 `PWR_powerOff(0)`；恢复后 1 秒内忽略误触。
- [`PWR_sleep()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/common/api.c#L4451-L4467)：
  先 `PWR_enterSleep()`，等待唤醒，再 `PWR_exitSleep()`。
  [`PWR_enterSleep()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/common/api.c#L4341-L4374)
  暂停音频、切 LED profile、背光 0、`SIGSTOP` 守护进程、`sync()`；
  `PWR_exitSleep()` 恢复频率/守护进程/背光/音量/音频。
- [`PWR_waitForWake()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/common/api.c#L4406-L4449)：
  轻睡等待；超时且不充电时调用 `PWR_deepSleep()`；深睡失败则关机。这证明“轻睡后
  深睡”的分层行为是真实存在的。
- [`PWR_deepSleep()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/common/api.c#L4469-L4493)
  优先执行 `${BIN_PATH}/suspend`，否则写 `/sys/power/state`；
  [`PLAT_deepSleep()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/all/common/api.c#L3849-L3896)
  对 open/write 失败重试 5 次。
- [`skeleton/SYSTEM/tg5040/bin/suspend`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/skeleton/SYSTEM/tg5040/bin/suspend#L16-L101)：
  深睡前停止 `bluetoothd`/`wpa_supplicant`、保存 ALSA、写 `mem`；失败重试 5 次，
  并用“睡超过 5 秒但返回非零”规避假失败；唤醒后异步恢复服务。
- [`workspace/tg5040/platform/platform.c` 的 `PLAT_powerOff()`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/tg5040/platform/platform.c#L235-L257)
  通过 `/tmp/poweroff`/`/tmp/reboot` 交给启动循环；
  [`poweroff_next.c`](https://github.com/LoveRetro/NextUI/blob/b6ec1343c0a2636ddaf2502fde52cff237c67377/workspace/tg5040/poweroff_next/poweroff_next.c#L279-L317)
  另有 I2C/PMIC 的软断电实现。

本实现采用了“轻睡层 + 深睡层 + 失败回滚”的结构，但**没有**复制 NextUI 的
5 次重试、假失败启发式、`SIGSTOP` 守护进程或 `/tmp` 文件协议。原因见第 5.5
节。

### 2.4 Knulli 行为

固定提交 `57afa662`：

- [`board/fsoverlay/usr/bin/power-button`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/fsoverlay/usr/bin/power-button#L24-L55)：
  短按 2 秒内进入 `knulli-suspend`；长按 2 秒执行 `knulli-shutdown -s`。
- [`board/fsoverlay/usr/bin/knulli-suspend`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/fsoverlay/usr/bin/knulli-suspend#L123-L157)：
  `pm-is-supported --suspend` 后调用 `pm-suspend`；另有 RTC 唤醒和 fake suspend。
- [`board/fsoverlay/etc/pm/sleep.d/10-hide-ui`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/fsoverlay/etc/pm/sleep.d/10-hide-ui#L5-L29)：
  suspend 关显示、静音；resume 恢复显示和音量。
- [`board/allwinner/a133/fsoverlay/etc/pm/sleep.d/99-xradio-wlan`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/allwinner/a133/fsoverlay/etc/pm/sleep.d/99-xradio-wlan#L14-L37)：
  suspend 解绑 USB、`rfkill block wifi`、卸载 `xradio_wlan`；resume 重新加载并 unblock。
- [`board/fsoverlay/usr/bin/knulli-shutdown`](https://github.com/knulli-cfw/knulli-linux/blob/57afa662009641d181f57241bcc480119b131530/board/fsoverlay/usr/bin/knulli-shutdown#L13-L58)：
  标准 `shutdown -Ph now`，并有 20 秒后 `reboot -f -p` 的兜底。

这些行为支持两个设计判断：深睡前后确实需要服务/模块的 prepare/restore；标准
关机需要兜底和真机验证，不能只靠一次 `reboot()` 调用。

### 2.5 CrossMix 与 MinUI

- CrossMix `206d612` 的 [`lcd_off.sh`](https://github.com/cizia64/CrossMix-OS/blob/206d612fdef83972ed54a17de955125d0c0b3528/System/usr/trimui/scripts/lcd_off.sh#L6-L14)/
  [`lcd_on.sh`](https://github.com/cizia64/CrossMix-OS/blob/206d612fdef83972ed54a17de955125d0c0b3528/System/usr/trimui/scripts/lcd_on.sh#L18-L27)
  只通过 `dispdbg` 改背光和 LED，不是 suspend。它明确说明“关屏”和“休眠”是两件事。
- MinUI `dbf8943` 的 README 只列 Brick（`Trimui Brick`），没有 Brick Pro
  分支；调研还确认 MinUI 没有正式许可证。本实现没有使用 MinUI 代码。

### 2.6 许可提醒

NextUI 使用 PolyForm Noncommercial 1.0.0；MinUI 没有正式许可证；Knulli 为
GPL 系；CrossMix 为 GPL-3.0。本文只记录接口事实和公开 ABI，不复制上述项目的
代码、脚本、寄存器序列或资源。本文的 C 实现是独立编写的。

## 3. 状态机

### 3.1 状态

| 状态 | 含义 |
|---|---|
| `IDLE` | 正常运行，所有 hook 层未激活 |
| `ENTERING_LIGHT` | 正在执行轻睡 enter 序列 |
| `LIGHT_SLEEP` | 输入/应用/资源/显示层已按调用方 hook 暂停 |
| `ENTERING_DEEP` | 正在执行深睡 prepare 或内核 `mem` |
| `WAKING` | 正在按逆序恢复；可从 `LIGHT_SLEEP` 或 `DEGRADED` 进入 |
| `SHUTTING_DOWN` / `REBOOTING` | 正在 quiesce 并调用 shutdown 后端 |
| `DEGRADED` | 至少一个 restore hook 失败，资源可能仍被暂停 |

没有可观察的 `DEEP_SLEEP` 状态：深睡后端是同步阻塞调用，返回时已经 resume。
在 `ENTERING_DEEP` 期间重入会得到 `BD_POWER_BUSY`。

### 3.2 转移

```text
IDLE --sleep_light--> ENTERING_LIGHT --ok--> LIGHT_SLEEP
IDLE --sleep_deep---> ENTERING_DEEP --kernel mem--> [resume] --> IDLE
LIGHT_SLEEP --sleep_deep--> ENTERING_DEEP --kernel mem--> [resume] --> LIGHT_SLEEP
LIGHT_SLEEP --wake/cancel--> WAKING --ok--> IDLE
LIGHT_SLEEP --poweroff/reboot--> SHUTTING_DOWN/REBOOTING
  --backend returns--> 回滚 --> LIGHT_SLEEP 或 DEGRADED
任意 enter/restore 失败且回滚失败 --> DEGRADED
DEGRADED --wake--> WAKING --ok--> IDLE
```

- `sleep_deep` 从 `IDLE` 直接调用时，会执行完整的 input/app/deep-resource/
  display enter；从 `LIGHT_SLEEP` 调用时只增加 `resource_prepare(DEEP)` 和内核
  `mem`，保留轻睡层，返回后仍是 `LIGHT_SLEEP`。这对应 NextUI 的混合休眠行为。
- `wake` 从 `DEGRADED` 调用会重试所有仍标记为 touched 的 restore 层；这是唯一
  的恢复入口。
- `cancel` 在 `IDLE` 只是排队取消下一次 sleep；在 `LIGHT_SLEEP` 等价于唤醒；
  在 `DEGRADED` 返回 `RECOVERY_REQUIRED`。

## 4. 调用方 hook 契约

### 4.1 hook 列表

| hook | 何时调用 | 调用方必须保证 |
|---|---|---|
| `input_release` | 任何 sleep/shutdown 的最前面 | 停止分发/消费输入，释放 `EVIOCGRAB` 或等价占用；保留电源键唤醒源；可重复调用 |
| `app_pause` | input release 之后、资源准备之前 | 暂停前台应用并保存会话状态；不阻塞事件循环；可重复调用 |
| `resource_prepare(mode)` | app pause 之后、display off/深睡前 | 按 `LIGHT` 或 `DEEP` 停止非必要服务、flush 状态、准备网络/音频等；必须可逆；允许部分成功后失败 |
| `display_off` | 轻睡或直接深睡的 enter 末尾 | 只关显示/背光；不等于 suspend；可重复调用 |
| `display_on` | restore 的第一个可见步骤 | 恢复显示；允许在 `display_off` 失败后调用 |
| `resource_restore(mode)` | display on 之后，按 DEEP→LIGHT 逆序 | 撤销同 mode 的 prepare；即使 prepare 只完成一部分也必须安全 |
| `app_resume` | 资源恢复之后 | 恢复前台应用；可重复调用 |
| `input_acquire` | 最后 | 恢复输入分发/占用；可重复调用 |

所有 hook 在调用线程执行，不能重入状态机；重入返回 `BD_POWER_BUSY`。hook 不应
无限阻塞；超时策略由 shell 自己负责。`bd_power_create()` 要求 input/app/display
三对 hook 都存在；`resource_prepare`/`resource_restore` 可同时为 NULL，否则必须
成对出现。没有物理显示的调用方应提供 no-op display hook，而不是让模块跳过顺序。

### 4.2 顺序

轻睡 enter：

```text
input_release -> app_pause -> resource_prepare(LIGHT) -> display_off
```

轻睡 leave：

```text
display_on -> resource_restore(LIGHT) -> app_resume -> input_acquire
```

直接深睡（从 `IDLE`）：

```text
input_release -> app_pause -> resource_prepare(DEEP) -> display_off
  -> backend.deep_sleep() [write mem; blocks until resume]
  -> display_on -> resource_restore(DEEP) -> app_resume -> input_acquire
```

分层深睡（从 `LIGHT_SLEEP`）：

```text
resource_prepare(DEEP) -> backend.deep_sleep() -> resource_restore(DEEP)
（保留轻睡层；之后由 wake 执行轻睡 leave）
```

从 `IDLE` 关机/重启：

```text
input_release -> app_pause -> resource_prepare(DEEP) -> display_off
  -> backend.shutdown()
  -> 若返回：display_on -> resource_restore(DEEP) -> app_resume -> input_acquire
```

从 `LIGHT_SLEEP` 关机/重启：

```text
resource_prepare(DEEP) -> backend.shutdown()
  -> 若返回：resource_restore(DEEP)，保持 LIGHT_SLEEP
```

### 4.3 失败回滚与恢复顺序

- 任何 enter 失败后，按“显示 → 深资源 → 轻资源 → 应用 → 输入”的逆序回滚。
  即使某个 restore 失败，也继续尝试后面的步骤，避免一个坏 hook 把其他资源永久
  留在暂停状态。
- 回滚全部成功：回到 enter 前的状态（`IDLE` 或 `LIGHT_SLEEP`），结果分别为
  `BD_POWER_HOOK_FAILED` / `BD_POWER_BACKEND_FAILED` 等。
- 回滚有失败：进入 `DEGRADED`，结果 `BD_POWER_DEGRADED`，报告的
  `failed_step` 保留第一次失败，`restore_failures` 记录恢复失败次数。
- `DEGRADED` 下 sleep/cancel/shutdown 都返回 `BD_POWER_RECOVERY_REQUIRED`；
  只有 `bd_power_wake()` 会重试未完成的 restore。重试成功后回到 `IDLE`。
- 深睡后端失败后，仍会执行对应的 restore，因为内核可能已经 resume 或部分
  suspend；不会因为后端返回错误就跳过恢复。
- 关机后端按契约成功时不返回。若它返回 `OK`，视为
  `BD_POWER_BACKEND_NO_EFFECT` 并回滚；不会假装已经关机。

### 4.4 重复与取消

| 场景 | 结果 | 是否调用 hook |
|---|---|---|
| `LIGHT_SLEEP` 时再次 `sleep_light` | `BD_POWER_ALREADY` | 否 |
| `LIGHT_SLEEP` 时 `sleep_deep` | 允许，分层深睡 | 是；仅 DEEP prepare/backend |
| 连续两次 `sleep_deep`（每次返回后） | 每次都是新周期，不视为重复 | 是 |
| 过渡状态中重入 | `BD_POWER_BUSY` | 否（外层继续） |
| `IDLE` 时 `wake` | `BD_POWER_NOT_SLEEPING` | 否 |
| `IDLE` 时 `cancel` | 排队取消；`OK` | 否 |
| 已排队时再次 `cancel` | `BD_POWER_ALREADY` | 否 |
| 排队后下一次 sleep | `BD_POWER_CANCELLED`，清除排队 | 否 |
| `LIGHT_SLEEP` 时 `cancel` | 等价唤醒；`OK` | 是，恢复全部层 |
| `DEGRADED` 时 `cancel` | `BD_POWER_RECOVERY_REQUIRED` | 否 |
| `PMIC_SAFE` 关机 | `BD_POWER_UNSUPPORTED` | 否 |
| 配置未启用某动作 | `BD_POWER_DISABLED` | 否 |

取消只覆盖“已经排队但尚未开始”的 sleep；同步 enter 一旦开始，不能从另一个
线程取消。深睡写 `mem` 无法取消；要中止只能等唤醒。这个限制是刻意保留的，
避免用信号或线程强行中断内核 suspend。

## 5. Linux 后端

### 5.1 能力探测（只读）

`bd_power_linux_probe()` 只读以下路径，不写任何文件：

- `/sys/power/state`：必须可读；包含 `mem` 才报告 `mem_supported=true`。
- `/sys/power/mem_sleep`：可选。存在时记录原始 mode 列表和 `[...]` 当前项。
- `/sys/class/power_supply/axp2202-usb/online`：可选，0/1。
- `/sys/class/power_supply/axp2202-battery/capacity`：可选，0..100。

`bd-power` 默认只打印这些值和 PMIC blocker；不会打开 `/dev/i2c-*`，不会写
`/sys/power/*`。

### 5.2 深睡顺序

`bd_power_linux_backend().deep_sleep`：

1. 如果未设置 `allow_state_changes`，返回 `DISABLED`。
2. 只读检查 `/sys/power/state` 是否包含 `mem`；没有则 `UNSUPPORTED`。
3. 如果调用方显式给了 `mem_sleep_mode`：
   - 读 `/sys/power/mem_sleep`；不存在或不包含该 mode 时 `UNSUPPORTED`；
   - 写入选定 mode 并读回验证；
   - 后续 `mem` 写入失败时尝试恢复原来的 mode。
   - 未给 mode 时保持内核当前选择，不做任何写。
4. `sync()`。
5. 打开 `/sys/power/state` 并写 `mem\n`。写成功并返回表示内核完成了
   suspend/resume 周期；关闭 fd 后返回 `OK`。
6. 任何 open/write/close 失败返回 `ERROR`，`errno` 保留给报告。

### 5.3 标准关机/重启

`shutdown`：

1. 未设置 `allow_state_changes`：`DISABLED`。
2. `sync()`。
3. `reboot(RB_POWER_OFF)` 或 `reboot(RB_AUTOBOOT)`。
4. 成功时进程不会返回；若返回 `0` 视为 `NO_EFFECT`，否则 `ERROR` + `errno`。

这条路径不执行 `shutdown -Ph`、不 unmount SD、不杀进程。它只调用 Linux
`reboot(2)`。数据落盘和会话保存必须由调用方在 `resource_prepare`/`app_pause`
里完成；`sync()` 不能替代卸载文件系统。

### 5.4 `mem_sleep` 与 v4.9 的差异

v4.9.191 主线的 `/sys/power/state` 只有 `mem` 标签，没有 `mem_sleep` 选择器。
Brick Pro 的厂商内核可能 backport 了 `mem_sleep`，也可能没有；调研没有给出
确定结论。因此：

- 默认不写 `mem_sleep`，只用 `mem`。
- `bd-power`/`bd_power_linux_probe` 会把实际存在的 mode 列表打印出来，便于
  真机确认。
- 如果真机确认需要 `deep` 而不是 `s2idle`，shell 可以显式传
  `mem_sleep_mode="deep"`；后端会在写入 `mem` 前验证该 mode 被内核列出。
- 不假设 `deep` 一定存在，也不把 `s2idle` 失败误判为“深睡成功”。

### 5.5 为什么没有重试/假失败启发式

NextUI 的 suspend 脚本在写 `mem` 失败时重试 5 次，并在“实际睡了超过 5 秒但
返回非零”时把结果当作成功。这在该项目上有其历史原因，但本实现不复制：

- 重试可能连续触发 suspend，尤其是在唤醒源/电源键仍活跃时。
- “睡了超过 5 秒”不能证明内核真的进入 Suspend-to-RAM，也不能证明恢复后的
  服务/输入/音频是好的。
- 把失败报告为成功会违反本项目的验收原则。

因此后端只报告一次尝试的真实结果；重试策略由 shell 在真机验证后显式实现。
没有自动 fallback 到轻睡，也没有“假关机”。

### 5.6 PMIC 安全关机：明确 unsupported

`BD_POWER_POWEROFF_PMIC_SAFE`、`bd_power_linux_pmic_poweroff()` 和
`bd_power_linux_pmic_blocker()` 的行为：

- 永远返回 `BD_POWER_BACKEND_UNSUPPORTED` / `BD_POWER_UNSUPPORTED`。
- 不打开 `/dev/i2c-*`，不写任何寄存器，不硬编码地址/寄存器值。
- 具体阻塞：
  1. 没有公开的 AXP2202 + TG4040 软断电寄存器序列；厂商 4.9.191 内核没有
     公开源码。
  2. 唯一能看到的实现是 NextUI `workspace/tg5040/poweroff_next/poweroff_next.c`
     （提交 `b6ec1343`，PolyForm Noncommercial），它没有在 Brick Pro 1.1.1
     上被本仓库复现或验证。
  3. I2C 总线号、PMIC 从地址、寄存器写入顺序和断电后的唤醒行为都没有只读
     探测可确认；错误写入可能导致 limbo 或无法开机。
  4. 标准 `reboot(RB_POWER_OFF)` 是否出现 limbo 也尚未真机确认。
- 因此本模块不提供“安全关机”实现；需要时返回 unsupported 并让 shell 走标准
  关机，或在获得真机证据后另开任务实现。

## 6. 小 API 与 shell 接线

### 6.1 构建目标

- 库：`brickdesk_power`（`src/device/power.c`、`src/device/power_linux.c`）。
- CLI：`bd-power`（`src/device/power_probe_main.c`）；target 构建进入
  `dist/bin/bd-power`，发布包安装到 `bin/bd-power`，并附带
  `docs/device-power.md`。默认只读，危险动作需要显式参数。
- 测试：`bd_device_power_test`（CTest 名称 `device_power`）。

库不链接 SDL/GLES/ALSA；target 构建和 host 构建使用同一份源码。

### 6.2 最小接线示例

下面是 shell 侧未来的接线形状，不是当前已实现的 `src/app/shell.c`。它只展示
API 的所有权和顺序；实际 hook 应调用 inputd、session、M4-A 的显示/音频接口。

```c
#include "brickdesk/device_power.h"

/* Shell-owned: both members must outlive the bd_power handle. */
struct shell_power {
  struct shell *shell;
  bd_power_linux linux;
  bd_power *power;
};

static int shell_input_release(void *user) {
  /* Release EVIOCGRAB / stop dispatching, keep the power-key wake source. */
  return shell_input_suspend((struct shell *)user) ? 0 : -1;
}

static int shell_input_acquire(void *user) {
  return shell_input_resume((struct shell *)user) ? 0 : -1;
}

static int shell_app_pause(void *user) {
  return session_pause_foreground((struct shell *)user) ? 0 : -1;
}

static int shell_app_resume(void *user) {
  return session_resume_foreground((struct shell *)user) ? 0 : -1;
}

static int shell_resource_prepare(void *user, bd_power_sleep_mode mode) {
  /* LIGHT: stop non-essential work. DEEP: also stop Wi-Fi/BT/audio if the
     shell decides so. Every change must be reversible. */
  return shell_quiesce((struct shell *)user, mode) ? 0 : -1;
}

static int shell_resource_restore(void *user, bd_power_sleep_mode mode) {
  return shell_unquiesce((struct shell *)user, mode) ? 0 : -1;
}

static int shell_display_off(void *user) {
  /* M4-A owns brightness; this is a hook, not implemented here. */
  return display_off((struct shell *)user) ? 0 : -1;
}

static int shell_display_on(void *user) {
  return display_on((struct shell *)user) ? 0 : -1;
}

static bool shell_power_create(struct shell_power *sp, struct shell *shell) {
  char error[256];
  sp->shell = shell;
  if (!bd_power_linux_init(&sp->linux, NULL, true, NULL, error,
                           sizeof(error))) {
    return false;
  }
  bd_power_config config = {
      .hooks = {
          .user = shell,
          .input_release = shell_input_release,
          .app_pause = shell_app_pause,
          .resource_prepare = shell_resource_prepare,
          .display_off = shell_display_off,
          .display_on = shell_display_on,
          .resource_restore = shell_resource_restore,
          .app_resume = shell_app_resume,
          .input_acquire = shell_input_acquire,
      },
      .backend = bd_power_linux_backend(&sp->linux),
      .allow_light_sleep = true,
      .allow_deep_sleep = true,
      .allow_poweroff = true,
      .allow_reboot = true,
  };
  sp->power = bd_power_create(&config, error, sizeof(error));
  return sp->power != NULL;
}
```

`bd_power_linux` 和 `bd_power` 必须由 shell 持有；不能把 `bd_power_linux` 放在
`shell_power_create()` 的栈上返回，否则 `bd_power` 的 backend `user` 指针会悬空。
`bd_power_linux_init()` 的第三个参数 `allow_state_changes=true` 是破坏性操作的
显式 opt-in；生产 shell 只能在真机验收（第 8 节）通过后设置，测试/探测默认
false。

### 6.3 事件循环中的调用点

- 短按电源键：`bd_power_sleep_light()`；返回 `BD_POWER_OK` 后 shell 停止重绘
  并等待电源键/其他唤醒源。
- 唤醒：`bd_power_wake()`；返回 `BD_POWER_DEGRADED` 时显示错误并保留
  `DEGRADED` 状态，直到再次 `wake` 成功。
- 空闲超时：shell 先 `bd_power_sleep_light()`，再在合适时机
  `bd_power_sleep_deep()`；深睡超时和充电策略由 shell 决定。
- 长按电源键：`bd_power_poweroff(power, BD_POWER_POWEROFF_STANDARD, &r)`；
  不要默认调用。PMIC 安全路径不可用。
- 重启：`bd_power_reboot()`。
- 活动事件（按键/触摸/应用请求）：`bd_power_cancel()` 取消排队中的自动休眠。
- 插件事件：shell 可在 enter/leave 前后发送 `system.suspend`/`system.resume`；
  这不是 power 模块的职责，当前未接线。

### 6.4 与 M4-A 和其他模块的边界

- 亮度/背光：power 只调用 `display_off`/`display_on` hook；不写 `/dev/disp`，
  不调 M4-A 的亮度 API。M4-A 的恢复顺序（先 raw 8 再实际值）由它自己实现。
- 音频：power 只调用 `resource_prepare/restore`；不碰 ALSA/tinymix。
- 应用/session：power 只调用 `app_pause`/`app_resume`；不杀进程、不解析存档。
- perf/网络：power 不直接停 Wi-Fi/BT；由 shell 在 resource hook 中按真机证据
  决定。Knulli 的 `99-xradio-wlan` 和 NextUI 的 suspend 脚本是参考，不是本模块
  的默认行为。
- 输入：power 不打开 evdev；只要求 inputd 释放占用并保留唤醒源。

## 7. 测试与验证

### 7.1 宿主

```sh
cmake -S . -B build-host -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBRICKDESK_WERROR=ON
cmake --build build-host --parallel
ctest --test-dir build-host -R device_power --output-on-failure
```

结果：

```text
1/1 Test #5: device_power .....................   Passed
100% tests passed, 0 tests failed out of 1
```

同一配置的全量 CTest 为 32/32 通过；ASan/UBSan 构建也通过
`device_power_test: OK`。

测试覆盖：

- 轻睡 enter/leave 的精确调用顺序；重复轻睡 `ALREADY` 不调用 hook。
- 排队取消、取消已排队、轻睡中取消、`DEGRADED` 取消。
- input/app/light-resource/display 每个 enter 失败点的回滚顺序。
- 回滚失败进入 `DEGRADED`，`wake` 只重试未完成层并回到 `IDLE`。
- 直接深睡、轻睡后分层深睡、连续两次深睡、深睡后唤醒。
- 深睡后端 `ERROR`/`DISABLED`/`UNSUPPORTED`/`NO_EFFECT` 的报告与回滚。
- 深睡 restore 失败进入 `DEGRADED`，`wake` 的恢复顺序。
- 标准关机/重启成功返回视为 `NO_EFFECT`；后端失败回滚；从轻睡关机的层保留。
- `PMIC_SAFE` 返回 `UNSUPPORTED` 且不调用任何 hook/backend。
- hook 重入返回 `BUSY`，外层继续。
- probe 解析 `full`/`limited` fixture；`state` 文件前后不变；缺文件报错。
- 未 opt-in 的 Linux backend 深睡/关机返回 `DISABLED`；未知 `mem_sleep`
  mode 在写 `mem` 前被拒绝；fixture 的 `state` 不被修改。

测试只使用假 hook/backend 和 fixture；没有在宿主执行真实 suspend 或
`reboot(2)`。

### 7.2 aarch64 交叉编译

本地用 Debian `aarch64-linux-gnu-gcc 12.2.0` 对电源库、CLI 和测试做了独立
交叉编译（不依赖 SDL/SDK）：

```sh
aarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude \
  -c src/device/power.c -o /tmp/power-cross/power.o
aarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror -Iinclude \
  -c src/device/power_linux.c -o /tmp/power-cross/power_linux.o
aarch64-linux-gnu-gcc ... -o /tmp/power-cross/bd-power
aarch64-linux-gnu-gcc ... -o /tmp/power-cross/bd_device_power_test
file /tmp/power-cross/bd-power
```

结果：两个 ELF 都是 `ARM aarch64`。所有由本实现引入的 libc 符号都是
`GLIBC_2.17`（包括 `reboot`、`sync`、`open`、`fopen`、`strtok_r` 等）；
Debian 交叉链接的 `__libc_start_main` 显示 `GLIBC_2.34` 是 Debian 12 启动
文件造成的，不是本模块的符号。正式 target CI 使用官方 GCC 7.5 工具链/GLIBC
上限检查，结果见本分支的 GitHub Actions。

本分支的 CI 记录（提交 `e4dfcb5`，2026-11-13）：

- host：<https://github.com/4fuu/brickdesk/actions/runs/43379408035>，
  `success`；全量 32/32 CTest 通过。
- target：<https://github.com/4fuu/brickdesk/actions/runs/43379408364>，
  `success`；日志显示官方工具链构建了 `libbrickdesk_power.a`、`bd-power` 和
  `tests/bd_device_power_test`（aarch64）。
- 早期 `m4/power` 的 CI 只构建了 `bd-power`；本打包跟进分支已把它加入
  `scripts/build-target.sh` 的 `dist/bin`、`scripts/package-target.sh` 的
  `payload/bin`、离线包检查以及 target workflow 的 `GLIBC <= 2.23` 检查。

### 7.3 探测 CLI

宿主 fixture：

```sh
./build-host/bd-power --root tests/fixtures/power/full
```

输出（节选）：

```text
probe.read_only=yes
sys.power.state=freeze mem disk
sys.power.mem_supported=yes
sys.power.mem_sleep.present=yes
sys.power.mem_sleep.modes=s2idle [deep]
sys.power.mem_sleep.current=deep
power.usb_online=1
power.battery_capacity=73
pmic.safe_poweroff=unsupported
backend.state_changes=disabled
```

真机默认只读：

```sh
adb shell /mnt/SDCARD/brickdesk/bin/bd-power
```

显式诊断（会真的改变设备状态）：

```sh
# 仅诊断内核 mem 路径；不经过 shell hook、不停止 Wi-Fi/BT/音频
adb shell /mnt/SDCARD/brickdesk/bin/bd-power \
  --allow-state-changes --deep-sleep

# 标准 Linux 关机；需要 --yes
adb shell /mnt/SDCARD/brickdesk/bin/bd-power \
  --allow-state-changes --poweroff --yes

# 标准 Linux 重启；需要 --yes
adb shell /mnt/SDCARD/brickdesk/bin/bd-power \
  --allow-state-changes --reboot --yes
```

`--deep-sleep` 是后端诊断，不是生产休眠序列；它不调用 input/app/display
hook，也不停止服务。生产路径必须由 shell 通过 `bd_power` 状态机执行。

`bd-power` 随 target 构建进入 `build-target/dist/bin/bd-power`，并随发布包安装到
`/mnt/SDCARD/brickdesk/bin/bd-power`；包内同时附带 `docs/device-power.md`。

## 8. 真机验收矩阵

以下全部是**待执行**项。每项记录：日期、固件 `/etc/version`、`/etc/model`、
内核 `uname -a`、操作者、命令输出、照片/日志。不要在没有记录的情况下把某项
标记为通过。

### 8.1 前置

```sh
adb shell 'cat /etc/model; cat /etc/version; uname -a'
adb shell 'cat /sys/power/state; cat /sys/power/mem_sleep 2>/dev/null'
adb shell 'cat /sys/class/power_supply/axp2202-usb/online 2>/dev/null'
adb shell 'cat /sys/class/power_supply/axp2202-battery/capacity 2>/dev/null'
adb shell '/mnt/SDCARD/brickdesk/bin/bd-power'
```

- [ ] `/etc/model` 是 `TG4040`，不是 TG4070。
- [ ] `/sys/power/state` 含 `mem`。
- [ ] `bd-power` 的 `probe.read_only=yes`，`pmic.safe_poweroff=unsupported`。
- [ ] 记录 `mem_sleep` 是否存在、当前 mode；不存在时记录为“主线 4.9 ABI 无此节点”。
- [ ] 如果 `mem_sleep` 存在且当前不是 `deep`，先不要自动切换；在 8.4 单独对比。

### 8.2 标准关机与 limbo

在已备份 SD 卡、确认可拔卡开机后进行。**这是破坏性操作，必须显式执行。**

- [ ] 执行 `bd-power --allow-state-changes --poweroff --yes`，观察是否真正断电。
- [ ] 若黑屏但未断电、插 USB 无反应（limbo），立即记录，不再重复；改用长按电源/
      拔电池恢复，并保留日志。
- [ ] 连续 10 次“关机 → 开机 → 进入桌面”，记录每次是否正常启动。
- [ ] 标准关机通过前，不把 `allow_poweroff=true` 接到生产 shell 的长按动作。

### 8.3 标准重启

- [ ] `bd-power --allow-state-changes --reboot --yes`，确认重启而非关机。
- [ ] 连续 10 次重启，记录启动时间、文件系统错误、设置是否保留。
- [ ] 若 `reboot(2)` 返回 `EINVAL`/`EPERM`，记录 `errno`；这可能是
      `pm_power_off` 未注册或权限问题，不要猜测。

### 8.4 深睡单次

- [ ] 记录 `bd-power --allow-state-changes --deep-sleep` 的耗时、返回码、
      `dmesg` 中 suspend/resume 行。
- [ ] 用 USB 电流表或电池采样记录 sleep 电流；与 idle 对比。若电流没有明显下降，
      不能称为深睡成功。
- [ ] 唤醒后检查：屏幕、背光、触摸/按键、音频、Wi-Fi、蓝牙、SD 卡、时间。
- [ ] 如果 `mem_sleep` 存在，分别记录当前 mode 和 `--mem-sleep deep` 的结果；
      不要假设 `deep` 一定可用。

### 8.5 连续 20 轮深睡/恢复（硬性门槛）

在电源键/唤醒源可控、服务已接入 shell 的前提下执行。若只用 `bd-power` 诊断，
它不经过 shell hook，不能作为“生产恢复可靠性”的证据；下表同时要求 shell 路径。

诊断后端 20 轮：

```sh
for i in $(seq 1 20); do
  echo "cycle $i $(date -u +%FT%TZ)"
  /mnt/SDCARD/brickdesk/bin/bd-power \
    --allow-state-changes --deep-sleep || { echo "FAIL cycle $i"; break; }
  sleep 3
  cat /sys/class/power_supply/axp2202-battery/capacity
done
```

- [ ] 20/20 次命令返回 0，且每次都能被电源键唤醒。
- [ ] 每次唤醒后 `bd-power` 仍能读到 `mem`，屏幕/输入可用。
- [ ] 记录每轮耗时、电量、`dmesg` 错误；任何一轮失败即整体失败。
- [ ] 诊断 20 轮通过后，再用 shell 的生产路径重复 20 轮（短按轻睡 → 超时深睡 →
      电源键唤醒），检查 app pause/resume、输入释放/恢复、音频、Wi-Fi/BT。
- [ ] 生产路径 20/20 通过前，不声称“休眠恢复可靠”。

建议的 shell 路径记录字段：

| 轮次 | 进入轻睡 | 深睡返回 | 唤醒键 | 显示 | 输入 | 前台应用 | 音频 | Wi-Fi | BT | 备注 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | | | | | | | | | | |
| ... | | | | | | | | | | |
| 20 | | | | | | | | | | |

### 8.6 轻睡与取消

- [ ] 短按电源键 → 轻睡；确认 `display_off` 后屏幕黑、音频按 hook 策略暂停、
      输入不再进入前台应用。
- [ ] 轻睡中短按/按键唤醒；确认 `display_on`、资源恢复、app resume、input
      acquire 的顺序没有卡死。
- [ ] 轻睡中再次短按不产生嵌套；重复请求返回 `ALREADY`（shell 日志）。
- [ ] 空闲计时中按键 → `cancel` 排队；下一次自动休眠被取消，不进入 sleep。
- [ ] 轻睡中长按电源 → 关机路径；失败时仍回到轻睡，而不是黑屏无响应。

### 8.7 失败与恢复

- [ ] 在 shell 中注入一个 restore hook 失败（例如临时让 `display_on` 返回
      非零），确认状态变为 `DEGRADED`、日志记录 `failed_step`，再次 `wake`
      能重试并回到 `IDLE`。
- [ ] 注入 `resource_prepare` 失败，确认没有调用深睡后端，且 app/input 已恢复。
- [ ] 注入深睡后端失败（测试构建），确认从 `LIGHT_SLEEP` 失败后仍是
      `LIGHT_SLEEP`，不是 `IDLE`，也不是 `DEGRADED`（若 restore 成功）。
- [ ] 任何失败都不得被报告为 `OK`。

### 8.8 PMIC 与安全关机

- [ ] 调用 `bd_power_poweroff(PMIC_SAFE)`（或在 CLI 中走对应 API）确认返回
      `UNSUPPORTED`，且 `strace`/日志没有 `/dev/i2c` 打开。
- [ ] 在拿到 AXP2202 数据手册、确认 I2C 总线/地址、并在可恢复设备上验证寄存器
      序列之前，不实现、不启用 PMIC 软断电。
- [ ] 若标准关机出现 limbo，记录为已知风险；不要用未验证的 I2C 写入“修复”。

### 8.9 充电、电量与自动休眠

- [ ] `axp2202-usb/online` 在“充电”和“USB 连接电脑”两种情况下分别记录。
- [ ] 充电时自动深睡是否被 shell 阻止；本模块只提供 `usb_online` 只读值，
      不强制策略。
- [ ] 低电量（`capacity` 接近 0）时能否安全关机；记录电量曲线和实际关机点。
- [ ] 电量计标称 5000 mAh 与 DTB `pmu_battery_cap` 3000 的差异按调研记录，
      不用电量百分比承诺剩余时间。

### 8.10 恢复与中止条件

任一项出现以下情况，立即停止该轮并恢复原厂 UI：

- 黑屏但设备未断电（limbo）；
- 唤醒后输入/显示/音频持续不可用；
- 连续 3 轮深睡失败；
- 内核日志出现文件系统错误或 PMIC 错误；
- 设备无法通过电源键/长按恢复。

恢复：拔卡开机进入原厂 UI，或按 `docs/device-testing.md` 的 `recover.sh`
流程。恢复后保留日志，不要继续重复失败操作。

## 9. 未证实路径与已知限制

- **未证实**：Brick Pro 上 `/sys/power/state` 的 `mem` 是否真的进入
  Suspend-to-RAM、唤醒源、唤醒耗时、电流、20 轮恢复、Wi-Fi/BT/音频/输入 fd
  恢复、标准关机是否 limbo、PMIC 行为、充电时自动休眠策略。
- **未证实**：`mem_sleep` 是否存在；v4.9.191 主线没有该属性，厂商内核可能
  backport。默认不写它。
- **未实现**：`wakeup_count` 竞态处理、自动重试、NextUI 式假失败启发式、
  SD 卡卸载、进程杀死、PMIC 软断电。
- **未实现**：自动休眠定时器、电源键长按判定、充电策略；这些由 shell 拥有。
- **不声称**：`bd_power_sleep_deep()` 返回 `OK` 只表示内核接受了 `mem` 且调用
  返回；不代表实际功耗、恢复完整性或 20 轮可靠性。
- **不声称**：`bd_power_sleep_light()` 是 suspend；它只是调用方 hook 的软件
  轻睡层。深睡没有自动 fallback 到它。
- **数据风险**：标准 `reboot(2)` 前只做 `sync()`，不 unmount SD；调用方必须在
  `app_pause`/`resource_prepare` 中保存并 flush 自己的状态。
- **CLI 诊断**：`bd-power --deep-sleep` 绕过 shell hook，只测内核路径，不能
  作为生产恢复证据。
- **构建**：`bd-power` 已加入 target `dist/bin`、发布包 `payload/bin`、离线包
  结构和安装检查，以及 target workflow 的 GLIBC 检查；默认只读和危险动作开关
  没有改变。

## 10. 文件与集成点

| 文件/目标 | 作用 |
|---|---|
| `include/brickdesk/device_power.h` | 公共 API、hook/backend 契约、报告 |
| `src/device/power.c` | 状态机、回滚、取消、重复、报告 |
| `src/device/power_linux.c` | 只读 probe、`mem` 深睡、`reboot(2)`、PMIC unsupported |
| `src/device/power_probe_main.c` | `bd-power` CLI，默认只读 |
| `tests/device_power_test.c` | 宿主状态机/故障注入/probe 测试 |
| `tests/fixtures/power/` | full/limited sysfs fixture |
| `brickdesk_power` | CMake 静态库 |
| `bd-power` | CMake 可执行文件；target 构建为 `build-target/dist/bin/bd-power`，发布包安装到 `bin/bd-power` |
| `bd_device_power_test` | 测试可执行文件；CTest 名称 `device_power` |

后续 shell 接线需要：

1. 在 shell 中持有 `bd_power_linux` 和 `bd_power`，生命周期覆盖事件循环。
2. 把 inputd 的 release/acquire、session 的 pause/resume、M4-A 的 display
   off/on、服务层的 prepare/restore 接入 hook。
3. 把电源键短按、空闲超时、长按、活动取消接到 API。
4. 在真机完成第 8 节，尤其是 8.5 的 20 轮；在此之前保持
   `allow_state_changes=false`，只使用只读 probe。
