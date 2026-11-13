# 通知与确认层（WP-01）

状态：宿主构建与截图检查通过；不涉及真机硬件，前台应用期间的行为见第 4 节。

## 1. 范围

`include/brickdesk/notice.h` + `src/app/notice.c` 提供一个桌面级、与具体功能
无关的提示层：

- 瞬时通知（toast）：文本、等级、超时，最多排队 8 条；
- 模态确认框：标题、正文、取消/确认，异步回调；
- 绘制在 home 或设置页之后，使用与桌面一致的深色卡片、字体和强调色；
- 输入只由 shell 转发：A 确认、B 取消、左右切换焦点；
- 不依赖 SDL/GLES 之外的模块，也不写配置或设备节点。

## 2. 对外接口

其他模块只使用 `shell.h` 上的三个入口：

```c
bool bd_shell_notice(bd_shell *shell, bd_notice_level level,
                     const char *text, uint32_t timeout_ms);
bool bd_shell_confirm(bd_shell *shell, const char *title, const char *text,
                      uint32_t timeout_ms, bd_notice_confirm_fn on_done,
                      void *user);
bool bd_shell_confirm_open(const bd_shell *shell);
```

- `timeout_ms == 0`：toast 使用 3 秒默认值，确认框不自动关闭。
- `bd_shell_confirm()` 同一时间只允许一个确认框；第二个请求返回 false。
- 回调在桌面线程执行；只有用户点“确认”时 `confirmed=true`，取消、超时、
  被新确认框替换或 shell 销毁都会收到 `confirmed=false`。
- 等级：`BD_NOTICE_INFO/SUCCESS/WARNING/ERROR`，只影响强调色。

## 3. 绘制与生命周期

- toast 位于屏幕上方居中（y=84），最大宽度 720，单行省略；同一时间只显示
  最早的一条，超时后自动显示下一条。
- 确认框带全屏 scrim，卡片居中，按钮焦点有描边；打开/关闭时请求整屏重绘。
- `bd_notice_update()` 只在 shell 可见时推进计时；`bd_notice_next_deadline()`
  被并入 shell 的 poll timeout，因此不需要常驻 60 Hz 刷新。
- 绘制自行登记 damage：toast 消失后其区域会被 home/settings 重绘，不会留下
  残影。

## 4. 前台应用期间

前台应用拥有显示表面，shell 不参与合成。因此通知在应用运行期间保持排队，
不消耗超时；回到桌面（应用退出/切后台）后立即显示。低电量、恢复失败等需要
在游戏内可见的提示，后续由应用协议/菜单 shim 的叠加通道实现（WP-25），
本层不改变应用表面，也不抢占焦点。

## 5. 接线点

- `bd_shell_create()` 创建 notice 层并加载 UI 字体；
- `shell_service()` 在桌面可见时更新计时；
- `bd_shell_render()` 在 home/settings 之后绘制；
- `shell_dispatch_key()` 在确认框打开时优先消费 A/B/左右；
- 现有调用方：低电量提醒/自动关机倒计时（WP-05）、恢复失败提示（WP-06）。

## 6. 验证

- `BRICKDESK_WERROR=ON ./scripts/build-host.sh` 通过；
- `ctest --test-dir build-host` 45/45 通过；
- headless 截图检查了普通 toast 与确认框（深色卡片、中文文本、按钮焦点），
  临时截图工具未进入仓库。
