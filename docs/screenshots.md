# 截图（WP-21）

状态：桌面路径已在宿主 headless 环境生成并检查 PNG；前台应用路径读
`/dev/fb0`，待真机验证。

- 默认热键：`L1+R1+Start` → `builtin.screenshot`（`config/hotkeys.ini`）。
- 桌面可见时从渲染器的场景 FBO 保存 PNG；前台应用占用显示时改为只读
  `/dev/fb0`（16/32 bpp，按 fb 的通道位域转 RGBA），不需要应用配合。
- 输出目录：`<config_dir>/screenshots/`，文件名
  `screenshot-YYYYmmdd-HHMMSS-NNN.png`；目录不存在时创建。
- 成功/失败通过通知层提示；截图不修改应用状态、不暂停帧、不写设备节点。
- 与 `scripts/device/screenshot.sh` 的区别：后者用 `display_probe --dump-fb`
  抓 PPM，适合开发期；本功能在桌面运行时即可用。
