# CUI 静态点阵样例（快捷面板后台内存条）

这是第一批只读样例，用来判断 8×8 Bayer 点阵是否适合 brickdesk 的快捷面板内存条。产品没有接入：`src/`、shell、settings、menu、CMake 和既有截图基线均未修改。

## 运行

先准备一次 host 构建（如果还没有 `build-host/`）：

```sh
scripts/build-host.sh
```

然后构建样例、渲染 14 张 1024×768 图、跑像素/布局/静止验证并生成对比图：

```sh
prototype/cui/run.sh
```

也可以指定输出目录：

```sh
prototype/cui/run.sh /tmp/cui-samples
```

工具通过 `prototype/cui/build.sh` 直接链接 `build-host/` 里的 `brickdesk_home/core/...` 静态库，不新增 CMake target。渲染强制使用 Mesa 的 `MESA_GLES_VERSION_OVERRIDE=2.0`，即真实 GLES2 + GLSL ES 1.00 上下文；这不是 GE8300 设备测量。

## 样例内容

基底是真实 1024×768 快捷面板：墙、顶栏、面板标题、正在播放卡、字体、颜色、三张真实封面和 fixture 运行列表都由 `bd_home` / `bd_gfx` 画出。只有“正在运行”以下的内存条、三行运行项和底部控件由样例重画，以便给加高后的点阵留出真实布局空间。

| 文件 | 内容 |
| --- | --- |
| `00_current_506.png` | 现状：366×2 实心条，506 MB |
| `00b_current_unknown.png` | 现状：未知内存（空条，和 0 无法区分） |
| `01`–`04_cell2_*` | 2 px 格，10 px 条（5 行），0 / 506 / 1024 / unknown（25% 中性提案，已否决） |
| `05`–`08_cell3_*` | 3 px 格，12 px 条（4 行），0 / 506 / 1024 / unknown（25% 中性提案，已否决） |
| `09`–`12_cell4_*` | 4 px 格，16 px 条（4 行），0 / 506 / 1024 / unknown（25% 中性提案，已否决） |
| `compare_bar_1x.png` | 现状 + 2/3/4 px 在 506 MB 的同尺度（1:1）对照 |
| `compare_values_1x.png` | 现状 unknown + 3 px 的 0/506/1024/unknown 同尺度对照 |
| `compare_panels_1x.png` | 四张完整面板并排，1:1 |
| `zoom_grid_4x.png` | 2/3/4 px 条放大 4× 最近邻，仅用于看点阵网格 |

“常用”取 fixture 的 210+210+86 = **506 MB**（49.4%）。100% 是 1024 MB；unknown 是其中一项 RSS 采样失败（-1）。

## 实现

- `cui_dither.c/.h`：CUI commit `86d9781bbc3c35c264ec46287ad89c1d86a4c4de` 的 8×8 Bayer 阈值 C 移植。每格烘一个 RGBA texel，点亮格为强调色（unknown 样例的 25% 中性 `ink3` 提案已否决；产品保持空条），空格透明；纹理用 `BD_TEXTURE_NEAREST` 按整数倍画到条上，不在 ES3 shader 里做位运算。
- `cui_samples.c`：真实 `bd_home` 面板 + 样例运行区。`bar_h` 增加后，行起点为 `204 + bar_h`，保留 8 px 间距；底部控件随之下移。
- 条几何：2 px 格 366×10（183×5 texels，3.6 KB）；3 px 格 366×12（122×4，1.95 KB）；4 px 格 364×16（91×4，1.46 KB）。4 px 用 364 宽是为了让 91 个格全部是精确 4 px；它比 366 左右各窄 1 px。2/3 px 保持产品 366 宽。
- 样例在每次 value/尺寸切换时主动重烘一次并通过 `texture_update` 上传；这是工具路径，不是产品缓存。`verify_idle` 只推进 `bd_home_update` 和 `bd_gfx_begin/end_frame`，不调用样例绘制或烘焙；因此它验证的是已稳定的 host 场景不会 present/damage，不证明产品侧的值/主题失效、重烘抑制或缓存接入。

## 验证

`prototype/cui/run.sh` 会自动跑：

- GLES2：`gl.txt` 记录 `OpenGL ES 2.0` / `GLSL ES 1.0.16`，无新 shader。
- 数值边界：0、506、1024、unknown，以及烘焙层的 density clamp（>1 / <0）自检。
- 圆角：100% 时检查四角不出现点阵色；条内直段实心。
- 网格：独立 Python 重算 Bayer 掩码，逐格检查中心颜色；内部格必须是 `cell × cell` 的纯色块，行/列步进与条原点对齐。
- 真实布局：面板 x=622、w=402；条 y=196；行起点与控件位置；条底到行起点 >= 8 px；控件不越界。
- 静止零重绘（有限范围）：每张图后连续 120 帧只调用 `bd_home_update` + `bd_gfx_begin/end_frame`，不调用样例 render/bake；`idle_presents=0`、`idle_draw_calls=0`、`idle_damage_rects=0`、`idle_bakes=0`。这证明已稳定的 host 场景在无输入时不申请帧，不等同于产品接入后的缓存/失效测试。`manifest.tsv` 和 `verify.log` 是证据。

结果：14/14 张通过；两次独立运行（一次 ES2 override、一次默认上下文）的 PNG SHA-256 完全一致；`ctest` 43/43 通过。

## 产品偏好（协调已确认）

- **3 px 格 / 12 px 条**：1× 下能看出方点，条高只比现状增加 10 px，行起点 216；比 2 px 的细网更接近 CUI 的点阵感，又没有 4 px 抢内容。作为首选。
- **unknown 保持空条 + 现有“后台内存 未知”文字**，不引入 25% 中性点阵；该方案已评估并否决，样例里的 25% 图仅保留为备选证据。
- 4 px / 16 px 只在想要更强的 CUI 2× 方点感时考虑：行起点 220，且为了整格宽度用 364 px；2 px / 10 px 在 1× 下更像纸纹，不建议。
- 不建议把点阵放到文字、图标、焦点环或 3 px 以下的既有进度条上。

## 边界

- 这是样例，不是产品实现。`bd_home` 的 `BD_HOME_QUICK_ROWS_START` 仍是 204，内存条仍是 366×2；接入时需要给 home 增加一个布局/样式字段（或改常量）、把烘焙纹理纳入 home 的缓存，并只在值变化时 damage 条和必要行。
- 样例用 `bd_home` 的真实面板作底，再用同色面板背景覆盖运行区来演示更高布局；它不是最终的 `draw_quick()` 代码。
- 没有 GE8300 / 真机帧时间。这里的 120 帧零重绘只证明已稳定的 host 场景在无输入时不申请帧，不是产品缓存/失效路径，也不是 GPU 驱动性能。
- 4 px 的 364 px 宽度是刻意的网格取舍；如果产品必须严格 366，需要接受最后一格 2 px 裁切或改条宽。
- 可选缺封面磁贴没有做；按本轮优先级只交付内存条。

## 来源与许可

CUI：https://github.com/yappologistic/CUI ，commit `86d9781bbc3c35c264ec46287ad89c1d86a4c4de`，MIT。只借鉴 8×8 Bayer 阈值与“整格亮/灭 + 圆角裁切”的视觉规则；没有搬 CUI 的组件、主题或 ES3 shader。许可与出处见 `NOTICE.md`。
