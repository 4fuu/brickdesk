# 显示增强与 USB-C DP（WP-10）

> 状态：宿主构建、假 sysfs 单元测试通过；真机显示效果和 `/dev/disp` gamma
> 尚未验证。真机验收见第 5 节。

## 1. 范围

`include/brickdesk/device_display.h` + `src/device/display.c` 实现：

- `/sys/class/disp/disp/attr/color_temperature`（-200..200）
- `/sys/class/disp/disp/attr/enhance_contrast`（10..100）
- `/sys/class/disp/disp/attr/enhance_saturation`（0..100）
- `/sys/class/disp/disp/attr/enhance_bright`（10..100）
- `/dev/disp` gamma 表白点校正（`DISP_LCD_SET_GAMMA_TABLE` 0x10b、
  `DISP_LCD_GAMMA_CORRECTION_ENABLE` 0x10c、
  `DISP_LCD_GAMMA_CORRECTION_DISABLE` 0x10d）
- `/sys/class/drm` 的 DP connector 只读检测

不实现：背光本身（见 [device-av.md](device-av.md)）、DP 模式切换（厂商没有
公开的切换节点）、色彩管理/ICC、HDR。

## 2. 证据与取值

NextUI 固定提交 `b6ec1343` 的
`workspace/tg5040/libmsettings/msettings.c`：

- `scaleColortemp()` 0..40 → -200..200；`SetRawColortemp()` 写
  `color_temperature`。
- `scaleContrast()` -4..5 → 10..100；`SetRawContrast()` 写
  `enhance_contrast`。
- `scaleSaturation()` -5..5 → 0..100；`SetRawSaturation()` 写
  `enhance_saturation`。
- `scaleExposure()` -4..5 → 10..100；`SetRawExposure()` 写
  `enhance_bright`。
- `DisplayCal_enableWithValues()` 用 sRGB 传输函数生成 256 项 gamma 表；
  Brick Pro 默认增益 100/94/54，0..200，100 中性。
- `PLAT_getHDMI()` 在 tg5040 上返回 0；NextUI 没有 DP 切换实现。

本实现直接写上述 raw 值，设置页用 raw 范围而不是 NextUI 的 0..40/-5..5
档位，避免再做一层会丢失精度的映射。白点 LUT 是独立实现的同一数学：
`sRGB -> linear * gain -> sRGB`，每项 `R<<16|G<<8|B`。

## 3. 配置

| key | 默认 | 范围 | 说明 |
|---|---|---|---|
| `display.color_temperature` | 0 | -200..200 | 负值偏冷，正值偏暖 |
| `display.contrast` | 50 | 10..100 | `enhance_contrast` |
| `display.saturation` | 50 | 0..100 | `enhance_saturation` |
| `display.exposure` | 50 | 10..100 | `enhance_bright` |
| `display.white_point` | true | — | 是否启用 gamma 白点 |
| `display.white_point_red` | 100 | 0..200 | 红增益 |
| `display.white_point_green` | 94 | 0..200 | 绿增益（Brick Pro 预设） |
| `display.white_point_blue` | 54 | 0..200 | 蓝增益（Brick Pro 预设） |

`bd_display_apply()` 先写四个 sysfs 节点，再应用 gamma。`/dev/disp` 不存在
或 ioctl 失败时，四个 sysfs 写入仍然算成功，错误写入
`bd_display.white_point_error`；这是“白点是增强项”的明确取舍，不是静默假成功。
设置页每次改值都会经 shell 的 config callback 立即应用；应用切到前台、退出
回桌面和桌面启动时也会重新应用。

## 4. USB-C DP

`bd_display_dp_connected()` 扫描 `/sys/class/drm` 下名字含 `-DP-` 的
connector，读取其 `status`，返回 `connected`/`disconnected` 和 connector 名。
shell 每 5 秒检查一次，变化时用 `bd_shell_notice()` 提示“外接显示已连接/
已断开”。没有写任何 DP 节点：调研文档没有给出 tg5040 的公开切换接口，猜测
一个 sysfs 写入会违反“真实路径、不猜寄存器”的规则。真机确认切换节点后再补。

## 5. 真机验收

- [ ] `ls /sys/class/disp/disp/attr`，确认四个节点存在且可写。
- [ ] 色温 -200/0/200、对比度 10/50/100、饱和度 0/50/100、曝光 10/50/100
      各写一次，记录实际画面变化方向和是否闪烁。
- [ ] `ls -l /dev/disp`；打开白点校正，记录 gamma 是否生效、是否有色偏；
      关闭后是否恢复。
- [ ] 白点增益 100/94/54 与 100/100/100 对比，确认 Brick Pro 预设是否合适。
- [ ] 接入 USB-C DP，确认 `/sys/class/drm` connector 名、热插拔提示和
      1024×768 内屏是否保持。
- [ ] 退出应用回桌面、息屏/唤醒、重启后，四项显示设置是否保持。
