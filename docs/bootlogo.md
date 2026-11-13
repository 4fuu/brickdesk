# 开机 logo（WP-23）

状态：BMP 校验、备份、安装、恢复已在宿主用假目录验证；真实
`/dev/mmcblk0p1` 挂载待真机验证。

## 使用

```sh
# 只校验，不碰启动分区
brickdesk --bootlogo-check /path/logo.bmp

# 安装（首次会保留原厂 bootlogo.bmp 为 bootlogo.brickdesk-original.bmp）
brickdesk --bootlogo-apply /path/logo.bmp

# 恢复原厂 logo
brickdesk --bootlogo-restore
```

安装/恢复成功后需要手动重启；命令不会自动 reboot。

## 格式要求

- 文件必须是 BMP（`BM`），未压缩（compression=0），planes=1；
- 24 或 32 bpp；
- 尺寸必须为 1024×768（`BD_BOOTLOGO_WIDTH/HEIGHT`）；
- 文件长度必须覆盖 `data_offset + row_stride × height`。

校验在挂载分区之前完成，失败不会打开或写入任何块设备。安装分区的默认路径
是 `/dev/mmcblk0p1`（FAT），临时挂载点为 `/tmp/bd-bootlogo`；`BRICKDESK_BOOTLOGO_DIR`
只用于宿主测试，把目标目录指向普通目录，生产不要设置。

## 边界

- 只处理 `bootlogo.bmp`，不重分区、不写 U-Boot、不改启动脚本。
- 备份只在第一次安装时创建；重复安装不会覆盖原厂备份。
- 挂载失败返回 `mount-error`，复制/刷新失败返回 `io-error`；不会静默成功。
- 与原厂/NextUI 的 Bootlogo 工具不共用代码，只使用相同的分区路径与文件名。
