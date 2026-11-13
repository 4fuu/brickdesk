/* brickdesk v3 — settings page: categories, sheet rows, accent picker */
(function () {
  const { S, $, clamp, esc } = window.ST;
  const ACC = () => BD.ACCENTS;

  function arrows(value) {
    return '<span class="ch">' + ico('left', 14, 2) + '</span><span class="value">' + value + '</span><span class="ch">' + ico('right', 14, 2) + '</span>';
  }

  function accentValue() {
    const cur = ACC()[S.accent];
    return '<span class="accent-val">' +
      Object.keys(ACC()).map(k => '<span class="sw' + (k === S.accent ? ' sel' : '') + '" style="--sw:' + ACC()[k].base + '" title="' + ACC()[k].label + '"></span>').join('') +
      '<span class="an">' + cur.label + '</span></span>';
  }

  function sliderValue(v) {
    return '<span class="slider"><span class="rail"><i style="width:' + v + '%"></i><em style="left:' + v + '%"></em></span><span class="num">' + v + '</span></span>';
  }

  function valueHTML(row) {
    if (row.type === 'accent') return '<span class="val">' + accentValue() + '</span>';
    if (row.type === 'slider') return '<span class="val">' + sliderValue(S.vals[row.key] || 0) + '</span>';
    if (row.type === 'toggle') {
      const on = !!S.vals[row.key];
      return '<span class="val"><span class="value">' + (on ? '开' : '关') + '</span><span class="switch ' + (on ? 'on' : '') + '"><i></i></span></span>';
    }
    if (row.type === 'info') return '<span class="val"><span class="info">' + esc(row.value || '') + '</span></span>';
    if (row.key === 'theme') return '<span class="val">' + arrows(S.theme === 'light' ? '浅色' : '深色') + '</span>';
    if (row.key === 'bg') return '<span class="val">' + arrows(S.bg === 'solid' ? '纯色' : '预烘焙') + '</span>';
    return '<span class="val">' + arrows(row.values ? row.values[S.vals[row.key] || 0] : '') + '</span>';
  }

  function render() {
    const el = $('settings');
    el.hidden = S.screen !== 'settings';
    if (el.hidden) return;
    const cat = BD.CATS[S.cat];
    const cats = BD.CATS.map((c, i) => {
      const cur = i === S.cat ? ' cur' : '';
      const f = S.szone === 'cats' && i === S.cat ? ' f' : '';
      return '<div class="set-cat' + cur + f + '" data-cat="' + i + '">' + ico(c.icon, 19, 1.75) + '<span>' + c.name + '</span></div>';
    }).join('');
    const rows = cat.rows.map((r, i) => {
      const f = S.szone === 'opts' && i === S.oi ? ' f' : '';
      return '<div class="set-row' + (r.desc ? ' tall' : '') + f + '" data-row="' + i + '">' +
        '<div class="set-label"><div class="l">' + r.label + '</div>' + (r.desc ? '<div class="d">' + r.desc + '</div>' : '') + '</div>' +
        valueHTML(r) + '</div>';
    }).join('');
    el.innerHTML = window.CH.pageBarHTML() +
      '<div class="settings-wrap"><div class="set-side"><div class="set-cats">' + cats + '</div>' +
      '<div class="set-side-note"><b>设置会即时保存</b><br>按 B 返回主页</div></div>' +
      '<div class="set-main"><div class="set-heading"><h2>' + cat.name + '</h2><p>' + cat.note + '</p></div>' +
      '<div class="set-sheet">' + rows + '</div>' +
      '<div class="settings-foot"><span class="dot"></span><span>强调色会作用到顶栏、进度、开关与焦点行。</span></div></div></div>';
  }

  function adjust(row, d) {
    if (!row) return;
    if (row.key === 'theme') { window.ST.setTheme(S.theme === 'light' ? 'dark' : 'light'); render(); return; }
    if (row.key === 'bg') { window.ST.setBg(S.bg === 'solid' ? 'prebaked' : 'solid'); render(); return; }
    if (row.key === 'accent') {
      const keys = Object.keys(ACC());
      const at = keys.indexOf(S.accent);
      const next = keys[(at + (d >= 0 ? 1 : keys.length - 1)) % keys.length];
      window.ST.setAccent(next);
      render();
      return;
    }
    if (row.type === 'slider') { S.vals[row.key] = clamp((S.vals[row.key] || 0) + d * 5, 0, 100); render(); return; }
    if (row.type === 'toggle') { S.vals[row.key] = !S.vals[row.key]; render(); return; }
    if (row.type === 'select') {
      const len = row.values.length;
      S.vals[row.key] = ((S.vals[row.key] || 0) + d + len) % len;
      render();
    }
  }

  function move(d) {
    if (S.szone === 'cats') {
      if (d === 'up') S.cat = clamp(S.cat - 1, 0, BD.CATS.length - 1);
      if (d === 'down') S.cat = clamp(S.cat + 1, 0, BD.CATS.length - 1);
      if (d === 'right') { S.szone = 'opts'; S.oi = 0; }
      S.oi = 0;
      render();
      return;
    }
    const rows = BD.CATS[S.cat].rows;
    if (d === 'up') S.oi = clamp(S.oi - 1, 0, rows.length - 1);
    if (d === 'down') S.oi = clamp(S.oi + 1, 0, rows.length - 1);
    if (d === 'left') adjust(rows[S.oi], -1);
    if (d === 'right') adjust(rows[S.oi], 1);
    render();
  }

  function enter() {
    if (S.szone === 'cats') { S.szone = 'opts'; S.oi = 0; render(); return; }
    adjust(BD.CATS[S.cat].rows[S.oi], 1);
  }

  function back() {
    if (S.szone === 'opts') { S.szone = 'cats'; render(); }
    else window.APP.goHome();
  }

  function clickCat(i) { S.cat = clamp(i, 0, BD.CATS.length - 1); S.oi = 0; S.szone = 'cats'; render(); }

  window.SETTINGS = { render, move, adjust, enter, back, clickCat };
})();
