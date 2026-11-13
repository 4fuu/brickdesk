/* brickdesk v3 — background app lifecycle and the two launch transitions */
(function () {
  const { S, $, esc, itemById, runningOf, runningMem, seedRunning, MAX_RUNNING, MEM_SOFT, estMem, toast, clamp } = window.ST;
  const FULL = { x: 0, y: 0, w: 1024, h: 768 };
  let timer = null;

  const frameOf = item => BD.frameFor(item) || 'img/app-town.jpg';
  const rectStyle = (r, radius) => 'left:' + r.x + 'px;top:' + r.y + 'px;width:' + r.w + 'px;height:' + r.h + 'px;border-radius:' + radius + 'px';

  function touch(id) {
    const at = S.running.findIndex(x => x.id === id);
    if (at >= 0) S.running.push(S.running.splice(at, 1)[0]);
  }

  function cardHTML(item, rect, radius, restore, freezeP) {
    const frame = frameOf(item);
    const base = restore || S.launch.mode === 'close' ? frame : (item.img || frame);
    const overlay = !restore && S.launch.mode === 'open' && item.img && frame !== item.img
      ? '<img class="launch-frame" src="' + frame + '" alt="">'
      : '';
    const ring = '<div class="launch-ring" style="opacity:' + (freezeP == null ? 1 : Math.max(0, 1 - freezeP * 1.6)) + '"></div>';
    return '<div class="launch-scrim"></div><div class="launch-card" id="launchCard" style="' + rectStyle(rect, radius) + '">' +
      '<img class="launch-base" style="object-position:' + (item.pos || '50% 50%') + '" src="' + base + '" alt="">' + overlay + ring + '</div>';
  }

  function renderLaunch() {
    const el = $('launch');
    if (!S.launch) { el.hidden = true; el.innerHTML = ''; return; }
    const item = itemById(S.launch.id);
    if (!item) { el.hidden = true; return; }
    el.hidden = false;
    const freeze = S.launch.freeze;
    if (freeze != null) {
      const start = HOME.tileScreenRect(item);
      const r = {
        x: start.x + (FULL.x - start.x) * freeze,
        y: start.y + (FULL.y - start.y) * freeze,
        w: start.w + (FULL.w - start.w) * freeze,
        h: start.h + (FULL.h - start.h) * freeze
      };
      el.className = 'launch-layer freeze';
      el.innerHTML = cardHTML(item, r, Math.max(0, 5 * (1 - freeze)), false, freeze);
      return;
    }
    el.className = 'launch-layer';
    if (S.launch.mode === 'close') {
      const target = HOME.tileScreenRect(item);
      el.innerHTML = cardHTML(item, FULL, 0, true, null);
      requestAnimationFrame(() => {
        const card = $('launchCard');
        if (!card) return;
        card.style.left = target.x + 'px';
        card.style.top = target.y + 'px';
        card.style.width = target.w + 'px';
        card.style.height = target.h + 'px';
        card.style.borderRadius = '5px';
      });
      return;
    }
    const start = HOME.tileScreenRect(item);
    el.innerHTML = cardHTML(item, start, 5, !!S.launch.restore, null);
    requestAnimationFrame(() => {
      const card = $('launchCard');
      if (!card) return;
      card.classList.add('expanded');
      card.style.left = '0px';
      card.style.top = '0px';
      card.style.width = '1024px';
      card.style.height = '768px';
      card.style.borderRadius = '0px';
      if (card.querySelector('.launch-frame')) {
        setTimeout(() => card.classList.add('show-frame'), 250);
      }
    });
  }

  function focusForLaunch(item) {
    const idx = S.items.indexOf(item);
    if (idx < 0) return;
    S.fi = idx;
    const pos = S.layout.placed[idx];
    S.navRow = pos.y;
    if (pos.px < S.panX + 64 || pos.right > S.panX + 960) S.panX = window.ST.initialPan(pos);
  }

  function launch(item) {
    if (!item || S.launch || S.app) return;
    const run = runningOf(item.id);
    if (run) { restore(item); return; }
    if (S.running.length >= MAX_RUNNING || runningMem() + estMem(item) > MEM_SOFT) { openFull(item); return; }
    seedRunning(item, 'foreground');
    startOpen(item, false);
  }

  function restore(item) {
    if (!item || S.launch || S.app) return;
    const run = runningOf(item.id);
    if (!run) { launch(item); return; }
    run.state = 'foreground';
    touch(item.id);
    startOpen(item, true);
  }

  function startOpen(item, isRestore) {
    focusForLaunch(item);
    S.screen = 'home';
    S.app = { id: item.id };
    S.quick = false;
    S.side = false;
    S.dialog = null;
    S.hint = false;
    S.launch = { id: item.id, mode: 'open', restore: isRestore };
    window.APP.render();
    clearTimeout(timer);
    timer = setTimeout(() => {
      S.launch = null;
      S.screen = 'app';
      window.APP.render();
    }, 440);
  }

  function returnHome() {
    if (!S.app || S.launch) return;
    const item = itemById(S.app.id);
    if (!item) { S.app = null; S.screen = 'home'; window.APP.render(); return; }
    const run = runningOf(item.id);
    if (run) { run.state = 'background'; run.frame = frameOf(item); }
    const idx = S.items.indexOf(item);
    const pos = S.layout.placed[idx];
    S.screen = 'home';
    S.app = null;
    S.quick = false;
    S.side = false;
    S.dialog = null;
    S.hint = false;
    S.fi = idx;
    S.navRow = pos.y;
    S.panX = window.ST.initialPan(pos);
    S.launch = { id: item.id, mode: 'close' };
    window.APP.render();
    clearTimeout(timer);
    timer = setTimeout(() => {
      S.launch = null;
      window.APP.render();
      toast('已回到桌面，' + item.name + '在后台运行');
    }, 440);
  }

  function openEnd(id) {
    const item = itemById(id);
    if (!item || !runningOf(id)) return;
    S.dialog = { kind: 'end', id, focus: 0 };
    S.hint = false;
    window.APP.render();
  }

  function openFull(item) {
    const victim = S.running[0];
    if (!victim) return;
    const full = S.running.length >= MAX_RUNNING;
    S.dialog = { kind: 'full', pendingId: item.id, victimId: victim.id, focus: 0, reason: full ? '后台已满' : '内存不足' };
    S.hint = false;
    window.APP.render();
  }

  function renderApp() {
    const el = $('appview');
    if (S.screen !== 'app' || !S.app) { el.hidden = true; el.innerHTML = ''; return; }
    const item = itemById(S.app.id);
    if (!item) { el.hidden = true; return; }
    el.hidden = false;
    el.innerHTML = '<img class="frame" src="' + frameOf(item) + '" alt=""><div class="app-dim"></div>';
  }


  window.RUN = { launch, restore, returnHome, openEnd, openFull, renderLaunch, renderApp, frameOf, FULL };
})();
