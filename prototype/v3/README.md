# brickdesk v3 视觉稿

1024×768（4:3）固定画布，缩放到浏览器容器。封面墙固定 4 行、向右无限增长；默认 60 项，可在原型外切换 8 / 60 / 500 验证布局和虚拟化。

## 打开

用任意静态服务器从仓库根目录启动，访问 `/prototype/v3/`：

```bash
python3 -m http.server 8000 --directory prototype
# http://localhost:8000/v3/
```

图片来自 `prototype/bg/`，程序画面是 `prototype/v3/img/app-*.jpg`（模拟帧），字体来自 `prototype/fonts/`。

## 操作

| 键盘 | 手柄 | 作用 |
| --- | --- | --- |
| 方向键 | 十字键 | 在混合跨度磁贴间移动焦点 |
| Enter / Z | A | 打开、恢复；按住为长按 |
| Esc | B | 返回；主页是根 |
| C / V | X / Y | 运行中结束进程 / 动态磁贴暂停与下一首 |
| Q / E | L1 / R1 | 切换分类 |
| Tab（按住） | Select（按住） | 约 0.4s 显示底部按键提示，松开收起 |
| Space / S | Start | 快捷设置 |
| M | Menu | 侧边栏；应用画面里长按 0.55s 回桌面 |
| H / Home | Home | 应用画面回桌面；主页聚焦最近运行 |

提示栏显示时其他键照常生效。短按 Select 不触发提示。

## 后台应用

- 主页对一个磁贴按 A：从磁贴矩形放大到全屏（420ms，`cubic-bezier(.22,1,.36,1)`），然后显示模拟程序画面。
- 在程序画面按 H / Home，或长按 M：程序画面缩回磁贴（420ms），程序进入“运行中”，墙自动聚焦并平移到该磁贴。
- 对运行中的磁贴按 A：从最后一帧直接放大恢复，不重新启动。
- 对运行中的磁贴按 X：确认后磁贴 180ms 关闭，程序从后台移除。
- 顶栏 `N 运行中` chip，快捷面板“正在运行”列表可集中查看；A 恢复、X 结束。
- 后台硬上限 3，内存软上限 560MB；超出时弹“后台已满 / 内存不足”，默认焦点在取消。

## 截图参数

在 URL 后加参数，复现固定画面。截图前建议加 `capture=1&instant=1`，`capture` 隐藏外部测试栏并固定 1×，`instant` 关闭过渡。

| 参数 | 作用 |
| --- | --- |
| `capture=1` | 隐藏浏览器测试栏，舞台固定 1024×768 |
| `instant=1` | 关闭所有过渡 |
| `theme=dark` / `theme=light` | 主题 |
| `accent=persimmon\|coral\|teal\|blue\|green\|purple` | 强调色 |
| `count=8\|60\|500` | 条目数量 |
| `focus=<id>` | 主页焦点，例如 `music`、`town`、`settings`、`app-9` |
| `tab=0..3` | 全部 / 游戏 / 应用 / 收藏 |
| `hint=1` | 显示按住 Select 的提示栏 |
| `quick=1&qi=0` | 打开快捷面板，`qi=0` 聚焦第一行/第一个后台程序 |
| `side=1&si=0` | 打开侧边栏 |
| `screen=settings&cat=0&zone=opts&oi=1` | 设置页、外观分类、强调色行 |
| `running=town,app-9,music` | 预置后台程序 |
| `launch=music&freeze=.46` | 冻结启动动画中间帧；`freeze` 0–1 |
| `app=town` | 直接显示模拟程序画面 |
| `dialog=end&id=town` | 显示结束进程确认 |
| `time=22:26` | 固定时钟 |

示例：

```text
prototype/v3/index.html?capture=1&instant=1&focus=music
prototype/v3/index.html?capture=1&instant=1&theme=light&focus=music
prototype/v3/index.html?capture=1&instant=1&launch=music&freeze=.46
```

## 文件

- `css/tokens.css`：颜色、强调色派生、字体、单元尺寸。
- `css/base.css`：舞台、环境底图、墙、边缘。
- `css/tiles.css`：磁贴、压暗、角标、封面内标题、运行中状态。
- `css/live.css`：正在播放磁贴的歌词、进度、暂停键。
- `css/chrome.css`：顶栏、提示栏、状态。
- `css/panels.css`：侧边栏、快捷面板、正在运行列表。
- `css/settings.css`：设置页。
- `css/overlays.css`：Toast、外部测试栏。
- `css/app.css`：启动/缩回动画、程序画面、结束确认。
- `js/data.js`：60 项目录、强调色、设置数据。
- `js/layout.js`：4 行、向右生长的确定性密集排布。
- `js/state.js`：状态、主题/强调色、环境底图、调试。
- `js/query.js`：截图参数。
- `js/tiles.js`：磁贴 HTML 与动态内容。
- `js/home.js`：墙 DOM、空间焦点、虚拟化。
- `js/run.js`：后台应用状态机、启动/返回/恢复。
- `js/dialog.js`：结束进程/容量确认。
- `js/input.js`：键盘映射、按住 Select、鼠标选择。
- `js/harness.js`：8/60/500 与强调色测试栏、时钟/歌词 ticker。
- `js/app.js`：屏幕编排与 boot。
- `docs/research/design.md` 第 9 节：完整规格和 C/GLES2 实现说明。

## 说明

- 原型是设计稿，不是最终 C 实现；“程序画面”是预生成 JPEG，最后一帧/共享内存/SIGSTOP 的行为按 `docs/research/architecture.md` 第 5 节记录在 `design.md` 中。
- 调试数字（DOM / IMG / 源 / 总数 / 墙宽 / 尾格）在外部测试栏右侧；`capture=1` 时隐藏。
- 8 项模式用 8 个 2×2 刚好填满 8×4，用来验证零空洞和虚拟化；它不展示混合跨度，默认 60 项才是主要视觉状态。
