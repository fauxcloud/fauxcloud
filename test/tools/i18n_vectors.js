// test/tools/i18n_vectors.js -- the dashboard's language runtime (web/lang.js on web/core.js) and its words in another
// language. 1. the language a browser gets (Auto from navigator.languages, the pick in localStorage, a language without a
// pack), the initial packs ahead of the first poll, a pack applied in place, a lazy script's pack before the script, a 404
// pack tried three times then English, the shell's data-t words, counts through Intl.PluralRules. 2. the pseudo
// language xx: every key of every W table bracketed; the status row, the chips, the state line, the dial, the sheet's
// rows, the outlook strip and its explanation rendered through the page's own functions; no two plain words outside the
// brackets (a text the extraction missed). 3. every Polish count entry for n = 0, 1, 2, 5, 12, 21, 22, 25, 101, 1.5.
// Usage: node test/tools/i18n_vectors.js   (needs python for the key list: tools/i18n_inventory.py --keys)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const cp = require('child_process');
const { decl, fcText } = require('./js_decl');

const ROOT = path.join(__dirname, '..', '..'), WEB = path.join(ROOT, 'firmware', 'fauxcloud', 'web');
const rd = f => fs.readFileSync(path.join(WEB, f), 'utf8');
let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const tick = () => new Promise(r => setImmediate(r));
const settle = async () => { for (let i = 0; i < 20; i++) await tick(); };

// ---------------------------------------------------------------- 1. the runtime
// core.js + lang.js in a vm with a fake page: fetch answers from `serve` (url -> body or a status), the shell holds two
// data-t elements and the language button
function page({ langs = ['en-US'], pick = null, serve = {}, manifest, fco = true, intl = Intl, ro = false, changed }) {
  const asked = [], store = pick ? { 'fc-lang': pick } : {}, shellEls = [];
  const el = (t, ds, at = {}) => ({ textContent: t, dataset: ds, attrs: at, setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k] ?? null; } });
  shellEls.push(el('Loading…', { t: 'index.loading' }), el('', { tl: 'index.units' }, { 'aria-label': 'Switch units' }));
  const btn = Object.assign(el('EN', {}), { hidden: true, title: '' }), on = {}, reloads = [];
  btn.closest = s => s === '#langBtn' ? btn : null;
  // a script element appended to the head runs in the page (FC.lazy)
  const document = { documentElement: { dataset: {}, lang: 'en' }, visibilityState: 'visible', addEventListener: (k, f) => { (on[k] = on[k] || []).push(f); }, createElement: () => ({}),
    head: { appendChild: s => vm.runInContext(s.textContent, ctx) },
    querySelector: s => s === '#langBtn' ? btn : null, querySelectorAll: s => s.includes('data-t') ? shellEls : [] };
  const fetch = url => { asked.push(url); const b = serve[url];
    const r = typeof b === 'number' ? { status: b, ok: false } : { status: 200, ok: true };
    return Promise.resolve(Object.assign(r, { headers: { get: () => null }, json: () => Promise.resolve(b), text: () => Promise.resolve(String(b)) })); };
  const ctx = { console, Object, String, Math, Array, JSON, Date, Number, Promise, Map, Set, Intl: intl, setTimeout, clearTimeout, setInterval: () => 0,
    document, navigator: { languages: langs }, location: { reload() { reloads.push(store['fc-lang']); } }, fetch, AbortController: class { abort() {} },
    localStorage: { getItem: k => store[k] ?? null, setItem: (k, v) => { if (ro) throw new Error('QuotaExceededError'); store[k] = v; } }, matchMedia: () => ({ matches: false }) };
  if (changed) ctx.changed = changed;
  vm.createContext(ctx);
  const lang = rd('lang.js').replace('/*@langs*/{}', JSON.stringify(manifest || { pl: { n: 'Polski', 'status.js': '/a/1/status.pl.json', 'outlook.js': '/a/2/outlook.pl.json', 'dash-sheet.js': '/a/3/dash-sheet.pl.json' }, _i: ['outlook.js', 'status.js'] }));
  vm.runInContext(rd('core.js') + (fco ? '\nconst FCO = { W: { dry: "No rain." } };\n' : '\n') + lang + '\nthis.FC = FC; this.FCL = FCL; this.WC = WC;' + (fco ? ' this.FCO = FCO;' : ''), ctx);
  // a tap on the button (the page's click listeners, as the browser would call them)
  const tap = () => (on.click || []).forEach(f => f({ target: btn }));
  return { ctx, asked, store, shellEls, btn, tap, reloads };
}
(async () => {
  const PL = { '/a/1/status.pl.json': { core: { wu_step: 'Krok' }, index: { loading: 'Wczytywanie…', units: 'Jednostki' }, dash: { x: 'y' },
      notice: JSON.parse('{"nc_x":"Czujnik","n_min":"{n} min","__proto__":{"polluted":1}}') },
    '/a/2/outlook.pl.json': { outlook: { dry: 'Bez opadow.' } }, '/a/3/dash-sheet.pl.json': { 'dash-sheet': { csv: 'Eksportuj CSV' } },
    '/a/h/dash-sheet.js': 'this.SHEET = FC.w("dash-sheet", { csv: "Export CSV", none: "None" });' };
  // the language a browser gets (the worked cases of the language choice)
  for (const [langs, pick, want] of [[['pl-PL', 'pl'], null, 'pl'], [['pl-PL', 'en-US'], null, 'pl'], [['en-US', 'pl'], null, 'en'], [['en-PL'], null, 'en'],
    [['de-DE', 'de'], null, 'en'], [['de-DE', 'pl'], null, 'pl'], [['pl-PL'], 'en', 'en'], [['en-US'], 'pl', 'pl'], [['pl-PL'], 'auto', 'pl'], [['pl-PL'], 'de', 'en'], [[], null, 'en']])
    check('lang ' + langs.join(',') + ' pick ' + pick, page({ langs, pick }).ctx.FCL.lang, want);
  check('no pack in the image: English, the button hidden', (p => [p.ctx.FCL.lang, p.btn.hidden])(page({ langs: ['pl'], manifest: {} })), ['en', true]);
  // English: no pack asked, the identity of every table, the English plural rule
  { const p = page({ langs: ['en-GB'], serve: PL }); await settle();
    check('English asks no pack', p.asked, []);
    check('English counts', [0, 1, 2, 1.5].map(n => p.ctx.FC.t({ one: '{n} row', other: '{n} rows' }, { n })), ['0 rows', '1 row', '2 rows', '1.5 rows']);
    check('English: no locale hooks', p.ctx.FC.L, undefined); }
  // Polish: both initial packs first in the queue, applied in place (core.js's and outlook.js's tables registered by lang.js)
  { const p = page({ langs: ['pl-PL'], serve: PL });
    p.ctx.FC.get('/api/live'); await settle();
    check('the initial packs before the first poll', [p.asked.slice(0, 2).sort(), p.asked[2]], [['/a/1/status.pl.json', '/a/2/outlook.pl.json'], '/api/live']);
    check('core.js table in place', p.ctx.WC.wu_step, 'Krok');
    check('outlook.js table in place', p.ctx.FCO.W.dry, 'Bez opadow.');
    check('the shell words', [p.shellEls[0].textContent, p.shellEls[1].attrs['aria-label']], ['Wczytywanie…', 'Jednostki']);
    check('html lang', p.ctx.document.documentElement.lang, 'pl');
    check('the button: the language it speaks, and the switch in words', [p.btn.hidden, p.btn.textContent, p.btn.attrs['aria-label'], p.btn.title], [false, 'PL', 'Language: Polski. Switch to English', 'Language: Polski. Switch to English']);
    check('Polish counts (Intl)', [1, 2, 5, 22, 1.5].map(n => p.ctx.FC.t({ one: 'o', few: 'f', many: 'm', other: 'x' }, { n })), ['o', 'f', 'm', 'f', 'x']);
    check('the decimal comma', p.ctx.FC.L.dp, ',');
    check('relative time from Intl', p.ctx.FC.L.ago(300), '5 min temu');
    check('a key the English table lacks is ignored', p.ctx.FC.w('dash', { a: 'A' }).x, undefined);
    check('an empty table (the notices) takes its pack whole', Object.assign({}, p.ctx.FC.w('notice', {})), { nc_x: 'Czujnik', n_min: '{n} min' });
    // a lazy script: its pack, then the script; its table registers with the pack in hand
    await p.ctx.FC.lazy('/a/h/dash-sheet.js'); await settle();
    check('lazy: the pack before the script', p.asked.slice(-2), ['/a/3/dash-sheet.pl.json', '/a/h/dash-sheet.js']);
    check('lazy: the table in Polish at once, missing keys English', [p.ctx.SHEET.csv, p.ctx.SHEET.none], ['Eksportuj CSV', 'None']);
    check('FC.en: a key\'s English after the pack', [p.ctx.FC.en(p.ctx.SHEET, 'csv'), p.ctx.WC.wu_step, p.ctx.FC.en(p.ctx.WC, 'wu_step').slice(0, 18)], ['Export CSV', 'Krok', 'On the console’s p']); }
  // the admin: its one initial script's pack first, the formats once it is in, outlook.js (lazy there) registered when it runs
  { const AD = { '/a/9/admin.pl.json': { core: { wu_step: 'Krok' } }, '/a/2/outlook.pl.json': { outlook: { dry: 'Bez opadow.' } },
      '/a/o/outlook.js': 'const FCO = { W: FC.w("x", { dry: "No rain." }) };' };
    const p = page({ langs: ['pl'], fco: false, serve: AD, manifest: { pl: { n: 'Polski', 'admin.js': '/a/9/admin.pl.json', 'outlook.js': '/a/2/outlook.pl.json' }, _i: ['admin.js'] } });
    p.ctx.FC.get('/api/admin/session'); await settle();
    check('admin: its pack before its first request', p.asked.slice(0, 2), ['/a/9/admin.pl.json', '/api/admin/session']);
    check('admin: the formats once the pack is in', [p.ctx.FC.L && p.ctx.FC.L.dp, p.ctx.document.documentElement.lang, p.ctx.WC.wu_step], [',', 'pl', 'Krok']);
    await p.ctx.FC.lazy('/a/o/outlook.js'); await settle();
    check('admin: the lazy outlook table registered and in Polish', p.ctx.FC.t(vm.runInContext('FCO.W.dry', p.ctx)), 'Bez opadow.'); }
  // a sensor's Home Assistant key comes from the type's English label whatever the page's language (admin-station.js)
  { const st = rd('admin-station.js'), ctx = { Object, FC: { en: (t, k) => ({ ty_temperature: 'Temperature', ty_humidity: 'Humidity' })[k] } };
    vm.createContext(ctx);
    vm.runInContext('const WA = { ty_temperature: "Temperatura", ty_humidity: "Wilgotnosc", ty_h: "x" };\n' + decl(st, 'enLabel') + '\nthis.f = enLabel;', ctx);
    check('enLabel: the English label behind a Polish one', [ctx.f({ label: 'Temperatura' }), ctx.f({ label: 'Wilgotnosc' }), ctx.f({ label: 'PM2.5' })], ['Temperature', 'Humidity', 'PM2.5']); }
  // a tap switches to the other language (no Auto state): stored, reloaded; the browser's language only before the first tap
  { const e = page({ langs: ['en-US'], serve: PL }); await settle();
    check('an English browser: EN, the switch to Polski in words', [e.btn.textContent, e.btn.attrs['aria-label']], ['EN', 'Language: English. Switch to Polski']);
    check('no choice stored before the first tap', e.store['fc-lang'], undefined);
    e.tap();
    check('the first tap on an English browser stores Polski (no dead first tap)', [e.reloads, e.store['fc-lang']], [['pl'], 'pl']);
    const p = page({ langs: ['pl'], serve: PL }); await settle();
    p.tap();
    check('a Polish browser: the first tap stores English', p.reloads, ['en']);
    const q = page({ langs: ['pl'], pick: 'en', serve: PL }); await settle();
    q.tap();
    check('a pick of English: EN, its label, a tap back to Polski', [q.btn.textContent, q.btn.attrs['aria-label'], q.reloads], ['EN', 'Language: English. Switch to Polski', ['pl']]);
    const r = page({ langs: ['de'], pick: 'pl', serve: PL }); await settle();
    check('a pick of Polski on a German browser', [r.btn.textContent, r.btn.attrs['aria-label']], ['PL', 'Language: Polski. Switch to English']);
    const a = page({ langs: ['pl'], pick: 'auto', serve: PL }); await settle();
    check('an earlier image\'s stored auto: the browser\'s language', a.btn.textContent, 'PL'); }
  // no reload while an admin sheet holds unsaved changes, nor when the storage refuses the choice (a private window)
  { const d = page({ langs: ['pl'], serve: PL, changed: () => ({ name: 'x' }) }); await settle();
    d.tap();
    check('an admin sheet with unsaved changes: the tap keeps the page', [d.reloads, d.store['fc-lang']], [[], undefined]);
    const c = page({ langs: ['pl'], serve: PL, changed: () => ({}) }); await settle();
    c.tap();
    check('a sheet without changes: the tap switches', c.reloads, ['en']);
    const r = page({ langs: ['pl'], serve: PL, ro: true }); await settle();
    r.tap();
    check('a storage that refuses the choice: no reload into the same language', r.reloads, []); }
  // the main pack never lands: another script's pack that did stays out of its table (no half-Polish page)
  { const p = page({ langs: ['pl'], serve: { '/a/1/status.pl.json': 404, '/a/2/outlook.pl.json': { outlook: { dry: 'Bez opadow.' } } } });
    await settle();
    check('outlook stays English while the main pack is missing', [p.ctx.FCO.W.dry, p.ctx.document.documentElement.lang, p.btn.textContent], ['No rain.', 'en', 'EN']);
    check('a table registered later stays English too', p.ctx.FC.w('outlook', { dry: 'No rain.' }).dry, 'No rain.'); }
  // an older browser without Intl.RelativeTimeFormat (Safari < 14): the words in Polish, every formatter English
  { const p = page({ langs: ['pl'], serve: PL, intl: Object.freeze(Object.assign(Object.create(null), { PluralRules: Intl.PluralRules, NumberFormat: Intl.NumberFormat, DateTimeFormat: Intl.DateTimeFormat })) });
    await settle();
    check('no RelativeTimeFormat: Polish words, English formatters', [p.ctx.WC.wu_step, p.ctx.FC.L, p.ctx.FC.t({ one: 'o', few: 'f', other: 'x' }, { n: 2 }), p.ctx.document.documentElement.lang, p.btn.textContent], ['Krok', undefined, 'x', 'pl', 'PL']); }
  // a shell word a render replaced before the pack landed stays as drawn (the state line over "Loading…")
  { const p = page({ langs: ['pl'], serve: PL }); p.shellEls[0].textContent = 'Online.'; await settle();
    check('a drawn shell element keeps its words', [p.shellEls[0].textContent, p.shellEls[1].attrs['aria-label']], ['Online.', 'Jednostki']); }
  // a pack that 404s (an old hash after an OTA): three tries, then English, no loop
  { const p = page({ langs: ['pl'], serve: { '/a/1/status.pl.json': 404, '/a/2/outlook.pl.json': 404, '/a/3/dash-sheet.pl.json': 404, '/a/h/dash-sheet.js': 'this.SHEET = FC.w("dash-sheet", { csv: "Export CSV" });' } });
    await settle();
    const n = u => p.asked.filter(x => x === u).length;
    check('the initial pack three times', [n('/a/1/status.pl.json'), n('/a/2/outlook.pl.json')], [3, 3]);
    check('English stays', p.ctx.WC.wu_step.slice(0, 18), 'On the console’s p');
    check('English formats and letters too', [p.ctx.FC.L, p.ctx.document.documentElement.lang, p.btn.textContent, p.ctx.FC.t({ one: 'o', other: 'x' }, { n: 2 })], [undefined, 'en', 'EN', 'x']);
    for (let i = 0; i < 6; i++) await p.ctx.FC.lazy('/a/h/dash-sheet.js').catch(() => {});
    await settle();
    check('a lazy pack three times a page life, the script still runs', [n('/a/3/dash-sheet.pl.json'), p.ctx.SHEET.csv], [3, 'Export CSV']); }

  // ---------------------------------------------------------------- 2. the pseudo language xx
  const EN = JSON.parse(cp.execFileSync(process.env.PYTHON || 'python', ['-I', path.join(ROOT, 'tools', 'i18n_inventory.py'), '--keys'], { encoding: 'utf8', env: Object.assign({}, process.env, { FAUXCLOUD_NODE: process.execPath }) }));
  const br = s => '⟦' + s + '⟧';
  const XX = {};
  for (const [k, v] of Object.entries(EN)) { const [f, key] = [k.slice(0, k.indexOf('.')), k.slice(k.indexOf('.') + 1)];
    (XX[f] = XX[f] || {})[key] = typeof v === 'object' ? Object.fromEntries(Object.entries(v).map(([c, t]) => [c, br(t)])) : br(v); }
  const xxW = `const XX = ${JSON.stringify(XX)};\nObject.assign(FC, { w: (f, t) => { const p = XX[f] || {}, all = !Object.keys(t).length; for (const k in p) if (all || k in t) t[k] = typeof t[k] == 'object' && typeof p[k] == 'object' ? Object.assign({}, p[k], { one: p[k].one || p[k].other }) : p[k]; return t; },` +
    ` L: { dp: ',', ago: s => '⟦' + s + ' s⟧', mon: t => 'M' + t.getMonth(), day: t => 'D' + t.getDay() }, pr: n => n == 1 ? 'one' : 'other' });\n`;
  // what is left outside the brackets, without tags, entities, numbers, units, compass points, names and codes
  const UNIT = /^(°[CF]|km\/h|mph|mm|in|hPa|inHg|%|W\/m²|UV|h|min|s|d|km|mi|ft|m|dBm|KB)$/;
  const leftover = html => { let t = html.replace(/<title>/g, ' ').replace(/<[^>]*>/g, ' ').replace(/(?:aria-label|title)="([^"]*)"/g, ' $1 ').replace(/&\w+;/g, ' ');
    for (let u = ''; u !== t; ) { u = t; t = t.replace(/⟦[^⟦⟧]*⟧/g, ' '); }  // nested brackets: a slot's words inside a template's
    const words = t.split(/[\s·,.:;()/–—→+−…?!]+/).filter(w => /[A-Za-z]{2}/.test(w) && !UNIT.test(w) && !/^[A-Z0-9]{2,5}$/.test(w) && !/^(fauxcloud|GitHub|ST\d|NNE|ENE|ESE|SSE|SSW|WSW|WNW|NNW)$/.test(w));
    return words; };
  check('xx: the check bites', leftover('<b>Planted words</b> ⟦ok ⟦nested⟧ ok⟧ 12 km/h'), ['Planted', 'words']);
  const words = (name, html) => { const a = []; html.replace(/(?:aria-label|title)="([^"]*)"/g, (_, x) => a.push(x)); const w = leftover(html + ' ' + a.join(' '));
    check('xx: no English left in ' + name, w, []); };
  const esc = "const esc = s => String(s ?? '').replace(/[&<>\"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;' }[c]));\nconst own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;\n";
  const core = rd('core.js'), health = rd('health.js'), dash = rd('dash.js'), sheet = rd('dash-sheet.js');
  // the status row, the chips and the state line (text_vectors.js's pieces)
  { const els = {}, el = s => els[s] || (els[s] = { innerHTML: '', textContent: '', hidden: false });
    const S = { cur: 'ST1', names: {}, stale: true, liveAt: Date.now(), status: { sta: false, mqtt: 0, too_long: 1, no_slot: 1, no_slot_id: 'K1', ids: ['ST1', 'ST2'], st: 'os', metar: true,
      notices: [{ id: 'time_none', chip: 'Waiting for time', text: 'x' }, { id: 'store_fail', n: 3, chip: 'History not saved', text: 'x' }] },
      live: { time_synced: true, station: { id: 'ST1', online: false, uploads: 3, age_s: 400, last_seen: new Date(Date.now() - 400e3).toISOString(), values: { battery_low: 1, pressure_hpa: 1013 },
        notices: [{ id: 'sensor_stale', since: '2026-11-17T08:00:00Z', chip: 'Sensor not updating', text: 'x' }, { id: 'console_clock', skew_s: 4000, chip: 'Console clock off', text: 'x' },
          { id: 'baro_ref', d_hpa: -4.1, target_hpa: 1012.3, since: '2026-11-17T08:00:00Z', chip: 'Pressure looks off', text: 'x' }] } } };
    const ctx = { S, FC: { face: 'lan' }, IMP: false, $: el, dur: s => s + ' s', rejChip: t => `<span>${t}</span>`, apView() {}, icon: () => '', Object, String, Math, Array, JSON, Date };
    vm.createContext(ctx);
    vm.runInContext(esc + fcText(core) + xxW + decl(core, 'WC') + '\n' + decl(core, 'ago') + '\n' + ['p2', 'hm', 'dm', 'WH', 'dayTime', 'offSince', 'STIC', 'stSvg', 'WIFI', 'healthRow'].map(n => decl(health, n)).join('\n') + '\n' +
      ['dsep', 'nameOf', 'WD', 'NT', 'ap', 'noSlotChip', 'ntx', 'derWhy', 'NOTICE_TILE', 'tileNote', 'renderBar', 'stateLine'].map(n => decl(dash, n)).join('\n') + '\nthis.f = { renderBar, stateLine, healthRow };', ctx);
    ctx.f.renderBar(); words('the chips', el('#bridge').innerHTML + el('#stations').innerHTML);
    ctx.f.stateLine(); words('the state line', el('#stState').innerHTML);
    words('the status row', ctx.f.healthRow({ time_synced: false, time_src: 'persisted', diag: { sta_up: true, sta_rssi: -75, mqtt_up: false, heap_free: 4e4, heap_low: true } }));
    S.live.station.online = true; S.live.station.uploads = 0; S.live.station.restored = true; ctx.f.stateLine(); words('the state line, restored', el('#stState').innerHTML); }
  // the dial and the sheet's Now row, range row and rose words
  { const ctx = { U: { t: '°C', w: 'km/h', r: 'mm', p: 'hPa', h: '%' }, cv: (k, x) => x, IMP: false, Math, String, Array, Date, Set, FC: {} };
    vm.createContext(ctx);
    vm.runInContext(esc + fcText(core) + xxW + ['p2', 'hm', 'dm'].map(n => decl(health, n)).join('\n') + '\n' +
      'const trGl = t => "", FCT = { word: r => "⟦w⟧" }, M = { temp: { u: "°C", dec: 1 }, hum: { u: "%", dec: 0 }, baro: { u: "hPa", dec: 1 }, wind: { u: "km/h", dec: 1 }, rain: { u: "mm", dec: 1 }, sun: { u: "UV", dec: 0 }, indoor: { u: "°C", dec: 1 } };\n' +
      ['WD', 'toKmh', 'dsep', 'f1', 'f2', 'f0', 'fr', 'CALM_KMH', 'calm', 'maxOf', 'uvf', 'hh', 'CARD', 'card', 'feels', 'dlt', 'trV', 'dial'].map(n => decl(dash, n)).join('\n') + '\n' +
      ['WS', 'rowOf', 'ROSE', 'POINTS', 'roseBins', 'SUN_WM2', 'rangeStats', 'rangeSum', 'nowStats'].map(n => decl(sheet, n)).join('\n') + '\nthis.f = { dial, rangeSum, nowStats, rowOf };', ctx);
    const f = ctx.f, t = h => ({ t: new Date(2026, 10, 16, h), open: false, n: 60, rej: 0 });
    words('the dial', f.dial({ v: { wind_kmh: 0.4, wind_dir: 90 }, hr: [{}], hrMin: 20 }) + f.dial({ v: { wind_kmh: 12, wind_dir: 90 }, hr: [{}], hrMin: 60 }));
    const R = [Object.assign(t(1), { temp_c: [1, 2, 3], dewpt_c: [0, 1, 2], rain_mm: 0.4, wind_kmh: [1, 5, 9], gust_kmh: 12, wind_dir: 200 }), Object.assign(t(2), { temp_c: [1, 3, 4], dewpt_c: [0, 1, 2], rain_mm: 1.2, wind_kmh: [2, 6, 9], gust_kmh: 14, wind_dir: 210 })];
    for (const [meas, k] of [['temp', 'temp_c'], ['rain', 'rain_mm'], ['wind', 'wind_kmh'], ['baro', 'pressure_hpa']])
      words('the range row: ' + meas, f.rangeSum('today', {}, R.map(x => f.rowOf(x, k)), [], meas, k, new Set(), []));
    const v = { temp_c: 3, dewpt_c: 1, windchill_c: 0, wind_kmh: 0.2, wind_dir: 100, gust_10m_kmh: 9, rain_daily_mm: 1, rain_mm: 0.2, humidity: 80, pressure_hpa: 1010, pressure_abs_hpa: 990, uv: 2, solar_wm2: 100, indoor_humidity: 40 };
    const m = { days: [], recent: [], rej: ['solar_wm2'], hid: new Set() };
    for (const [meas, k] of [['temp', 'temp_c'], ['rain', 'rain_daily_mm'], ['wind', 'wind_kmh'], ['baro', 'pressure_hpa'], ['sun', 'uv'], ['hum', 'humidity']])
      words('the Now row: ' + meas, f.nowStats(meas, v, m, k, null, f => String(f), () => '1').map(x => x[0] + ' ' + x[1]).join(' ')); }
  // the outlook's lines and its explanation (both faces' words)
  { const ctx = { console, Object, String, Math, Array, JSON, Date, FC: {} };
    vm.createContext(ctx);
    vm.runInContext(esc + fcText(core) + xxW + 'const LUCIDE = {};\n' + ['p2', 'hm', 'dm', 'WH', 'dayTime'].map(n => decl(health, n)).join('\n') + '\n' + rd('outlook.js') + '\nFC.w("outlook", FCO.W);\n' + rd('outlook-why.js') + '\nthis.FCO = FCO; this.FCW = FCW;', ctx);
    const o = { v: 1, state: 'rain_light', next: 'p_falling', warn: 'icy', qual: 'windy', tend: { d: -2, tide: true }, p: { since: '2026-11-17T08:00:00Z' }, in: { t: 0.5, td: -1, rate: 1.2, rain12: 0.4, wind10: 25, night: true, night_src: 'sun', learn: ['l2'], dt_h: -1.2, calm: true } };
    const lines = ctx.FCO.lines(o, { gust: g => g + ' km/h', range: (a, b) => a + '–' + b + ' °C' });
    words('the strip', [lines.l1, lines.l2, lines.warn].join(' '));
    words('the explanation', ctx.FCW.why(o, ctx.FCW.M) + ctx.FCW.why(Object.assign({}, o, { state: 'night_clear', qual: 'calm', warn: 'hoar_frost', next: 'collecting' }), ctx.FCW.M));
    words('the admin explanation', ctx.FCW.admin(o, ctx.FCW.M) + ctx.FCW.admin(null, ctx.FCW.M));
    words('the notice sheet', ctx.FCW.note({ id: 'baro_ref', d_hpa: 4, target_hpa: 1010, since: '2026-11-17T08:00:00Z' }, 1014, null, Object.assign({ pa: v => v + ' hPa' }, ctx.FCW.M))
      + ctx.FCW.note({ id: 'baro_uncalibrated', last_hpa: 1010, target_hpa: 1009, last_t: '2026-11-17T08:00:00Z' }, 1014, 1012, Object.assign({ pa: v => v + ' hPa' }, ctx.FCW.M))); }

  // ---------------------------------------------------------------- 3. the Polish count entries
  const pl = JSON.parse(fs.readFileSync(path.join(WEB, 'lang', 'pl.json'), 'utf8')), pr = new Intl.PluralRules('pl');
  let counts = 0;
  for (const [k, e] of Object.entries(pl)) {
    if (k === '_meta' || !e.pl || typeof e.pl !== 'object') continue;
    counts++;
    for (const n of [0, 1, 2, 5, 12, 21, 22, 25, 101, 1.5]) {
      const f = e.pl[pr.select(n)] ?? e.pl.other;
      check(`pl ${k} n=${n}`, typeof f === 'string' && f.length > 0 && !/\{(?!\w+\})/.test(f), true);
    }
  }
  check('pl: count entries present', counts > 0, true);

  console.log(fail ? `FAILED: ${pass} passed, ${fail} failed` : `OK: ${pass} passed, 0 failed`);
  process.exit(fail ? 1 : 0);
})();
