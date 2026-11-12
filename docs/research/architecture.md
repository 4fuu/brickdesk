# brickdesk 架构调研与设计

- 状态：调研稿，M1 开发前
- 日期：2026-11-12
- 目标设备：TrimUI Brick Pro（Allwinner A133P，PowerVR GE8300，1 GB LPDDR3，1024×768@60 Hz）
- 范围：插件、全局组合键、通用应用内菜单、性能配置、配置系统、测试与 CI 基建。本文只做设计与验证记录，不包含产品代码。

---

## 0. 结论摘要

| 问题 | 推荐方案 | 主要备选 | 核心理由 |
|---|---|---|---|
| 插件架构 | 独立进程 + 版本化 JSON IPC + 宿主渲染的声明式 UI；动态磁贴用共享内存 RGBA | 动态库 `.so`；嵌入 Lua | 崩溃隔离、任意语言、可做设置页/后台服务；1 GB 内存下按需启动和 LRU 足够 |
| 全局组合键 | 常驻 `brickdesk-inputd` 被动读 evdev；菜单打开时 `EVIOCGRAB`；需要抑制时可选 grab+uinput 转发 | 每个应用自己读；SDL 手柄层 | 不抢游戏按键的默认路径最简单；输入只有一个读者，状态机集中 |
| 应用内菜单 | 优先 `LD_PRELOAD` 进程内叠加层，用前台应用自己的 GL 上下文画在它的后缓冲上；协议挂起+显示交接作兜底 | 暂停+显示交接；嵌入静态库 | 无合成器，跨进程真叠加不可行；进程内叠加不需要交接触发上下文丢失 |
| 性能配置 | 单个 root 守护进程 `brickdesk-perfd`，白名单 + 校验 + 租约/看门狗；全局/应用/核心三级 profile | 启动器直接写 sysfs；`cpufrequtils`/`tuned` | 单一写者、崩溃可恢复、可审计；避免多个进程互相覆盖 |
| 配置系统 | INI（inih）做人读配置，JSON 做 manifest/IPC；SD 卡分层 + 原子写 + schema 迁移 | TOML；全 JSON | C 依赖最小、可注释、section 天然对应 profile；JSON 适合机器数据 |
| 测试与 CI | Docker 官方工具链交叉编译 + 宿主机 SDL2/GLES2 无头构建；llvmpipe 截图逐像素对比；GitHub Actions 双构建 | 只做设备测试；只做宿主机测试 | 大部分回归在秒级完成；设备只做冒烟和性能 |

一句话：**插件进程化、UI 声明化、输入集中化、性能单写者、配置分层、测试双轨。**

---

## 1. 设备与约束

### 1.1 硬件与系统事实

| 项目 | 事实 | 对设计的影响 |
|---|---|---|
| SoC | Allwinner A133P（sun50iw10 家族），4×Cortex-A53，最高约 1.8 GHz | 性能配置以 CPU 为主；GPU 调频不一定有公开接口 |
| GPU | Imagination PowerVR GE8300，最高约 660 MHz | 只有 GLES2 可用；不要依赖计算着色器、GLES3 特性 |
| 内存 | 1 GB LPDDR3 | 桌面与常驻进程预算要小；插件按需启动；大图有上限 |
| 屏幕 | 1024×768，60 Hz，4:3 | 所有布局按 1024×768 设计；移植到其他分辨率要缩放 |
| 显示栈 | EGL 1.4 + `libpvrNULL_WSEGL.so`（NULL window system），无 GBM/Wayland/X11 | 没有合成器；同一时刻基本只有一个进程能拥有显示 |
| SDL | 官方 `SDL2-2.26.1.GE8300`，`mali-fbdev` 视频驱动 | 必须用这个驱动；不要用 X11/Wayland 后端 |
| 系统 | OpenWrt 风格 Linux，root，ADB | 进程管理不能用 systemd；需要自带 supervisor/启动脚本 |
| 工具链 | 官方 `aarch64-linux-gnu-7.5.0-linaro`（GCC 7.5），SDK 20231018 | 只能用 C11/C++14；不能依赖新 libc 符号 |
| 目标 glibc | SDK 内是 `libpthread-2.23.so`；设备实际版本待 `ldd --version` 确认，按 2.23 保守处理 | 交叉编译产物必须检查 `GLIBC_*` 版本不超过 2.23 |

> 设备具体 sysfs 路径、背光/音量节点、按键码由另一条调研线确认。本文只写设计与探测项，不把未验证的路径当成事实。

### 1.2 三个会直接影响架构的发现

**（1）官方 SDK 的 SDL2 把窗口硬编码成 1280×720。**

`SDL2-2.26.1.GE8300.tgz` 里的 `src/video/mali-fbdev/SDL_malivideo.h` 定义了 `SDL2_POWERVR_GE8300`，`MALI_CreateWindow()` 中：

```c
#ifdef SDL2_POWERVR_GE8300
    window->w = 1280;
    window->h = 720;
#endif
```

预编译的 `libSDL2-2.0.so.0.2600.1` 里也能看到同样的常量（`MALI_CreateWindow` 先 `mov x0,#0x500`，再 `movk x0,#0x2d0,lsl#32`，把 0x000002d000000500 写到窗口结构）。这套 SDK 是给 1280×720 的 Smart Pro 的，Brick Pro 是 1024×768。直接使用会出现逻辑分辨率、EGL surface 尺寸和面板不一致，轻则画面被拉伸，重则只显示左上角或黑屏。

处理方式（M1 必须做）：

1. 不用预编译 SDL2，改为从 `SDL2-2.26.1.GE8300.tgz` 源码构建；
2. 打一个小 patch：`MALI_VideoInit()` 从 `/dev/fb0` 的 `FBIOGET_VSCREENINFO` 读 `xres/yres`，填入 `struct fbdev_window`，`MALI_CreateWindow()` 用这个尺寸创建 EGL surface，不再硬编码；
3. 在设备上跑 `display-probe`，确认 `SDL_GetWindowSize()`、`eglQuerySurface()`、面板实际输出三者一致。

> 如果 SDK 的 EGL 头把 `EGLNativeWindowType` 定义为 `void*`（本 SDK 就是），patch 要在 `SDL_malivideo.h` 本地定义 `struct fbdev_window { unsigned short width; unsigned short height; }`，把 `SDL_DisplayData.native_display` 改成该结构，并传 `&native_display` 给 `SDL_EGL_CreateSurface()`。不要继续传 NULL。

> 这也是“为什么要有设备侧冒烟测试”的最好例子：宿主机测试永远发现不了这类问题。

**（2）没有合成器，跨进程“真叠加”不可行。**

EGL 只有 1.4，只有 NULL WSEGL；`/dev/dri/card0` 存在但驱动不能消费 GBM buffer。显示由 PowerVR 的 Display Class 接口负责，`eglSwapBuffers()` 就是提交/翻转。没有 X/Wayland compositor，也没有可用的硬件 overlay 层给普通进程。

因此“在别的 SDL 程序上面画一层半透明菜单”不能靠再开一个窗口实现。可行路径只有：

- 在**前台应用自己的 GL 上下文里**画（进程内叠加，`LD_PRELOAD` 或静态库）；
- 或者让前台应用**释放显示**，由桌面/菜单进程接管屏幕（暂停+交接）。

后文的通用应用内菜单按这个事实设计。

**（3）目标 glibc 按 2.23 保守处理，交叉工具链的 sysroot 是 2.25。**

官方工具链里的 `libc.so.6` 带 `GLIBC_2.25`，设备 SDK 里的 `libpthread` 是 `libpthread-2.23.so`，因此按目标 2.23 保守处理（最终以设备 `ldd --version` 为准）。我们的探针二进制目前只引用了 `GLIBC_2.17`，所以安全；但一旦用了较新的 API（`statx`、`getrandom`、新的 `pthread` 符号等），就可能产生设备上不存在的版本依赖。

CI 必须加一步：

```sh
aarch64-linux-gnu-readelf --version-info brickdesk \
  | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1
# 必须 <= GLIBC_2.23
```

### 1.3 设计原则

1. **单一写者**：CPU/GPU/背光/音量等硬件状态只由一个守护进程写，避免桌面、插件、游戏互相覆盖。
2. **静止即阻塞**：主循环用 `poll()`/`SDL_WaitEventTimeout()` 阻塞，没有动画就不交换缓冲；脏矩形只重绘变化区域。
3. **声明式 UI**：插件不直接画像素（动态磁贴除外），由宿主统一渲染，保证主题、焦点、性能一致。
4. **崩溃隔离**：第三方代码不进入桌面地址空间；插件崩溃只影响自己的磁贴/菜单项。
5. **默认可恢复**：性能配置、菜单、输入都有超时、看门狗和回滚；应用崩溃不能把设备留在高温/高频状态。
6. **双轨可测**：同一份 C 代码在宿主机（SDL2 + Mesa llvmpipe）和目标机（SDL2 mali + GLES2）上构建；绝大多数回归在宿主机完成。

---

## 2. 进程模型

### 2.1 进程清单

| 进程 | 职责 | 生命周期 | 估算内存 |
|---|---|---|---|
| `brickdesk` | 桌面 UI、封面墙、设置、插件 UI 宿主 | 前台常驻；启动应用时释放显示并退居后台 | 35–60 MB（待实测） |
| `brickdesk-inputd` | 读 evdev、组合键/长按识别、菜单期间 grab、转发菜单输入 | 开机到关机，始终运行 | 2–4 MB |
| `brickdesk-session` | 启动/等待应用、应用能力握手、暂停/恢复、状态槽、菜单编排 | 开机常驻；很小 | 2–3 MB |
| `brickdesk-perfd` | 唯一拥有 sysfs 写权限的性能守护进程 | 开机到关机 | 2–3 MB |
| `brickdesk-plugin-<id>` | 第三方插件 | 按需/常驻，LRU 回收 | 3–20 MB/个 |
| `libbdmenu` | 菜单渲染库，`LD_PRELOAD` 进前台应用或静态链接 | 跟随应用 | +2–4 MB |

> 内存数字是工程预算，不是设备实测。M1 要在设备上用 `/proc/<pid>/status` 的 `VmRSS` 校准，并把结果写回本文。

`brickdesk-menud` 不作为默认常驻进程。它只在“暂停+显示交接”兜底模式下以 `brickdesk --menu` 的形式短命启动，复用同一套渲染代码。

### 2.2 启动流程

```text
init
 ├─ brickdesk-perfd        (root, 先起，保证默认 profile 已应用)
 ├─ brickdesk-inputd       (root, 打开 evdev)
 └─ brickdesk              (UI)
      ├─ brickdesk-session  (空闲)
      └─ brickdesk-plugin-* (按需)
```

启动脚本要兼容 OpenWrt 的 `procd` 和简单 SysV 两种形式；M1 先提供 `scripts/init/S99brickdesk`，设备侧确认 init 体系后再包成 procd 服务。

### 2.3 启动一个应用

```text
brickdesk          session            perfd             app
   |  launch(id) -->|                  |                 |
   |               |-- read app.json ->|                 |
   |               |-- apply(profile) ->|                 |
   |               |<-- ok ------------|                 |
   |-- release EGL surface/context --->|                 |
   |               |-- fork/exec app ------------------->|
   |               |-- waitpid ------------------------->|
   |               |-- restore(default) ->|              |
   |<-- app exited ---------------------|                 |
   |-- recreate EGL surface/context --->|                 |
```

关键点：

- 桌面在 `exec` 前销毁自己的 EGL surface 和 context，避免和游戏争显示；
- 应用运行期间桌面进程仍在，但不持有 GL 资源；
- 应用退出后桌面重建 surface；所有 GL 对象（纹理、shader、字体 atlas）必须可重建，不能假设 context 永久有效；
- 应用崩溃由 `waitpid`/`pidfd` 发现，`perfd` 在租约到期后自动恢复默认 profile。

### 2.4 静止时 CPU 为 0 的实现

- 主循环阻塞在 `poll()`（输入 fd、socket、timerfd）上，不用固定 60 Hz 轮询；
- 只有动画/滚动/视频磁贴才启动帧时钟；动画结束立即回到阻塞；
- 渲染到一张持久的全屏纹理/FBO，脏矩形更新；没有脏矩形就不 `eglSwapBuffers`；
- 交换后保留最后一帧；不要释放正在扫描输出的 buffer；
- 如果设备支持 `EGL_KHR_swap_buffers_with_damage`，传 damage rect；不支持就整屏交换，但 CPU 只重绘脏区；
- 静态界面（设置页、菜单）不跑定时器；时钟一分钟一次，电量变化才更新。

宿主机验证：一个只创建 SDL 窗口、每 16 ms 处理一次事件、不重绘的循环，2 秒内 CPU 时间为 0.000 ms（见第 8.8 节）。设备上要用 `perfd` 或 `top` 复测。

### 2.5 渲染器与脏矩形

目标：1024×768、60 Hz 下，静止 0% CPU；动画时 CPU 预算 4–8 ms/帧（A53，待实测）。

结构：

```text
场景树（retained） -> 布局 -> damage list（矩形）
       -> 只对 damage 区域画到持久 FBO
       -> 交换时把整张 FBO 画到 back buffer（一个全屏 quad）
       -> eglSwapBuffers（有 damage 扩展则传矩形）
```

要点：

- **持久 FBO**：一张 1024×768 RGBA 纹理（约 3 MB），保存当前完整画面；脏区更新只改这块纹理，交换时整屏 blit。这样不需要 `EGL_BUFFER_PRESERVED`，也不怕双缓冲内容过期。
- **脏矩形合并**：多个小矩形先合并/扩张到 4 像素对齐，减少 draw call；重叠的合并；全屏变化直接走全屏路径。
- **批处理**：同一纹理/同一 shader 的 quad 合并成一个 draw call；顶点写进预分配的 VBO，帧内不 `malloc`。
- **文本**：FreeType 按需光栅化，glyph 缓存在一张 R8 atlas；CJK 只缓存用到的字；atlas 满了按 LRU 淘汰或开第二张。不要启动时把 2 万字全部光栅化。
- **图片**：JPEG/PNG 在 worker 线程解码，渲染线程只做 `glTexSubImage2D`；封面缓存 96 MB 上限，LRU；不可见的不解码。
- **状态**：按“纹理 -> shader -> 混合”排序，减少 `glUseProgram`/`glBindTexture`；不要每帧重建资源。
- **动画**：只有动画/滚动时才启动 60 Hz 帧时钟；动画结束回到 `poll()` 阻塞；掉帧时跳帧而不是积压。
- **测试**：`glFinish()` 只在截图/基准模式用；正常渲染不等待 GPU。
- **测量**：`clock_gettime(CLOCK_MONOTONIC)` 统计 CPU 帧时间；GPU 时间如果没有 `EXT_disjoint_timer_query` 就用帧率/温度间接判断；不要把 `eglSwapBuffers` 的阻塞时间算成 CPU。

这套结构让“只重绘变化区域”和“静止 0% CPU”同时成立：脏区决定 CPU 工作量，是否交换决定 GPU/显示工作量。

---

## 3. 插件架构

### 3.1 三种方案对比

| 维度 | 独立进程 + IPC | 动态库 `.so` | 嵌入 Lua |
|---|---|---|---|
| 内存（每插件，估算） | 3–20 MB（C/Shell 低，Python 高） | 0.2–2 MB | 0.3–1 MB |
| CPU/调用开销 | 一次 IPC + JSON 约 20–100 µs（A53） | 函数调用，<1 µs | 1–10 µs + GC |
| 崩溃隔离 | 有；插件崩溃不影响桌面 | 无；段错误直接带走桌面 | 无；Lua 错误可 `pcall`，C 模块崩溃仍带走 |
| 开发门槛 | 任意语言，只要会说 JSON；SDK 可选 | 必须用官方 GCC 7.5 交叉编译，C ABI 锁定 | 必须学 Lua + 宿主绑定 |
| 动态磁贴 | 可；共享内存 RGBA，15–30 fps | 最好；可直接用 GL | 可以，但每帧过 Lua/FFI 不划算 |
| 菜单项/设置页 | 可；声明式 UI 树由宿主渲染 | 可；但直接操作 UI 内部结构，ABI 脆弱 | 可；需要大量绑定 |
| 后台服务 | 最适合；独立生命周期、可限内存 | 跟桌面同生共死 | 不适合长跑/阻塞 |
| 安全性 | 可降权运行、cgroup/rlimit、只能访问自己的 socket/数据目录 | 与桌面同权限（root） | 与桌面同权限 |
| 安装/升级 | 解压目录，重启插件即可 | 需要匹配宿主 ABI，升级易崩 | 拷贝脚本，简单 |
| 适合场景 | 第三方插件默认 | 第一方高性能渲染/背景 | 用户规则、简单自动化 |

### 3.2 参考系统

| 系统 | 扩展单元 | 进程模型 | 宿主提供什么 | 局限/启示 |
|---|---|---|---|---|
| NextUI Paks | `*.pak/launch.sh`，Tools/Emus 目录 | 独立进程 | `PLATFORM`/`DEVICE`/`SDCARD_PATH`/`LOGS_PATH` 等环境变量；启动前后切 governor | 极低门槛，生态大；只能全屏接管，没有磁贴/设置页 API |
| muOS | `.muxapp`（zip）+ `mux_launch.sh` | 独立进程 | 统一 wrapper 处理 governor、HOME、SD 挂载、前后台状态；manifest 头 | 统一 wrapper 解决环境一致性；仍然没有宿主 UI 协议 |
| OnionOS | `App/<name>/config.json` + `launch.sh` | 独立进程 | Package Manager 安装；主题/图标资源 | 简单直接；配置和 UI 能力弱 |
| Batocera / Knulli | Ports：`roms/ports/*.sh`；ES 前端 | 独立进程 | `batocera-settings` 等 CLI；EmulationStation 负责展示 | CLI 是稳定集成面；前端和扩展解耦 |
| EmulationStation 主题 | XML + 图片/字体资源 | 数据，无代码 | 视图、颜色、字体、九宫格 | 纯数据扩展安全、生态大；说明“展示”和“逻辑”要分开 |

结论：

- **进程式扩展在这些设备上是事实标准**，因为门槛低、崩溃隔离、任何语言都能写；
- 它们普遍缺少“宿主 UI 协议”，所以插件只能全屏；我们要补的正是这一层；
- 主题应该是纯数据，不应该允许代码；插件才允许代码；
- 统一的 launch wrapper（muOS 的做法）值得借鉴：环境、governor、日志、HOME 都由宿主统一处理。

### 3.3 推荐方案

**进程外插件（pak）+ 版本化 JSON 协议 + 宿主渲染的声明式 UI + 共享内存动态磁贴。**

不推荐把 `.so` 作为第三方默认方案：

- 官方工具链是 GCC 7.5，目标 glibc 按 2.23 保守处理（工具链自带 sysroot 是 2.25），C ABI/结构体布局一旦变化就要重编所有插件；
- 插件段错误会带走整个桌面；
- 插件以 root 运行，没有安全边界；
- 1 GB 内存下，多一个进程的成本可以接受，换来的隔离和语言自由更值。

也不推荐把 Lua 作为插件主模型：

- 崩溃隔离没有；
- 动态 UI 需要大量 C 绑定；
- 后台服务/阻塞 IO 不适合；
- 但 Lua 可以作为**用户规则**的表达式层（例如“当应用是 X 且电量 < 20% 时切到 powersave”），M3 再考虑，且要沙箱化。

保留一个第一方例外：**渲染模块**。如果以后要做 60 fps 的动态背景/粒子，可以由第一方、与宿主同版本编译的 `.so` 提供，不开放给第三方。这不算插件模型的一部分。

### 3.4 包格式

```text
/mnt/SDCARD/brickdesk/plugins/com.example.music/
  plugin.json
  bin/music              # 可执行文件，或脚本
  assets/icon.png
  assets/tile.png
```

`plugin.json`：

```json
{
  "schema": 1,
  "id": "com.example.music",
  "name": "音乐",
  "version": "0.1.0",
  "api": 1,
  "entry": ["bin/music", "--socket", "{socket}"],
  "runtime": "native",
  "autostart": false,
  "capabilities": ["tile", "menu", "settings", "hotkey", "background"],
  "permissions": ["files.read:music", "network"],
  "tile": { "w": 1, "h": 2, "refresh_hz": 10 },
  "menu": [
    { "id": "now-playing", "label": "正在播放", "icon": "assets/icon.png" }
  ],
  "hotkeys": [
    { "id": "toggle", "combo": "L1+Play", "label": "播放/暂停" }
  ],
  "settings": [
    { "key": "server", "type": "string", "label": "服务器", "default": "" },
    { "key": "quality", "type": "enum", "label": "音质",
      "values": ["low", "high"], "default": "high" }
  ]
}
```

字段说明：

- `id`：反向域名，全局唯一；安装目录名必须与 `id` 一致（防止路径混淆）。
- `api`：协议主版本；宿主只加载 `api == 1` 的插件。
- `entry`：argv 数组；占位符 `{socket}`、`{plugin_dir}`、`{data_dir}`、`{cache_dir}` 由宿主替换。不要用 shell 字符串拼接，避免注入。
- `runtime`：`native`/`python`/`shell`/`lua`，只用于展示和依赖检查，不限制实现。
- `capabilities`：声明需要哪些宿主能力；安装时展示给用户。
- `permissions`：`files.read:<subdir>`、`files.write:<subdir>`、`network`、`audio` 等；M1 只做展示和基本限制，M3 再做细粒度。
- `tile`：磁贴占用的格数（1×1、2×1、1×2、2×2）；`refresh_hz` 是上限，宿主只在前台可见时允许。
- `menu`/`hotkeys`/`settings`：声明式贡献，不需要插件写 UI 代码。

### 3.5 IPC 协议

传输：Unix `SOCK_STREAM`，路径由宿主通过 `BRICKDESK_PLUGIN_SOCKET` 传给插件；宿主 `listen`，插件 `connect`。用 4 字节小端长度 + UTF-8 JSON；单条消息上限 1 MB。选择 `SOCK_STREAM` 而不是 `SOCK_SEQPACKET`，是为了避免 64 KB 消息上限，同时实现简单。

帧格式：

```text
+--------+---------------------------+
| u32 len| JSON (UTF-8, len bytes)   |
+--------+---------------------------+
```

消息采用 JSON-RPC 2.0 子集：

- 请求有 `id`，响应 `result` 或 `error`；
- 通知没有 `id`；
- 超时由调用方设置；插件必须在 2 秒内完成握手。

宿主 → 插件：

| 方法 | 说明 |
|---|---|
| `host.hello` | 握手：`api`、`display`、`theme`、`locale`、`paths`、`session_id` |
| `host.event` | `tile.visible`、`menu.activate`、`settings.changed`、`hotkey`、`theme.changed`、`system.suspend`、`system.resume` |
| `host.shutdown` | 请求插件退出；3 秒后 SIGTERM |
| `host.action` | 宿主调用插件声明的动作（菜单/磁贴/热键） |
| `host.ping` | 心跳，5 秒一次 |

插件 → 宿主：

| 方法 | 说明 |
|---|---|
| `plugin.ready` | 握手回复：`pid`、`api`、实际能力 |
| `plugin.ui.set` | 设置整棵 UI 树（M1 只支持整树，树小于 48 KB） |
| `plugin.ui.patch` | JSON Patch 子集（M2） |
| `plugin.tile.acquire` | 申请共享内存：`w`、`h`、`format`，宿主回 `fd`、`stride`、`seq_offset` |
| `plugin.tile.release` | 归还共享内存 |
| `plugin.tile.frame` | 通知宿主“第 seq 帧已画好” |
| `plugin.settings.get` / `plugin.settings.set` | 读写插件配置 |
| `plugin.toast` | 请求宿主显示一条提示 |
| `plugin.log` | 结构化日志：`level`、`msg`、`fields` |

示例握手：

```json
{"jsonrpc":"2.0","id":1,"method":"host.hello","params":{
  "api":1,
  "display":{"w":1024,"h":768,"scale":1.0},
  "theme":{"name":"default","dark":true,"accent":"#3b82f6"},
  "locale":"zh-CN",
  "paths":{"data":"/mnt/SDCARD/brickdesk/plugin-data/com.example.music",
           "cache":"/mnt/SDCARD/brickdesk/cache/com.example.music"},
  "session_id":"7f3c2a"
}}
```

```json
{"jsonrpc":"2.0","id":1,"result":{"pid":1234,"api":1,
  "capabilities":["tile","menu","settings","hotkey","background"]}}
```

错误码：

| code | 含义 |
|---|---|
| -32600 | 非法请求 |
| -32601 | 方法不存在 |
| -32602 | 参数错误 |
| -32001 | 不支持的能力 |
| -32002 | 忙/暂时不可用 |
| -32003 | 超时 |
| -32004 | 权限拒绝 |
| -32005 | 内部错误 |

### 3.6 声明式 UI

宿主只认一组固定的节点，插件不能自定义绘制、字体或任意颜色。这样宿主可以统一焦点、主题、动画和脏矩形，也能保证 0 成本的空闲。

节点类型：

| 类型 | 用途 | 关键属性 |
|---|---|---|
| `column`/`row` | 布局 | `padding`、`gap`、`align`、`grow` |
| `text` | 文本 | `text`、`style`（`title`/`body`/`caption`/`mono`） |
| `image` | 图片 | `src`（插件目录相对路径或 `asset:`）、`fit` |
| `button` | 可点项 | `label`、`icon`、`action`、`enabled` |
| `toggle` | 开关 | `label`、`value` |
| `slider` | 滑条 | `label`、`value`、`min`、`max`、`step` |
| `select` | 枚举 | `label`、`value`、`options` |
| `progress` | 进度 | `value`、`max` |
| `list` | 列表 | `items`、`selected` |
| `spacer` | 占位 | `grow` |

插件/应用可以自带图片和配置（`assets/`、`icon.png`、`theme.ini`），但**由宿主负责解码、缓存、上传纹理和渲染**；插件不直接持有 GPU 资源，也不决定字体/颜色，只引用主题 token 和资源路径。这样大图、缓存和内存预算都在桌面手里。

示例：

```json
{
  "type": "column", "id": "root", "padding": 24, "gap": 12,
  "children": [
    { "type": "text", "id": "title", "text": "音乐", "style": "title" },
    { "type": "text", "id": "now", "text": "夜航 · 林间电台", "style": "body" },
    { "type": "progress", "id": "pos", "value": 42, "max": 100 },
    { "type": "row", "gap": 16, "children": [
      { "type": "button", "id": "prev", "label": "上一首", "icon": "prev", "action": "prev" },
      { "type": "button", "id": "play", "label": "暂停", "icon": "pause", "action": "toggle" },
      { "type": "button", "id": "next", "label": "下一首", "icon": "next", "action": "next" }
    ]}
  ]
}
```

宿主 → 插件事件：

```json
{"jsonrpc":"2.0","method":"host.event","params":{
  "type":"menu.activate","node":"play","event":"activate"
}}
```

M1 只支持 `plugin.ui.set` 整树替换；树通常小于 16 KB，A53 上解析/布局一次在毫秒级。M2 再加 JSON Patch，减少高频更新。

### 3.7 动态磁贴

磁贴是唯一允许插件“画像素”的地方，因为每帧走 JSON 不现实。

流程：

1. 宿主发现磁贴可见，给插件发 `host.event {type:"tile.visible", visible:true}`；
2. 插件发 `plugin.tile.acquire {w:400,h:400,format:"rgba8888"}`；
3. 宿主 `memfd_create()` 两块 buffer（双缓冲），通过 `SCM_RIGHTS` 把 fd 传给插件；内核不支持 `memfd` 时退回 `/dev/shm` 临时文件；
4. 插件在自己的进程里画（CPU 或离屏 GL 都可以），递增 sequence，发 `plugin.tile.frame {seq}`；
5. 宿主在渲染线程 `glTexSubImage2D` 上传，只上传脏区；不可见时停止；
6. 插件发 `plugin.tile.release` 或进程退出时回收。

约束：

- 默认上限 400×400、15 fps；磁贴不可见时 0 fps；
- 每帧 400×400×4 = 640 KB，15 fps = 9.6 MB/s，A53 上拷贝 + 上传约几个百分点 CPU，可接受；
- 需要 60 fps 的动画（例如波形）不做成磁贴，做成插件全屏页面，或由第一方渲染模块实现；
- 静态封面用 `image` 节点，宿主解码/缓存，不走共享内存。

### 3.8 生命周期与资源限制

| 阶段 | 行为 |
|---|---|
| 发现 | 扫描 `plugins/*/plugin.json`；校验 `id`、`api`、entry 存在、路径不越界；坏的记日志并跳过 |
| 启动 | `fork` + `exec`；`cwd=plugin_dir`；`PR_SET_PDEATHSIG`；`RLIMIT_AS=64 MB`、`RLIMIT_NOFILE=64`；`nice=10`；可选 cgroup v2 `memory.max` |
| 握手 | 宿主发 `host.hello`，2 秒内必须 `plugin.ready`；否则 SIGTERM/SIGKILL |
| 心跳 | 5 秒 ping，3 次无响应重启 |
| 空闲 | 无可见 UI、无 `background` 能力，60 秒后停止；配置可调 |
| 崩溃 | 指数退避重启（1s/2s/4s，最多 3 次/分钟）；超过则隔离，磁贴显示“插件未运行” |
| 内存 | 宿主统计 `/proc/<pid>/statm`；插件总 RSS 超过 128 MB 时按 LRU 停掉空闲插件 |
| 升级 | 安装到新目录，原子 rename；旧进程退出后切换；失败回滚 |

### 3.9 安全

参考系统几乎都以 root 跑插件；我们可以在不牺牲能力的前提下做得更好：

- 插件只通过 Unix socket 和宿主通信，不需要 `/dev/fb0`、`/dev/input`、sysfs；
- 宿主以专用 UID（安装时创建 `brickdesk`，没有就退回 `nobody`）启动插件；
- 插件数据目录 `0700`，socket `0660`，组为 `brickdesk`；
- `no_new_privs`，drop capabilities；M3 可加 seccomp allowlist（`read/write/poll/recvmsg/sendmsg/mmap/...`）；
- 所有特权动作（音量、亮度、性能、启动应用）都由宿主代理，插件不能直接写；
- manifest 的 `permissions` 在安装/首次运行时展示；网络权限单独提示；
- 安装包校验：拒绝 `..`、绝对路径、符号链接越界；限制解压后总大小；
- 签名不是 M1 必需；M3 若做插件商店，再加 Ed25519（Monocypher 单文件）和信任链。

这套边界比“root 跑 shell 脚本”强，但也不是强沙箱：同一 UID 的插件之间仍可互相影响。真要强隔离，需要 user namespace/seccomp，M3 再评估。

### 3.10 为什么不是 `.so` 或 Lua（细化）

**.so 的问题：**

- ABI：宿主的结构体、GL 上下文、UI 树生命周期都要暴露；GCC 7.5 与未来工具链升级会反复破坏；
- 崩溃：一个空指针就让桌面重启，用户在游戏里丢进度；
- 安全：插件与桌面同权限，能读 SD 卡任何文件、写 sysfs；
- 内存：省下的几 MB 在 1 GB 设备上不是决定性优势；
- 调试：插件崩溃的 backtrace 和桌面混在一起，现场日志更难。

**.so 的合理用途：**

- 第一方渲染模块（动态背景、粒子、视频输出）；
- 性能关键的图像解码/滤镜；
- 与宿主同版本、同仓库、同 CI 的代码。

**Lua 的问题：**

- 不是隔离边界；`pcall` 只能接住 Lua 错误；
- 动态 UI 需要把布局、焦点、主题都绑定出去，绑定层比 Lua 本身大；
- 长跑/阻塞/网络不适合；
- 1 GB 内存下，Lua VM 本身不大，但每个插件一个 VM + 绑定 + 资源，优势不明显。

**Lua 的合理用途：**

- 用户自定义规则（热键条件、自动切换 profile）；
- 简单的文本转换/脚本动作；
- 由宿主提供受限 API，不允许 `os.execute`/`io` 任意访问。

### 3.11 插件 SDK 草案

C 侧只需要一个很小的客户端库，负责连接、握手、发消息、收消息；UI 树用 JSON 构造。头文件草案：

```c
/* include/brickdesk/plugin.h */
typedef struct bd_plugin bd_plugin;

typedef struct {
    const char *id;
    const char *name;
    const char *version;
    int api;
} bd_plugin_info;

/* 连接宿主；socket 路径从 BRICKDESK_PLUGIN_SOCKET 读取 */
bd_plugin *bd_plugin_connect(const bd_plugin_info *info);
void bd_plugin_disconnect(bd_plugin *p);

/* 主循环：处理宿主消息，超时返回 0 */
int bd_plugin_poll(bd_plugin *p, int timeout_ms);

/* 声明式 UI：整树替换；root_json 必须是合法 JSON */
int bd_plugin_ui_set(bd_plugin *p, const char *root_json);

/* 动态磁贴：申请/提交/释放 */
int bd_plugin_tile_acquire(bd_plugin *p, int w, int h, int *fd, int *stride);
int bd_plugin_tile_commit(bd_plugin *p, unsigned seq);
void bd_plugin_tile_release(bd_plugin *p);

/* 设置、日志、提示 */
const char *bd_plugin_setting(bd_plugin *p, const char *key);
int bd_plugin_toast(bd_plugin *p, const char *text);
int bd_plugin_log(bd_plugin *p, int level, const char *msg);
```

Python/Shell 插件不需要这个库，直接按协议读写 JSON 即可。M1 提供 `plugins/hello`（C）和 `plugins/hello-py`（Python）两个样例，验证语言无关。

---

## 4. 全局组合键

### 4.1 约束

游戏运行时桌面不在前台，但 `brickdesk-inputd` 仍在。组合键要满足：

- 在任意前台应用/游戏里都能触发；
- 不与游戏抢按键（默认不让游戏“少看到”按键）；
- 支持组合、长按、双击、序列；
- 支持内置动作、启动应用、运行脚本、插件动作；
- 配置可读、可校验、能检测冲突；
- inputd 崩溃不能把设备变成“没有按键”。

Linux evdev 的事实：

- 一个 event device 可以被多个进程同时打开；每个读者都会收到同一份事件。**只读监听不会偷走按键**。
- `EVIOCGRAB` 是独占：一个 fd grab 后，其他 fd 不再收到该设备的事件；可以运行时 grab/ungrab。
- SDL 的 `mali` 视频驱动内部用 `SDL_EVDEV_Poll()` 读 `/dev/input`，所以游戏和 inputd 会同时看到按键。
- 如果游戏自己 grab 了设备，inputd 再 grab 会失败（`EBUSY`），此时只能走应用内集成。
- Power/Volume 可能是独立的 `gpio-keys` 设备，也可能和手柄在同一个节点；必须枚举确认。

### 4.2 三种方案

| 方案 | 能否抑制游戏看到按键 | 延迟 | 复杂度 | 风险 | 适用 |
|---|---|---|---|---|---|
| 被动监听（不 grab） | 不能 | 最低 | 低 | 游戏可能同时响应组合键 | 默认；组合键选游戏不用的键 |
| grab + uinput 转发 | 能 | +1–3 ms | 高 | inputd 挂掉会导致游戏暂时无输入；虚拟设备兼容性 | 必须抑制的组合，按应用/配置开启 |
| 应用内集成 | 能 | 最低 | 中 | 需要改应用/模拟器 | 第一方、RetroArch 等 |

推荐：

1. **默认被动监听**。inputd 只读 evdev，识别组合键并执行动作；游戏仍能看到按键。选择 Menu、Select、L1+R1+Start 这类游戏通常不用的组合，避免冲突。
2. **菜单打开时 grab**。此时游戏本来就不应该收到输入；grab 同时解决“菜单按键漏进游戏”和“暂停后事件堆积”的问题。
3. **需要完全抑制时，按应用开启 `exclusive_input`**：inputd grab 物理设备，创建 uinput 虚拟手柄，把除绑定组合外的所有事件转发给游戏。这个模式必须默认关闭，且要在设备上验证虚拟设备的按键码、摇杆、震动、蓝牙。
4. **应用自己 grab 的设备**，inputd 无法读；在应用 manifest 里声明 `input.grabbed=true`，改用应用内集成或放弃该应用的全局热键。

### 4.3 架构

```text
/dev/input/event* ──> brickdesk-inputd ──> 动作分发
                          │  │  │
                          │  │  └─> brickdesk-session (启动应用/返回桌面/挂起)
                          │  └────> brickdesk-perfd (性能 profile)
                          └───────> libbdmenu / brickdesk (菜单/提示)
```

inputd 的职责：

- 枚举设备，按 `EVIOCGNAME`/`EVIOCGID`/`EVIOCGBIT` 识别手柄、键盘、电源键；
- 维护每个设备的按键状态和热键状态机；
- 把归一化事件发给订阅者（桌面、菜单 shim）；
- 菜单打开时 `EVIOCGRAB`，关闭时等所有键释放后 `EVIOCUNGRAB`；
- 执行低风险内置动作（音量、亮度、截图、菜单开关）；高风险动作交给 session/worker；
- 心跳、看门狗、日志。

inputd 不做的事：

- 不做 UI 布局/渲染；
- 不直接启动应用（交给 session）；
- 不直接写性能 sysfs（交给 perfd）；
- 不加载第三方代码。

### 4.4 识别算法

每个设备一个状态机，使用 `CLOCK_MONOTONIC`：

```text
key_down(k):
  pressed |= k
  if k 是某个 chord/sequence 的起始:
      pending = {keys: {k}, start: now}
  if 所有 chord 键在 chord_window(50ms) 内按下:
      fire(chord); pending = none
  else if 单键绑定:
      schedule(single, chord_window)
  if 长按绑定:
      schedule(long, hold_ms)
  if 双击绑定:
      if 上次同键释放 < double_tap_ms: fire(double); else schedule(single_tap)

key_up(k):
  pressed &= ~k
  if pending 且未 fire: 取消 pending
  if long 已 fire: 不再触发短按
  if 所有键释放: reset
```

规则：

- **组合窗口**默认 50 ms；超过则视为先后按，不组成 chord。
- **长按**默认 600 ms；触发一次，不重复；`repeat=true` 的动作（音量）在 400 ms 后每 100 ms 重复。
- **双击**默认 300 ms；如果同时有单击绑定，单击延迟到双击窗口结束再触发。
- **序列**（如 `Select,Select`）默认 800 ms 内完成；超时重置。
- **优先级**：精确 chord > 长按 > 序列 > 单键。冲突在配置加载时检测，运行时按优先级。
- **SYN_DROPPED**：内核事件队列溢出；用 `EVIOCGKEY`/`EVIOCGABS` 重新同步状态，丢弃未完成的 pending，记一条 warning。
- **重复键**：`value=2` 的自动重复事件忽略；由状态机自己的 repeat 控制。
- **多设备**：每个设备独立状态机；同一逻辑键在不同设备上不合并（避免手柄+键盘误触）。跨设备 chord 不支持（M3 再评估）。
- **摇杆**：默认忽略；M3 可做“摇杆方向 + 键”的手势，但要防抖。

### 4.5 冲突检测

加载 `hotkeys.ini` 后构建一棵 trie（按设备/scope 分组），检查：

| 类型 | 例子 | 处理 |
|---|---|---|
| 完全相同 | 两个 `L1+R1+Start` | 错误，拒绝加载后一个，UI 标红 |
| 前缀冲突 | `Menu` 与 `Menu+X` | 警告；`Menu` 的长按与 `Menu+X` 用 chord 窗口/优先级解析 |
| 单键与 chord 冲突 | `Up` 与 `L1+Up` | 单键延迟一个 chord 窗口；若 chord 成立则取消单键 |
| scope 重叠 | 全局 `Menu` 与 app 级 `Menu` | app 级优先；UI 显示覆盖关系 |
| 保留键 | `Power` 短按、`Volume` | 默认不可覆盖；要覆盖必须 `allow_system=true` 并二次确认 |
| 平台保留 | 组合会触发内核/驱动行为 | 文档列出，加载时警告 |

提供 `brickdesk-hotkeys --check`：输出冲突列表、最终解析表、每个绑定的 scope 和动作；CI 用它校验默认配置。

### 4.6 动作类型

| 动作 | 语法 | 说明 | 风险 |
|---|---|---|---|
| 内置 | `builtin.volume_up`、`builtin.brightness_down`、`builtin.screenshot`、`builtin.menu`、`builtin.home`、`builtin.sleep`、`builtin.reboot`、`builtin.shutdown`、`builtin.wifi_toggle` | inputd/系统直接执行 | reboot/shutdown 需确认 |
| 性能 | `perf.set`、`perf.cycle`、`perf.profile` | 交给 perfd，按会话租约 | 低；有白名单 |
| 启动应用 | `app.launch` + `app` | 交给 session | 中；目标必须是已安装应用 |
| 运行脚本 | `script.run` + `path`/`args` | fork/exec，超时，cwd/env 限定 | 高；需 `confirm`，日志记录 |
| 插件动作 | `plugin.invoke` + `plugin`/`action` | JSON IPC | 中；插件可能崩溃，不影响 inputd |
| 菜单 | `menu.open` | 打开通用应用内菜单 | 低 |
| 发送按键 | `key.send` | uinput 注入 | 高；只用于按键重映射，默认禁用 |

执行模型：

- inputd 自己执行低风险、快速的动作（音量、亮度、截图、菜单开关）；
- 其他动作发给 `brickdesk-session` 的 action worker，或 fork/exec 一个短命子进程；
- 每个动作有 `timeout_ms`（默认 3000，脚本最大 30000）；
- 动作失败写日志 + toast（如果 UI 可见），不阻塞 inputd；
- 破坏性动作（reboot/shutdown/杀应用）默认 `confirm=true`，在菜单里二次确认；热键直接触发时必须显式关闭 `confirm`；
- 所有动作记录 `timestamp, combo, action, result, duration`，便于复现。

### 4.7 配置草案

`config/hotkeys.ini`：

```ini
[hotkey.menu]
combo = Menu
hold_ms = 600
action = menu.open
scope = global
suppress = false

[hotkey.return_home]
combo = Menu+X
action = session.return_home
scope = global
confirm = true

[hotkey.screenshot]
combo = L1+R1+Start
window_ms = 50
action = builtin.screenshot
scope = global

[hotkey.performance]
combo = Select+Up
action = perf.cycle
scope = global

[hotkey.music_toggle]
combo = L1+Play
action = plugin.invoke
plugin = com.example.music
action_id = toggle
scope = app:com.example.music
```

语法：

- `A+B+C`：chord；
- `A,B`：序列；
- `Key(hold:800)`：长按，覆盖全局 `hold_ms`；
- `Key(double)`：双击；
- `*`：任意键（只用于诊断，不推荐）；
- scope：`global`、`app:<id>`、`mode:game`、`mode:desktop`；
- `suppress=true`：需要独占输入（grab+uinput），默认 false；
- `repeat=true`、`repeat_ms=100`：重复动作。

`config/input.ini`：

```ini
[device.TrimUI Gamepad]
vendor = 0x1a2c
product = 0x0001
A = BTN_SOUTH
B = BTN_EAST
X = BTN_NORTH
Y = BTN_WEST
L1 = BTN_TL
R1 = BTN_TR
Start = BTN_START
Select = BTN_SELECT
Menu = BTN_MODE
Up = ABS_HAT0Y:-1
Down = ABS_HAT0Y:1
Left = ABS_HAT0X:-1
Right = ABS_HAT0X:1
```

如果设备名/按键码未知，用 `brickdesk-input --learn` 让用户按一遍，生成映射；映射按 vendor/product 保存，不按 event 编号（重启后编号会变）。

### 4.8 与游戏抢按键的边界

默认被动模式：

- 游戏会看到组合键的每一个按键；如果游戏把 Menu 当暂停键，会同时触发游戏暂停和桌面菜单；
- 解决方式：选游戏不用的键；在应用 manifest 里声明 `input.reserved`；或开启 `exclusive_input`。

菜单打开时：

- inputd grab 所有手柄/键盘设备；游戏收不到事件；
- 关闭时先等所有键释放（最多 500 ms），再 ungrab；如果用户一直按着，菜单保持“请松开按键”提示，避免游戏收到半截状态；
- 游戏在 grab 期间产生的 evdev 事件通常不会排队给它（grab 会阻止投递）；ungrab 后一般只收到新事件。不同内核版本的缓冲行为要在设备上确认，所以关闭菜单前仍要等所有键释放；
- 如果游戏有自己的输入线程且在 grab 前已经读到 Menu 按下，可能在恢复后仍认为 Menu 按着；应用内 shim 可以在恢复时发一个“释放所有键”的提示，或在游戏侧 `SDL_FlushEvents`。

独占转发模式：

- inputd grab 物理设备，创建 uinput 虚拟手柄；
- 默认转发所有事件；命中的组合键被吞掉；
- 需要处理：按键码映射、ABS 摇杆、震动 ff、蓝牙设备、热插拔；
- 延迟增加 1–3 ms，音游/格斗游戏可能感知；按应用开启，默认关闭；
- inputd 崩溃时，虚拟设备消失，游戏失去输入；必须有 supervisor 1 秒内重启，并在崩溃前 ungrab（`atexit`/signal handler 不可靠，supervisor 兜底）。

### 4.9 设备验证清单（M1）

- [ ] 枚举 `/dev/input/event*`，记录 name/vendor/product/phys/uniq/capabilities；
- [ ] 确认 Menu、Power、Volume、L1/R1/L2/R2、Start/Select 的真实 key code；
- [ ] 确认 Power/Volume 是否独立设备，是否被内核/守护进程处理；
- [ ] 确认游戏（RetroArch、一个 SDL2 原生游戏）运行时，inputd 能否同时读到；
- [ ] 确认 `EVIOCGRAB` 在菜单期间对游戏的影响，ungrab 后是否丢键/粘键；
- [ ] 确认 SDL 的 `mali` 驱动是否 grab；如果是，记录并调整方案；
- [ ] 测试长按、双击、chord、序列、重复；
- [ ] 测试蓝牙手柄/USB 键盘热插拔；
- [ ] 测试 suspend/resume 后 fd 是否失效，是否需要重新枚举；
- [ ] 测试 inputd 被 kill 后 supervisor 重启、游戏输入恢复。

### 4.10 输入 API 草案

```c
/* include/brickdesk/input.h */
typedef enum {
    BD_KEY_A, BD_KEY_B, BD_KEY_X, BD_KEY_Y,
    BD_KEY_UP, BD_KEY_DOWN, BD_KEY_LEFT, BD_KEY_RIGHT,
    BD_KEY_L1, BD_KEY_R1, BD_KEY_L2, BD_KEY_R2,
    BD_KEY_START, BD_KEY_SELECT, BD_KEY_MENU,
    BD_KEY_POWER, BD_KEY_VOLUME_UP, BD_KEY_VOLUME_DOWN,
    BD_KEY_MAX
} bd_key;

typedef struct {
    bd_key key;
    int pressed;        /* 1 down, 0 up */
    unsigned source;    /* device id */
    unsigned long long time_ms;
} bd_key_event;

typedef struct {
    const char *id;
    const char *combo;      /* "L1+R1+Start" */
    const char *action;     /* "builtin.screenshot" */
    const char *scope;
    int hold_ms;
    int suppress;
    int confirm;
} bd_hotkey;
```

inputd 通过 `SOCK_SEQPACKET` 或共享环把 `bd_key_event` 发给菜单 shim；协议是二进制的，避免 JSON 在 60 Hz 输入下的开销。

---

## 5. 通用应用内菜单与应用能力协议

### 5.1 目标与非目标

目标：

- 在任何前台应用/游戏里，按住 Menu（默认 600 ms）呼出一个统一菜单；
- 菜单至少包含：返回桌面、亮度、音量、性能配置、截图、关闭菜单；
- 如果应用声明支持，再显示：保存状态、读取状态、挂起、后台运行、退出应用；
- 菜单的外观、焦点、主题与桌面一致；
- 菜单打开时游戏暂停/不响应输入；关闭后恢复；
- 不要求应用做 NextUI 那种深度集成（存档位、游戏切换器、成就等都不做）。

“任何应用”按可达性分层，不夸大：

- A（进程内叠加）覆盖大多数动态链接 SDL2/GLES2 的游戏；
- B/C 覆盖可改源码或愿意实现协议的应用；
- D 保证音量/亮度/返回桌面这类最低动作，但不提供完整菜单。

非目标：

- 不实现游戏切换器/多游戏快速切换；
- 不内置 RetroArch 的存档管理；
- 不接管应用的存档格式；
- 不承诺对所有静态链接/自定义渲染的应用都能显示菜单。

### 5.2 为什么不能跨进程“真叠加”

- 没有 X/Wayland compositor；
- EGL 1.4 只有 NULL WSEGL，没有 `eglGetPlatformDisplay`；
- 没有 GBM，`/dev/dri/card0` 不能给 EGL 消费；
- 显示由 PowerVR Display Class 管理，`eglSwapBuffers()` 直接提交/flip；
- `/dev/fb0` 不是可合成的 overlay 层，写它不一定能盖在 GPU 画面上；
- 没有暴露给普通进程的硬件 overlay API（Allwinner DE 的 `/dev/disp` 是否可用、是否能叠在 GPU 层上，需要设备探测，不能作为设计前提）。

所以：

- **独立进程再开一个 EGL surface** 不一定能和前台应用的 surface 共存；即使能创建，也没有 alpha 合成，只能是“整屏切换”；
- 真正的“半透明菜单盖在游戏画面上”只能由**拥有当前 GL 上下文的前台进程**来画；
- 或者让前台应用**释放显示**，菜单进程整屏接管。

### 5.3 四种模式

| 模式 | 谁画 | 是否暂停游戏 | 上下文丢失 | 适用 | 风险 |
|---|---|---|---|---|---|
| A. 进程内叠加（`LD_PRELOAD` shim） | 前台应用进程内的 `libbdmenu`，用它的 GL 上下文 | 渲染线程阻塞在 swap，逻辑暂停 | 不丢 | 动态链接 SDL2/GLES2 的第三方游戏 | 需要 hook；GL 状态/输入兼容性 |
| B. 嵌入库（`libbdmenu` 静态/动态链接） | 应用自己调用库 | 应用自己决定 | 不丢 | 第一方应用、可改源码的模拟器 | 需要改应用 |
| C. 暂停 + 显示交接 | 菜单进程（`brickdesk --menu`） | 是，应用释放 surface/context | 可能丢 | 静态链接、自定义 GL、支持协议的应用 | 应用必须能重建 context 或重启 |
| D. 无菜单 | 不画 | 否 | 无 | 不兼容应用 | 只有 inputd 级动作（音量/亮度/返回桌面） |

默认选择矩阵：

| 应用类型 | 默认模式 | 兜底 |
|---|---|---|
| 第一方/可改源码 | B | C |
| 第三方 SDL2/GLES2，动态链接 | A | C（若声明 `display_release`）→ D |
| 第三方静态链接/自定义 GL | C（若声明能力） | D |
| SDL_Renderer 软件渲染/raw fb | D | C |
| 自己 grab 输入的应用 | B | D（overlay/C 都拿不到输入） |
| 不支持 context 重建的应用 | A | D（不要用 C） |

M1 先实现 A（shim）+ D（inputd 动作），并用设备探针确认 C 是否可行；B 作为第一方后续接入。

### 5.4 模式 A：进程内叠加 shim

原理：`LD_PRELOAD` 一个 `libbdmenu-shim.so`，拦截应用每次“提交帧”的调用，在提交前用**应用当前的 GL 上下文**把菜单画到后缓冲上。

hook 点（按优先级）：

1. `SDL_GL_SwapWindow(SDL_Window*)`：SDL2 + OpenGL/GLES 应用最常用；
2. `SDL_RenderPresent(SDL_Renderer*)`：SDL_Renderer 应用；
3. `eglSwapBuffers(EGLDisplay, EGLSurface)`：直接用 EGL 的应用；
4. `eglSwapBuffersWithDamageKHR`：如果应用用了扩展。

同一个帧可能同时经过 `SDL_RenderPresent` 和 `SDL_GL_SwapWindow`（SDL 内部调用），shim 要用线程局部标记或帧序号去重，避免菜单画两次。

实现要点：

```c
static void (*real_swap)(SDL_Window *);

void SDL_GL_SwapWindow(SDL_Window *w) {
    if (!real_swap) real_swap = dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    if (bd_menu_is_open()) {
        bd_menu_render_in_current_context();  /* 画在后缓冲上 */
    }
    real_swap(w);
}
```

菜单打开时：

- 游戏渲染线程在 `SDL_GL_SwapWindow` 里被菜单阻塞（菜单循环），CPU 不空转；
- 菜单画在游戏刚渲染好的后缓冲上：先画一层 55% 黑色 scrim，再画面板/文字/图标；游戏画面自然成为背景，不需要截图；
- 菜单打开的第一帧先用 `glCopyTexImage2D` 把游戏后缓冲复制到一张纹理（1024×768 RGBA，约 3 MB），后续菜单帧都用它作背景；否则第一次 `eglSwapBuffers` 后后缓冲内容可能被交换掉，背景会变黑；
- 菜单关闭后返回，游戏下一帧正常；
- 不创建新窗口/context，不会触发 context loss；
- 额外内存：菜单纹理/字体 atlas 约 2–4 MB，在应用进程内。

GL 状态保存/恢复清单（必须）：

- `GL_VIEWPORT`、`GL_SCISSOR_BOX`/`GL_SCISSOR_TEST`；
- `GL_BLEND`/`GL_BLEND_SRC_RGB`/`GL_BLEND_DST_RGB`/`GL_BLEND_EQUATION`；
- `GL_DEPTH_TEST`、`GL_CULL_FACE`、`GL_STENCIL_TEST`；
- `GL_CURRENT_PROGRAM`；
- `GL_ARRAY_BUFFER_BINDING`、`GL_ELEMENT_ARRAY_BUFFER_BINDING`；
- 用到的 vertex attrib：`GL_VERTEX_ATTRIB_ARRAY_ENABLED/BUFFER_BINDING/SIZE/TYPE/NORMALIZED/STRIDE/POINTER`；
- `GL_ACTIVE_TEXTURE`、当前 texture unit 的 `GL_TEXTURE_BINDING_2D`；
- `GL_CLEAR_COLOR`；
- 如果用了 FBO：`GL_FRAMEBUFFER_BINDING`。

ES2 没有 core VAO；如果驱动支持 `GL_OES_vertex_array_object`，优先用 VAO 隔离；否则逐项保存/恢复。任何一项漏了都可能让游戏下一帧花屏——这是 shim 最大的兼容性风险，必须用真实游戏测试。

输入切换：

```text
inputd 检测到 Menu 长按
  -> EVIOCGRAB 所有手柄/键盘
  -> 通过 Unix socket 通知 shim "menu.open"
  -> shim 在下次 swap 画菜单，进入菜单事件循环
  -> inputd 把归一化按键发给 shim
  -> 用户选择/关闭
  -> shim 通知 inputd "menu.close"
  -> inputd 等所有键释放 -> EVIOCUNGRAB
  -> shim 返回，游戏继续
```

- 游戏不会收到菜单期间的按键（grab）；
- shim 启动时连接 inputd 的 Unix socket（`BRICKDESK_INPUT_SOCKET`），读二进制 `bd_key_event`，不直接读 evdev；
- 游戏渲染线程被阻塞，逻辑暂停；音频线程可能继续，需要在菜单打开时通过应用 hook 暂停音频（可选）；
- 如果游戏有独立输入线程，grab 会阻止它读到新事件；恢复后不会补发；
- 如果游戏在菜单打开前已经看到 Menu 按下，可能在恢复后认为它仍按着；shim 可以在关闭时发一个 `release_all` 给 SDL（如果应用用 SDL 事件队列）或忽略。

兼容性限制：

- 静态链接 SDL/EGL 的应用：`LD_PRELOAD` 的符号可能不生效；
- 应用自己加载 `eglSwapBuffers` 函数指针并直接调用：hook 不到；
- 应用用 raw fb/DRM：不适用；
- 应用自己 grab 输入：菜单收不到按键；
- 应用用多线程渲染/多个 context：需要在正确的线程/context 上画；shim 用 `pthread_getspecific` 或只在主 swap 线程画；
- 应用在 swap 后立即读回像素：菜单会污染读回结果；这种情况用 C/D。

因此 shim 必须：

- 按应用黑白名单启用（首次运行可自动探测 `ldd` 是否含 `libSDL2`/`libEGL`）；
- 提供 `BRICKDESK_MENU_DISABLE=1` 环境变量，出问题可一键关闭；
- 崩溃/黑屏时 session 记录并加入黑名单，下次用 C/D；
- 不在 shim 里做任何文件/网络/插件调用，只做渲染和 IPC。

### 5.5 模式 B：嵌入库

适合第一方应用和可改源码的模拟器（包括我们自己的 RetroArch fork）：

```c
/* 应用主循环 */
while (running) {
    poll_events();
    bd_menu_event(&e);          /* 菜单可见时消费事件，返回 1 表示已消费 */
    if (!bd_menu_visible()) {
        update_game();
        render_game();
    }
    bd_menu_render();           /* 在游戏画面之上画菜单；内部保存/恢复 GL 状态 */
    SDL_GL_SwapWindow(win);
}
```

优点：

- 无 hook、无 context 切换、无输入 grab 依赖；
- 应用可以把“暂停/恢复音频/存档”做成自己的回调；
- 菜单可以访问应用内部状态（当前游戏、核心、存档槽）。

缺点：需要改应用；只适合我们控制的应用。

API 草案：

```c
typedef struct {
    const char *app_id;
    const char *app_name;
    int (*suspend)(void *ud);
    int (*resume)(void *ud);
    int (*save_state)(const char *slot, void *ud);
    int (*load_state)(const char *slot, void *ud);
    void *ud;
} bd_menu_hooks;

int  bd_menu_init(const bd_menu_hooks *hooks);
void bd_menu_shutdown(void);
int  bd_menu_event(const void *sdl_event);  /* 返回 1 表示消费 */
void bd_menu_render(void);
int  bd_menu_visible(void);
void bd_menu_open(void);
void bd_menu_close(void);
```

### 5.6 模式 C：暂停 + 显示交接

流程：

```text
inputd        session            app                 brickdesk --menu
  | menu.hold ->|                  |                       |
  |            |-- app.pause ----->|                       |
  |            |<-- paused --------| (可选截图/存档)        |
  |            |-- start menu ---------------------------->|
  |<-- grab ----|                  |                       |
  |-- input ---------------------------------------------->|
  |            |<-- menu.closed --------------------------|
  |            |-- app.resume ----->|                       |
  |-- ungrab ---|                  |                       |
```

要求应用：

- 收到 `app.pause` 后停止渲染、完成当前帧、可选截图；
- 销毁 EGL surface + context（或至少释放 surface），回复 `paused`；
- 菜单进程创建自己的 surface，整屏绘制；
- 关闭后应用重建 context/surface，重新上传纹理/shader，继续；
- 如果应用不能重建 context，就先 `save_state` 再退出，恢复时重启（慢，但可靠）。

优点：

- 不需要 hook，不挑渲染库；
- 菜单是独立进程，崩溃不影响游戏（游戏已暂停）；
- 可以显示完整的桌面级菜单。

缺点：

- 应用必须配合；不配合就只能 SIGSTOP，但 SIGSTOP 不会释放 surface；如果应用自己 grab 了输入，C 模式也拿不到菜单按键；
- context 丢失，很多 SDL2 游戏没有处理，恢复会黑屏/花屏；
- 切换耗时 100–500 ms；
- 菜单进程要重新创建 GL 资源，内存峰值高。

关键设备探针（M1）：

> 当前台应用仍然持有 EGL surface、但被 SIGSTOP 暂停时，另一个进程能否 `eglCreateWindowSurface` 并 `eglSwapBuffers` 成功？

- 如果能：C 模式可以扩展到不配合的应用（SIGSTOP + 菜单进程整屏接管），但恢复后应用可能不知道自己丢过帧；
- 如果不能（`EGL_BAD_ALLOC`/`EGL_BAD_NATIVE_WINDOW`）：C 只适用于声明 `display_release` 的应用；
- 探针程序：`tools/display_probe.c`，输出 `eglQueryString`、surface 创建结果、swap 结果、恢复后应用下一帧结果。

### 5.7 模式 D：无菜单兜底

对于不兼容的应用，至少保证：

- 音量 +/-、亮度 +/-：inputd 直接写 sysfs/ALSA；
- 返回桌面：长按 Menu + X（或 Menu 长按两次）发送 SIGTERM；有 `save_state` hook 的先调用；
- 性能 profile：inputd 直接请求 perfd，切换后 toast 不可见，但下一次进桌面能看到；
- 截图：如果 `/dev/fb0` 可读且能拿到当前帧，保存；否则不提供；
- 反馈：震动/LED/提示音，让用户知道动作生效。

这是“任何应用都能用”的最低保证，但**不是完整菜单**；文档和 UI 要如实说明。

### 5.8 应用能力声明

应用包目录（由 brickdesk 管理，或应用自带）：

```text
/mnt/SDCARD/brickdesk/apps/org.libretro.retroarch/
  app.json
  icon.png
  launch.sh
```

`app.json`：

```json
{
  "schema": 1,
  "id": "org.libretro.retroarch",
  "name": "RetroArch",
  "version": "1.19.0",
  "exec": ["/mnt/SDCARD/Emu/retroarch/retroarch"],
  "cwd": "/mnt/SDCARD/Emu/retroarch",
  "env": { "HOME": "/mnt/SDCARD/Emu/retroarch" },
  "icon": "icon.png",
  "category": "emulator",
  "menu": {
    "mode": "overlay",
    "overlay": { "safe": true, "exclude_keys": ["Menu"] }
  },
  "capabilities": {
    "pause": true,
    "resume": true,
    "save_state": true,
    "load_state": true,
    "screenshot": true,
    "background": false,
    "display_release": false
  },
  "hooks": {
    "pause":   { "type": "ipc", "method": "app.pause", "timeout_ms": 300 },
    "resume":  { "type": "ipc", "method": "app.resume", "timeout_ms": 2000 },
    "save":    { "type": "ipc", "method": "state.save", "timeout_ms": 5000 },
    "load":    { "type": "ipc", "method": "state.load", "timeout_ms": 5000 },
    "screenshot": { "type": "ipc", "method": "app.screenshot", "timeout_ms": 500 }
  },
  "perf": { "profile": "performance", "core": "snes9x" },
  "state": { "format": "retroarch", "max_slots": 4 }
}
```

字段表：

| 字段 | 类型 | 说明 |
|---|---|---|
| `schema` | int | manifest 版本，当前 1 |
| `id` | string | 反向域名，唯一 |
| `name` | string | 显示名 |
| `version` | string | 应用版本 |
| `exec` | string[] | argv；不含 shell 解析 |
| `cwd` | string | 工作目录 |
| `env` | object | 额外环境变量；只允许字符串 |
| `icon` | string | 相对路径 |
| `category` | enum | `game`/`app`/`emulator`/`tool`/`service` |
| `menu.mode` | enum | `overlay`/`embedded`/`handoff`/`none` |
| `menu.overlay.exclude_keys` | string[] | shim 不拦截的键 |
| `capabilities.pause` | bool | 支持暂停（不退出） |
| `capabilities.resume` | bool | 支持恢复 |
| `capabilities.save_state` | bool | 支持保存状态 |
| `capabilities.load_state` | bool | 支持读取状态 |
| `capabilities.screenshot` | bool | 支持应用内截图 |
| `capabilities.background` | bool | 支持后台运行（音乐/服务） |
| `capabilities.display_release` | bool | 支持释放/重建显示 |
| `hooks.*.type` | enum | `ipc` 或 `command` |
| `hooks.*.method` | string | IPC 方法名 |
| `hooks.*.exec` | string[] | command 模式 argv，支持 `{slot}`、`{state_dir}`、`{pid}` |
| `hooks.*.timeout_ms` | int | 超时 |
| `perf.profile` | string | 默认性能 profile |
| `perf.core` | string | 模拟器核心 id |
| `state.format` | string | 应用自定义格式标识 |
| `state.max_slots` | int | 状态槽上限 |

如果应用没有 `app.json`，session 生成一个运行时条目：

- `id` = 可执行文件路径的 hash；
- `menu.mode` = `overlay`（如果 `ldd` 含 `libSDL2`/`libEGL`），否则 `none`；
- 无状态/无 hook；
- 用户可以后来补 `app.json`。

### 5.9 应用 IPC

应用启动时，session 注入：

```text
BRICKDESK_APP_ID=org.libretro.retroarch
BRICKDESK_SESSION_ID=7f3c2a
BRICKDESK_SESSION_SOCKET=/run/brickdesk/session-7f3c2a.sock
BRICKDESK_STATE_DIR=/mnt/SDCARD/brickdesk/state/org.libretro.retroarch
BRICKDESK_MENU_MODE=overlay
```

握手：

```json
{"jsonrpc":"2.0","id":1,"method":"app.hello","params":{
  "pid":2345, "protocol":1,
  "capabilities":["pause","resume","save_state","load_state","screenshot"]
}}
```

```json
{"jsonrpc":"2.0","id":1,"result":{
  "session_id":"7f3c2a", "menu_mode":"overlay",
  "state_dir":"/mnt/SDCARD/brickdesk/state/org.libretro.retroarch"
}}
```

宿主 → 应用：

| 方法 | 说明 | 默认超时 |
|---|---|---|
| `app.pause` | 暂停渲染/逻辑，准备菜单 | 300 ms |
| `app.resume` | 恢复 | 2000 ms |
| `state.save` | 保存到 `slot`，返回 `{ok, bytes, screenshot}` | 5000 ms |
| `state.load` | 从 `slot` 读取 | 5000 ms |
| `state.list` | 列出槽位/元数据 | 1000 ms |
| `app.screenshot` | 截图到指定路径或共享内存 | 500 ms |
| `app.quit` | 退出应用（可先保存） | 3000 ms |
| `app.ping` | 心跳 | 500 ms |

应用 → 宿主：

| 方法 | 说明 |
|---|---|
| `app.hello` | 握手 |
| `app.toast` | 请求提示 |
| `app.perf.request` | 运行中请求性能 profile（如核心切换） |
| `app.state` | 主动报告状态（运行/暂停/存档中） |
| `app.log` | 结构化日志 |

错误码同 3.5。所有请求带 `id`，超时后宿主认为失败，按模式回退（见 5.10）。

command 模式：

- 适合 shell 包装的应用；
- session fork/exec `hooks.save.exec`，cwd 为应用目录，env 加上 `BRICKDESK_*`；
- 占位符替换只做一次，不经过 shell；
- 返回码 0 成功，非 0 失败；stdout 最后一行可返回 JSON `{ok, bytes, screenshot}`；
- 超时 SIGTERM，1 秒后 SIGKILL；
- 与 IPC 模式互斥：有 socket 时优先 IPC。

### 5.10 失败与恢复

| 失败 | 处理 |
|---|---|
| `app.pause` 超时 | 发 SIGSTOP；尝试菜单进程接管（C）；失败则 SIGCONT + D |
| `app.resume` 超时 | 菜单关闭；session 标记应用“恢复失败”；建议用户返回桌面 |
| 应用暂停中崩溃 | `waitpid` 发现；菜单显示“应用已退出”；关闭回桌面 |
| 菜单进程崩溃 | session 恢复应用；inputd ungrab；记录 |
| shim 崩溃/黑屏 | 应用崩溃 → session 记录并加入黑名单；下次 D |
| inputd 崩溃 | supervisor 重启；菜单 shim 进入“无输入”超时 5 秒后自动关闭；游戏恢复 |
| perfd 崩溃 | 租约到期，硬件保持最后状态；supervisor 重启后恢复默认 profile |
| 电量低 | 菜单提示；禁止性能提升；必要时保存并退出 |
| 温度高 | 菜单显示警告；perfd 降频；不允许 performance |

### 5.11 菜单内容与交互

默认布局（1024×768）：

```text
+--------------------------------------------------+
|  RetroArch                          82%  14:32  |
|  ----------------------------------------------  |
|  返回桌面                              >         |
|  保存状态                              >         |
|  读取状态                              >         |
|  挂起                                  >         |
|  截图                                            |
|  ----------------------------------------------  |
|  亮度       [----------o------]   70             |
|  音量       [-------o---------]   45             |
|  性能       [ 平衡 | 性能 | 省电 ]               |
|  ----------------------------------------------  |
|  A 选择    B 关闭    X 截图    Y 性能            |
+--------------------------------------------------+
```

- 菜单不透明区域占屏幕中间约 70%，背景是游戏画面 + 55% 黑 scrim；
- 焦点用高亮行 + 左侧强调条，和桌面设置一致；
- 亮度/音量实时调整：菜单把 `builtin.brightness`/`builtin.volume` 发给 inputd，由 inputd 写背光/ALSA，不经过前台应用；
- 性能切换立即生效，显示当前 profile 和频率；
- 截图：在画菜单之前先抓当前后缓冲（游戏画面），或调用应用的 `app.screenshot` hook；保存到 `screenshots/`；
- 保存/读取状态显示槽位、时间、截图缩略图；
- “返回桌面”：菜单发 `session.return_home`；session 等当前帧结束，有 `save_state` 的先保存，再 `app.quit`/SIGTERM，最后重建桌面 surface；
- 菜单打开时游戏暂停；音乐类应用如果声明 `background`，音频继续。

### 5.12 快速恢复、后台应用、存档

这些能力都通过协议声明，桌面只负责调用和展示：

**存档**

- 目录：`/mnt/SDCARD/brickdesk/state/<app-id>/<slot>/`
- 元数据：`meta.json`（`app_id`、`slot`、`created_at`、`game_id`、`core`、`screenshot`、`bytes`、`format_version`）
- 负载：应用自定义；桌面不解析
- 配额：每应用默认 512 MB，LRU 清理；用户可在设置里删除
- 展示：应用磁贴角标“有存档”；菜单里显示槽位列表

**快速恢复**

- 方式 1：`save_state` + `app.quit`，下次 `app.load` + 启动；省内存，慢 1–3 秒；
- 方式 2：SIGSTOP 保留内存，下次 SIGCONT；快，但占 RAM；只允许一个“快速恢复”槽，且可用内存低于阈值时禁用；
- 桌面根据 `capabilities` 和当前内存选择；用户可在设置里强制“省内存/快速”；
- 恢复后如果是 `handoff` 模式，应用要重建 GL；如果是 `overlay`/`embedded`，不需要。

**后台应用**

- 只对 `category=service` 或声明 `background=true` 的应用开放（音乐、播客、下载）；
- 后台应用不渲染，只保留音频/网络/定时器；
- 桌面显示“正在后台运行”的指示；
- 游戏默认不允许后台（除非声明，且用户确认）；
- 内存不足时按优先级停止后台应用。

### 5.13 安全与兼容性

- shim 是第一方代码，随桌面发布，签名/校验；不加载第三方 `.so`；
- shim 只 hook 必要的符号，不做系统调用拦截，不读文件，不联网；
- `LD_PRELOAD` 只对 session 启动的应用生效，不影响系统守护进程；
- 应用可以通过 `BRICKDESK_MENU_DISABLE=1` 或 manifest `menu.mode=none` 退出；
- 黑名单持久化在 `config/menu-blacklist.ini`，用户可清除；
- 菜单进程/ shim 的所有输入都来自 inputd，不直接读 evdev（除非 inputd 崩溃的兜底）；
- 所有 hook 调用有超时；shim 卡住时 inputd 5 秒后强制关闭菜单并 ungrab。

### 5.14 M1 验证项

- [ ] 在宿主机用 `LD_PRELOAD` shim + SDL2/GLES2 测试程序，确认菜单画在应用帧上（本 orb 已做最小验证，见 8.8）；
- [ ] 在设备上用 2–3 个真实 SDL2 游戏测试 shim：画面、输入、退出、恢复；
- [ ] 确认 `eglCreateWindowSurface` 在另一个进程持有 surface 时是否成功（C 模式探针）；
- [ ] 确认 `EVIOCGRAB` 在菜单期间对游戏输入的影响；
- [ ] 确认应用 context 丢失后的恢复能力（C 模式）；
- [ ] 用 `app.json` 样例跑通 pause/resume/save/load 的 IPC；
- [ ] 用 command hook 跑通一个 shell 包装的应用；
- [ ] 测试低电量/高温时的菜单行为；
- [ ] 测试 shim 被禁用/崩溃后的黑名单和回退。

---

## 6. 性能配置

### 6.1 可调参数

| 类别 | 参数 | 典型路径 | 建议值 | 风险 |
|---|---|---|---|---|
| CPU 调度 | governor | `/sys/devices/system/cpu/cpufreq/policy0/scaling_governor` | 桌面 `schedutil`；游戏 `performance`；省电 `powersave` | 低 |
| CPU 频率 | min/max | 同目录 `scaling_min_freq`/`scaling_max_freq` | 408 MHz–1.8 GHz；按 profile | 中；写错会卡顿/过热 |
| 在线核 | cpu1–3 online | `/sys/devices/system/cpu/cpuN/online` | 默认 4；省电 2；永远保留 cpu0 | 中；热插拔可能影响中断/延迟 |
| GPU 频率 | devfreq/PVR | `/sys/class/devfreq/*/cur_freq`、`/sys/kernel/debug/pvr/*` | 只在探测到接口时暴露 | 高；接口不标准，可能不存在 |
| 内存 | swappiness | `/proc/sys/vm/swappiness` | 20（默认 60） | 低 |
| 内存 | vfs_cache_pressure | `/proc/sys/vm/vfs_cache_pressure` | 50 | 低 |
| 回写 | dirty_writeback_centisecs | `/proc/sys/vm/dirty_writeback_centisecs` | 1500 | 低；减少 SD 写入 |
| IO | scheduler | `/sys/block/mmcblk*/queue/scheduler` | `mq-deadline` 或 `bfq`（探测） | 低 |
| IO | read_ahead_kb | 同目录 | 256 | 低 |
| 热 | thermal zone | `/sys/class/thermal/thermal_zone*/temp` | 80 °C 降频，70 °C 恢复 | 只读/保护 |
| 电池 | capacity/temp | `/sys/class/power_supply/*/capacity` | <5% 禁止性能提升 | 只读 |
| 显示 | 背光 | `/sys/class/backlight/*/brightness` | 用户设置 | 低 |
| 音频 | ALSA mixer | `amixer`/`libasound` | 用户设置 | 低 |

> 具体路径、可用频率表、governor 列表、GPU 是否有 devfreq/PVR 接口，由设备调研线程确认。perfd 必须在启动时探测，不能硬编码。

A133P 公开资料：4×A53，A133P 最高可到 2.0 GHz（linux-sunxi），但 Brick Pro 标称 1.8 GHz；GE8300 最高 660 MHz。实际 OPP 表以设备 `/sys/.../scaling_available_frequencies` 为准。

### 6.2 分层

优先级从低到高：

```text
全局 default
  < 设备型号 profile（可选）
    < 应用 profile（app.json / perf.ini）
      < 模拟器核心 profile（perf.ini）
        < 运行时覆盖（菜单/热键/应用 IPC）
```

示例 `config/perf.ini`：

```ini
[profile.default]
governor = schedutil
min_khz = 408000
max_khz = 1200000
online_cores = 4
swappiness = 20

[profile.balanced]
inherit = default
max_khz = 1416000

[profile.performance]
inherit = balanced
governor = performance
min_khz = 816000
max_khz = 1800000

[profile.powersave]
inherit = default
governor = powersave
max_khz = 816000
online_cores = 2

[app.org.libretro.retroarch]
profile = performance

[core.snes9x]
inherit = performance
max_khz = 1416000

[core.pcsx_rearmed]
inherit = performance
max_khz = 1800000
```

规则：

- `inherit` 形成链；循环检测；未知 profile 报错并回退 default；
- 频率写 kHz；perfd 读取 `scaling_available_frequencies`，选择**不超过请求值的最近频率**；如果没有，报错并保持原值；
- `online_cores` 只允许 2–4（M1），不允许 0/1；写入顺序 cpu3→cpu2，失败则停止并回滚；
- `governor` 必须在 `scaling_available_governors` 里；否则按 `schedutil`→`ondemand`→`performance` 回退；
- 核心 profile 由 session 从 `app.json` 的 `perf.core` 或 argv（RetroArch `--libretro ...`）解析；解析不到就用应用 profile；
- 应用运行中可以通过 `app.perf.request` 请求切换（例如 RetroArch 换核心），perfd 更新租约，退出时恢复。

### 6.3 切换时机

| 时机 | 动作 |
|---|---|
| 开机 | perfd 应用 `default`，记录当前值 |
| 启动应用前 | session 发 `perf.apply`，perfd 在 50 ms 内应用并回读校验 |
| 应用运行中 | 菜单/热键/应用 IPC 发 `perf.set`，立即应用，租约续期 |
| 应用退出 | session 发 `perf.restore`，perfd 恢复 default/上一个全局 profile |
| 应用崩溃 | `waitpid` 后恢复；perfd 租约到期也会恢复 |
| 菜单打开 | 菜单显示当前 profile；切换只影响当前会话 |
| 返回桌面 | 恢复 `desktop` profile（`schedutil`，max 1.2 GHz） |
| 休眠/关机 | 恢复 default，关闭租约 |

### 6.4 `brickdesk-perfd`

为什么需要独立守护进程：

- 桌面、session、inputd、插件都可能想改性能；多个写者会互相覆盖；
- 应用崩溃/桌面重启时，必须有人恢复；
- sysfs 写入需要 root，插件不应该有；
- 需要一个地方做白名单、校验、回滚、审计。

接口（Unix socket，JSON）：

```json
{"jsonrpc":"2.0","id":1,"method":"perf.apply","params":{
  "session":"7f3c2a","profile":"performance","ttl_ms":0
}}
```

```json
{"jsonrpc":"2.0","id":1,"result":{"applied":{
  "governor":"performance","min_khz":816000,"max_khz":1800000,
  "online_cores":4,"swappiness":20
}}}
```

方法：

| 方法 | 说明 |
|---|---|
| `perf.apply` | 为 session 应用 profile；`ttl_ms=0` 表示直到 restore |
| `perf.set` | 运行中切换（菜单/应用请求） |
| `perf.restore` | 恢复会话前的状态 |
| `perf.get` | 返回当前 profile、实际频率、温度、电量 |
| `perf.list` | 返回可用 profile/频率/governor |
| `perf.capabilities` | 返回探测到的可控项（CPU/GPU/背光/…） |

写入事务：

```text
1. 读取旧值（governor/min/max/online/swappiness）
2. 计算目标值并校验（白名单、范围、可用频率）
3. 写 governor
4. 写 max（降频先写 max；升频后写 max）
5. 写 min（保证 min <= max）
6. 写 online cores（如有）
7. 写 vm 参数（如有）
8. 回读校验；任一步失败 -> 逆序回滚 -> 返回错误
9. 记录 old/new 到日志和 /run/brickdesk/perf-state.json
```

### 6.5 安全边界

必须：

- 只写白名单路径；路径在编译期/启动时固定，不接受客户端传路径；
- 客户端只能传 profile 名/受限字段，不能传任意 kHz（菜单可以传 `max_khz`，但必须在可用频率表里）；
- 频率 clamp 到 `[cpuinfo_min_freq, cpuinfo_max_freq]`；不允许超频/升压；
- `min <= max`；不允许 offline cpu0；至少保留 2 个核；
- 不允许关闭 thermal/cooling device；不允许写 trip point；
- 温度 ≥ 80 °C：降频到 1.2 GHz + `schedutil`；≥ 90 °C：恢复 default 并提示；温度回落到 70 °C 才允许再次提升；
- 电量 < 5%：禁止 performance；< 2%：恢复 default，建议休眠；
- 租约：session 崩溃/退出后 `ttl_ms` 到期自动恢复；默认租约 10 秒心跳续期；
- 启动风暴：如果 60 秒内重启两次且上次是非 default profile，进入 safe mode，强制 default，并在 UI 提示；
- 所有写入记录 old/new/result；日志轮转 512 KB × 3；
- `--dry-run` 模式只打印计划，不写；CI/单测用 fake sysfs。

禁止：

- 改 bootloader/设备树/内核参数；
- 改 GPU 电压/频率（除非厂商提供明确、受支持的接口）；
- 改 thermal trip/cooling；
- 持久化写 `/etc`；
- 让插件/普通应用直接写 sysfs。

### 6.6 GPU 与内存

GPU：

- 优先探测 `/sys/class/devfreq/*/cur_freq`、`available_frequencies`；
- PowerVR 可能只在 `/sys/kernel/debug/pvr/` 暴露 DVFS/apphint；debugfs 默认可能不可用；
- 如果没有稳定接口，UI 里显示“GPU 频率：不支持”，不要假装能调；
- 不要用 `pvrsrvctl` 之类的私有工具做未验证的写操作；
- 如果只读，可以显示当前 GPU 频率用于诊断。

内存：

- `swappiness=20` 减少换出，延长 SD 寿命；如果系统用 zram，保持 zram 的 swappiness 更高（如 60）需实测；
- `vfs_cache_pressure=50` 保留目录缓存；
- `dirty_writeback_centisecs=1500` 减少频繁写 SD；
- 不要关闭 overcommit；不要改 `min_free_kbytes` 除非有实测依据；
- 桌面/插件的内存预算用 cgroup v2（如果内核启用）；否则用 `RLIMIT_AS`；
- 大图缓存上限（如 96 MB）+ LRU；解码在 worker 线程，避免主线程卡顿。

### 6.7 设备探测清单（交给设备线程）

- [ ] `/sys/devices/system/cpu/cpufreq/policy*/` 是否存在，policy 数量；
- [ ] `scaling_available_governors`、`scaling_available_frequencies`、`cpuinfo_min/max_freq`；
- [ ] `cpu1..3/online` 是否可写；
- [ ] `/sys/class/devfreq/*`、`/sys/kernel/debug/pvr/*`、`/sys/kernel/debug/clk/gpu/clk_rate`；
- [ ] thermal zone 数量、类型、trip point；
- [ ] 电池 capacity/temp/status；
- [ ] 背光节点、max_brightness；
- [ ] ALSA card/mixer 名称；
- [ ] `/proc/sys/vm/*` 可写项；
- [ ] SD 卡块设备、scheduler、read_ahead；
- [ ] 当前内核版本、是否支持 cgroup v2、pidfd、memfd、user namespace；
- [ ] 默认 governor/频率/温度/电量基线。

---

## 7. 配置系统

### 7.1 格式取舍

| 格式 | C 库/体积 | 读 | 写 | 注释 | 嵌套/数组 | 适合 |
|---|---|---|---|---|---|---|
| INI | inih，约 2 个文件/500 行，BSD-3 | 是 | 自己写，简单 | 是 | section；数组用重复 section | 人读配置、profile、热键 |
| TOML | tomlc99，约 2.5k 行，MIT | 是 | 官方无 writer | 是 | 表/数组完整 | 复杂配置、插件 manifest |
| JSON | cJSON 约 3k 行；jsmn 约 200 行 | 是 | 是 | 否 | 完整 | IPC、manifest、机器数据 |

推荐：

- **人读配置用 INI**：`brickdesk.ini`、`perf.ini`、`hotkeys.ini`、`input.ini`。理由：inih 极小、无依赖、回调式解析、可注释；section 天然对应 profile/app/hotkey；写回只需按 schema 生成，不需要通用 TOML writer。
- **机器数据用 JSON**：`plugin.json`、`app.json`、IPC、状态元数据。理由：插件作者熟悉；结构化；易于跨语言。
- **主题用 INI + 资源**：颜色、字体、间距、背景图；不引入代码。

不选 TOML 作为主格式的原因：

- 配置面主要是扁平标量和 section，TOML 的嵌套优势用不上；
- tomlc99 只读；UI 修改要额外写 writer，转义/数组/日期的边界比 INI 多；
- 多一个 2.5k 行的解析器，对 1 GB 设备不是大问题，但没有收益；
- 如果未来插件设置真的需要嵌套，可以在 `config.c` 后面换 TOML，不影响上层。

不选全 JSON 的原因：

- 不能写注释，手改体验差；
- 逗号/引号错误对普通用户不友好；
- UI 写回会丢格式；
- 配置文件在 SD 卡上，用户会用文本编辑器改。

### 7.2 目录布局

```text
/mnt/SDCARD/brickdesk/
  config/
    brickdesk.ini        # 通用设置（随固件发布，可被覆盖）
    perf.ini             # 性能 profile
    hotkeys.ini          # 组合键
    input.ini            # 设备/按键映射
    local.ini            # UI 写入的用户覆盖（最高优先级）
    menu-blacklist.ini   # 菜单 shim 兼容性黑名单
    plugins/<id>.ini     # 插件设置（宿主代写）
  apps/<id>/app.json     # 应用能力声明
  plugins/<id>/plugin.json
  themes/<id>/theme.ini
  state/<app-id>/<slot>/ # 存档（应用自定义负载 + meta.json）
  cache/                 # 封面/缩略图缓存，可删
  logs/                  # 轮转日志
  screenshots/
```

路径解析：

1. `BRICKDESK_HOME` 环境变量（测试/开发用）；
2. `/mnt/SDCARD/brickdesk`（spruce/NextUI 常见挂载点）；
3. `/mnt/mmc/brickdesk`、`/media/sdcard/brickdesk`（其他 CFW）；
4. 都不可写时用 `/run/brickdesk`（tmpfs，重启丢失），并在 UI 提示“配置不会保存”。

内置默认值在 `/usr/share/brickdesk/defaults/`；设备镜像可以在 `/etc/brickdesk/` 放只读覆盖。**UI 永远不重写固件自带的默认文件**，只写 `local.ini`。

### 7.3 覆盖层级

从低到高，按 key 合并：

| 层 | 位置 | 谁写 | 说明 |
|---|---|---|---|
| 1 | 编译期默认值 | 开发者 | 每个 key 必须有默认 |
| 2 | `/usr/share/brickdesk/defaults/*.ini` | 固件 | 文档化的默认值 |
| 3 | `/etc/brickdesk/*.ini` | 设备镜像 | 机型/发行版覆盖 |
| 4 | SD `config/*.ini` | 用户/发行包 | 常规配置 |
| 5 | SD `config/apps/<id>.ini` | 应用/用户 | 应用级 |
| 6 | SD `config/local.ini` | UI | 只写用户改过的 key |
| 7 | `BRICKDESK_*` 环境变量 | 测试/高级用户 | 少量白名单 key |
| 8 | 命令行参数 | 测试/开发 | 最高 |

语义：

- 标量：高优先级覆盖低优先级；
- 列表/数组：高优先级**整体替换**，不做合并（避免“删不掉默认项”的困惑）；
- 热键：按 `[hotkey.<id>]` section 合并；同 id 高优先级覆盖整个 section；
- profile：按 `[profile.<name>]` 合并；`inherit` 链在层内解析；
- 未知 key：保留、警告、不删除；`local.ini` 里未知 key 原样保留，便于降级；
- 空值：`key =` 表示“显式空”，与“未设置”不同；字符串空值合法，数值空值报错并用默认。

### 7.4 类型与校验

支持的类型：

| 类型 | 例子 | 校验 |
|---|---|---|
| `string` | `theme = default` | 长度、允许字符 |
| `int` | `hold_ms = 600` | min/max |
| `float` | `scale = 1.0` | min/max |
| `bool` | `animations = true` | `true/false/yes/no/1/0` |
| `enum` | `governor = schedutil` | 枚举表 |
| `path` | `theme_dir = themes/foo` | 必须在允许根目录下，拒绝 `..` |
| `duration` | `sleep_after = 180s` | 解析 ms/s/min；范围 |
| `frequency` | `max_khz = 1800000` | 必须在可用频率表内（perfd 再校验） |
| `color` | `accent = #3b82f6` | `#RRGGBB`/`#RRGGBBAA` |
| `combo` | `combo = L1+R1+Start` | 热键语法 + 冲突检测 |

加载流程：

```text
parse -> 类型转换 -> 范围/枚举校验 -> 跨 key 校验（min<=max、profile 循环）
      -> 冲突检测（热键、scope） -> 生成 effective config + source map
```

- 单个 key 失败：用默认值，记录 `file:line`，继续加载；
- 致命错误（schema 版本不支持、配置文件不可读）：进入 safe mode，用默认值，UI 顶部显示警告；
- `brickdesk-config check` 输出所有错误和警告，返回非 0；CI 用它校验默认配置；
- 配置值不带单位时按 schema 的默认单位解析；带单位更清晰（`180s`、`1800000khz`）。

### 7.5 迁移

- 每个文件有 `[meta] schema_version = 1`；
- 启动时比较版本：
  - 低于当前：按迁移链 `1->2->3` 逐步迁移；先备份 `file.bak.v1`；写回用原子替换；
  - 等于当前：正常加载；
  - 高于当前：safe mode，只读加载已知 key，提示“配置来自更新版本”；
- 迁移只做结构变化，不改用户值；新增 key 用默认值；
- 迁移失败：保留原文件，写 `.failed` 副本，safe mode；
- `brickdesk-config migrate --dry-run` 打印将做的修改；
- 降级时未知 key 保留，不删除；旧版本忽略它们。

### 7.6 原子写与并发

- 写 `local.ini`/插件配置：同目录临时文件 `*.tmp.<pid>`，`fsync`，`rename`，再 `fsync` 目录；
- 写前 `flock` 配置文件（`LOCK_EX`），避免 UI 和 CLI 同时写；
- 保留 `.bak` 一份；失败时原文件不变；
- 不跟随符号链接（`O_NOFOLLOW`），防止 SD 卡上的恶意链接；
- 权限：配置 0644，state 0700，日志 0644；不写密码/token；
- 日志轮转：512 KB × 3。

### 7.7 热重载

- inotify 监听 `config/`；200 ms debounce；
- 解析到新树，校验通过后原子替换；失败保留旧树 + toast；
- `restart_required` 的 key（分辨率、渲染后端、语言字体、输入设备映射）只标记，不立即应用；UI 显示“重启后生效”；
- 游戏运行中不重载会影响性能/输入的内容；session 在应用退出后应用；
- SIGHUP 作为兜底（inotify 不可用时）。

### 7.8 CLI

```sh
brickdesk-config get ui.theme
brickdesk-config set ui.theme light          # 写 local.ini
brickdesk-config dump --json                 # 有效值 + 来源文件/行
brickdesk-config check                       # CI/支持
brickdesk-config migrate --dry-run
brickdesk-config diff                        # 与默认值差异
```

`dump` 示例：

```json
{
  "ui.theme": { "value": "light", "source": "/mnt/SDCARD/brickdesk/config/local.ini:3" },
  "perf.default.max_khz": { "value": 1200000, "source": "compiled" }
}
```

### 7.9 示例

`config/brickdesk.ini`：

```ini
[meta]
schema_version = 1

[ui]
theme = default
language = zh-CN
animations = true
time_format = 24h
clock_seconds = false
cover_cache_mb = 96

[display]
width = 1024
height = 768
refresh_hz = 60
brightness = 70

[input]
repeat_delay_ms = 400
repeat_interval_ms = 80
chord_window_ms = 50
long_press_ms = 600
double_tap_ms = 300

[power]
sleep_after = 180s
low_battery_percent = 5
shutdown_on_low_battery = true

[plugins]
enabled = true
max_total_rss_mb = 128
idle_stop = 60s
autostart_delay = 2s

[session]
state_quota_mb = 512
quick_resume = auto
```

`config/local.ini`（UI 只写改动）：

```ini
[ui]
theme = light

[display]
brightness = 80
```

> 注意：`local.ini` 里不要重复整份配置；只写用户改过的 key。这样默认值升级后能生效。

---

## 8. 测试与 CI

### 8.1 构建矩阵

| 目标 | 环境 | 编译器 | SDL | GL | 用途 |
|---|---|---|---|---|---|
| host | Debian 12 / Mesa 22.3.6 | GCC 12 | 2.26.5 | llvmpipe GLES2 | 日常开发、截图回归、逻辑单测 |
| target | Docker Ubuntu 20.04 + 官方 SDK | GCC 7.5 Linaro | 2.26.1 GE8300 | PowerVR GLES2 | 交叉编译、产物 |
| device | Brick Pro | — | 同上 | 同上 | 冒烟、性能、输入/显示验证 |

原则：

- host 和 target 共用同一份 C 源码，用 `BRICKDESK_BACKEND=host|target` 区分平台层；
- 平台差异只在 `src/platform/`；渲染、布局、配置、输入状态机、插件协议都能在 host 测；
- host 截图基线只对 host 负责；不要拿 host 基线去卡设备截图。

### 8.2 Docker 工具链

`ci/Dockerfile.toolchain`：

```dockerfile
FROM ubuntu:20.04

ARG TOOLCHAIN_URL=https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/aarch64-linux-gnu-7.5.0-linaro.tgz
ARG TOOLCHAIN_SHA=604d97c1f5fadc88d6ebff577ea648d0fbc650411af98a3bd8783dbe4e3ed154
ARG SDK_URL=https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/SDK_usr_tg5040_a133p.tgz
ARG SDK_SHA=b6b615d03204e9d9bb1a91c31de0c3402434f6b8fc780743fa88a5f1da6f3c79
ARG SDL2_URL=https://github.com/trimui/toolchain_sdk_smartpro/releases/download/20231018/SDL2-2.26.1.GE8300.tgz
ARG SDL2_SHA=a37fbed6c8771f1050e6520fa9c48354e01519de813fe176ae53089f2087bb87

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config ca-certificates curl file binutils python3 \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/trimui/toolchain /opt/trimui/sdk /opt/trimui/sdl2 \
 && curl -fsSL "$TOOLCHAIN_URL" -o /tmp/toolchain.tgz \
 && echo "$TOOLCHAIN_SHA  /tmp/toolchain.tgz" | sha256sum -c - \
 && tar -xzf /tmp/toolchain.tgz -C /opt/trimui/toolchain --strip-components=1 \
 && curl -fsSL "$SDK_URL" -o /tmp/sdk.tgz \
 && echo "$SDK_SHA  /tmp/sdk.tgz" | sha256sum -c - \
 && tar -xzf /tmp/sdk.tgz -C /opt/trimui/sdk \
 && curl -fsSL "$SDL2_URL" -o /tmp/sdl2.tgz \
 && echo "$SDL2_SHA  /tmp/sdl2.tgz" | sha256sum -c - \
 && tar -xzf /tmp/sdl2.tgz -C /opt/trimui/sdl2 \
 && rm -f /tmp/*.tgz

ENV TRIMUI_TOOLCHAIN=/opt/trimui/toolchain
ENV TRIMUI_SDK=/opt/trimui/sdk
ENV PATH=/opt/trimui/toolchain/bin:$PATH

RUN useradd -m -u 1000 builder
USER builder
WORKDIR /src
ENTRYPOINT ["/src/ci/entrypoint.sh"]
```

构建：

```sh
docker build -f ci/Dockerfile.toolchain -t brickdesk-toolchain:20231018 .
docker run --rm -v "$PWD:/src" -w /src brickdesk-toolchain:20231018 \
  ./ci/build-target.sh
```

`ci/build-target.sh` 的核心：

```sh
cmake -S . -B build-target -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake \
  -DBRICKDESK_BACKEND=target \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-target
file build-target/brickdesk
ci/check-glibc.sh build-target/brickdesk
```

**链接 SDK 的坑**（本 orb 已复现）：

- 不要简单 `-L$TRIMUI_SDK/usr/lib`：这个目录里有设备的 `libpthread-2.23.so`，会和工具链的 glibc 2.25 冲突，链接时报 `__libc_dl_error_tsd@GLIBC_PRIVATE`；
- 只把 `libSDL2.so`/`libGLESv2.so` 作为显式输入，或者构建一个“工具链 libc + SDK usr/include + SDK 非 glibc 库”的合并 sysroot；
- 对厂商 blob 的未解析符号用 `-Wl,--allow-shlib-undefined`，但不要全局用它掩盖自己的符号错误；
- 链接后检查 `readelf -d` 的 NEEDED 和 `--version-info` 的 GLIBC 最大值。

`ci/check-glibc.sh`：

```sh
#!/bin/sh
set -eu
BIN=${1:?usage: check-glibc.sh <binary>}
MAX=$(aarch64-linux-gnu-readelf --version-info "$BIN" \
  | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)
echo "max glibc: $MAX"
case "$MAX" in
  GLIBC_2.17|GLIBC_2.18|GLIBC_2.19|GLIBC_2.20|GLIBC_2.21|GLIBC_2.22|GLIBC_2.23) ;;
  *) echo "ERROR: requires $MAX, device has 2.23" >&2; exit 1 ;;
esac
readelf -d "$BIN" | grep NEEDED
```

### 8.3 宿主机无头构建与截图

宿主机依赖：

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libsdl2-dev libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dri xvfb \
  python3 python3-pil python3-numpy
```

运行：

```sh
cmake -S . -B build-host -G Ninja -DBRICKDESK_BACKEND=host -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-host
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy BRICKDESK_HEADLESS=1 \
  ./build-host/brickdesk --scenario tests/scenarios/boot.json
```

截图路径：

1. 应用自己 `glReadPixels` 到 RGBA buffer；
2. `stb_image_write` 写 PNG（零依赖，避免 libpng 版本影响）；
3. 测试模式下固定 1024×768、固定字体、固定 dt；
4. 不用 X 截图工具（`import`/`scrot`），避免窗口管理器差异。

为什么用 `offscreen`：

- 不需要 Xvfb，CI 更简单；
- Mesa 的 `offscreen` 视频驱动使用 EGL surfaceless/pbuffer，llvmpipe 渲染；
- 与 `xvfb-run` 的输出在本 orb 中逐字节一致（见 8.8），可以按环境选择。

`dummy` 驱动不能创建 GL 上下文（`OpenGL support is either not configured...`），只能测非 GL 逻辑，不能用于截图。

### 8.4 截图与输入回放

场景文件（JSON）：

```json
{
  "name": "home_focus_2",
  "size": [1024, 768],
  "fixed_dt_ms": 16,
  "seed": 1,
  "steps": [
    { "type": "wait", "frames": 5 },
    { "type": "key", "key": "right", "action": "down" },
    { "type": "wait", "frames": 2 },
    { "type": "key", "key": "right", "action": "up" },
    { "type": "wait", "frames": 10 },
    { "type": "screenshot", "name": "home_focus_2" },
    { "type": "assert_node", "id": "tile.3", "prop": "focused", "equals": true }
  ]
}
```

- 输入回放直接注入 `bd_key_event`，不经过 evdev；和真实 inputd 共用状态机；
- `fixed_dt_ms` 让动画/滚动可复现；测试模式禁用“按真实时间自动消失”的 toast/hint，或把它们固定为帧数；
- 时钟/电量：`BRICKDESK_FAKE_TIME_MS` + 固定电量，避免基线随时钟变化；
- 语言/字体：`LC_ALL=C.UTF-8`，随包字体，固定 FreeType 版本；字体文件 hash 写进基线元数据；
- 线程：渲染/布局单线程；图片解码可以多线程，但测试模式用同步解码，保证顺序；
- 网络：测试模式禁用；插件测试用本地假 socket；
- 随机：固定 seed；不要用 `rand()` 的默认种子。

场景覆盖（M1 最小集）：

- `boot`：启动后的主页/占位；
- `focus_move`：方向键在磁贴间移动；
- `settings`：进入设置、改一个值、返回；
- `quick_panel`：Select 两次打开快捷面板；
- `menu`：在测试应用上打开应用内菜单；
- `plugin_tile`：hello 插件的磁贴；
- `perf_dry_run`：性能菜单显示。

### 8.5 像素对比与阈值

比较的是**解码后的 RGBA 像素**，不是 PNG 文件字节（PNG 压缩器/版本会变）。

模式：

| 模式 | 用途 | 判定 |
|---|---|---|
| `exact` | CI 默认 | 所有像素完全相同；raw pixel SHA256 相等 |
| `tolerance` | 跨 Mesa/字体版本 | 最大通道差、差异像素比例、SSIM、差异连通区域 |
| `mask` | 无法冻结的动态区域 | 先屏蔽矩形，再 exact/tolerance |

推荐阈值：

- **CI 默认 exact，0 像素差异**。本 orb 验证 llvmpipe 在同机、同驱动下 3 次运行以及 offscreen/xvfb 之间完全一致，说明固定镜像里 exact 是可行的。
- 只有跨版本/跨驱动才用 tolerance：
  - 最大通道差 ≤ 2（0–255）；
  - 差异像素比例 ≤ 0.05%；
  - SSIM ≥ 0.9995；
  - 不允许出现大于 3×3 的连通差异区域（防止文字整体位移被 SSIM 放过）。
- 不要只用 SSIM。实测 1 像素整体位移：AE 19.73%，SSIM 0.9935；如果阈值是 SSIM ≥ 0.99，会误判通过。
- 也不要只看“差异像素比例”。5% 像素 ±1 的噪声：AE 4.77%，但最大差只有 1；这种应该通过。所以要同时看最大差、比例、连通区域。
- 设备截图不和 host 基线比；设备只做“非黑屏/尺寸正确/关键颜色存在”的冒烟，以及人工目检。

基线目录：

```text
tests/baselines/host/
  boot.png
  boot.json          # renderer, mesa, sdl, font hash, scenario hash
  focus_move.png
  ...
tests/baselines/device/   # 可选，人工维护
```

更新基线：

```sh
./scripts/update-baselines.sh home_focus_2
# 生成 build-host/tests/artifacts/home_focus_2.diff.png
```

- 基线更新必须单独提交，并在 PR 里附 diff 图；
- CI 检测到基线变化时，输出旧/新/差异三张图到 artifact；
- 不允许在普通功能 PR 里静默更新基线。

### 8.6 GitHub Actions

`.github/workflows/ci.yml` 草案：

```yaml
name: ci
on:
  push:
  pull_request:

permissions:
  contents: read

concurrency:
  group: ci-${{ github.ref }}
  cancel-in-progress: true

jobs:
  host:
    runs-on: ubuntu-22.04
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@v4
      - uses: docker/build-push-action@v6
        with:
          context: .
          file: ci/Dockerfile.host
          tags: brickdesk-host:ci
          load: true
          cache-from: type=gha
          cache-to: type=gha,mode=max
      - run: docker run --rm -v "$PWD:/src" -w /src brickdesk-host:ci ./ci/run-host-tests.sh
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: host-test-artifacts
          path: |
            build-host/tests/artifacts
            build-host/Testing
          retention-days: 14

  target:
    runs-on: ubuntu-22.04
    timeout-minutes: 30
    steps:
      - uses: actions/checkout@v4
      - uses: docker/build-push-action@v6
        with:
          context: .
          file: ci/Dockerfile.toolchain
          tags: brickdesk-toolchain:20231018
          load: true
          cache-from: type=gha
          cache-to: type=gha,mode=max
      - run: docker run --rm -v "$PWD:/src" -w /src brickdesk-toolchain:20231018 ./ci/build-target.sh
      - run: docker run --rm -v "$PWD:/src" -w /src brickdesk-toolchain:20231018 ./ci/check-glibc.sh build-target/brickdesk
      - run: tar -czf brickdesk-aarch64.tar.gz -C build-target brickdesk
      - uses: actions/upload-artifact@v4
        with:
          name: brickdesk-aarch64
          path: brickdesk-aarch64.tar.gz
          retention-days: 30
```

要点：

- host 测试也放进固定镜像（Debian 12 + Mesa 22.3.6），避免 runner 的 Mesa 版本漂移导致截图不稳定；镜像 digest 固定；
- `target` 只做交叉编译 + glibc 检查 + 打包，不跑设备；
- 设备任务单独 `workflow_dispatch`，使用带 `brickdesk-device` 标签的自托管 runner，不暴露给 fork PR；
- 所有 action 版本固定到 SHA（供应链安全）；文档里用 tag 只为可读；
- 缓存：Docker buildx `gha`、ccache（可选）；不要把 400 MB 的 SDK 放进仓库；
- 产物：host 截图/测试日志、target 二进制、`SHA256SUMS`。

可选 nightly：

- 每天跑一次完整 host 截图 + target 构建；
- 设备 runner 在线时跑冒烟，结果写 artifact；
- 失败通知到 issue/IM（M2）。

### 8.7 设备侧脚本

`scripts/device/`：

| 脚本 | 作用 | 关键命令 |
|---|---|---|
| `deploy.sh` | 推送二进制/配置/资源到 SD | `adb push`、`adb shell sync` |
| `run.sh` | 停/让开 stock UI，启动 brickdesk，日志落盘 | `adb shell` + `LD_LIBRARY_PATH` |
| `logs.sh` | 拉日志、dmesg、logread | `adb pull`、`adb shell logread` |
| `screenshot.sh` | 调应用 `--screenshot`，拉回 PNG | `adb shell`、`adb pull` |
| `perf.sh` | 采样 CPU/RSS/温度/频率/电量 | `/proc`、`top`、sysfs |
| `recover.sh` | 恢复 stock UI/服务 | 启停脚本 |
| `smoke.sh` | 一键部署+启动+截图+退出 | 组合上面 |

`deploy.sh` 示例：

```sh
#!/bin/sh
set -eu
ADB=${ADB:-adb}
DEST=${DEST:-/mnt/SDCARD/brickdesk}
$ADB shell "mkdir -p '$DEST/bin' '$DEST/config' '$DEST/logs'"
$ADB push build-target/brickdesk "$DEST/bin/brickdesk"
$ADB push config/. "$DEST/config/"
$ADB shell "chmod +x '$DEST/bin/brickdesk' && sync"
```

`perf.sh` 采样：

```sh
PID=$($ADB shell pidof brickdesk | tr -d '\r')
$ADB shell "cat /proc/$PID/status | grep -E 'VmRSS|VmPeak'"
$ADB shell "cat /proc/$PID/stat"
$ADB shell "cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq"
$ADB shell "cat /sys/class/thermal/thermal_zone0/temp"
$ADB shell "cat /sys/class/power_supply/*/capacity"
$ADB shell "top -b -n 1 | head -20"
```

注意：

- 设备上不一定有 `top`/`pidof`/`logread`；脚本要探测，失败降级到 `/proc`；
- `adb root` 不一定可用；不要假设；
- 不要直接改 stock 分区；部署只写 SD 卡；
- 提供 `recover.sh`，并在文档里写“如何回到 stock UI”；
- 测试前备份 SD 卡关键配置；
- `adb exec-out screencap` 是 Android 的，Linux 设备没有；用应用自己的截图或 `/dev/fb0`（后者可能拿不到 GPU 扫描输出）。

### 8.8 本 orb 的验证记录

环境：Debian 12，GCC 12，SDL2 2.26.5，Mesa 22.3.6，llvmpipe；无 `/dev/input`、无 `/dev/uinput`、无 GPU、无 Docker 容器能力。

| # | 项目 | 命令 | 结果 |
|---|---|---|---|
| 1 | SDL2 + GLES2 无头 | `SDL_VIDEODRIVER=offscreen ./host_gl 3 out.bmp` | 成功；请求 ES2，Mesa 返回兼容的 `OpenGL ES 3.2` 上下文；`GL_RENDERER=llvmpipe`；截图非黑屏 |
| 2 | 3 次确定性 | 同上，跑 3 次 | BMP MD5 全部 `19eb21666d494790ee6ab1f7cd325d05` |
| 3 | offscreen vs xvfb | `xvfb-run -a -s "-screen 0 1024x768x24" ./host_gl` | 输出 BMP 与 offscreen 完全相同 |
| 4 | `dummy` 驱动 | `SDL_VIDEODRIVER=dummy ./host_gl` | 失败：`OpenGL support is either not configured...`；符合预期 |
| 5 | 裸 EGL surfaceless | `EGL_PLATFORM=surfaceless ./egl_null` | 成功；EGL 1.5 Mesa，GLES 3.2 llvmpipe，像素 25,51,77 |
| 6 | 静止 CPU | 2 秒事件循环，`SDL_Delay(16)` | `cpu_ms=0.000`；RSS 14.4 MB（host 参考） |
| 7 | 交叉工具链 | 下载 20231018 三个包，GCC 7.5 | `aarch64-linux-gnu-gcc 7.5.0` 可运行 |
| 8 | CMake host/target | `cmake -DCMAKE_TOOLCHAIN_FILE=...` | host 与 aarch64 均构建成功；target 为 ELF64 AArch64 |
| 9 | 目标依赖 | `readelf -d` | NEEDED：`libSDL2-2.0.so.0`、`libGLESv2.so.2`、`libc.so.6` |
| 10 | glibc 版本 | `readelf --version-info` | 仅 `GLIBC_2.17`；按 SDK 推断的目标 2.23 是安全的 |
| 11 | SDK SDL2 分辨率 | 源码 + 反汇编 | `MALI_CreateWindow` 硬编码 1280×720；Brick Pro 需 patch |
| 12 | LD_PRELOAD 菜单叠加 | `LD_PRELOAD=libshim.so SDL_VIDEODRIVER=offscreen ./app` | 成功在应用帧上画出面板/5 行菜单；截图已目检 |
| 13 | 图像差异指标 | 合成 1px 位移/噪声 | 1px 位移 AE 19.73%、SSIM 0.9935；5% ±1 噪声 AE 4.77%、max 1 |
| 14 | Docker | `dockerd --storage-driver=vfs` + `docker pull` | 守护进程能起，但 `failed to register layer: unshare: operation not permitted`；容器构建未执行 |

结论：

- **宿主机 SDL2 + GLES2 无头渲染 + 截图是可行的，而且可做到逐像素确定**；
- **交叉编译链可用**，但链接 SDK 时要避开设备 glibc，且必须检查 GLIBC 版本；
- **官方 SDL2 的 1280×720 硬编码是 Brick Pro 的阻塞项**，M1 必须 patch 并在设备上确认；
- **进程内 `LD_PRELOAD` 菜单叠加在宿主机可行**，设备上还要验证 SDL 版本、GL 状态、输入；
- **Docker 在本 orb 不可用**（gVisor 限制），Dockerfile 需要在 GitHub Actions 的真实 runner 上验证；本 orb 已用同样的工具链命令原生产生了 aarch64 二进制；
- 设备侧的 evdev/GPU/显示/温度/频率全部未验证，M1 设备任务必须补齐。

限制：

- 没有 `/dev/input`，无法验证 evdev/grab；
- 没有 `/dev/uinput`，无法验证按键注入；
- 没有 PowerVR，无法验证 EGL NULL WSEGL、显示交接、驱动行为；
- 没有设备，所有设备数字（RSS、帧时间、温度）都是待测。

---

## 9. M1 工程基建任务拆分

### 9.1 M1 的目标与边界

M1 不是“把封面墙做完”，而是**把后续开发的地基和反馈回路搭好**：

- 同一份代码能在宿主机和目标机构建；
- 无头截图 + 输入回放能在 CI 里跑；
- 配置能加载、校验、迁移、写回；
- 输入守护进程能在设备上枚举/识别组合键；
- 应用启动/性能 profile 有最小闭环；
- 插件协议有一个能跑的 hello world；
- 应用内菜单 shim 有一个能跑的 demo；
- 设备部署/日志/性能脚本可用。

M1 不做：

- 完整封面墙交互/动画/主题系统；
- 插件商店/签名/权限 UI；
- 存档管理 UI/快速恢复 UI；
- GPU 调频（先探测，不承诺）；
- 多语言/多分辨率适配（先固定 1024×768/zh-CN）。

### 9.2 任务表

| ID | 任务 | 产出文件 | 验收 | 依赖 |
|---|---|---|---|---|
| M1-01 | 构建骨架（host/target） | `CMakeLists.txt`、`cmake/toolchain-aarch64.cmake`、`cmake/options.cmake`、`src/main.c`、`scripts/build-host.sh`、`scripts/build-target.sh`、`.gitignore` | host `brickdesk --version` 可运行；target 产出 ELF64 AArch64；`ctest` 空跑通过 | — |
| M1-02 | 宿主机平台层 + 最小渲染/截图 | `src/platform/host_sdl.c`、`src/gfx/renderer.c`、`src/gfx/text.c`、`src/gfx/capture.c`、`include/brickdesk/gfx.h`、`tests/smoke_render.c` | `SDL_VIDEODRIVER=offscreen` 下画出 1024×768 非黑屏 PNG；3 次运行像素一致 | M1-01 |
| M1-03 | 设备平台层 + SDL2 patch + 显示探针 | `src/platform/device_sdl.c`、`patches/sdl2/0001-read-fb-mode.patch`、`tools/display_probe.c`、`scripts/build-sdl2.sh`、`docs/device-display.md` | 设备上 `SDL_GetWindowSize()` 和 `eglQuerySurface()` 均为 1024×768；不再出现 1280×720；探针输出 surface/swap 结果 | M1-01 |
| M1-04 | 配置核心 | `third_party/inih/ini.c`、`third_party/inih/ini.h`、`src/config/config.c`、`src/config/schema.c`、`include/brickdesk/config.h`、`config/brickdesk.ini`、`config/perf.ini`、`config/hotkeys.ini`、`tools/brickdesk-config.c`、`tests/config_test.c` | `brickdesk-config check` 通过；坏行报 `file:line`；覆盖层级/原子写/迁移有单测 | M1-01 |
| M1-05 | 输入守护进程 + 组合键状态机 | `src/input/evdev.c`、`src/input/device_map.c`、`src/input/hotkey.c`、`src/input/inputd.c`、`include/brickdesk/input.h`、`tools/bd-input.c`、`tests/hotkey_test.c`、`config/input.ini` | 合成事件单测覆盖 chord/长按/双击/前缀冲突；设备上 `bd-input --list` 打印设备/按键码；菜单 grab/ungrab 冒烟 | M1-04 |
| M1-06 | Session + 应用 manifest + 启动闭环 | `src/session/session.c`、`src/session/manifest.c`、`src/session/app_protocol.c`、`include/brickdesk/app.h`、`apps/demo/app.json`、`apps/demo/main.c`、`scripts/launch-app.sh` | demo 应用能被启动/等待/退出；`app.json` 解析；IPC `app.hello`/`app.pause`/`app.resume` 跑通；perf apply/restore 被调用（fake perfd 或 dry-run） | M1-04、M1-07 |
| M1-07 | 性能守护进程 | `src/perfd/perfd.c`、`src/perfd/policy.c`、`src/perfd/sysfs.c`、`include/brickdesk/perf.h`、`tests/perf_policy_test.c`、`tests/fixtures/sysfs/`、`config/perf.ini` | fake sysfs 单测覆盖 inherit/clamp/min>max/回滚；`--dry-run` 打印计划；设备上 apply/restore 日志正确 | M1-04 |
| M1-08 | 插件宿主 + hello 插件 | `src/plugin/host.c`、`src/plugin/ipc.c`、`src/plugin/ui_tree.c`、`include/brickdesk/plugin.h`、`plugins/hello/plugin.json`、`plugins/hello/main.c`、`plugins/hello/Makefile`、`plugins/hello-py/plugin.json`、`plugins/hello-py/main.py`、`tests/plugin_test.c` | hello 磁贴显示；插件崩溃宿主不崩；握手超时/重启/内存上限有测试 | M1-02、M1-04 |
| M1-09 | 应用内菜单 shim + 菜单 UI | `src/menu/shim/shim.c`、`src/menu/menu.c`、`src/menu/menu_ui.c`、`src/menu/menu_input.c`、`include/brickdesk/menu.h`、`apps/menu_test/main.c`、`apps/menu_test/app.json`、`tests/menu_test.sh` | 宿主机 `LD_PRELOAD` 在 demo 应用帧上画出菜单并截图；设备上 2 个 SDL2 游戏可开/关；禁用/黑名单生效 | M1-05、M1-06 |
| M1-10 | 截图/输入回放测试框架 | `tests/harness/main.c`、`tests/harness/scenario.c`、`tests/harness/capture.c`、`tests/scenarios/boot.json`、`tests/baselines/host/boot.png`、`tools/imagediff.py`、`tests/CMakeLists.txt`、`scripts/update-baselines.sh` | `boot` 截图 exact 通过；人为 1px 位移会失败；diff/old/new 图进 artifact | M1-02、M1-08 |
| M1-11 | CI + Docker | `ci/Dockerfile.toolchain`、`ci/Dockerfile.host`、`ci/entrypoint.sh`、`ci/build-target.sh`、`ci/run-host-tests.sh`、`ci/check-glibc.sh`、`.github/workflows/ci.yml`、`ci/README.md` | PR 上 host/target 两个 job 绿；target 产物可下载；glibc 检查生效；CI 时间 < 15 分钟（缓存后） | M1-01…M1-10 |
| M1-12 | 设备脚本 + 文档 | `scripts/device/deploy.sh`、`run.sh`、`logs.sh`、`screenshot.sh`、`perf.sh`、`recover.sh`、`smoke.sh`、`docs/device-testing.md` | 设备上 deploy→run→screenshot→logs→recover 全流程跑通；不修改 stock 分区 | M1-03、M1-09 |
| M1-13 | 基准与内存预算 | `bench/bench_render.c`、`bench/bench_idle.c`、`scripts/bench.sh`、`docs/performance.md` | 记录 idle CPU、帧时间 p50/p95、RSS、温度；写入文档；后续 PR 可比 | M1-02、M1-12 |
| M1-14 | ADR 与协议文档 | `docs/adr/0001-plugin-model.md`、`docs/adr/0002-config-format.md`、`docs/adr/0003-menu-model.md`、`docs/app-protocol.md`、`docs/plugin-api.md`、`docs/config-reference.md` | 决策、接口、示例与本文一致；链接可点；新人能按文档写 hello 插件 | M1-04、M1-08、M1-09 |

### 9.3 建议顺序

```text
第 1 周：M1-01 -> M1-02 -> M1-04 -> M1-03（设备 patch 并行）
第 2 周：M1-05 -> M1-07 -> M1-06
第 3 周：M1-08 -> M1-09 -> M1-10
第 4 周：M1-11 -> M1-12 -> M1-13 -> M1-14
```

并行边界：

- M1-03 依赖设备/工具链，可和第 1 周其他任务并行；
- M1-05（inputd）和 M1-07（perfd）互不依赖，可并行；
- M1-08（插件）和 M1-09（菜单）可以并行，但都需要 M1-04 的配置；
- M1-10 依赖 M1-02 的截图能力和 M1-08 的插件磁贴；
- M1-11 最后做，但 CI 的基本 host 构建可以第 1 周就先上，后续增量加 job。

### 9.4 M1 文件树

```text
CMakeLists.txt
cmake/
  toolchain-aarch64.cmake
  options.cmake
src/
  main.c
  platform/
    host_sdl.c
    device_sdl.c
  gfx/
    renderer.c
    text.c
    capture.c
  config/
    config.c
    schema.c
  input/
    evdev.c
    device_map.c
    hotkey.c
    inputd.c
  session/
    session.c
    manifest.c
    app_protocol.c
  perfd/
    perfd.c
    policy.c
    sysfs.c
  plugin/
    host.c
    ipc.c
    ui_tree.c
  menu/
    menu.c
    menu_ui.c
    menu_input.c
    shim/shim.c
include/brickdesk/
  gfx.h
  config.h
  input.h
  app.h
  perf.h
  plugin.h
  menu.h
third_party/
  inih/
  jsmn/
  stb/
config/
  brickdesk.ini
  perf.ini
  hotkeys.ini
  input.ini
  local.ini
apps/
  demo/
  menu_test/
plugins/
  hello/
  hello-py/
tests/
  harness/
  scenarios/
  baselines/host/
  config_test.c
  hotkey_test.c
  perf_policy_test.c
  plugin_test.c
  fixtures/sysfs/
tools/
  brickdesk-config.c
  bd-input.c
  display_probe.c
  imagediff.py
ci/
  Dockerfile.toolchain
  Dockerfile.host
  entrypoint.sh
  build-target.sh
  run-host-tests.sh
  check-glibc.sh
scripts/
  build-host.sh
  build-target.sh
  build-sdl2.sh
  update-baselines.sh
  bench.sh
  device/
docs/
  research/architecture.md
  adr/
  app-protocol.md
  plugin-api.md
  config-reference.md
  device-testing.md
  performance.md
```

### 9.5 M1 退出标准

必须全部满足：

- [ ] GitHub Actions 上 host job 绿：构建、单测、`boot` 截图 exact；
- [ ] target job 绿：Docker 工具链构建、交叉编译、`GLIBC <= 2.23` 检查、产物上传；
- [ ] host 截图 3 次一致；人为 1px 位移会失败；
- [ ] 配置：`brickdesk-config check` 通过，坏配置有行号，`local.ini` 原子写；
- [ ] inputd：合成事件单测通过；设备上能列出按键；菜单 grab/ungrab 不粘键；
- [ ] perfd：fake sysfs 单测通过；设备上 `--dry-run` 和一次真实 apply/restore 成功；
- [ ] 插件：hello 磁贴显示；插件崩溃不影响桌面；
- [ ] 菜单：宿主机 shim 截图通过；设备上至少 2 个 SDL2 应用可开/关菜单；
- [ ] 设备：deploy→run→screenshot→logs→recover 全流程；
- [ ] 文档：本文 + ADR + 协议/配置/设备文档齐全，链接可点。

不满足时不上 M2（封面墙完整实现）。

---

## 10. 风险、待验证项与需要拍板的选项

### 10.1 主要风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 官方 SDL2 1280×720 硬编码 | Brick Pro 画面拉伸/黑边/黑屏 | 源码重建 + fb0 尺寸 patch；设备探针；CI 不覆盖此项，必须设备冒烟 |
| EGL NULL WSEGL 是否允许多进程 surface | 菜单 C 模式不可用 | M1 `display_probe` 先测；不可用则 A/B/D |
| shim 的 GL 状态污染 | 游戏花屏/崩溃 | 完整 save/restore；真实游戏测试；按应用黑名单；禁用开关 |
| 应用自己 grab 输入 | 菜单收不到键 | manifest 声明；应用内集成；D 兜底 |
| inputd grab/ungrab 粘键 | 游戏按键卡住 | 等所有键释放再 ungrab；超时提示；测试矩阵 |
| GPU 频率无接口 | 无法按预期调 GPU | 只做探测/只读展示；不承诺；CPU 调优为主 |
| 设备 glibc 2.23 vs 工具链 2.25 | 运行时符号缺失 | CI 检查最大 GLIBC；避免新 API；设备冒烟 |
| SD 卡路径/权限不明 | 配置/存档写失败 | 多候选路径探测；只读时 tmpfs + 提示；设备确认 |
| stock UI/init 冲突 | 两个 UI 抢显示 | 不直接改 stock；手动/脚本停 stock；recover.sh；M2 再做无缝集成 |
| 插件以 root 运行 | 安全风险 | 专用 UID/降权；能力代理；安装提示；M3 seccomp |
| 1 GB 内存不足 | OOM/卡顿 | 插件预算 128 MB；LRU；磁贴 15 fps；大图缓存上限；快速恢复只留一个 |
| Docker 在本 orb 不可用 | 本地无法验证容器 | CI 真实 runner 验证；本 orb 原生命令验证；Dockerfile 固定 SHA |

### 10.2 需要用户拍板的选项

| # | 选项 | 推荐 | 备选 | 影响 |
|---|---|---|---|---|
| 1 | 插件模型 | 进程外 + JSON + 声明式 UI | `.so`；Lua | 决定 SDK、安装包、安全模型、内存预算 |
| 2 | 菜单主路径 | `LD_PRELOAD` 进程内叠加优先 | 暂停+显示交接优先；只做嵌入库 | 决定兼容性范围和 M1/M2 工作量 |
| 3 | 配置格式 | INI + JSON | TOML；全 JSON | 决定解析库、UI 写回、用户手改体验 |
| 4 | 性能范围 | CPU + 内存/IO 先行，GPU 只探测 | 必须调 GPU | 决定是否能承诺“GPU 调频” |
| 5 | CI 镜像托管 | GHCR 公开/私有包 | 每次现建；自托管 registry | 决定 CI 速度和仓库权限 |
| 6 | 默认菜单热键 | Menu 长按 600 ms | Menu+Select；Select 双击 | 决定与游戏的冲突程度 |
| 7 | 存档配额 | 512 MB/应用 | 1 GB；不限制 | 决定 SD 空间和 LRU 策略 |
| 8 | 兼容现有 pak | 先做导入器（M2），不直接运行 | 完全兼容 NextUI/spruce pak | 决定生态迁移成本 |
| 9 | stock UI 集成 | M1 手动切换，M2 做启动器替换 | 一开始就替换 | 决定变砖风险 |

### 10.3 交给设备调研线程的输入

- sysfs：cpufreq policy 数量、可用 governor/频率、online、thermal、battery、backlight、ALSA；
- 输入：`/dev/input` 设备列表、Menu/Power/Volume 的 key code、SDL 是否 grab；
- 显示：`/dev/fb0` 的 `FBIOGET_VSCREENINFO`、EGL vendor/version/extensions、`eglCreateWindowSurface` 行为、`/dev/disp` 是否存在；
- 系统：glibc 版本、内核版本、cgroup v2、pidfd、memfd、user namespace、SD 挂载点/权限、init 体系；
- 基线：默认 governor/频率、空闲温度、电量、启动时间、stock UI 内存。

这些结果回来后，更新本文的 1.1、4.9、5.14、6.7 和 8.8。

---

## 11. 参考资料

- brickdesk 仓库：<https://github.com/4fuu/brickdesk>
- TrimUI 官方工具链/SDK：<https://github.com/trimui/toolchain_sdk_smartpro>
- NextUI Paks 文档：<https://nextui.loveretro.games/paks/making-paks>
- NextUI `PAKS.md`：<https://github.com/LoveRetro/NextUI/blob/main/PAKS.md>
- muOS 前端与启动：<https://muos.dev/docs/infrastructure/launching>
- muOS 应用运行器说明：<https://community.muos.dev/t/application-runner/1282>
- OnionOS 文档：<https://onionui.github.io/docs>
- OnionOS FAQ（App/config.json/launch.sh 实例）：<https://onionui.github.io/docs/faq>
- Batocera Ports：<https://wiki.batocera.org/systems:ports>
- Knulli 主题（EmulationStation）：<https://knulli.org/configure/customization/themes/>
- EmulationStation 主题格式：<https://github.com/Aloshi/EmulationStation/blob/master/THEMES.md>
- linux-sunxi A133：<https://linux-sunxi.org/A133>
- Ebitengine issue #3491（TrimUI EGL/NULL WSEGL 调研）：<https://github.com/hajimehoshi/ebiten/issues/3491>
- SDL2 `mali-fbdev` 驱动源码：官方 `SDL2-2.26.1.GE8300.tgz` 内 `src/video/mali-fbdev/`
- Mesa llvmpipe：<https://docs.mesa3d.org/drivers/llvmpipe.html>
- inih：<https://github.com/benhoyt/inih>
- jsmn：<https://github.com/zserge/jsmn>
- stb_image_write：<https://github.com/nothings/stb>
- GitHub Actions 文档：<https://docs.github.com/actions>
- Android ADB 文档：<https://developer.android.com/tools/adb>

---

## 附录 A：关键决策一句话版

- 插件：**独立进程，JSON 协议，宿主画 UI，磁贴共享内存。**
- 热键：**inputd 被动读，菜单时 grab，必要时才 grab+uinput。**
- 菜单：**前台进程内叠加优先；不能叠加就暂停交接；都不行就只做 inputd 动作。**
- 性能：**perfd 单写者，白名单，租约，回滚，CPU 为主。**
- 配置：**INI 给人，JSON 给机器，SD 分层，local.ini 只写改动。**
- 测试：**Docker 交叉编译 + 宿主机 llvmpipe 截图 + GitHub Actions；设备只做冒烟和性能。**
- M1：**先修 SDL2 分辨率，再搭双构建、截图、配置、inputd、perf、hello 插件、菜单 shim、CI、设备脚本。**

## 附录 B：本文中用到的术语

| 术语 | 含义 |
|---|---|
| pak | 进程式插件包；参考 NextUI/spruce 的目录 + launch 脚本 |
| shim | `LD_PRELOAD` 进前台应用的菜单叠加库 |
| handoff | 应用释放显示，菜单进程整屏接管 |
| damage/dirty rect | 只重绘发生变化的矩形区域 |
| profile | 一组性能参数（governor/频率/核数/内存参数） |
| lease | perfd 给会话的租约；会话崩溃后到期自动恢复 |
| capability | 应用声明的可选能力（暂停/恢复/存档/后台/显示释放） |
| hook | 应用为某个能力提供的 IPC 方法或命令 |
