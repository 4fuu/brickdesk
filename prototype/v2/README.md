# brickdesk v2 视觉稿

按 1024×768（4:3）绘制，文件拆分为设计令牌、基础层、封面墙、面板、设置页、覆盖层和三个脚本。

## 打开

直接用浏览器打开 `prototype/v2/index.html`，或用任意静态服务器：

```bash
python3 -m http.server 8000 --directory prototype
# http://localhost:8000/v2/
```

图片来自 `prototype/bg/`，字体来自 `prototype/fonts/`。

## 截图状态

在 URL 后加参数，可复现固定画面（用于截图与评审）：

| 参数 | 作用 |
| --- | --- |
| `capture=1` | 隐藏浏览器手柄并去掉缩放，页面固定为 1024×768 |
| `theme=dark` / `theme=light` | 主题 |
| `focus=<序号>` | 主页焦点，`0` 小镇四季，`5` 音乐 |
| `tab=0..3` | 全部 / 游戏 / 应用 / 收藏 |
| `side=1&si=0` | 打开左侧菜单并聚焦第一项 |
| `quick=1&qi=0` | 打开右侧快捷设置并聚焦第一项 |
| `screen=settings&cat=0&zone=opts&oi=0` | 打开设置页的指定分类与行 |
| `hint=1` | 显示按键提示栏 |
| `dialog=1` | 显示关机确认框 |
| `toast=<文字>` | 显示轻提示 |
| `instant=1` | 关闭所有过渡，便于稳定截图 |

示例：

```text
prototype/v2/index.html?capture=1&theme=light&focus=5
```

## 操作

方向键移动，Z/Enter = A，X/Esc = B，C/V = X/Y，Q/E = L1/R1，M = Menu，S = Start，T = 切换主题。按住 Tab 显示按键提示栏，松开收起。
