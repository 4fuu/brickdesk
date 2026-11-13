brickdesk @VERSION@ (aarch64) 安装包

适用：TrimUI Brick Pro (TG4040)，原厂固件；SD 卡挂载在 /mnt/SDCARD。

安装：
  1. 把 zip 解压到 SD 卡根目录，得到 brickdesk-@VERSION@-aarch64/。
  2. 在设备上运行：
       sh /mnt/SDCARD/brickdesk-@VERSION@-aarch64/install.sh
  3. 重启设备。

安装脚本只写 SD 卡：
  /mnt/SDCARD/brickdesk/
  /mnt/SDCARD/System/starts/zz_brickdesk.sh
  /mnt/SDCARD/brickdesk-uninstall.sh

升级会保留 config/local.ini、state/ 和用户自己的 apps/plugins。
连续 3 次启动失败后，启动钩子会让原厂 MainUI 接管；详细说明和恢复方法见
本包内 docs/install.md。

卸载：
  sh /mnt/SDCARD/brickdesk-uninstall.sh
彻底删除（包括 local.ini 和 state）：
  sh /mnt/SDCARD/brickdesk-uninstall.sh --purge

注意：System/starts 的执行顺序和阻塞语义尚未在 Brick Pro 1.1.1 真机上验证；
如果设备无法启动到 brickdesk，拔掉 SD 卡即可回到原厂 UI，然后在电脑上删除
System/starts/zz_brickdesk.sh。
