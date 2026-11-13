/* brickdesk v3 — side menu and quick panel, including background apps */
(function () {
  const { S, $, clamp, esc, clock, runningMem, itemById } = window.ST;
  const QUICK_KEYS = ['bright', 'vol', 'wifi', 'bt', 'theme'];
  const quickOpts = () => QUICK_KEYS.map(k => BD.findRow(k));

  function foot(keys) {
    return '<div class="p-foot">' + keys.map(([k, label]) => '<div class="fg"><span class="kbd">' + k + '</span><span>' + label + '</span></div>').join('') + '</div>';
  }

  function sideItems() {
    return BD.SIDE.slice();
  }

  function renderSide() {
    const el = $('side');
    el.classList.toggle('on', S.side);
    if (!S.side) return;
    if (S.sub) {
      el.innerHTML = '<div class="p-head"><span class="p-title">电源</span><span class="p-meta">按 B 返回</span></div>' +
        '<div class="p-list">' + BD.POWER.map((p, i) => '<div class="mi ' + (i === S.pi ? 'f' : '') + '" data-power="' + i + '">' + ico(p.icon, 20, 1.75) + '<span>' + p.label + '</span></div>').join('') + '</div>' +
        '<div class="p-spacer"></div><div class="panel-note" style="padding:0 10px 8px">睡眠会保留当前进度。关机前会再确认。</div>' + foot([['A', '确定'], ['B', '返回']]);
      return;
    }
    const items = sideItems();
    S.si = clamp(S.si, 0, items.length - 1);
    const cur = S.screen === 'settings' ? '设置' : '主页';
    el.innerHTML = '<div class="p-head"><span class="p-title">菜单</span><span class="p-meta">' + clock() + '</span></div>' +
      '<div class="p-list">' + items.map((x, i) => {
        const f = i === S.si ? ' f' : '';
        const c = x.label === cur ? ' cur' : '';
        return '<div class="mi' + f + c + '" data-side="' + i + '">' + ico(x.icon, 20, 1.75) + '<span>' + x.label + '</span>' +
          (x.sub ? '<span class="r">' + ico('right', 16, 2) + '</span>' : '') + '</div>';
      }).join('') + '</div>' +
      '<div class="p-spacer"></div><div class="panel-note" style="padding:0 10px 8px">' + S.running.length + ' 个后台程序</div>' +
      foot([['A', '打开'], ['B', '关闭']]);
  }

  function sliderHTML(o) {
    const v = S.vals[o.key] || 0;
    return '<span class="val"><span class="slider"><span class="rail"><i style="width:' + v + '%"></i><em style="left:' + v + '%"></em></span><span class="num">' + v + '</span></span></span>';
  }
  function toggleHTML(o) {
    const on = !!S.vals[o.key];
    return '<span class="val"><span class="value">' + (on ? '开' : '关') + '</span><span class="switch ' + (on ? 'on' : '') + '"><i></i></span></span>';
  }
  function valueHTML(o) {
    if (o.type === 'slider') return sliderHTML(o);
    if (o.type === 'toggle') return toggleHTML(o);
    if (o.key === 'theme') return '<span class="val"><span class="value">' + (S.theme === 'light' ? '浅色' : '深色') + '</span></span>';
    return '<span class="val"><span class="value">' + (o.values ? o.values[S.vals[o.key] || 0] : '') + '</span></span>';
  }

  function runRows() {
    const mem = runningMem();
    const pct = Math.min(100, Math.round(mem / window.ST.MEM_SOFT * 100));
    let html = '<div class="run-sec"><div class="run-head"><span>正在运行</span><span>' + S.running.length + ' / ' + window.ST.MAX_RUNNING + '</span></div>' +
      '<div class="run-mem"><i style="width:' + pct + '%"></i></div>';
    if (!S.running.length) {
      html += '<div class="run-empty">没有后台程序。启动应用后会出现在这里。</div>';
    } else {
      html += S.running.map((r, i) => {
        const f = S.qi === i ? ' f' : '';
        return '<div class="mi run-row' + f + '" data-run="' + i + '">' +
          '<img class="run-thumb" src="' + r.frame + '" alt="">' +
          '<span class="run-name">' + esc(r.name) + '</span>' +
          '<span class="run-meta">已暂停 · ' + r.mem + ' MB</span></div>';
      }).join('');
    }
    html += '<div class="run-foot">后台内存 ' + mem + ' / 1024 MB</div></div>';
    return html;
  }

  function renderQuick() {
    const el = $('quick');
    el.classList.toggle('on', S.quick);
    if (!S.quick) return;
    const opts = quickOpts();
    const n = S.running.length;
    S.qi = clamp(S.qi, 0, n + opts.length - 1);
    const live = window.ST.liveItem();
    const song = window.ST.LIVE.songs[window.ST.LIVE.song];
    const rows = opts.map((o, i) => {
      const f = S.qi === n + i ? ' f' : '';
      return '<div class="mi' + f + '" data-opt="' + i + '">' + ico(o.key === 'theme' ? (S.theme === 'light' ? 'sun' : 'moon') : (o.icon || 'sliders'), 20, 1.75) +
        '<span>' + o.label + '</span>' + valueHTML(o) + '</div>';
    }).join('');
    el.innerHTML = '<div class="p-head"><span class="p-title">快捷设置</span><span class="p-meta">电量 <b>82%</b></span></div>' +
      '<div class="now"><div class="cover"><img src="' + (live ? live.img : '') + '" alt=""></div><div class="info">' +
      '<div class="kicker">正在播放</div><div class="nt">' + esc(song.title) + '</div><div class="na">' + esc(song.artist) + '</div>' +
      '<div class="np"><i style="width:' + song.progress + '%"></i></div></div><div class="play">' + ico(window.ST.LIVE.playing ? 'pause' : 'play', 14, 2) + '</div></div>' +
      runRows() + '<div class="p-list quick-list">' + rows + '</div>' +
      '<div class="p-spacer"></div>' + (n && S.qi < n
        ? foot([['A', '恢复'], ['X', '结束'], ['B', '关闭']])
        : foot([['A', '切换'], ['B', '关闭']]));
  }

  window.PANELS = { renderSide, renderQuick, sideItems, quickOpts, QUICK_KEYS };
})();
