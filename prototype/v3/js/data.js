/* brickdesk v3 — sample catalog, accents, settings data. Images reuse prototype/bg. */
(function () {
  const ACCENTS = {
    persimmon: { label: '柿红', base: '#e07a5f' },
    coral: { label: '珊瑚红', base: '#ef5b47' },
    teal: { label: '青', base: '#4fb3a8' },
    blue: { label: '蓝', base: '#6c8cd5' },
    green: { label: '绿', base: '#7fa86b' },
    purple: { label: '紫', base: '#a98ad6' }
  };

  const TABS = ['全部', '游戏', '应用', '收藏'];
  const IMG = key => '../bg/' + key + '.jpg';
  const POS = ['50% 50%', '38% 34%', '62% 44%', '50% 30%', '70% 56%', '44% 62%'];

  const FEATURED = [
    { id: 'read', name: '阅读', sub: '《三体》读到第 14 章', type: 'app', span: [2, 1], img: IMG('read'), pos: '50% 58%', favorite: true },
    { id: 'terminal', name: '终端', sub: '本机 Shell', type: 'app', span: [1, 1], material: true, mat: 'terminal', glyph: '>_', favorite: true },
    { id: 'town', name: '小镇四季', sub: 'GBA · 第 3 年夏季', type: 'game', span: [3, 2], img: IMG('town'), frame: 'img/app-town.jpg', pos: '92% 22%', favorite: true },
    { id: 'clock', name: '时钟', sub: '工作日 · 06:42', type: 'app', span: [1, 1], material: true, mat: 'clock', glyph: '06:42' },
    { id: 'files', name: '文件', sub: 'SD 卡 41.2 GB / 128 GB', type: 'app', span: [2, 1], img: IMG('files'), pos: '50% 50%' },
    { id: 'music', name: '音乐', sub: '夜航 · 林间电台', type: 'app', span: [2, 2], img: IMG('music'), frame: 'img/app-music.jpg', pos: '50% 45%', live: true, favorite: true },
    { id: 'photos', name: '相册', sub: '截图与照片 · 86 张', type: 'app', span: [2, 2], img: IMG('photos'), pos: '50% 45%', favorite: true },
    { id: 'settings', name: '设置', sub: '显示、声音、网络与按键', type: 'app', span: [2, 2], img: IMG('settings'), pos: '50% 50%', favorite: true }
  ];

  const NAMES = [
    '雾港', '星海拾荒者', '无线传输', '深井', '赤色公路', '计算器', '播客', '天气',
    '相机', '录音机', '笔记', '地图', '书店', '画廊', '代码', '电台',
    '胶片', '潮汐', '远山', '夜行', '灯塔', '荒原', '迷城', '雨巷',
    '车站', '邮局', '花园', '森林', '海岛', '矿脉', '轨道', '风筝',
    '手帐', '词典', '沙漏', '尺规', '星图', '日历', '备忘录', '算盘',
    '黑胶', '磁带', '广播', '唱片', '剧场', '图书馆', '档案馆', '气象站',
    '天文台', '观测站', '实验室', '工作台'
  ];

  const GAMES = new Set(['雾港', '星海拾荒者', '深井', '赤色公路', '远山', '夜行', '灯塔', '荒原',
    '迷城', '雨巷', '花园', '森林', '海岛', '矿脉', '轨道', '剧场']);

  const ART = {
    雾港: 'harbor', 星海拾荒者: 'space', 无线传输: 'transfer', 深井: 'well', 赤色公路: 'road',
    播客: 'music', 相机: 'photos', 电台: 'music', 书店: 'read', 画廊: 'photos', 代码: 'files',
    远山: 'town', 夜行: 'road', 灯塔: 'harbor', 荒原: 'space', 迷城: 'well', 雨巷: 'town',
    车站: 'transfer', 邮局: 'files', 花园: 'town', 森林: 'well', 海岛: 'harbor', 矿脉: 'well',
    轨道: 'space', 风筝: 'town', 手帐: 'read', 词典: 'read', 尺规: 'files', 星图: 'space',
    日历: 'settings', 备忘录: 'read', 算盘: 'files', 黑胶: 'music', 磁带: 'music', 广播: 'music',
    唱片: 'music', 剧场: 'town', 图书馆: 'read', 档案馆: 'files', 气象站: 'space',
    天文台: 'space', 观测站: 'space', 实验室: 'settings', 工作台: 'files'
  };

  const SPAN = {
    雾港: [1, 2], 星海拾荒者: [2, 2], 无线传输: [1, 2], 深井: [2, 2], 赤色公路: [3, 1],
    计算器: [1, 1], 播客: [2, 1], 相机: [2, 2], 电台: [1, 2], 书店: [2, 1], 画廊: [2, 2],
    代码: [2, 1], 远山: [3, 2], 图书馆: [2, 2], 剧场: [3, 2]
  };

  const FRAME = { 星海拾荒者: 'img/app-space.jpg' };

  function defaultSpan(i) {
    const m = i % 24;
    if (m === 0) return [3, 2];
    if (m === 3 || m === 7 || m === 12 || m === 17 || m === 21) return [2, 2];
    if (m === 5 || m === 15) return [1, 2];
    if (m === 9 || m === 14 || m === 19) return [2, 1];
    return [1, 1];
  }

  function spanFor(item, i) {
    if (item.span) return item.span;
    if (item.live || item.recent) return [2, 2];
    return defaultSpan(i);
  }

  function buildBase() {
    const out = FEATURED.map(x => ({ ...x }));
    NAMES.forEach((name, j) => {
      const i = j + 8;
      const game = GAMES.has(name);
      const art = ART[name] || ['harbor', 'space', 'transfer', 'well', 'road', 'read', 'music', 'photos', 'settings', 'files', 'town'][j % 11];
      const recent = i % 37 === 0;
      const item = {
        id: 'app-' + i,
        name,
        type: game ? 'game' : 'app',
        sub: game
          ? ['GBA', 'SFC', 'PS', 'PICO-8', 'MD'][j % 5] + ' · 已玩 ' + (3 + (j * 7) % 40) + ' 小时'
          : ['本地应用', '工具', '媒体', '系统'][j % 4] + ' · 本地',
        img: IMG(art),
        frame: FRAME[name] || null,
        pos: POS[j % POS.length],
        favorite: j % 9 === 0,
        recent
      };
      if (name === '计算器') { item.material = true; item.mat = 'calc'; item.glyph = '='; item.img = null; }
      item.span = SPAN[name] || (recent ? [2, 2] : defaultSpan(i));
      out.push(item);
    });
    return out;
  }

  const BASE = buildBase();

  const EIGHT = [
    { id: 'read', span: [2, 2] },
    { id: 'terminal', span: [2, 2] },
    { id: 'town', span: [2, 2] },
    { id: 'clock', span: [2, 2] },
    { id: 'files', span: [2, 2] },
    { id: 'music', span: [2, 2] },
    { id: 'photos', span: [2, 2] },
    { id: 'settings', span: [2, 2] }
  ];

  function makeItems(count) {
    if (count === 8) {
      return EIGHT.map(x => ({ ...BASE.find(b => b.id === x.id), span: x.span }));
    }
    const out = [];
    for (let i = 0; i < count; i++) {
      const b = BASE[i % BASE.length];
      const round = Math.floor(i / BASE.length);
      const x = { ...b };
      if (round > 0) {
        x.id = b.id + '-' + round;
        x.name = b.name + ' ' + (round + 1);
        x.live = false;
        x.favorite = b.favorite && i % 13 === 0;
        x.recent = i % 37 === 0;
        if (!b.material) x.span = x.recent ? [2, 2] : defaultSpan(i);
      }
      out.push(x);
    }
    return out;
  }

  const CATS = [
    { name: '外观', icon: 'monitor', note: '主题、强调色与背景', rows: [
      { key: 'theme', label: '主题', desc: '深色适合夜间，浅色适合白天', type: 'select', values: ['深色', '浅色'] },
      { key: 'accent', label: '强调色', desc: '焦点、进度与开关共用一个颜色', type: 'accent' },
      { key: 'bg', label: '背景', desc: '预烘焙封面底图，或纯色', type: 'select', values: ['预烘焙', '纯色'] },
      { key: 'bright', label: '亮度', type: 'slider', v: 70 },
      { key: 'anim', label: '界面动画', desc: '焦点移动、墙平移与面板滑入', type: 'toggle', on: true }
    ] },
    { name: '声音', icon: 'volume', note: '扬声器与按键反馈', rows: [
      { key: 'vol', label: '音量', type: 'slider', v: 45 },
      { key: 'keySound', label: '按键音', desc: '按下手柄时发出轻响', type: 'toggle', on: false },
      { label: '输出', type: 'info', value: '扬声器' }
    ] },
    { name: '网络', icon: 'wifi', note: '无线连接状态', rows: [
      { key: 'wifi', label: 'Wi-Fi', desc: '连接无线网络', type: 'toggle', on: true },
      { key: 'bt', label: '蓝牙', desc: '连接手柄、耳机与音箱', type: 'toggle', on: false },
      { label: '已连接', type: 'info', value: 'Home-5G' },
      { label: 'IP 地址', type: 'info', value: '192.168.1.37' }
    ] },
    { name: '按键', icon: 'game', note: '掌机按键的作用', rows: [
      { label: '提示栏', type: 'info', value: '按住 Select' },
      { label: '开始', type: 'info', value: 'Start 快捷设置' },
      { label: '播放控制', type: 'info', value: 'X 暂停 · Y 下一首' },
      { label: '分类', type: 'info', value: 'L1 / R1' }
    ] },
    { name: '系统', icon: 'info', note: '关于这台掌机', rows: [
      { label: '语言', type: 'info', value: '简体中文' },
      { label: '设备', type: 'info', value: 'TrimUI Brick Pro' },
      { label: '版本', type: 'info', value: 'brickdesk 0.1.0' }
    ] }
  ];

  const SIDE = [
    { id: 'home', label: '主页', icon: 'home' },
    { id: 'search', label: '搜索', icon: 'search' },
    { id: 'messages', label: '消息', icon: 'bell' },
    { id: 'files', label: '文件', icon: 'folder' },
    { id: 'transfer', label: '无线传输', icon: 'swap' },
    { id: 'settings', label: '设置', icon: 'gear' },
    { id: 'power', label: '电源', icon: 'power', sub: true }
  ];
  const POWER = [
    { label: '睡眠', icon: 'moon' },
    { label: '重启', icon: 'restart' },
    { label: '关机', icon: 'power' }
  ];

  function findRow(key) {
    for (const c of CATS) for (const r of c.rows) if (r.key === key) return r;
    return null;
  }

  const frameFor = item => item.frame || item.img;

  window.BD = { ACCENTS, TABS, FEATURED, BASE, CATS, SIDE, POWER, makeItems, findRow, spanFor, frameFor };
})();
