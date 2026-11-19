// test/tools/metar_vectors.js -- renders firmware/fauxcloud/web/admin-metar.js (the admin "Airport weather"
// sheet) from fixtures in the /api/admin/metar + /api/metar shapes, and its formatters in both unit systems:
// off; on without a location ("set the location first", a typed code still offered); the lookup states; three
// airports (no "add"); a stale report; the reference with its sources and the inconsistent note; "Fetch now" and
// the last fetch in words. Each check runs on the source and on the terser-mangled bundle. Usage: node test/tools/metar_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const root = path.join(__dirname, '..', '..');
// the page's one clock-time helper (health.js), ahead of the source under test
const HM = (h => { const { decl } = require('./js_decl'); return decl(h, 'p2') + '\n' + decl(h, 'hm') + '\n'; })(fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'health.js'), 'utf8'));
const plain = HM + fs.readFileSync(path.join(root, 'firmware', 'fauxcloud', 'web', 'admin-metar.js'), 'utf8');
let mangled = null;
try {
  mangled = execFileSync(process.execPath, [path.join(root, 'tools', 'terser_run.js')], { input: plain }).toString();
} catch (e) {
  if (process.env.CI) throw e;
  console.log('note: terser not available (npm ci --ignore-scripts in tools/), the mangled pass is skipped');
}

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;
const ago = s => s < 90 ? s + ' s ago' : s < 5400 ? Math.round(s / 60) + ' min ago' : s < 172800 ? Math.round(s / 3600) + ' h ago' : Math.round(s / 86400) + ' days ago';
let units = 'metric';
const FC = require('./js_decl').fcStub({
  imperial: () => units === 'imperial', get: () => new Promise(() => {}),
  UNITS: { t: { m: '°C', i: '°F', f: v => v * 1.8 + 32 }, w: { m: 'km/h', i: 'mph', f: v => v / 1.609344 }, r: { m: 'mm', i: 'in', f: v => v / 25.4 }, p: { m: 'hPa', i: 'inHg', f: v => v * 0.02953 } },
});
function load(src, api = () => new Promise(() => {}), fc = FC) {
  const SHEETS = {}, API = {};
  // tog as adminface.js has it (the sheet uses the shared switch); flag / TZ as picker.js gives them once the zone list is in
  const tog = (id, on, l, s) => `<label class="toggle" for="${id}"><input type="checkbox" role="switch" id="${id}"${on ? ' checked' : ''}><span>${l}${s ? `<small>${s}</small>` : ''}</span></label>`;
  new Function('esc', 'own', 'ago', 'FC', 'SHEETS', 'API', 'icon', 'api', 'draw', 'SH', 'toast', 'noAnswer', 'refresh', '$', 'tog', 'flag', 'TZ', src)(
    esc, own, ago, fc, SHEETS, API, k => `<svg data-i="${k}"></svg>`, api, () => {}, null, () => {}, () => () => {}, () => {}, () => null,
    tog, cc => `<img class="flag" data-cc="${cc}">`, {});
  return SHEETS.metar;
}

const M = o => Object.assign({ available: true, on: true, location: true, time: true, airports: [], state: 'idle', next_s: 600, n: {},
  run: { state: 'idle', result: null }, lookup: { state: 'idle', near: false, result: null, list: [] }, last: null }, o);
// the firmware adds `cc` (airport.cpp's ICAO-prefix table); the vectors give it for the two real prefixes
const apt = (icao, km, name) => Object.assign({ icao, name: name || icao + ' field', km }, { EG: { cc: 'GB' }, KJ: { cc: 'US' } }[icao.slice(0, 2)]);
const row = (icao, o) => Object.assign({ icao, name: icao + ' field', km: 12, obs: '2026-11-13T10:50:00Z', age_s: 1380, stale: false, cur: true, qnh_hpa: 1018.2,
  wind_dir: 320, wind_kmh: 9.3, gust_kmh: null }, o);

function run(label, src) {
  const S = load(src), f = S.fmt, W = S.W, R = (m, p, v) => S.render(m, p, v || { asked: true });  // asked: a lookup made in this visit
  const has = (h, s) => h.indexOf(s) >= 0;
  // formatters, metric then imperial
  units = 'metric';
  check(label + 'km', f.dist(12), '12 km');
  check(label + 'hPa', f.press(1018.24), '1018.2 hPa');
  check(label + 'near', f.away(12), 'about 12 km away');
  check(label + 'far', f.away(120), '120 km away — too far to compare pressure');
  check(label + 'no km', f.away(null), '');
  check(label + 'wind', f.wind({ wind_dir: 320, wind_kmh: 9.3, gust_kmh: 27.8 }), '320° 9 km/h, gusts 28');
  check(label + 'vrb', f.wind({ wind_dir: 'vrb', wind_kmh: 3.7, gust_kmh: null }), 'variable 4 km/h');
  check(label + 'calm', f.wind({ wind_dir: null, wind_kmh: 0, gust_kmh: null }), '0 km/h');
  check(label + 'values', f.values(row('EGLL')), '1018.2 hPa · wind 320° 9 km/h');
  check(label + 'none', f.values(row('EGLL', { obs: null })), 'No report yet');
  check(label + 'result', [f.result('ok'), f.result('fail'), f.result('weird')], [W.r_ok, W.r_fail, W.r_other]);
  units = 'imperial';
  check(label + 'mi', f.dist(16), '10 mi');
  check(label + 'inHg', f.press(1013.2), '29.92 inHg');
  check(label + 'mph', f.wind({ wind_dir: 180, wind_kmh: 16.1, gust_kmh: null }), '180° 10 mph');
  units = 'metric';
  // the sheet
  check(label + 'loading', has(R(null, null), 'Loading'), true);
  // Rename per row; the field with the name, its code as placeholder, Save / Cancel and the hint
  {
    const m2 = M({ airports: [apt('EGLL', 23, 'Heathrow'), apt('EGKK', 41)] });
    const h0 = R(m2, null);
    check(label + 'rename: the name is the control, labelled', [(h0.match(/class="nmb" data-a="ren"/g) || []).length, has(h0, 'aria-label="Rename EGLL"'), has(h0, '>Rename<')], [2, true, false]);
    const h1 = R(m2, null, { asked: true, ren: 'EGLL' });
    check(label + 'rename: the field in its row', [has(h1, 'id="mName"'), has(h1, 'value="Heathrow"'), has(h1, 'placeholder="Heathrow"'),
      has(h1, 'data-a="nsave" data-id="EGLL"'), has(h1, W.name_h), has(h1, 'data-a="ren"')], [true, true, true, true, true, false]);
    const h2 = R(M({ airports: [apt('EGLL', 23, '<b>&"x')] }), null, { asked: true, ren: 'EGLL' });
    check(label + 'rename: the value escaped', has(h2, 'value="&lt;b&gt;&amp;&quot;x"'), true);
    // the placeholder is the name that comes back: the user's own name -> the last lookup's, else none
    const own = Object.assign(apt('EGLL', 23, 'My field'), { own: true });
    const h3 = R(M({ airports: [own], lookup: { state: 'done', near: true, result: 'ok', list: [apt('EGLL', 23, 'Heathrow')] } }), null, { asked: true, ren: 'EGLL' });
    const h4 = R(M({ airports: [own] }), null, { asked: true, ren: 'EGLL' });
    check(label + 'rename: placeholder for an own name', [has(h3, 'placeholder="Heathrow"'), has(h4, 'placeholder=""')], [true, true]);
    // adding is folded behind one button while airports exist (open with none, or after a lookup in this visit)
    const lastM = M({ airports: [apt('EGLL', 23, 'Heathrow')], last: { t: '2026-11-14T20:30:00Z', kind: 'fetch', result: 'ok' } });
    const h5 = R(lastM, null, {});
    check(label + 'add: folded', [has(h5, 'data-a="addo"'), has(h5, 'data-a="near"'), has(h5, 'id="mCode"')], [true, false, false]);
    check(label + 'add: open on the tap, and with no airport', [has(R(lastM, null, { add: true }), 'data-a="near"'), has(R(M({}), null, {}), 'data-a="near"')], [true, true]);
    check(label + 'the last fetch in the tooltip, not a line', [/data-a="run" title="Fetched \d\d:\d\d · OK"/.test(h5), has(h5, '<p class="hint">Fetched')], [true, false]);
    // the Online services shape; a failed fetch says "Tried", never "Fetched"
    const tried = R(M({ airports: [apt('EGLL', 23, 'Heathrow')], last: { t: '2026-11-14T20:30:00Z', kind: 'fetch', result: 'fail' } }), null, {});
    check(label + 'a failed fetch is "Tried {t} · {r}"', [/data-a="run" title="Tried \d\d:\d\d · could not reach the service; the last reports are kept"/.test(tried), has(tried, 'title="Fetched')], [true, false]);
    check(label + 'no new report is still "Fetched"', /title="Fetched \d\d:\d\d · no new report"/.test(R(M({ airports: [apt('EGLL', 23, 'Heathrow')], last: { t: '2026-11-14T20:30:00Z', kind: 'fetch', result: 'nodata' } }), null, {})), true);
  }
  let h = R(M({ on: false }), null);
  check(label + 'off: switch off, explained, no lookup', [has(h, 'id="mOn"'), has(h, 'checked'), has(h, 'aviationweather.gov'), has(h, W.off_first), has(h, 'data-a="near"')], [true, false, true, true, false]);
  h = R(M({ location: false }), null);
  check(label + 'no location', [has(h, W.loc), has(h, 'data-a="near"'), has(h, 'data-a="code"')], [true, false, true]);
  h = R(M({ time: false }), null);
  check(label + 'no clock', has(h, W.clock), true);
  h = R(M({ lookup: { state: 'running', near: true, result: null, list: [] } }), null);
  check(label + 'looking', has(h, W.looking), true);
  h = R(M({ lookup: { state: 'done', near: true, result: 'ok', list: [apt('EGLL', 23, 'London/Heathrow'), apt('EGKK', 41)] } }), null);
  check(label + 'found: add buttons with km', [has(h, 'data-a="add" data-id="EGLL"'), has(h, 'London/Heathrow'), has(h, 'about 23 km away')], [true, true, true]);
  h = R(M({ lookup: { state: 'done', near: true, result: 'none', list: [] } }), null);
  check(label + 'none near', has(h, W.none), true);
  h = R(M({ lookup: { state: 'done', near: false, result: 'none', list: [] } }), null, { code: 'ZZZZ', asked: true });
  check(label + 'unknown code', has(h, 'No airport ZZZZ in the aviation weather list.'), true);
  h = R(M({ lookup: { state: 'done', near: true, result: 'busy', list: [] } }), null);
  check(label + 'lookup busy', has(h, W.r_busy), true);
  const three = M({ airports: [apt('EGLL', 23), apt('EGKK', 41), apt('EGLC', 90)], last: { t: '2026-11-13T11:05:00Z', kind: 'fetch', result: 'ok' } });
  const live = { airports: [row('EGLL', { km: 23 }), row('EGKK', { km: 41, stale: true, age_s: 6000 }), row('EGLC', { km: 90, obs: null })],
    reference: { qnh_hpa: 1018.1, qnh_from: [{ icao: 'EGLL', km: 23, age_s: 1380 }], qnh_inconsistent: true } };
  h = R(three, live);
  check(label + 'three: no add section', [has(h, W.found), has(h, 'data-a="rm" data-id="EGLC"')], [false, true]);
  // the age beside the status dot, the values on their own line; a stale row greys, its dot grey
  check(label + 'rows', [has(h, '<span class="dot" title="Last report 23 min ago"></span><span class="hint">23 min ago</span>'), has(h, '1018.2 hPa · wind'),
    has(h, '<span class="dot off" title="Last report 2 h ago">'), has(h, W.no_rep), has(h, '90 km away — too far')], [true, true, true, true, true]);
  // amber once the firmware's `cur` is false (75-90 min); a 70-min report of an hourly airport stays green
  check(label + 'amber dot for a 75-90-min report', has(R(M({ airports: [apt('EGLL', 23)] }), { airports: [row('EGLL', { age_s: 4800, cur: false })], reference: null }), 'class="dot late"'), true);
  check(label + 'a 70-min current report is green', has(R(M({ airports: [apt('EGLL', 23)] }), { airports: [row('EGLL', { age_s: 4200, cur: true })], reference: null }), 'class="dot late"'), false);
  check(label + 'the add section is "Add an airport"', W.found, 'Add an airport');
  check(label + 'reference', [has(h, 'Pressure reference: 1018.1 hPa (from EGLL)'), has(h, W.mixed)], [true, true]);
  check(label + 'fetch now + last', [has(h, 'data-a="run"'), /Fetched \d\d:\d\d · OK/.test(h)], [true, true]);
  h = R(Object.assign({}, three, { run: { state: 'running', result: null } }), live);
  check(label + 'fetching', [has(h, W.fetching), has(h, 'disabled')], [true, true]);
  check(label + 'no wind in the reference line', /Pressure reference:[^<]*wind/.test(h), false);
  check(label + 'empty', has(R(M({}), null), W.no_air), true);
  // privacy wording, wind templates and the other sheet details
  check(label + 'privacy: the rough area is said', [has(R(M({ on: false }), null), 'your area, rounded to about 10 km')], [true]);
  h = R(M({ lookup: { state: 'done', near: true, result: 'partial', list: [apt('EGKK', 41)] } }), null);
  check(label + 'partial: the list and the note', [has(h, 'data-a="add" data-id="EGKK"'), has(h, W.partial)], [true, true]);
  h = R(M({ airports: [apt('EGLL', 23)], lookup: { state: 'done', near: false, result: 'ok', list: [apt('EGLL', 23)] } }), null, { code: 'EGLL', asked: true });
  check(label + 'already in the list', [has(h, 'EGLL is already in your airports.'), has(h, 'No airport EGLL')], [true, false]);
  h = R(M({ airports: [apt('EGLC', 90)] }), { airports: [row('EGLC', { km: 90 })],
    reference: { qnh_hpa: 1018.1, qnh_from: [{ icao: 'EGLC', km: 90, age_s: 600 }], qnh_inconsistent: false } });
  check(label + 'no reference line from 90 km', has(h, 'Pressure reference'), false);
  check(label + 'wind templates', [W.wind_dir, W.wind_vrb, W.gusts], ['{d}° {s}', 'variable {s}', '{w}, gusts {g}']);
  // the forecast line (the outlook's words): nothing before the first TAF fetch, then "no forecast",
  // "nothing significant", or the first window of each phenomenon with its hedge
  const hmL = iso => { const d = new Date(iso), p = n => String(n).padStart(2, '0'); return p(d.getHours()) + ':' + p(d.getMinutes()); };
  const tw = (what, possible, from, to) => ({ what, possible, from, to });
  // taf null = not asked in an hourly cycle yet; absent (asked, not in the answer) = no forecast
  check(label + 'taf: off, nothing', f.forecast(row('EGLL'), false), '');
  check(label + 'taf: on, not asked yet', f.forecast(row('EGLL'), true), W.taf_wait);
  check(label + 'taf: absent from the answer', f.forecast(row('EGLL', { taf: { status: 'absent', current: false, next12: [] } }), true), W.taf_na);
  check(label + 'taf: expired', f.forecast(row('EGLL', { taf: { current: false, next12: [] } }), true), W.taf_na);
  // the row carries no forecast head ("Forecast, next 12 h:" / "Forecast to {to}:" are the dashboard's)
  check(label + 'no forecast head', [f.forecastHead, W.taf_head, W.taf_head_to], [undefined, undefined, undefined]);
  check(label + 'taf: nothing significant', f.forecast(row('EGLL', { taf: { current: true, next12: [] } }), true), W.taf_none);
  const ts = '2026-11-14T14:00:00Z', te = '2026-11-14T18:00:00Z', g0 = '2026-11-14T20:00:00Z', g1 = '2026-11-14T23:00:00Z';
  const line = f.forecast(row('EGLL', { taf: { current: true, next12: [tw('rain', true, ts, te), tw('rain', false, g0, g1), tw('gusts', false, g0, g1)] } }), true);
  check(label + 'taf: first window per phenomenon, hedges', line,
    `Rain possible ${hmL(ts)}–${hmL(te)}. Strong gusts likely ${hmL(g0)}–${hmL(g1)}.`);
  check(label + 'taf 3d(i): gusts without a number, one family with the dashboard', [W.taf_p, f.forecast(row('EGLL', { taf: { current: true, next12: [tw('gusts', true, ts, te)] } }), true)],
    ['{what} possible {from}–{to}.', `Strong gusts possible ${hmL(ts)}–${hmL(te)}.`]);
  // a window the board cut at its horizon (cut: true): no end time, one whole template per certainty
  check(label + 'taf: a cut window has no end', f.forecast(row('EGLL', { taf: { current: true, next12: [Object.assign(tw('rain', false, ts, te), { cut: true }),
    Object.assign(tw('fog', true, ts, te), { cut: true })] } }), true), `Rain likely from ${hmL(ts)}. Fog possible from ${hmL(ts)}.`);
  // a window that began before now (started: from = now): no start time -- "throughout" when also cut, else "until {to}"
  check(label + 'started + cut: throughout', f.forecast(row('EGLL', { taf: { current: true, next12: [Object.assign(tw('rain', false, ts, te), { cut: true, started: true }),
    Object.assign(tw('fog', true, ts, te), { cut: true, started: true })] } }), true), 'Rain likely throughout. Fog possible throughout.');
  check(label + 'started, ends inside: until', f.forecast(row('EGLL', { taf: { current: true, next12: [Object.assign(tw('rain', false, ts, te), { started: true }),
    Object.assign(tw('gusts', true, ts, te), { started: true })] } }), true), `Rain likely until ${hmL(te)}. Strong gusts possible until ${hmL(te)}.`);
  check(label + 'not started: unchanged', f.forecast(row('EGLL', { taf: { current: true, next12: [Object.assign(tw('rain', true, ts, te), { started: false })] } }), true),
    `Rain possible ${hmL(ts)}–${hmL(te)}.`);
  h = R(M({ airports: [apt('EGLL', 23)] }), { airports: [row('EGLL', { km: 23, taf: { current: true, next12: [tw('thunder', true, ts, te)] } })],
    reference: null, last_taf: { t: '2026-11-14T12:50:00Z', result: 'ok' } });
  check(label + 'taf: the line on the sheet, no head', [has(h, 'Forecast,'), has(h, 'Thunder possible')], [false, true]);
  h = R(M({ airports: [apt('EGLL', 23)] }), { airports: [row('EGLL', { km: 23, taf: null })], reference: null, last_taf: { t: null, result: 'fail' } });
  check(label + 'taf: a failed first cycle is no "no forecast"', [has(h, W.taf_na), has(h, W.taf_wait)], [false, true]);
  h = R(M({ lookup: { state: 'done', near: true, result: 'ok', list: [Object.assign(apt('EGKK', 41), { taf: true }), apt('EGLC', 50)] } }), null);
  check(label + 'taf: the badge in the found list', [has(h, 'about 41 km away · forecast'), has(h, 'about 50 km away · forecast')], [true, false]);
  check(label + 'taf: the badge is one template', W.taf_badge, '{d} · forecast');
  // the list's order is the priority -- the first is the main airport, ↑ / ↓ move a row, Add goes in by km
  h = R(M({ airports: [apt('EGLL', 18), apt('EGKK', 41), apt('ZZZZ', null)] }), null);
  check(label + '3d(i): main marked once', h.split(W.main).length - 1, 1);
  check(label + '3d(i): no up on the first, no down on the last', [has(h, 'data-a="up" data-id="EGLL"'), has(h, 'data-a="down" data-id="EGLL"'),
    has(h, 'data-a="up" data-id="ZZZZ"'), has(h, 'data-a="down" data-id="ZZZZ"')], [false, true, true, false]);
  check(label + '3d(i): up / down labels', has(h, 'aria-label="Move EGKK up"') && has(h, 'aria-label="Move EGKK down"'), true);
  h = R(M({ airports: [apt('EGLL', 18), apt('EGKK', 41)] }), null, { moving: true });
  check(label + 'list edits: Remove and Add wait too while the list is saved', [has(h, 'data-a="rm" data-id="EGLL" disabled'), has(h, 'data-a="rm" data-id="EGKK" disabled')], [true, true]);
  check(label + 'list edits: Add waits in the found list', has(R(M({ airports: [apt('EGLL', 18)], lookup: { state: 'done', near: true, result: 'ok', list: [apt('EGSS', 60)] } }), null, { asked: true, moving: true }), 'data-a="add" data-id="EGSS" disabled'), true);
  check(label + 'up / down wait while a move is saved', has(h, 'aria-label="Move EGKK up" disabled') && has(h, 'aria-label="Move EGLL down" disabled'), true);
  const m3 = M({ airports: [apt('EGLL', 18), apt('EGKK', 41)], lookup: { state: 'done', near: true, result: 'ok', list: [apt('EGLC', 25), apt('EGSS', 60)] } });
  check(label + '3d(i): add by km, between', S.insertByKm(['EGLL', 'EGKK'], 'EGLC', m3), ['EGLL', 'EGLC', 'EGKK']);
  check(label + '3d(i): add by km, last', S.insertByKm(['EGLL', 'EGKK'], 'EGSS', m3), ['EGLL', 'EGKK', 'EGSS']);
  check(label + '3d(i): add by km after a reorder keeps the user rows', S.insertByKm(['EGKK', 'EGLL'], 'EGLC', m3), ['EGKK', 'EGLL', 'EGLC']);
  check(label + '3d(i): a nearer one than all goes first', S.insertByKm(['EGLL', 'EGKK'], 'EGXX', M({ airports: [apt('EGLL', 18), apt('EGKK', 41)], lookup: { list: [apt('EGXX', 5)] } })), ['EGXX', 'EGLL', 'EGKK']);
  check(label + '3d(i): unknown km goes last', S.insertByKm(['EGLL'], 'XXXX', m3), ['EGLL', 'XXXX']);
  // the found list only for a lookup asked in this visit; flags from the ICAO prefix; the name before the code; the switch
  const lk = M({ lookup: { state: 'done', near: true, result: 'ok', list: [apt('EGLC', 12, 'London City'), apt('KJFK', null, 'JF Kennedy'), apt('XXZZ', 40)] } });
  check(label + 'an old lookup is not shown on a new visit', has(S.render(lk, null, {}), 'data-a="add"'), false);
  const h10 = R(lk, null);
  check(label + 'flags from the API cc field (GB, US), none without one', [has(h10, 'data-cc="GB"'), has(h10, 'data-cc="US"'), (h10.match(/data-cc=/g) || []).length], [true, true, 2]);
  check(label + 'the name first, then the code', has(h10, '<b>London City</b> <span class="hint">EGLC</span>'), true);
  check(label + 'glass rows', has(h10, '<li class="glass">'), true);
  check(label + 'the module switch is the shared toggle', /class="toggle" for="mOn"/.test(R(M(), null)), true);
  check(label + 'no figure in the none-found text', W.none, 'No airport with reports near your location. Add one by its code.');
  check(label + 'the switch says hourly forecasts, in two short lines', /every 15 min and forecasts every hour/.test(W.on_s) && /internet address/.test(W.on_s2), true);
}

// every list edit (remove, add, up / down) shows at once from the list on screen, and the list's buttons wait for the reload:
// a quick second Remove neither brings the first airport back nor gets lost; the next edit starts from the saved list
async function edits(label, src) {
  const tick = () => new Promise(r => setTimeout(r, 0)), posts = [], pend = [];
  let server = M({ airports: [apt('EGLL', 18), apt('EGKK', 41), apt('EGLC', 25)], lookup: { state: 'done', near: true, result: 'ok', list: [apt('EGSS', 60)] } });
  const api = (meth, url, body) => meth !== 'POST' ? Promise.resolve(JSON.parse(JSON.stringify(server)))
    : new Promise(res => { posts.push(body.airports); pend.push(() => { server = Object.assign({}, server, { airports: body.airports.map(id => apt(id, id === 'EGSS' ? 60 : 30)) }); res({}); }); });
  const sh = load(src, api, Object.assign(Object.create(FC), { get: () => Promise.resolve({ data: null }) }))();
  const click = (a, id) => sh.click({ dataset: { a, id } }), saved = async () => { pend.shift()(); for (let i = 0; i < 4; i++) await tick(); };
  await tick(); await tick();
  click('rm', 'EGLL'); click('rm', 'EGKK');
  check(label + 'list edits: two quick Removes post once, without the first', posts, [['EGKK', 'EGLC']]);
  const b = sh.body();
  check(label + 'list edits: the row goes at once, the rest wait', [b.includes('data-id="EGLL"'), b.includes('data-a="rm" data-id="EGKK" disabled')], [false, true]);
  await saved();
  check(label + 'list edits: after the reload the buttons work again', sh.body().includes('data-a="rm" data-id="EGKK" disabled'), false);
  click('rm', 'EGKK');
  check(label + 'list edits: the second Remove starts from the saved list', posts[1], ['EGLC']);
  await saved();
  click('add', 'EGSS'); click('add', 'EGSS');
  check(label + 'list edits: Add posts once, in km order', posts.slice(2), [['EGLC', 'EGSS']]);
  check(label + 'list edits: the added row shows at once', sh.body().includes('data-a="rm" data-id="EGSS" disabled'), true);
  await saved();
  click('up', 'EGSS');
  check(label + 'list edits: up / down use the same path', posts[3], ['EGSS', 'EGLC']);
}

run('', plain);
if (mangled) run('mangled ', mangled);
(async () => {
  await edits('', plain);
  if (mangled) await edits('mangled ', mangled);
  console.log(fail ? `FAILED: ${pass} passed, ${fail} failed` : `OK: ${pass} passed, 0 failed`);
  process.exit(fail ? 1 : 0);
})();
