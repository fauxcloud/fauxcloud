// test/tools/sky_vectors.js -- web/sky.js, the glass tiles cut out of the sky effects' mask. Run in a vm with a stub
// DOM: the holes' path (one rounded rectangle per box in page coordinates, grown by 6 px, whole px, the radius clamped, empty
// boxes left out), the SVG (a union: an in-SVG <mask>, never even-odd), the mask's px size (the page's), that an unchanged mask
// is not written again, that a page scroll writes only --sy (no new mask image) while a scroll inside a sheet or under an open
// one rebuilds, which resizes rebuild, and that the mask is built only while an effect shows. And sky.css: every --holes layer
// shifted up by --sy (the rain's and snow's keyframes too); the moon a px offset from the top, never a percentage of the fixed
// sky (its height follows the URL bar while scrolling).
// Usage: node test/tools/sky_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const WEB = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web');
const src = fs.readFileSync(path.join(WEB, 'sky.js'), 'utf8');

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
// one page: dataset = <html>'s data-*, tiles = [left, top, width, height, radius, class ('glass' when absent), position] with
// top in viewport px at the page's scroll (win.scrollY); the stub's querySelectorAll answers a selector list of classes, as the
// DOM does. rAF runs the frame at once.
function page(dataset, tiles, o = {}) {
  const props = {}, writes = [], ls = {}, mo = [], timers = [];
  const sky = { style: { setProperty: (k, v) => { props[k] = v; writes.push(k); }, removeProperty: k => { delete props[k]; writes.push('-' + k); } } };
  const win = { scrollY: o.scrollY || 0, innerWidth: 360, innerHeight: 780 }, de = { dataset, scrollHeight: o.pageH ?? 2400 };
  const els = tiles.map(([l, t, w, h, r, c, p]) => ({ box: [l, t, w, h], getBoundingClientRect() { const [left, top, width, height] = this.box; return { left, top, width, height }; },
    r, c: c || 'glass', p: p || 'static' }));
  const ctx = {
    document: { documentElement: de, body: {}, querySelector: s => (s === '.sky' ? sky : null),
      querySelectorAll: s => { const k = s.split(',').map(x => x.trim().replace(/^\./, '')); return els.filter(e => k.includes(e.c)); } },
    getComputedStyle: e => ({ borderTopLeftRadius: e.r + 'px', position: e.p }),
    requestAnimationFrame: f => { f(); return 0; }, cancelAnimationFrame() {}, addEventListener: (k, f) => { ls[k] = f; },
    setTimeout: (f, ms) => { timers.push([f, ms]); },
    MutationObserver: class { observe(n, opt) { mo.push(opt); } }, ResizeObserver: class { observe() {} }, encodeURIComponent, parseFloat, Math,
  };
  for (const k of ['scrollY', 'innerWidth', 'innerHeight']) Object.defineProperty(ctx, k, { get: () => win[k] });
  vm.createContext(ctx);
  vm.runInContext(src + '\n;this.SKYH = SKYH;', ctx);
  // the page scrolls by d px: every box moves up on screen, then the scroll event (on the document, or on a sheet's scroller)
  const scroll = (d, target = ctx.document) => { win.scrollY += d; els.forEach(e => { if (e.p !== 'fixed') e.box[1] -= d; }); ls.scroll({ target }); };
  // the timers due: run them (the stub's clock jumps)
  const tick = () => timers.splice(0).map(([f, ms]) => (f(), ms));
  return { H: ctx.SKYH, props, writes, els, ls, mo, win, de, scroll, tick, n: k => writes.filter(w => w === k).length };
}
const holes = p => decodeURIComponent(p.props['--holes'] || '');

const { H } = page({ sky: 'day' }, []);
const ONE = 'M34 14h52a30 30 0 0 1 30 30v2a30 30 0 0 1 -30 30h-52a30 30 0 0 1 -30 -30v-2a30 30 0 0 1 30 -30z';
check('no box: no hole', H.path([]), '');
check('one tile: grown by 6, radius 24 + 6', H.path([[10, 20, 100, 50, 24]]), ONE);
check('whole px', H.path([[10.4, 19.6, 99.7, 50.2, 24.3]]), ONE);
check('radius clamped to half the box', H.path([[0, 0, 20, 20, 99]]), 'M10 -6h0a16 16 0 0 1 16 16v0a16 16 0 0 1 -16 16h0a16 16 0 0 1 -16 -16v0a16 16 0 0 1 16 -16z');
check('zero size left out', H.path([[0, 10, 0, 50, 10], [0, 10, 50, 0, 10]]), '');
check('page coordinates: a box far below the window stays (the mask is the page\'s)', H.path([[0, 2200, 50, 50, 10]]).split('M').length, 2);
// two grown holes that overlap (a 12-px gap: 6 + 6) stay one union -- the same winding, filled in a <mask>
const two = H.path([[0, 0, 100, 50, 24], [0, 62, 100, 50, 24]]), m = decodeURIComponent(H.svg(two, 360, 2400));
check('overlap: two holes, one winding (every arc sweeps 1)', [two.split('M').length - 1, (two.match(/ 0 0 0 /g) || []).length], [2, 0]);
check('svg: a <mask> union, never even-odd', [m.includes("<mask id='m'><rect width='360' height='2400' fill='#fff'/><path d='"), m.includes("mask='url(#m)'"), m.includes('evenodd')], [true, true, false]);
const day = page({ sky: 'day', rain: '', snow: '' }, [[10, 20, 100, 50, 24]]);
check('day: no mask built, no shift', [day.props['--holes'], day.props['--sy']], [undefined, undefined]);
const night = page({ sky: 'night', rain: '', snow: '' }, [[10, 20, 100, 50, 24]]);
check('night: the mask holds the tile', holes(night).includes(ONE), true);
check('the mask size in px: the window\'s width by the page\'s height', night.props['--hs'], '360px 2400px');
check('a page shorter than the window: the window\'s height', page({ sky: 'night' }, [], { pageH: 500 }).props['--hs'], '360px 780px');
check('the shift starts at the page\'s scroll', night.props['--sy'], '0px');
night.H.update(); night.H.update();
check('an unchanged mask is not written again', night.writes, ['--holes', '--hs', '--sy']);
const rain = page({ sky: 'rain', rain: 'm', snow: '' }, []);
check('rain: built', typeof rain.props['--holes'], 'string');
const stale = page({ sky: 'rain', rain: '', snow: '' }, []);
check('rain while stale (no effect): not built', stale.props['--holes'], undefined);
night.de.dataset.sky = 'day'; night.H.update();
check('the effect ends: the mask and the shift are dropped', [night.props['--holes'], night.props['--sy']], [undefined, undefined]);

// page coordinates: a tile 20 px from the window's top at a scroll of 500 is a hole at 520 on the page
const sc = page({ sky: 'night' }, [[10, 20, 100, 50, 24]], { scrollY: 500 });
check('built at a scroll: the hole in page px, the shift at the scroll', [holes(sc).includes(H.path([[10, 520, 100, 50, 24]])), sc.props['--sy']], [true, '500px']);
// a page scroll: --sy follows scrollY, the mask image is never rewritten (the blink was one new data: image per frame)
const pg = page({ sky: 'night' }, [[10, 20, 100, 50, 24], [10, 82, 340, 300, 24], [10, 394, 340, 900, 24], [0, 0, 0, 0, 16, 'sheet', 'fixed']]);
const h0 = holes(pg);
for (let i = 0; i < 300; i++) pg.scroll(i < 150 ? 3 : -2);  // 5 s at 60 frames a second, down then back up
check('page scroll x300: --holes written 0 times, --sy 300 times, the holes unchanged', [pg.n('--holes') - 1, pg.n('--sy') - 1, holes(pg) === h0], [0, 300, true]);
check('page scroll: --sy is scrollY', [pg.props['--sy'], pg.win.scrollY], ['150px', 150]);
check('page scroll: a rebuild at any scroll gives the same holes (page coordinates)', (pg.H.update(), [pg.n('--holes'), holes(pg) === h0]), [1, true]);
// a height-only resize (a phone's URL bar) builds the same mask: no new image; a width change does
pg.win.innerHeight = 700; pg.ls.resize(); pg.tick();
check('URL-bar resize (height only): no new mask, also after the late rebuild', pg.n('--holes'), 1);
pg.win.innerWidth = 400; pg.ls.resize();
check('a width change: a new mask, its size', [pg.n('--holes'), pg.props['--hs']], [2, '400px 2400px']);
// the dashboard's debounced relayout (dash.js fillRow, 180 ms after the resize) stretches the last tile by an inline style that
// no observer sees: the late rebuild, after it, carries the new width
pg.els[2].box[2] = 380;
check('resize: a second rebuild after the dashboard relayout (later than its 180 ms)', [pg.tick(), pg.n('--holes'), holes(pg).includes(H.path([[10, 394, 380, 900, 24]]))], [[250], 3, true]);
pg.de.scrollHeight = 2600; pg.H.update();
check('the page grows (ResizeObserver): a new mask, its height', [pg.n('--holes'), pg.props['--hs']], [4, '400px 2600px']);

// an open sheet (frosted, not .glass) is cut out too; its nested .glass list adds no seam (the union); a hidden sheet's empty
// box is left out; the sheet's slide-in end and its hidden attribute rebuild the mask
const SH = [0, 100, 360, 680, 16, 'sheet', 'fixed'];
const withSheet = page({ sky: 'night', rain: '', snow: '' }, [[10, 20, 100, 50, 24], SH, [20, 160, 320, 40, 12]]);
const hs = holes(withSheet);
check('night + an open sheet: the tile, the sheet and its nested glass all holes', [hs.includes(ONE), (hs.match(/M/g) || []).length], [true, 3]);
check('the sheet is one rounded rectangle grown by 6', hs.includes(H.path([SH.slice(0, 5)])), true);
const hidden = page({ sky: 'night', rain: '', snow: '' }, [[10, 20, 100, 50, 24], [0, 0, 0, 0, 16, 'sheet', 'fixed']]);
check('a hidden sheet (empty box): no hole, the tile still cut', [(holes(hidden).match(/M/g) || []).length, holes(hidden).includes(ONE)], [1, true]);
check('rebuilt at the sheet’s animationend (capture) and on its hidden attribute', [typeof withSheet.ls.animationend, withSheet.mo[0].attributeFilter.includes('hidden')], ['function', true]);
withSheet.els[1].box = [0, 300, 360, 480];  // the slide-in ended lower
const w0 = withSheet.n('--holes');
withSheet.ls.animationend();
check('animationend: the mask follows the sheet', [withSheet.n('--holes') > w0, holes(withSheet).includes(H.path([[0, 300, 360, 480, 16]]))], [true, true]);
// a scroll inside the sheet moves its own tiles: rebuilt; a page scroll under the open (fixed) sheet: rebuilt, the sheet's
// hole stays where the window shows it
const w1 = withSheet.n('--holes');
withSheet.els[2].box[1] -= 30; withSheet.ls.scroll({ target: {} });
check('a scroll inside a sheet (capture): rebuilt, its tile moved', [withSheet.n('--holes'), holes(withSheet).includes(H.path([[20, 130, 320, 40, 12]]))], [w1 + 1, true]);
withSheet.scroll(40);
check('a page scroll under an open sheet: rebuilt, the sheet\'s hole at the new scroll', [withSheet.n('--holes'), holes(withSheet).includes(H.path([[0, 340, 360, 480, 16]])), withSheet.props['--sy']], [w1 + 2, true, '40px']);
withSheet.els[1].box = [0, 0, 0, 0]; withSheet.H.update();  // closed
const w2 = withSheet.n('--holes');
withSheet.scroll(40);
check('the sheet closed: a page scroll moves the mask only again', [withSheet.n('--holes'), withSheet.props['--sy']], [w2, '80px']);
const lsrc = /addEventListener\('scroll', .*\{ passive: true, capture: true \}\)/.test(src);
check('the scroll listener stays passive and capture (a sheet scrolls inside)', lsrc, true);

// sky.css: every --holes mask layer shifted up by the page's scroll, the moving rain and snow in their keyframes too
const css = fs.readFileSync(path.join(WEB, 'sky.css'), 'utf8');
const layers = [...css.matchAll(/mask: var\(--holes\) ([^/]*) \//g)].map(x => x[1]);
check('every --holes layer (moon, stars, rain, snow): its y shifted by --sy', [layers.length, layers.every(p => / calc\([^ ]+ [-*] var\(--sy, 0px\)\)$/.test(p))], [4, true]);
check('stars, rain, snow: the holes at the page\'s corner minus the scroll', layers.slice(1), Array(3).fill('0 calc(-1 * var(--sy, 0px))'));
for (const k of ['rainfall', 'snowfall']) check(`@keyframes ${k}: the holes layer keeps the shift`, new RegExp(`@keyframes ${k} \\{ to \\{ mask-position: 0 calc\\(-1 \\* var\\(--sy, 0px\\)\\), `).test(css), true);
// the moon: px from the top (never a percentage of the dynamic viewport), its centre below the top bar (12 + 58 px + the
// mask's 6-px growth), and masked by --holes at the box's own offset minus the scroll, so it is never drawn under the bar's
// controls or a tile
const mo = /\.sky::before \{ left: calc\(88vw - (\d+)px\); top: (\d+)px; width: (\d+)px; height: (\d+)px;[^}]*mask: var\(--holes\) calc\((\d+)px - 88vw\) calc\(-(\d+)px - var\(--sy, 0px\)\) \//.exec(css);
check('moon: placed in px, masked at its own offset', !!mo && [mo[1] === mo[5], mo[2] === mo[6], mo[3] === mo[4], +mo[1] * 2 === +mo[3]].every(Boolean), true);
check('moon: its picture below the bar (centre - 42 px >= 76 px)', mo && +mo[2] + +mo[3] / 2 - 42 >= 76, true);
check('moon: no percentage from the top', /\.sky::before \{[^}]*top: \d+%/.test(css), false);
// the sun on a phone (app.css --sun-at below 720 px): its 34-px disc ends above the bar's buttons (bar 12 + padding 10 + border 1)
const app = fs.readFileSync(path.join(WEB, 'app.css'), 'utf8');
const sun = /@media \(max-width: 719px\) \{ :root \{ --sun-at: \d+% (-?\d+)px; \} \}/.exec(app);
check('sun: on a phone the disc ends above the bar\'s buttons', !!sun && +sun[1] + 34 < 23, true);

console.log(`sky_vectors: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
