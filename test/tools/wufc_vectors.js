// test/tools/wufc_vectors.js -- page 2 "Next" of the
// outlook card (web/dash-wufc.js) and the i sheet (web/outlook-why.js) under node. The degradation rows: no source -> no
// page 2 (today's strip); WU only; TAF only; both; TAF not current / no window / farther than 60 km / module off / the second
// airport's TAF -> no TAF line; the highest phenomenon first; "possible" never on the strip; °F; pop unset; "Today"; the dots (two
// tabs, tooltips naming only the sources present, 20-px targets 8 px apart). The source tags,
// the sheet's Now header, its one Forecast list in time order (grouped windows, WU at its day's 07:00), the one source line, and
// WU's phrase in the page's words. Usage: node test/tools/wufc_vectors.js   (sets TZ=Europe/Paris itself: the DST nights)
'use strict';
if (process.env.TZ !== 'Europe/Paris') {
  const r = require('child_process').spawnSync(process.execPath, [__filename], { stdio: 'inherit', env: Object.assign({}, process.env, { TZ: 'Europe/Paris' }) });
  process.exit(r.status);
}
const fs = require('fs');
const path = require('path');
const { decl } = require('./js_decl');

const web = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web');
let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
// the page's one clock-time helper (health.js); the strip's page 1 and its i (dash.js olIn / OLI), ahead of the source
const rd = f => fs.readFileSync(path.join(web, f), 'utf8');
const HM = ['p2', 'hm'].map(n => decl(rd('health.js'), n)).join('\n') + '\n';
const OL = ['WD', 'olIn', 'OLI'].map(n => decl(rd('dash.js'), n)).join('\n') + '\n';
const hm = new Function(HM + 'return hm;')();
const f0 = v => v == null || isNaN(v) ? '–' : String(Math.round(v)).replace('-', '−');
const FCO = new Function(rd('outlook.js') + '\nreturn FCO;')();
let IMP = false, renders = 0, css = '';
const cv = (k, x) => (IMP && k === 'temp_c' ? x * 9 / 5 + 32 : x);
const polls = {};
// the page's clock for the day words: NOW fixes "now" (new Date() / Date.now() in dash-wufc.js); null = the real clock
let NOW = null;
class FD extends Date { constructor(...a) { if (a.length) super(...a); else super(NOW == null ? Date.now() : NOW); } static now() { return NOW == null ? Date.now() : NOW; } }
const env = { Date: FD, esc, f0, cv, FCO, IC: { plane: '<path d="plane"/>' }, FC: { poll: (n, t) => { polls[n] = t; }, w: (f, t) => t },
  S: { ap: false, status: { metar: true } }, render: () => { renders++; }, setTimeout: f => f(),  // the ring's pause settles at once here
  document: { createElement: () => ({ set textContent(v) { css = v; } }), head: { appendChild() {} } } };
const FCX = new Function(...Object.keys(env), HM + OL + rd('dash-wufc.js') + '\nreturn FCX;')(...Object.values(env));
const metar = m => polls.wufcm.on({ data: m, changed: true });

const t = (h, m = 0) => new Date(2026, 10, 16, h, m).toISOString();
const sp = (a, b) => hm(t(a)) + '–' + hm(t(b));
const win = (what, from, to, possible = false) => ({ what, possible, from: t(from), to: t(to) });
const airport = (o, taf) => Object.assign({ icao: 'EGLL', name: 'Heathrow', km: 18, cur: true, stale: false, taf: Object.assign({ status: 'ok', current: true, next12: [] }, taf) }, o);
const M = (a0, a1) => ({ on: true, airports: [a0].concat(a1 ? [a1] : []) });
const L = (wufc, night) => ({ wufc, outlook: { in: { night: !!night } } });
const WU = o => Object.assign({ day: 'tomorrow', phrase: 'Light Rain', hi: 9, lo: 4, pop: 70, t: t(16, 40) }, o);
const ol = { glyph: '<svg g1/>', l1: 'No rain.', l2: 'Pressure steady.', warn: 'Fog reported nearby.', wglyph: '<svg w/>' };

check('the poll: /api/metar while the module is on, the station page only', [polls.wufcm.url(), (env.S.ap = true, polls.wufcm.url())], ['/api/metar', null]);
env.S.ap = false;
// ---- WU's phrase in the page's words, one function for page 2 and the sheet
const PH = [['PM Light Rain', 'Afternoon light rain'], ['AM Clouds/PM Sun', 'Morning clouds, afternoon sun'], ['Showers Early', 'Showers early'],
  ['Mostly Cloudy/Wind', 'Mostly cloudy, wind'], ['Partly Cloudy', 'Partly cloudy'], ['Mostly Sunny', 'Mostly sunny'], ['Cloudy', 'Cloudy'],
  ['AM Showers', 'Morning showers'], ['PM Thunderstorms', 'Afternoon thunderstorms'], ['Rain/Snow Showers', 'Rain, snow showers'],
  ['Showers Late', 'Showers late'], ['Mostly Cloudy with Scattered Thunderstorms', 'Mostly cloudy with scattered thunderstorms'],
  ['Heavy Rain', 'Heavy rain'], ['Few Showers / Wind', 'Few showers, wind']];
check('the common phrases', PH.map(([p]) => FCX.ph(p)), PH.map(x => x[1]));
check('an unknown phrase as received, the first letter capitalised', [FCX.ph('Sct T-Storms'), FCX.ph('freezing fog'), FCX.ph('PM T-Storms')], ['Sct T-Storms', 'Freezing fog', 'Afternoon T-Storms']);
check('admin-fwd.js carries the same ph() (the admin does not load dash-wufc.js)', (() => { const d = x => (/  const ph = [^\n]*\n[^\n]*\n/.exec(rd(x)) || [''])[0]; return d('dash-wufc.js') !== '' && d('dash-wufc.js') === d('admin-fwd.js'); })(), true);
check('no AM / PM / slash left in the words', PH.some(([p]) => /\b(AM|PM)\b|\//.test(FCX.ph(p))), false);
// ---- no source: no page 2 (today's strip)
metar(null);
check('no source -> no page 2', FCX.p2(L(null)), null);
// ---- WU only
let p = FCX.p2(L(WU()));
check('WU only: line 2, no line 1', [!!p.b, p.a], [true, null]);
// no chance on the strip (a bare "70 %" says too little there); the sheet says "70 % chance of precipitation"
check('WU line', p.b.text, 'Tomorrow: Light rain · 9° / 4°');
check('WU line: the phrase converted', FCX.p2(L(WU({ phrase: 'PM Light Rain' }))).b.text, 'Tomorrow: Afternoon light rain · 9° / 4°');
check('WU, pop unset: the same line', FCX.p2(L(WU({ pop: null }))).b.text, 'Tomorrow: Light rain · 9° / 4°');
check('WU line: no percentage for any chance', [0, 100].map(n => FCX.p2(L(WU({ pop: n }))).b.text.includes('%')), [false, false]);
check('WU "Today" (00:00-03:00)', FCX.p2(L(WU({ day: 'today' }))).b.text.startsWith('Today: '), true);
IMP = true;
check('°F through the page formatter', FCX.p2(L(WU())).b.text, 'Tomorrow: Light rain · 48° / 39°');
IMP = false;
check('the phrase escaped', FCX.p2(L(WU({ phrase: '<b>' }))).b.text.includes('&lt;b&gt;'), true);
let c = FCX.card(ol, p);
check('card: two pages, the warning on page 1 only', [(c.match(/class="olp"/g) || []).length, c.indexOf('Fog reported nearby.') < c.indexOf('class="olp"', c.indexOf('class="olp"') + 1)], [2, true]);
check('card: page 1 is the strip\'s own (dash.js olIn)', c.includes('<div class="olp" aria-hidden="false">' + new Function('esc', 'FCO', 'FC', OL + 'return olIn;')(esc, FCO, { w: (f, t) => t })(ol) + '</div>'), true);
check('card: the WU line ends with its tag, the tooltip naming the source', c.includes('<span class="oln">Tomorrow: Light rain · 9° / 4° <abbr class="tag" title="Weather Underground forecast">WU</abbr></span>'), true);
check('card: no plane glyph, no leading chip', [c.includes('class="src'), c.includes('plane')], [false, false]);
check('card: dots = two tabs, Now first, tooltips name the sources present', [(c.match(/role="tab"/g) || []).length, /aria-selected="true" aria-label="Now"/.test(c), c.includes('aria-label="Next: tomorrow"')], [2, true, true]);
check('card: the i stays, the title "Outlook"', c.includes('id="olWhy"') && c.includes('aria-label="Outlook"'), true);
check('card: WU only -> a neutral, dimmed glyph on page 2', /class="olp" aria-hidden="true"><svg class="dim"/.test(c.slice(c.indexOf('class="olp"', c.indexOf('class="olp"') + 1) - 1)), true);
// ---- TAF only, then both
metar(M(airport({}, { next12: [win('rain', 10, 16), win('thunder', 12, 14, true)] })));
check('a new /api/metar answer re-renders', renders > 0, true);
p = FCX.p2(L(null));
check('TAF only: the LIKELY window, "possible" never on the strip', [p.a.text, p.b], [`Rain likely ${hm(t(10))}–${hm(t(16))}`, null]);
c = FCX.card(ol, p);
check('TAF line: bold, then the ICAO tag with the airport in its tooltip', c.includes(`<span class="oln"><b>Rain likely ${hm(t(10))}–${hm(t(16))}</b> <abbr class="tag" title="Heathrow airport forecast (TAF)">EGLL</abbr></span>`)
  && c.includes('aria-label="Next: airport, 12 h"'), true);
metar(M(airport({}, { next12: [win('rain', 9, 16), win('thunder', 13, 15), win('fog', 6, 8)] })));
check('the highest phenomenon first (thunder > snow > rain > fog > gusts)', FCX.p2(L(null)).a.what, 'thunder');
p = FCX.p2(L(WU()));
check('both: two lines, the tooltip names both', FCX.card(ol, p).includes('aria-label="Next: airport, 12 h, tomorrow"') && !!p.a && !!p.b, true);
// ---- the module turned off while a TAF line is up -- the cached answer does not speak
metar(M(airport({}, { next12: [win('rain', 10, 16)] })));
env.S.status.metar = false;
check('module off with a cached TAF: no line, no sheet section', [FCX.p2(L(null)), FCX.why(L(null))], [null, {}]);
env.S.status.metar = true;
check('module back on: the line again', !!FCX.p2(L(null)).a, true);
// ---- no TAF line: not current, nothing likely, > 60 km, no distance, module off, the second airport's TAF
const none = m => { metar(m); return FCX.p2(L(null)); };
check('TAF not current -> no line', none(M(airport({}, { current: false, next12: [win('rain', 10, 16)] }))), null);
check('no window -> no line', none(M(airport({}, { next12: [] }))), null);
// page 2 shows possible windows too -- likely bold, possible plain, grouped as the sheet
{
  const one = (n12, l) => { metar(M(airport({}, { next12: n12 }))); const q = FCX.p2(L(l || null)); return q && [q.a.text, q.a.k, FCX.card(ol, q).includes(`<b>${q.a.text}</b>`)]; };
  check('A: possible only -> a plain line', one([win('rain', 10, 16, true)]), [`Rain possible ${sp(10, 16)}`, false, false]);
  check('A: likely only -> a bold line', one([win('rain', 10, 16)]), [`Rain likely ${sp(10, 16)}`, true, true]);
  check('A: both, touching -> one grouped line, bold', one([win('rain', 9, 12, true), win('rain', 12, 16)]), [`Rain possible from ${hm(t(9))}, likely ${sp(12, 16)}`, true, true]);
  check('A: a likely group wins over a higher possible one', one([win('thunder', 9, 11, true), win('rain', 12, 14)]), [`Rain likely ${sp(12, 14)}`, true, true]);
  check('A: among possible groups the highest phenomenon', one([win('fog', 6, 8, true), win('snow', 9, 11, true)]), [`Snow possible ${sp(9, 11)}`, false, false]);
}
check('main airport > 60 km -> no line', none(M(airport({ km: 61 }, { next12: [win('rain', 10, 16)] }))), null);
check('main airport without a distance -> no line', none(M(airport({ km: null }, { next12: [win('rain', 10, 16)] }))), null);
check('module off -> no line', none({ on: false, airports: [airport({}, { next12: [win('rain', 10, 16)] })] }), null);
check('never the second airport', none(M(airport({}, { status: 'absent', current: false }), airport({ icao: 'KJFK' }, { next12: [win('rain', 10, 16)] }))), null);
check('a 200 without the TAF replaces the old answer (no caching)', (metar(M(airport({}, { next12: [win('rain', 10, 16)] }))), none(M(airport({ taf: null }, {})))), null);
// ---- the i sheet's Forecast rows -- one list in time order, a row per phenomenon's touching windows
const S = (o, l) => (metar(o), FCX.why(l || L(null)));
let x = S(M(airport({}, { next12: [win('gusts', 13, 19, true), win('rain', 15, 18, true), win('rain', 18, 23)] })), L(WU()));
check('rows: time order, the span, the words, likely bold, the tags', x.rows.map(r => [r.w, r.t, !!r.k, r.tag, r.g]), [
  [sp(13, 19), 'Strong gusts possible', false, 'EGLL', 'wind'],
  [sp(15, 23), `Rain possible from ${hm(t(15))}, likely ${sp(18, 23)}`, true, 'EGLL', 'cloud-rain'],
  ['Tomorrow', 'Light rain, 9° / 4°, 70 % chance of precipitation', false, 'WU', 'cloud-rain']]);
check('rows: the tooltips name the sources', x.rows.map(r => r.tip), ['Heathrow airport forecast (TAF)', 'Heathrow airport forecast (TAF)', 'Weather Underground forecast']);
check('source line: both', x.src, 'Airport windows from the Heathrow (EGLL) forecast; tomorrow from Weather Underground, fetched 16:40. Not this station’s readings.');
x = S(M(airport({}, { next12: [win('rain', 10, 14), win('rain', 14, 17, true)] })));
check('grouping: likely first, then possible until', [x.rows.length, x.rows[0].w, x.rows[0].t, x.rows[0].k], [1, sp(10, 17), `Rain likely ${sp(10, 14)}, possible until ${hm(t(17))}`, true]);
x = S(M(airport({}, { next12: [win('rain', 9, 17, true), win('rain', 11, 13)] })));
// possible on both sides of the likely run -> its own whole template with "possible until"
check('grouping: a likely window inside a possible one', x.rows.map(r => [r.w, r.t]), [[sp(9, 17), `Rain possible from ${hm(t(9))}, likely ${sp(11, 13)}, possible until ${hm(t(17))}`]]);
x = S(M(airport({}, { next12: [win('rain', 9, 13, true), win('rain', 11, 15)] })));
check('grouping: possible first, then likely to the end (pl)', x.rows.map(r => [r.w, r.t]), [[sp(9, 15), `Rain possible from ${hm(t(9))}, likely ${sp(11, 15)}`]]);
// a possible-only gap between two likely windows is never called likely -- two rows
x = S(M(airport({}, { next12: [win('rain', 10, 12), win('rain', 12, 14, true), win('rain', 14, 16)] })));
check('likely - possible - likely -> two rows', x.rows.map(r => [r.w, r.t, r.k]), [[sp(10, 14), `Rain likely ${sp(10, 12)}, possible until ${hm(t(14))}`, true], [sp(14, 16), 'Rain likely', true]]);
x = S(M(airport({}, { next12: [win('rain', 9, 17, true), win('rain', 10, 11), win('rain', 15, 16)] })));
check('two likely windows inside one possible -> the second its own row', x.rows.map(r => [r.w, r.t]),
  [[sp(9, 17), `Rain possible from ${hm(t(9))}, likely ${sp(10, 11)}, possible until ${hm(t(17))}`], [sp(15, 16), 'Rain likely']]);
x = S(M(airport({}, { next12: [win('rain', 10, 12), win('rain', 12, 14)] })));
check('touching likely windows stay one row', x.rows.map(r => [r.w, r.t]), [[sp(10, 14), 'Rain likely']]);
x = S(M(airport({}, { next12: [win('rain', 10, 16), win('rain', 12, 14, true)] })));
check('grouping: a possible window inside a likely one says likely', x.rows.map(r => [r.w, r.t, r.k]), [[sp(10, 16), 'Rain likely', true]]);
x = S(M(airport({}, { next12: [win('rain', 6, 8), win('rain', 10, 12), win('snow', 7, 9, true)] })));
check('grouping: windows apart stay apart; another phenomenon its own row', x.rows.map(r => [r.w, r.t]), [[sp(6, 8), 'Rain likely'], [sp(7, 9), 'Snow possible'], [sp(10, 12), 'Rain likely']]);
check('source line: the airport only', x.src, 'Airport windows from the Heathrow (EGLL) forecast. Not this station’s readings.');
x = S(null, L(WU()));
check('WU only: one row, its source line', [x.rows.length, x.src], [1, 'Tomorrow from Weather Underground, fetched 16:40. Not this station’s readings.']);
x = S(M(airport({}, { next12: [win('rain', 1, 5)] })), L(WU({ day: 'today', t: t(2, 11) })));
check('Today (00-03): a night window before WU\'s day (07:00), "today" in the source line', [x.rows.map(r => r.w), x.src],
  [[sp(1, 5), 'Today'], 'Airport windows from the Heathrow (EGLL) forecast; today from Weather Underground, fetched 02:11. Not this station’s readings.']);
check('Today, WU only', S(null, L(WU({ day: 'today' }))).src.startsWith('Today from Weather Underground, fetched '), true);
check('WU row without a chance', S(null, L(WU({ pop: null, phrase: 'AM Clouds/PM Sun' }))).rows[0].t, 'Morning clouds, afternoon sun, 9° / 4°');
check('a current TAF with nothing in it and no WU: no Forecast section', S(M(airport({}, { next12: [] }))), {});
// a window cut at the 12-h horizon (the board's now + 12 h, off the whole hour) has no end time
const tc = (h, m) => new Date(2026, 10, 16, h, m).toISOString();
x = S(M(airport({}, { next12: [{ what: 'rain', possible: true, from: t(16), to: tc(25, 54), cut: true }] })));
check('horizon: the sheet spans "from", the words have no end', [x.rows[0].w, x.rows[0].t], [`from ${hm(t(16))}`, 'Rain possible']);
x = S(M(airport({}, { next12: [{ what: 'rain', possible: true, from: t(14), to: t(16) }, { what: 'rain', possible: false, from: t(16), to: tc(25, 54), cut: true }] })));
check('horizon: possible, then likely to the horizon', [x.rows[0].w, x.rows[0].t], [`from ${hm(t(14))}`, `Rain possible from ${hm(t(14))}, likely from ${hm(t(16))}`]);
x = S(M(airport({}, { next12: [{ what: 'rain', possible: false, from: t(14), to: t(16) }, { what: 'rain', possible: true, from: t(16), to: tc(25, 54), cut: true }] })));
check('horizon: likely, then possible to the horizon', x.rows[0].t, `Rain likely ${sp(14, 16)}, then possible`);
x = S(M(airport({}, { next12: [{ what: 'rain', possible: true, from: t(12), to: t(14) }, { what: 'rain', possible: false, from: t(14), to: t(16) }, { what: 'rain', possible: true, from: t(16), to: tc(25, 54), cut: true }] })));
check('horizon: possible, likely, then possible to the horizon', x.rows[0].t, `Rain possible from ${hm(t(12))}, likely ${sp(14, 16)}, then possible`);
metar(M(airport({}, { next12: [{ what: 'rain', possible: false, from: t(16), to: tc(25, 54), cut: true }] })));
check('horizon: page 2 "likely from"', FCX.p2(L(null)).a.text, `Rain likely from ${hm(t(16))}`);
metar(M(airport({}, { next12: [{ what: 'rain', possible: true, from: t(16), to: tc(25, 54), cut: true }] })));
check('horizon: page 2 "possible from"', FCX.p2(L(null)).a.text, `Rain possible from ${hm(t(16))}`);
check('horizon: a whole-hour end is a real end', S(M(airport({}, { next12: [win('rain', 10, 12, true)] }))).rows[0].w, sp(10, 12));
// a forecast shown late (a missed hourly cycle): the data stops on a whole hour, the board's cut flag still says so
check('horizon: a cut window ending on the hour has no end time', S(M(airport({}, { next12: [Object.assign(win('rain', 10, 12, true), { cut: true })] }))).rows[0].w, `from ${hm(t(10))}`);
// a group that began before now (its first window carries the board's started flag: from = now) prints no start time
{ const st = (what, possible, from, to, cut) => Object.assign({ what, possible, from, to, started: true }, cut ? { cut: true } : {});
  x = S(M(airport({}, { next12: [st('rain', true, t(12, 20), tc(23, 20), true)] })));
  check('started + cut: the sheet says "throughout"', [x.rows[0].w, x.rows[0].t], ['throughout', 'Rain possible']);
  metar(M(airport({}, { next12: [st('rain', true, t(12, 20), tc(23, 20), true)] })));
  check('started + cut: page 2', FCX.p2(L(null)).a.text, 'Rain possible throughout');
  x = S(M(airport({}, { next12: [st('rain', false, t(12, 20), t(15))] })));
  check('started, ends inside: the sheet says "until"', [x.rows[0].w, x.rows[0].t], [`until ${hm(t(15))}`, 'Rain likely']);
  metar(M(airport({}, { next12: [st('rain', false, t(12, 20), t(15))] })));
  check('started, ends inside: page 2', FCX.p2(L(null)).a.text, `Rain likely until ${hm(t(15))}`);
  // the touching-window sentences: the started part without its start time
  const g = n12 => S(M(airport({}, { next12: n12 }))).rows[0].t;
  check('started: possible, then likely', g([st('rain', true, t(12, 20), t(14)), { what: 'rain', possible: false, from: t(14), to: t(16) }]), `Rain possible now, likely ${sp(14, 16)}`);
  check('started: possible, then likely to the horizon', g([st('rain', true, t(12, 20), t(14)), { what: 'rain', possible: false, from: t(14), to: tc(23, 20), cut: true }]),
    `Rain possible now, likely from ${hm(t(14))}`);
  check('started: possible, likely, possible', g([st('rain', true, t(12, 20), t(14)), { what: 'rain', possible: false, from: t(14), to: t(16) }, { what: 'rain', possible: true, from: t(16), to: t(18) }]),
    `Rain possible now, likely ${sp(14, 16)}, possible until ${hm(t(18))}`);
  check('started: possible, likely, possible to the horizon', g([st('rain', true, t(12, 20), t(14)), { what: 'rain', possible: false, from: t(14), to: t(16) }, { what: 'rain', possible: true, from: t(16), to: tc(23, 20), cut: true }]),
    `Rain possible now, likely ${sp(14, 16)}, then possible`);
  check('started: likely, then possible', g([st('rain', false, t(12, 20), t(14)), { what: 'rain', possible: true, from: t(14), to: t(16) }]), `Rain likely until ${hm(t(14))}, possible until ${hm(t(16))}`);
  check('started: likely, then possible to the horizon', g([st('rain', false, t(12, 20), t(14)), { what: 'rain', possible: true, from: t(14), to: tc(23, 20), cut: true }]),
    `Rain likely until ${hm(t(14))}, then possible`);
  check('not started: unchanged', g([{ what: 'rain', possible: true, from: t(14), to: t(16), started: false }, { what: 'rain', possible: false, from: t(16), to: t(18) }]),
    `Rain possible from ${hm(t(14))}, likely ${sp(16, 18)}`); }
// a span that reaches the next local day says so, as the airport view does (relative to the real today)
{ const d = (dd, h) => { const x = new Date(); x.setDate(x.getDate() + dd); x.setHours(h, 0, 0, 0); return x.toISOString(); };
  const r = n12 => S(M(airport({}, { next12: n12 }))).rows.map(x => x.w);
  check('across midnight -> "... tomorrow"', r([{ what: 'rain', possible: true, from: d(0, 23), to: d(1, 4) }]), ['23:00–04:00 tomorrow']);
  check('on the next day -> "tomorrow ..."', r([{ what: 'rain', possible: true, from: d(1, 2), to: d(1, 5) }]), ['tomorrow 02:00–05:00']);
  check('an end at local midnight reads 00:00', r([{ what: 'rain', possible: true, from: d(0, 21), to: d(1, 0) }]), ['21:00–00:00']);
  // a started group that ends on the next local day says "tomorrow"; one ending at local midnight reads 00:00
  check('started across midnight -> "until ... tomorrow"', r([{ what: 'rain', possible: true, from: d(0, 23), to: d(1, 4), started: true }]), ['until 04:00 tomorrow']);
  check('started, an end at local midnight reads 00:00', r([{ what: 'rain', possible: true, from: d(0, 23), to: d(1, 0), started: true }]), ['until 00:00']); }
// page 2's line carries the sheet's day words on a span that reaches the next local day
{ const d = (dd, h) => { const x = new Date(); x.setDate(x.getDate() + dd); x.setHours(h, 0, 0, 0); return x.toISOString(); };
  metar(M(airport({}, { next12: [{ what: 'rain', possible: false, from: d(0, 23), to: d(1, 4) }] })));
  check('page 2 across midnight -> "... tomorrow"', FCX.p2(L(null)).a.text, 'Rain likely 23:00–04:00 tomorrow');
  metar(M(airport({}, { next12: [{ what: 'rain', possible: true, from: d(1, 2), to: d(1, 5) }] })));
  check('page 2 on the next day -> "tomorrow ..."', FCX.p2(L(null)).a.text, 'Rain possible tomorrow 02:00–05:00');
  // page 2, a started group across midnight / to local midnight
  metar(M(airport({}, { next12: [{ what: 'rain', possible: false, from: d(0, 23), to: d(1, 4), started: true }] })));
  check('page 2 started across midnight', FCX.p2(L(null)).a.text, 'Rain likely until 04:00 tomorrow');
  metar(M(airport({}, { next12: [{ what: 'rain', possible: false, from: d(0, 23), to: d(1, 0), started: true }] })));
  check('page 2 started to local midnight', FCX.p2(L(null)).a.text, 'Rain likely until 00:00'); }
// on a DST day (25 Oct: 25 h, 29 Mar: 23 h) an end at the next local day's 00:00 reads 00:00 (local calendar units),
// an end after it says "tomorrow" -- the sheet's span column and page 2
{ const L0 = (y, mo, d, h) => new Date(y, mo, d, h).toISOString();
  for (const [lab, y, mo, d] of [['25 Oct', 2026, 9, 25], ['29 Mar', 2026, 2, 29]]) {
    NOW = new Date(y, mo, d, 20, 0).getTime();
    const w2 = (e, st) => [Object.assign({ what: 'rain', possible: false, from: L0(y, mo, d, st ? 20 : 21), to: e }, st ? { started: true } : {})];
    const row = n12 => S(M(airport({}, { next12: n12 }))).rows[0].w, pg = n12 => (metar(M(airport({}, { next12: n12 }))), FCX.p2(L(null)).a.text);
    check('' + lab + ': the sheet, to local midnight / after it', [row(w2(L0(y, mo, d + 1, 0))), row(w2(L0(y, mo, d + 1, 2))), row(w2(L0(y, mo, d + 1, 0), 1)), row(w2(L0(y, mo, d + 1, 2), 1))],
      ['21:00–00:00', '21:00–02:00 tomorrow', 'until 00:00', 'until 02:00 tomorrow']);
    check('' + lab + ': page 2, to local midnight / after it', [pg(w2(L0(y, mo, d + 1, 0))), pg(w2(L0(y, mo, d + 1, 2))), pg(w2(L0(y, mo, d + 1, 0), 1))],
      ['Rain likely 21:00–00:00', 'Rain likely 21:00–02:00 tomorrow', 'Rain likely until 00:00']);
  }
  NOW = null; }
// the touching-window sentences take their time phrases from the span words; the first phrase on the next local day
// says "tomorrow" (a start on it before the times, an end on it after them; an end at 00:00 reads 00:00), the later ones do not
// repeat it. Every order x (all today / midnight inside the likely part / after the possible start / after the likely part / all
// tomorrow / an end at local midnight), plain and began-before-now, on the sheet's words and page 2; "now" = 16 Nov 18:00 local
{ const FORMS = {  // [possible, from, to, cut] per window, A..D the boundaries
    pl: ([A, B, C]) => [[1, A, B], [0, B, C]], plo: ([A, B, C]) => [[1, A, B], [0, B, C, 1]],
    plp: ([A, B, C, D]) => [[1, A, B], [0, B, C], [1, C, D]], plpo: ([A, B, C, D]) => [[1, A, B], [0, B, C], [1, C, D, 1]],
    lp: ([A, B, C]) => [[0, A, B], [1, B, C]], lpo: ([A, B, C]) => [[0, A, B], [1, B, C, 1]] };
  const run = (y, mo, d, form, T, st) => {
    const H = h => new Date(y, mo, d, h).toISOString();  // local hour h of the day (24 and later: the next day)
    const n12 = FORMS[form](T).map(([p, a, b, c], i) => Object.assign({ what: 'rain', possible: !!p, from: H(a), to: H(b) }, c ? { cut: true } : {}, st && !i ? { started: true } : {}));
    const t = S(M(airport({}, { next12: n12 }))).rows[0].t;
    metar(M(airport({}, { next12: n12 })));
    return [t, FCX.p2(L(null)).a.text];
  };
  const CASES = [
    // all today
    ['pl', [19, 20, 21], 0, 'Rain possible from 19:00, likely 20:00–21:00'], ['plo', [19, 20, 21], 0, 'Rain possible from 19:00, likely from 20:00'],
    ['plp', [19, 20, 21, 22], 0, 'Rain possible from 19:00, likely 20:00–21:00, possible until 22:00'],
    ['plpo', [19, 20, 21, 22], 0, 'Rain possible from 19:00, likely 20:00–21:00, then possible'],
    ['lp', [19, 20, 21], 0, 'Rain likely 19:00–20:00, possible until 21:00'], ['lpo', [19, 20, 21], 0, 'Rain likely 19:00–20:00, then possible'],
    // local midnight inside the likely part
    ['pl', [22, 23, 25], 0, 'Rain possible from 22:00, likely 23:00–01:00 tomorrow'], ['plo', [22, 23, 25], 0, 'Rain possible from 22:00, likely from 23:00'],
    ['plp', [22, 23, 25, 27], 0, 'Rain possible from 22:00, likely 23:00–01:00 tomorrow, possible until 03:00'],
    ['plpo', [22, 23, 25, 27], 0, 'Rain possible from 22:00, likely 23:00–01:00 tomorrow, then possible'],
    ['lp', [22, 25, 27], 0, 'Rain likely 22:00–01:00 tomorrow, possible until 03:00'], ['lpo', [22, 25, 27], 0, 'Rain likely 22:00–01:00 tomorrow, then possible'],
    // after the first phrase: the likely part all tomorrow
    ['pl', [23, 25, 26], 0, 'Rain possible from 23:00, likely tomorrow 01:00–02:00'], ['plo', [23, 25, 26], 0, 'Rain possible from 23:00, likely tomorrow from 01:00'],
    ['plp', [23, 25, 26, 27], 0, 'Rain possible from 23:00, likely tomorrow 01:00–02:00, possible until 03:00'],
    ['plpo', [23, 25, 26, 27], 0, 'Rain possible from 23:00, likely tomorrow 01:00–02:00, then possible'],
    // after the likely part: only the possible end tomorrow
    ['plp', [21, 22, 23, 26], 0, 'Rain possible from 21:00, likely 22:00–23:00, possible until 02:00 tomorrow'],
    ['lp', [21, 22, 26], 0, 'Rain likely 21:00–22:00, possible until 02:00 tomorrow'],
    // all tomorrow: the first phrase says it once
    ['pl', [25, 26, 27], 0, 'Rain possible tomorrow from 01:00, likely 02:00–03:00'], ['plo', [25, 26, 27], 0, 'Rain possible tomorrow from 01:00, likely from 02:00'],
    ['plp', [25, 26, 27, 28], 0, 'Rain possible tomorrow from 01:00, likely 02:00–03:00, possible until 04:00'],
    ['plpo', [25, 26, 27, 28], 0, 'Rain possible tomorrow from 01:00, likely 02:00–03:00, then possible'],
    ['lp', [25, 26, 27], 0, 'Rain likely tomorrow 01:00–02:00, possible until 03:00'], ['lpo', [25, 26, 27], 0, 'Rain likely tomorrow 01:00–02:00, then possible'],
    // an end at local midnight reads 00:00 and leaves the day word to the next phrase
    ['plp', [21, 22, 24, 26], 0, 'Rain possible from 21:00, likely 22:00–00:00, possible until 02:00 tomorrow'],
    ['lp', [21, 22, 24], 0, 'Rain likely 21:00–22:00, possible until 00:00'],
    // began before now (the first window started at 18:00 = now)
    ['pl', [18, 20, 21], 1, 'Rain possible now, likely 20:00–21:00'], ['plo', [18, 20, 21], 1, 'Rain possible now, likely from 20:00'],
    ['plp', [18, 20, 21, 22], 1, 'Rain possible now, likely 20:00–21:00, possible until 22:00'],
    ['plpo', [18, 20, 21, 22], 1, 'Rain possible now, likely 20:00–21:00, then possible'],
    ['lp', [18, 20, 21], 1, 'Rain likely until 20:00, possible until 21:00'], ['lpo', [18, 20, 21], 1, 'Rain likely until 20:00, then possible'],
    ['pl', [18, 23, 25], 1, 'Rain possible now, likely 23:00–01:00 tomorrow'], ['plo', [18, 25, 26], 1, 'Rain possible now, likely tomorrow from 01:00'],
    ['plp', [18, 23, 25, 27], 1, 'Rain possible now, likely 23:00–01:00 tomorrow, possible until 03:00'],
    ['plpo', [18, 25, 26, 27], 1, 'Rain possible now, likely tomorrow 01:00–02:00, then possible'],
    ['lp', [18, 25, 26], 1, 'Rain likely until 01:00 tomorrow, possible until 02:00'], ['lpo', [18, 25, 26], 1, 'Rain likely until 01:00 tomorrow, then possible'],
    ['lp', [18, 23, 26], 1, 'Rain likely until 23:00, possible until 02:00 tomorrow']];
  NOW = new Date(2026, 10, 16, 18).getTime();
  for (const [form, T, st, want] of CASES) check(`${form}${st ? ' started' : ''} ${T.join('/')}: the sheet and page 2`, run(2026, 10, 16, form, T, st), [want, want]);
  // the DST nights in Europe/Paris (25 Oct: 25 h, 29 Mar: 23 h): an end at local midnight still reads 00:00
  for (const [lab, y, mo, d] of [['25 Oct', 2026, 9, 25], ['29 Mar', 2026, 2, 29]]) {
    NOW = new Date(y, mo, d, 18).getTime();
    check('DST ' + lab, [run(y, mo, d, 'plp', [21, 22, 24, 26], 0)[0], run(y, mo, d, 'lp', [21, 22, 25], 0)[0], run(y, mo, d, 'pl', [23, 25, 26], 0)[0]],
      ['Rain possible from 21:00, likely 22:00–00:00, possible until 02:00 tomorrow', 'Rain likely 21:00–22:00, possible until 01:00 tomorrow',
        'Rain possible from 23:00, likely tomorrow 01:00–02:00']);
  }
  NOW = null; }
check('escaped', S(M(airport({ name: 'A<b>', icao: 'X<' }, { next12: [win('rain', 10, 12)] }))).rows[0].tip + S(M(airport({ name: 'A<b>', icao: 'X<' }, { next12: [win('rain', 10, 12)] }))).rows[0].tag, 'A&lt;b&gt; airport forecast (TAF)X&lt;');
// ---- the sheet (outlook-why.js draws FCX.why's rows)
const LUCIDE = { gauge: '', thermometer: '', droplet: '' };
const FCW = new Function('FCO', 'LUCIDE', 'own', 'FC', HM + rd('outlook-why.js') + '\nreturn FCW;')(FCO, LUCIDE, (o, k) => (Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined),
  { w: (f, t) => t });
const o = { v: 1, t: t(10, 55), state: 'dry', next: 'p_steady', warn: null, qual: 'freezing', in: { night: false, t: -1.2 }, p: {}, tend: { d: 0.4 } };
x = S(M(airport({}, { next12: [win('gusts', 13, 19, true), win('rain', 15, 18, true), win('rain', 18, 23)] })), L(WU()));
const h = FCW.why(o, FCW.M, x), at = s => h.indexOf(s);
check('sheet: "Now · this station’s readings, {t}" heads the blocks, no "No forecast model"', [h.startsWith('<p class="cap why-h">Now · this station’s readings, 10:55</p>'), h.includes('No forecast model')], [true, false]);
check('sheet: then ONE Forecast list after the blocks', [at('<div class="blk') < at('>Forecast</p>'), (h.match(/>Forecast<\/p>/g) || []).length, (h.match(/class="blk fcr"/g) || []).length], [true, 1, 3]);
check('sheet: a row = glyph, the span bold, the words (likely bold), the tag with its tooltip',
  h.includes(`<b>${sp(15, 23)}</b><p><b>Rain possible from ${hm(t(15))}, likely ${sp(18, 23)}</b></p><abbr class="tag" title="Heathrow airport forecast (TAF)">EGLL</abbr></div>`)
  && h.includes(`<b>${sp(13, 19)}</b><p>Strong gusts possible</p>`), true);
check('sheet: the one source line at the end; the old loose lines gone', [h.endsWith('<p class="cap ft">Airport windows from the Heathrow (EGLL) forecast; tomorrow from Weather Underground, fetched 16:40. Not this station’s readings.</p>'),
  /Fetched \d|The airport’s forecast, not|Next 12 h ·|· Weather Underground/.test(h)], [true, false]);
check('sheet: the sun caveat is the note of the temperature block', /<b>Freezing[^]*?<p class="nt">In direct sun with light wind the sensor can read high\.<\/p><\/div>/.test(h) && (h.match(/direct sun/g) || []).length === 1, true);
const ho = FCW.why(Object.assign({}, o, { qual: null }), FCW.M, x);
check('sheet: no temperature block, no sun caveat', ho.includes('direct sun'), false);
const hs = FCW.why(o, FCW.M, {});
check('sheet, station alone: the one head line, no headers', [hs.startsWith('<p class="cap">From this station’s own readings, as of 10:55. No forecast model.</p>'), /why-h|Forecast</.test(hs)], [true, false]);
check('sheet, no x at all: as alone', FCW.why(o, FCW.M, null) === hs, true);
check('sheet escaped nothing unfilled', /\{\w+\}/.test(h), false);
// ---- a poll's re-render keeps the page and the ring's phase (wire() on fake nodes, the clock stubbed)
{
  const hov = { v: false };  // the card under the pointer / holding the focus (:hover, :focus-within)
  const mk = () => {
    const ls = {}, set = function (k, v) { this.a[k] = String(v); };
    const D = [0, 1].map(() => ({ a: {}, setAttribute: set, addEventListener() {}, firstChild: { firstChild: { style: {} } } }));
    const P = [0, 1].map(() => ({ a: {}, setAttribute: set }));
    return { ol: { classList: { add() {} }, dataset: {}, matches: () => hov.v, addEventListener: (k, fn) => { ls[k] = fn; }, querySelectorAll: q => (q === '.olp' ? P : D) }, P, D, ls };
  };
  const now0 = Date.now;
  let T = 1e12;
  Date.now = () => T;
  const g1 = mk();
  env.document.querySelector = () => g1.ol;
  FCX.wire();
  g1.ls.animationend({ animationName: 'olring' });  // the ring's end turns the page, the ring starts again
  check('ring end: page 2 shown, its ring from the start', [g1.P[1].a['aria-hidden'], g1.D[1].a['aria-selected'], g1.D[1].firstChild.firstChild.style.animationDelay], ['false', 'true', '']);
  T += 2000;
  const g2 = mk();  // a poll re-renders the grid 2 s later: new nodes
  env.document.querySelector = () => g2.ol;
  FCX.wire();
  check('re-render keeps the page and the ring phase (-2 s)', [g2.P[0].a['aria-hidden'], g2.P[1].a['aria-hidden'], g2.D[1].firstChild.firstChild.style.animationDelay, g2.D[0].firstChild.firstChild.style.animationDelay],
    ['true', 'false', '-2000ms', '']);
  T += 7000;
  const g3 = mk();
  env.document.querySelector = () => g3.ol;
  FCX.wire();
  check('a late re-render: the phase capped just before the end (the turn still fires)', g3.D[1].firstChild.firstChild.style.animationDelay, '-4900ms');
  // a hover pauses the ring (CSS); a re-render during the pause restores the phase it had when the pause began, and the
  // paused span moves the start on, so the turn comes 5 s of running time after the start, not 5 s of clock time
  g3.ls.animationend({ animationName: 'olring' });  // page 1 again, a fresh ring at T
  const t0 = T;
  T += 1000; hov.v = true; (g3.ls.pointerenter || Object)();  // hovered after 1 s
  T += 2000;
  const g4 = mk();  // a poll re-renders during the hover (3 s on the clock, 1 s on the ring)
  env.document.querySelector = () => g4.ol;
  FCX.wire();
  check('re-render during a hover: the phase the ring had at the pause (-1 s), not -3 s', g4.D[0].firstChild.firstChild.style.animationDelay, '-1000ms');
  T += 1000; hov.v = false; (g4.ls.pointerleave || Object)();  // left after 3 s of pause
  T += 500;
  const g5 = mk();
  env.document.querySelector = () => g5.ol;
  FCX.wire();
  check('after the pause: 1.5 s of running time (4.5 s on the clock)', [g5.D[0].firstChild.firstChild.style.animationDelay, T - t0], ['-1500ms', 4500]);
  // a re-render that lands under a resting pointer (no pointerenter on the new card) starts the pause too
  hov.v = true; T += 500;
  const g6 = mk();
  env.document.querySelector = () => g6.ol;
  FCX.wire();
  T += 4000;
  const g7 = mk();
  env.document.querySelector = () => g7.ol;
  FCX.wire();
  check('a new card already hovered: paused from its render (-2 s held)', g7.D[0].firstChild.firstChild.style.animationDelay, '-2000ms');
  hov.v = false;
  const g8 = mk();
  env.document.querySelector = () => g8.ol;
  FCX.wire();  // not hovered any more: the pause closes at the render
  T += 1000;
  const g9 = mk();
  env.document.querySelector = () => g9.ol;
  FCX.wire();
  check('the pause closed by a render without the hover: running again (-3 s)', g9.D[0].firstChild.firstChild.style.animationDelay, '-3000ms');
  Date.now = now0;
}
// ---- the cross-fade: a page turn flips each page's aria-hidden once (app.css: .olp transitions on it); the card's markup
// carries the shown page, so wire() on a re-render's new nodes flips nothing and no fade starts
{
  const flips = [];
  const mk = () => {
    const P = [...FCX.card(ol, FCX.p2(L(WU()))).matchAll(/class="olp" aria-hidden="(\w+)"/g)].map((m, k) => ({ a: { 'aria-hidden': m[1] },
      setAttribute(n, v) { v = String(v); if (this.a[n] !== v) flips.push(k + ':' + v); this.a[n] = v; } }));
    const D = [0, 1].map(() => ({ setAttribute() {}, addEventListener() {}, firstChild: { firstChild: { style: {} } } })), ls = {};
    return { ol: { classList: { add() {} }, dataset: {}, matches: () => false, addEventListener: (k, fn) => { ls[k] = fn; }, querySelectorAll: q => (q === '.olp' ? P : D) }, P, ls };
  };
  const g1 = mk();
  env.document.querySelector = () => g1.ol;
  FCX.wire();
  check('fade: the card marks one page shown', g1.P.map(x => x.a['aria-hidden']).sort(), ['false', 'true']);
  check('fade: same words re-rendered -> no flip', flips, []);
  g1.ls.animationend({ animationName: 'olring' });
  check('fade: a page turn flips each page once', flips.map(f => f[0]).sort(), ['0', '1']);
  flips.length = 0;
  const g2 = mk();  // a poll after the turn: the markup has the new page
  env.document.querySelector = () => g2.ol;
  FCX.wire();
  check('fade: a poll after the turn -> no flip', flips, []);
}
// a poll that changes the shown words: dash.js olFade puts .fd on for one style pass (the transition back runs in CSS)
{
  const [olNow, olFade] = new Function('$', ['olNow', 'olFade'].map(n => decl(rd('dash.js'), n)).join('\n') + '\nreturn [olNow, olFade];')(
    s => s === '#grid' && { querySelector: q => (q === '.ol>.olt,[aria-hidden=false]>.olt' ? { previousElementSibling: { outerHTML: '<svg g1/>' }, outerHTML: '<div class="olt">x</div>' } : null) });
  const o = { log: [], classList: { add: c => o.log.push('+' + c), remove: c => o.log.push('-' + c) }, get offsetWidth() { o.log.push('style'); return 0; } };
  check('olNow: the shown glyph + words (plain strip or the card\'s shown page)', olNow(), '<svg g1/><div class="olt">x</div>');
  const run = (a, b) => (o.log = [], olFade(a, b, o), o.log);
  check('olFade: same words -> no class', run('<svg/>A', '<svg/>A'), []);
  check('olFade: the first strip -> no fade', run(null, '<svg/>A'), []);
  check('olFade: the strip gone -> nothing', run('<svg/>A', null), []);
  check('olFade: new words -> .fd once, a style pass between', run('<svg/>A', '<svg/>B'), ['+fd', 'style', '-fd']);
  check('olFade: a new glyph alone fades too', run('<svg a/>A', '<svg b/>A'), ['+fd', 'style', '-fd']);
}
{
  const app = rd('app.css'), rm = (/@media \(prefers-reduced-motion: reduce\) \{ \.tile[^\n]*/.exec(app) || [''])[0];
  check('fade CSS: new words from a poll 180 ms, .fd without a transition', [app.includes('.ol > svg, .olp > svg, .olt { transition: opacity .18s; }'),
    app.includes('.fd > svg, .fd .olp > svg, .fd .olt { opacity: 0; transition: none; }')], [true, true]);
  // a page turn never shows both pages: the outgoing one 1 -> 0 in [0, 120 ms] and hidden at 120 ms; the incoming one hidden and at 0
  // until 120 ms, then 0 -> 1 in [120, 240 ms]. Each side's transition is the one on its end state (the CSS rule)
  const T = sel => { const m = new RegExp(sel.replace(/[.[\]"]/g, '\\$&') + ' \\{([^}]*)\\}').exec(app); return m && m[1].trim(); };
  check('fade CSS: the page turn is sequential, 240 ms in all', [T('.olp'), T('.olp[aria-hidden="true"]')],
    ['transition: opacity .12s .12s, visibility 0s .12s;', 'opacity: 0; transition: opacity .12s, visibility 0s .12s;']);
  check('fade CSS: reduced motion -> no transition on any faded part (both page states)', ['.ol > svg', '.olp,', '.olp[aria-hidden="true"],', '.olp > svg', '.olt {'].map(x => rm.includes(x)), [true, true, true, true, true]);
}
// ---- the dots' targets (WCAG 2.5.8 spacing exception: 20-px targets with 8 px between them and to the i)
check('dots: 20 x 24 px targets, 8-px gaps', css.includes('.olpg button{width:20px;height:24px') && css.includes('.olr,.olpg{display:flex;align-items:center;gap:8px}'), true);
check('reduced motion: no ring, no turn', css.includes('@media(prefers-reduced-motion:reduce){[aria-selected=true] .rg{animation:none;opacity:0}}'), true);
// the tag and the rows' CSS live in app.css (dash face), beside .blk
const app = rd('app.css');
check('CSS: the tag, the row, the phone layout', [/abbr\.tag \{[^}]*border: 1px solid var\(--grid\)/.test(app), app.includes('.fcr { grid-template-columns: 24px auto 1fr auto;'), app.includes('@media (max-width: 420px) { .fcr {')], [true, true, true]);

console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
