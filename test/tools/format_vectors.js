// test/tools/format_vectors.js -- runs firmware/fauxcloud/web/core.js (inlined in all three faces) under node with
// minimal browser stubs and checks its shared formatters: `dur`, the uptime of the admin's "Running for" and the
// dashboard's "Uptime". Usage: node test/tools/format_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'core.js'), 'utf8');
const noop = () => {};
const el = { addEventListener: noop, removeEventListener: noop, setAttribute: noop, classList: { add: noop, remove: noop, toggle: noop } };
const stubs = {
  document: Object.assign({ documentElement: Object.assign({ dataset: {} }, el), visibilityState: 'visible', querySelector: () => null,
    querySelectorAll: () => [], getElementById: () => null, body: Object.assign({ children: [] }, el) }, el),
  window: Object.assign({ matchMedia: () => ({ matches: false, addEventListener: noop }) }, el),
  matchMedia: () => ({ matches: false, addEventListener: noop }),
  localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
  navigator: { userAgent: 'node' }, location: { pathname: '/', search: '', hostname: 'test' },
};
const dur = new Function(...Object.keys(stubs), src + '\nreturn dur;')(...Object.values(stubs));

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

const cases = [[0, '0 s'], [42, '42 s'], [59, '59 s'], [60, '1 min'], [1020, '17 min'], [3599, '59 min'], [3600, '1 h'],
  [3660, '1 h 1 min'], [18720, '5 h 12 min'], [86399, '23 h 59 min'], [86400, '1 d'], [93600, '1 d 2 h'], [864000, '10 d'],
  [59.9, '59 s'], [-1, '–'], [NaN, '–'], [undefined, '–'], [null, '–'], [Infinity, '–']];
for (const [s, want] of cases) check('dur(' + s + ')', dur(s), want);

// dash.js beaufort() in display case, offSince() as one template
{
  const vm = require('vm');
  const { decl, fcText } = require('./js_decl');
  const web = f => fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', f), 'utf8');
  const dash = web('dash.js'), health = web('health.js');  // p2, hm, dm, dayTime, offSince live in health.js
  const ctx = { IMP: false, String, Date, Math, FC: {} };
  vm.createContext(ctx);
  vm.runInContext(fcText(src) + ['p2', 'hm', 'dm', 'WH', 'dayTime', 'offSince'].map(n => decl(health, n)).concat(['WD', 'toKmh', 'CALM_KMH', 'BEAU', 'beaufort'].map(n => decl(dash, n))).join('\n') +
    '\nthis.f = { beaufort, offSince };', ctx);
  const { beaufort, offSince } = ctx.f;
  for (const [v, want] of [[0, 'Beaufort 0, Calm'], [0.9, 'Beaufort 0, Calm'], [1, 'Beaufort 1, Light air'], [5.9, 'Beaufort 1, Light air'],
    [6, 'Beaufort 2, Light breeze'], [28.9, 'Beaufort 4, Moderate breeze'], [40, 'Beaufort 6, Strong breeze'], [117.9, 'Beaufort 11, Violent storm'],
    [118, 'Beaufort 12, Hurricane force']]) check('beaufort(' + v + ')', beaufort(v), want);
  ctx.IMP = true;  // mph in, km/h bands: 10 mph = 16.1 km/h
  check('beaufort(10 mph)', beaufort(10), 'Beaufort 3, Gentle breeze');
  const now = new Date(2026, 10, 13, 15, 0);
  check('offSince today', offSince(new Date(2026, 10, 13, 9, 5).toISOString(), now), 'Offline since 09:05');
  check('offSince yesterday', offSince(new Date(2026, 10, 12, 23, 40).toISOString(), now), 'Offline since yesterday 23:40');
  check('offSince older', offSince(new Date(2026, 10, 10, 7, 0).toISOString(), now), 'Offline since 10 Nov 07:00');
  check('offSince none', [offSince('', now), offSince('garbage', now)], ['', '']);
}

console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
