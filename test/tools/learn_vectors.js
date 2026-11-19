// test/tools/learn_vectors.js -- renders firmware/fauxcloud/web/admin-learn.js (the admin "Learning" sheet) from fixtures in
// the /api/learn shape: every block per state (default, learned, in use,
// info), "Measured …" vs "Default … (not yet learned)", "Still learning: about {k} more …" from the firmware's `need`,
// the meter's tooltip from `bars`, L7's stages and flags, L3's console-step words, the sky map, the warnings, the restart
// dialog; no em dash, no unfilled placeholder, no internal terms. Usage: node test/tools/learn_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'admin-learn.js'), 'utf8');
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const toasts = [], posts = [];
const ARGS = ['esc', 'SHEETS', 'API', 'FC', 'icon', 'draw', 'SH', 'atob', 'toast', 'ago', 'api'];
const load = (sheets, api, fc, post) => new Function(...ARGS, src)(esc, sheets, api, require('./js_decl').fcStub(fc || {}), k => `<svg data-i="${k}"></svg>`, () => {}, null,
  s => Buffer.from(s, 'base64').toString('binary'), t => toasts.push(t), s => (s >= 3600 ? Math.round(s / 3600) + ' h' : s + ' s') + ' ago',
  post || ((m, p, b) => { posts.push([m, p]); return Promise.resolve({ ok: true }); }));
const SHEETS = {}, API = { stations: [] };
load(SHEETS, API, {});

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const clone = o => JSON.parse(JSON.stringify(o));
const TITLES = ['Clear-sky level', 'Shade around the sensor', 'Humidity at saturation', 'Smallest wind reported', 'Usual wind here',
  'Evening cooling', 'Night frost', 'Pressure check'];
function blockOf(html, title) {
  const i = html.indexOf(title + ' <span>') >= 0 ? html.indexOf(title + ' <span>') : html.indexOf(title + '</b>');
  if (i < 0) return '';
  const a = html.lastIndexOf('<div class="lb', i), next = html.indexOf('<div class="lb', i), h3 = html.indexOf('<h3>', i);
  return html.slice(a, Math.min(next < 0 ? Infinity : next, h3 < 0 ? Infinity : h3));
}
const text = h => h.replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ');
const meterOf = blk => (blk.match(/<button type="button" class="lv"[^>]*>/) || [''])[0];
const onBars = blk => (blk.match(/<i class="on">/g) || []).length;
const attr = (tag, a) => ((tag.match(new RegExp(a + '="([^"]*)"')) || [])[1] || '').replace(/&quot;/g, '"').replace(/&amp;/g, '&');

// the firmware's table (learn.h LEARN_BARS) and a fresh station: everything at its prior
const BARS = { l1: [6, 12, 27], l2_wet: [9, 19, 47], l2_night: [24, 63, 92], l3: [237, 591, 1019], l4: [265, 569, 1358], l5: [7, 18, 27], l6: [18, 42, 78], l7: [14, 51, 51] };
const cell = (i, o) => Object.assign({ el: [5, 10, 15, 20, 30, 45][i >> 1], pm: !!(i & 1), e: .55, n: 0, conf: 'default', last: 0, hit: false, need: 6, state: 'default', from: null }, o);
const empty = {
  v: 1, state: 'learning', why: null, epoch: 0, last_hour: 1794322800, last_day: 0, relearn: false, migrated_from: 0,
  backfill: { hours: 0, days: 0, done: true }, written: 0, writes: 0, nvs: { used: 308, free_entries: 322 },
  l1: { def: .55, lo: .35, hi: 1.3, cells: Array.from({ length: 12 }, (_, i) => cell(i)) },
  l1b: { sun: 0, shade: 0, unknown: 949, today: 0, clear_today: false, mask: Buffer.alloc(238).toString('base64') },
  l2: { wet: { e: 100, n: 0, conf: 'default', hit: false, need: 9, state: 'default' }, night: { e: 100, n: 0, conf: 'default', need: 24, state: 'default' },
    s: 100, src: 'prior', stuck: false, state: 'default' },
  l3: { e: 1.0, n: 0, conf: 'default', hit: false, need: 237, state: 'default' },
  l4: { p50: 3, p90: 12, n: 0, conf: 'default', hit: false, need: 265, state: 'default' },
  l7: { e: null, n: 0, days: 0, stage: 0, flag: 'none', uncal: false, off: null, norm: 1013.4, need: 14, state: 'default' },
  l5: { clear: { e: 1.5, n: 0, conf: 'default', need: 7, state: 'default' }, overcast: { e: .5, n: 0, conf: 'default', need: 7, state: 'default' },
    thr: 1, src: 'default', hit: false, state: 'default' },
  l6: { a: 1, b: .4, c: .2, n: 0, sd: null, mae: null, conf: 'default', learned: false, hit: false, need: 18, state: 'default' },
  relearn_hour: 0, bars: BARS,
  gate_state: { indoor: false, frozen_run: 0, geo_days: 0, dome: true, iced: true }, night: { open: null, last: null },
};

// ---- global rules on every render
function globals(h, name) {
  const vis = text(h.replace(/<svg[^]*?<\/svg>/g, '').replace(/ (title|aria-label|data-tip)="[^"]*"/g, ''));
  check(name + ': no unfilled placeholder', /\{\w+\}|undefined|NaN|Infinity/.test(h), false);
  check(name + ': no (default), derived, D-numbers, L-names, n_eff, gate names, em dash in visible text',
    /\(default\)|derived|\bD\d\b|\bL[1-7]b?\b|n_eff|\bgates?\b|—/.test(vis), false);
  const meters = TITLES.map(t => meterOf(blockOf(h, t)));
  check(name + ': every meter a button whose title equals data-tip', meters.every(m => m && attr(m, 'title') && attr(m, 'title') === attr(m, 'data-tip')), true);
  check(name + ': aria-label = bars + tooltip', meters.every(m => /^(No bars|One bar of three|Two bars of three|Three bars of three)\. /.test(attr(m, 'aria-label')) &&
    attr(m, 'aria-label').endsWith(attr(m, 'title'))), true);
}

// ---- default: "Default … (not yet learned)" and "Still learning: about {k} more …" from `need`
let html = SHEETS.learn.body(empty, 'Garden'), B = t => blockOf(html, t);
globals(html, 'default');
check('header line', html.includes('Each part below is used by the outlook once it is learned; until then the standard rules apply. Not saved yet: the first save comes within 6 hours.'), true);
check('eight learner blocks in order', TITLES.map(t => html.indexOf(t)).every((x, i, a) => x > 0 && (!i || x > a[i - 1])) && (html.match(/<div class="lb">/g) || []).length, 8);
check('L1 default', [text(B('Clear-sky level')).includes('Clear-sky level · Default 55 % (not yet learned)'), B('Clear-sky level').includes('Still learning: about 6 more clear-sky hours.'), B('Clear-sky level').includes('bright haze does not count.')], [true, true, true]);
check('L1b default', [B('Shade around the sensor').includes('No sky cells seen yet: the map below fills on clear days.'), B('Shade around the sensor').includes('Fills on clear days.')], [true, true]);
check('L2 default', [text(B('Humidity at saturation')).includes('· Default 100 % (not yet learned)'), B('Humidity at saturation').includes('Still learning: about 24 more clear nights.')], [true, true]);
check('L3 default', [text(B('Smallest wind reported')).includes('· Default 1.0 km/h (not yet learned)'), B('Smallest wind reported').includes('Still learning: about 237 more readings of light wind.'),
  B('Smallest wind reported').includes('cups')], [true, true, false]);
check('L4 default', [text(B('Usual wind here')).includes('· Default 12.0 km/h (not yet learned)'), B('Usual wind here').includes('Still learning: about 265 more hours of wind.')], [true, true]);
check('L5 default (and the seasonal wait)', [text(B('Evening cooling')).includes('· Standard 1.00 °C an hour (not yet learned)'),
  B('Evening cooling').includes('It waits for the clear-sky level at evening sun heights, usually from spring.'),
  B('Evening cooling').includes('Still learning: about 7 more clear and 7 more cloudy evenings.')], [true, true, true]);
check('L6 default', [text(B('Night frost')).includes('Night frost · Not learned yet'), B('Night frost').includes('Still learning: about 18 more clear calm nights.')], [true, true]);
check('L7 learning', [B('Pressure check').includes('Pressure check</b>'), B('Pressure check').includes('Checking: about 14 more days before a first check.')], [true, true]);
check('sky empty state', html.includes('No sky cells seen yet: the map fills on clear days.') && !html.includes('<svg viewBox'), true);
check('every meter: no bars at the prior', TITLES.map(t => onBars(B(t))), [0, 0, 0, 0, 0, 0, 0, 0]);
check('no "Measured" anywhere at the prior', /Measured/.test(html), false);
const tips = Object.fromEntries(TITLES.map(t => [t, attr(meterOf(blockOf(html, t)), 'title')]));
check('tooltips from the bars table', [tips['Clear-sky level'], tips['Humidity at saturation'], tips['Smallest wind reported'], tips['Usual wind here'],
  tips['Evening cooling'], tips['Night frost'], tips['Pressure check']],
  ['One bar at 6 clear-sky hours at one sun height, two at 12, three at 27.', 'One bar at 24 clear nights, two at 63, three at 92.',
   'One bar at 237 readings of light wind, two at 591, three at 1019.', 'One bar at 265 hours of wind, two at 569, three at 1358.',
   'One bar at 7 clear and 7 cloudy evenings, two at 18 of each, three at 27 of each.',
   'One bar at 18 clear calm nights, two at 42, three at 78; in use from three bars with guesses within 2.0 °C.',
   'A first check after 14 days, a finer one after 51 days.']);
check('restart button', [html.includes('<h3>Restart learning</h3>'), html.includes('Moved or replaced the sensor? Learning restarts from what it knows; the outlook uses the standard rules until each part is learned again.'),
  html.includes('data-rl="ask"')], [true, true, true]);
// L7 without a usual pressure (no table, or no place)
check('L7 without a normal', blockOf(SHEETS.learn.body(Object.assign(clone(empty), { l7: Object.assign(clone(empty.l7), { norm: null }) }), 'G'), 'Pressure check')
  .includes('No usual pressure is known for this place, so there is no check.'), true);

// ---- learned and in use
const mask = Buffer.alloc(238);
mask[0] = 0b0110;  // cell 0 sun, cell 1 shade
const fx = clone(empty);
fx.written = 1794300000;
fx.l1.cells[2] = cell(2, { e: .58, n: 0, from: 15, need: 6 });                                         // seeded from the 15° band
fx.l1.cells[4] = cell(4, { e: .62, n: 13, conf: 'medium', last: 20772, need: 14, state: 'in_use' });  // 15-20 deg AM
fx.l1.cells[6] = cell(6, { e: .66, n: 7, conf: 'low', last: 20772, need: 5, state: 'learned' });       // 20-30 deg AM
fx.l1b = { sun: 1, shade: 1, unknown: 947, today: 0, clear_today: false, mask: mask.toString('base64') };
fx.l2 = { wet: { e: 100, n: 0, conf: 'default', hit: false, need: 9, state: 'default' }, night: { e: 93.4, n: 26, conf: 'low', need: 21, state: 'in_use' },
  s: 93.4, src: 'night', stuck: false, state: 'in_use' };
fx.l3 = { e: 0.5, n: 700, conf: 'high', hit: false, need: 0, state: 'info' };
fx.l4 = { p50: 3.2, p90: 9.8, n: 300, conf: 'low', hit: false, need: 210, state: 'info' };
fx.l5 = { clear: { e: 1.62, n: 19, conf: 'medium', need: 5, state: 'in_use' }, overcast: { e: .21, n: 18.5, conf: 'medium', need: 6, state: 'in_use' }, thr: .92,
  src: 'learned', hit: false, state: 'in_use' };
fx.l6 = { a: 2, b: .5, c: .3, n: 80, sd: 1.4, mae: 1.1, conf: 'high', learned: true, hit: false, need: 0, state: 'in_use' };
fx.l7 = { e: 1009.2, n: 33, days: 52, stage: 2, flag: 'none', uncal: false, off: -4.2, norm: 1013.4, need: 0, state: 'info' };
fx.gate_state = { indoor: true, frozen_run: 0, geo_days: 0, dome: false, iced: false };
html = SHEETS.learn.body(fx, 'Garden');
globals(html, 'in use');
check('saved ago', / Saved \d+ h ago\.<\/p>/.test(html), true);
check('L1 measured, in use', [text(B('Clear-sky level')).includes('· Measured 66 %'),
  B('Clear-sky level').includes('Measured at 2 of 12 sun heights; the others fill in as the sun climbs through the year.'),
  B('Clear-sky level').includes('Sun heights next to a learned one start from its value (15° band).'),
  B('Clear-sky level').includes('In use. “Sunny.”, “Sunny spells.” and “No sun.” come from it at the learned sun heights.'),
  B('Clear-sky level').includes('Still learning'), onBars(B('Clear-sky level'))], [true, true, true, true, false, 2]);
check('L1b with shade', [B('Shade around the sensor').includes('Trees or buildings hide the sun from the sensor in 1 of the 2 sky cells seen so far.'),
  B('Shade around the sensor').includes('2 of 949 sky cells seen.'), onBars(B('Shade around the sensor'))], [true, true, 1]);
check('L2 in use', [text(B('Humidity at saturation')).includes('· Measured 93.4 %'),
  B('Humidity at saturation').includes('This sensor shows about 93.4 % in saturated air. In use. “Mist or fog possible.” takes 93.4 % as saturated air on calm nights.'),
  B('Humidity at saturation').includes('Still learning')], [true, true, false]);
check('L3 information (the console step)', [text(B('Smallest wind reported')).includes('· Measured 0.5 km/h'),
  B('Smallest wind reported').includes('The console reports wind in steps; its smallest reading above 0 is about 0.5 km/h. For information; the outlook does not use it.'),
  /In use/.test(B('Smallest wind reported'))], [true, true, false]);
check('L4 information', [text(B('Usual wind here')).includes('· Measured 9.8 km/h'),
  B('Usual wind here').includes('Half of the hours here have a mean wind under 3.2 km/h; one hour in ten is above 9.8 km/h. For information; the outlook does not use it.'),
  /Windier than usual/.test(B('Usual wind here'))], [true, true, false]);
check('L5 in use', [text(B('Evening cooling')).includes('· Measured 0.92 °C an hour'),
  B('Evening cooling').includes('After sunset, clear evenings here cool 1.62 °C an hour and cloudy ones 0.21 °C an hour. In use. “Clear night.” needs a cooling of 0.92 °C an hour.'),
  onBars(B('Evening cooling'))], [true, true, 2]);
check('L6 in use', [text(B('Night frost')).includes('· Measured 8.1 °C ± 1.4 °C'), B('Night frost').includes('In use. “Minimum around … tonight.” comes from it on clear nights.'),
  onBars(B('Night frost'))], [true, true, 3]);
check('L7 stage 2, no error (never "looks right"; admitted days)', [B('Pressure check').includes('No large error found over 52 days.'), /looks right/.test(html), onBars(B('Pressure check'))], [true, false, 3]);
check('covered-sensor warning first, no meter', html.indexOf('class="lb w"') > 0 && html.indexOf('class="lb w"') < html.indexOf('Clear-sky level') &&
  html.includes('Light sensor looks covered'), true);
check('sky: intro, one sun, one shade, the rest outlined', [html.includes('Each cell is a patch of sky the sun crosses, by direction and height.'),
  (html.match(/fill="var\(--sunc\)"/g) || []).length, (html.match(/fill="#9fb0d8"/g) || []).length, (html.match(/fill="none"/g) || []).length], [true, 1, 1, 947]);
check('sky aria-label and legend', html.includes('aria-label="Sky map: 1 cells with sun, 1 in shade, 947 not seen yet."') &&
  html.includes('Gold: the sun reached the sensor there. Grey: something blocked it. Outline: not seen yet; the sun’s path changes with the season.'), true);

// ---- learned, not yet in use (below the use level): the value is measured, the line says how far
const lr = clone(fx);
lr.l1.cells[4] = cell(4, { e: .62, n: 8, conf: 'low', last: 20772, need: 4, state: 'learned' });
lr.l5 = { clear: { e: 1.62, n: 8, conf: 'low', need: 10, state: 'learned' }, overcast: { e: .21, n: 9, conf: 'low', need: 9, state: 'learned' }, thr: .92,
  src: 'learned', hit: false, state: 'learned' };
lr.l6 = { a: 2, b: .5, c: .3, n: 30, sd: 2.2, mae: 1.6, conf: 'low', learned: true, hit: false, need: 12, state: 'learned' };
html = SHEETS.learn.body(lr, 'Garden');
globals(html, 'learned');
check('L1 learned: the nearest cell to its next bar', [B('Clear-sky level').includes('Still learning: about 4 more clear-sky hours.'), /In use/.test(B('Clear-sky level'))], [true, false]);
check('L5 learned', [B('Evening cooling').includes('After sunset, clear evenings here cool 1.62'), B('Evening cooling').includes('Still learning: about 10 more clear and 9 more cloudy evenings.'),
  /In use/.test(B('Evening cooling'))], [true, true, false]);
check('L6 learned, its guesses too wide for use', [B('Night frost').includes('Its guesses still miss by about 2.2 °C; 2.0 °C is allowed.'), B('Night frost').includes('Still learning: about 12 more clear calm nights.')], [true, true]);

// ---- L7 flags (rounded to 5 hPa), L5 inseparable, L6 not learned with a miss, paused, warnings
const off = clone(fx);
off.l7 = Object.assign(off.l7, { flag: 'high', off: 23.6 });
check('L7 with airport weather on: no second pressure sentence', [blockOf(SHEETS.learn.body(Object.assign(clone(off), { use: { metar: true } }), 'G'), 'Pressure check')
  .includes('Airport weather is on: its pressure check is used instead.'), /Set REL/.test(blockOf(SHEETS.learn.body(Object.assign(clone(off), { use: { metar: true }, l7: Object.assign(clone(off.l7), { flag: 'high', off: 23.6 }) }), 'G'), 'Pressure check'))], [true, false]);
// a night value under 92 % is not "measured" saturation; days past 60; REL at its factory value
{ const lo = clone(empty);
  lo.l2.night = { e: 88.4, n: 26, conf: 'low', need: 21, state: 'learned' };
  const h2 = blockOf(SHEETS.learn.body(lo, 'G'), 'Humidity at saturation');
  const ls = clone(lo);
  ls.l2.stuck = true;  // the stuck warning is the reason, not "too low"
  check('L2 night under 92 % while stuck: no "too low" sentence', /too low to take/.test(blockOf(SHEETS.learn.body(ls, 'G'), 'Humidity at saturation')), false);
  check('L2 night under 92 %: default S, the night value said', [text(h2).includes('· Default 100 % (not yet learned)'),
    h2.includes('Clear nights here top out at about 88.4 %, too low to take as saturated air.'), /Measured/.test(h2)], [true, true, false]);
  const l60 = clone(fx);
  l60.l7.days = 61;
  check('L7 over 60 days', blockOf(SHEETS.learn.body(l60, 'G'), 'Pressure check').includes('No large error found over 60 days.'), true);
  const un = clone(fx);
  un.l7 = Object.assign(un.l7, { uncal: true, flag: 'none' });
  check('L7 under baro_uncalibrated', [blockOf(SHEETS.learn.body(un, 'G'), 'Pressure check').includes('The console’s pressure is at its factory value; the check waits until REL is set.'),
    /No large error/.test(blockOf(SHEETS.learn.body(un, 'G'), 'Pressure check'))], [true, false]); }
check('L7 high', blockOf(SHEETS.learn.body(off, 'G'), 'Pressure check').includes('The pressure here averaged about 25 hPa above what is usual here. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.'), true);
off.l7 = Object.assign(off.l7, { flag: 'low', off: -13.0, stage: 1 });
check('L7 low', [blockOf(SHEETS.learn.body(off, 'G'), 'Pressure check').includes('The pressure here averaged about 15 hPa below what is usual here'),
  onBars(blockOf(SHEETS.learn.body(off, 'G'), 'Pressure check'))], [true, 1]);
const ins = clone(empty);
ins.l5 = { clear: { e: .4, n: 19, conf: 'medium', need: 3, state: 'learned' }, overcast: { e: .5, n: 18, conf: 'medium', need: 4, state: 'learned' }, thr: 1, src: 'inseparable', hit: false, state: 'learned' };
ins.l6 = Object.assign(ins.l6, { n: 19.2, sd: 2.8, mae: 2.1, need: 18 });
html = SHEETS.learn.body(ins, 'G');
globals(html, 'specials');
check('L5 inseparable', [B('Evening cooling').includes('Not learned: clear and cloudy evenings here are too alike to tell apart.'), B('Evening cooling').includes('Still learning'),
  text(B('Evening cooling')).includes('· Standard 1.00 °C an hour (not yet learned)'), /Measured/.test(B('Evening cooling'))], [true, false, true, false]);
check('L6 counted, not learned: the miss', [text(B('Night frost')).includes('Night frost · Not learned yet'), B('Night frost').includes('Its guesses still miss by about 2.8 °C; 2.5 °C is allowed.')], [true, true]);
const pz = Object.assign(clone(fx), { why: 'no_location', state: 'paused' });
pz.l1.cells[4].state = 'learned';
html = SHEETS.learn.body(pz, 'G');
const P = 'Paused until the bridge has a location.';
check('paused header', html.includes('Paused: set the bridge location (Location and time).'), true);
check('paused per block (the sun learners: L1, L1b, L5, L6)', TITLES.map(t => B(t).includes(P)), [true, true, false, false, false, true, true, false]);
const pze = Object.assign(clone(empty), { why: 'no_location', state: 'paused' });
check('paused (default)', TITLES.map(t => blockOf(SHEETS.learn.body(pze, 'G'), t).includes(P)), [true, true, false, false, false, true, true, false]);
const warns = Object.assign(clone(empty), { why: 'no_location', state: 'paused', gate_state: { indoor: false, frozen_run: 7, geo_days: 3, dome: false, iced: false } });
warns.l2.stuck = true;
html = SHEETS.learn.body(warns, 'G');
check('warnings: location, geo, frozen, stuck', ['No location set', 'Sunlight reported at night', 'Temperature not changing', 'Humidity stuck at its top'].map(t => html.includes(t)), [true, true, true, true]);
check('backfill state', SHEETS.learn.body(Object.assign(clone(empty), { state: 'backfill', backfill: { hours: 812, done: false } }), 'G').includes('Reading the stored history: 812 hours so far.'), true);
check('read-only: no restart', /Restart learning/.test(SHEETS.learn.body(Object.assign(clone(empty), { why: 'blob_newer', state: 'read_only' }), 'G')), false);
check('error', SHEETS.learn.body({ err: 1 }), '<p class="hint">No learning state for this station.</p>');

// ---- the sheet: title with the station's name, the meter tap, the restart dialog
// the Learning tile on the admin home, a status like every other tile (learnTile from adminface.js)
{
  const face = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'adminface.js'), 'utf8');
  const m = face.match(/function learnTile\(T, b\) \{[\s\S]*?\n\}/);
  check('learnTile found', !!m, true);
  // Learning and the Weather description in ONE row with the station tiles (the Stations grid), not in
  // This bridge; Home Assistant and online services before Connections
  // the section heads and the tile's words are adminface.js's table (WF) since the i18n extraction
  const at = s => face.indexOf(s), wt = "T('weather', icon('sun'), WF.weather", sg = face.slice(at('<h2 class="sect">${WF.sect_st}</h2>'), at('${API.noSlot'));
  check('the two tiles inside the Stations grid (one tiles div), not in This bridge',
    [sg.includes('learnTile(T, b)') && sg.includes(wt), (sg.match(/<div class="tiles">/g) || []).length,
      (b => b.includes('learnTile') || b.includes(wt))(face.slice(at('<h2 class="sect">${WF.sect_bridge}</h2>'), at('${WF.health}</h2>')))], [true, 1, false]);
  check('the two tiles only once a first station exists', sg.includes('${API.stations.length ? learnTile(T, b) + T(\'weather\''), true);
  check('the section order', ['${WF.sect_st}</h2>', '${SECT_OUT()}</h2>', '${WF.sect_conn}</h2>', '${WF.sect_bridge}</h2>'].map(at).every((x, i, a) => x > 0 && (!i || x > a[i - 1])), true);
  if (m) {
    const T = (key, ic, label, val, sub, warn, idle) => ({ key, label, val, sub, warn: !!warn, idle: !!idle });
    const { decl, fcStub } = require('./js_decl');
    const tile = (stations, b) => new Function('API', 'icon', 'FC', decl(face, 'WF') + '\n' + m[0] + '\nreturn learnTile;')({ stations }, () => '', fcStub())(T, b);
    const loc = { lat: 52, lon: 20 };
    check('no location -> paused, grey', [tile([{ lu: 2 }], { lat: null, lon: null }).val, tile([], { lat: 52, lon: null }).idle], ['Paused: set the location', true]);
    check('none in use -> Learning', [tile([{ lu: 0 }], loc).val, tile([], loc).val, tile([{ lu: 0 }], loc).idle], ['Learning', 'Learning', false]);
    check('n of 4 (the station with the most)', [tile([{ lu: 1 }, { lu: 3 }], loc).val, tile([{ lu: 4 }], loc).val], ['3 of 4 in use', '4 of 4 in use']);
    check('the sub stays', tile([{ lu: 1 }], loc).sub, 'Each part feeds the outlook once learned');
  }
}
(async () => {
  const S3 = {}, A3 = { stations: [{ id: 'ST1', cfg: { name: 'Garden' } }] };
  const fcs = { get: () => Promise.resolve({ status: 200, data: clone(empty) }) };
  load(S3, A3, fcs);
  let sheet = S3.learn();
  check('title', sheet.title, 'Learning · Garden');
  toasts.length = 0;
  sheet.click({ dataset: { tip: 'One bar at 6 clear-sky hours.' } });
  check('tap on the meter -> toast', toasts, ['One bar at 6 clear-sky hours.']);
  await new Promise(r => setTimeout(r, 0));
  sheet.click({ dataset: { rl: 'ask' } });
  let h = sheet.body();
  check('restart: the confirm step', [h.includes('Restart learning for Garden? Every part goes back to the standard rules until it is learned again.'),
    h.includes('data-rl="go"'), h.includes('data-rl="no"')], [true, true, true]);
  sheet.click({ dataset: { rl: 'no' } });
  check('restart: cancel', sheet.body().includes('data-rl="go"'), false);
  sheet.click({ dataset: { rl: 'ask' } });
  posts.length = 0, toasts.length = 0;
  sheet.click({ dataset: { rl: 'go' } });
  await new Promise(r => setTimeout(r, 0));
  check('restart: POST', posts, [['POST', '/api/learn/relearn?station=ST1']]);
  check('restart: toast, the dialog closed', [toasts, sheet.body().includes('data-rl="go"')], [['Learning restarts.'], false]);
  // a refused restart ({"ok":false}) is not "Learning restarts."
  const S4 = {};
  load(S4, A3, fcs, () => Promise.resolve({ ok: false, errors: ['station: no learning state'] }));
  const s4 = S4.learn();
  await new Promise(r => setTimeout(r, 0));
  s4.click({ dataset: { rl: 'ask' } });
  toasts.length = 0;
  const btn = { dataset: { rl: 'go' }, disabled: false };
  s4.click(btn);
  await new Promise(r => setTimeout(r, 0));
  check('restart refused: no success toast, the button usable', [toasts, btn.disabled, s4.body().includes('data-rl="go"')], [['No answer from fauxcloud.'], false, true]);
  // /api/learn answering 404 shows the error state, not a TypeError inside draw()
  const S2 = {}, A2 = { stations: [{ id: 'ST1', cfg: {} }] };
  load(S2, A2, { get: () => Promise.resolve({ status: 404, data: { error: 'no such station' } }) });
  sheet = S2.learn();
  await new Promise(r => setTimeout(r, 0));
  let out;
  try { out = sheet.body(); } catch (e) { out = 'threw ' + e.message; }
  check('404 from /api/learn', out, '<p class="hint">No learning state for this station.</p>');
  console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
