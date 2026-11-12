/* brickdesk v2 — sample content for the visual prototype.
   Geometry is in 200px wall units: x, y, w, h. */
(function () {
  const items = [
    { name: '小镇四季', type: 'game', platform: 'GBA', desc: '第 3 年夏季 · 已玩 41 小时',
      x: 2, y: 1, w: 3, h: 2, img: '../bg/town.jpg', pos: '92% 22%', hero: true,
      fav: true, accent: '#C97744' },
    { name: '阅读', type: 'app', desc: '《三体》读到第 14 章',
      x: 0, y: 0, w: 2, h: 1, img: '../bg/read.jpg', pos: '50% 58%', icon: 'book',
      accent: '#5D7C5A', fav: true },
    { name: '星海拾荒者', type: 'game', platform: 'PICO-8', desc: '第 7 扇区 · 已玩 6 小时',
      x: 5, y: 0, w: 2, h: 2, img: '../bg/space.jpg', pos: '84% 30%', accent: '#4C5C70' },
    { name: '文件', type: 'app', desc: 'SD 卡已用 41.2 GB，共 128 GB',
      x: 2, y: 0, w: 2, h: 1, img: '../bg/files.jpg', pos: '50% 50%', icon: 'folder',
      accent: '#4C5C70' },
    { name: '终端', type: 'app', desc: '本机 Shell',
      x: 4, y: 0, w: 1, h: 1, material: true, icon: 'term', glyph: '>_',
      accent: '#2C2E2C', onCover: '#EFE8DA' },
    { name: '音乐', type: 'app', desc: '312 首歌曲 · 6 个播放列表',
      x: 0, y: 1, w: 1, h: 2, img: '../bg/music.jpg', pos: '50% 45%', icon: 'music',
      accent: '#7B5A8C', live: true, fav: true,
      songs: [
        { name: '夜航', artist: '林间电台', duration: '03:48', progress: 38,
          lines: ['灯塔在身后慢慢变小', '风把航线吹成一条细线', '我们往没有名字的海去', '天亮以前不必回头'] },
        { name: '旧磁带', artist: '南窗', duration: '04:12', progress: 12,
          lines: ['按下播放键的那一秒', '夏天从喇叭里漏出来', '有些歌只在夜里才响', '转到尽头再翻一面'] }
      ],
      playing: true, song: 0, line: 0 },
    { name: '雾港', type: 'game', platform: 'PS', desc: '第二章「灯塔」 · 已玩 3 小时',
      x: 1, y: 1, w: 1, h: 2, img: '../bg/harbor.jpg', pos: '88% 28%', accent: '#51606C', fav: true },
    { name: '相册', type: 'app', desc: '截图与照片，共 86 张',
      x: 5, y: 2, w: 2, h: 1, img: '../bg/photos.jpg', pos: '50% 45%', icon: 'image',
      accent: '#B08A4E', fav: true },
    { name: '深井', type: 'game', platform: 'SFC', desc: '到达地下 12 层 · 已玩 9 小时',
      x: 0, y: 3, w: 2, h: 2, img: '../bg/well.jpg', pos: '100% 20%', accent: '#4B5E66' },
    { name: '无线传输', type: 'app', desc: '在电脑浏览器里向掌机传文件',
      x: 2, y: 3, w: 1, h: 2, img: '../bg/transfer.jpg', pos: '50% 50%', icon: 'swap',
      accent: '#3F8587' },
    { name: '赤色公路', type: 'game', platform: 'PS', desc: '最佳圈速 1:42.08',
      x: 5, y: 3, w: 2, h: 2, img: '../bg/road.jpg', pos: '90% 34%', accent: '#A25A4C' },
    { name: '设置', type: 'app', desc: '显示、声音、网络与按键',
      x: 3, y: 3, w: 2, h: 2, img: '../bg/settings.jpg', pos: '50% 50%', icon: 'gear',
      accent: '#6F737B' }
  ];

  const tabs = ['全部', '游戏', '应用', '收藏'];

  const sideOpts = [
    { label: '搜索', icon: 'search', type: 'toggle', on: true },
    { label: '消息', icon: 'bell', type: 'toggle', on: true },
    { label: '文件', icon: 'folder', type: 'toggle', on: false },
    { label: '无线传输', icon: 'swap', type: 'toggle', on: true }
  ];

  const power = [
    { label: '睡眠', icon: 'moon' },
    { label: '重启', icon: 'restart' },
    { label: '关机', icon: 'power' }
  ];

  const cats = [
    { name: '显示', icon: 'monitor', note: '主题、亮度与休眠', opts: [
      { key: 'theme', label: '主题', icon: 'moon', desc: '深色适合夜间，浅色适合白天', type: 'select', values: ['深色', '浅色'], i: 0 },
      { key: 'bright', label: '亮度', icon: 'sun', desc: '屏幕背光强度', type: 'slider', v: 70 },
      { label: '自动休眠', type: 'select', values: ['1 分钟', '3 分钟', '5 分钟', '从不'], i: 1 },
      { key: 'anim', label: '界面动画', desc: '面板滑入、焦点移动与封面过渡', type: 'toggle', on: true }
    ]},
    { name: '声音', icon: 'volume', note: '扬声器与按键反馈', opts: [
      { key: 'vol', label: '音量', icon: 'volume', desc: '扬声器输出音量', type: 'slider', v: 45 },
      { label: '按键音', type: 'toggle', on: false }
    ]},
    { name: '网络', icon: 'wifi', note: '无线连接状态', opts: [
      { key: 'wifi', label: 'Wi-Fi', icon: 'wifi', desc: '连接无线网络', type: 'toggle', on: true },
      { key: 'bt', label: '蓝牙', icon: 'bt', desc: '连接手柄、耳机与音箱', type: 'toggle', on: false },
      { label: '已连接', type: 'info', value: 'Home-5G' },
      { label: 'IP 地址', type: 'info', value: '192.168.1.37' }
    ]},
    { name: '按键', icon: 'game', note: '掌机按键的作用', opts: [
      { label: '提示栏', desc: '按住 Select 显示，松开即收起', type: 'info', value: '按住显示' },
      { label: 'Menu', type: 'info', value: '打开或关闭侧边栏' },
      { label: 'Start', type: 'info', value: '打开或关闭快捷设置' },
      { label: 'L1 / R1', type: 'info', value: '切换主页分类' },
      { label: 'X / Y', type: 'info', value: '当前封面的快捷操作' }
    ]},
    { name: '侧边栏', icon: 'side', note: '选择要显示的入口', opts: sideOpts },
    { name: '系统', icon: 'info', note: '关于这台掌机', opts: [
      { label: '语言', type: 'info', value: '简体中文' },
      { label: '设备', type: 'info', value: 'TrimUI Brick Pro' },
      { label: '版本', type: 'info', value: 'brickdesk 0.1.0' }
    ]}
  ];

  window.BD_DATA = { items, tabs, sideOpts, power, cats };
})();
