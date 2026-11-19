// test/tools/demo_vectors.js -- the /try/ demo (demo/fixtures + demo/replay.js) through the pages' own code. The committed
// snapshots are a fresh replay run; replay's day aggregation equals the generator's daily ring; replay's hour rows carry
// web_render.cpp's keys. Then, at every example day (day and night), every scenario and a day / night time each week of the
// year, with a fake clock: the history sheet's ranges (dash-sheet.js spanOf / spanUrl / rowOf / bucket, the range aggregation
// without the summary row; dash.js's recs() lives inside model() and is restated here), the outlook strip's words
// (outlook.js), the pressure tendency (tendency.js) against the outlook's `tend`, page 2 of the card (dash-wufc.js), the
// airport view (dash-airport.js), the admin Learning sheet (admin-learn.js) for every learner snapshot, and the live body
// between uploads (age_s counts, values change only with an upload).
// Runs in Europe/Warsaw (the demo station's zone). Usage: node test/tools/demo_vectors.js
'use strict';
if (process.env.TZ !== 'Europe/Warsaw') {
  const r = require('child_process').spawnSync(process.execPath, [__filename], { stdio: 'inherit', env: Object.assign({}, process.env, { TZ: 'Europe/Warsaw' }) });
  process.exit(r.status);
}
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { decl, fcStub } = require('./js_decl');

const ROOT = path.join(__dirname, '..', '..'), WEB = path.join(ROOT, 'firmware', 'fauxcloud', 'web'), FIX = path.join(ROOT, 'demo', 'fixtures');
const rd = f => fs.readFileSync(path.join(WEB, f), 'utf8');
const raw = f => fs.readFileSync(path.join(FIX, f), 'utf8').replace(/\r\n/g, '\n');
const J = f => JSON.parse(raw(f));
const I = J('index.json'), F = { 'index.json': I };
[I.series.file, I.daily.file, I.diag, ...I.learn.map(x => x.file), ...Object.values(I.static)].forEach(f => { F[f] = J(f); });
const R = require(path.join(ROOT, 'demo', 'replay.js')).create(F);
const dash = rd('dash.js'), sheet = rd('dash-sheet.js'), health = rd('health.js'), SID = I.station, DAYS = F[I.daily.file].days;
const sec = s => Date.parse(s) / 1e3;

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const clean = (name, s) => check(name + ': no undefined / NaN / placeholder', /undefined|NaN|\{[a-z_0-9]+\}/.test(s), false);
let NOW = 0;
class FD extends Date { constructor(...a) { if (a.length) super(...a); else super(NOW); } static now() { return NOW; } }

// ---- 0. the replay is the source of the snapshots; its aggregation is the generator's; its rows the firmware's keys
const times = [];
I.example_days.forEach(e => times.push([e.id + '-day', e.day, e.day_t0, {}], [e.id + '-night', e.night, e.night_t0, {}]));
Object.entries(I.scenarios).forEach(([k, s]) => times.push([k, s, s.t0, s.switches]));
for (const [k, o, t, sw] of times) {
  check(k + ': live snapshot is a fresh replay', raw(o.live), JSON.stringify(R.live(sec(t), sw)) + '\n');
  check(k + ': frames are a fresh replay', raw(o.frames), JSON.stringify(R.frames(sec(t), sw)) + '\n');
}
const writer = (a, b) => { const src = fs.readFileSync(path.join(ROOT, 'firmware', 'fauxcloud', 'src', 'web_render.cpp'), 'utf8'), body = src.slice(src.indexOf(a), src.indexOf(b));
  return [...new Set([...body.matchAll(/\\"([a-z_0-9]+)\\"/g), ...body.matchAll(/(?:triple|one)\(o, "([a-z_0-9]+)"/g)].map(m => m[1]).concat(['q', 'wind_dir', 'wind_steady']))].sort(); };
const HK = writer('void hourRow(', 'void dayRow('), DK = writer('void dayRow(', 'void renderHourly(');
let nd = 0;
for (const d of DAYS) {
  const m = R.midnight(sec(d.d + 'T12:00:00Z')), H = [];
  if (m < R.H0) continue;
  for (let h = m; h < R.midnight(m + 30 * 3600); h += 3600) { const x = R.hourRow(R.ix(h)); if (x) H.push(x); }
  check(d.d + ': replay dayRow = the daily ring', R.dayRow(d.d, H), d);
  if (!nd++) check('hour row keys = web_render.cpp hourRow', Object.keys(H[0]).sort(), HK);
}
check('days cross-checked', nd > 360, true);
check('day row keys = web_render.cpp dayRow', Object.keys(DAYS[0]).sort(), DK);

// ---- 1. the history sheet, through the replay's answers
const sctx = { measure: 'temp', S: { cur: SID }, Math, Date: FD, String, Array, JSON, encodeURIComponent, FC: {} };
vm.createContext(sctx);
vm.runInContext(['p2', 'dm'].map(n => decl(health, n)).concat(['HOUR', 'hh'].map(n => decl(dash, n)))
  .concat(['SPC', 'ymd', 'addD', 'spanOf', 'spanUrl', 'rowOf', 'bucket'].map(n => decl(sheet, n))).join('\n') + '\nthis.f = { spanOf, spanUrl, rowOf, bucket, addD };', sctx);
const { spanOf, spanUrl, rowOf, bucket, addD } = sctx.f;
const recs = (D, d) => (d ? D.days : D.hours).concat(D.open ? [D.open] : []).map((x, j, a) =>
  Object.assign({}, x, { rej: x.rej_fields || 0, t: new Date(d ? x.d + 'T00:00' : x.h), open: !!D.open && j === a.length - 1 }));
const KEYS = { temp: 'temp_c', wind: 'wind_kmh', rain: 'rain_mm', hum: 'humidity', baro: 'pressure_hpa', sun: 'solar_wm2' };
function ranges(tag, t, sw, presets) {
  NOW = t * 1e3;
  const now = new Date(NOW);
  for (const p of presets) {
    const z = spanOf({ p }, now), url = spanUrl(z, { p }, SID) || `/api/daily?station=${SID}&days=90`, a = R.answer(url, t, sw).body;
    const rec = recs(a, z.step > 6).filter(x => !z.a || x.t >= z.a && x.t < addD(z.b, 1));
    if (!z.a) { z.a = addD(rec[0].t, 0); z.step = Math.round((addD(z.b, 1) - z.a) / 864e5) <= 366 ? 24 : 168; }
    const g = `${tag} ${p}`;
    check(g + ': rows, in order, the open one last', [rec.length > 0, rec.every((x, i) => !i || x.t > rec[i - 1].t), rec[rec.length - 1].open], [true, true, true]);
    check(g + ': nothing after now', rec.every(x => x.t <= now), true);
    for (const [m, k] of Object.entries(KEYS)) {
      sctx.measure = m;
      const B = z.step === 6 || z.step === 168 ? bucket(rec.map(x => rowOf(x, k)), z.step) : rec.map(x => rowOf(x, k));
      check(`${g} ${k}: finite, lo <= mid <= hi`, B.every(b => [b.lo, b.mid, b.hi, b.g, b.dir].every(v => v == null || Number.isFinite(v))
        && (b.lo == null || b.hi == null || b.mid == null || b.lo <= b.mid + 1e-9 && b.mid <= b.hi + 1e-9)), true);
    }
    if (p === 1) check(g + ': today hour by hour', rec.length, Math.floor((t - R.midnight(t)) / 3600) + 1 - (rec.some(x => x.n === 0) ? 1 : 0));
    if (p === 30) check(g + ': 30 days', rec.length, 30);
  }
}

// ---- 2. the strip, the tendency, page 2, the airport view
const FCO = new Function(rd('outlook.js') + '\nreturn FCO;')(), FCT = new Function('FC', rd('tendency.js') + '\nreturn FCT;')({ w: (f, t) => t });
const doc = { documentElement: { dataset: {} }, visibilityState: 'visible', addEventListener() {}, body: { dataset: {} },
  querySelector: s => s === '#apv' ? { addEventListener() {} } : null, head: { insertAdjacentHTML() {}, appendChild() {} }, createElement: () => ({}) };
const ctx = { console, Object, String, Math, Array, JSON, Date: FD, Number, isNaN, document: doc, setInterval: () => 0, setTimeout: () => 0, clearTimeout() {},
  localStorage: { getItem: () => null, setItem() {} }, matchMedia: () => ({ matches: false }), AbortController: class {}, fetch: () => new Promise(() => {}) };
vm.createContext(ctx);
vm.runInContext(rd('core.js') + '\n' + rd('outlook.js') + '\nlet IMP = false; const U = { t: "°C", w: "km/h", r: "mm", p: "hPa" }; const M = {}; const S = { ap: true };\n' +
  'let why = false, lastFocus = null;\n' + ['WD', 'CONV', 'cvv', 'cv', 'toKmh', 'dsep', 'f1', 'f2', 'f0', 'fr', 'CARD', 'card', 'CALM_KMH', 'calm', 'dial', 'IC', 'icon', 'olIn', 'OLI'].map(n => decl(dash, n)).join('\n') +
  '\n' + ['p2', 'hm'].map(n => decl(health, n)).join('\n') + '\nFC.poll = (n, t) => { (this.TASKS = this.TASKS || {})[n] = t; };\n' + rd('dash-airport.js') + '\n' + rd('dash-wufc.js') +
  '\nthis.APV = APV; this.FCX = FCX; this.S = S;', ctx);
function sources(tag, t, sw) {
  NOW = t * 1e3;
  const L = R.answer(`/api/live?station=${SID}`, t, sw).body.station, m = R.answer('/api/metar', t, sw).body, st = R.answer('/api/bridge/status', t, sw).body;
  // no shell version in the demo's answers: core.js never reloads the demo page (the board's /api/live carries one)
  check(tag + ': no page key', ['page' in R.answer(`/api/live?station=${SID}`, t, sw).body, 'page' in (R.answer('/api/live', t, sw).body.diag || {})], [false, false]);
  const o = L.outlook, ln = FCO.lines(o, { gust: g => Math.round(g) + ' km/h' });
  check(tag + ': the strip has words', !!(ln && (ln.l1 || ln.l2)), true);
  const last = sec(L.last_seen), H = [];
  for (let h = last - last % 3600 - 6 * 3600; h < last - last % 3600; h += 3600) { const x = R.hourRow(R.ix(h)); if (x) H.push(x); }
  const tr = FCT.pressure(H, { lat: I.lat, lon: I.lon }, last * 1e3);
  if (tr) check(tag + ': tendency.js = outlook.tend', [tr.d, tr.code, tr.fast], [o.tend.d, o.tend.code, o.tend.fast]);
  Object.assign(ctx.S, { ap: false, status: st });
  ctx.TASKS.wufcm.on({ data: m, changed: false });
  const p2 = ctx.FCX.p2(L);
  check(tag + ': page 2 with a source only', !!p2, sw.metar !== false && m.airports[0].taf.next12.length > 0 || sw.wufc !== false);
  if (p2) clean(tag + ' page 2', JSON.stringify(p2));
  if (m.on) {
    const h = ctx.APV.html(m, false, new Date(NOW));
    clean(tag + ' airport view', h.replace(/<svg[\s\S]*?<\/svg>/g, ''));
    check(tag + ': airport view', [h.includes('<h1>Airport weather · Warsaw Modlin</h1>'), h.includes('EPWA')], [true, true]);
  }
  // polled every 30 s: age_s counts, the values change only with an upload
  const a = R.live(t + 30, sw).station, b = R.live(t + 31, sw).station;
  check(tag + ': age_s counts between polls', b.last_seen === a.last_seen ? b.age_s - a.age_s : 1, 1);
  return { L, ln, p2 };
}
const LINES = {
  'winter-day': ['Sleet or snow. Freezing.', 'Pressure steady.', 'Icy surfaces possible.'], 'spring-day': ['Sunny.', 'Pressure steady.', ''], 'summer-day': ['Sunny. Hot.', 'Pressure steady.', ''],
  'autumn-day': ['No rain.', 'Pressure falling: less settled weather possible.', ''], 'winter-night': ['Clear night. Freezing.', 'Pressure steady.', 'Icy surfaces possible.'],
  'spring-night': ['Clear night. Calm.', 'Pressure falling: less settled weather possible.', 'Ground frost possible.'], 'autumn-night': ['Light rain.', 'Pressure rising.', ''],
  today: ['Sunny spells.', 'Pressure falling: less settled weather possible.', ''], sunny: ['Sunny. Hot.', 'Pressure steady.', ''],
  thunderstorm: ['Heavy rain.', 'Pressure steady.', 'Thunder reported nearby.'], snow: ['Sleet or snow. Freezing.', 'Pressure steady.', 'Icy surfaces possible.'],
  'pressure-off': ['Sunny.', 'Pressure steady.', ''], 'no-sources': ['Sunny spells.', 'Pressure falling: less settled weather possible.', ''],
};
// the wet snow tells one story: over the demo's start window (t0 +- 30 min) and the hour after, the gauge's rate, a
// temperature at or below 1 °C (the sky's flakes) and the main airport's snow
for (const k of ['snow', 'winter-day']) {
  const t0 = sec(k === 'snow' ? I.scenarios.snow.t0 : I.example_days.find(e => e.id === 'winter').day_t0);
  for (let t = t0 - 1800; t <= t0 + 1800; t += 300) {
    const v = R.live(t, {}).station.values, a = R.metar(t, {}, v.pressure_hpa).airports[0];
    check(`${k} @${t - t0}: the gauge counts the wet snow`, v.rain_rate_mmh > 0, true);
    check(`${k} @${t - t0}: at or below 1 °C (flakes)`, v.temp_c <= 1, true);
    check(`${k} @${t - t0}: the main airport reports snow`, a.icao === 'EPMO' && a.wx.snow, true);
    // the words say what the sky draws (sleet_snow at <= 1.0 °C, the station's own state over the airport's snow)
    check(`${k} @${t - t0}: the strip says sleet or snow`, FCO.lines(R.live(t, {}).station.outlook, {}).l1.startsWith('Sleet or snow.'), true);
  }
}
for (const [k, o, t, sw] of times) {
  ranges(k, sec(t), sw, [1, 3, 7, 30, 90, 365, 'all']);
  const { L, ln } = sources(k, sec(t), sw);
  if (LINES[k]) check(k + ': strip lines', [ln.l1, ln.l2, ln.warn], LINES[k]);
  if (/night-clear|frost-night|summer-night/.test(k)) check(k + ': the minimum range', /^Minimum around .* tonight\.$/.test(ln.l2), true);
}
// a day and a night time in every week of the year: nothing breaks anywhere
for (let t = sec(I.series.first_t0) + 86400; t <= sec(I.t0); t += 7 * 86400) {
  const d = R.midnight(t);
  ranges('week ' + R.dkey(t), d + 12 * 3600 + 777, {}, [1, 7, 30]);
  sources('week ' + R.dkey(t) + ' noon', d + 12 * 3600 + 777, {});
  sources('week ' + R.dkey(t) + ' night', d + 23 * 3600 + 333, {});
}

// ---- 3. the admin Learning sheet for every learner snapshot, and the snapshot a time picks
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const SHEETS = {};
new Function('esc', 'SHEETS', 'API', 'FC', 'icon', 'draw', 'SH', 'atob', 'toast', 'ago', 'api', rd('admin-learn.js'))(esc, SHEETS, { stations: [] }, fcStub(),
  k => `<svg data-i="${k}"></svg>`, () => {}, null, s => Buffer.from(s, 'base64').toString('binary'), () => {}, s => s + ' s ago', () => Promise.resolve({ ok: true }));
for (const e of I.learn) {
  const h = SHEETS.learn.body(F[e.file], I.name), txt = h.replace(/<svg[\s\S]*?<\/svg>/g, '');
  clean(e.t + ' Learning sheet', txt);
  for (const t of ['Clear-sky level', 'Humidity at saturation', 'Evening cooling', 'Night frost', 'Pressure check']) check(e.t + ': ' + t, h.includes(t), true);
  check(e.t + ': the sky map drawn', (h.match(/<rect /g) || []).length, 949);
}
const off = R.answer('/api/learn?station=' + SID, sec(I.scenarios['pressure-off'].t0), I.scenarios['pressure-off'].switches).body;
check('pressure-off: L7 flags it, the sheet says Set REL', [off.l7.flag, /Set REL/.test(SHEETS.learn.body(off, I.name))], ['high', true]);
check('the Learning tile: 4 of 4 in use', R.answer('/api/admin/station', sec(I.t0), {}).body.learn_use, 4);

console.log(fail ? `FAILED: ${fail} failed, ${pass} passed` : `OK: ${pass} passed, 0 failed`);
process.exit(fail ? 1 : 0);
