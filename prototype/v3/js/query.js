/* brickdesk v3 — URL state for reproducible screenshots and deep links */
(function () {
  const { S, q, CAPTURE, INSTANT, stage, clamp, rebuild, applyFocus, itemById, seedRunning, initValues,
    setTheme, setAccent, setBg, setAnim, initialPan } = window.ST;

  function apply() {
    /* body */
    if (CAPTURE) document.body.classList.add('capture');
    if (INSTANT) stage.classList.add('instant');
    const c = Number(q.get('count'));
    if (c === 8 || c === 60 || c === 500) S.count = c;
    if (q.get('theme') === 'light') S.theme = 'light';
    if (q.get('accent') && BD.ACCENTS[q.get('accent')]) S.accent = q.get('accent');
    if (q.get('bg') === 'solid') S.bg = 'solid';
    if (q.get('time')) S.clockText = q.get('time');
    if (q.get('tab')) S.tab = clamp(Number(q.get('tab')) || 0, 0, 3);

    initValues();
    rebuild(S.count, 'music');
    if (q.get('focus')) applyFocus(q.get('focus'));

    if (q.get('running')) {
      for (const id of q.get('running').split(',')) {
        const item = itemById(id);
        if (item) seedRunning(item, 'background');
      }
    }
    if (q.get('app')) {
      const item = itemById(q.get('app'));
      if (item) {
        seedRunning(item, 'foreground');
        S.app = { id: item.id };
        S.screen = 'app';
        applyFocus(item.id);
      }
    }
    if (q.get('launch')) {
      const item = itemById(q.get('launch'));
      if (item) {
        seedRunning(item, 'foreground');
        applyFocus(item.id);
        S.launch = { id: item.id, mode: 'open', freeze: Math.max(0, Math.min(1, Number(q.get('freeze') || .46))) };
        S.fi = S.items.indexOf(item);
        S.panX = initialPan(S.layout.placed[S.fi]);
      }
    }

    if (q.get('screen') === 'settings') {
      S.screen = 'settings';
      S.cat = clamp(Number(q.get('cat')) || 0, 0, BD.CATS.length - 1);
      S.szone = q.get('zone') === 'opts' ? 'opts' : 'cats';
      S.oi = clamp(Number(q.get('oi')) || 0, 0, BD.CATS[S.cat].rows.length - 1);
    }
    if (q.get('quick') === '1') { S.quick = true; S.qi = clamp(Number(q.get('qi')) || 0, 0, 4); }
    if (q.get('side') === '1') { S.side = true; S.si = clamp(Number(q.get('si')) || 0, 0, BD.SIDE.length - 1); }
    if (q.get('hint') === '1') S.hint = true;
    if (q.get('dialog') === 'end' && q.get('id')) {
      const item = itemById(q.get('id'));
      if (item) { seedRunning(item, 'background'); S.dialog = { kind: 'end', id: item.id, focus: 0 }; }
    }
    setTheme(S.theme);
    setAccent(S.accent);
    setBg(S.bg);
    setAnim(S.anim);
  }

  window.QUERY = { apply };
})();
