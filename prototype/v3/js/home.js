/* brickdesk v3 — home wall rendering, virtualization, spatial focus, launch/restore */
(function () {
  const { S, $, clamp, esc, match, maxPan, initialPan, itemById, runningOf, updateDebug } = window.ST;
  const BUF = 1024;
  const tileEls = new Map();
  let pan = 0;
  let pruneTimer = null;
  let gen = 0;

  function ensureTile(item, pos, index) {
    let el = tileEls.get(item.id);
    if (!el) {
      const wrap = document.createElement('div');
      wrap.innerHTML = TILES.tileHTML(item, pos, index);
      el = wrap.firstElementChild;
      tileEls.set(item.id, el);
      $('wall').appendChild(el);
    }
    return el;
  }

  function intersects(pos, from, to) {
    return pos.px < to && pos.right > from;
  }

  function syncWall() {
    const old = pan;
    const target = S.panX;
    pan = target;
    const wall = $('wall');
    wall.style.width = S.layout.wallW + 'px';
    wall.style.transform = 'translate3d(' + (-target) + 'px,0,0)';

    const from = Math.min(old, target) - BUF;
    const to = Math.max(old, target) + 1024 + BUF;
    const active = new Set();
    S.layout.placed.forEach((pos, i) => {
      const item = S.items[i];
      if (!item) return;
      if (intersects(pos, from, to) || i === S.fi) {
        active.add(item.id);
        const el = ensureTile(item, pos, i);
        TILES.paintTile(el, item, pos, i);
      }
    });
    for (const [id, el] of tileEls) {
      if (active.has(id)) continue;
      const img = el.querySelector('img');
      if (img) img.removeAttribute('src');
      el.remove();
      tileEls.delete(id);
    }
    updateDebug();

    gen++;
    const my = gen;
    clearTimeout(pruneTimer);
    const finalFrom = target - BUF;
    const finalTo = target + 1024 + BUF;
    const prune = () => {
      if (my !== gen) return;
      for (const [id, el] of tileEls) {
        const pos = S.layout.placed.find(p => S.items[p.i] && S.items[p.i].id === id);
        if (!pos || !intersects(pos, finalFrom, finalTo)) {
          const img = el.querySelector('img');
          if (img) img.removeAttribute('src');
          el.remove();
          tileEls.delete(id);
        }
      }
      updateDebug();
    };
    if (window.ST.INSTANT || document.getElementById('stage').classList.contains('noanim')) prune();
    else pruneTimer = setTimeout(prune, 260);
  }

  function render() {
    const home = $('home');
    home.hidden = S.screen !== 'home';
    if (home.hidden) { updateDebug(); return; }
    if (!$('homebar').firstChild) window.CH.buildHomeBar();
    window.CH.updateHomeBar();
    syncWall();
    const item = S.items[S.fi];
    if (item) window.ST.setAmbient(item);
  }

  function ensureVisible() {
    const pos = S.layout.placed[S.fi];
    if (!pos) return;
    let x = S.panX;
    if (pos.px < x + 64) x = pos.px - 64;
    if (pos.right > x + 1024 - 64) x = pos.right - 1024 + 64;
    S.panX = clamp(x, 0, maxPan());
  }

  function move(dir) {
    const f = S.layout.placed[S.fi];
    if (!f) return;
    const horizontal = dir === 'left' || dir === 'right';
    let pool = S.layout.placed.filter(c => c.i !== S.fi && match(S.items[c.i]));
    pool = pool.filter(c => {
      if (dir === 'right') return c.px >= f.right;
      if (dir === 'left') return c.right <= f.px;
      if (dir === 'down') return c.py >= f.bottom;
      return c.bottom <= f.py;
    });
    if (!pool.length) return;
    if (horizontal) {
      const band = pool.filter(c => c.y <= S.navRow && S.navRow < c.y + c.h);
      if (band.length) pool = band;
    }
    let best = null;
    let bestScore = Infinity;
    for (const c of pool) {
      const primary = dir === 'right' ? c.px - f.right : dir === 'left' ? f.px - c.right : dir === 'down' ? c.py - f.bottom : f.py - c.bottom;
      const overlap = horizontal
        ? Math.min(f.y + f.h, c.y + c.h) - Math.max(f.y, c.y)
        : Math.min(f.x + f.w, c.x + c.w) - Math.max(f.x, c.x);
      const cross = horizontal
        ? Math.abs((c.y + c.h / 2) - (S.navRow + .5))
        : Math.abs((c.x + c.w / 2) - (f.x + f.w / 2));
      const miss = overlap > 0 ? 0 : 90 - overlap * 22;
      const score = primary * 10 + cross * 4 + miss;
      if (score < bestScore) { bestScore = score; best = c; }
    }
    if (!best) return;
    S.fi = best.i;
    S.navRow = horizontal ? clamp(S.navRow, best.y, best.y + best.h - 1) : (dir === 'down' ? best.y : best.y + best.h - 1);
    ensureVisible();
  }

  function setTab(delta) {
    const old = S.layout.placed[S.fi];
    S.tab = (S.tab + delta + BD.TABS.length) % BD.TABS.length;
    let best = null;
    let dist = Infinity;
    for (const c of S.layout.placed) {
      if (!match(S.items[c.i])) continue;
      const d = Math.abs(c.x + c.w / 2 - old.x - old.w / 2) + Math.abs(c.y + c.h / 2 - old.y - old.h / 2);
      if (d < dist) { dist = d; best = c; }
    }
    if (best) { S.fi = best.i; S.navRow = clamp(S.navRow, best.y, best.y + best.h - 1); ensureVisible(); }
  }

  function activate() {
    const item = S.items[S.fi];
    if (!item) return;
    if (item.id === 'settings') { window.APP.openSettings(); return; }
    if (runningOf(item.id)) window.RUN.restore(item);
    else window.RUN.launch(item);
  }

  function quickAct(k) {
    const item = S.items[S.fi];
    if (!item) return;
    if (runningOf(item.id)) {
      if (k === 'x') window.RUN.openEnd(item.id);
      return;
    }
    if (!item.live) return;
    const L = window.ST.LIVE;
    if (k === 'x') L.playing = !L.playing;
    if (k === 'y') { L.song = (L.song + 1) % L.songs.length; L.line = 0; L.playing = true; }
    render();
  }

  function homeKey() {
    const last = S.running[S.running.length - 1];
    if (!last) { window.ST.toast('没有后台程序'); return; }
    const idx = S.items.findIndex(x => x.id === last.id);
    if (idx < 0) return;
    S.fi = idx;
    S.panX = initialPan(S.layout.placed[idx]);
    render();
  }

  function tileScreenRect(item) {
    const idx = S.items.indexOf(item);
    const pos = S.layout.placed[idx];
    if (!pos) return { x: 0, y: 0, w: 164, h: 164 };
    return { x: pos.px - S.panX, y: pos.py, w: pos.pw, h: pos.ph };
  }

  function refreshLive() { paintLiveForAll(); }
  function paintLiveForAll() {
    for (const [id, el] of tileEls) {
      const item = itemById(id);
      if (item && item.live) TILES.paintLive(el, item);
    }
  }

  window.HOME = { render, move, setTab, activate, quickAct, homeKey, ensureVisible, tileScreenRect, refreshLive, syncWall, tileEls };
})();
