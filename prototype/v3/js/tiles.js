/* brickdesk v3 — tile markup, cover text, live tile painting */
(function () {
  const { S, esc, match, runningOf } = window.ST;

  function spanName(w, h) { return w + 'x' + h; }

  function badgeHTML(item, index, run) {
    const num = String(index + 1).padStart(2, '0');
    if (run) return '<i class="dot"></i>运行中 · ' + num;
    if (item.live) return '<i class="dot"></i>' + (window.ST.LIVE.playing ? '正在播放' : '已暂停') + ' · ' + num;
    return num;
  }

  function liveHTML(item) {
    const song = window.ST.LIVE.songs[window.ST.LIVE.song];
    return '<div class="live-body">' +
      '<div class="live-top"><div class="live-title">' + esc(song.title) + '</div><div class="live-artist">' + esc(song.artist) + '</div></div>' +
      '<div class="live-lyrics"><div class="ly-prev"></div><div class="ly-now"></div><div class="ly-next"></div></div>' +
      '<div class="live-bottom"><div class="live-bar"><i></i></div><div class="live-row">' +
      '<span class="times"><span class="tnow">00:00</span><span class="tall">03:48</span></span>' +
      '<span class="live-play"></span></div></div></div>';
  }

  function tileHTML(item, pos, index) {
    const run = runningOf(item.id);
    const cls = ['tile'];
    if (item.live) cls.push('live');
    if (run) cls.push('running');
    if (pos.y === 0) cls.push('row0');
    const src = run ? BD.frameFor(item) : item.img;
    const art = item.material
      ? '<div class="mat" style="--mat:var(--mat-' + (item.mat || 'terminal') + ')"><span class="glyph">' + esc(item.glyph || '') + '</span></div>'
      : '<div class="art"><img src="' + src + '" alt="" style="object-position:' + (item.pos || '50% 50%') + '" decoding="async"></div>';
    const sub = item.sub ? '<div class="tsub">' + esc(item.sub) + '</div>' : '';
    return '<button class="' + cls.join(' ') + '" data-id="' + item.id + '" data-i="' + index + '" data-span="' + spanName(pos.w, pos.h) +
      '" style="left:' + pos.px + 'px;top:' + pos.py + 'px;width:' + pos.pw + 'px;height:' + pos.ph + 'px" aria-label="' + esc(item.name) + '">' +
      art + '<div class="shade"></div>' +
      '<div class="badge">' + badgeHTML(item, index, run) + '</div>' +
      '<div class="copy"><div class="tcopy">' + esc(item.name) + '</div>' + sub + '</div>' +
      (item.live ? liveHTML(item) : '') + '</button>';
  }

  function paintLive(el, item) {
    if (!item.live) return;
    const L = window.ST.LIVE;
    const song = L.songs[L.song];
    const n = song.lines.length;
    const idx = L.line % n;
    const set = (sel, text) => { const n2 = el.querySelector(sel); if (n2) n2.textContent = text; };
    set('.live-title', song.title);
    set('.live-artist', song.artist);
    set('.ly-prev', song.lines[(idx - 1 + n) % n]);
    set('.ly-now', L.playing ? song.lines[idx] : '已暂停');
    set('.ly-next', song.lines[(idx + 1) % n]);
    const bar = el.querySelector('.live-bar i');
    if (bar) bar.style.width = song.progress + '%';
    set('.tnow', window.ST.mmss(Math.round(song.duration * song.progress / 100)));
    set('.tall', window.ST.mmss(song.duration));
    const play = el.querySelector('.live-play');
    if (play) play.innerHTML = ico(L.playing ? 'pause' : 'play', 14, 2);
  }

  function paintTile(el, item, pos, index) {
    const run = runningOf(item.id);
    el.classList.toggle('f', index === S.fi && S.screen === 'home' && !S.dialog);
    el.classList.toggle('off', !match(item));
    el.classList.toggle('running', !!run);
    el.classList.toggle('row0', pos.y === 0);
    el.classList.toggle('closing', S.closingId === item.id);
    const badge = el.querySelector('.badge');
    if (badge) badge.innerHTML = badgeHTML(item, index, run);
    const img = el.querySelector('.art img');
    if (img) {
      const src = run ? BD.frameFor(item) : item.img;
      if (img.getAttribute('src') !== src) img.setAttribute('src', src);
    }
    paintLive(el, item);
  }

  window.TILES = { tileHTML, paintTile, paintLive };
})();
