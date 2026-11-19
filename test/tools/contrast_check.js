// test/tools/contrast_check.js -- WCAG contrast of the caption inks (--ink-3, --ink-2) on every
// background the faces draw them on, computed from the tokens in firmware/fauxcloud/web/app.css (each face's copy:
// dash, setup, admin; dark = the face's first :root block, light = its :root[data-theme="light"] block, the only
// place the light palette lives). Backgrounds, worst case per sky state (day sd, night sn, rain sr): each sky
// stop (top, mid, low) alone and under one of its layers at full strength (deck, cloud, the three glows); glass =
// --tint-a or --tint-b over each of those; sheet = --solid over each. Captions are 12-13 px, so the floor is 4.5:1.
// Gate: both themes. Light: --ink-3 .80, --ink-2 .86. Dark: --ink-3 .90, --ink-2 .95 and --tint-a .03, in every face
// copy (a dark --ink-3 of .68 over a --tint-a of .11 reads 2.95 on glass).
// Approximate: one layer at a time, no stacking (cloud + glow), no backdrop blur or saturate, glass taken
// at either end of its gradient; phone screenshots over the brightest sky remain the real check.
// Usage: node test/tools/contrast_check.js [--table]   (exit 1 when a caption ink is under 4.5:1)
'use strict';
const fs = require('fs');
const path = require('path');

const css = fs.readFileSync(process.env.CC_CSS || path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'app.css'), 'utf8');
const MIN = 4.5, GATED = ['light', 'dark'];
// the strongest a layer gets on screen: glow-a is centred below the viewport (50% 108%, 70vw x 40vh, clear at 70 %), so
// at the bottom edge it reaches 1 - (8/40)/0.7 = 0.71 of its colour; the other glows peak on screen (app.css .sky)
const REACH = { ga: 0.71 };

function parse(c) {
  if (c === 'transparent') return [0, 0, 0, 0];
  let m = /^#([0-9a-f]{6})$/i.exec(c);
  if (m) return [0, 2, 4].map(i => parseInt(m[1].slice(i, i + 2), 16)).concat(1);
  m = /^rgba?\(([^)]+)\)$/.exec(c);
  if (m) { const p = m[1].split(',').map(Number); return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1]; }
  throw new Error('colour ' + c);
}
const over = (top, bot) => [0, 1, 2].map(i => top[i] * top[3] + bot[i] * (1 - top[3])).concat(1);  // bot opaque
const lum = c => { const f = v => { v /= 255; return v <= .03928 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4; }; return .2126 * f(c[0]) + .7152 * f(c[1]) + .0722 * f(c[2]); };
const ratio = (fg, bg) => { const a = lum(over(fg, bg)), b = lum(bg); return (Math.max(a, b) + .05) / (Math.min(a, b) + .05); };

function tokens(block) {
  const t = {};
  for (const m of block.matchAll(/--([a-z0-9-]+):\s*([^;]+);/g)) t[m[1]] = m[2].trim();
  return t;
}
// a face's text, then its dark and light token blocks
function face(name) {
  const i = css.indexOf(`/*@face ${name}*/`), j = css.indexOf('/*@face ', i + 1), s = css.slice(i, j < 0 ? undefined : j);
  const blk = re => { const m = re.exec(s); if (!m) throw new Error(name + ': no block ' + re); return s.slice(m.index, s.indexOf('}', m.index)); };
  return { dark: tokens(blk(/:root \{\s*\/\*|:root \{\n\s*--s/)), light: tokens(blk(/:root\[data-theme="light"\] \{/)) };
}

function worst(t, ink) {
  const fg = parse(t[ink]), res = {};
  for (const sky of ['sd', 'sn', 'sr']) {
    const bare = [];
    for (const st of ['t', 'm', 'l']) {
      const base = parse(t[`${sky}-${st}`]);
      bare.push([st, base]);
      for (const ly of ['dk', 'cl', 'ga', 'gb', 'gc']) if (t[`${sky}-${ly}`]) { const c = parse(t[`${sky}-${ly}`]); c[3] *= REACH[ly] || 1; bare.push([st + '+' + ly, over(c, base)]); }
    }
    const layers = { sky: bare, glass: bare.flatMap(([w, b]) => ['tint-a', 'tint-b'].map(k => [w + '/' + k, over(parse(t[k]), b)])),
      sheet: bare.map(([w, b]) => [w + '/solid', over(parse(t.solid), b)]) };
    for (const [l, list] of Object.entries(layers)) {
      const r = list.map(([w, b]) => [ratio(fg, b), sky + ':' + w]).sort((a, b) => a[0] - b[0])[0];
      if (!res[l] || r[0] < res[l][0]) res[l] = r;
    }
  }
  return res;
}

let fail = 0;
const rows = [];
for (const f of ['dash', 'setup', 'admin']) {
  const F = face(f);
  for (const theme of ['dark', 'light']) for (const ink of ['ink-3', 'ink-2']) {
    const w = worst(F[theme], ink);
    for (const [l, [r, where]] of Object.entries(w)) {
      const bad = r < MIN;
      if (bad && GATED.includes(theme)) fail++;
      rows.push(`${f.padEnd(6)} ${theme.padEnd(5)} --${ink.padEnd(5)} ${F[theme][ink].padEnd(22)} ${l.padEnd(6)} ${r.toFixed(2).padStart(5)}:1  ${where}${bad ? '  UNDER ' + MIN : ''}`);
    }
  }
}
if (process.argv.includes('--table') || fail) console.log(rows.join('\n'));
console.log((fail ? 'FAILED' : 'OK') + ': ' + rows.length + ' worst cases, ' + fail + ' gated under ' + MIN + ':1, ' +
  rows.filter(r => r.includes('UNDER')).length + ' under in all');
process.exit(fail ? 1 : 0);
