/* brickdesk v3 — top bar, status, hold-to-show hint contents */
(function () {
  const { S, $, clock, runningOf } = window.ST;

  function statusHTML() {
    return '<div class="status">' + ico('wifi', 17, 1.75) +
      '<span class="battery"><i></i></span><span class="clock">' + clock() + '</span></div>';
  }

  function runChipHTML() {
    const n = S.running.length;
    return n ? '<span class="run-chip"><i></i>' + n + ' 运行中</span>' : '';
  }

  function homeBarHTML() {
    const tabs = BD.TABS.map((t, i) => '<span class="tab' + (i === S.tab ? ' on' : '') + '" data-tab="' + i + '">' + t + '</span>').join('');
    return '<header class="topbar">' +
      '<div class="brand"><span class="brand-mark"><i></i><i></i><i></i><i></i></span>brickdesk</div>' +
      '<nav class="tabs">' + tabs + '</nav>' + runChipHTML() + statusHTML() + '</header>';
  }

  function pageBarHTML() {
    const cat = BD.CATS[S.cat] || BD.CATS[0];
    return '<header class="topbar page">' +
      '<span class="back-mark" id="backMark">' + ico('left', 22, 2) + '</span>' +
      '<span class="page-title">设置</span>' +
      '<span class="page-meta"><b>' + cat.name + '</b></span>' +
      statusHTML() + '</header>';
  }

  function buildHomeBar() {
    $('homebar').innerHTML = homeBarHTML();
  }

  function updateHomeBar() {
    const bar = $('homebar');
    if (!bar) return;
    bar.querySelectorAll('.tab').forEach((el, i) => el.classList.toggle('on', i === S.tab));
    const c = bar.querySelector('.clock');
    if (c) c.textContent = clock();
    const old = bar.querySelector('.run-chip');
    if (S.running.length) {
      if (old) old.innerHTML = '<i></i>' + S.running.length + ' 运行中';
      else {
        const chip = document.createElement('span');
        chip.className = 'run-chip';
        chip.innerHTML = '<i></i>' + S.running.length + ' 运行中';
        bar.querySelector('.status').before(chip);
      }
    } else if (old) old.remove();
  }

  function kbd(t) { return '<span class="kbd">' + t + '</span>'; }
  function item(keys, label) { return '<div class="hint-item">' + keys.map(kbd).join('') + '<span>' + label + '</span></div>'; }

  function hintHTML() {
    let left = [];
    let right = [];
    const running = S.running.length;
    if (S.dialog) {
      left = [item(['←', '→'], '选择')];
      right = [item(['A'], '确定'), item(['B'], '取消')];
    } else if (S.screen === 'app') {
      left = [item(['HOME'], '返回桌面')];
      right = [item(['MENU'], '长按返回'), item(['SELECT'], '松开收起')];
    } else if (S.quick) {
      if (S.qi < running) {
        left = [item(['↑', '↓'], '后台程序')];
        right = [item(['A'], '恢复'), item(['X'], '结束'), item(['B'], '关闭')];
      } else {
        left = [item(['↑', '↓'], '移动'), item(['←', '→'], '调整')];
        right = [item(['A'], '切换'), item(['B'], '关闭')];
      }
    } else if (S.side) {
      left = [item(['↑', '↓'], '移动')];
      right = S.sub ? [item(['A'], '确定'), item(['B'], '返回')] : [item(['A'], '打开'), item(['B'], '关闭')];
    } else if (S.screen === 'settings') {
      if (S.szone === 'cats') {
        left = [item(['↑', '↓'], '分类')];
        right = [item(['A'], '进入'), item(['B'], '主页')];
      } else {
        left = [item(['←', '→'], '调整')];
        right = [item(['A'], '切换'), item(['B'], '返回')];
      }
    } else {
      left = [item(['MENU'], '菜单'), item(['START'], '快捷设置')];
      right = [item(['L1', 'R1'], '分类')];
      const a = S.items[S.fi];
      const isRunning = a && runningOf(a.id);
      if (isRunning) {
        right.push(item(['A'], '恢复'), item(['X'], '结束'), item(['HOME'], '最近'));
      } else {
        if (a && a.live) right.push(item(['X'], window.ST.LIVE.playing ? '暂停' : '播放'), item(['Y'], '下一首'));
        right.push(item(['A'], '打开'));
      }
      if (running) left.push(item(['HOME'], '后台'));
    }
    return '<div class="hint-group">' + left.join('') + '</div>' +
      '<div class="hint-sep"></div>' +
      '<div class="hint-group">' + right.join('') + '</div>' +
      '<div class="hint-spacer"></div><div class="hint-note">' + (S.screen === 'app' ? '应用内叠加菜单' : '松开收起') + '</div>';
  }

  function renderHint() {
    const el = $('hint');
    if (!el) return;
    if (S.hint) {
      el.innerHTML = hintHTML();
      el.classList.add('on');
    } else {
      el.classList.remove('on');
    }
  }

  window.CH = { buildHomeBar, updateHomeBar, homeBarHTML, pageBarHTML, statusHTML, hintHTML, renderHint };
})();
