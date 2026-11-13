# 网络：Wi-Fi 与热点

`brickdesk/device_net.h` 是 tg5040 的 Wi-Fi/热点后端。它只通过 argv 调用
`wpa_cli`、`ip`、`udhcpc`、`hostapd`、`dnsmasq`，不经过 shell，所以 SSID 和
密码不会被解释成 shell 语法。

## 接口

```c
bd_net *bd_net_create(const char *iface, const char *socket_dir);
bool bd_net_get_status(bd_net *net, bd_net_status *status);
bool bd_net_set_enabled(bd_net *net, bool enabled, char *error, size_t size);
bool bd_net_scan_start(bd_net *net, char *error, size_t size);
bool bd_net_scan_results(bd_net *net, bd_net_network *out, size_t max,
                         size_t *count, char *error, size_t size);
bool bd_net_connect(bd_net *net, const char *ssid, const char *psk,
                    char *error, size_t size);
bool bd_net_select_saved(bd_net *net, int network_id, char *error, size_t size);
bool bd_net_forget(bd_net *net, int network_id, char *error, size_t size);
bool bd_net_hotspot_start(bd_net *net, const char *ssid, const char *psk,
                          char *error, size_t size);
bool bd_net_hotspot_stop(bd_net *net, char *error, size_t size);
bool bd_net_suspend(char *error, size_t size);
bool bd_net_resume(char *error, size_t size);
```

默认接口 `wlan0`，控制 socket `/etc/wifi/sockets`，supplicant 配置
`/etc/wifi/wpa_supplicant.conf`。`BRICKDESK_WIFI_IFACE`、
`BRICKDESK_WIFI_SOCKET_DIR`、`BRICKDESK_WIFI_CONFIG` 可覆盖，供宿主测试和
不同镜像使用。

## 设置页

设置页“网络”分类：

- **Wi-Fi** 开关：`ip link`、`wpa_supplicant`、`udhcpc` 一起启停；状态行显示
  `wpa_state`、SSID 和 IP。
- **扫描网络**：`wpa_cli scan`，1.5 秒后读取 `scan_results`；同一 SSID 只保留
  最强信号，按信号从强到弱排列。选择加密网络会打开屏幕键盘输入密码，开放
  网络直接连接。
- **已保存网络**：`list_networks`，A 通过 `select_network` 重连；忘记网络可在
  `local.ini` 删除对应 `[hotkey]` 之外的网络配置，当前 UI 不提供删除按钮。
- **热点**：`net.hotspot_enabled` 开关；SSID/密码取
  `net.hotspot_ssid`（默认 `BrickDesk`）和 `net.hotspot_psk`（默认
  `brickdesk`）。启动时写 `/run/brickdesk/hostapd.conf` 和
  `dnsmasq.conf`，地址 `192.168.4.1/24`，DHCP 池
  `192.168.4.10-192.168.4.200`。关闭时按 pid 文件停两个进程并清地址。

扫描和状态刷新都在 `bd_settings_update()` 里轮询，不阻塞按键线程；`wpa_cli`
单次命令有 3 秒超时，避免 daemon 卡死时桌面一起卡住。

## 深睡 hook

`bd_net_suspend()` 停 `udhcpc`/`wpa_supplicant` 并把 `wlan0` 置 down；
`bd_net_resume()` 重新 up、启动 supplicant 和 DHCP。两者在 Wi-Fi 本来就关闭时
也可安全调用。WP-03 的 resource_prepare/restore 直接调用这两个函数。

## 测试

`tests/device_net_test.c` 用 PATH 里的 `wpa_cli`/`ip`/`killall`/`udhcpc` stub
覆盖状态解析、扫描去重与排序、连接参数、已保存网络、忘记网络、启停和
suspend/resume。真机需要确认 `/etc/wifi/sockets` 路径、`udhcpc` 是否自动续租、
hostapd 驱动和 `dnsmasq` 是否存在。
