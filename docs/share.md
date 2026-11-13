# 文件共享与连接

`brickdesk/device_share.h` 管理 SSH、FTP、Samba、ADB 和 USB 模式。服务通过
argv 启动、`killall` 停止，不经过 shell。

## 接口

```c
bd_share *bd_share_create(const char *iface);
bool bd_share_get_status(bd_share *share, bd_share_status *status);
bool bd_share_set_service(bd_share *share, bd_share_service service,
                          bool enabled, char *error, size_t size);
bool bd_share_set_usb_mode(bd_share *share, const char *mode, char *error,
                           size_t size);
```

- SSH：优先 `/usr/sbin/sshd`，否则 `/usr/sbin/dropbear`；停止时两者都
  `killall`。
- FTP：优先 `/usr/sbin/vsftpd`，否则 `tcpsvd` + busybox `ftpd`。
- Samba：`smbd -D`。
- ADB：`/usr/bin/adbd` 或 `/sbin/adbd`。
- USB 模式：`host`/`dock`/`data`，调用厂商
  `/usr/trimui/bin/usbmode <mode>`；脚本不存在时返回错误，设置页仍保存偏好。
- 状态行读取 `ip -4 addr show dev wlan0` 的 `inet` 地址，SSH/FTP/Samba 共用。

## 设置页

设置页“连接”分类底部有 IP、SSH、FTP、Samba、ADB 开关和 USB 模式枚举。
服务状态每 5 秒轮询一次；切换失败时保留原开关值并显示错误。

## 测试

`tests/device_share_test.c` 用 PATH 里的 `pgrep`/`killall`/`ip`/`smbd` stub
覆盖运行状态、IP 解析、停止 SSH（同时杀 sshd/dropbear）、启动 Samba 和
usbmode 缺失时的错误。真机需确认各服务实际二进制、配置文件和端口。
