/* brickdesk v3 — screen orchestration and boot */
(function () {
  const { S, $, stage, clamp, toast } = window.ST;

  function render() {
    HOME.render();
    SETTINGS.render();
    RUN.renderApp();
    PANELS.renderSide();
    PANELS.renderQuick();
    RUN.renderLaunch();
    DIALOG.render();
    CH.renderHint();
    $('scrim').classList.toggle('on', S.side || S.quick);
    stage.classList.toggle('app-open', S.screen === 'app');
    window.ST.updateDebug();
  }

  function goHome() {
    S.screen = 'home';
    S.side = false;
    S.quick = false;
    S.dialog = null;
    S.hint = false;
    render();
  }

  function openSettings() {
    S.screen = 'settings';
    S.szone = 'cats';
    S.cat = 0;
    S.oi = 0;
    S.side = false;
    S.quick = false;
    S.hint = false;
    render();
  }

  function openSide() {
    if (S.app) return;
    S.quick = false;
    S.side = !S.side;
    S.sub = false;
    if (S.side) {
      const items = PANELS.sideItems();
      const want = S.screen === 'settings' ? '设置' : '主页';
      S.si = Math.max(0, items.findIndex(x => x.label === want));
    }
    render();
  }

  function openQuick() {
    if (S.app) return;
    if (S.side) { S.side = false; S.quick = true; S.qi = 0; render(); return; }
    S.quick = !S.quick;
    if (S.quick) S.qi = 0;
    render();
  }


  function sideConfirm() {
    const items = PANELS.sideItems();
    const x = items[S.si];
    if (!x) return;
    if (x.sub) { S.sub = true; S.pi = 0; render(); return; }
    S.side = false;
    if (x.label === '主页') goHome();
    else if (x.label === '设置') openSettings();
    else { render(); toast('正在打开 ' + x.label); }
  }

  function powerConfirm() {
    const p = BD.POWER[S.pi];
    S.side = false;
    S.sub = false;
    render();
    toast('正在' + p.label + '（演示）');
  }

  function quickCount() { return S.running.length + PANELS.quickOpts().length; }

  function adjustQuick(i, d) {
    const o = PANELS.quickOpts()[i];
    if (!o) return;
    if (o.type === 'slider') S.vals[o.key] = clamp((S.vals[o.key] || 0) + d * 5, 0, 100);
    else if (o.type === 'toggle') S.vals[o.key] = !S.vals[o.key];
    else if (o.key === 'theme') window.ST.setTheme(S.theme === 'light' ? 'dark' : 'light');
    else if (o.type === 'select') {
      const len = o.values.length;
      S.vals[o.key] = ((S.vals[o.key] || 0) + d + len) % len;
    }
  }

  function moveQuick(dir) {
    const n = quickCount();
    if (dir === 'up') S.qi = (S.qi - 1 + n) % n;
    if (dir === 'down') S.qi = (S.qi + 1) % n;
    render();
  }

  function enterQuick() {
    if (S.qi < S.running.length) {
      const item = window.ST.itemById(S.running[S.qi].id);
      if (item) RUN.restore(item);
      return;
    }
    adjustQuick(S.qi - S.running.length, 1);
    render();
  }

  function cycleTab(d) { HOME.setTab(d); render(); }

  function menu() {
    if (S.dialog || S.launch || S.app) return;
    openSide();
  }

  function start() {
    if (S.dialog || S.launch || S.app) return;
    openQuick();
  }

  window.APP = { render, goHome, openSettings, openSide, openQuick, menu, start, sideConfirm, powerConfirm,
    moveQuick, enterQuick, adjustQuick, quickCount, cycleTab };

  function boot() {
    HARNESS.init();
    CH.buildHomeBar();
    QUERY.apply();
    window.ST.updateHarness();
    render();
    window.ST.fit();
    INPUT.init();
    setInterval(HARNESS.tick, 3000);
  }

  boot();
})();
