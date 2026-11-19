// test/tools/airport_vectors.js -- the dashboard's airport view (web/dash-airport.js) run in a vm with
// dash.js's formatters taken out by name (js_decl.js) and core.js's FC run against stub DOM objects: no humidity,
// the tall Wind tile with dash.js's dial, the weather words, the forecast line in local time (Europe/Paris: a window past midnight, a DST night), the whole
// view's HTML for its states, units, and the forecast phrases kept equal to admin-metar.js's.
// Usage: node test/tools/airport_vectors.js   (sets TZ=Europe/Paris itself)
'use strict';
if (process.env.TZ !== 'Europe/Paris') {
  const r = require('child_process').spawnSync(process.execPath, [__filename], { stdio: 'inherit', env: Object.assign({}, process.env, { TZ: 'Europe/Paris' }) });
  process.exit(r.status);
}
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { decl } = require('./js_decl');

const WEB = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web');
const rd = f => fs.readFileSync(path.join(WEB, f), 'utf8');
const dash = rd('dash.js'), core = rd('core.js'), apsrc = rd('dash-airport.js'), admin = rd('admin-metar.js'), outlook = rd('outlook.js');

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

// #apv takes the sheet's click listener, head the sheet's CSS
const apv = { addEventListener() {} };  // one #apv node, so a render can be read back
let css = '';  // the view's injected CSS, read back for the tall tile's rules
const doc = { documentElement: { dataset: {} }, visibilityState: 'visible', addEventListener() {}, body: { dataset: {} },
  querySelector: s => s === '#apv' ? apv : null, head: { insertAdjacentHTML(w, h) { css += h; } } };
const ctx = { console, Object, String, Math, Array, JSON, Date, Number, isNaN, document: doc, setInterval: () => 0, setTimeout: () => 0,
  clearTimeout() {}, localStorage: { getItem: () => null, setItem() {} }, matchMedia: () => ({ matches: false }), AbortController: class {},
  fetch: () => new Promise(() => {}) };
vm.createContext(ctx);
const names = ['WD', 'CONV', 'cvv', 'cv', 'toKmh', 'dsep', 'f1', 'f2', 'f0', 'fr', 'CARD', 'card', 'CALM_KMH', 'calm', 'dial', 'IC', 'icon'];
vm.runInContext(core + '\n' + outlook + '\nlet IMP = false; const U = { t: "°C", w: "km/h", r: "mm", p: "hPa" }; const M = {}; const S = { ap: true };\n' +
  'let why = false, lastFocus = null;\n' +
  names.map(n => decl(dash, n)).join('\n') + '\n' + ['p2', 'hm'].map(n => decl(rd('health.js'), n)).join('\n') + '\n' +
  'FC.poll = (n, t) => { (this.TASKS = this.TASKS || {})[n] = t; };\n' +  // the view's poll task, driven by the test
  apsrc + '\nthis.APV = APV;', ctx);
const A = ctx.APV, F = A.fmt;
const imperial = on => vm.runInContext(`IMP = ${on}; U.t = IMP ? '°F' : '°C'; U.w = IMP ? 'mph' : 'km/h'; U.p = IMP ? 'inHg' : 'hPa';`, ctx);

// 1. no humidity from whole-degree T / Td: no formatter, no words
check('no rh(), no Humidity words', [typeof F.rh, A.W.t_hum, A.W.oh], ['undefined', undefined, undefined]);

// 2. weather words: precipitation / obscuration first, then the sky
const wx = o => Object.assign({ precip: false, rain: false, drizzle: false, snow: false, freezing: false, thunder: false, fog: false, mist: false, int: 0 }, o);
check('thunder first', F.wxKey({ wx: wx({ thunder: true, rain: true }) }), 'thunder');
check('light rain', F.wxKey({ wx: wx({ rain: true, int: -1 }) }), 'rain_l');
check('heavy rain', F.wxKey({ wx: wx({ rain: true, int: 1 }) }), 'rain_h');
check('mist', F.wxKey({ wx: wx({ mist: true }) }), 'mist');
check('nothing', F.wxKey({ wx: wx({}) }), '');
check('sky cavok', F.skyKey({ cavok: true, cover: null }), 'clear');
check('sky none auto', F.skyKey({ cover: 'none', low_conf: true }), 'nocloud');
check('sky few', F.skyKey({ cover: 'few' }), 'partly');
check('sky overcast', F.skyKey({ cover: 'overcast' }), 'overcast');
check('sky unknown', F.skyKey({ cover: null }), '');

// 3. forecast windows in local time (Europe/Paris, CET in November)
const now = new Date(2026, 10, 14, 14, 30);  // 14:30 local
const Z = s => new Date(s).toISOString();
check('span same day', F.span(Z('2026-11-14T15:00:00Z'), Z('2026-11-14T20:00:00Z'), now), '16:00–21:00');
check('span to midnight', F.span(Z('2026-11-14T17:00:00Z'), Z('2026-11-14T23:00:00Z'), now), '18:00–00:00');
check('span past midnight', F.span(Z('2026-11-14T19:00:00Z'), Z('2026-11-15T01:00:00Z'), now), '20:00–02:00 tomorrow');
check('span tomorrow', F.span(Z('2026-11-15T00:00:00Z'), Z('2026-11-15T03:00:00Z'), now), 'tomorrow 01:00–04:00');
// the DST night (25 Oct 2026, 03:00 CEST -> 02:00 CET at 01:00Z)
check('span over the DST change', F.span(Z('2026-10-25T00:00:00Z'), Z('2026-10-25T05:00:00Z'), new Date(2026, 9, 25, 0, 30)), '02:00–06:00');
// on a DST day (25 Oct: 25 h, 29 Mar: 23 h) an end at the next local day's 00:00 reads 00:00 (local calendar units)
{ const oct = new Date(2026, 9, 25, 20, 0), mar = new Date(2026, 2, 29, 20, 0);
  check('25 Oct: to local midnight / after it', [F.span('2026-10-25T18:00:00Z', '2026-10-25T23:00:00Z', oct), F.span('2026-10-25T18:00:00Z', '2026-10-26T01:00:00Z', oct),
    F.span('2026-10-25T19:00:00Z', '2026-10-25T23:00:00Z', oct, false, true), F.span('2026-10-25T19:00:00Z', '2026-10-26T01:00:00Z', oct, false, true)],
    ['19:00–00:00', '19:00–02:00 tomorrow', 'until 00:00', 'until 02:00 tomorrow']);
  check('29 Mar: to local midnight / after it', [F.span('2026-03-29T17:00:00Z', '2026-03-29T22:00:00Z', mar), F.span('2026-03-29T17:00:00Z', '2026-03-29T23:00:00Z', mar),
    F.span('2026-03-29T18:00:00Z', '2026-03-29T22:00:00Z', mar, false, true), F.span('2026-03-29T18:00:00Z', '2026-03-29T23:00:00Z', mar, false, true)],
    ['19:00–00:00', '19:00–01:00 tomorrow', 'until 00:00', 'until 01:00 tomorrow']);
  }
const taf = o => Object.assign({ status: 'ok', current: true, issued: Z('2026-11-14T11:00:00Z'), valid_to: Z('2026-11-15T12:00:00Z'), to: Z('2026-11-15T02:00:00Z'), next12: [] }, o);
check('taf null -> nothing', F.forecast({ taf: null }, now), '');
// the strip's line 2 only when the TAF has something in it; an old TAF or "nothing significant" leave no line
check('taf not current: no line', F.forecast({ taf: taf({ current: false }) }, now), '');
check('taf empty: no line', F.forecast({ taf: taf() }, now), '');
check('taf short horizon, empty: no line', F.forecast({ taf: taf({ to: Z('2026-11-14T20:00:00Z') }) }, now), '');
check('taf only unknown kinds: no line', F.forecast({ taf: taf({ next12: [{ what: 'hail', possible: true, from: Z('2026-11-14T14:00:00Z'), to: Z('2026-11-14T16:00:00Z') }] }) }, now), '');
const w = (what, possible, a, b) => ({ what, possible, from: Z(a), to: Z(b) });
check('taf windows: first per kind, at most three, gusts without a number', F.forecast({ taf: taf({ next12: [
  w('thunder', true, '2026-11-14T16:00:00Z', '2026-11-14T18:00:00Z'), w('rain', false, '2026-11-14T15:00:00Z', '2026-11-14T20:00:00Z'),
  w('rain', true, '2026-11-14T21:00:00Z', '2026-11-14T23:00:00Z'), w('fog', true, '2026-11-14T22:00:00Z', '2026-11-15T01:00:00Z'),
  w('gusts', true, '2026-11-14T17:00:00Z', '2026-11-14T23:00:00Z')] }) }, now),
  'Forecast, next 12 h: Thunder possible 17:00–19:00. Rain likely 16:00–21:00. Fog possible 23:00–02:00 tomorrow.');
check('taf gusts phrase', F.forecast({ taf: taf({ next12: [w('gusts', false, '2026-11-14T17:00:00Z', '2026-11-14T23:00:00Z')] }) }, now),
  'Forecast, next 12 h: Strong gusts likely 18:00–00:00.');
check('25 Oct: the strip line to local midnight', F.forecast({ taf: taf({ to: Z('2026-10-26T07:00:00Z'), next12: [w('rain', false, '2026-10-25T19:00:00Z', '2026-10-25T23:00:00Z')] }) }, new Date(2026, 9, 25, 20, 0)),
  'Forecast, next 12 h: Rain likely 20:00–00:00.');
// a window that began before now (the board's started flag: from = now) prints no start time -- "throughout" when it is
// also cut at the horizon, "until {to}" when it ends inside it ("tomorrow" across local midnight); a later window is unchanged
const ws = (what, possible, a, b, cut) => Object.assign(w(what, possible, a, b), { started: true }, cut ? { cut: true } : {});
check('started + cut: throughout', F.forecast({ taf: taf({ next12: [ws('rain', true, '2026-11-14T13:30:00Z', '2026-11-15T01:30:00Z', true)] }) }, now),
  'Forecast, next 12 h: Rain possible throughout.');
check('started, ends inside: until', F.forecast({ taf: taf({ next12: [ws('rain', false, '2026-11-14T13:30:00Z', '2026-11-14T18:00:00Z')] }) }, now),
  'Forecast, next 12 h: Rain likely until 19:00.');
check('not started: unchanged', F.forecast({ taf: taf({ next12: [w('rain', true, '2026-11-14T16:00:00Z', '2026-11-14T18:00:00Z')] }) }, now),
  'Forecast, next 12 h: Rain possible 17:00–19:00.');
{ const late = new Date(2026, 10, 14, 22, 10);  // 22:10 local: an end past local midnight says "tomorrow"
  check('started across midnight: until ... tomorrow', [F.span('2026-11-14T21:10:00Z', '2026-11-14T23:30:00Z', late, false, true),
    F.span('2026-11-14T21:10:00Z', '2026-11-15T02:00:00Z', late, false, true), F.span('2026-11-14T21:10:00Z', '2026-11-14T23:00:00Z', late, false, true),
    F.span('2026-11-14T21:10:00Z', '2026-11-15T09:10:00Z', late, true, true)], ['until 00:30 tomorrow', 'until 03:00 tomorrow', 'until 00:00', 'throughout']); }

// 4. lengths and words
check('cloud base m', F.base(1500), ['460', 'm']);
check('vis 4.5 km', F.vis(4500), ['4.5', 'km']);
check('vis 8 km', F.vis(8000), ['8', 'km']);
check('vis 9999', F.vis(9999), ['10', 'km']);
check('cover cavok', F.coverWord({ cavok: true }), 'clear (CAVOK)');
check('cover none auto', F.coverWord({ cover: 'none', low_conf: true }), 'no low clouds');
check('cover broken', F.coverWord({ cover: 'broken' }), 'broken');
imperial(true);
check('cloud base ft', F.base(1520), ['1500', 'ft']);
check('vis mi', F.vis(9999), ['6.2', 'mi']);
imperial(false);

// 5. the whole view (its states)
const ap = (icao, name, km, age, o) => Object.assign({ icao, name, km, obs: '2026-11-14T13:00:00Z', age_s: age, stale: age > 5400, cur: age <= 4500, qnh_hpa: null, wind_dir: null,
  wind_kmh: null, gust_kmh: null, vis_m: null, vis_ge: false, cavok: false, cover: null, ceil_ft: null, low_conf: false, wx: wx({}), t_c: null, td_c: null, taf: null }, o);
const H = ap('EGLL', 'Heathrow', 18, 840, { qnh_hpa: 1018.2, wind_dir: 250, wind_kmh: 17, gust_kmh: 28, vis_m: 8000, cover: 'broken', ceil_ft: 1500,
  wx: wx({ drizzle: true }), t_c: 7, td_c: 6, taf: taf({ next12: [w('rain', true, '2026-11-14T14:00:00Z', '2026-11-14T20:00:00Z')] }) });
const G = ap('EGKK', 'Gatwick', 41, 1320, { qnh_hpa: 1017.6, wind_dir: 230, wind_kmh: 13, vis_m: 4500, cover: 'overcast', ceil_ft: 1000, wx: wx({ mist: true }), t_c: 6.4, td_c: 5.1 });
const Q = ap('ZZZZ', 'Sample Field', null, 540, { qnh_hpa: 1018.9, wind_dir: 'vrb', wind_kmh: 6, vis_m: 9999, vis_ge: true, cover: 'none', low_conf: true, t_c: 7.8, td_c: 4.9 });
const view = D => A.html(D, false, now);
const h3 = view({ on: true, airports: [H, G, Q] });
// the main airport in the title, one plain subtitle; the others are in the list bar and the sheet
check('head: title and subtitle', [h3.includes('<h1>Airport weather · Heathrow</h1>'), h3.includes('Main airport. Not this station’s readings.</span></p>'), h3.includes('more.')], [true, true, false]);
// the feet say whose values they are -- several others: an "Others" header, then a row each
check('feet, several: the header first', /<span class="sub foot oth"><span>Others<\/span><span class="" title="EGKK/.test(h3), true);
check('the sky tile is named Sky (no "Clouds / Low clouds")', h3.includes('</svg>Sky</span>') && !h3.includes('>Clouds<'), true);
// every tile and the strip open the all-airports sheet; the strip's way in by keyboard is its ⓘ
check('tiles are buttons that open the sheet', (h3.match(/<button type="button" class="glass tile apt[^"]*" style="[^"]*" data-aps="\w+" aria-haspopup="dialog" aria-label="[^"<>]+">/g) || []).length, 5);
check('the strip opens it, its ⓘ too', h3.includes('<div class="glass ol" role="group" data-aps="s">') && h3.includes('class="oli" data-aps="ol" aria-haspopup="dialog" aria-label="All airports"'), true);
check('no cursor:default left on the strip', h3.includes('cursor:default'), false);
check('five tiles, Wind first and tall, no Humidity', [(h3.match(/class="glass tile apt/g) || []).length, /^[\s\S]*?class="glass tile apt aph" style="--c:var\(--wind\)" data-aps="wind"/.test(h3), h3.includes('Humidity'), h3.includes('data-aps="hum"')], [5, true, false, false]);
check('two other-airport lines per tile', (h3.match(/<span class="" title="EGKK/g) || []).length + (h3.match(/<span class="" title="ZZZZ/g) || []).length, 12);
check('line 1 now', h3.includes('<b>Drizzle at Heathrow.</b>'), true);
check('line 2 forecast', h3.includes('Forecast, next 12 h: Rain possible 15:00–21:00.'), true);
// nothing significant in the TAF -> the strip is one line; the all-airports sheet keeps its "Next 12 h" row
const hq = view({ on: true, airports: [Object.assign({}, H, { taf: taf() })] });
check('strip: one line when the TAF has nothing', [hq.includes('<b>Drizzle at Heathrow.</b></div>'), hq.includes('Nothing significant'), hq.includes('Forecast, next 12 h')], [true, false, false]);
check('sheet: keeps its Next 12 h row', A.sheet({ on: true, airports: [Object.assign({}, H, { taf: taf() })] }, now, false)[1].includes('<li class="q">Nothing significant forecast.</li>'), true);
// the station page's dial in the Wind tile -- grey sectors (no hour here), the marker where the wind comes
// from, the speed in the middle with "from WSW" and the degrees; the gust under it, then the other airports' foot
const wt = h3.slice(h3.indexOf('data-aps="wind"'), h3.indexOf('data-aps="temp"'));
check('the dial in its box, before the gust line and the foot', [/<span class="aphd"><svg class="dial" viewBox="0 0 300 300"/.test(wt), wt.indexOf('class="aphd"') < wt.indexOf('gust <b>28</b> km/h'), wt.indexOf('gust <b>28</b> km/h') < wt.indexOf('class="sub foot oth"')], [true, true, true]);
// an airport's speed in whole units everywhere (METAR gives whole knots)
check('speed, direction and degrees in the dial; no lit sector', [wt.includes('>17</text>'), wt.includes('km/h from WSW</text>'), wt.includes('>250°</text>'), wt.includes('stroke="var(--wind)"'), (wt.match(/stroke="var\(--grid\)"/g) || []).length], [true, true, true, false, 16]);
check('the marker (where the wind comes from)', /<path d="M[\d.]+ [\d.]+L[\d.]+ [\d.]+L[\d.]+ [\d.]+Z" fill="var\(--ink\)"/.test(wt), true);
check('the tile keeps its short accessible name', h3.includes('aria-label="Wind: 17 km/h. Open all airports"'), true);
check('the direction words left the sub line (the dial says them)', h3.includes('from <b>WSW</b>'), false);
const vt = view({ on: true, airports: [Object.assign({}, Q, { gust_kmh: 20 })] });
const vw = vt.slice(vt.indexOf('data-aps="wind"'), vt.indexOf('data-aps="temp"'));
check('a variable wind -- no marker, no "from", no degrees; "variable, gust" under the dial', [/fill="var\(--ink\)" stroke="var\(--ring\)"/.test(vw), vw.includes(' from '), vw.includes('°</text>'), vw.includes('<b>variable</b>, gust <b>20</b> km/h')], [false, false, false, true]);
const ct = view({ on: true, airports: [Object.assign({}, H, { wind_kmh: 0, gust_kmh: null })] });
check('calm -- the dial says Calm, nothing under it', [ct.includes('>Calm</text>'), /data-aps="wind"[\s\S]*?<span class="sub"><\/span>/.test(ct)], [true, true]);
// on a phone the tile spans the width with a large centred dial (about two thirds, at most 200 px), gust and foot under it
check('the tall tile -- two rows in the 3-column grid, the phone a full-width tile with a large dial',
  [css.includes('@media (min-width:820px){.grid>.aph{grid-row:span 2}}'), css.includes('@media (max-width:819px){.grid>.aph{grid-column:1/-1}.aphd .dial{width:65%;max-width:200px}}'), css.includes('112px')], [true, true, false]);
// on a phone the other-airport row is the plane, the name, then the value with its unit -- two spans the CSS lays
// out as name (ellipsis) and value (never broken)
check('foot row: plane, name span, value span', /title="EGKK[^"]*"><svg class="pl"[^>]*>[\s\S]*?<\/svg><span>Gatwick<\/span><span><b>13<\/b> km\/h SW<\/span><\/span>/.test(h3), true);
check('variable other', h3.includes('Sample Field</span><span><b>6</b> km/h variable'), true);
check('clouds: the level in words as the headline', h3.includes('<span class="val w">Low clouds</span>'), true);
check('clouds: cover and base under it', h3.includes('<span>broken, base 460 m</span>'), true);
const lvl = ft => { const h = view({ on: true, airports: [Object.assign({}, H, { ceil_ft: ft, cover: 'overcast' })] }); return (/<span class="val w">([^<]*)<\/span><span class="sub"><span>overcast, base/.exec(h) || [])[1]; };
check('levels: 300 m and 2 / 6 km bounds', [lvl(900), lvl(1000), lvl(6500), lvl(6600), lvl(19600), lvl(19700)],
  ['Very low clouds', 'Low clouds', 'Low clouds', 'Mid-level clouds', 'Mid-level clouds', 'High clouds']);
check('clouds other, no ceiling', h3.includes('Sample Field</span><span>no low clouds'), true);
check('visibility ge other', h3.includes('Sample Field</span><span><b>10</b> km or more'), true);
check('visibility sub word', h3.includes('<span>drizzle</span>'), true);
check('strip main', h3.includes('<b>Heathrow</b> (main) 18 km · 14 min ago'), true);
check('strip no km', h3.includes('<b>Sample Field</b> · 9 min ago'), true);
check('source link', h3.includes('href="https://aviationweather.gov/" target="_blank" rel="noopener"'), true);
check('no tile graphics beyond the label icon and the dial', (h3.match(/<svg/g) || []).length, 1 + 5 + 1 + 1 + 1 + 10 + 3);  // head plane, 5 labels, the dial, strip glyph + its ⓘ, 10 foot planes, 3 strip planes
const hs = view({ on: true, airports: [H, Object.assign({}, G, { age_s: 7800, stale: true })] });
check('stale other greys its lines', (hs.match(/<span class="old( al)?" title="EGKK/g) || []).length, 6);
const h2o = view({ on: true, airports: [H, G] });
check('feet, one other: "Also {name}: {value}"', [h2o.includes('<span>Also Gatwick:</span><span><b>13</b> km/h SW</span>'), h2o.includes('>Others<')], [true, false]);
check('one other airport -> the row may wrap its value under the name (narrow tiles)', [/<span class=" al" title="EGKK/.test(h2o), css.includes('.apt .oth>.al{flex-wrap:wrap}')], [true, true]);
const h1 = view({ on: true, airports: [H] });
check('one airport: no foot lines', h1.includes('title="EGKK'), false);
check('one airport head', h1.includes('<h1>Airport weather · Heathrow</h1>'), true);
const hm0 = view({ on: true, airports: [Object.assign({}, H, { stale: true, age_s: 6000 }), G] });
check('stale main stays the headline, greyed', hm0.includes('class="glass tile apt old"') && hm0.includes('1018.2<small>hPa</small>'), true);
check('stale main: line 1 says no current report', hm0.includes('<b>No current report from Heathrow.</b>'), true);
const hn = view({ on: true, airports: [{ icao: 'EGLL', name: 'Heathrow', km: 18, obs: null, age_s: null, stale: true, taf: null }] });
check('no report yet: dashes', (hn.match(/<span class="val">–<\/span>/g) || []).length, 5);
check('no report yet: strip', hn.includes('<b>Heathrow</b> (main) · no report yet'), true);
check('module off', view({ on: false, airports: [] }).includes('Airport weather is off.'), true);
check('loading', A.html(null, false, now).includes('Loading airport weather…'), true);
check('failed', A.html(null, true, now).includes('Could not load the airport weather.'), true);
// the first answer is a 304 when the station page's TAF line (dash-wufc.js) polled /api/metar before (one ETag cache):
// it renders at once, not "Loading…" until the data changes; a later 304 does not re-render
apv.innerHTML = '';
ctx.TASKS.metar.on({ status: 304, changed: false, data: { on: true, airports: [H] } });
check('first answer a 304: rendered, no "Loading…"', [apv.innerHTML.includes('Heathrow'), apv.innerHTML.includes('Loading airport weather…')], [true, false]);
apv.innerHTML = 'kept';
ctx.TASKS.metar.on({ status: 304, changed: false, data: { on: true, airports: [H] } });
check('a later 304: no re-render', apv.innerHTML, 'kept');
const hc = view({ on: true, airports: [Object.assign({}, Q, { name: 'Sample Field' })] });
check('no ceiling: the sky in words as the headline, "no ceiling" below', hc.includes('<span class="val w">No low clouds</span><span class="sub"><span>no ceiling</span></span>'), true);
const hf = view({ on: true, airports: [Object.assign({}, H, { ceil_ft: null, cover: 'scattered' })] });
check('no ceiling, scattered', hf.includes('<span class="val w">Scattered clouds</span>'), true);
check('no ceiling, CAVOK', view({ on: true, airports: [Object.assign({}, H, { ceil_ft: null, cover: null, cavok: true })] }).includes('<span class="val w">Clear</span>'), true);
const cloudsOf = h => h.slice(h.indexOf('data-aps="cloud"'), h.indexOf('data-aps="vis"'));
check('no ceiling, nothing known: a dash', cloudsOf(view({ on: true, airports: [Object.assign({}, H, { ceil_ft: null, cover: null })] })).includes('<span class="val">–</span>'), true);
check('with a ceiling the number is never the headline', /<span class="val">460/.test(h3), false);
// broken or overcast with the base not reported (///) is a ceiling of unknown height, never "no ceiling";
// few / scattered keep "no ceiling" (none by definition); the "Also" row and the all-airports sheet say the same word
{
  const sky = o => Object.assign({}, H, { ceil_ft: null }, o), main = o => cloudsOf(view({ on: true, airports: [sky(o)] }));
  const sub = h => (/<span class="val w">([^<]*)<\/span><span class="sub"><span>([^<]*)<\/span>/.exec(h) || []).slice(1);
  check('OVC///: Overcast, base unknown', sub(main({ cover: 'overcast' })), ['Overcast', 'base unknown']);
  check('BKN///: Broken clouds, base unknown', sub(main({ cover: 'broken' })), ['Broken clouds', 'base unknown']);
  check('SCT///: Scattered clouds, no ceiling', sub(main({ cover: 'scattered' })), ['Scattered clouds', 'no ceiling']);
  check('FEW030 (no ceiling sent): Few clouds, no ceiling', sub(main({ cover: 'few' })), ['Few clouds', 'no ceiling']);
  check('CAVOK with a broken cover field: no ceiling', sub(main({ cover: 'broken', cavok: true })), ['Clear', 'no ceiling']);
  check('nothing known stays a dash', main({ cover: null }).includes('<span class="val">–</span>'), true);
  check('never "no ceiling" for broken / overcast', ['broken', 'overcast'].map(c => main({ cover: c }).includes('no ceiling')), [false, false]);
  const also = o => { const h = cloudsOf(view({ on: true, airports: [H, Object.assign({}, G, { ceil_ft: null }, o)] })); return (/<span>Also Gatwick:<\/span><span>([^<]*)<\/span>/.exec(h) || [])[1]; };
  check('Also row', [also({ cover: 'overcast' }), also({ cover: 'broken' }), also({ cover: 'scattered' }), also({ cover: 'few' }), also({ cover: null })],
    ['overcast, base unknown', 'broken, base unknown', 'scattered clouds', 'few clouds', '–']);
  const card = o => { const c = A.sheet({ on: true, airports: [sky(o)] }, now, false)[1]; return (/Sky<\/span><span class="apc-v"><b>([^<]*)<\/b>(?:<small>([^<]*)<\/small>)?<\/span>/.exec(c) || []).slice(1); };
  check('sheet', [card({ cover: 'overcast' }), card({ cover: 'broken' }), card({ cover: 'scattered' }), card({ cover: 'few' }), card({ cover: null })],
    [['Overcast', 'base unknown'], ['Broken clouds', 'base unknown'], ['Scattered clouds', undefined], ['Few clouds', undefined], ['–', undefined]]);
}
const hd = view({ on: true, airports: [Object.assign({}, H, { td_c: null }), Object.assign({}, G, { td_c: null })] });
check('no Humidity tile: no dew point -> the temperature alone', [hd.includes('dew point'), hd.includes('Humidity'), hd.includes('7.0<small>°C</small>')], [false, false, true]);
check('names escaped', view({ on: true, airports: [Object.assign({}, H, { name: 'A<b>' })] }).includes('<h1>Airport weather · A&lt;b&gt;</h1>'), true);
imperial(true);
const hi = view({ on: true, airports: [H, G] });
check('imperial wind', [hi.includes('aria-label="Wind: 11 mph. Open all airports"'), hi.includes('>11</text>'), hi.includes('mph from WSW</text>')], [true, true, true]);
check('imperial cloud base under the words', hi.includes('<span>broken, base 1500 ft</span>'), true);
check('imperial strip distance', hi.includes('(main) 11 mi'), true);
imperial(false);

// 7. the all-airports sheet
const sh = (D, n) => A.sheet(D, n || now, false);
// a card's sky glyph follows the airport's own day / night (the firmware's `night`, its position at the report's time); the
// station's day / night only when the airport carries no flag. Q reads "No low clouds" (the sun / the moon)
{
  const card = (a, stationNight) => A.sheet({ on: true, airports: [a] }, now, stationNight)[1];
  const qn = o => Object.assign({}, Q, o), noFlag = Object.assign({}, Q);
  delete noFlag.night;
  const day = card(noFlag, false), nite = card(noFlag, true);
  check('the old rule kept -- no flag: the station decides (sun by day, moon by night)', [day !== nite, card(noFlag, false) === day], [true, true]);
  check('night at the airport while day at the station -> the moon', card(qn({ night: true }), false), nite);
  check('day at the airport while night at the station -> the sun', card(qn({ night: false }), true), day);
  // the view's strip glyph (the main airport) follows the main airport's flag the same way
  const strip = (a, sky) => { doc.documentElement.dataset.sky = sky; const h = view({ on: true, airports: [a] }); doc.documentElement.dataset.sky = undefined; return h.slice(h.indexOf('data-aps="s">'), h.indexOf('<div class="olt">')); };
  check('the view -- no flag: the station decides', strip(noFlag, 'night') !== strip(noFlag, 'day'), true);
  check('the view -- the main airport’s own night / day wins', [strip(qn({ night: true }), 'day'), strip(qn({ night: false }), 'night')], [strip(noFlag, 'night'), strip(noFlag, 'day')]);
}
const [s3, b3] = sh({ on: true, airports: [H, G, Q] });
check('sheet: subtitle', [s3, sh({ on: true, airports: [H] })[0]], ['3 airports, in your order', '1 airport']);
check('sheet: one card per airport, in the user order', [b3.indexOf('<b>Heathrow</b>') < b3.indexOf('<b>Gatwick</b>'), b3.indexOf('<b>Gatwick</b>') < b3.indexOf('<b>Sample Field</b>'), (b3.match(/<section class="apc/g) || []).length], [true, true, 3]);
check('sheet: reference line at the top, source at the end', b3.startsWith('<p class="cap">For reference: not this station’s readings.</p>') && b3.endsWith('The order is yours (Settings, Airport weather).</p>'), true);
check('sheet: header as the admin row (no main chip, no distance)', b3.includes('<div class="apc-n"><b>Heathrow</b> <span class="hint">EGLL</span> <span class="dot"></span><span class="hint">14 min ago</span></div>') && !b3.includes('main') && !b3.includes('18 km'), true);
check('sheet: the weather now as a word', [b3.includes('<b>Drizzle</b>'), b3.includes('<b>Mist</b>'), b3.includes('<b>No low clouds</b>')], [true, true, true]);
check('sheet: five mini tiles per card (no humidity)', [(b3.match(/<li( class="apw")? style="--c:/g) || []).length, b3.includes('Humidity')], [15, false]);
// the card's Wind is the view's dial (wnd(): speed, from, degrees) with the gust line under it
const hwc = b3.slice(b3.indexOf('<b>Heathrow</b>'), b3.indexOf('<b>Gatwick</b>')), gc = b3.slice(b3.indexOf('<b>Gatwick</b>'), b3.indexOf('<b>Sample Field</b>'));
check('the Wind tile first, with the dial, then the gust line', [/<ul class="apc-tiles"><li class="apw" style="--c:var\(--wind\)"><span class="apc-l">[^]*?Wind<\/span><svg class="dial"/.test(hwc),
  hwc.includes('>17</text>'), hwc.includes('km/h from WSW</text>'), hwc.includes('>250°</text>'), hwc.includes('<span class="apc-v"><small>gust <b>28</b> km/h</small></span></li>')], [true, true, true, true, true]);
check('then Temperature, Pressure, Sky, Visibility (2 x 2 beside it)', ['Temperature', 'Pressure', 'Sky', 'Visibility'].map(t => hwc.indexOf(t + '</span>')).every((x, i, a) => x > hwc.indexOf('class="apw"') && (!i || x > a[i - 1])), true);
check('a card with a TAF keeps its left column; one without has none', [hwc.includes('<p class="apc-fh">'), hwc.includes('<ul class="apc-taf">'), gc.includes('apc-fh'), gc.includes('<svg class="dial"')], [true, true, false, true]);
check('the layout -- two rows tall, a phone: full width with a large dial', [css.includes('.apw{grid-row:span 2}'), css.includes('.apw{grid-column:1/-1;grid-row:auto}.apw .dial{width:65%;max-width:200px}'), css.includes('88px')], [true, true, false]);
// on a phone the airport sheet sizes to its content like every read-only sheet (app.css .sheet.why, up to 92dvh)
check('no 72dvh cap of its own on the airport sheet', css.includes('72dvh'), false);
// the desktop sheet keeps its 72vh; a long name keeps its ellipsis in a narrow tile
check('desktop 72vh, the name capped at the row', [css.includes('@media (min-width:820px){.sheet.aps{max-height:72vh}}'), css.includes('.apt .oth>.al>:nth-child(2){flex:1 0 auto;max-width:100%}')], [true, true]);
// an airport has no wind history: the dial's label names no outer ring
check('no ring sentence without a history', [b3.includes('Outer ring'), h3.includes('Outer ring'), /aria-label="Wind 17 km\/h from WSW, 250 degrees\. "/.test(b3)], [false, false, true]);
check('the dial box is phrasing content inside the tile button', h3.includes('<div class="aphd">'), false);
// a window the board cut at its now + 12 h (off the whole hour) has no end time; a whole-hour end is a real end
check('horizon span', [F.span('2026-11-14T15:00:00Z', '2026-11-15T00:54:00Z', now, true), F.span('2026-11-14T15:00:00Z', '2026-11-14T19:00:00Z', now),
  F.span('2026-11-14T15:00:00Z', '2026-11-15T01:00:00Z', now, true)], ['from 16:00', '16:00–20:00', 'from 16:00']);
// the ⓘ sheet's span column uses the same day templates as this view's span()
{ const dw = rd('dash-wufc.js'), p = (src, k) => (new RegExp(k + ": '([^']*)'").exec(src) || [])[1];
  check('the same day words in the view and the sheet', [['span', 'sp'], ['span_tm', 'sp_tm'], ['span_to_tm', 'sp_to_tm'], ['span_o', 'fr'], ['span_otm', 'fr_tm'],
    ['span_on_o', 'sp_on_o'], ['span_on', 'sp_on'], ['span_on_tm', 'sp_on_tm']].every(([a, b]) => p(apsrc, a) && p(apsrc, a) === p(dw, b)), true); }
check('one wind builder for the view and the sheet', [/function wnd\(x\)/.test(apsrc), (apsrc.match(/dial\(\{/g) || []).length], [true, 1]);
check('sheet: temperature and dew point', b3.includes('<b>7.0 °C</b><small>dew point 6.0</small>'), true);
check('sheet: pressure alone', b3.includes('<b>1018.2 hPa</b></span>'), true);
check('sheet: sky level and base', b3.includes('<b>Low clouds</b><small>broken, base 460 m</small>'), true);
check('sheet: visibility and the weather', b3.includes('<b>8 km</b><small>drizzle</small>'), true);
check('sheet: variable wind, visibility or more, no ceiling', [b3.includes('<small><b>variable</b></small>'), b3.includes('<b>10 km or more</b>'), b3.includes('Sky</span><span class="apc-v"><b>No low clouds</b></span>')], [true, true, true]);
check('sheet: the measure colours', [b3.includes('<li class="apw" style="--c:var(--wind)">'), b3.includes('<li style="--c:var(--ink-2)">')], [true, true]);
check('sheet: the forecast under its head', b3.includes('<p class="apc-fh">') && b3.includes('<li>Rain possible 15:00–21:00.</li>'), true);
check('sheet: no forecast section without a cycle', b3.slice(b3.indexOf('<b>Gatwick</b>'), b3.indexOf('<b>Sample Field</b>')).includes('apc-fh'), false);
const ww = F => A.windows(Object.assign({}, H, { taf: F }), now);
const two = ww(taf({ next12: [w('gusts', true, '2026-11-14T14:00:00Z', '2026-11-14T18:00:00Z'), w('rain', false, '2026-11-14T16:00:00Z', '2026-11-14T20:00:00Z')] }));
check('sheet: likely first and marked', two[1], [['Rain likely 17:00–21:00.', 'lk'], ['Strong gusts possible 15:00–19:00.', '']]);
check('sheet: nothing significant / no forecast', [ww(taf({ next12: [] }))[1], ww(Object.assign(taf({ next12: [] }), { current: false }))[1]], [[['Nothing significant forecast.', 'q']], [['No forecast for this airport.', 'q']]]);
check('sheet: none without a cycle', ww(null), null);
check('sheet rows: started', ww(taf({ next12: [ws('gusts', true, '2026-11-14T13:30:00Z', '2026-11-14T18:00:00Z'), ws('rain', false, '2026-11-14T13:30:00Z', '2026-11-15T01:30:00Z', true)] }))[1],
  [['Rain likely throughout.', 'lk'], ['Strong gusts possible until 19:00.', '']]);
{ const hv = view({ on: true, airports: [Object.assign({}, H, { taf: taf({ next12: [ws('rain', true, '2026-11-14T13:30:00Z', '2026-11-15T01:30:00Z', true)] }) })] });
  check('the view: started + cut on the strip and the sheet', [hv.includes('Rain possible throughout.'), / from \d/.test(hv)], [true, false]); }
const [, bs] = sh({ on: true, airports: [Object.assign({}, H, { stale: true, age_s: 6000 })] });
check('sheet: a stale report greys its card, says so, keeps the values', [bs.includes('<section class="apc old"'), bs.includes('<b>No current report</b>'), bs.includes('<span class="dot off">'), bs.includes('<b>1018.2 hPa</b>')], [true, true, true, true]);
const [, bn] = sh({ on: true, airports: [{ icao: 'EGLL', name: 'Heathrow', km: 18, obs: null, age_s: null, stale: true, taf: null }] });
check('sheet: no report yet', [(bn.match(/<b>–<\/b>/g) || []).length, bn.includes('<span class="hint">no report yet</span>')], [5, true]);
check('sheet: off or empty -> no card', sh({ on: false, airports: [H] })[1].includes('apc'), false);
check('sheet: names escaped', sh({ on: true, airports: [Object.assign({}, H, { name: 'A<b>' })] })[1].includes('<b>A&lt;b&gt;</b>'), true);
check('sheet: a flag waits for its pack (cc, no pack yet -> nothing)', sh({ on: true, airports: [Object.assign({}, H, { cc: 'GB', fp: 0 })] })[1].includes('class="flag"'), false);
// the sheet's headline words are the strip's sentences without " at {main}." (two tables, kept equal)
// the 14 sentences are one template over the headline words -- every weather / sky key has its word,
// and the strip says exactly the word + " at {main}." (and "No current report from {main}." without a report)
const AW = A.W, KS = ['thunder', 'freezing', 'snow', 'rain_l', 'rain', 'rain_h', 'drizzle', 'fog', 'mist', 'clear', 'nocloud', 'partly', 'mostly', 'overcast'];
check('every strip sentence has its headline word', KS.filter(k => !AW['s_' + k] || AW.n.replace('{w}', AW['s_' + k]).replace('{main}', 'X') !== AW['s_' + k] + ' at X.'), []);
check('no headline word without its sentence', Object.keys(AW).filter(k => /^s_[a-z_]+$/.test(k) && !/^s_(v|w|wn|wv|g|dew|none)$/.test(k) && !KS.includes(k.slice(2))), []);
check('no current report keeps its own sentence', AW.n_none.replace('{main}', 'X'), AW.s_none + ' from X.');
check('the strip says the template', [view({ on: true, airports: [Object.assign({}, H, { wx: wx({ thunder: true }) })] }).includes('<b>Thunderstorm at Heathrow.</b>'), view({ on: true, airports: [Object.assign({}, H, { wx: wx({}), cover: 'overcast' })] }).includes('<b>Overcast at Heathrow.</b>')], [true, true]);
// a tile's accessible name is its measure, the main value and what it opens
check('tile aria-label', [h3.includes('aria-label="Wind: 17 km/h. Open all airports"'), h3.includes('aria-label="Temperature: 7.0 °C. Open all airports"'), h3.includes('aria-label="Sky: Low clouds. Open all airports"')], [true, true, true]);
check('wind without a direction (the dial says the unit alone)', (s => [s.includes('>km/h</text>'), s.includes('km/h from'), s.includes('<small>gust <b>28</b> km/h</small>')])(sh({ on: true, airports: [Object.assign({}, H, { wind_dir: null })] })[1]), [true, false, true]);
check('no empty headline', sh({ on: true, airports: [Object.assign({}, G, { wx: wx({}), cover: null, ceil_ft: null })] })[1].includes('<b></b>'), false);
imperial(true);
check('sheet: imperial', [sh({ on: true, airports: [H] })[1].includes('mph from WSW</text>') && sh({ on: true, airports: [H] })[1].includes('>11</text>'), sh({ on: true, airports: [H] })[1].includes('base 1500 ft')], [true, true]);
imperial(false);

// 6. the forecast phrases equal admin-metar.js's (one wording family, gusts included), {from}–{to} as {span}
const phr = (src, k) => { const m = new RegExp(k + ": '([^']*)'").exec(src); return m && m[1]; };
// the admin row has no forecast head; the dashboard view keeps taf_head / taf_head_to
for (const k of ['taf_none', 'taf_na']) check('same phrase ' + k, phr(apsrc, k), phr(admin, k));
// one template per certainty, the phenomena as the same words in both files
for (const k of ['taf_e', 'taf_p']) check('same phrase ' + k, phr(apsrc, k), (phr(admin, k) || '').replace('{from}–{to}', '{span}'));
// a window that began before now -- the view's {span} words give the admin row's whole sentences
for (const [k, s, a] of [['taf_e', 'span_on_o', 'taf_e_n'], ['taf_p', 'span_on_o', 'taf_p_n'], ['taf_e', 'span_on', 'taf_e_u'], ['taf_p', 'span_on', 'taf_p_u']])
  check('same phrase ' + a, (phr(apsrc, k) || '').replace('{span}', phr(apsrc, s)), phr(admin, a));
// both keep them in their W tables (tc_<code>)
const tcw = src => '{ ' + ['thunder', 'snow', 'rain', 'fog', 'gusts'].map(k => `${k}: '${(new RegExp('tc_' + k + ": '([^']*)'").exec(src) || [])[1]}'`).join(', ') + ' }';
const apw = tcw(apsrc);
check('the same phenomenon words', [apw, apw === tcw(admin)], ["{ thunder: 'Thunder', snow: 'Snow', rain: 'Rain', fog: 'Fog', gusts: 'Strong gusts' }", true]);

console.log(`airport_vectors: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
