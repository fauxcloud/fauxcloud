// test/tools/text_vectors.js -- the dashboard's words as rendered. renderHealth (Details) and
// renderBar (the chips) are taken out of firmware/fauxcloud/web/dash.js by name and run in a vm with stub elements (the
// core_vectors.js pattern): no raw field key in Details, the offline chip template with its tooltip, "No answer from
// fauxcloud", notice chips without re-casing, and OFF_WHY in the station's state line.
// Usage: node test/tools/text_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WEB = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web');
const dash = fs.readFileSync(path.join(WEB, 'dash.js'), 'utf8');
const core = fs.readFileSync(path.join(WEB, 'core.js'), 'utf8');
// the Details live in Settings (admin-station.js healthDetails), the status row in health.js (both faces)
const station = fs.readFileSync(path.join(WEB, 'admin-station.js'), 'utf8');
const health = fs.readFileSync(path.join(WEB, 'health.js'), 'utf8');
const adminface = fs.readFileSync(path.join(WEB, 'adminface.js'), 'utf8');

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

const { decl, fcText } = require('./js_decl');

const els = {};
const el = sel => els[sel] || (els[sel] = { innerHTML: '', textContent: '', hidden: false, open: false, href: '' });
function run(S, face) {
  const ctx = { S, FC: { face }, IMP: false, $: el, dur: s => s + ' s', healthRow: () => '', rejChip: t => `[${t}]`,
    Object, String, Math, Array, JSON, Date };
  vm.createContext(ctx);
  // own / esc as in core.js (their regex literal is outside decl()'s scanner); checked against core.js below
  const stubs = "const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;\n" +
    "const esc = s => String(s ?? '').replace(/[&<>\"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;' }[c]));";
  if (!stubs.split('\n').every(l => core.includes(l))) throw new Error('own / esc changed in core.js: update the stubs');
  const dnames = ['nameOf', 'WD', 'NT', 'ap', 'noSlotChip', 'ntx', 'derWhy', 'renderHealth', 'NOTICE_TILE', 'tileNote', 'renderBar', 'stateLine'];
  vm.runInContext(stubs + '\n' + fcText(core) + decl(core, 'WC') + '\n' + decl(core, 'ago') + '\n' + ['p2', 'hm', 'dm', 'WH', 'dayTime', 'offSince'].map(n => decl(health, n)).concat(dnames.map(n => decl(dash, n))).join('\n') +
    '\nthis.f = { renderHealth, renderBar, OFF_WHY: WD.off_why, hm, stateLine, tileNote };', ctx);
  return ctx.f;
}

const live = {
  id: 'ST1', uploads: 12, src_if: 'ap', src_ip: '192.168.77.2',
  rejects_since_boot: { wind_kmh: 2, foo_x: 3, bar_y: 1, humidity: 1 },
  extras: { soilmoisture: 31 },
  derived: { wind_avg2m_kmh: 30, wind_avg10m_kmh: 11, mystery_k: 2 },
};
const head = { diag: { fw_version: '0.1.0-b14', mqtt_up: true, heap_low: false, heap_free: 60000, heap_min: 50000, sta_up: true,
  sta_rssi: -60, sta_ip: '192.168.1.20', ap_clients: 1, uploads: 12, mqtt_published: 40, mqtt_failed: 0 }, time_synced: true, uptime_s: 99,
  station: { derived: live.derived } };

// 1. Details (in Settings): labels, never a raw key; unknown keys summed; the summary's two words
const details = (() => {
  const ctx = { Object, String, Math, Array, JSON, Set, dur: s => s + ' s', FC: {} };
  vm.createContext(ctx);
  vm.runInContext(fcText(core) + decl(station, 'WA') + '\n' + "const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;\n" +
    "const esc = s => String(s ?? '').replace(/[&<>\"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '\"': '&quot;' }[c]));\n" +
    decl(station, 'LBL') + '\n' + decl(station, 'healthDetails') + '\nthis.f = healthDetails;', ctx);
  return ctx.f;
})();
{
  const h = details({ diag: head.diag, uptime_s: 99, stations: [Object.assign({ name: 'Garden' }, live)] });
  check('Rejected: Wind', h.includes('<dt>Rejected: Wind</dt><dd>2</dd>'), true);
  check('Rejected: Humidity', h.includes('<dt>Rejected: Humidity</dt><dd>1</dd>'), true);
  check('unknown keys summed into other readings', h.includes('<dt>Rejected: other readings</dt><dd>4</dd>'), true);
  check('no raw key as a rejected label', /Rejected: [a-z]+_[a-z]/.test(h), false);
  check('Other data from the console', h.includes('<dt>Other data from the console</dt><dd>soilmoisture=31</dd>'), true);
  check('computed: labels, an unknown key as sent', h.includes('<dd>Wind, 2-min average, Wind, 10-min average, mystery_k</dd>'), true);
  check('no raw known key in Details', ['>wind_kmh<', '>wind_avg2m_kmh<', '>wind_avg10m_kmh<', '>humidity<'].some(k => h.includes(k)), false);
  check('station heading', h.includes('<h3>Garden (ST1)</h3>'), true);
  check('summary words', adminface.includes('<summary data-more="${WF.details}" data-less="${WF.hide_details}">') && adminface.includes("details: 'Details', hide_details: 'Hide details'"), true);
  check('dropped rows', ['Queue drops', 'Other hits', 'DNS', 'NTP', 'Largest block'].some(k => h.includes(k)), false);
  check('none rejected', details({ diag: head.diag, stations: [Object.assign({}, live, { rejects_since_boot: {} })] }).includes('<dt>Rejected readings</dt><dd>none</dd>'), true);
  check('AP face: no bridge block', details({ stations: [live] }).includes('<h3>Bridge</h3>'), false);
}
// 1b. the status row (health.js): glyphs only, the words in title and aria-label, weak Wi-Fi amber
{
  const ctx = { String, Math, FC: {} };
  vm.createContext(ctx);
  vm.runInContext(fcText(core) + ['WH', 'STIC', 'stSvg', 'WIFI', 'healthRow'].map(n => decl(health, n)).join('\n') + '\nthis.f = healthRow;', ctx);
  const r = ctx.f(head), weak = ctx.f(Object.assign({}, head, { diag: Object.assign({}, head.diag, { sta_rssi: -75, mqtt_up: false }) }), true);
  check('row: four states', (r.match(/class="state[" ]/g) || []).length, 4);
  check('row: no visible words', r.replace(/<[^>]*>/g, ''), '');
  check('row: words in aria', ['Clock: synced', 'MQTT: connected', 'Memory: 59 KB free', 'Wi-Fi: good, \u221260 dBm'].every(w => r.includes(`aria-label="${w}"`)), true);
  check('row: weak Wi-Fi and MQTT down are amber', (weak.match(/class="state warn"/g) || []).length, 2);
  // no broker set: no MQTT glyph at all (never a standing amber "disconnected"); connected shows even before the caller knows
  const none = ctx.f(Object.assign({}, head, { diag: Object.assign({}, head.diag, { mqtt_up: false }) }), false);
  check('row: no broker, three states', (none.match(/class="state[" ]/g) || []).length, 3);
  check('row: no broker, no MQTT words, no amber', none.includes('MQTT') || none.includes('warn'), false);
  check('row: up without the broker flag still shows', ctx.f(head).includes('aria-label="MQTT: connected"'), true);
  check('row: broker set and down: amber MQTT', /class="state warn"[^>]*aria-label="MQTT: disconnected"/.test(ctx.f(Object.assign({}, head, { diag: Object.assign({}, head.diag, { mqtt_up: false }) }), true)), true);
  check('row: AP face shows the clock only', (ctx.f({ time_synced: false, time_src: 'persisted' }).match(/aria-label="Clock: approximate"/g) || []).length, 1);
}

// 2. The bar: "No answer from fauxcloud" first, the offline chip template + OFF_WHY tooltip, notice chips as sent
{
  const S = { names: { ST1: 'Garden' }, cur: 'ST1', stale: true,
    status: { sta: true, mqtt: 1, ids: ['ST1', 'ST2'], st: 'os', notices: [{ chip: 'Time approximate', text: 'Time is estimated since the restart.' }] },
    live: { station: { values: {}, notices: [{ chip: 'Sensor not updating', text: 'Outdoor sensor not updating since 10:00.' }] } } };
  const f = run(S, 'lan');
  f.renderBar();
  const b = els['#bridge'].innerHTML;
  check('No answer first', b.startsWith('<span class="rej">No answer from fauxcloud</span>'), true);
  check('bridge notice chip keeps its case', b.includes('title="Time is estimated since the restart.">Time approximate</span>'), true);
  check('offline chip', b.includes(`<button type="button" class="chip" data-id="ST2" title="${f.OFF_WHY}" aria-label="${f.OFF_WHY.replace(/\.$/, '')}. Show this station">ST2: offline</button>`), true);
  check('no doubled full stop in an aria-label', b.includes('.. Show'), false);
  check('station notice chip: name + chip as sent', b.includes('>Garden: Sensor not updating</button>'), true);
  check('no lower-cased chip', b.includes('sensor not updating</button>'), false);
  check('no old wording', /No NTP|lost connection/i.test(b), false);
}
// 2b. a tile notice leaves the chips while its tile exists (one at a time, the factory value first); without a
// pressure reading it stays a chip
{
  const ref = { id: 'baro_ref', chip: 'Pressure looks off', text: 'This station reads about 2.7 hPa above nearby airports.' };
  const unc = { id: 'baro_uncalibrated', chip: 'Pressure at factory value', text: 'The console is at its factory value.' };
  const mk = (values, notices) => ({ names: { ST1: 'Garden' }, cur: 'ST1', status: { sta: true, mqtt: 1, ids: ['ST1'], st: 'o', notices: [] },
    live: { station: { values, notices } } });
  let S = mk({ pressure_hpa: 1015.9 }, [ref]), f = run(S, 'lan');
  f.renderBar();
  check('baro_ref on the tile, not a chip', [els['#bridge'].innerHTML.includes('Pressure looks off'), f.tileNote('baro').id], [false, 'baro_ref']);
  S = mk({}, [ref]); f = run(S, 'lan');
  f.renderBar();
  check('no pressure tile -> the chip stays', [els['#bridge'].innerHTML.includes('Garden: Pressure looks off'), f.tileNote('baro')], [true, null]);
  S = mk({ pressure_hpa: 1013.2 }, [ref, unc]); f = run(S, 'lan');
  check('the factory value outranks the airports', f.tileNote('baro').id, 'baro_uncalibrated');
  const chk = { id: 'baro_check', chip: 'Pressure looks off', text: 'This station averaged about 3.0 hPa above the usual.' };
  S = mk({ pressure_hpa: 1016.2 }, [chk, ref]); f = run(S, 'lan');
  check('baro_ref outranks baro_check', f.tileNote('baro').id, 'baro_ref');
  S = mk({ pressure_hpa: 1016.2 }, [chk]); f = run(S, 'lan');
  f.renderBar();
  check('baro_check accepted on the tile, not a chip', [f.tileNote('baro').id, els['#bridge'].innerHTML.includes('Pressure looks off')], ['baro_check', false]);
  S = mk({ pressure_hpa: 1013.2 }, [{ id: 'sensor_stale', chip: 'Sensor not updating', text: '.' }, unc]); f = run(S, 'lan');
  f.renderBar();
  check('other notices stay chips', [els['#bridge'].innerHTML.includes('Garden: Sensor not updating'), els['#bridge'].innerHTML.includes('Pressure at factory value')], [true, false]);
}

// 3. The station's state line: the day rule of offSince() with a synced clock, the age before it;
// OFF_WHY as its own sentence (no slice of a UI sentence)
{
  const WHY = 'The console sends nothing on batteries or during a power cut; readings resume on their own.';
  const line = (synced, lastSeen, age_s) => {
    const S = { names: {}, liveAt: Date.now(), status: null, hourly: null, stale: false,
      live: { time_synced: synced, station: { id: 'ST1', uploads: 5, online: false, age_s, last_seen: lastSeen, values: {} } } };
    run(S, 'lan').stateLine();
    return els['#stState'].innerHTML.replace('<span class="dot off"></span><span>', '').replace(/<\/span>$/, '');
  };
  const f = run({ names: {} }, 'lan');
  check('OFF_WHY', f.OFF_WHY, WHY);
  check('no slice of a UI sentence', dash.includes('OFF_WHY.slice'), false);
  const two = new Date(Date.now() - 2 * 864e5);
  two.setHours(14, 5, 0, 0);
  const mon = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][two.getMonth()];
  check('synced, two days old: the date', line(true, two.toISOString(), 2 * 86400), `Offline since ${two.getDate()} ${mon} 14:05. ${WHY}`);
  // before the clock is set last_seen is seconds since boot (1970): never a clock time, the age instead
  check('unsynced: the age, no clock time', line(false, '1970-01-01T00:12:00Z', 600), `Offline. Last upload 10 min ago. ${WHY}`);
  // the "restored" branch (no upload since the restart, the last reading from flash) under the same rule
  const restored = (synced, lastSeen) => {
    const S = { names: {}, liveAt: Date.now(), status: null, hourly: null, stale: false,
      live: { time_synced: synced, station: { id: 'ST1', uploads: 0, restored: true, online: false, age_s: 9e4, last_seen: lastSeen, values: {} } } };
    run(S, 'lan').stateLine();
    return els['#stState'].innerHTML.replace('<span class="dot off"></span><span>', '').replace(/<\/span>$/, '');
  };
  const yest = new Date(Date.now() - 864e5);
  yest.setHours(23, 40, 0, 0);
  check('restored, synced: the day rule', restored(true, yest.toISOString()), 'Offline. No upload since the restart; showing the last reading from yesterday 23:40.');
  const today = new Date();
  today.setHours(0, 5, 0, 0);
  check('restored, synced, today: the time', restored(true, today.toISOString()), 'Offline. No upload since the restart; showing the last reading from 00:05.');
  check('restored, unsynced: no time', restored(false, yest.toISOString()), 'Offline. No upload since the restart; showing the last reading kept on flash.');
  check('restored, no last_seen: no time', restored(true, ''), 'Offline. No upload since the restart; showing the last reading kept on flash.');
  // a pre-2020 time is uptime: never shown as a date, even with a synced clock
  check('restored, synced, 1970: no time', restored(true, '1970-01-01T00:20:00Z'), 'Offline. No upload since the restart; showing the last reading kept on flash.');
  check('synced, 1970 last_seen: the age', line(true, '1970-01-01T00:12:00Z', 600), `Offline. Last upload 10 min ago. ${WHY}`);
}

// 4. pairing step 3 is one sentence in core.js (inlined in every face), used by the dashboard's empty
// state, the admin's "No station yet" card and AP sheet, and setup's last step; never copied as text
{
  const web = f => fs.readFileSync(path.join(WEB, f), 'utf8');
  const m = core.match(/^ {2}wu_step: '([^']*)',$/m);  // core.js's W table (WC); WU_STEP = WC.wu_step for admin and setup
  check('WU_STEP in core.js', !!m, true);
  const step = m ? m[1] : '';
  // the page named, and the key rule (first key kept; a new one is accepted in Settings)
  check('WU_STEP names the page and the box', /^On the console’s page \(192\.168\.1\.1\), tick <b>Weather Underground<\/b>/.test(step), true);
  check('WU_STEP states the key rule', /remembers the first key/.test(step) && /accept the new key in Settings\.$/.test(step), true);
  check('WU_STEP never says the key is not checked', /does not check|any station ID/.test(step), false);
  check('WU_STEP plain words', step.length <= 230 && !/\b(must|simply|just)\b/i.test(step), true);
  const uses = { 'dash.js': 1, 'adminface.js': 2, 'setup.js': 1 };
  for (const [f, n] of Object.entries(uses)) {
    check('WU_STEP used in ' + f, (web(f).match(f !== 'setup.js' ? /\$\{WC\.wu_step\}/g : /\$\{WU_STEP\}/g) || []).length, n);  // the translated faces read the table when they draw
    check('no copy of the sentence in ' + f, web(f).includes('a key of your choice'), false);
  }
}

// 5. log levels: the admin's Maintenance sheet has the detailed-log switch, off unless the bridge says on
{
  const adm = fs.readFileSync(path.join(WEB, 'admin-sheets.js'), 'utf8');  // Maintenance is a lazy sheet
  check('detailed log switch: its name', adm.includes("tog('logv', API.bridge.log_verbose, WY.x_logv") && adm.includes("x_logv: 'Log every upload and page request'"), true);
  // Maintenance in one order -- update, restart, the Bug report group (switch, then the log), reset
  const at = k => adm.indexOf(k);
  check('Maintenance order', [at('<h3>${WY.x_upd}</h3>'), at('<h3>${WY.x_restart}</h3>'), at('<h3>${WY.x_bug}</h3>'), at("tog('logv'"), at('id="blGet"'), at('<h3>${WY.x_reset}</h3>')].every((v, i, a) => v > 0 && (!i || v > a[i - 1])), true);
  check('switch saves log_verbose', adm.includes("api('PUT', '/api/admin/bridge', { log_verbose: on })"), true);
}

// 5b. the MQTT sheet: the "keep the saved password" hint exists only while the saved password stays (same host and port), so a
// screen reader never reads it through the password field's aria-describedby once a new broker is typed
{
  const src = fs.readFileSync(path.join(WEB, 'admin-sheets.js'), 'utf8'), D = require('./js_decl');
  const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const fld = { html: '', insertAdjacentHTML(w, h) { this.html += h; els['#mpass-msg'] = { remove() { delete els['#mpass-msg']; } }; } };
  const els = { '#mpass': { placeholder: 'x', closest: s => s === '.fld' ? fld : null }, '#mpass-msg': { remove() { delete els['#mpass-msg']; } } };
  const SHEETS = {}, API = { settings: { mqtt_host: 'ha.local', mqtt_port: 1883, mqtt_user: 'u', mqtt_pass: '<set>' }, status: { mqtt_up: true } };
  const any = new Proxy(function () { return ''; }, { get: (t, k) => (k === Symbol.toPrimitive ? () => '' : any), apply: () => '' });
  const given = new Set(['stub', 'esc', 'SHEETS', 'API', 'FC', '$', 'clone', 'num', 'field', 'text']);
  const stub = new Proxy({}, { has: (t, k) => typeof k === 'string' && !given.has(k) && !(k in globalThis), get: (t, k) => (k === Symbol.unscopables ? undefined : any) });
  new Function('stub', 'esc', 'SHEETS', 'API', 'FC', '$', 'clone', 'num', D.decl(adminface, 'field') + '\n' + D.decl(adminface, 'text') + `\nwith (stub) { ${src} }`)(
    stub, esc, SHEETS, API, D.fcStub(), s => els[s] || null, o => JSON.parse(JSON.stringify(o)), v => (v === '' ? null : +v));
  const sh = SHEETS.mqtt(); sh.err = {};
  const hint = h => /<p class="hint" id="mpass-msg">Leave empty to keep the saved password\.<\/p>/.test(h), node = h => /<p [^>]*id="mpass-msg"/.test(h);
  check('MQTT keep hint: same broker: the keep hint', hint(sh.body()), true);
  sh.input({ id: 'mhost', value: 'other.local' });
  check('MQTT keep hint: a new host: the hint element goes (not hidden), the placeholder empties', ['#mpass-msg' in els, els['#mpass'].placeholder], [false, '']);
  check('MQTT keep hint: a new host renders no hint element', node(sh.body()) || sh.body().includes(' hidden'), false);
  sh.input({ id: 'mhost', value: 'ha.local' });
  check('MQTT keep hint: back to the saved broker: the hint comes back once', ['#mpass-msg' in els, hint(fld.html), els['#mpass'].placeholder], [true, true, 'Saved, type to change']);
  sh.input({ id: 'mport', value: '1884' });
  check('MQTT keep hint: a new port drops it too', '#mpass-msg' in els, false);
  API.settings.mqtt_pass = ''; const fresh = SHEETS.mqtt(); fresh.err = {};
  check('MQTT keep hint: no saved password: no hint', node(fresh.body()), false);
}

// 4. the wind dial's ring and its label follow the samples' time span, not the ring's 60 slots
{
  const ctx = { U: { w: 'km/h' }, cv: (k, x) => x, calm: () => false, Math, String, Array, FC: {} };
  vm.createContext(ctx);
  vm.runInContext(fcText(core) + ['WD', 'dsep', 'f1', 'CARD', 'card', 'dial'].map(n => decl(dash, n)).join('\n') + '\nthis.dial = dial;', ctx);
  const t = Date.now(), smp = (min, dir) => ({ t: new Date(t - min * 6e4), wind_kmh: 10, wind_dir: dir });
  const m = (hr, hrMin) => ({ v: { wind_kmh: 10, wind_dir: 90 }, hr, hrMin });
  check('a full hour', ctx.dial(m([smp(59, 90), smp(0, 90)], 59)).includes('Outer ring: directions over the last hour.'), true);
  check('a short ring says its minutes', ctx.dial(m([smp(20, 90), smp(0, 90)], 20)).includes('Outer ring: directions over the last 20 min.'), true);
  check('the template is whole in dash.js', ["avg_m: 'Average, last {m} min'", "avg_h: 'Average, last hour'", "avg_gust_m: 'Average gust, last {m} min'"].every(x => dash.includes(x)), true);
  check('the hour is cut by time, the 10-min gust too', ['recent.filter(x => x.t >= tn - HOUR)', 'm.recent.filter(x => x.t >= m.tn - 6e5)'].every(x => dash.includes(x)), true);
  check('no count-based slice left', dash.includes('slice(-10)'), false);
  // lit sectors need 2 readings and 5 % of the hour's non-calm ones (a calm hour with strays, a gusty hour)
  const lit = svg => (svg.match(/stroke="var\(--wind\)" stroke-width="9"/g) || []).length;
  const strays = [0, 45, 90, 135, 180, 225, 270, 315].map((d, i) => smp(i * 7, d));
  check('eight single strays light nothing', lit(ctx.dial(m(strays, 59))), 0);
  const gusty = Array.from({ length: 40 }, (_, i) => smp(i, 250)).concat([smp(41, 90), smp(42, 90), smp(43, 180)]);
  check('a steady sector lights, a 2-reading sector under 5 % does not, a stray does not', lit(ctx.dial(m(gusty, 59))), 1);
  const two = Array.from({ length: 20 }, (_, i) => smp(i, 250)).concat([smp(30, 90), smp(31, 90)]);
  check('2 readings at >= 5 % light their sector', lit(ctx.dial(m(two, 59))), 2);
}

// 6. the console turned away for lack of a slot, named on the dashboard chip and in the admin line; no "Stations
// kept" field and no max_stations at cap 1; the "Memory is low" callout does not point to that field
{
  const ctx = { esc: s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])), String, FC: {} };
  vm.createContext(ctx);
  const adm = fs.readFileSync(path.join(WEB, 'adminface.js'), 'utf8'), st = fs.readFileSync(path.join(WEB, 'admin-station.js'), 'utf8');
  vm.runInContext(fcText(core) + decl(dash, 'WD') + '\n' + decl(dash, 'noSlotChip') + '\n' + decl(adm, 'WF') + '\n' + decl(adm, 'ADOPT_LINE') + '\n' + decl(adm, 'ADOPT_BTN') + '\n' + decl(adm, 'NO_SLOT') +
    '\n' + decl(st, 'WA') + '\nthis.f = { noSlotChip, NO_SLOT, ADOPT_ASK: WA.adopt_ask };', ctx);
  const { noSlotChip, NO_SLOT, ADOPT_ASK } = ctx.f;
  // the same device as the loaded station -> the offer, both ids named, the button opens the station's sheet
  check('admin line: the adopt offer', NO_SLOT('NEW1', [{ id: 'OLD1', adopt: 'NEW1' }], 1),
    'Console <b>NEW1</b> uploads from the same device as <b>OLD1</b>. Did its ID change? <button type="button" class="link" data-sheet="st0" data-adopt="1">Keep the history under NEW1</button>');
  check('admin line: an offer for another id is not shown', NO_SLOT('NEW2', [{ id: 'OLD1', adopt: 'NEW1' }], 1).includes('forget'), true);
  check('admin line: no offer at cap 2', NO_SLOT('NEW1', [{ id: 'OLD1', adopt: 'NEW1' }, { id: 'ST2' }], 2).includes('data-adopt'), false);
  check('admin line escapes both ids', NO_SLOT('<n>', [{ id: '<o>', adopt: '<n>' }], 1).includes('<b>&lt;n&gt;</b> uploads from the same device as <b>&lt;o&gt;</b>'), true);
  const ask = ADOPT_ASK.replace(/\{from\}/g, 'OLD1').replace(/\{to\}/g, 'NEW1');
  check('confirmation names both ids', [ask.includes('Move <b>OLD1</b> to <b>NEW1</b>?'), ask.includes('OLD1 stops.'), ask.includes('accepts NEW1’s key')],
    [true, true, true]);
  check('chip: id and the one to forget', JSON.stringify(noSlotChip({ no_slot_id: 'KNOCK1', ids: ['ST1'] })),
    JSON.stringify(['Console KNOCK1 turned away', 'This fauxcloud keeps one console. To switch, forget ST1 in Settings.']));
  check('chip: two places', noSlotChip({ no_slot_id: 'KNOCK1', ids: ['ST1', 'ST2'] })[1], 'All station places are in use. To take it, forget a station in Settings.');
  check('chip: no id (older count only)', noSlotChip({ no_slot: 2, ids: ['ST1'] }), 'A console was turned away: no free station place');
  check('admin line at cap 1', NO_SLOT('KNOCK1', [{ id: 'ST1' }], 1),
    'Console <b>KNOCK1</b> tried to upload, but this fauxcloud keeps one console. To switch to it, forget <button type="button" class="link" data-sheet="st0">ST1</button>.');
  check('admin line at cap 2', NO_SLOT('KNOCK1', [{ id: 'ST1' }, { id: 'ST2' }], 2), 'Console <b>KNOCK1</b> tried to upload, but all station places are in use. To take it, forget a station first.');
  check('admin line escapes the id', NO_SLOT('<x>', [{ id: 'ST1' }], 1).includes('<b>&lt;x&gt;</b>'), true);
  // keyed on the cap; at cap 1 with no station loaded, no "forget" and no "all places"
  check('admin line at cap 1, none loaded', NO_SLOT('KNOCK1', [], 1), "Console <b>KNOCK1</b> tried to upload, but this fauxcloud's one station place is not free. Restart fauxcloud in Settings.");
  check('admin line at cap 2 with one loaded', NO_SLOT('KNOCK1', [{ id: 'ST1' }], 2).includes('all station places are in use'), true);
  check('chip: no station loaded', noSlotChip({ no_slot_id: 'KNOCK1', ids: [] }), 'A console was turned away: no free station place');
  // the Publishing tile, one template per case
  check('Publishing no longer counts stations; the memory note follows the cap', !adm.includes('one station on this board') && adm.includes("b.max_stations_cap === 1 ? WF.mem_1 : WF.mem_n") && adm.includes("mem_1: '<b>Memory is low.</b>fauxcloud keeps working, but open pages may fail."), true);
  check('no "up to 1 station" template left', /up to \$\{b\.max_stations\} station\$\{/.test(adm), false);
  check('admin line only while no_slot_id is set', adm.includes("${API.noSlot ? `<p class=\"hint\" role=\"status\">${NO_SLOT(API.noSlot, API.stations, API.bridge.max_stations_cap)}</p>` : ''}"), true);
  check('callout: no pointer to "Stations kept"', /Stations kept/.test(adm), false);
  check('Stations kept only above cap 1', st.includes("${cap > 1 ? field('maxst', WA.pub_maxst"), true);
  check('max_stations never sent at cap 1', st.includes('cap > 1 ? { max_stations: b.max_stations } : {}'), true);
}

// the offset from "Set north from the vane" -- saved offset minus the corrected reading, in (-180, 180]
{
  const st = fs.readFileSync(path.join(WEB, 'admin-station.js'), 'utf8'), ctx = {};
  vm.createContext(ctx);
  vm.runInContext(decl(st, 'northOffset') + ';' + decl(st, 'northAgree') + ';this.f = northOffset; this.g = northAgree;', ctx);
  const f = ctx.f, g = ctx.g;
  check('north: 355 and 3 agree (across north)', g(355, 3), true);
  check('north: 10 and 21 agree, 10 and 22 do not', [g(10, 21), g(10, 22)], [true, false]);
  check('north: 90 and 270 do not', g(90, 270), false);
  check('north: reading 90 with no offset -> -90', f(0, 90), -90);
  check('north: reading 270 -> 90', f(0, 270), 90);
  check('north: reading 180 -> 180 (not -180)', f(0, 180), 180);
  check('north: already north -> 0', f(0, 0), 0);
  check('north: saved +30, reading 30 -> 0', f(30, 30), 0);
  check('north: saved -20, reading 350 -> -10', f(-20, 350), -10);
  check('north: saved 200 wraps', f(200, 10), -170);
}
// a notice's "since" in the page's words (a language pack): the day word when it is not today; English shows the firmware's text
{
  const ctx = { FC: {}, Object, String, Math, Date };
  vm.createContext(ctx);
  vm.runInContext(fcText(core) + "Object.assign(FC, { L: { mon: t => 'M' + t.getMonth() } });\nconst own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;\nconst dsep = s => s;\n" +
    "const NT = { nc_sensor_absent: 'Missing', nt_sensor_absent: 'Missing since {since}.' };\n" + ['p2', 'hm', 'dm', 'WH', 'dayTime'].map(n => decl(health, n)).join('\n') + '\n' + decl(dash, 'ntx') + '\nthis.f = ntx;', ctx);
  const now = new Date(), at = (dd, h, m) => new Date(now.getFullYear(), now.getMonth(), now.getDate() - dd, h, m), n = since => ctx.f({ id: 'sensor_absent', since }).text;
  check('notice since today: the time alone', n(at(0, 0, 5).toISOString()), 'Missing since 00:05.');
  check('notice since yesterday: the day word', n(at(1, 14, 5).toISOString()), 'Missing since yesterday 14:05.');
  check('notice since 3 days: the date', n(at(3, 14, 5).toISOString()), `Missing since ${at(3, 0, 0).getDate()} M${at(3, 0, 0).getMonth()} 14:05.`);
  check('notice without a time', n(undefined), 'Missing since --:--.');
}
// the live answer's age counts from the 200 that brought the body: a 304 (also one replayed after a station switch) keeps it
{
  const S = { live: null, liveAt: 0, names: {}, status: { ids: ['A'] } };
  const ctx = { S, FC: { store() {}, kick() {} }, renderBar() {}, render() {}, stateLine() {}, Date, JSON };
  vm.createContext(ctx);
  vm.runInContext(decl(dash, 'onLive') + '\nthis.f = onLive;', ctx);
  const body = { station: { id: 'A', name: '' } }, t0 = Date.now() - 12 * 60e3;
  ctx.f({ data: body, changed: false, status: 304, at: t0 });
  check('a 304 keeps the 200 receive time', S.liveAt, t0);
  ctx.f({ data: body, changed: true, status: 200 });
  check('a 200 counts from now', Date.now() - S.liveAt < 1000, true);
}
console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
