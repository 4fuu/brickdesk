/* brickdesk v3 — deterministic dense layout on a fixed-row, right-growing wall.
   Cells are 188×188 with a 5px gutter. Four rows. Placement is a global
   first-fit, column-major: every new app scans from the wall's start for the
   first free rectangle, so later small tiles backfill earlier holes. Appending
   an app never moves an existing tile; inserting or deleting recomputes from
   that index forward. Same list, same layout. */
(function () {
  const ROWS = 4;
  const U = 188;
  const GAP = 5;
  const PITCH = U + GAP;
  const BACKFILL = 0;
  const VIEW_W = 1024;
  const VIEW_H = 768;

  function spanOf(item) {
    const s = item.span || [1, 1];
    const w = Math.max(1, Math.min(3, s[0] | 0));
    const h = Math.max(1, Math.min(ROWS, s[1] | 0));
    return [w, h];
  }

  function computeLayout(items) {
    const cols = [];
    const placed = [];
    let maxCol = -1;

    for (let i = 0; i < items.length; i++) {
      const [w, h] = spanOf(items[i]);
      const need = ((1 << h) - 1);
      let spot = null;
      const start = 0;

      for (let c = start; c <= maxCol && !spot; c++) {
        for (let r = 0; r <= ROWS - h; r++) {
          const mask = need << r;
          let ok = true;
          for (let x = c; x < c + w; x++) {
            if (((cols[x] || 0) & mask) !== 0) { ok = false; break; }
          }
          if (ok) { spot = { c, r }; break; }
        }
      }
      if (!spot) spot = { c: maxCol + 1, r: 0 };

      while (cols.length < spot.c + w) cols.push(0);
      const mask = need << spot.r;
      for (let x = spot.c; x < spot.c + w; x++) cols[x] |= mask;
      maxCol = Math.max(maxCol, spot.c + w - 1);

      const pw = w * U + (w - 1) * GAP;
      const ph = h * U + (h - 1) * GAP;
      placed.push({
        i, x: spot.c, y: spot.r, w, h,
        px: spot.c * PITCH, py: spot.r * PITCH,
        pw, ph,
        right: spot.c * PITCH + pw,
        bottom: spot.r * PITCH + ph
      });
    }

    let free = 0;
    for (const m of cols) free += ROWS - ((m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1));
    return {
      placed,
      cols,
      wallW: cols.length ? cols.length * PITCH - GAP : 0,
      wallH: ROWS * U + (ROWS - 1) * GAP,
      free
    };
  }

  window.BL = { ROWS, U, GAP, PITCH, BACKFILL, VIEW_W, VIEW_H, spanOf, computeLayout };
})();
