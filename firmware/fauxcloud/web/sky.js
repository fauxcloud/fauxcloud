// sky.js -- no sky effect under a glass tile or an open sheet. sky.css masks its effect layer (.sky::after: stars, rain,
// snow) and the moon (.sky::before) with --holes, one SVG the size of the page: opaque everywhere except every .glass and
// .sheet element's rounded rectangle (a hidden sheet has an empty box), grown by 6 px because the glass blur reaches a little
// past its edge. The holes sit in page coordinates and a page scroll only writes --sy (scrollY in px), by which sky.css shifts
// every mask up: no new mask image while the page scrolls, because WebKit loads a data: image asynchronously and paints a
// masked layer only once its mask has loaded (a new image per scrolled frame blinked the moon and the stars). The holes are
// rebuilt, once per frame at most, when a tile can move within the page: a re-render, the page's size, a resize, a sheet
// opened, closed, slid in or scrolled inside (its own tiles move), the sky's state; a sheet is fixed to the window, so a
// page scroll under an open one rebuilds too. Only while an effect shows (night, or rain / snow). Lazy: core.js setSky()
// loads it with sky.css the first time the sky is not day.
// The holes are a UNION (an in-SVG <mask>: white field, holes filled black), so two grown holes that overlap
// (a 12-px grid gap, an 8-px list gap, a nested .glass) never re-fill their overlap; boxes in whole px, and an unchanged
// mask is not written again (a poll's re-render or a URL-bar resize costs no new image); the mask's size in px: the
// window's width by the page's height, never less than the window's (a phone's URL bar makes 100vh taller than the window).
const SKYH = (() => {
  const E = 6;
  // the holes' path: one rounded rectangle per box [x, y, w, h, radius] in page px, all the same winding (nonzero: overlaps
  // stay holes); an empty box is left out, a radius never exceeds half the box
  function path(boxes) {
    let d = '';
    for (const b of boxes) {
      const [bx, by, bw, bh, br] = b.map(Math.round);
      if (bw <= 0 || bh <= 0) continue;
      const x = bx - E, y = by - E, w = bw + 2 * E, h = bh + 2 * E, r = Math.min(br + E, w / 2, h / 2);
      d += `M${x + r} ${y}h${w - 2 * r}a${r} ${r} 0 0 1 ${r} ${r}v${h - 2 * r}a${r} ${r} 0 0 1 ${-r} ${r}h${2 * r - w}a${r} ${r} 0 0 1 ${-r} ${-r}v${2 * r - h}a${r} ${r} 0 0 1 ${r} ${-r}z`;
    }
    return d;
  }
  const svg = (d, W, V) => `url("data:image/svg+xml,${encodeURIComponent(`<svg xmlns='http://www.w3.org/2000/svg' width='${W}' height='${V}'><mask id='m'><rect width='${W}' height='${V}' fill='#fff'/><path d='${d}'/></mask><rect width='${W}' height='${V}' mask='url(#m)'/></svg>`)}")`;
  const sky = document.querySelector('.sky'), de = document.documentElement, dd = de.dataset, st = sky && sky.style;
  // full: the holes are rebuilt at the next frame; fix: an open sheet (fixed to the window) was among them
  let raf = 0, last = '', sy = '', full = 0, fix = 0;
  function frame() {
    raf = 0;
    const y = scrollY;
    if (full) {
      full = fix = 0;
      const W = innerWidth, V = Math.max(de.scrollHeight, innerHeight);
      const boxes = [...document.querySelectorAll('.glass, .sheet')].map(t => { const b = t.getBoundingClientRect(), c = getComputedStyle(t);
        if (b.height && c.position === 'fixed') fix = 1;
        return [b.left, b.top + y, b.width, b.height, parseFloat(c.borderTopLeftRadius) || 0]; });
      const m = svg(path(boxes), W, V);
      if (m !== last) { st.setProperty('--holes', last = m); st.setProperty('--hs', `${W}px ${V}px`); }
    }
    if (`${y}px` !== sy) st.setProperty('--sy', sy = `${y}px`);
  }
  function go() {
    if (!sky) return;
    if (dd.sky !== 'night' && !dd.rain && !dd.snow) {  // no effect shows: nothing to cut
      cancelAnimationFrame(raf); raf = 0;
      if (last) st.removeProperty('--holes'), st.removeProperty('--sy'), last = sy = '';
      return;
    }
    raf = raf || requestAnimationFrame(frame);
  }
  const update = () => { full = 1; go(); };
  if (sky) {
    // a page scroll moves the mask only; a scroll inside a sheet, or a page scroll under an open one, moves holes
    addEventListener('scroll', e => { if (e.target !== document || fix) full = 1; go(); }, { passive: true, capture: true });
    // a resize, and again after the dashboard's own debounced relayout (dash.js: the last row's tile stretched by an inline
    // grid-column, which no observer here sees)
    addEventListener('resize', () => { update(); setTimeout(update, 250); });
    addEventListener('animationend', update, true);
    // a re-render (the dashboard's polls, a sheet's list), the sky's state and the airport view (data-v), a sheet opened or
    // closed (hidden), a page that grows
    new MutationObserver(update).observe(de, { childList: true, subtree: true, attributes: true, attributeFilter: ['data-sky', 'data-rain', 'data-snow', 'data-v', 'hidden'] });
    new ResizeObserver(update).observe(document.body);
    update();
  }
  return { path, svg, update };
})();
