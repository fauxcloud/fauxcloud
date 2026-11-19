// test/tools/sheet_vectors.js -- the history sheet's Now row and range summary: nowStats(), rangeSum(), rangeStats() and
// roseBins() taken out of firmware/fauxcloud/web/dash-sheet.js by name (the js_decl.js pattern). The records are written in
// /api/hourly and /api/daily's shapes (docs/api.md: hourly temp_c / humidity [min,avg,max], dewpt_c / pressure_hpa / wind_kmh
// [null,avg,null], uv [null,null,max], solar_wm2 [null,avg,max], gust_kmh / rain_mm / wind_dir single numbers; daily adds
// dewpt_c / pressure_hpa triples, rose, calm_hours) and turned into sheet rows by the page's own rowOf. Every sheet x
// {Today hours, Yesterday hours, a From-To span of hours, of days}: empty, one record, a gap, weighting by n, lowest / highest,
// calm, a tie, rain 0, the day word, solar hidden / rejected / zero / day records, hidden fields, imperial; the range row's
// records, head word and aria per range kind, none on "Now"; the Now row's current values only; the chart's lowest / highest
// labels (extOf, then chart() itself: x, anchor, height above the average line, none on "Now", on bars or in the running hour); the
// hour and day CSV files (recCsv: only the columns a record keeps, the compass beside the degrees, a day's rose fallback).
// Usage: node test/tools/sheet_vectors.js
'use strict';
process.env.TZ = 'Europe/Paris';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { decl, fcText } = require('./js_decl');
const WEB = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web');
const rd = f => fs.readFileSync(path.join(WEB, f), 'utf8');
const dash = rd('dash.js'), sheet = rd('dash-sheet.js'), health = rd('health.js'), core = rd('core.js');

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const ctx = { Math, Date, String, Array, JSON, Number, isNaN, Set };
vm.createContext(ctx);
vm.runInContext('let IMP = false; const U = { t: "°C", w: "km/h", r: "mm", p: "hPa", h: "%" };\n'
  + 'const M = { temp: { u: "°C", dec: 1 }, hum: { u: "%", dec: 0 }, baro: { u: "hPa", dec: 1 }, wind: { u: "km/h", dec: 1 }, rain: { u: "mm", dec: 1 }, sun: { u: "UV", dec: 0 }, indoor: { u: "°C", dec: 1 } };\n'
  // stubs: esc (a regex literal), the trend glyph (an SVG), the outlook words, the unit table
  + 'const esc = s => String(s ?? "").replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", \'"\': "&quot;" }[c]));\n'
  + 'const trGl = t => `<span class="tri">^</span>`, FCT = { word: r => r.w }, FC = { UNITS: {} };\n' + fcText(core)
  + ['p2', 'dm'].map(n => decl(health, n)).join('\n') + '\n'
  + ['WD', 'toKmh', 'dsep', 'f1', 'f2', 'f0', 'fr', 'CALM_KMH', 'maxOf', 'uvf', 'hh', 'CARD', 'card', 'calm', 'feels', 'dlt', 'trV'].map(n => decl(dash, n)).join('\n') + '\n'
  + ['WS', 'rowOf', 'ROSE', 'POINTS', 'roseBins', 'SUN_WM2', 'rangeStats', 'rangeSum', 'nowStats'].map(n => decl(sheet, n)).join('\n') + '\n'
  // the chart: its own code with the page's geometry; $ answers the sheet body's width
  + 'let measure = "temp", range = "today", SPZ = null, BODYW = 680; const $ = () => ({ clientWidth: BODYW });\n'
  + ['HOUR', 'BEAU', 'cvv'].map(n => decl(dash, n)).concat(decl(health, 'hm')).join('\n') + '\n'
  + ['addD', 'ymd', 'spanTicks', 'BFT', 'extOf', 'chart', 'CSV_NM', 'csvNum', 'CSV_MAM', 'recCsv'].map(n => decl(sheet, n)).join('\n')
  + '\nthis.S = { rangeStats, rangeSum, nowStats, roseBins, rowOf, f1, f0, extOf, recCsv, imp: i => { IMP = !!i; },'
  + ' draw: (m, rg, R, fx, w = 680) => { measure = m; range = rg; BODYW = w; return chart(R, fx); } };', ctx);
const { rangeStats: RS, rangeSum, nowStats, roseBins, rowOf, f1, f0, extOf, recCsv, draw, imp } = ctx.S;
const NOHID = new Set(), H = (...f) => new Set(f);
// API records (after the page's unit conversion): an hour of 16 Nov 2026 local, or a day of Nov 2026
const T = (d, h = 0) => new Date(2026, 10, d, h);
const hr = (h, o, d = 16) => Object.assign({ t: T(d, h), open: false, n: 60, rej: 0 }, o);
const day = (d, o) => Object.assign({ t: T(d), open: false, n: 1440, rej: 0 }, o);
const rows = (k, A) => A.map(x => rowOf(x, k));
const strip = s => s.replace(/<small>/g, ' ').replace(/<\/small>/g, '');
let RG = 'today';
const run = (meas, k, A, days = false, hid = NOHID, rej = []) => RS(meas, rows(k, A), k, days, hid, rej, RG).map(([v, l]) => [strip(v), l]);

// ---- empty and one record
for (const [m, k] of [['temp', 'temp_c'], ['hum', 'humidity'], ['baro', 'pressure_hpa'], ['wind', 'wind_kmh'], ['rain', 'rain_mm'], ['sun', 'uv']]) check('empty range: no figures, ' + m, run(m, k, []), []);
check('one record: temp', run('temp', 'temp_c', [hr(9, { temp_c: [4, 5, 6], dewpt_c: [null, 2, null] })]),
  [['5.0 °C', 'average'], ['4.0 / 6.0 °C', 'lowest / highest'], ['2.0 °C', 'dew point average']]);
check('one record: an hour of pressure has its average only (mid stands in), no change', run('baro', 'pressure_hpa', [hr(9, { pressure_hpa: [null, 1010.4, null] })]),
  [['1010.4 / 1010.4 hPa', 'lowest / highest']]);

// ---- temperature: weighting by n, lowest / highest from the records' min / max (never the averages), a gap, the open hour
const TD = [hr(0, { n: 60, temp_c: [1, 2, 3], dewpt_c: [null, 0, null] }), hr(1, { n: 20, temp_c: [3, 6, 9], dewpt_c: [null, 2, null] }), /* 02:00 missing (a gap) */
  hr(3, { n: 60, temp_c: [-1.5, 0, 1], dewpt_c: [null, -2, null], open: true })];
check('temp: weighted average, lo / hi from the records\' min / max', run('temp', 'temp_c', TD),
  [['1.7 °C', 'average'], ['−1.5 / 9.0 °C', 'lowest / highest'], ['−0.6 °C', 'dew point average']]);  // weighted: (2·60 + 6·20 + 0·60) / 140; a plain average would be 2.7
check('temp: dew point hidden', run('temp', 'temp_c', TD, false, H('dewpt_c')).map(x => x[1]), ['average', 'lowest / highest']);
check('temp: a headline other than temp_c has no dew point line', run('temp', 'feels_c', [hr(0, { feels_c: [1, 2, 3], dewpt_c: [null, 0, null] })]).map(x => x[1]), ['average', 'lowest / highest']);
check('temp, day records', run('temp', 'temp_c', [day(10, { temp_c: [-2, 3, 8], dewpt_c: [-4, 0, 2] }), day(11, { temp_c: [1, 4, 9], dewpt_c: [-1, 1, 3] })], true),
  [['3.5 °C', 'average'], ['−2.0 / 9.0 °C', 'lowest / highest'], ['0.5 °C', 'dew point average']]);

// ---- humidity and pressure
check('hum', run('hum', 'humidity', [hr(0, { humidity: [80, 85, 90] }), hr(1, { humidity: [70, 75, 88] })]), [['80 %', 'average'], ['70 / 90 %', 'lowest / highest']]);
const P = [hr(0, { pressure_hpa: [null, 1012.3, null] }), hr(1, { pressure_hpa: null }), hr(2, { pressure_hpa: [null, 1010.1, null] })];
check('baro, hours: lowest / highest of the hourly means, change = last minus first record with a value', run('baro', 'pressure_hpa', P),
  [['1010.1 / 1012.3 hPa', 'lowest / highest'], ['−2.2 hPa', 'change']]);
check('baro, day records: lowest / highest from the days\' min / max', run('baro', 'pressure_hpa', [day(10, { pressure_hpa: [1008, 1010, 1013] }), day(11, { pressure_hpa: [1005, 1007, 1009] })], true),
  [['1005.0 / 1013.0 hPa', 'lowest / highest'], ['−3.0 hPa', 'change']]);
const ch = (a, b) => run('baro', 'pressure_hpa', [hr(0, { pressure_hpa: [null, a, null] }), hr(1, { pressure_hpa: [null, b, null] })])[1][0];
check('baro: rising, level, and a change that rounds to zero carries no sign', [ch(1000, 1003.25), ch(1000, 1000), ch(1000, 1000.04), ch(1000, 999.96)], ['+3.3 hPa', '±0.0 hPa', '±0.0 hPa', '±0.0 hPa']);

// ---- wind: average, strongest gust (the hours' maxima), mostly from (the rose's own bins), calm, a tie, hidden fields
const WD = [hr(0, { wind_kmh: [null, 10, null], gust_kmh: 22, wind_dir: 225 }), hr(1, { wind_kmh: [null, 14, null], gust_kmh: 31, wind_dir: 230 }),
  hr(2, { wind_kmh: [null, 8, null], gust_kmh: 18, wind_dir: 90 })];  // SW twice, E once (the rose's 8 points)
check('wind: average, strongest gust, mostly from', run('wind', 'wind_kmh', WD), [['10.7 km/h', 'average'], ['31.0 km/h', 'strongest gust'], ['SW', 'mostly from']]);
check('wind: mostly from = the rose\'s winner (one binning)', (b => b.tot.indexOf(Math.max(...b.tot)))(roseBins(rows('wind_kmh', WD), false)), 5);
check('wind: all calm -> no "mostly from"', run('wind', 'wind_kmh', [hr(0, { wind_kmh: [null, .4, null], gust_kmh: 3, wind_dir: 250 })]).map(x => x[1]), ['average', 'strongest gust']);
check('wind: a tie -> the first compass point', run('wind', 'wind_kmh', [hr(0, { wind_kmh: [null, 9, null], wind_dir: 180 }), hr(1, { wind_kmh: [null, 9, null], wind_dir: 0 })])[1], ['N', 'mostly from']);
check('wind: gust and direction hidden', run('wind', 'wind_kmh', WD, false, H('gust_kmh', 'wind_dir')).map(x => x[1]), ['average']);
// the gust as the headline (wind speed hidden): an hour's gust_kmh is its maximum, so there is no average gust
check('wind: gust as the headline -> the strongest only, no average of maxima', run('wind', 'gust_kmh', WD, false, H('wind_kmh')), [['31.0 km/h', 'strongest gust'], ['SW', 'mostly from']]);
check('wind, day records: the rose\'s hours per sector, the days\' gusts', run('wind', 'wind_kmh', [day(10, { wind_kmh: [null, 12, null], gust_kmh: 40, wind_dir: 260, rose: [0, 0, 0, 0, 0, 3, 9, 2], calm_hours: 1 }),
  day(11, { wind_kmh: [null, 8, null], gust_kmh: 25, wind_dir: 225, rose: [0, 0, 0, 0, 0, 6, 1, 0], calm_hours: 4 })], true), [['10.0 km/h', 'average'], ['40.0 km/h', 'strongest gust'], ['W', 'mostly from']]);

// ---- rain: total, 0.0 is shown without "wettest", the wettest hour / day with its time or day word
const RN = [hr(0, { rain_mm: .2 }), hr(1, { rain_mm: 1.4 }), hr(2, { rain_mm: 0 })];
check('rain today: total, wettest hour', run('rain', 'rain_mm', RN), [['1.6 mm', 'total'], ['1.4 mm', 'wettest hour, 01:00']]);
check('rain 0.0: total, no wettest', run('rain', 'rain_mm', [hr(0, { rain_mm: 0 }), hr(1, { rain_mm: 0 })]), [['0.0 mm', 'total']]);
RG = 'days';
check('rain, a From-To span of hours: the wettest hour carries its day', run('rain', 'rain_mm', [hr(23, { rain_mm: .4 }, 14), hr(1, { rain_mm: 1.4 }, 15)])[1], ['1.4 mm', 'wettest hour, 15 Nov 01:00']);
check('rain, day records: the wettest day', run('rain', 'rain_mm', [day(10, { rain_mm: 3 }), day(12, { rain_mm: 7.5 })], true), [['10.5 mm', 'total'], ['7.5 mm', 'wettest day, 12 Nov']]);
RG = 'yday';
check('rain yesterday: the hour without a day word', run('rain', 'rain_mm', RN)[1], ['1.4 mm', 'wettest hour, 01:00']);

// ---- UV and sun: highest; hours of sun only with light readings, hourly only, not hidden or rejected
RG = 'today';
const SN = [hr(9, { uv: [null, null, 1], solar_wm2: [null, 90, 150] }), hr(11, { uv: [null, null, 3], solar_wm2: [null, 260, 400] }), hr(12, { uv: [null, null, 2], solar_wm2: [null, 140, 300] })];
check('sun: UV highest, 2 h of sun (hourly means > 120 W/m²)', run('sun', 'uv', SN), [['3 UV', 'highest'], ['2 h', 'of sun']]);
check('sun: light readings present, none above 120 -> 0 h', run('sun', 'uv', [hr(9, { uv: [null, null, 1], solar_wm2: [null, 60, 80] })])[1], ['0 h', 'of sun']);
check('sun: no light readings -> no sun hours (unknown is not zero)', run('sun', 'uv', [hr(9, { uv: [null, null, 1] })]).map(x => x[1]), ['highest']);
check('sun: solar hidden or rejected -> no sun hours', [run('sun', 'uv', SN, false, H('solar_wm2')).length, run('sun', 'uv', SN, false, NOHID, ['solar_wm2']).length], [1, 1]);
check('sun: day records -> no sun hours', run('sun', 'uv', [day(10, { uv: [null, null, 4], solar_wm2: [null, 200, 700] })], true), [['4 UV', 'highest']]);
check('sun: solar as the headline', run('sun', 'solar_wm2', [hr(11, { solar_wm2: [null, 180, 420] }), hr(12, { solar_wm2: [null, 90, 200] })]), [['420 W/m²', 'highest'], ['1 h', 'of sun']]);
check('sun: UV with a decimal keeps it', run('sun', 'uv', [hr(11, { uv: [null, null, 2.5], solar_wm2: [null, 50, 60] })])[0], ['2.5 UV', 'highest']);

// ---- the range row: its records, head word and aria per range kind; none on "Now" or for indoor
const sum = (rg, z, all, raw, meas, k, hid = NOHID) => rangeSum(rg, z, rows(k, all), rows(k, raw), meas, k, hid, []);
const head = s => (s.match(/aria-label="([^"]*)"/) || [])[1], labs = s => [...s.matchAll(/<span class="k">([^<]*)<\/span>/g)].map(x => x[1]);
const OTHER = [hr(5, { rain_mm: 9 })];  // what the other source holds: never read for that range
check('range row: none on "Now"', sum('now', {}, RN, RN, 'rain', 'rain_mm'), '');
check('range row: none for indoor', sum('today', {}, TD, TD, 'indoor', 'temp_c'), '');
check('range row: none for an empty range', sum('today', {}, [], OTHER, 'rain', 'rain_mm'), '');
check('range row, Today: the day\'s hour rows, head and aria', [head(sum('today', {}, RN, OTHER, 'rain', 'rain_mm')), labs(sum('today', {}, RN, OTHER, 'rain', 'rain_mm'))],
  ['Today summary', ['total', 'wettest hour, 01:00']]);
check('range row, Yesterday', [head(sum('yday', {}, RN, OTHER, 'rain', 'rain_mm')), sum('yday', {}, RN, OTHER, 'rain', 'rain_mm').includes('<p class="rsh">Yesterday</p>')], ['Yesterday summary', true]);
const z = (a, b, step) => ({ a: T(a), b: T(b), step });
check('range row, From-To of hours (step 1): RAW, the dates as the head, the hour with its day', [head(sum('days', z(14, 16, 1), OTHER, RN, 'rain', 'rain_mm')), labs(sum('days', z(14, 16, 1), OTHER, RN, 'rain', 'rain_mm'))],
  ['14 Nov–16 Nov summary', ['total', 'wettest hour, 16 Nov 01:00']]);
check('range row, From-To of hours in 6-h buckets (step 6): still the hours (sun hours counted)', labs(sum('days', z(10, 16, 6), OTHER, SN, 'sun', 'uv')), ['highest', 'of sun']);
for (const st of [24, 168]) check(`range row, From-To of days (step ${st}, also the day records standing in for hours): days, no sun hours`,
  labs(sum('days', z(1, 30, st), OTHER, [day(10, { uv: [null, null, 4], solar_wm2: [null, 200, 700] })], 'sun', 'uv')), ['highest']);
check('range row, a single day', head(sum('days', z(12, 12, 1), OTHER, RN, 'rain', 'rain_mm')), '12 Nov summary');

// ---- the Now row: the station's current values only, each with its unit; no range figure
const mod = (o = {}) => Object.assign({ hid: NOHID, rej: [], recent: [], days: [] }, o);
const now = (meas, v, k, m = mod(), tr = null) => nowStats(meas, v, m, k, tr, f1, (w, f) => w === 'lo' ? '1.0' : '9.0').map(([a, b]) => [strip(a), b]);
const NOW_ONLY = ['lowest / highest', 'strongest gust', 'average', 'total', 'highest'];
const trd = { w: 'rising', code: 'rising', d: .6 };
check('Now, temp: reading, trend, dew point, feels like (its own tag: shown beside the temperature headline)',
  now('temp', { temp_c: 7.2, dewpt_c: 3.1, windchill_c: 4.0 }, 'temp_c', mod(), { temp: [trd, 't', x => x.toFixed(1), 3] }),
  [['7.2 °C', 'reading'], ['+0.6 °C <span class="tri in">^</span>', 'in 3 h, rising'], ['3.1 °C', 'dew point'], ['4.0 °C', 'feels like']]);
check('Now, temp: dew point hidden, feels like within 0.5 °C of the temperature -> neither',
  now('temp', { temp_c: 7.2, dewpt_c: 3.1, windchill_c: 7.0 }, 'temp_c', mod({ hid: H('dewpt_c') })), [['7.2 °C', 'reading']]);
check('Now, temp: feels like goes with the temperature hidden', now('temp', { temp_c: 7.2, windchill_c: 4.0, dewpt_c: 3.1 }, 'dewpt_c', mod({ hid: H('temp_c') })).map(x => x[1]), ['reading']);
check('Now, wind: reading, from, the 10-min gust', now('wind', { wind_kmh: 12, wind_dir: 250, gust_10m_kmh: 30.4 }, 'wind_kmh'),
  [['12.0 km/h', 'reading'], ['WSW 250°', 'from'], ['30.4 km/h', 'gust, last 10 min']]);
check('Now, wind: calm', now('wind', { wind_kmh: .3, wind_dir: 250 }, 'wind_kmh'), [['0.3 km/h', 'reading'], ['Calm', 'direction']]);
check('Now, wind: the gust as the headline', now('wind', { gust_kmh: 20, wind_dir: 90 }, 'gust_kmh', mod({ hid: H('wind_kmh') })).map(x => x[1]), ['gust', 'from']);
check('Now, rain: the console\'s totals', now('rain', { rain_daily_mm: 1.2, rain_mm: .3, rain_weekly_mm: 5, rain_monthly_mm: 20, rain_yearly_mm: 300 }, 'rain_mm').map(x => x[1]),
  ['today', 'last hour', 'this week', 'this month', 'this year']);
check('Now, rain: no console totals -> the last 7 days of our day records', now('rain', { rain_daily_mm: 1.2, rain_mm: .3 }, 'rain_mm', mod({ days: [{ rain_mm: 1 }, { rain_mm: 2 }] }))[2], ['3.0 mm', 'last 7 days']);
check('Now, sun: the UV reading, the light', now('sun', { uv: 3, solar_wm2: 250 }, 'uv'), [['3 UV', 'reading'], ['250 W/m²', 'light']]);
check('Now, sun: a rejected light reading keeps its name', now('sun', { uv: 3, solar_wm2: 250 }, 'uv', mod({ rej: ['solar_wm2'] })), [['3 UV', 'reading'], ['rejected', 'light']]);
check('Now, pressure: reading, trend, absolute', now('baro', { pressure_hpa: 1012.4, pressure_abs_hpa: 990.1 }, 'pressure_hpa', mod(), { baro: [trd, 'p', x => x.toFixed(1), 3] }).map(x => x[1]),
  ['reading', 'in 3 h, rising', 'absolute']);
check('Now, humidity', now('hum', { humidity: 81 }, 'humidity'), [['81 %', 'reading']]);
check('Now: no range figure on any tile sheet', ['temp', 'wind', 'rain', 'sun', 'baro', 'hum'].flatMap(m => now(m, { temp_c: 7, dewpt_c: 3, wind_kmh: 9, wind_dir: 90, gust_kmh: 20, rain_daily_mm: 1,
  rain_mm: 0, uv: 2, solar_wm2: 300, pressure_hpa: 1010, humidity: 80 }, { temp: 'temp_c', wind: 'wind_kmh', rain: 'rain_mm', sun: 'uv', baro: 'pressure_hpa', hum: 'humidity' }[m])).filter(x => NOW_ONLY.includes(x[1])), []);
check('Now, indoor keeps its own lowest / highest', now('indoor', { indoor_temp_c: 21.5, indoor_humidity: 45 }, 'indoor_temp_c').map(x => x[1]), ['reading', 'humidity', 'lowest / highest']);

// ---- the chart's lowest / highest labels (the tile's rules): extOf on [x % of the plot, value, index], then the chart itself
const pts = A => A.map(([x, v], i) => [x, v, i]), ex = (A, end = false) => extOf(pts(A), end, f1).map(([p, a]) => [p[2], a]);
check('labels: fewer than 2 points -> none', [ex([]), ex([[50, 3]])], [[], []]);
check('labels: highest then lowest, each grows inwards near an edge', ex([[10, 1], [50, 9], [90, 4]]), [[1, 'middle'], [0, 'start']]);
check('labels: near the right edge -> anchored at its end', ex([[30, 1], [60, 2], [85, 9]]), [[2, 'end'], [0, 'middle']]);
check('labels: the lowest within 12 % of the highest -> the highest only', ex([[40, 9], [51, 1], [90, 5]]), [[0, 'middle']]);
check('labels: the lowest shown as the same number -> the highest only', ex([[10, 5.04], [50, 5.01]]), [[0, 'start']]);
check('labels: a flat line -> the highest only (the first of a tie)', ex([[10, 3], [50, 3], [90, 3]]), [[0, 'start']]);
check('labels: within 8 % of the running hour -> none there (the headline shows it); the other stays', ex([[10, 1], [50, 4], [95, 9]], true), [[0, 'start']]);
check('labels: the same series without a running hour (Yesterday) labels its last point', ex([[10, 1], [50, 4], [95, 9]]), [[2, 'end'], [0, 'start']]);
// the chart: Today's hours 00-13 (13:00 running), the lowest at 03:00, the highest at 10:00; x = 44 + 626 / 24 * (h + .5) at 680 px
const TT = [2, 1, 0, -1.5, 0, 1, 2, 4, 6, 7.5, 8.2, 8, 7.4, 7].map((v, h) => rowOf(hr(h, { temp_c: [v - 1, v, v + 1], open: h === 13 }), 'temp_c'));
const labels = svg => [...svg.matchAll(/<text x="([\d.]+)" y="([\d.-]+)" text-anchor="(\w+)" style="fill:var\(--ink-2\)[^"]*">([^<]*) <tspan style="font-size:9px">([^<]*)<\/tspan><\/text>/g)]
  .map(m => [+m[1], +m[2], m[3], m[4], m[5]]);
// the average line: the polyline with round joins and caps; y of its point at x
const lineY = (svg, x) => { const l = (/<polyline points="([^"]*)"[^>]*stroke-linejoin="round" stroke-linecap="round"/.exec(svg) || [])[1] || '', p = l.split(' ').find(q => +q.split(',')[0] === x);
  return p ? +p.split(',')[1] : null; };
const today = draw('temp', 'today', TT, f1).svg, LT = labels(today);
check('chart, Today temp: the highest (10:00) and the lowest (03:00) with the unit, their x and anchors', LT.map(l => [l[0], l[2], l[3], l[4]]),
  [[317.9, 'middle', '8.2', '°C'], [135.3, 'start', '−1.5', '°C']]);
check('chart, Today temp: each label 6 px above its point on the average line', LT.map(l => +(lineY(today, l[0]) - l[1]).toFixed(1)), [6, 6]);
check('chart, a phone width (375 px body -> 375 px viewBox): the same points', labels(draw('temp', 'today', TT, f1, 375).svg).map(l => [l[2], l[3]]), [['middle', '8.2'], ['start', '−1.5']]);
const TH = TT.map((r, i) => Object.assign({}, r, { mid: i === 13 ? 9 : r.mid }));
check('chart, Today: the highest in the running hour -> no label there, the lowest stays', labels(draw('temp', 'today', TH, f1).svg).map(l => l[3]), ['−1.5']);
check('chart, Yesterday: the same series labels its last hour (no running hour)', labels(draw('temp', 'yday', TH.map(r => Object.assign({}, r, { open: false })), f1).svg).map(l => [l[2], l[3]]),
  [['middle', '9.0'], ['start', '−1.5']]);
check('chart, "Now": no labels (live readings)', labels(draw('temp', 'now', TT.map((r, i) => Object.assign({}, r, { t: new Date(2026, 10, 16, 13, i) })), f1).svg), []);
check('chart, one point: no labels', labels(draw('temp', 'today', TT.slice(0, 1), f1).svg), []);
check('chart, rain bars: no labels', labels(draw('rain', 'today', [.2, 1.4, 0].map((v, h) => rowOf(hr(h, { rain_mm: v }), 'rain_mm')), f1).svg), []);
const WW = [8, 14, 9, 7, 6, 5, 3, 10].map((v, h) => rowOf(hr(h, { wind_kmh: [null, v, null], gust_kmh: v * 2, wind_dir: 250 }), 'wind_kmh')), wsv = draw('wind', 'yday', WW, f1).svg, LW = labels(wsv);
check('chart, wind: the speed line, 11 px above its point (clear of the arrow)', [LW.map(l => [l[3], l[4]]), LW.map(l => +(lineY(wsv, l[0]) - l[1]).toFixed(1))], [[['14.0', 'km/h'], ['3.0', 'km/h']], [11, 11]]);
check('chart, the sheet\'s formatter: humidity whole numbers', labels(draw('hum', 'yday', [80, 92.4, 71].map((v, h) => rowOf(hr(h * 8, { humidity: [v - 2, v, v + 2] }), 'humidity')), f0).svg).map(l => [l[3], l[4]]),
  [['92', '%'], ['71', '%']]);
// light (the sun sheet's line) follows the tile's bars: the highest alone and only above 0, never a night's 0
check('labels, hiOnly: the highest alone; nothing when all 0', [extOf(pts([[10, 0], [50, 420], [90, 0]]), false, f0, true).map(([p, a]) => [p[2], a]),
  extOf(pts([[10, 0], [50, 0]]), false, f0, true)], [[[1, 'middle']], []]);
const SUN = [0, 0, 0, 0, 0, 0, 0, 12, 80, 210, 380, 420, 300, 150, 40, 0].map((v, h) => rowOf(hr(h, { uv: [null, null, 1], solar_wm2: [null, v, v + 20] }), 'solar_wm2'));
check('chart, the sun sheet: the highest only (no "0 W/m²" before sunrise)', labels(draw('sun', 'yday', SUN, f0).svg).map(l => l[3]), ['420']);
check('chart, the sun sheet at night: no label', labels(draw('sun', 'yday', SUN.map(r => Object.assign({}, r, { mid: 0 })), f0).svg), []);
// a point at the plot's top (humidity at its 100 % cap): the label flips below the point instead of over the legend
const hsv = draw('hum', 'yday', [80, 100, 71, 75, 78, 82].map((v, h) => rowOf(hr(h, { humidity: [v - 2, v, v] }), 'humidity')), f0).svg, LH = labels(hsv)[0];
check('chart, a label at the plot\'s top: below its point', [LH[3], +(LH[1] - lineY(hsv, LH[0])).toFixed(1)], ['100', 14]);
// `end` is P's last point: a running hour without a value yet leaves the last hour with a value labelled
const TO = TT.concat(rowOf(hr(14, { temp_c: null, open: true }), 'temp_c')).map((r, i) => Object.assign({}, r, { open: i === 14, mid: i === 13 ? 9 : r.mid }));
check('chart, the running hour without a value: the last value is labelled (not "now")', labels(draw('temp', 'today', TO, f1).svg).map(l => l[3]), ['9.0', '−1.5']);

// ---- Export CSV of hour and day records (API shapes, as src/web_render.cpp hourRow / dayRow write them): only the columns the
// record keeps, the compass beside the degrees, a day without a steady direction falls back to its rose's leading sector
const ALL = () => true;
const HOURLY_HEAD = ['hour', 'hour_utc', 'temperature min (°C)', 'temperature avg (°C)', 'temperature max (°C)', 'dew point avg (°C)',
  'humidity min (%)', 'humidity avg (%)', 'humidity max (%)', 'pressure avg (hPa)', 'wind avg (km/h)', 'UV index max', 'solar avg (W/m²)', 'solar max (W/m²)',
  'rain (mm)', 'rain rate max (mm/h)', 'gust max (km/h)', 'wind direction (°)', 'wind direction (compass)', 'samples', 'rejected', 'status'];
const DAILY_HEAD = ['date', 'temperature min (°C)', 'temperature avg (°C)', 'temperature max (°C)', 'dew point min (°C)', 'dew point avg (°C)', 'dew point max (°C)',
  'humidity min (%)', 'humidity avg (%)', 'humidity max (%)', 'pressure min (hPa)', 'pressure avg (hPa)', 'pressure max (hPa)', 'wind avg (km/h)', 'UV index max',
  'solar avg (W/m²)', 'solar max (W/m²)', 'rain (mm)', 'rain rate max (mm/h)', 'gust max (km/h)', 'wind direction (°)', 'wind direction (compass)', 'calm hours',
  'samples', 'rejected', 'hours from N', 'hours from NE', 'hours from E', 'hours from SE', 'hours from S', 'hours from SW', 'hours from W', 'hours from NW', 'status'];
const HREC = { h: '2026-11-16T09:00Z', n: 55, rej_fields: 0, temp_c: [4, 5, 6], dewpt_c: [null, 2, null], humidity: [80, 85, 90], pressure_hpa: [null, 1012.3, null],
  wind_kmh: [null, 7.5, null], gust_kmh: 18.3, wind_dir: 250, rain_mm: .2, rain_rate_max: 1.5, uv: [null, null, 1.2], solar_wm2: [null, 90, 150] };
const [hh0, hl0] = recCsv([HREC], false, ALL, null);
check('CSV hourly head: only what an hour record keeps (no dew point / pressure / wind min-max, no UV min-avg, no solar min)', hh0, HOURLY_HEAD);
check('CSV hourly row: one cell per column, the compass beside the degrees', [hl0[0].split(',').length, hl0[0].split(',').slice(2)],
  [HOURLY_HEAD.length, ['4', '5', '6', '2', '80', '85', '90', '1012.3', '7.5', '1.2', '90', '150', '0.2', '1.5', '18.3', '250', 'WSW', '55', '0', 'closed']]);
check('CSV hourly: no direction -> both direction cells empty (no rose fallback for an hour)', recCsv([Object.assign({}, HREC, { wind_dir: null })], false, ALL, null)[1][0].split(',').slice(17, 19), ['', '']);
const DREC = (o = {}) => Object.assign({ d: '2026-11-12', n: 1330, rej_fields: 0, temp_c: [0.7, 5.2, 8.9], dewpt_c: [-0.2, 2.9, 5.3], humidity: [77, 86, 91],
  pressure_hpa: [1008.6, 1015.2, 1020], wind_kmh: [null, 5.9, null], gust_kmh: 18.3, wind_dir: 284, calm_hours: 0, rose: [0, 0, 0, 0, 0, 0, 16, 8], rain_mm: 8.32,
  rain_rate_max: 3, uv: [null, null, 0.8], solar_wm2: [null, 40, 295] }, o);
const dRow = r => { const [h, l] = recCsv([r], true, ALL, null), c = l[0].split(','); return [c[h.indexOf('wind direction (°)')], c[h.indexOf('wind direction (compass)')]]; };
check('CSV daily head: only what a day record keeps (no wind min-max, no UV min-avg, no solar min)', recCsv([DREC()], true, ALL, null)[0], DAILY_HEAD);
check('CSV daily, a steady direction: its degrees and 16-point compass', dRow(DREC()), ['284', 'WNW']);
check('CSV daily, no steady direction, a rose: the leading sector (8 points), no degrees', dRow(DREC({ wind_dir: null, rose: [0, 0, 0, 0, 0, 3, 9, 2] })), ['', 'W']);
check('CSV daily, a rose tie: the earlier sector', dRow(DREC({ wind_dir: null, rose: [0, 4, 0, 0, 0, 0, 4, 0] })), ['', 'NE']);
check('CSV daily, neither (an all-calm rose or none): both empty', [dRow(DREC({ wind_dir: null, rose: [0, 0, 0, 0, 0, 0, 0, 0] })), dRow(DREC({ wind_dir: null, rose: undefined }))], [['', ''], ['', '']]);
check('CSV, direction hidden: no direction columns, no rose', recCsv([DREC()], true, k => k !== 'wind_dir', null)[0].filter(c => /direction|hours from/.test(c)), []);
check('CSV, the open record says so', recCsv([HREC], false, ALL, HREC)[1][0].endsWith(',open'), true);

// ---- imperial: the averages of converted records are the converted averages (linear); the sun threshold stays W/m²
RG = 'today'; imp(true);
vm.runInContext('U.t = "°F"; U.w = "mph"; U.r = "in"; U.p = "inHg"; M.temp.u = "°F";', ctx);  // the page rebuilds M with the units
check('imperial temp', run('temp', 'temp_c', [hr(0, { temp_c: [32, 35.6, 39.2] }), hr(1, { temp_c: [41, 44.6, 48.2] })])[0], ['40.1 °F', 'average']);
check('imperial rain: 2 decimals', run('rain', 'rain_mm', [hr(0, { rain_mm: .05 }), hr(1, { rain_mm: .12 })])[0], ['0.17 in', 'total']);
check('imperial pressure: a change under 0.005 inHg carries no sign', run('baro', 'pressure_hpa', [hr(0, { pressure_hpa: [null, 29.921, null] }), hr(1, { pressure_hpa: [null, 29.918, null] })])[1], ['±0.00 inHg', 'change']);
check('imperial wind: calm judged in km/h (0.2 mph = 0.3 km/h: calm; 0.5 mph = 0.8 km/h rounds to 1: not calm)',
  [run('wind', 'wind_kmh', [hr(0, { wind_kmh: [null, .2, null], wind_dir: 90 })]).map(x => x[1]), run('wind', 'wind_kmh', [hr(0, { wind_kmh: [null, .5, null], wind_dir: 90 })]).map(x => x[1])],
  [['average'], ['average', 'mostly from']]);

console.log(`sheet_vectors: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
