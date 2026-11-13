/* brickdesk v3 — end-process / capacity dialogs */
(function () {
  const { S, $, esc, itemById, runningOf, estMem, toast, MAX_RUNNING, MEM_SOFT } = window.ST;
  const frame = item => BD.frameFor(item) || 'img/app-town.jpg';
  let timer = null;

  function move(dir) {
    if (!S.dialog) return;
    S.dialog.focus = dir === 'left' ? 0 : dir === 'right' ? 1 : (S.dialog.focus === 0 ? 1 : 0);
    window.APP.render();
  }

  function cancel() {
    if (!S.dialog) return;
    S.dialog = null;
    window.APP.render();
  }

  function confirm() {
    const d = S.dialog;
    if (!d) return;
    if (d.focus === 0) { S.dialog = null; window.APP.render(); return; }
    if (d.kind === 'end') {
      const item = itemById(d.id);
      S.dialog = null;
      S.closingId = d.id;
      window.APP.render();
      clearTimeout(timer);
      timer = setTimeout(() => {
        const i = S.running.findIndex(x => x.id === d.id);
        if (i >= 0) S.running.splice(i, 1);
        S.closingId = null;
        window.APP.render();
        toast('已结束 ' + (item ? item.name : '程序'));
      }, 220);
      return;
    }
    const victim = itemById(d.victimId);
    const pending = itemById(d.pendingId);
    S.dialog = null;
    if (victim) {
      const i = S.running.findIndex(x => x.id === victim.id);
      if (i >= 0) S.running.splice(i, 1);
    }
    window.APP.render();
    if (pending) setTimeout(() => window.RUN.launch(pending), 40);
  }

  function render() {
    const el = $('dialog');
    const d = S.dialog;
    if (!d) { el.classList.remove('on'); el.innerHTML = ''; return; }
    let html = '';
    if (d.kind === 'end') {
      const item = itemById(d.id);
      const run = runningOf(d.id);
      if (!item) { el.classList.remove('on'); return; }
      html = '<div class="dialog-card"><div class="dialog-head"><img class="dialog-thumb" src="' + frame(item) + '" alt="">' +
        '<h3>结束 ' + esc(item.name) + '？</h3></div>' +
        '<div class="dialog-body">未保存的进度会丢失，' + esc(item.name) + '会被移出后台列表。</div>' +
        '<div class="dialog-meta">已暂停 · ' + (run ? run.mem : estMem(item)) + ' MB · 返回桌面后可再次打开</div>' +
        '<div class="dialog-actions"><span class="dialog-btn' + (d.focus === 0 ? ' f' : '') + '" data-dbtn="0">取消</span>' +
        '<span class="dialog-btn primary' + (d.focus === 1 ? ' f' : '') + '" data-dbtn="1">结束进程</span></div></div>';
    } else {
      const victim = itemById(d.victimId);
      const pending = itemById(d.pendingId);
      html = '<div class="dialog-card"><div class="dialog-head"><span class="dialog-thumb" style="display:grid;place-items:center;color:var(--ink-2)">' + ico('info', 24, 1.75) + '</span>' +
        '<h3>' + d.reason + '</h3></div>' +
        '<div class="dialog-body">最多保留 ' + MAX_RUNNING + ' 个后台程序，后台内存上限 ' + MEM_SOFT + ' MB。<br>结束最久未用的「' + esc(victim ? victim.name : '') + '」并启动「' + esc(pending ? pending.name : '') + '」？</div>' +
        '<div class="dialog-meta">也可按 B 取消，先手动结束一个程序。</div>' +
        '<div class="dialog-actions"><span class="dialog-btn' + (d.focus === 0 ? ' f' : '') + '" data-dbtn="0">取消</span>' +
        '<span class="dialog-btn primary' + (d.focus === 1 ? ' f' : '') + '" data-dbtn="1">结束并启动</span></div></div>';
    }
    el.innerHTML = html;
    el.classList.add('on');
  }

  window.DIALOG = { move, cancel, confirm, render };
})();
