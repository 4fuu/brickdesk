/* brickdesk v3 — external test harness and idle clock/live ticker */
(function () {
  const { S, $, CAPTURE, toast, setAccent, rebuild, updateHarness } = window.ST;

  function setCount(n) {
    S.launch = null;
    S.app = null;
    S.screen = 'home';
    S.running = [];
    S.dialog = null;
    S.closingId = null;
    S.quick = false;
    S.side = false;
    S.hint = false;
    rebuild(n, 'music');
    updateHarness();
    window.APP.render();
    toast('已切换为 ' + n + ' 个条目');
  }

  function init() {
    $('accents').innerHTML = Object.keys(BD.ACCENTS).map(k =>
      '<button class="swbtn" data-accent="' + k + '"><i style="--sw:' + BD.ACCENTS[k].base + '"></i>' + BD.ACCENTS[k].label + '</button>').join('');
    $('counts').innerHTML = [8, 60, 500].map(n => '<button class="hbtn" data-count="' + n + '">' + n + '</button>').join('');
    $('accents').addEventListener('click', e => {
      const b = e.target.closest('[data-accent]');
      if (!b) return;
      setAccent(b.dataset.accent);
      if (S.screen === 'settings') SETTINGS.render();
    });
    $('counts').addEventListener('click', e => {
      const b = e.target.closest('[data-count]');
      if (b) setCount(Number(b.dataset.count));
    });
    const sel = $('selectBtn');
    sel.addEventListener('pointerdown', e => { e.preventDefault(); INPUT.showHintSoon(); });
    ['pointerup', 'pointerleave', 'pointercancel'].forEach(n => sel.addEventListener(n, INPUT.hideHint));
  }

  function tick() {
    const c = document.querySelector('#homebar .clock');
    if (c) c.textContent = window.ST.clock();
    if (CAPTURE) return;
    if (S.screen === 'home' && !S.launch) {
      const L = window.ST.LIVE;
      const song = L.songs[L.song];
      L.line = (L.line + 1) % song.lines.length;
      HOME.refreshLive();
      if (S.hint) CH.renderHint();
    }
  }

  window.HARNESS = { init, tick, setCount };
})();
