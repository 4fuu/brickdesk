# 系统时间

`brickdesk/device_time.h` 管理日期时间、时区、NTP 和 RTC。

## 接口

```c
bool bd_time_get_status(bd_time_status *status);
bool bd_time_set_epoch(time_t epoch, char *error, size_t size);
bool bd_time_set_timezone(const char *timezone, char *error, size_t size);
bool bd_time_set_ntp(bool enabled, char *error, size_t size);
bool bd_time_sync_now(char *error, size_t size);
bool bd_time_write_rtc(char *error, size_t size);
```

- 设置时间：`date -u -s "YYYY-MM-DD HH:MM:SS"`，随后 `hwclock -u -w`。
- 时区：`uci set system.@system[0].zonename` + `uci commit system`，并更新
  `/etc/localtime` 与 `/etc/timezone`；没有 uci 时仍更新文件链接。
- NTP：`uci set system.ntp.enable=1/0`、`uci commit system`，再
  `/etc/init.d/ntpd restart|stop`；开启时立即 `ntpd -q` 校时。
- RTC：`hwclock -u -w`，需要 `/dev/rtc0`（AXP2202）。
- 状态：当前时间、`system.zonename`、`system.ntp.enable` 或 `pgrep ntpd`、
  `/dev/rtc0` 是否存在。

## 设置页

设置页“系统”页顶部有日期时间、设置日期时间（屏幕键盘输入
`YYYY-MM-DD HH:MM`）、时区枚举、NTP 开关、立即同步和写入 RTC。顶栏时钟每
5 秒随状态刷新，支持 `ui.time_format`（24h/12h）和 `ui.clock_seconds`。

## 真机检查

- `hwclock -u -w` 是否写进 AXP2202 RTC；断网重启后时间是否保持。
- `uci system.ntp.enable` 是否真的控制 `ntpd`/`sysntpd`。
- 时区切换后 `date`、顶栏和 `localtime` 是否一致。
