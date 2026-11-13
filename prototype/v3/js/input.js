/* brickdesk v3 — keyboard mapping, hold-Select, pointer selection */
(function () {
  const { S } = window.ST;
  let hintTimer = null;
  let menuTimer = null;
  const held = {};

  function showHintSoon() {
    clearTimeout(hintTimer);
    hintTimer = setTimeout(() => {
      S.hint = true;
      CH.renderHint();
    }, 400);
  }
  function hideHint() {
    clearTimeout(hintTimer);
    if (!S.hint) return;
    S.hint = false;
    CH.renderHint();
  }

  function keyName(e) {
    if (e.key === 'Tab') return 'select';
    if (e.key === 'Enter') return 'a';
    if (e.key === 'Escape') return 'b';
    if (e.key === ' ') return 'start';
    if (e.key === 'Home') return 'home';
    if (e.key === 'ArrowUp') return 'up';
    if (e.key === 'ArrowDown') return 'down';
    if (e.key === 'ArrowLeft') return 'left';
    if (e.key === 'ArrowRight') return 'right';
    if (e.key.length === 1) {
      const k = e.key.toLowerCase();
      if (k === 's') return 'start';
      if (k === 'm') return 'menu';
      if (k === 'c') return 'x';
      if (k === 'v') return 'y';
      if (k === 'q') return 'l1';
      if (k === 'e') return 'r1';
      if (k === 'z') return 'a';
      if (k === 'h') return 'home';
    }
    return null;
  }

  function keyAction(k) {
    if (S.dialog) {
      if (k === 'left' || k === 'right') DIALOG.move(k);
      else if (k === 'a') DIALOG.confirm();
      else if (k === 'b') DIALOG.cancel();
      return;
    }
    if (S.launch) return;
    if (S.screen === 'app') {
      if (k === 'home') RUN.returnHome();
      return;
    }
    if (S.quick) {
      if (k === 'up' || k === 'down') { APP.moveQuick(k); return; }
      if (k === 'left' || k === 'right') {
        if (S.qi >= S.running.length) APP.adjustQuick(S.qi - S.running.length, k === 'left' ? -1 : 1);
        APP.render();
        return;
      }
      if (k === 'a') { APP.enterQuick(); return; }
      if (k === 'x' && S.qi < S.running.length) { RUN.openEnd(S.running[S.qi].id); return; }
      if (k === 'b') { S.quick = false; APP.render(); return; }
      return;
    }
    if (S.side) {
      const items = PANELS.sideItems();
      if (S.sub) {
        if (k === 'up') S.pi = (S.pi - 1 + BD.POWER.length) % BD.POWER.length;
        else if (k === 'down') S.pi = (S.pi + 1) % BD.POWER.length;
        else if (k === 'a') { APP.powerConfirm(); return; }
        else if (k === 'b' || k === 'left') S.sub = false;
        else return;
        APP.render();
        return;
      }
      if (k === 'up') S.si = (S.si - 1 + items.length) % items.length;
      else if (k === 'down') S.si = (S.si + 1) % items.length;
      else if (k === 'a') { APP.sideConfirm(); return; }
      else if (k === 'b' || k === 'left') S.side = false;
      else return;
      APP.render();
      return;
    }
    if (S.screen === 'settings') {
      if (k === 'b') SETTINGS.back();
      else if (k === 'a') SETTINGS.enter();
      else if (k === 'home') APP.goHome();
      else if (k === 'l1' || k === 'r1') {
        S.cat = (S.cat + (k === 'r1' ? 1 : -1) + BD.CATS.length) % BD.CATS.length;
        S.szone = 'cats'; S.oi = 0; APP.render();
      } else SETTINGS.move(k);
      return;
    }
    if (k === 'up' || k === 'down' || k === 'left' || k === 'right') { HOME.move(k); APP.render(); return; }
    if (k === 'a') { HOME.activate(); return; }
    if (k === 'x' || k === 'y') { HOME.quickAct(k); return; }
    if (k === 'l1' || k === 'r1') { APP.cycleTab(k === 'r1' ? 1 : -1); return; }
    if (k === 'home') { HOME.homeKey(); return; }
  }

  function init() {
    addEventListener('keydown', e => {
      const k = keyName(e);
      if (!k) return;
      if (k === 'select') { e.preventDefault(); if (!e.repeat) showHintSoon(); return; }
      if (['a', 'b', 'left', 'right', 'up', 'down', 'start', 'menu', 'home', 'l1', 'r1', 'x', 'y'].includes(k)) e.preventDefault();
      if (held[k] && k !== 'up' && k !== 'down' && k !== 'left' && k !== 'right') return;
      held[k] = true;
      if (S.screen === 'app') {
        if (k === 'menu') {
          clearTimeout(menuTimer);
          menuTimer = setTimeout(() => RUN.returnHome(), 550);
          return;
        }
        if (k === 'home') RUN.returnHome();
        return;
      }
      if (k === 'menu' && !S.dialog) APP.menu();
      else if (k === 'start' && !S.dialog) APP.start();
      else keyAction(k);
    });

    addEventListener('keyup', e => {
      const k = keyName(e);
      if (!k) return;
      if (k === 'select') { hideHint(); return; }
      held[k] = false;
      if (k === 'menu') clearTimeout(menuTimer);
    });
    addEventListener('blur', () => {
      hideHint(); clearTimeout(menuTimer);
      for (const k of Object.keys(held)) held[k] = false;
    });

    document.addEventListener('click', e => {
      const tab = e.target.closest('#homebar .tab');
      if (tab) {
        S.tab = Number(tab.dataset.tab);
        const idx = S.layout.placed.findIndex(p => window.ST.match(S.items[p.i]));
        if (idx >= 0) { S.fi = idx; HOME.ensureVisible(); }
        APP.render(); return;
      }
      const cat = e.target.closest('#settings .set-cat');
      if (cat) { SETTINGS.clickCat(Number(cat.dataset.cat)); return; }
      if (e.target.closest('#backMark')) { SETTINGS.back(); return; }
      const dbtn = e.target.closest('[data-dbtn]');
      if (dbtn && S.dialog) { S.dialog.focus = Number(dbtn.dataset.dbtn); DIALOG.confirm(); return; }
      const runRow = e.target.closest('#quick [data-run]');
      if (runRow) { S.qi = Number(runRow.dataset.run); APP.render(); return; }
      const optRow = e.target.closest('#quick [data-opt]');
      if (optRow) { S.qi = S.running.length + Number(optRow.dataset.opt); APP.render(); return; }
      const sideRow = e.target.closest('#side [data-side]');
      if (sideRow) { S.si = Number(sideRow.dataset.side); APP.render(); return; }
      const power = e.target.closest('#side [data-power]');
      if (power) { S.pi = Number(power.dataset.power); APP.render(); return; }
    });
  }

  window.INPUT = { init, showHintSoon, hideHint };
})();
