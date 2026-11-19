// test/tools/outlook_vectors.js -- runs firmware/fauxcloud/web/outlook.js (the outlook strip's words and glyphs)
// under node: every firmware code has a phrase and the states / warnings a glyph, placeholders are filled, an
// unknown code renders nothing. The codes are read from src/forecast.cpp, so a new firmware code without a web phrase
// fails here. Usage: node test/tools/outlook_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud');
const FCO = new Function(fs.readFileSync(path.join(root, 'web', 'outlook.js'), 'utf8') + '\nreturn FCO;')();
const cpp = fs.readFileSync(path.join(root, 'src', 'forecast.cpp'), 'utf8');
const table = name => {
  const m = cpp.match(new RegExp('const char\\* const ' + name + '\\[[A-Z_]+\\] = \\{([^}]*)\\}'));
  return m ? [...m[1].matchAll(/"([a-z_0-9]+)"/g)].map(x => x[1]) : [];
};

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

const states = table('OUT_STATE'), nexts = table('OUT_NEXT'), warns = table('OUT_WARN'), gates = table('OUT_GATE'), quals = table('OUT_QUAL');
check('firmware tables read', [states.length, nexts.length, warns.length, gates.length, quals.length], [13, 9, 8, 2, 6]);  // incl. 3 T3 states / warnings, 3 day words, min_range, fog_possible, sleet_snow
// the qualifier-free states, from the firmware's OUT_STATE_QUAL (false = a T3 state, never paired)
const qm = cpp.match(/const bool OUT_STATE_QUAL\[OS_COUNT\] = \{([^}]*)\}/), sq = qm ? qm[1].split(',').map(x => x.trim() === 'true') : [];
check('OUT_STATE_QUAL read', sq.length, states.length);
const qs = states.filter((c, i) => sq[i]), t3s = states.filter((c, i) => !sq[i]);
check('the qualifier-free states are the T3 ones', t3s, ['wx_snow_near', 'wx_rain_near', 'wx_drizzle_near']);
for (const c of [...states, ...nexts, ...warns, ...quals, 'time']) check('phrase ' + c, typeof FCO.W[c], 'string');
// glyphs: the rain classes and the moon only; every other state takes the dimmed neutral sign
for (const c of states) check('state glyph ' + c, !!FCO.G[FCO.SG[c]], /^(rain_|sleet_snow|night_clear|wx_|day_)/.test(c));  // the day sky words too
// line 1 = state (<= 14) + qualifier (<= 13): every pair fits 28 characters
for (const c of qs) check('state <= 14: ' + c, FCO.W[c].length <= 14, true);
for (const c of quals) check('qualifier <= 13: ' + c, FCO.W[c].length <= 13, true);
check('every line 1 <= 28', Math.max(...qs.flatMap(a => quals.map(b => (FCO.W[a] + ' ' + FCO.W[b]).length))), 28);
for (const c of t3s) check('a T3 state alone <= 28: ' + c, FCO.W[c].length <= 28, true);
for (const c of warns) check('warn glyph ' + c, !!FCO.G[FCO.WG[c]], true);

const base = { v: 1, gate: null, state: 'dry', next: 'p_steady', warn: null, left_h: 0, p: { gust: null }, in: { night: false } };
const L = o => FCO.lines(Object.assign({}, base, o), { gust: g => Math.round(g) + ' km/h' });
check('quiet day', [L({}).l1, L({}).l2, L({}).warn], ['No rain.', 'Pressure steady.', '']);
// the no-rain states and the null state: a neutral sign, cloud-sun by day, cloud-moon at night
check('no-rain day: cloud-sun, dimmed', /class="dim"/.test(L({}).glyph) && L({}).glyph.includes('15.947'), true);
check('no-rain night: cloud-moon, dimmed', /class="dim"/.test(L({ in: { night: true } }).glyph) && L({ in: { night: true } }).glyph.includes('10.188'), true);
check('null state: cloud-sun, dimmed', /class="dim"/.test(L({ gate: 'stale', state: null }).glyph), true);
check('recent rain: neutral sign, dimmed', [L({ state: 'recent_rain' }).l1, /class="dim"/.test(L({ state: 'recent_rain' }).glyph)], ['Recent rain.', true]);
check('rain glyph full ink', /class="dim"/.test(L({ state: 'rain_light' }).glyph), false);
check('clear night: moon', L({ state: 'night_clear' }).glyph.includes('M12 3a6 6'), true);
// the gauge's precipitation at <= 1 °C: one sentence, no intensity word, the snowflake in full ink; the qualifier stays
check('sleet_snow', [L({ state: 'sleet_snow' }).l1, L({ state: 'sleet_snow', qual: 'freezing' }).l1, L({ state: 'sleet_snow', qual: 'severe_frost' }).l1],
  ['Sleet or snow.', 'Sleet or snow. Freezing.', 'Sleet or snow. Severe frost.']);
check('sleet_snow glyph: the snowflake, full ink', [L({ state: 'sleet_snow' }).glyph.includes('m10 20-1.25-2.5L6 18'), /class="dim"/.test(L({ state: 'sleet_snow' }).glyph)], [true, false]);
check('sleet_snow with icy', [L({ state: 'sleet_snow', qual: 'freezing', warn: 'icy' }).warn, L({ state: 'sleet_snow', warn: 'icy' }).wglyph.includes('m10 20-1.25-2.5L6 18')], ['Icy surfaces possible.', true]);
check('collecting fills left_h', FCO.phrase('collecting', { left_h: 3 }), 'Pressure outlook in 3 h.');
// the status lines are not on the strip; alone they hide it
check('collecting: no strip line', L({ next: 'collecting', left_h: 3 }).l2, '');
check('paused: no strip line', L({ state: 'recent_rain', next: 'p_paused' }).l2, '');
check('paused alone: no strip', L({ state: null, next: 'p_paused' }), null);
check('a tendency stays', L({ next: 'p_rising' }).l2, 'Pressure rising.');
check('gusts fill the gust', L({ warn: 'gusts', p: { gust: 41.6 } }).warn, 'Gusts to 42 km/h.');
// the qualifier, its own phrase after the state's
check('freezing appended', L({ qual: 'freezing' }).l1, 'No rain. Freezing.');
check('severe frost appended', L({ state: 'night_clear', qual: 'severe_frost' }).l1, 'Clear night. Severe frost.');
check('hot appended', L({ qual: 'hot' }).l1, 'No rain. Hot.');
check('calm appended', L({ qual: 'calm' }).l1, 'No rain. Calm.');
check('blind cold: qualifier alone', L({ state: null, qual: 'freezing' }).l1, 'Freezing.');
check('no qualifier: state only', L({ qual: null }).l1, 'No rain.');
check('unknown qualifier -> no strip', L({ qual: 'balmy' }), null);
check('empty state line is valid', L({ state: null }).l1, '');
check('stale: next only', !!L({ gate: 'stale', state: null, warn: null }), true);
check('time gate -> no strip', L({ gate: 'time', state: null, next: null }), null);
check('all null -> no strip', L({ state: null, next: null, warn: null }), null);
check('unknown state -> no strip', L({ state: 'fog' }), null);
check('unknown next -> no strip', L({ next: 'p_soaring' }), null);
check('no outlook -> no strip', FCO.lines(null), null);
// every T3 phrase x every qualifier -> the phrase alone (an old or odd body cannot pair them), full ink
for (const c of t3s) for (const q of [null, ...quals]) check('T3 ' + c + ' + ' + q, L({ state: c, qual: q }).l1, FCO.W[c]);
for (const c of t3s) check('T3 glyph full ink ' + c, /class="dim"/.test(L({ state: c }).glyph), false);
// every state x every qualifier -- the qualifier follows the state iff its phrase is <= 14 characters
// (the firmware's OUT_STATE_QUAL): the day words keep it, a T3 state never
let pairs = 0;
for (const [i, c] of states.entries())
  for (const q of quals) {
    check('pair ' + c + ' + ' + q, L({ state: c, qual: q }).l1, sq[i] ? FCO.W[c] + ' ' + FCO.W[q] : FCO.W[c]);
    check('pair flag ' + c, sq[i], FCO.W[c].length <= 14);
    pairs++;
  }
check('every state x qualifier pair', pairs, states.length * quals.length);
for (const c of ['day_sunny', 'day_spells', 'day_nosun']) check('day word keeps the qualifier: ' + c, L({ state: c, qual: 'calm' }).l1, FCO.W[c] + ' Calm.');
check('T3 rain', L({ state: 'wx_rain_near' }).l1, 'Rain reported nearby.');
// the warning slot: the firmware picks the one line by rank (host test); the page shows what it gets
for (const w of ['thunder_near', 'fog_near', 'mist_near']) check('T3 warn ' + w, L({ warn: w }).warn, FCO.W[w]);
check('T3 warn with a local state', [L({ state: 'rain_light', warn: 'fog_near' }).l1, L({ state: 'rain_light', warn: 'fog_near' }).warn], ['Light rain.', 'Fog reported nearby.']);
// degradation (page 1): METAR off while a T3 phrase was up -> the next body has the plain strip, no stale line
{ const up = L({ state: 'wx_drizzle_near', warn: 'mist_near', in: { night: false, near: { icao: 'EGLL', name: 'Heathrow', km: 18, age_s: 1500, auto: false } } });
  const off = L({ state: 'dry', warn: null, in: { night: false, near: null } });
  check('METAR on: T3 lines', [up.l1, up.warn], ['Drizzle reported nearby.', 'Mist reported nearby.']);
  check('METAR off: the plain strip', [off.l1, off.warn, /reported nearby/.test(JSON.stringify(off))], ['No rain.', '', false]); }
// the learners' codes -- the day sky words in full ink (claims), fog with its glyph, the minimum range in °C and °F
check('sunny: sun, full ink', [L({ state: 'day_sunny' }).l1, /class="dim"/.test(L({ state: 'day_sunny' }).glyph), L({ state: 'day_sunny' }).glyph.includes('r="4"')], ['Sunny.', false, true]);
check('spells: cloud-sun, full ink', [L({ state: 'day_spells', qual: 'calm' }).l1, /class="dim"/.test(L({ state: 'day_spells' }).glyph)], ['Sunny spells. Calm.', false]);
check('no sun: cloud, full ink', [L({ state: 'day_nosun' }).l1, /class="dim"/.test(L({ state: 'day_nosun' }).glyph), L({ state: 'day_nosun' }).glyph.includes('M17.5 19H9')], ['No sun.', false, true]);
check('fog warning', [L({ warn: 'fog_possible' }).warn, L({ warn: 'fog_possible' }).wglyph.includes('M16 17H7')], ['Mist or fog possible.', true]);
check('min range, default °C', L({ state: 'night_clear', next: 'min_range', p: { min_lo: -9, min_hi: -5 } }).l2, 'Minimum around −9 to −5 °C tonight.');
const toF = c => Math.round(c * 1.8 + 32);
check('min range in °F (the page formatter)', FCO.lines(Object.assign({}, base, { state: 'night_clear', next: 'min_range', p: { min_lo: -9, min_hi: -5 } }),
  { range: (a, b) => [a, b].map(x => String(toF(x)).replace('-', '−')).join(' to ') + ' °F' }).l2, 'Minimum around 16 to 23 °F tonight.');
check('min range above zero', L({ state: 'night_clear', next: 'min_range', p: { min_lo: 2, min_hi: 5 } }).l2, 'Minimum around 2 to 5 °C tonight.');

// the explanation (outlook-why.js) is whole sentences per variant; every template is reached and filled
const LUCIDE = { gauge: '<path d="gauge"/>', thermometer: '<path d="thermometer"/>', droplet: '<path d="droplet"/>' };
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;  // core.js's (a page global)
// the page's one clock-time helper (health.js), ahead of the source under test
const HM = (h => { const { decl } = require('./js_decl'); return ['p2', 'hm', 'dm', 'WH', 'dayTime'].map(n => decl(h, n)).join('\n') + '\n'; })(fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'health.js'), 'utf8'));
const FCW = new Function('FCO', 'LUCIDE', 'own', 'FC', HM + fs.readFileSync(path.join(root, 'web', 'outlook-why.js'), 'utf8') + '\nreturn FCW;')(FCO, LUCIDE, own,
  { w: (f, t) => t, t: (s, o) => s.replace(/\{(\w+)\}/g, (m, k) => k in o ? o[k] : m) });  // core.js's FC.w (English) and FC.t
const M = FCW.M, seen = new Set(), inp = { night: true, t: 0.5, td: -1.2, rate: 1.2, gust10: 31, wind_h: 2, calm: true, dt_h: -1.6, rain12: 0.4, cool_at: '2026-11-10T17:00:00Z', rh: 93, wind10: 22.4 };
const cases = [
  { state: 'rain_light', p: { since: '2026-11-10T16:00:00Z' } }, { state: 'rain_light', p: {} }, { state: 'night_clear', qual: 'calm' },
  { state: 'sleet_snow', qual: 'freezing', warn: 'icy', p: { since: '2026-11-10T16:00:00Z' } }, { state: 'sleet_snow', p: {} },
  { state: 'dry', in: Object.assign({}, inp, { rain12: 0 }) }, { state: 'dry' }, { state: 'recent_rain' }, { state: null },
  { qual: 'windy' }, { qual: 'humid' },
  { next: 'p_falling', tend: { d: -2.1, tide: true } }, { next: 'p_steady', tend: { d: 0.4, tide: false } }, { next: 'cooling_fast' },
  { next: 'collecting', left_h: 2 }, { next: 'p_paused' }, { warn: 'icy' }, { warn: 'gusts', p: { gust: 41 } }, { warn: 'hoar_frost' },
  { warn: 'ground_frost' }, { gate: 'stale', state: null }, { qual: 'freezing' }, { qual: 'severe_frost' }, { qual: 'hot' },
  // the source sentence under a sourced state and a sourced warning
  { state: 'wx_snow_near', warn: 'thunder_near', in: Object.assign({}, inp, { near: { icao: 'EGLL', name: 'Heathrow', km: 18, age_s: 1500, auto: false } }) },
  // the learners' codes and the "uses what this station learned" sentences
  { state: 'day_sunny', in: Object.assign({}, inp, { learn: ['l1'] }) }, { state: 'day_spells' }, { state: 'day_nosun' },
  { warn: 'fog_possible', in: Object.assign({}, inp, { learn: ['l2'], rh_sat: 93, rh_rel: 98.4 }) },
  { state: 'night_clear', next: 'min_range', p: { min_lo: -9, min_hi: -5 }, in: Object.assign({}, inp, { learn: ['l5', 'l6'], cool_thr: 0.8 }) },
];
// the admin draws the blocks too (the Learning sheet's style); no sentence list or its templates
check('no sentence list left', FCW.lines, undefined);
check('the sentences drawn', Object.keys(FCW.P), ['neutral', 'sun', 'u_l1', 'u_l2', 'u_l5', 'u_l6']);
// one sentence per learner in use, after the blocks; none without
// a learner's note once per sheet (the fog warning and the humid qualifier both use l2)
check('l2 note once', (FCW.why({ warn: 'fog_possible', qual: 'humid', in: Object.assign({}, inp, { learn: ['l2'], rh_rel: 98 }), p: {} }, Object.assign({}, M, { g: v => v + ' km/h' })).match(/Uses what this station learned about its humidity sensor/g) || []).length, 1);
check('why: learner sentences', (FCW.why({ state: 'night_clear', next: 'min_range', p: { min_lo: -9, min_hi: -5 }, in: Object.assign({}, inp, { learn: ['l5', 'l6'], cool_thr: 0.8 }) }, Object.assign({}, M, { g: v => v + ' km/h' })).match(/Uses what this station learned/g) || []).length, 2);
check('why: no learner sentence by default', /Uses what this station/.test(FCW.why({ state: 'night_clear', in: inp, p: {} }, Object.assign({}, M, { g: v => v + ' km/h' }))), false);
check('why: the learned cooling threshold', FCW.blocks({ state: 'night_clear', in: Object.assign({}, inp, { cool_thr: 0.8 }), p: {} }, M)[0].s.includes('0.8 °C'), true);
// one header sentence, learned or not
for (const conf of ['learned', 'default'])
  check('admin: header ' + conf, /Fixed rules; uses what this station learned where a part is in use\./.test(FCW.admin(Object.assign({}, base, { conf, in: Object.assign({}, inp, { learn: conf === 'learned' ? ['l5'] : undefined }) }), M)), true);
check('a temperature change has no offset', M.dt(-1.6), '1.6 °C');
// the ⓘ blocks -- every body template reached and filled, every block has a glyph and a title, warnings first
const MB = Object.assign({}, M, { g: v => Math.round(v) + ' km/h' }), seenB = new Set();
for (const c of cases) {
  const o = Object.assign({ in: inp, p: {} }, c), bl = FCW.blocks(o, MB);
  for (const b of bl) {
    check('block filled: ' + b.s, /\{\w+\}/.test(b.s) || !b.ti || !/<path/.test(FCO.G[b.g] || LUCIDE[b.g] || ''), false);
    for (const [k, t] of Object.entries(FCW.B)) if (new RegExp('^' + t.split(/\{\w+\}/).map(s => s.replace(/[.*+?^$()|[\]\\{}]/g, '\\$&')).join('.+') + '$').test(b.s)) seenB.add(k);
    if (b.ti === FCW.B.blind_t || b.ti === FCW.B.stale_t) seenB.add(b.ti === FCW.B.blind_t ? 'blind_t' : 'stale_t');
  }
  check('warning block first', bl.findIndex(b => b.w) <= 0, true);
  check('why renders, nothing unfilled', /\{\w+\}/.test(FCW.why(o, MB)), false);
}
for (const k of Object.keys(FCW.B).filter(k => k !== 'head')) check('block template used: ' + k, seenB.has(k), true);
const ex = FCW.blocks({ state: 'rain_light', next: 'p_paused', warn: 'gusts', p: { gust: 41.6, since: '2026-11-10T12:00:00Z' }, in: { rate: 1.3 } }, MB);
check('order: warning, state, pressure', ex.map(b => b.g), ['wind', 'cloud-drizzle', 'gauge']);
check('titles are the strip phrases', ex.map(b => b.ti), ['Gusts to 42 km/h', 'Light rain', 'Pressure outlook paused (calibration or clock)']);
check('rain number', ex[1].n, '1.3 mm/h');
check('blind block only below the limit', FCW.blocks({ state: null, in: { t: 5 }, p: {} }, MB).some(b => b.ti === FCW.B.blind_t), false);
check('blind block at the limit', FCW.blocks({ state: null, in: { t: 2 }, p: {} }, MB).some(b => b.ti === FCW.B.blind_t), true);
check('no neutral note under a rain glyph', FCW.blocks({ state: 'rain_heavy', in: {}, p: {} }, MB).some(b => b.note), false);
{ const sb = FCW.blocks({ state: 'sleet_snow', in: { t: 0.4, rate: 2.1 }, p: { since: '2026-11-10T12:00:00Z' } }, MB)[0];
  check('sleet block: the snowflake, the strip phrase, the temperature, the sentence', [sb.g, sb.ti, sb.n, sb.s, !!sb.note],
    ['snowflake', 'Sleet or snow', '0.4 °C', 'Wet since ' + MB.hm('2026-11-10T12:00:00Z') + '. At or below 1.0 °C the gauge’s precipitation may be sleet or snow.', false]);
  check('sleet block without since', FCW.blocks({ state: 'sleet_snow', in: { t: 1 }, p: {} }, MB)[0].s, 'At or below 1.0 °C the gauge’s precipitation may be sleet or snow.'); }
check('admin renders', /Thresholds/.test(FCW.admin(Object.assign({}, base, { in: inp }), M)), true);
// the admin as the Learning sheet -- one block per phrase, warnings first, readings and thresholds under Details
{ const h = FCW.admin(Object.assign({ in: inp, p: { gust: 41.6 } }, { state: 'rain_light', warn: 'gusts', next: 'p_paused' }), M);
  check('admin: blocks', (h.match(/<div class="lb/g) || []).length, 3);
  check('admin: the warning block first', /^[^]*?<div class="lb w">[^]*Gusts to 42 km\/h</.test(h) && h.indexOf('lb w') < h.indexOf('Light rain'), true);
  check('admin: readings folded', /<details class="adv"><summary>Readings and thresholds<\/summary>/.test(h), true);
  check('admin: no raw code', /\(p_|<code>|Gate/.test(h), false);
  check('admin: nothing unfilled', /\{\w+\}/.test(h), false);
  check('admin: the clock gate', /Waiting for a valid clock\./.test(FCW.admin(Object.assign({}, base, { gate: 'time', in: inp }), M)), true); }
// the night source in words, via own(); an unknown night has no "(unknown)" after it
const night = x => (FCW.admin(Object.assign({}, base, { in: Object.assign({}, inp, x) }), M).match(/<dt>Night<\/dt><dd>([^<]*)<\/dd>/) || [])[1];
check('night from the sun', night({ night: true, night_src: 'sun' }), 'yes (from the sun’s position)');
check('night source not an own key', night({ night: false, night_src: 'constructor' }), 'no (unknown)');
check('night unknown', night({ night: null, night_src: 'sun' }), 'unknown');

// the ⓘ names the source once per sourced slot, warning first; no neutral note under a T3 glyph
{ const nr = { icao: 'EGLL', name: 'Heathrow', km: 18, age_s: 1500, auto: false };
  const bl = FCW.blocks({ state: 'wx_drizzle_near', warn: 'fog_near', in: { near: nr }, p: {} }, MB);
  check('T3 blocks', bl.map(b => [b.g, b.ti, !!b.w]), [['cloud-fog', 'Fog reported nearby', true], ['cloud-drizzle', 'Drizzle reported nearby', false]]);
  check('T3 source sentence', bl[1].s, 'From the Heathrow (EGLL) report, 25 min old, 18 km away. A reading from this station comes first.');
  check('T3 no neutral note', bl.some(b => b.note), false);
  check('T3 age at the admission edge', FCW.blocks({ state: 'wx_rain_near', in: { near: Object.assign({}, nr, { age_s: 4500 }) }, p: {} }, MB)[0].s.includes(', 75 min old,'), true);
  check('T3 name escaped', FCW.blocks({ state: 'wx_rain_near', in: { near: Object.assign({}, nr, { name: '<b>' }) }, p: {} }, MB)[0].s.includes('&lt;b&gt;'), true);
  check('admin footer names the airport source', /come from the main airport/.test(FCW.admin({ state: 'wx_rain_near', in: Object.assign({}, inp, { near: nr }), p: {} }, M)), true);
  check('admin footer without a source', /uses only this station/.test(FCW.admin(Object.assign({}, base, { in: inp }), M)), true); }
// the pressure notices' short sheet and the Airport weather sheet's live comparison
{ const P = Object.assign({}, M, { hm: () => '08:40' });
  const st = (d, tg) => FCW.rel({ p_hpa: 1015.9, diff_hpa: d, target_hpa: tg, active: false }, P);
  check('rel_ok', st(0.4, 1013.2), 'Your station reads 1015.9 hPa, within 2.0 hPa of nearby airports.');
  check('rel_off', st(2.7, 1012.7), 'Your station reads 1015.9 hPa; nearby airports say 1013.2 hPa. To match, set REL in the console’s SET menu to about 1012.7 hPa.');
  check('rel_off_old', st(-2.7, null), 'Your station reads 1015.9 hPa, more than 2.0 hPa away from nearby airports. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.');
  check('rel: no station line', FCW.rel(null, P), '');
  // {ref} is the station line's own QNH (the near basis), not p - diff
  check('rel: quotes station.qnh_hpa', /nearby airports say 1013\.0 hPa/.test(FCW.rel({ p_hpa: 1015.9, qnh_hpa: 1013.0, diff_hpa: 2.7, target_hpa: 1013.0 }, P)), true);
  const now = new Date(), at = (dd, h, m) => new Date(now.getFullYear(), now.getMonth(), now.getDate() - dd, h, m).toISOString();
  const ref = { id: 'baro_ref', d_hpa: 2.7, qnh_hpa: 1013.2, target_hpa: 1013.2, since: at(0, 8, 40) };  // today: the time alone
  // {ref} is the notice's qnh_hpa even when the live reading has moved since
  check('note baro_ref: the airports say their QNH', /nearby airports say 1013\.2 hPa/.test(FCW.note(ref, 1016.6, null, P)), true);
  check('note baro_ref', FCW.note(ref, 1015.9, null, P), '<p class="cap">Your station reads 1015.9 hPa; nearby airports say 1013.2 hPa. To match, set REL in the console’s SET menu to about 1013.2 hPa.</p><p class="cap">The difference has held since 08:40.</p>');
  // not today: the day word (dayTime), so a three-day-old difference never reads as this morning's
  check('note baro_ref since yesterday', FCW.note(Object.assign({}, ref, { since: at(1, 8, 40) }), 1015.9, null, P).includes('<p class="cap">The difference has held since yesterday 08:40.</p>'), true);
  check('note baro_ref since 3 days', /held since \d{1,2} [A-Z][a-z]{2} 08:40\./.test(FCW.note(Object.assign({}, ref, { since: at(3, 8, 40) }), 1015.9, null, P)), true);
  check('note baro_ref, reference old: no number', /about \d/.test(FCW.note(Object.assign({}, ref, { target_hpa: null }), 1015.9, null, P)), false);
  const last = new Date(Date.now() - 40 * 60e3).toISOString();
  check('note uncal, last good', FCW.note({ id: 'baro_uncalibrated', last_hpa: 1015.9, target_hpa: 1015.4, last_t: last }, 1013.2, null, P),
    '<p class="cap">The console’s pressure is at its factory value (after a unit change or a power cut). Set REL in its SET menu to about 1015.4 hPa (the last good reading, 40 min ago).</p>');
  check('note uncal, none, airports now', FCW.note({ id: 'baro_uncalibrated' }, 1013.2, 1016.1, P),
    '<p class="cap">The console’s pressure is at its factory value (after a unit change or a power cut). Set REL in its SET menu to the current sea-level pressure of a nearby official station.</p><p class="cap">Nearby airports now: 1016.1 hPa.</p>');
  check('note baro_check: its own sentence, escaped', FCW.note({ id: 'baro_check', text: 'Usual <1>.' }, 1016, null, P), '<p class="cap">Usual &lt;1&gt;.</p>');
  check('REL templates: 7, whole sentences', Object.keys(FCW.R).length === 7 && Object.values(FCW.R).every(t => /\.$/.test(t)), true); }
console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
