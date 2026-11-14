# 在 Brick Pro 上安装 brickdesk

本文面向用户，说明如何安装、升级、回退和卸载 brickdesk。设备目标是 TrimUI
Brick Pro（TG4040）原厂固件，SD 卡挂载在 `/mnt/SDCARD`。

> 状态：安装/卸载脚本和启动钩子已通过离线测试（假 adb、假 SD 根目录），
> 但还没有在 Brick Pro 真机上验证。特别是原厂 `System/starts` 的执行顺序和
> 阻塞语义，见第 8 节。第一次安装前请先备份 SD 卡。

## 1. 安装包内容

发布包文件名形如：

```text
brickdesk-0.1.0-aarch64.zip
SHA256SUMS
```

解压后得到：

```text
brickdesk-0.1.0-aarch64/
  VERSION
  GIT_REV
  README.txt
  install.sh
  uninstall.sh
  docs/install.md
  docs/device-av.md
  docs/device-battery.md
  docs/device-feedback.md
  docs/device-power.md
  payload/
    bin/
      brickdesk
      brickdesk-inputd
      brickdesk-perfd
      bd-input
      brickdesk-config
      bd-av
      bd-battery
      bd-feedback
      bd-power
      display_probe
      bench_render
      brickdesk-demo
    lib/
      libSDL2-2.0.so.0        # 后备 SDL2；install.sh 会用原厂库原子覆盖
      libbrickdesk-menu.so
    assets/
      fonts/
      covers/
    config/
      brickdesk.ini
      perf.ini
      hotkeys.ini
      input.ini
    plugins/hello/
      plugin.json
      bin/hello
    apps/brickdesk-demo/
      app.json
      brickdesk-demo
    boot-hook.sh
  SHA256SUMS
```

安装后程序位于 `/mnt/SDCARD/brickdesk/`，配置、资源、插件和应用都使用这个
根目录下的路径：

```text
/mnt/SDCARD/brickdesk/
  bin/
  lib/
  assets/
  config/
  plugins/
  apps/
  state/
  logs/
```

## 2. 安装前

1. 备份 SD 卡上已有的 `System/`、`brickdesk/` 和 `Apps/` 目录。
2. 确认 `/mnt/SDCARD` 已挂载且可写。可以在设备终端或 adb 中执行：

   ```sh
   mount | grep /mnt/SDCARD
   ```

3. 确认设备是 aarch64：

   ```sh
   uname -m
   ```

   应输出 `aarch64`。其他架构的包不能运行。

4. 如果设备上已经装了 brickdesk，先退出桌面，避免安装时替换正在运行的文件。

## 3. 安装

推荐在电脑上解压 zip 到 SD 卡根目录，然后在设备上运行安装脚本。这样不依赖
设备上的 `unzip` 命令。

如果使用 adb，可以把整个目录推到 SD 卡后再执行：

```sh
adb push brickdesk-0.1.0-aarch64 /mnt/SDCARD/
adb shell 'sh /mnt/SDCARD/brickdesk-0.1.0-aarch64/install.sh'
```

也可以直接在设备终端里执行：

```sh
sh /mnt/SDCARD/brickdesk-0.1.0-aarch64/install.sh
```

安装脚本会：

- 把 `payload/` 复制到 `/mnt/SDCARD/brickdesk/`；
- 保留已有的 `config/local.ini`、`state/`、用户自己的 `apps/` 和 `plugins/`；
- 如果设备存在 `/usr/trimui/lib/libSDL2-2.0.so.0`，把它原子复制到
  `brickdesk/lib/libSDL2-2.0.so.0`；失败时保留包内后备 SDL2；
- 写入启动钩子 `/mnt/SDCARD/System/starts/zz_brickdesk.sh`；
- 如果该钩子文件已经存在且不是 brickdesk 的，先备份为
  `brickdesk/state/stock-hook.bak`；
- 在 `/mnt/SDCARD/brickdesk-uninstall.sh` 放一份卸载脚本。

安装完成后重启设备。安装脚本本身不会立即启动桌面。

可选参数：

```sh
sh install.sh --no-hook          # 只复制文件，不安装启动钩子
sh install.sh --reset-fallback   # 清除崩溃计数和 boot-skip 标志
```

### 校验下载包

在电脑上，把 `SHA256SUMS` 和 zip 放在同一目录：

```sh
sha256sum -c SHA256SUMS
```

Windows 可以用 PowerShell 的 `Get-FileHash`，或 7-Zip 的校验功能。

## 4. 启动流程

安装后，原厂启动脚本会在启动时执行 `/mnt/SDCARD/System/starts/` 下的脚本。
brickdesk 的钩子是 `zz_brickdesk.sh`，它：

1. 检查 `config/boot-skip` 和崩溃计数；如果要求跳过，直接返回，让原厂 MainUI
   继续启动；
2. 停掉已经运行的 MainUI（如果它比钩子先启动）；
3. 用 `display_probe --ready` 等待 PowerVR RGX 固件和 SDL/EGL surface 就绪；
   默认每 1 秒重试，最多 25 秒；readiness 失败不计入桌面崩溃次数，超时则直接
   留在原厂 MainUI；
4. 启动 `brickdesk-inputd` 和 `brickdesk-perfd`；
5. 在前台运行 `brickdesk`；
6. 桌面退出后停止两个守护进程，并返回启动脚本。

boot hook 支持以下环境变量（默认值适合 Brick Pro）：

```text
BRICKDESK_START_DELAY=0              # readiness 前的固定等待秒数
BRICKDESK_DISPLAY_READY_TIMEOUT=25   # readiness 总超时
BRICKDESK_DISPLAY_READY_INTERVAL=1   # 重试间隔
BRICKDESK_DISPLAY_READY=0            # 关闭 readiness 门控
BRICKDESK_PROBE=/path/to/display_probe
```

输入和性能守护进程的日志在：

```text
/mnt/SDCARD/brickdesk/logs/inputd.log
/mnt/SDCARD/brickdesk/logs/perfd.log
/mnt/SDCARD/brickdesk/logs/brickdesk.log
/mnt/SDCARD/brickdesk/logs/boot.log
```

Unix socket 默认放在 `/run/brickdesk/`，失败时回退到 `/tmp/`。SD 卡通常是
FAT32，不能创建 Unix socket，所以 socket 不能放在 `/mnt/SDCARD`。安装脚本
本身不写 `/mnt/SDCARD` 以外的位置；运行时只有 socket/临时文件会使用 tmpfs。

## 5. 崩溃回退

如果 `brickdesk` 连续启动失败，钩子会自动回到原厂 MainUI：

- 每次启动失败（非 0 退出，或运行时间短于 20 秒）计数加一；
- 连续 3 次失败后，当前启动直接返回，让原厂 MainUI 接管；
- 计数保存在 `/mnt/SDCARD/brickdesk/state/boot/attempts`；
- 计数达到 3 后，后续启动会跳过 brickdesk，直到手动清除。

手动跳过一次启动（不需要终端）：

1. 用电脑把 SD 卡挂载到读卡器；
2. 在 `/mnt/SDCARD/brickdesk/config/` 下新建空文件 `boot-skip`；
3. 开机后会直接进入原厂 MainUI。

恢复 brickdesk：

```sh
sh /mnt/SDCARD/brickdesk-0.1.0-aarch64/install.sh --reset-fallback
```

或者删除：

```text
/mnt/SDCARD/brickdesk/state/boot/attempts
/mnt/SDCARD/brickdesk/config/boot-skip
```

如果桌面能正常运行超过 20 秒，或正常退出，计数会自动清零。

> 开机时按住某个键跳过 brickdesk 还没有实现。调研文档没有给出在
> `System/starts` 阶段读取按键的可靠方法，这属于真机验证项；目前请使用
> `config/boot-skip` 文件。

## 6. 卸载

默认卸载会删除程序文件和启动钩子，恢复安装前的钩子，并保留用户数据：

```sh
sh /mnt/SDCARD/brickdesk-uninstall.sh
```

保留的内容：

```text
/mnt/SDCARD/brickdesk/config/local.ini
/mnt/SDCARD/brickdesk/config/plugins/
/mnt/SDCARD/brickdesk/state/
/mnt/SDCARD/brickdesk/logs/
```

彻底删除（包括 `local.ini`、应用状态和日志）：

```sh
sh /mnt/SDCARD/brickdesk-uninstall.sh --purge
```

只卸载、不自动启动原厂 UI：

```sh
sh /mnt/SDCARD/brickdesk-uninstall.sh --no-restart
```

默认卸载会尝试停止 brickdesk 进程，然后通过 `/etc/init.d/runtrimui start` 或
`/usr/trimui/bin/runtrimui.sh` 启动原厂 MainUI。这两条恢复路径都还没有在真机
验证；如果失败，重启设备即可回到原厂 UI。

卸载脚本也会删除 `/mnt/SDCARD/brickdesk-uninstall.sh` 自身。

## 7. 无法启动时的手动恢复

如果开机黑屏或一直停在 brickdesk：

1. 长按电源键关机；
2. 取出 SD 卡；
3. 不插 SD 卡开机，确认原厂 UI 能启动；
4. 把 SD 卡插到电脑，删除或改名：

   ```text
   /mnt/SDCARD/System/starts/zz_brickdesk.sh
   ```

   也可以把整个 `/mnt/SDCARD/brickdesk/` 改名；
5. 插回 SD 卡开机，应该进入原厂 MainUI。

如果之前安装时备份了原厂钩子，文件在：

```text
/mnt/SDCARD/brickdesk/state/stock-hook.bak
```

把它复制回 `System/starts/zz_brickdesk.sh` 即可恢复。

## 8. 未验证项与风险

以下内容来自调研和离线测试，但没有在 Brick Pro 1.1.1 真机上确认：

- `docs/research/device-features.md` 没有记录 `System/starts`。目前依据的是
  TrimUI 原厂 Smart Pro rootfs 的公开说明（`runtrimui.sh` 会执行
  `/mnt/SDCARD/System/starts` 下的脚本）以及 CrossMix 对该目录的使用。Brick
  Pro 1.1.1 的确切路径、脚本排序、是否同步执行、返回值是否会让
  `runtrimui.sh` 继续启动 MainUI，都必须在真机上确认。
- 钩子会阻塞 `System/starts` 的后续脚本，直到 brickdesk 退出。如果原厂或其
  他工具在 `zz_brickdesk.sh` 之后放了必须执行的启动脚本，它们会被延后。
- 开机按键跳过没有实现。
- `brickdesk-inputd`/`brickdesk-perfd` 是否能在原厂 `runtrimui.sh` 的环境里
  正常打开 evdev、写 sysfs，以及 socket 路径 `/run/brickdesk` 是否可写，需要
  真机确认。
- 原厂 `keymon`/`trimui_inputd` 仍在运行；brickdesk 自己的 inputd 与它们同时读
  同一批 evdev 是否会出现双份输入或按键冲突，需要真机确认。当前安装器不停止
  原厂守护进程，只停 MainUI。
- 停掉 MainUI 后，原厂 `runtrimui.sh` 是否会在钩子返回时可靠地重新启动
  MainUI，需要真机确认。
- 崩溃计数依赖 `state/boot/attempts` 可写；如果 SD 卡只读或 FAT32 写入失败，
  回退可能不生效。
- `bench_render`、`display_probe` 和桌面本身的显示/性能数字仍需真机测量。
- 安装脚本不会修改原厂分区，但会在 SD 卡上新增 `System/starts/zz_brickdesk.sh`；
  这是启动钩子严格要求的最小写入。

## 9. 开发调试

不经过启动钩子，手动运行：

```sh
cd /mnt/SDCARD/brickdesk
LD_LIBRARY_PATH=/mnt/SDCARD/brickdesk/lib:/usr/trimui/lib:/usr/lib:/lib \
BRICKDESK_ASSET_DIR=/mnt/SDCARD/brickdesk/assets \
  ./bin/brickdesk
```

运行显示探针：

```sh
cd /mnt/SDCARD/brickdesk
LD_LIBRARY_PATH=$PWD/lib:/usr/trimui/lib:/usr/lib:/lib \
  ./bin/display_probe
```

运行性能基准（会先停掉原厂 UI）：

```sh
scripts/device/bench.sh --scenario idle --idle-ms 3000
```

更多设备测试项见 `docs/device-testing.md`。
