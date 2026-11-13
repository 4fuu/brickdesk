/* brickdesk v3 — state, query params, theme/accent, data rebuild, debug */
(function () {
  const $ = id => document.getElementById(id);
  const q = new URLSearchParams(location.search);
  const CAPTURE = q.get('capture') === '1';
  const INSTANT = q.get('instant') === '1';
  const stage = $('stage');

  const S = {
    screen: 'home', tab: 0, count: 60, items: [], layout: null,
    fi: 5, navRow: 0, panX: 0,
    theme: 'dark', accent: 'persimmon', bg: 'prebaked', anim: true,
    hint: false, side: false, si: 0, sub: false, pi: 0,
    quick: false, qi: 0,
    cat: 0, oi: 0, szone: 'cats',
    vals: {}, clockText: CAPTURE ? '22:26' : null,
    running: [], app: null, launch: null, dialog: null, closingId: null
  };

  const MAX_RUNNING = 3;
  const MEM_SOFT = 560;
  const estMem = item => item.live ? 86 : item.type === 'game' ? 210 : 132;
  const itemById = id => S.items.find(x => x.id === id) || null;
  const runningOf = id => S.running.find(x => x.id === id) || null;
  const runningMem = () => S.running.reduce((sum, x) => sum + x.mem, 0);

  const LIVE = {
    playing: true,
    song: 0,
    line: 1,
    songs: [
      { title: '夜航', artist: '林间电台', duration: 228, progress: 38,
        lines: ['灯塔在身后慢慢变小', '风把航线吹成一条细线', '我们往没有名字的海去', '天亮以前不必回头'] },
      { title: '旧磁带', artist: '南窗', duration: 252, progress: 12,
        lines: ['按下播放键的那一秒', '夏天从喇叭里漏出来', '有些歌只在夜里才响', '转到尽头再翻一面'] }
    ]
  };

  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const match = item => S.tab === 0 || (S.tab === 1 && item.type === 'game') || (S.tab === 2 && item.type === 'app') || (S.tab === 3 && item.favorite);
  const liveItem = () => S.items.find(x => x.live) || S.items[5] || S.items[0];
  const maxPan = () => Math.max(0, (S.layout ? S.layout.wallW : 0) - 1024);
  const initialPan = pos => clamp(pos.px - 96, 0, maxPan());
  const mmss = sec => String(Math.floor(sec / 60)).padStart(2, '0') + ':' + String(sec % 60).padStart(2, '0');

  function clock() {
    if (S.clockText) return S.clockText;
    try {
      return new Intl.DateTimeFormat('zh-CN', { timeZone: 'Asia/Hong_Kong', hour: '2-digit', minute: '2-digit', hour12: false }).format(new Date());
    } catch (e) {
      const d = new Date();
      return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
    }
  }

  function initValues() {
    for (const c of BD.CATS) for (const r of c.rows) {
      if (!r.key) continue;
      if (r.type === 'slider') S.vals[r.key] = r.v;
      if (r.type === 'toggle') S.vals[r.key] = r.on;
      if (r.type === 'select') S.vals[r.key] = 0;
    }
  }

  function rebuild(count, focusId) {
    S.count = count;
    S.items = BD.makeItems(count);
    S.layout = BL.computeLayout(S.items);
    let idx = focusId ? S.items.findIndex(x => x.id === focusId) : 5;
    if (idx < 0) idx = 0;
    S.fi = idx;
    S.navRow = S.layout.placed[idx] ? S.layout.placed[idx].y : 0;
    S.panX = S.layout.placed[idx] ? initialPan(S.layout.placed[idx]) : 0;
  }

  function setTheme(theme) {
    S.theme = theme === 'light' ? 'light' : 'dark';
    stage.dataset.theme = S.theme;
  }

  function setAccent(name) {
    if (!BD.ACCENTS[name]) name = 'persimmon';
    S.accent = name;
    stage.dataset.accent = name;
    updateHarness();
  }

  function setBg(bg) { S.bg = bg === 'solid' ? 'solid' : 'prebaked'; stage.dataset.bg = S.bg; }
  function setAnim(on) { S.anim = !!on; stage.classList.toggle('noanim', !S.anim); }

  let ambIndex = 0;
  let ambKey = '';
  const ambLayers = [$('amb0'), $('amb1')];
  function setAmbient(item) {
    if (!item) return;
    const key = item.img || ('mat:' + (item.mat || 'terminal'));
    if (key === ambKey) return;
    ambKey = key;
    const next = ambLayers[1 - ambIndex];
    if (item.img) {
      next.style.backgroundImage = 'url("' + item.img + '")';
      next.style.backgroundColor = 'transparent';
    } else {
      next.style.backgroundImage = 'none';
      next.style.backgroundColor = 'var(--mat-' + (item.mat || 'terminal') + ')';
    }
    next.classList.add('on');
    ambLayers[ambIndex].classList.remove('on');
    ambIndex = 1 - ambIndex;
  }

  function updateHarness() {
    document.querySelectorAll('#accents .swbtn').forEach(b => b.classList.toggle('on', b.dataset.accent === S.accent));
    document.querySelectorAll('#counts .hbtn').forEach(b => b.classList.toggle('on', Number(b.dataset.count) === S.count));
  }

  function updateDebug() {
    const tiles = document.querySelectorAll('#wall .tile').length;
    const imgs = Array.from(document.querySelectorAll('#wall img'));
    const srcs = new Set(imgs.map(i => i.getAttribute('src')).filter(Boolean));
    const d = $('debug');
    if (!d) return;
    d.textContent = 'DOM ' + tiles + ' · IMG ' + imgs.length + ' · 源 ' + srcs.size +
      ' · 共 ' + S.items.length + ' · 墙 ' + Math.round(S.layout.wallW) + 'px · 空位 ' + S.layout.free;
  }

  let toastTimer = null;
  function toast(text, sticky) {
    const el = $('toast');
    if (!el) return;
    el.textContent = text;
    el.classList.add('on');
    clearTimeout(toastTimer);
    if (!sticky) toastTimer = setTimeout(() => el.classList.remove('on'), 1900);
  }

  function applyFocus(id) {
    let idx = /^\d+$/.test(id) ? Number(id) : S.items.findIndex(x => x.id === id);
    if (idx < 0 || idx >= S.items.length) return;
    S.fi = idx;
    S.navRow = S.layout.placed[idx].y;
    S.panX = initialPan(S.layout.placed[idx]);
  }

  function seedRunning(item, state) {
    const run = runningOf(item.id);
    if (run) { run.state = state; return run; }
    const entry = {
      id: item.id, name: item.name, mem: estMem(item), state,
      frame: BD.frameFor(item), since: Date.now()
    };
    S.running.push(entry);
    return entry;
  }


  function fit() {
    if (CAPTURE) return;
    const k = Math.max(.4, Math.min((innerWidth - 36) / 1024, (innerHeight - 196) / 768, 1));
    $('fit').style.width = 1024 * k + 'px';
    $('fit').style.height = 768 * k + 'px';
    stage.style.transform = 'scale(' + k + ')';
  }

  window.ST = { S, LIVE, $, q, CAPTURE, INSTANT, stage, clamp, esc, match, liveItem, maxPan, initialPan,
    mmss, clock, rebuild, setTheme, setAccent, setBg, setAnim, setAmbient, updateHarness, updateDebug,
    toast, applyFocus, fit, initValues, MAX_RUNNING, MEM_SOFT, estMem, itemById, runningOf, runningMem, seedRunning };
})();
