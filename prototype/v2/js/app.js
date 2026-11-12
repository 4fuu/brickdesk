/* brickdesk v2 — interaction prototype
   Everything is drawn at 1024×768. The browser pad is only a test harness. */
(function () {
  const D = window.BD_DATA;
  const items = D.items;
  const tabs = D.tabs;
  const sideOpts = D.sideOpts;
  const power = D.power;
  const cats = D.cats;

  const U = 180;
  const GAP = 16;
  const WW = 7 * U;
  const WH = 5 * U;
  let panX = 0;
  let panY = 0;

  const $ = id => document.getElementById(id);
  const stage = $('stage');
  const view = $('view');
  const hintEl = $('hint');
  const toastEl = $('toast');
  const scrim = $('scrim');
  const sideEl = $('side');
  const quickEl = $('quick');
  const veil = $('veil');
  const dialogEl = $('dialog');
  const amb = [$('amb0'), $('amb1')];

  const S = {
    screen: 'home', tab: 0, fi: 0,
    cat: 0, oi: 0, szone: 'cats',
    side: false, si: 0, sub: false, pi: 0,
    quick: false, qi: 0,
    dialog: null, dialogFocus: 0
  };

  const q = new URLSearchParams(location.search);
  const capture = q.get('capture') === '1';
  const forceInstant = capture || q.get('instant') === '1';
  let hintOn = false;
  let hintHeld = false;
  let ambientOn = 1;
  let currentAmbient = '';
  let toastTimer = null;
  let bootHintTimer = null;

  function opt(key) {
    for (const c of cats) for (const o of c.opts) if (o.key === key) return o;
    return null;
  }
  const themeIndex = () => (opt('theme').i ? 1 : 0);
  const clock = () => {
    try {
      return new Intl.DateTimeFormat('zh-CN', { timeZone: 'Asia/Shanghai', hour: '2-digit', minute: '2-digit', hour12: false }).format(new Date());
    } catch (e) {
      const d = new Date();
      return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
    }
  };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));

  function statusHTML() {
    return '<div class="status">' + ico('wifi', 22, 1.75) +
      '<span class="battery"><i></i></span><span class="clock">' + clock() + '</span></div>';
  }

  function mastheadHTML() {
    const tabHTML = tabs.map((t, i) => '<span class="tab' + (i === S.tab ? ' on' : '') + '" data-tab="' + i + '">' + t + '</span>').join('');
    return '<header class="masthead">' +
      '<div class="brand"><span class="brand-mark"><i></i><i></i><i></i><i></i></span>brickdesk</div>' +
      '<nav class="tabs">' + tabHTML + '</nav>' + statusHTML() + '</header>';
  }

  function pageHeadHTML() {
    return '<header class="masthead is-page">' +
      '<span class="back-mark">' + ico('left', 24, 2) + '</span>' +
      '<span class="page-title">设置</span>' + statusHTML() + '</header>';
  }

  const match = a => S.tab === 0 || (S.tab === 1 && a.type === 'game') || (S.tab === 2 && a.type === 'app') || (S.tab === 3 && !!a.fav);

  function fmtTime(text) {
    const p = text.split(':').map(Number);
    return p[0] * 60 + p[1];
  }
  function mmss(sec) {
    const s = Math.max(0, Math.round(sec));
    return String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
  }

  function liveCaption(a) {
    const song = a.songs[a.song];
    const now = Math.round(fmtTime(song.duration) * song.progress / 100);
    return '<div class="cap live-cap">' +
      '<div class="live-head"><span class="name">音乐</span><span class="live-badge"><i></i>' + (a.playing ? '播放中' : '已暂停') + '</span></div>' +
      '<div class="track">' + song.name + ' <span>' + song.artist + '</span></div>' +
      '<div class="lyric">' + (a.playing ? song.lines[a.line] : '已暂停') + '</div>' +
      '<div class="progress"><div class="bar"><i style="width:' + song.progress + '%"></i></div>' +
      '<div class="time"><span>' + mmss(now) + '</span><span>' + song.duration + '</span></div></div></div>';
  }

  function captionHTML(a) {
    if (a.live) return liveCaption(a);
    const icon = a.icon && a.type === 'app' ? '<span class="mini-ico">' + ico(a.icon, 18, 1.75) + '</span>' : '';
    const meta = [a.platform, a.desc].filter(Boolean).join(' · ');
    return '<div class="name-row">' + icon + '<span class="name">' + a.name + '</span></div>' +
      (meta ? '<div class="meta">' + meta + '</div>' : '');
  }

  function tileHTML(a, i) {
    const wpx = a.w * U - GAP;
    const hpx = a.h * U - GAP;
    const capH = a.live ? 146 : a.hero ? 86 : (a.w * a.h >= 4 ? 76 : (a.h > a.w ? 74 : 50));
    const cls = ['tile'];
    if (a.type === 'game') cls.push('game');
    if (a.hero) cls.push('hero');
    if (a.w * a.h >= 4 && !a.hero) cls.push('big');
    if (a.w === 1 && a.h === 1) cls.push('small');
    if (a.h > a.w && !a.live) cls.push('tall');
    if (a.live) cls.push('live');
    if (a.material) cls.push('material');
    let art;
    if (a.material) {
      const glyph = a.glyph ? '<span class="glyph prompt">' + a.glyph + '</span>' : '<span class="glyph">' + ico(a.icon || 'term', 64, 1.5) + '</span>';
      art = '<div class="art" style="--cover-color:' + a.accent + ';--on-cover:' + (a.onCover || '#EFE8DA') + '">' + glyph + '</div>';
    } else {
      art = '<div class="art"><img src="' + a.img + '" alt="" style="object-position:' + (a.pos || 'center') + '"></div>';
    }
    const cap = a.live ? liveCaption(a) : '<div class="cap">' + captionHTML(a) + '</div>';
    return '<button class="' + cls.join(' ') + '" data-i="' + i + '" aria-label="' + a.name + '" ' +
      'style="left:' + (a.x * U + GAP / 2) + 'px;top:' + (a.y * U + GAP / 2) + 'px;width:' + wpx + 'px;height:' + hpx + 'px;--cap-h:' + capH + 'px">' +
      art + cap + '</button>';
  }

  function buildHome() {
    view.innerHTML = mastheadHTML() + '<div class="wall" id="wall">' + items.map(tileHTML).join('') + '</div>';
    view.onclick = e => {
      const tile = e.target.closest('.tile');
      if (!tile) return;
      const i = Number(tile.dataset.i);
      if (S.fi !== i) { S.fi = i; render(); }
    };
  }

  function updateHome() {
    if (!view.querySelector('.wall')) buildHome();
    const wallEl = $('wall');
    view.querySelectorAll('.tab').forEach((el, i) => el.classList.toggle('on', i === S.tab));
    let focused = items[S.fi];
    if (!focused || !match(focused)) {
      const first = items.findIndex(match);
      if (first >= 0) S.fi = first;
      focused = items[S.fi];
    }
    items.forEach((a, i) => {
      const el = wallEl.querySelector('[data-i="' + i + '"]');
      if (!el) return;
      el.classList.toggle('f', i === S.fi);
      el.classList.toggle('off', !match(a));
      if (a.live) {
        const cap = el.querySelector('.cap');
        if (cap) cap.outerHTML = liveCaption(a);
      }
    });
    const a = items[S.fi];
    const viewW = 1024 - 48;
    const viewH = 768 - 94 - 44;
    const mx = 32;
    const my = 88;
    const mb = 46;
    let x = panX;
    let y = panY;
    const left = a.x * U;
    const right = (a.x + a.w) * U;
    const top = a.y * U;
    const bottom = (a.y + a.h) * U;
    if (left < x + mx) x = left - mx;
    if (right > x + viewW - mx) x = right - viewW + mx;
    if (top < y + my) y = top - my;
    if (bottom > y + viewH - mb) y = bottom - viewH + mb;
    x = clamp(x, 0, WW - viewW);
    y = clamp(y, 0, WH - viewH);
    panX = x;
    panY = y;
    wallEl.style.transform = 'translate3d(' + (-x) + 'px,' + (-y) + 'px,0)';
    stage.classList.toggle('fade-right', a.x + a.w < 6.5);
    stage.classList.toggle('fade-bottom', a.y + a.h < 4.35);
    setAmbient(a);
  }

  function setAmbient(a) {
    const key = a.img || ('color:' + a.accent);
    if (key === currentAmbient) return;
    currentAmbient = key;
    const next = 1 - ambientOn;
    const layer = amb[next];
    if (a.img) {
      layer.style.backgroundImage = 'url("' + a.img + '")';
      layer.style.backgroundColor = 'transparent';
    } else {
      layer.style.backgroundImage = 'none';
      layer.style.backgroundColor = a.accent;
    }
    layer.classList.add('on');
    amb[ambientOn].classList.remove('on');
    ambientOn = next;
  }

  function valHTML(o) {
    if (o.type === 'select') {
      return '<span class="val"><span class="ch">' + ico('left', 18, 2) + '</span><span>' + o.values[o.i] + '</span><span class="ch">' + ico('right', 18, 2) + '</span></span>';
    }
    if (o.type === 'slider') {
      return '<span class="val"><span class="slider"><span class="rail"><i style="width:' + o.v + '%"></i><span class="knob" style="left:' + o.v + '%"></span></span><span class="num">' + o.v + '</span></span></span>';
    }
    if (o.type === 'toggle') {
      return '<span class="val"><span class="value">' + (o.on ? '开' : '关') + '</span><span class="switch ' + (o.on ? 'on' : '') + '"><i></i></span></span>';
    }
    return '<span class="val"><span class="info">' + (o.value || '') + '</span></span>';
  }

  function renderSettings() {
    const c = cats[S.cat];
    const catsHTML = cats.map((k, i) => {
      const cls = 'set-cat' + (i === S.cat ? ' cur' : '') + (S.szone === 'cats' && i === S.cat ? ' f' : '');
      return '<div class="' + cls + '" data-cat="' + i + '">' + ico(k.icon, 24, 1.75) + '<span>' + k.name + '</span></div>';
    }).join('');
    const rows = c.opts.map((o, i) => {
      const f = S.szone === 'opts' && i === S.oi ? ' f' : '';
      const tall = o.desc ? ' tall' : '';
      return '<div class="set-row' + tall + f + '">' +
        '<div class="set-label"><div class="l">' + o.label + '</div>' + (o.desc ? '<div class="d">' + o.desc + '</div>' : '') + '</div>' +
        valHTML(o) + '</div>';
    }).join('');
    view.innerHTML = pageHeadHTML() +
      '<div class="settings">' +
      '<div class="set-side"><div class="set-cats">' + catsHTML + '</div>' +
      '<div class="set-side-note"><strong>设置会即时保存</strong>按 B 返回主页</div></div>' +
      '<div class="set-main"><div class="set-heading"><h2>' + c.name + '</h2><p>' + c.note + '</p></div>' +
      '<div class="set-sheet">' + rows + '</div></div>' +
      '</div>';
    stage.classList.remove('fade-right', 'fade-bottom');
    view.onclick = e => {
      const cat = e.target.closest('.set-cat');
      if (cat) { S.cat = Number(cat.dataset.cat); S.oi = 0; S.szone = 'cats'; render(); }
    };
  }

  function sideItems() {
    return [{ label: '主页', icon: 'home' },
      ...sideOpts.filter(o => o.on).map(o => ({ label: o.label, icon: o.icon })),
      { label: '设置', icon: 'gear' },
      { label: '电源', icon: 'power', sub: true }];
  }

  function renderSide() {
    sideEl.classList.toggle('on', S.side);
    if (!S.side) return;
    const it = sideItems();
    if (S.si >= it.length) S.si = it.length - 1;
    if (S.sub) {
      sideEl.innerHTML = '<div class="p-head"><span class="p-title">电源</span><span class="p-meta">按 B 返回</span></div>' +
        '<div style="height:18px"></div><div class="power-list">' +
        power.map((p, i) => '<div class="mi ' + (i === S.pi ? 'f' : '') + '">' + ico(p.icon, 25, 1.75) + '<span>' + p.label + '</span></div>').join('') +
        '</div><div class="power-note">睡眠会保持当前进度。<br>关机前会再确认一次。</div><div class="side-spacer"></div>' +
        footHTML([['A', '确定'], ['B', '返回']]) + '';
      return;
    }
    const cur = S.screen === 'home' ? '主页' : '设置';
    const split = it.findIndex(x => x.label === '设置');
    const row = (x, i) => '<div class="mi ' + (i === S.si ? 'f ' : '') + (x.label === cur ? 'cur' : '') + '">' +
      ico(x.icon, 25, 1.75) + '<span>' + x.label + '</span>' + (x.sub ? '<span class="r">' + ico('right', 20, 2) + '</span>' : '') + '</div>';
    sideEl.innerHTML = '<div class="p-head"><span class="p-title">菜单</span><span class="p-meta">' + clock() + '</span></div>' +
      '<div class="side-list">' + it.slice(0, split).map((x, i) => row(x, i)).join('') + '</div>' +
      '<div class="side-group"></div><div class="side-list" style="padding-top:0">' + it.slice(split).map((x, i) => row(x, i + split)).join('') + '</div>' +
      '<div class="side-spacer"></div>' + footHTML([['A', '打开'], ['B', '关闭']]);
  }

  function footHTML(keys) {
    return '<div class="p-foot">' + keys.map(([k, label]) => '<div class="fgroup"><span class="kbd">' + k + '</span><span>' + label + '</span></div>').join('') + '</div>';
  }

  function quickOpts() {
    return ['bright', 'vol', 'wifi', 'bt', 'theme'].map(opt);
  }

  function renderQuick() {
    quickEl.classList.toggle('on', S.quick);
    if (!S.quick) return;
    const music = items.find(a => a.live);
    const song = music.songs[music.song];
    const rows = quickOpts().map((o, i) => '<div class="mi ' + (i === S.qi ? 'f' : '') + '">' + ico(o.key === 'theme' ? (themeIndex() ? 'sun' : 'moon') : (o.icon || 'sliders'), 25, 1.75) +
      '<span>' + o.label + '</span>' + valHTML(o) + '</div>').join('');
    quickEl.innerHTML = '<div class="p-head"><span class="p-title">快捷设置</span><span class="p-meta">电量 <strong>82%</strong></span></div>' +
      '<div class="now"><div class="cover"><img src="' + music.img + '" alt=""></div>' +
      '<div class="info"><div class="now-kicker">正在播放</div><div class="now-title">' + song.name + '</div><div class="now-artist">' + song.artist + '</div>' +
      '<div class="now-progress"><i style="width:' + song.progress + '%"></i></div></div>' +
      '<div class="play">' + ico(music.playing ? 'pause' : 'play', 18, 1.75) + '</div></div>' +
      '<div class="quick-list">' + rows + '</div><div class="side-spacer"></div>' +
      footHTML([['←→', '调整'], ['A', '切换'], ['B', '关闭']]);
  }

  function renderDialog() {
    if (!S.dialog) {
      veil.classList.remove('on');
      dialogEl.classList.remove('on');
      return;
    }
    veil.classList.add('on');
    dialogEl.classList.add('on');
    dialogEl.innerHTML = '<h3>' + S.dialog.title + '</h3><p>' + S.dialog.body + '</p>' +
      '<div class="dialog-actions">' +
      '<span class="btn ' + (S.dialogFocus === 0 ? 'f' : '') + '">取消</span>' +
      '<span class="btn primary ' + (S.dialogFocus === 1 ? 'f' : '') + '">' + S.dialog.confirm + '</span></div>';
  }

  function render() {
    stage.dataset.theme = themeIndex() ? 'light' : 'dark';
    stage.classList.toggle('noanim', !opt('anim').on);
    stage.classList.toggle('instant', forceInstant);
    if (S.screen === 'home') updateHome(); else renderSettings();
    renderSide();
    renderQuick();
    renderDialog();
    scrim.classList.toggle('on', S.side || S.quick || !!S.dialog);
    stage.classList.toggle('panel-open', S.side || S.quick || !!S.dialog);
    if (hintOn) {
      hintEl.innerHTML = hintHtml();
      hintEl.classList.add('on');
    } else {
      hintEl.classList.remove('on');
    }
  }

  function hintHtml() {
    let left = [];
    let right = [];
    const k = t => '<span class="kbd dark">' + t + '</span>';
    const item = (keys, label) => '<div class="hint-item">' + keys.map(k).join('') + '<span>' + label + '</span></div>';
    if (S.dialog) {
      left = [item(['←→'], '选择'), item(['A'], '确定')];
      right = [item(['B'], '取消')];
    } else if (S.side) {
      left = [item(['↑↓'], '移动')];
      right = S.sub ? [item(['A'], '确定'), item(['B'], '返回')] : [item(['A'], '打开'), item(['B'], '关闭')];
    } else if (S.quick) {
      left = [item(['↑↓'], '移动'), item(['←→'], '调整')];
      right = [item(['A'], '切换'), item(['B'], '关闭')];
    } else if (S.screen === 'home') {
      left = [item(['MENU'], '菜单'), item(['START'], '快捷设置')];
      right = [item(['L1', 'R1'], '分类')];
      if (items[S.fi] && items[S.fi].live) right.push(item(['X'], items[S.fi].playing ? '暂停' : '播放'), item(['Y'], '下一首'));
      right.push(item(['A'], '打开'));
    } else if (S.szone === 'cats') {
      left = [item(['↑↓'], '分类')];
      right = [item(['A'], '进入'), item(['B'], '主页')];
    } else {
      left = [item(['←→'], '调整')];
      right = [item(['A'], '选择'), item(['B'], '返回')];
    }
    return '<div class="hint-group">' + left.join('') + '</div><div class="hint-sep"></div><div class="hint-group">' + right.join('') + '</div>';
  }

  function showHint() {
    hintOn = true;
    hintEl.innerHTML = hintHtml();
    hintEl.classList.add('on');
  }
  function hideHint() {
    hintOn = false;
    hintEl.classList.remove('on');
  }

  function toast(text, persist) {
    toastEl.textContent = text;
    toastEl.classList.add('on');
    clearTimeout(toastTimer);
    if (!persist) toastTimer = setTimeout(() => toastEl.classList.remove('on'), 1900);
  }

  function move(d) {
    const horizontal = d === 'left' || d === 'right';
    const forward = d === 'right' || d === 'down';
    const f = items[S.fi];
    let best = -1;
    let bestScore = 1e9;
    items.forEach((c, i) => {
      if (i === S.fi || !match(c)) return;
      const gap = horizontal ? (forward ? c.x - f.x - f.w : f.x - c.x - c.w) : (forward ? c.y - f.y - f.h : f.y - c.y - c.h);
      if (gap < 0) return;
      const overlap = horizontal
        ? Math.min(f.y + f.h, c.y + c.h) - Math.max(f.y, c.y)
        : Math.min(f.x + f.w, c.x + c.w) - Math.max(f.x, c.x);
      const off = Math.abs(horizontal ? c.y + c.h / 2 - f.y - f.h / 2 : c.x + c.w / 2 - f.x - f.w / 2);
      const score = gap * 4 + off + (overlap > 0 ? 0 : 8);
      if (score < bestScore) { bestScore = score; best = i; }
    });
    if (best >= 0) S.fi = best;
  }

  function adjust(o, d, wrap) {
    if (!o) return;
    if (o.type === 'select') o.i = wrap ? (o.i + d + o.values.length) % o.values.length : clamp(o.i + d, 0, o.values.length - 1);
    if (o.type === 'slider') o.v = clamp(o.v + d * 5, 0, 100);
    if (o.type === 'toggle') o.on = !o.on;
  }

  function openSettings() {
    S.screen = 'settings';
    S.szone = 'cats';
    S.cat = 0;
    S.oi = 0;
    S.side = false;
    S.quick = false;
    render();
  }

  function sideConfirm() {
    const it = sideItems();
    const x = it[S.si];
    if (x.sub) { S.sub = true; S.pi = 0; return; }
    S.side = false;
    if (x.label === '主页') { S.screen = 'home'; render(); }
    else if (x.label === '设置') openSettings();
    else { render(); toast('正在打开 ' + x.label); }
  }

  function powerConfirm() {
    const p = power[S.pi];
    if (p.label === '关机') {
      S.side = false;
      S.sub = false;
      S.dialog = { title: '关机', body: '所有未保存的进度会保留。确定要关机吗？', confirm: '关机', action: () => toast('关机（演示）') };
      S.dialogFocus = 0;
      render();
      return;
    }
    S.side = false;
    S.sub = false;
    render();
    toast(p.label + '（演示）');
  }

  function confirm() {
    if (S.dialog) { confirmDialog(); return; }
    if (S.side) {
      if (S.sub) powerConfirm(); else sideConfirm();
      return;
    }
    if (S.quick) { adjust(quickOpts()[S.qi], 1, true); render(); return; }
    if (S.screen === 'home') {
      const a = items[S.fi];
      if (a.name === '设置') openSettings();
      else toast('正在打开 ' + a.name);
      return;
    }
    if (S.szone === 'cats') { S.szone = 'opts'; S.oi = 0; }
    else adjust(cats[S.cat].opts[S.oi], 1, true);
    render();
  }

  function back() {
    if (S.dialog) { S.dialog = null; render(); return; }
    if (S.side) { if (S.sub) S.sub = false; else S.side = false; render(); return; }
    if (S.quick) { S.quick = false; render(); return; }
    if (S.screen === 'settings') {
      if (S.szone === 'opts') { S.szone = 'cats'; render(); }
      else { S.screen = 'home'; render(); }
    }
  }

  function confirmDialog() {
    if (S.dialogFocus === 0) { S.dialog = null; render(); return; }
    const action = S.dialog.action;
    S.dialog = null;
    render();
    if (action) action();
  }

  function press(k) {
    if (S.dialog) {
      if (k === 'left' || k === 'right') { S.dialogFocus = 1 - S.dialogFocus; render(); }
      else if (k === 'a') confirmDialog();
      else if (k === 'b') { S.dialog = null; render(); }
      return;
    }
    if (S.side) {
      if (S.sub) {
        if (k === 'up') S.pi = clamp(S.pi - 1, 0, power.length - 1);
        else if (k === 'down') S.pi = clamp(S.pi + 1, 0, power.length - 1);
        else if (k === 'left' || k === 'b') S.sub = false;
        else if (k === 'a') powerConfirm();
      } else {
        const it = sideItems();
        if (k === 'up') S.si = clamp(S.si - 1, 0, it.length - 1);
        else if (k === 'down') S.si = clamp(S.si + 1, 0, it.length - 1);
        else if (k === 'right' && it[S.si].sub) { S.sub = true; S.pi = 0; }
        else if (k === 'left' || k === 'b') S.side = false;
        else if (k === 'a') sideConfirm();
      }
      render();
      return;
    }
    if (S.quick) {
      if (k === 'up') S.qi = clamp(S.qi - 1, 0, 4);
      else if (k === 'down') S.qi = clamp(S.qi + 1, 0, 4);
      else if (k === 'left') adjust(quickOpts()[S.qi], -1);
      else if (k === 'right') adjust(quickOpts()[S.qi], 1);
      else if (k === 'a') adjust(quickOpts()[S.qi], 1, true);
      else if (k === 'b') S.quick = false;
      render();
      return;
    }
    if (S.screen === 'home') {
      if (['left', 'right', 'up', 'down'].includes(k)) move(k);
      else if (k === 'a') confirm();
      else if (k === 'b') { /* home is the root */ }
      else if (k === 'l1' || k === 'r1') {
        S.tab = (S.tab + (k === 'r1' ? 1 : -1) + tabs.length) % tabs.length;
        if (!match(items[S.fi])) { const first = items.findIndex(match); if (first >= 0) S.fi = first; }
      } else if (k === 'x' || k === 'y') quickAct(k);
      render();
      return;
    }
    if (S.szone === 'cats') {
      if (k === 'up') S.cat = clamp(S.cat - 1, 0, cats.length - 1);
      else if (k === 'down') S.cat = clamp(S.cat + 1, 0, cats.length - 1);
      else if (k === 'right' || k === 'a') S.szone = 'opts';
      else if (k === 'b') { S.screen = 'home'; }
      S.oi = 0;
      render();
      return;
    }
    const opts = cats[S.cat].opts;
    if (k === 'up') S.oi = clamp(S.oi - 1, 0, opts.length - 1);
    else if (k === 'down') S.oi = clamp(S.oi + 1, 0, opts.length - 1);
    else if (k === 'left') adjust(opts[S.oi], -1);
    else if (k === 'right') adjust(opts[S.oi], 1);
    else if (k === 'a') adjust(opts[S.oi], 1, true);
    else if (k === 'b') S.szone = 'cats';
    render();
  }

  function quickAct(k) {
    const a = items[S.fi];
    if (!a || !a.live) return;
    if (k === 'x') a.playing = !a.playing;
    else {
      a.song = (a.song + 1) % a.songs.length;
      a.line = 0;
      a.playing = true;
    }
    render();
  }

  function menu() {
    if (S.dialog) return;
    S.quick = false;
    S.side = !S.side;
    S.sub = false;
    if (S.side) S.si = Math.max(0, sideItems().findIndex(x => x.label === (S.screen === 'home' ? '主页' : '设置')));
    render();
  }

  function start() {
    if (S.dialog) return;
    if (S.side) { S.side = false; S.quick = true; S.qi = 0; render(); return; }
    S.quick = !S.quick;
    if (S.quick) S.qi = 0;
    render();
  }

  function setTheme() {
    const t = opt('theme');
    t.i = 1 - t.i;
    render();
    toast('已切换为' + t.values[t.i]);
  }

  const KEY = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', z: 'a', Enter: 'a', x: 'b', Escape: 'b', c: 'x', v: 'y', q: 'l1', e: 'r1', m: 'menu', s: 'start' };
  const held = {};

  addEventListener('keydown', e => {
    const key = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (key === 't') { if (!e.repeat) setTheme(); return; }
    if (key === 'Tab') {
      e.preventDefault();
      if (!e.repeat) { hintHeld = true; showHint(); }
      return;
    }
    const k = KEY[key];
    if (!k) return;
    e.preventDefault();
    if (held[k] && !['up', 'down', 'left', 'right'].includes(k)) return;
    held[k] = true;
    if (k === 'menu') menu();
    else if (k === 'start') start();
    else press(k);
  });

  addEventListener('keyup', e => {
    const key = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (key === 'Tab') { hintHeld = false; hideHint(); return; }
    const k = KEY[key];
    if (k) { held[k] = false; }
  });
  addEventListener('blur', () => { hintHeld = false; hideHint(); });

  document.querySelectorAll('#pad [data-k]').forEach(btn => {
    const k = btn.dataset.k;
    const down = e => {
      e.preventDefault();
      if (k === 'select') { hintHeld = true; showHint(); return; }
      if (k === 'menu') menu();
      else if (k === 'start') start();
      else press(k);
    };
    const up = () => {
      if (k === 'select') { hintHeld = false; hideHint(); return; }
      held[k] = false;
    };
    btn.addEventListener('pointerdown', down);
    ['pointerup', 'pointerleave', 'pointercancel'].forEach(n => btn.addEventListener(n, up));
  });

  document.querySelectorAll('#pad [data-act]').forEach(btn => {
    btn.addEventListener('click', () => {
      const act = btn.dataset.act;
      if (act === 'theme') setTheme();
      else if (act === 'side') menu();
      else if (act === 'quick') start();
      else if (act === 'settings') openSettings();
      else if (act === 'hint') { if (hintOn) hideHint(); else showHint(); }
    });
  });

  function fit() {
    if (document.body.classList.contains('capture')) {
      stage.style.transform = 'none';
      return;
    }
    const s = Math.min((innerWidth - 32) / 1024, (innerHeight - 184) / 768, 1.2);
    const k = Math.max(s, .3);
    $('fit').style.width = 1024 * k + 'px';
    $('fit').style.height = 768 * k + 'px';
    stage.style.transform = 'scale(' + k + ')';
  }
  addEventListener('resize', fit);

  function applyQuery() {
    if (capture) document.body.classList.add('capture');
    if (q.get('theme') === 'light') opt('theme').i = 1;
    if (q.get('theme') === 'dark') opt('theme').i = 0;
    if (q.get('tab')) S.tab = clamp(Number(q.get('tab')) || 0, 0, tabs.length - 1);
    if (q.get('focus')) S.fi = clamp(Number(q.get('focus')) || 0, 0, items.length - 1);
    if (q.get('screen') === 'settings') {
      S.screen = 'settings';
      S.side = false;
      S.quick = false;
    }
    if (q.get('cat')) S.cat = clamp(Number(q.get('cat')) || 0, 0, cats.length - 1);
    if (q.get('zone') === 'opts') S.szone = 'opts';
    if (q.get('oi')) S.oi = clamp(Number(q.get('oi')) || 0, 0, cats[S.cat].opts.length - 1);
    if (q.get('side') === '1') { S.side = true; S.si = clamp(Number(q.get('si')) || 0, 0, sideItems().length - 1); }
    if (q.get('quick') === '1') { S.quick = true; S.qi = clamp(Number(q.get('qi')) || 0, 0, 4); }
    if (q.get('hint') === '1') hintOn = true;
    if (q.get('dialog') === '1') { S.dialog = { title: '关机', body: '所有未保存的进度会保留。确定要关机吗？', confirm: '关机', action: () => toast('关机（演示）') }; S.dialogFocus = 0; }
  }

  function boot() {
    applyQuery();
    if (forceInstant) stage.classList.add('instant');
    render();
    fit();
    if (hintOn) showHint();
    if (q.get('toast')) toast(q.get('toast'), true);
    if (!capture && !q.get('hint')) {
      bootHintTimer = setTimeout(() => {
        if (!hintHeld) showHint();
        bootHintTimer = setTimeout(() => { if (!hintHeld) hideHint(); }, 2600);
      }, 700);
    }
    setInterval(() => {
      const clockEls = document.querySelectorAll('.clock');
      clockEls.forEach(el => { el.textContent = clock(); });
    }, 30000);
    setInterval(() => {
      const a = items.find(x => x.live);
      if (!a || !a.playing || capture) return;
      const song = a.songs[a.song];
      a.line = (a.line + 1) % song.lines.length;
      if (S.screen === 'home') {
        const el = $('wall') && $('wall').querySelector('[data-i="' + items.indexOf(a) + '"]');
        if (el) { const cap = el.querySelector('.cap'); if (cap) cap.outerHTML = liveCaption(a); }
      }
      if (hintOn) hintEl.innerHTML = hintHtml();
    }, 2400);
  }

  boot();
})();
