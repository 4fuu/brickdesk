# 音频补齐

现有 `bd_av_audio` 已处理音量、静音、耳机/扬声器路由和 `/sys/class/speaker/mute`。
WP-13 在其上增加可选 ALSA 控件：

```c
bool bd_av_audio_has_control(const bd_av_audio *audio, const char *name);
bd_av_result bd_av_audio_set_switch_by_name(bd_av_audio *audio,
                                            const char *name, bool value,
                                            bd_av_error *error);
bd_av_result bd_av_audio_set_percent_by_name(bd_av_audio *audio,
                                             const char *name, int percent,
                                             bd_av_error *error);
bd_av_result bd_av_audio_get_percent_by_name(const bd_av_audio *audio,
                                             const char *name, int *percent,
                                             bd_av_error *error);
```

控件名按精确 `strcmp` 匹配，不做模糊猜测；找不到时返回
`BD_AV_ERR_UNSUPPORTED`，设置页显示“不可用”。

## 设置页

“声音”页新增：

- `audio.channel_swap`：依次尝试 `Speaker Channel Swap`、`Headphone Channel Swap`。
- `audio.speaker_gain`：映射到 ALSA `Speaker Volume` 的 0..100%。
- `audio.headphone_gain`：映射到 ALSA `Headphone Volume` 的 0..100%。
- `audio.mic`：依次尝试 `Mic Switch`、`Capture Switch`、`Mic Capture Switch`。

所有值写入 `local.ini`；启动时由 shell 重新应用。USB DAC 需要真机确认
`/proc/asound/cards` 里的卡名和 `PCM/Playback + Volume` 控件，当前没有自动切换
声卡，避免在未验证时把系统输出切到错误设备。

## 测试

`tests/device_av_test.c` 覆盖已有音量/静音/路由；新增的可选控件 helper 由
设置页编译和真机验收覆盖。真机检查项见 `docs/device-testing.md` 4.3。
