# 设备测试（Brick Pro / TG4040）

> **状态：全部未在真机验证。** 设备尚未到货；本文件中的命令、路径和预期结果来自
> `docs/research/architecture.md` 与 `docs/research/device-features.md` 的调研，
> 以及桌面/CI 侧的交叉编译验证。拿到设备后按本文顺序执行，并把实际输出补回本文
> 或对应调研文档。

本文覆盖 M1-03（SDL2 分辨率 patch、显示探针）、M1-12（设备脚本）和 M8-A
（发布包、安装/卸载、启动回退）。它不修改原厂分区：所有部署文件都在 SD 卡的
`/mnt/SDCARD/brickdesk/` 下，启动钩子只新增
`/mnt/SDCARD/System/starts/zz_brickdesk.sh`。

## 1. 组件与完成度

| 组件 | 位置 | 已验证 | 待真机确认 |
|---|---|---|---|
| 官方工具链 + SDK 镜像 | `ci/Dockerfile.toolchain` | GitHub Actions 构建、GHCR 推送 | — |
| CMake 工具链文件 | `cmake/toolchain-aarch64.cmake` | 独立 CMake 冒烟工程 + 真正 `brickdesk` 目标交叉编译 | 设备上运行 `brickdesk` |
| SDL2 fb0 分辨率 patch | `patches/sdl2/0001-read-fb-mode.patch` | 源码可应用、aarch64 共享库可构建、GLIBC 2.17 | `SDL_GetWindowSize()`/EGL surface 是否 1024×768 |
| 显示探针 | `tools/display_probe.c` | aarch64 交叉编译、`file`/`readelf` 检查 | fb0/EGL/GL/swap 的真实值 |
| 部署/运行/日志/截图/性能/恢复/冒烟 | `scripts/device/` | shellcheck + 假 adb 全流程 | 真实 adb、真实 SD 路径、原厂进程 |
| 发布包 + 安装/卸载 + 启动钩子 | `scripts/package-target.sh`、`scripts/package/` | 假 target 构建、假 SD 根目录、升级/回退/卸载测试 | `System/starts` 语义、真机启动/回退/恢复 |
| 交叉编译 CI | `.github/workflows/target.yml` | GitHub Actions 绿灯（见最新 push） | — |

## 2. 构建与部署

### 2.1 用 CI 镜像

镜像由 `.github/workflows/target.yml` 构建并推送到：

```text
ghcr.io/4fuu/brickdesk-toolchain:20231018
```

每个 push 还会推一个 `sha-<full commit>` 标签。CI 在本仓库根目录执行：

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -w /src ghcr.io/4fuu/brickdesk-toolchain:20231018 \
  ./scripts/build-target.sh
```

产物：

```text
build-target/brickdesk
build-target/brickdesk-inputd
build-target/brickdesk-perfd
build-target/bd-input
build-target/brickdesk-config
build-target/brickdesk-demo
build-target/display_probe
build-target/bench/bench_render
build-target/libbrickdesk-menu.so
build-target/dist/bin/...
build-target/dist/lib/libSDL2-2.0.so.0 -> libSDL2-2.0.so.0.2600.1
build-target/glibc-report.txt
build-target/package/brickdesk-<version>-aarch64.zip
build-target/package/SHA256SUMS
```

`scripts/build-target.sh` 会优先使用 `/opt/trimui/sysroot` 里预装好的 patched SDL2；
没有时才调用 `scripts/build-sdl2.sh`。它随后用 `cmake/toolchain-aarch64.cmake` 和
`-DBRICKDESK_BACKEND=target` 构建目标（当前 target 后端暂时复用
`src/platform/host_sdl.c`），把桌面、两个守护进程、工具、demo、菜单 shim、
`display_probe` 和 `bench_render` 一起放进 `dist/bin/`，并对它们和 SDL2 跑
glibc 检查。`scripts/package-target.sh` 再把 `dist/`、assets、config、plugins、
apps 和安装脚本打成版本化 zip。

### 2.2 本机直接交叉编译

前提：官方 `aarch64-linux-gnu-7.5.0-linaro` 解压到 `/opt/trimui/toolchain`，
`SDK_usr_tg5040_a133p` 解压到 `/opt/trimui/sdk`，SDL2 源码解压到
`/opt/trimui/sdl2-src/SDL2-2.26.1`。

```sh
scripts/build-sdl2.sh --prefix /opt/trimui/sysroot
scripts/build-target.sh
file build-target/brickdesk build-target/brickdesk-inputd build-target/bench/bench_render
aarch64-linux-gnu-readelf --version-info build-target/brickdesk \
  | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1
scripts/package-target.sh --build-dir build-target
```

预期：所有二进制都是 `ELF 64-bit LSB executable, ARM aarch64`，最高 `GLIBC_2.17`
（上限 2.23）。SDL2 共享库也应是 `GLIBC_2.17`。
只构建探针可用 `scripts/build-target.sh --skip-cmake`。

### 2.3 工具链文件

`cmake/toolchain-aarch64.cmake` 不依赖 brickdesk 的 CMakeLists，可直接给任何 CMake
工程使用：

```sh
cmake -S . -B build-target -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
```

可用环境变量覆盖路径：`TRIMUI_TOOLCHAIN`、`TRIMUI_SDK`、`TRIMUI_SYSROOT`。
CMake 项目中可用 `BRICKDESK_TARGET_SYSROOT`、`TRIMUI_SDK_INCLUDE_DIR`、
`TRIMUI_SDK_LIB_DIR`。工具链文件把 SDK include 加为系统 include，并把 SDK lib
放进 `CMAKE_FIND_ROOT_PATH`，但**不**把 `-L$SDK/usr/lib` 加到链接命令行，
避免链接到 SDK 里的 `libpthread-2.23.so`/`libdl-2.23.so` 与工具链 glibc 2.25
冲突。

## 3. 连接设备

```sh
adb devices
adb shell uname -a
adb shell 'mount | grep -E "SDCARD|UDISK"'
```

确认：

- `adb devices` 里设备状态是 `device`，不是 `offline`/`unauthorized`。
- `/mnt/SDCARD` 已挂载且可写；下面所有部署只写这个目录。
- `adb root` 不一定可用；脚本不依赖 root，失败时只影响 `dmesg` 等可选采集。

先备份 SD 卡上已有的 `brickdesk/` 配置（如果用户已经装了其它前端，也备份 `Apps/`）：

```sh
adb pull /mnt/SDCARD/brickdesk /tmp/sdcard-brickdesk-backup 2>/dev/null || true
adb pull /mnt/SDCARD/Apps /tmp/sdcard-apps-backup 2>/dev/null || true
```

## 4. 设备到货后第一次要跑的命令

以下命令按“先能启动/显示，再输入和电源，最后外设/性能”的顺序。每条都写明要确认
什么；没写“未验证”的结论都只是调研推断，不能当成事实。

### 4.1 型号、固件、系统

```sh
adb shell 'cat /etc/model'
adb shell 'cat /etc/version'
adb shell 'uname -a'
adb shell 'cat /proc/version'
adb shell 'cat /proc/device-tree/model; echo; cat /proc/device-tree/compatible; echo'
adb shell 'strings /usr/trimui/bin/MainUI | grep "^Trimui" | head'
adb shell 'ldd --version | head -1'
adb shell 'ls -l /lib/libc.so.6 /lib/libpthread.so.0 2>/dev/null'
```

确认：

- `/etc/model` 是 `TG4040`（Brick Pro），不是 TG4070/Smart Pro。
- 内核是 4.9.191、aarch64，与调研一致。
- device-tree 是 `allwinner,a133` / `sun50iw10` 家族。
- `ldd --version` 的设备实际 glibc。若高于 2.23，把 CI 的
  `ci/check-glibc.sh --max` 和本文上限一起调整；若低于 2.23，保持 2.23 保守值。
- `libSDL2` 的实际版本和路径；原厂 `/usr/trimui/lib/libSDL2-2.0.so.0.3000.8` 是
  2.30.8，和我们要部署的 2.26.1 patched 版本不同，确认 `LD_LIBRARY_PATH` 优先级。

### 4.2 显示（M1-03 核心）

部署并跑探针：

```sh
scripts/device/deploy.sh
scripts/device/run.sh --foreground display_probe
```

或一键：

```sh
scripts/device/smoke.sh
```

确认探针输出中的每一行：

| 输出 | 预期 | 不满足时说明 |
|---|---|---|
| `fb0_res` | `1024x768` | 面板/DTB 或旋转不是调研值 |
| `fb0_bpp` | 16 或 32 | 记录实际格式；截图脚本按格式解码 |
| `fb0_rotate` | 记录实际值 | 原厂 MainUI 提到 `rotate 270`；确认方向 |
| `sdl_video_driver` | `mali` | 不是 `mali` 说明 SDL 走了别的后端 |
| `sdl_window_size` | `1024x768` | 出现 `1280x720` 说明还在用未 patch 的 SDL2 |
| `egl_vendor` | Imagination/PowerVR | 记录实际字符串 |
| `egl_version` | 1.4 左右 | 不支持 1.5 是正常 |
| `egl_surface_size` | `1024x768` | 与 window/fb0 三者必须一致 |
| `EGL_EXT_buffer_age: yes` | 有则可用于脏矩形 | 没有则只能整帧重绘 |
| `EGL_KHR_swap_buffers_with_damage: yes` | 有则可用 damage | 没有则整帧提交 |
| `egl_proc_swap_with_damage` | 记录 yes/no | 与扩展字符串交叉验证 |
| `egl_buffer_age` | 若支持，通常 1–3 | 记录实际值 |
| `gl_renderer` | `PowerVR GE8300` 一类 | 不是则 GL 上下文不对 |
| `gl_version` | OpenGL ES 2.0/3.x | 设计只依赖 GLES2 特性 |
| `swap_ms_p50` | 接近 16.7（60 Hz）或更低 | 持续 >100 ms 视为阻塞级性能问题 |
| `RESULT` | `PASS` | 任一项 FAIL 先查本节 |

其他显示检查（来自调研，未验证）：

```sh
adb shell 'cat /sys/class/graphics/fb0/virtual_size; cat /sys/class/graphics/fb0/rotate 2>/dev/null'
adb shell 'fbset 2>/dev/null | head -20'
adb shell 'ls -l /dev/disp 2>/dev/null; ls -l /sys/class/backlight 2>/dev/null'
adb shell 'ls -l /sys/class/disp/disp/attr 2>/dev/null; ls -l /sys/devices/virtual/disp/disp/attr 2>/dev/null'
```

确认：

- 屏幕原生方向：`virtual_size` 是 1024×768 还是 768×1024；`rotate` 是否存在。
- `/dev/disp` 可打开；`ioctl 0x102` 的亮度范围（原厂 0–255）是否能实际改变背光。
- `/sys/class/backlight/*` 是否存在；如果存在，优先用它，避免写厂商 ioctl。
- 亮度 0/1/8/255 是否出现“0 仍微亮”或“1 全黑”；恢复时是否需要先写 8。
- `eglSwapInterval(1)` 是否生效，快速横向滚动是否撕裂。

截图验证（fb0 读回）：

```sh
scripts/device/screenshot.sh -o /tmp/brickpro-fb.png
```

确认：

- 生成文件尺寸是 1024×768。
- 画面不是全黑、不是纯色；如果全黑，先确认 `/dev/fb0` 是否真的是 GPU 扫描输出
  （调研已提示在某些 PowerVR 配置下不是），再用应用自身截图路径。
- 方向与面板一致；不一致记录为旋转问题，供后续布局使用。

### 4.3 输入

```sh
adb shell 'cat /proc/bus/input/devices'
adb shell 'ls -l /dev/input/event*'
adb shell 'command -v evtest || echo no-evtest'
adb shell 'cat /sys/class/gpio/gpio243/value 2>/dev/null'
adb shell 'ls -l /tmp/trimui_inputd/ 2>/dev/null'
```

逐键确认（有 `evtest` 时）：

- A/B/X/Y/L/R/Select/Start/Menu/Home/F1/F2/L4/R4、电源、音量±、十字键、双摇杆、
  L3/R3 都产生正确的 `type/code/value`。
- Menu 是 316 还是 315；Home 是 102 还是 15；记录实际值。
- FN 拨动开关：`gpio243` 的 0/1 极性，以及是否同时产生 `SW_TABLET_MODE` (EV_SW 1)。
- 耳机插入是否产生 `SW_HEADPHONE_INSERT` (EV_SW 2)，插入后 `amixer -c 0 sget Headphone`
  是否从 0 变 3，扬声器是否静音。
- 游戏运行时，`trimui_inputd`/`keymon` 是否 grab 物理设备；全局音量/亮度是否双份输入。
- `/tmp/trimui_inputd/grab` 由谁创建、何时删除。
- 长按/组合键（Menu+音量、Select+音量、Menu+Select、Menu+Start、电源 1/2/6 s）
  是否有重复、粘键、丢失。
- 挂起/恢复后 evdev fd 是否失效，是否需要重新枚举；inputd 被 kill 后 supervisor
  是否重启。

### 4.4 电源、电池、休眠

```sh
adb shell 'cat /sys/power/state; cat /sys/power/mem_sleep'
adb shell 'cat /sys/class/power_supply/*/capacity 2>/dev/null'
adb shell 'cat /sys/class/power_supply/*/status 2>/dev/null'
adb shell 'ls /sys/class/power_supply/ 2>/dev/null'
adb shell 'cat /sys/bus/i2c/drivers/axp2202-usb/*/online 2>/dev/null'
```

确认：

- 是否支持 `mem`，是 `s2idle` 还是 `deep`；一次深睡/唤醒耗时、电流、发热。
- 深睡 20 次后 Wi-Fi/BT/音频/背光/输入是否恢复；是否出现 `wpa_supplicant` 卡死、
  `hci0` 消失、ALSA 无声。
- 充电/USB 插入是否阻止自动休眠；`axp2202-usb/online` 和 `time_to_full_now`
  是否能区分“充电”和“USB 连接电脑”。
- 标准 `poweroff` 是否出现 limbo（黑屏但未断电、插 USB 无反应）；PMIC 断电序列。
- 电源键 1/2/6/10 s 的实际动作；`pmu_powkey_off_time=10s` 与 keymon 6 s 谁先生效。
- 电量计：标称 5000 mAh 但 DTB `pmu_battery_cap` 为 3000，满充/放空各测一次，
  记录 `capacity` 曲线；低电量到 0% 是否还能安全关机。

### 4.5 音频、LED、震动

```sh
adb shell 'amixer -c 0 contents 2>/dev/null | head -80'
adb shell 'tinymix contents 2>/dev/null | head -80'
adb shell 'cat /sys/class/speaker/mute 2>/dev/null'
adb shell 'cat /proc/asound/cards'
adb shell 'ls /sys/class/led_anim/ 2>/dev/null'
adb shell 'ls /sys/class/motor/ 2>/dev/null'
```

确认：

- `digital volume`、`DAC volume`、`Headphone`、`HpSpeaker Switch`、`DAC Swap`
  的实际控件名、范围、方向；`tinymix set 9 1`/`1 0` 对应哪个控件。
- 扬声器 0 音量时 `/sys/class/speaker/mute` 是否必须写 1 才能消除底噪。
- 耳机插入后扬声器是否物理静音。
- 蓝牙 A2DP：`bluealsa` control 名、`amixer -D bluealsa` 是否可调、切回扬声器是否恢复。
- USB DAC：卡名、`PCM/Playback + Volume` 控件、OTG 供电是否足够。
- `led_anim` 完整节点：`m/lr/f1/f2/rear` 对应哪颗物理灯；`effect_*` 编号；
  `max_scale_*` 范围；`frame_hex` 格式；低电量/充电/睡眠灯是否冲突。
- 马达 `/sys/class/motor/voltage` 安全范围（2.5/3.3 V 的强度、噪音、发热）；
  GPIO227 极性；长时间震动是否掉压/重启。

### 4.6 网络、时间、性能

```sh
adb shell 'lsmod | grep -i -E "xradio|aic|rtl"'
adb shell 'ls /sys/bus/sdio/devices/*/modalias 2>/dev/null'
adb shell 'hciconfig -a 2>/dev/null'
adb shell 'wpa_cli -p /etc/wifi/sockets -i wlan0 status 2>/dev/null'
adb shell 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors 2>/dev/null'
adb shell 'cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies 2>/dev/null'
adb shell 'ls /sys/class/thermal/'
adb shell 'ls /sys/class/devfreq/ 2>/dev/null'
```

确认：

- Wi-Fi 是 XR829 还是 RTL8821CS；`lsmod`/`modalias`/`dmesg` 三者一致。
- 蓝牙 `hciattach` 还是 `hciattach_aic` 能起 `hci0`；`rfkill0/state` 极性。
- `wpa_supplicant` 的 `ctrl_interface` 路径；`udhcpc` 是否自动拿 IP。
- CPU policy 数量、可用 governor/频率、`cpu1..3/online` 是否可写；写
  `scaling_min_freq`/`max_freq` 的顺序是否造成瞬时超频。
- thermal zone 0/1/2 分别是什么；满载是否降频；GPU 是否有 devfreq 或
  `/sys/kernel/debug/pvr/*`，厂商 “GPU Max Frequency” 实际写哪个节点。
- `hwclock -u -w` 是否写进 AXP2202 RTC；断网重启后时间是否保持；
  `uci system.ntp.enable` 是否真的控制 `ntpd`。

### 4.7 启动、存储、恢复

```sh
adb shell 'ls -l /etc/init.d/runtrimui /usr/trimui/bin/runtrimui.sh 2>/dev/null'
adb shell 'pidof MainUI; pidof runtrimui.sh; pidof trimui_inputd; pidof keymon'
adb shell 'ls -l /tmp/cmd_to_run.sh /tmp/program_to_run /tmp/.cmdenc 2>/dev/null'
adb shell 'mount | grep -E "SDCARD|UDISK"'
adb shell 'df -h /mnt/SDCARD /mnt/UDISK 2>/dev/null'
adb shell 'getprop 2>/dev/null | head; adbd --version 2>/dev/null'
```

确认：

- `/etc/init.d/runtrimui` 是否由 rcS 调用；`runtrimui stop/start` 是否能可靠
  停/起 MainUI（`recover.sh` 依赖它）。
- `MainUI` 退出码；`/tmp/cmd_to_run.sh` 的写入者/格式；`premainui.sh`/`preload.sh`
  对 governor、DPad 模式做了什么。
- `/mnt/SDCARD` 文件系统（FAT32/ext4）、挂载选项、写权限；`/mnt/UDISK` 剩余空间。
- `system.json` 的 schema/默认值。
- 替换 MainUI 后，`keymon`/`trimui_inputd`/`hardwareservice`/`trimui_osdd` 是否仍需
  由原厂脚本启动。
- ADB/SSH 是否默认启动；串口/日志位置。
- 拔卡/只读 SD 的恢复：`runtrimui.sh` 的 `e2fsck` 逻辑是否生效；brickdesk 的
  `recover.sh` 是否能在只读 SD 下至少启动原厂 UI。

### 4.8 Benchmark and memory (M1-13)

The host benchmark and budgets are documented in `docs/performance.md`. The
following device checks are still unverified because no target `bench_render`
is staged yet.

- [ ] Cross-build `bench_render` for aarch64, place it in
  `$DEVICE_ROOT/bin`, deploy `assets/` (fonts and covers) next to it, and add
  the binary to `ci/check-glibc.sh` in the target workflow. The JSON schema is
  device-ready.
- [ ] `scripts/device/bench.sh --scenario idle --idle-ms 3000`: expect
  `presented=0` and `cpu_ms` near zero in the JSON.
- [ ] While the desktop runs, `scripts/device/perf.sh -n 30 -i 1 -o
  /tmp/perf.csv`: record `VmRSS`, `VmHWM`, CPU%, CPU frequency and temperature.
- [ ] `--scenario focus`, `pan`, `blur` at 120 frames: record avg/p50/p95/max,
  draw calls and repainted pixels. Do not compare absolute host llvmpipe
  timings; compare only the relative shape and device-to-device runs.
- [ ] `--scenario wall5000`: confirm visited items stay in the dozens (host:
  42-72) and resident texture bytes stay below the configured 96 MiB.
- [ ] `--scenario plugin15`: this is host-side upload/repaint only. Repeat with
  the real plugin host and the hello plugin to include IPC and plugin CPU.
- [ ] `--scenario settings`: full-screen overlay; check p95 and temperature
  after a 5-minute loop.
- [ ] Read renderer accounting from JSON: `font_data_bytes`,
  `glyph_atlas_bytes`, `fbo_bytes`, `texture_cache_bytes`. Set the device RSS
  ceiling from the measured `VmHWM`, not the host 320 MiB guard.
- [ ] Confirm `/proc/self/clear_refs` peak reset works on the device kernel;
  if `peak_reset=false`, treat `rss_peak_kb` as process-lifetime/sampled peak.
- [ ] `bench_render` uses `glFinish()`. For present latency use
  `display_probe`'s `swap_ms_p50`, not the bench frame time.

Host reference (llvmpipe, not a device budget): idle 0 ms CPU / 0 presents,
focus 4.7 ms avg, pan 28.9 ms, blur 8.3 ms, wall5000 28.0 ms, plugin15 4.3 ms,
settings 44.2 ms, steady RSS about 235-245 MiB with most of it in Mesa/LLVM.

## 5. 设备脚本

所有脚本都用 `ADB`（默认 `adb`）和 `ANDROID_SERIAL` 选择设备；默认部署目录
`/mnt/SDCARD/brickdesk`。

| 脚本 | 作用 | 关键行为 |
|---|---|---|
| `deploy.sh` | 推送 `display_probe`、可选的 `brickdesk`、patched `libSDL2-2.0.so.0`、assets 和默认 config | 只写 SD 卡应用目录；不覆盖 `config/local.ini`；`sync` |
| `run.sh` | 停原厂 UI，启动二进制；`--foreground` 直接跑 | 日志 `$DEST/logs/<name>.log` |
| `logs.sh` | 拉应用日志、dmesg、logread、系统信息 | 默认输出 `build-device/logs` |
| `screenshot.sh` | 通过 `display_probe --dump-fb` 读 fb0，拉回 PPM/PNG | 需要应用不占用 fb0 或能读 |
| `perf.sh` | 采 CPU/RSS/频率/温度/电量 CSV | 不依赖 `top`/`pidof` |
| `bench.sh` | 运行已部署的 `bench_render` 并拉回 JSON | 发布包已包含 target 版 `bench_render` |
| `recover.sh` | 杀掉 brickdesk/display_probe，启动原厂 UI | 不修改原厂分区 |
| `smoke.sh` | deploy → probe → logs → recover | 失败也会尝试恢复 |

典型流程：

```sh
scripts/device/deploy.sh
scripts/device/smoke.sh
scripts/device/perf.sh -n 30 -i 1 -o /tmp/perf.csv
scripts/device/logs.sh -o /tmp/brickpro-logs
scripts/device/recover.sh
```

`smoke.sh` 会在结束时调用 `recover.sh`，即使探针失败。若真机恢复失败，先手动执行：

```sh
adb shell '/etc/init.d/runtrimui start'
# 或
adb shell 'nohup /usr/trimui/bin/runtrimui.sh >/tmp/runtrimui.log 2>&1 &'
```

两条恢复路径都未在真机验证；优先使用原厂 init 脚本。

### 5.1 假 adb 测试

没有设备时验证脚本本身：

```sh
scripts/device/test/run-tests.sh
```

它会把 `scripts/device/test/fake-adb` 放到临时 PATH 的最前面，跑
deploy/run/screenshot/logs/perf/recover/smoke，并检查：

- 所有脚本通过 `shellcheck -x`；
- `push` 目标只在 `/mnt/SDCARD/brickdesk/` 下；
- 探针输出、日志、性能 CSV、smoke 结果都存在。

这只证明脚本逻辑和路径正确，不证明任何真实设备行为。`package-tests.sh` 还会用
假 target 构建跑 `scripts/package-target.sh`，然后在假 SD 根目录里执行
install → 升级 → 健康启动 → 3 次崩溃回退 → 重置 → 卸载 → purge，并检查
`local.ini`/`state/` 是否保留、原厂钩子是否恢复。

### 5.2 发布包、安装、升级、回退、卸载（M8-A）

#### 5.2.1 先确认原厂 `System/starts` 语义

调研文档没有记录这个机制。当前实现依据的是 TrimUI 原厂 Smart Pro rootfs 的公开
说明：`/usr/trimui/bin/runtrimui.sh` 会执行 `/mnt/SDCARD/System/starts` 下的脚本；
CrossMix 也把固件检查放在这个目录。Brick Pro 1.1.1 上必须逐项确认：

```sh
adb shell 'ls -l /mnt/SDCARD/System/starts/'
adb shell 'cat /mnt/SDCARD/System/starts/zz_brickdesk.sh'
adb shell 'ls -l /mnt/SDCARD/brickdesk/boot-hook.sh'
```

要记录：

- [ ] `runtrimui.sh` 是否真的遍历 `/mnt/SDCARD/System/starts/*`；排序规则是什么。
- [ ] 脚本是同步执行还是后台执行；脚本返回 0 后是否继续启动 MainUI。
- [ ] 脚本返回非 0 时 `runtrimui.sh` 是否继续启动 MainUI。
- [ ] `zz_brickdesk.sh` 阻塞期间，排在它后面的 `System/starts` 脚本是否被延后；
      原厂是否有必须执行的后续脚本。
- [ ] 如果脚本先于 MainUI 执行，钩子里的 `killall MainUI` 分支是否永远不会命中；
      如果 MainUI 已经启动，杀掉后原厂循环是否会重启它。
- [ ] 如果原厂在钩子返回后不启动 MainUI，需要把回退改成显式启动
      `/etc/init.d/runtrimui start`，并重新验证不会递归。

这些结果决定 `docs/install.md` 第 4、5、8 节是否要改写。在确认前，安装包只能
标记为“离线测试通过，真机未验证”。

#### 5.2.2 安装包内容与路径

```sh
scripts/package-target.sh --build-dir build-target --out-dir build-target/package
unzip -l build-target/package/brickdesk-*.zip
cat build-target/package/SHA256SUMS
```

检查 zip 里至少有：

```text
brickdesk-<version>-aarch64/install.sh
brickdesk-<version>-aarch64/uninstall.sh
brickdesk-<version>-aarch64/payload/bin/{brickdesk,brickdesk-inputd,brickdesk-perfd,
  bd-input,brickdesk-config,display_probe,bench_render,brickdesk-demo}
brickdesk-<version>-aarch64/payload/lib/{libSDL2-2.0.so.0,libbrickdesk-menu.so}
brickdesk-<version>-aarch64/payload/assets/fonts/*
brickdesk-<version>-aarch64/payload/assets/covers/*
brickdesk-<version>-aarch64/payload/config/*.ini
brickdesk-<version>-aarch64/payload/plugins/hello/*
brickdesk-<version>-aarch64/payload/apps/brickdesk-demo/*
brickdesk-<version>-aarch64/payload/boot-hook.sh
```

确认 `libSDL2-2.0.so.0` 是真实文件而不是符号链接：FAT32 不能保留符号链接。
`bench_render` 必须出现在 target workflow 的 `GLIBC <= 2.23` 检查里。

#### 5.2.3 安装与首次启动

在设备上：

```sh
sh /mnt/SDCARD/brickdesk-<version>-aarch64/install.sh
```

确认：

- [ ] `/mnt/SDCARD/brickdesk/` 下有 `bin/`、`lib/`、`assets/`、`config/`、
      `plugins/`、`apps/`、`state/`、`logs/`。
- [ ] `/mnt/SDCARD/System/starts/zz_brickdesk.sh` 存在，内容包含
      `brickdesk-boot-hook`，并 `exec sh '/mnt/SDCARD/brickdesk/boot-hook.sh'`。
- [ ] 如果安装前有同名钩子，原文件在 `brickdesk/state/stock-hook.bak`。
- [ ] `/mnt/SDCARD/brickdesk-uninstall.sh` 存在。
- [ ] 安装脚本没有写 `/mnt/SDCARD` 以外的文件；`/tmp` 只在运行时出现 socket。
- [ ] 重启后先出现 brickdesk，而不是 MainUI；`logs/boot.log` 里能看到
      `starting brickdesk-inputd`、`starting brickdesk-perfd`、`starting desktop`，
      且顺序是 inputd/perfd 在 desktop 之前。
- [ ] `logs/inputd.log`、`logs/perfd.log` 没有立即退出；`state/run/*.pid` 在桌面
      运行时存在。
- [ ] 桌面退出后两个守护进程被停止，原厂 MainUI 能回来。

#### 5.2.4 升级保留用户数据

1. 在 `config/local.ini` 写一个非默认值（例如 `[ui] theme = light`）。
2. 在 `state/` 下写一个测试文件。
3. 安装新版本。
4. 确认 `local.ini`、`state/`、用户自己的 `apps/`/`plugins/` 都还在，新的
   `VERSION` 和二进制已替换。

#### 5.2.5 崩溃回退

安全的真机步骤：

1. 备份 `/mnt/SDCARD/brickdesk/bin/brickdesk`。
2. 用 `exit 1` 的脚本替换它（或临时改名后放一个假程序）。
3. 重启，观察 `logs/boot.log`：应尝试 3 次，然后出现
   `crash limit reached; falling back to the stock UI`，并进入 MainUI。
4. 再重启一次，确认 `state/boot/attempts` 为 3 且不再启动 brickdesk。
5. 运行 `sh install.sh --reset-fallback`，恢复真实二进制，再重启。

确认：

- [ ] 3 次失败后当前启动能回到原厂 MainUI。
- [ ] 计数跨重启保留；重置后能再次尝试。
- [ ] `config/boot-skip` 空文件能让下一次启动直接进入 MainUI。
- [ ] 正常桌面运行超过 20 秒或正常退出后，`state/boot/attempts` 归零。
- [ ] 开机按住某个键跳过 brickdesk：调研没有给出可靠方法，当前未实现；记录
      在该阶段 `evtest`/`dd` 是否能读到按键，作为后续设备项。

#### 5.2.6 卸载与原厂恢复

```sh
sh /mnt/SDCARD/brickdesk-uninstall.sh
```

确认：

- [ ] `System/starts/zz_brickdesk.sh` 被删除；如果安装前有原厂钩子，原文件被恢复。
- [ ] `brickdesk/state/stock-hook.bak` 不再存在（恢复后应被移走）。
- [ ] `brickdesk/bin`、`lib`、`assets`、`plugins`、`apps`、`VERSION` 被删除。
- [ ] `config/local.ini` 和 `state/` 保留。
- [ ] `/mnt/SDCARD/brickdesk-uninstall.sh` 自身被删除。
- [ ] 原厂 MainUI 被启动；如果 `/etc/init.d/runtrimui start` 不可用，
      `runtrimui.sh` 回退路径有效。
- [ ] `--purge` 会删除整个 `/mnt/SDCARD/brickdesk/`，包括 `local.ini` 和 `state/`。
- [ ] 卸载后重启能正常进入原厂 UI。

#### 5.2.7 手动恢复演练

- [ ] 关机拔卡，不插 SD 卡能进原厂 UI。
- [ ] 在电脑上删除 `System/starts/zz_brickdesk.sh` 后插卡开机，能进原厂 UI。
- [ ] 把 `brickdesk/state/stock-hook.bak` 复制回 `System/starts/zz_brickdesk.sh` 后，原厂行为恢复。
- [ ] 只读 SD 卡/写满 SD 卡时，安装和回退计数失败的行为可预期（不静默破坏原厂 UI）。

#### 5.2.8 发布包性能工具

- [ ] `scripts/device/bench.sh --scenario idle --idle-ms 3000` 能从发布包里的
      `bench_render` 跑出 JSON；`presented=0`、`cpu_ms` 接近 0。
- [ ] `bench_render` 的 `BRICKDESK_ASSET_DIR` 指向
      `/mnt/SDCARD/brickdesk/assets`，不依赖仓库路径。
- [ ] target workflow 的 GLIBC 检查包含 `bench_render`，且最高版本 <= 2.23。

## 6. SDL2 patch 说明

原厂 `SDL2-2.26.1.GE8300.tgz` 的 `src/video/mali-fbdev/SDL_malivideo.h` 定义了
`SDL2_POWERVR_GE8300`，`MALI_CreateWindow()` 里硬编码：

```c
window->w = 1280;
window->h = 720;
```

并且 `native_display` 是 `void *`（NULL），传给 `SDL_EGL_CreateSurface()`。
这对 1280×720 的 Smart Pro 没问题，对 1024×768 的 Brick Pro 会导致逻辑分辨率和
EGL surface 尺寸与面板不一致。

`patches/sdl2/0001-read-fb-mode.patch` 做三件事：

1. 在 `SDL_malivideo.h` 本地定义
   `struct fbdev_window { unsigned short width; unsigned short height; }`，
   `SDL_DisplayData.native_display` 改为该结构；
2. `MALI_VideoInit()` 把 `FBIOGET_VSCREENINFO` 读到的 `xres/yres` 填进
   `native_display`（原来这段被 `#if 0` 关掉）；
3. `MALI_CreateWindow()` 用 `native_display` 的宽高设置 `window->w/h`，并把
   `&native_display` 传给 `SDL_EGL_CreateSurface()`。

构建脚本 `scripts/build-sdl2.sh` 从源码复制一份、打 patch、用官方 7.5 工具链构建
shared + static，安装到 sysroot，并对 `libSDL2-2.0.so.0` 跑 glibc 检查。可重复执行，
每次都会重新复制源码，避免 patch 残留。

真机判断 patch 是否生效，只看这三点同时成立：

```text
sdl_window_size: 1024x768
egl_surface_size: 1024x768
fb0_res: 1024x768
```

如果 `sdl_window_size` 是 1280×720：

```sh
adb shell 'ls -l /mnt/SDCARD/brickdesk/lib/'
adb shell 'LD_LIBRARY_PATH=/mnt/SDCARD/brickdesk/lib ldd /mnt/SDCARD/brickdesk/bin/display_probe'
adb shell 'readelf -d /mnt/SDCARD/brickdesk/bin/display_probe | grep NEEDED'
```

确认 `libSDL2-2.0.so.0` 来自 `$DEST/lib`，而不是 `/usr/trimui/lib`。

## 7. 已知风险与未验证项

- 所有显示数字（fb0 模式、EGL vendor/extensions、swap 耗时）都未在 PowerVR 上实测；
  探针只保证能在 aarch64 上编译并输出结构化结果。
- `/dev/fb0` 读回不一定等于 GPU 扫描输出；`screenshot.sh` 的结果需人工目检。
- `recover.sh` 假设 `/etc/init.d/runtrimui` 支持 `start`/`stop`；如果原厂脚本不提供，
  需要按真机实际启动方式修正。
- `run.sh` 用 `nohup` 后台启动；如果 busybox 没有 `nohup`，需要改为 `setsid` 或
  原厂启动器方式。
- `perf.sh` 的 CPU% 依赖 `/proc/$PID/stat` 的字段和 `CLK_TCK`；先跑一次确认真机
  `getconf CLK_TCK` 返回 100。
- `display_probe` 的 GL 扩展字符串在 GLES3 上下文下可能为空；探针请求 ES2，若设备
  返回 ES3，需要后续补 `glGetStringi` 枚举（未实现，避免依赖 GLES3 符号）。
- 设备 glibc 实际上是 2.23 还是更高，必须用 `ldd --version` 确认；CI 目前按 2.23
  卡上限。
- CI 的 GHCR 镜像、Dockerfile 和 target workflow 已在 GitHub Actions 验证；Docker
  在本 orb 不可用，不能本地复跑容器。
- `System/starts` 的路径和“同步执行、返回后继续 MainUI”的语义来自外部公开资料，
  不是 `docs/research/device-features.md` 的结论；Brick Pro 1.1.1 上未验证。
- 发布包的崩溃回退依赖 SD 卡可写；`state/boot/attempts` 写入失败时不会触发回退。
- 安装脚本在 FAT32 上使用 rename 做整树替换；如果 SD 卡有文件系统错误或写入被
  中断，可能留下 `.new.<pid>`/`.old.<pid>` 目录，需要手动删除。
- 安装包不包含厂商 `libEGL.so`/`libGLESv2.so`，运行时依赖设备的
  `/usr/trimui/lib` 或 `/usr/lib`。
- 原厂 `keymon`/`trimui_inputd` 不会被安装器停止；它们与 brickdesk-inputd 同时
  读 evdev 是否产生双份输入/冲突，必须在真机确认。

## 8. M1/M8-A 验收对应

- M1-03：`scripts/device/smoke.sh` 输出 `sdl_window_size: 1024x768`、
  `egl_surface_size: 1024x768`、`RESULT: PASS`；SDL2 patch 在源码和构建产物中生效。
- M1-12：`deploy → run/foreground probe → logs → screenshot → perf → recover`
  在真机跑通；`recover.sh` 能回到原厂 MainUI；所有写操作只在 SD 卡应用目录。
- M1-11（target 部分）：target workflow 绿、GHCR 镜像可拉取、`brickdesk`、
  `display_probe` 和 SDL2 产物可下载、`GLIBC <= 2.23`。
- M8-A：target workflow 产出 `brickdesk-<version>-aarch64.zip` + `SHA256SUMS`；
  `bench_render` 在包里并通过 GLIBC 检查；`package-tests.sh` 覆盖安装、升级保留
  `local.ini`/`state`、3 次崩溃回退、卸载恢复原厂钩子；真机完成 5.2 的全部检查项。
