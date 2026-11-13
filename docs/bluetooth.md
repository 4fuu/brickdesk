# 蓝牙

`brickdesk/device_bt.h` 通过 `bluetoothctl` 管理适配器、扫描、配对和连接，
音频输出走 `bluealsa`。所有命令都用 argv，不经过 shell。

## 接口

```c
bd_bt *bd_bt_create(void);
bool bd_bt_get_status(bd_bt *bt, bd_bt_status *status);
bool bd_bt_set_powered(bd_bt *bt, bool powered, char *error, size_t size);
bool bd_bt_scan_start(bd_bt *bt, int scan_seconds, char *error, size_t size);
bool bd_bt_list_devices(bd_bt *bt, bd_bt_device *out, size_t max,
                        size_t *count, char *error, size_t size);
bool bd_bt_connect(bd_bt *bt, const char *addr, char *error, size_t size);
bool bd_bt_disconnect(bd_bt *bt, const char *addr, char *error, size_t size);
bool bd_bt_forget(bd_bt *bt, const char *addr, char *error, size_t size);
bool bd_bt_suspend(char *error, size_t size);
bool bd_bt_resume(char *error, size_t size);
```

状态来自 `bluetoothctl show` 的 `Powered`/`Discovering` 和
`bluetoothctl devices Connected`。扫描用
`bluetoothctl --timeout N scan on` 在独立进程里跑，桌面只轮询设备列表，不会
阻塞按键线程。连接依次执行 `pair`、`trust`、`connect`。

## 设置页与快捷面板

设置页“连接”分类包含蓝牙开关、状态、扫描和每个设备的连接/断开行。首页
快捷面板（START）的“蓝牙”行显示开关状态，A 直接切换电源。连接成功后
`bluealsa` 提供 A2DP sink；后续音量控制走 `amixer -D bluealsa`。

## 深睡 hook

`bd_bt_suspend()` 关闭电源、停 `bluealsa`/`bluetoothd` 并 `rfkill block
bluetooth`（没有 rfkill 时写 `/sys/class/rfkill/rfkill0/state`）。
`bd_bt_resume()` 反向执行：`rfkill unblock`、启动 `bluetoothd`、`hciconfig
hci0 up`、`bluetoothctl power on`、启动 `bluealsa -p a2dp-sink`。

## 测试

`tests/device_bt_test.c` 用 PATH 里的 `bluetoothctl` stub 覆盖状态、已连接
设备、设备列表、pair/trust/connect、断开、忘记、电源和 suspend/resume。
真机需确认 `bluetoothctl --timeout` 是否被 BlueZ 版本支持、`bluealsa` 的
服务名和 `hciconfig hci0` 是否存在。
