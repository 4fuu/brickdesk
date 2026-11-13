# 屏幕键盘

`brickdesk/keyboard.h` 提供手柄操作的通用文本输入控件，供 Wi-Fi/热点密码、
插件声明式 UI 和后续需要文本输入的设置项使用。样式沿用现有深色面板、圆角
按键和强调色，不另做一套视觉。

## 控件

```c
bd_keyboard *bd_keyboard_create(void);
void bd_keyboard_destroy(bd_keyboard *keyboard);

bool bd_keyboard_open(bd_keyboard *keyboard, const char *title,
                      const char *initial, size_t max_length, bool mask,
                      bd_keyboard_done_fn on_done, void *user);
bool bd_keyboard_is_open(const bd_keyboard *keyboard);
const char *bd_keyboard_text(const bd_keyboard *keyboard);
bool bd_keyboard_input(bd_keyboard *keyboard, bd_key key, bool pressed);
void bd_keyboard_draw(bd_keyboard *keyboard, bd_gfx *gfx,
                      const bd_ui_fonts *fonts, bd_color accent,
                      uint64_t now_ms);
```

`mask=true` 时输入框只画 `*`，用于密码。`max_length` 上限为
`BD_KEYBOARD_TEXT_MAX`（128），超出后继续按键不会改变文本。

## 操作

| 键 | 行为 |
| --- | --- |
| 方向键 | 在键盘网格中移动；上下跨行，左右到头循环 |
| A | 输入当前键 |
| B | 删除最后一个字符 |
| X | 在字母/符号布局间切换 |
| Y | 完成，回调 `accepted=true` |
| 底部“取消” | 关闭，回调 `accepted=false`，`text=NULL` |

大写键只影响下一次输入；输入一个字母后自动回到小写。符号页的“ABC”回到
字母页。回调在桌面线程执行，键盘已关闭后再调用。

## 桌面集成

`bd_shell_prompt_text()` 打开键盘模态层，`bd_shell_keyboard_open()` 查询状态。
键盘打开时 shell 在普通按键路径之前消费所有按键边沿，因此密码不会泄漏到下面
的设置页；POWER 仍由电源状态机优先处理。确认框打开时拒绝再开键盘，避免两层
模态叠加。

`bd_shell_render()` 在设置页/home 之后、notice 之前绘制键盘，所以确认框和
通知仍显示在最上层。

## 测试

- `tests/keyboard_test.c`：输入、删除、符号页、完成/取消、长度上限。
- `tests/gfx_capture.c` 的 `keyboard.png`：密码遮罩和完整布局截图基线。
- `tests/shell_settings_test.c`：通过 `bd_shell_prompt_text()` 打开、A 输入、
  Y 完成，验证 shell 的按键路由和回调。
