// test/tools/fwd_vectors.js -- renders firmware/fauxcloud/web/admin-fwd.js (the admin "Online services" sheet) from
// fixtures in the /api/admin/forwarding shape: off; 0 / 1 / 3 targets; a paused target; a running test; every result
// in words; each service's help and location sentences; the key field never prefilled; "Add a service" hidden at 3;
// no interval choice and HTTPS only on a TLS board; the one-station views (keep which station, the confirmation naming
// the other); and the Publishing sheet's two controls greyed while forwarding is on. Each check runs on the source and
// on the terser-mangled bundle (tools/terser_run.js, the shipped form). Usage: node test/tools/fwd_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
globalThis.FC = require('./js_decl').fcStub();  // core.js's text helpers (the sheet's tables register through FC.w)

const root = path.join(__dirname, '..', '..');
const web = path.join(root, 'firmware', 'fauxcloud', 'web');
// the page's one clock-time helper (health.js), ahead of the source under test
const HM = (h => { const { decl } = require('./js_decl'); return ['p2', 'hm', 'dm', 'WH', 'dayTime'].map(n => decl(h, n)).join('\n') + '\n'; })(fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'health.js'), 'utf8'));
const plain = HM + fs.readFileSync(path.join(web, 'admin-fwd.js'), 'utf8');
let mangled = null;
try {
  mangled = execFileSync(process.execPath, [path.join(root, 'tools', 'terser_run.js')], { input: plain }).toString();
} catch (e) {
  if (process.env.CI) throw e;  // in CI the shipped (mangled) form must be tested
  console.log('note: terser not available (npm ci --ignore-scripts in tools/), the mangled pass is skipped');
}

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
// the admin face's helpers (adminface.js), as they render
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const field = (id, label, inner, hint, err) => `<div class="fld"><label class="f" for="${id}">${label}${inner}</label>${err ? `<p class="error" id="${id}-msg">${err}</p>` : hint ? `<p class="hint" id="${id}-msg">${hint}</p>` : ''}</div>`;
const sel = (id, v, opts) => `<span class="input"><select id="${id}">${opts.map(([val, l]) => `<option value="${esc(val)}"${String(val) === String(v) ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select></span>`;
const tog = (id, on, l, s) => `<label class="toggle" for="${id}"><input type="checkbox" role="switch" id="${id}"${on ? ' checked' : ''}><span>${l}${s ? `<small>${s}</small>` : ''}</span></label>`;

function load(src) {
  const SHEETS = {}, API = { stations: [] };
  new Function('esc', 'SHEETS', 'API', 'field', 'sel', 'tog', 'icon', 'api', 'draw', 'SH', 'toast', 'noAnswer', 'refresh', src)(
    esc, SHEETS, API, field, sel, tog, k => `<svg data-i="${k}"></svg>`, () => new Promise(() => {}), () => {}, null, () => {}, () => () => {}, () => {});
  return SHEETS.fwd;
}

// clock times relative to the run's own local day (dayTime: today = "HH:MM", yesterday = "yesterday HH:MM", older = "D Mon HH:MM")
const now = new Date(), at = (dd, h, m) => new Date(now.getFullYear(), now.getMonth(), now.getDate() - dd, h, m), TODAY = at(0, 0, 5).toISOString();
const MON = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'], dmOf = t => t.getDate() + ' ' + MON[t.getMonth()];
const tgt = (o) => Object.assign({ slot: 0, type: 'windy', station: 'ST1', id: 'EXAMPLE1', key: '<set>', min_s: 300, max_s: 420, floor_s: 300, https: true,
  enabled: true, paused: null, state: 'idle', next_s: 120, last: { t: '2026-11-12T10:05:00Z', status: 200, result: 'ok', ms: 343 }, last_ok: TODAY,
  n: { ok: 12, dup: 1, skip: 2, nodata: 1, reject: 0, refused: 0, limited: 0, drop: 1, heap: 0, fail: 0 }, skip_why: null }, o);

const wf = o => Object.assign({ on: false, key: true, verified: false, loc: { lat: 51.5, lon: -0.1 }, state: 'off', pause: 'none', next_t: null,
  last: null, run: { state: 'idle', result: null }, n: {}, tomorrow: null }, o);
function run(label, src) {
  const S = load(src), R = (F, v, st) => S.render(F, v || {}, st || ['ST1']), W = S.W;
  const ok = (name, got, want) => check(label + ': ' + name, got, want);
  // the forecast card -- four states and the key-problem line; its own section after "Add a service"
  {
    const C = w => R({ enabled: false, stations: ['ST1'], targets: [], wufc: wf(w) });
    let f = C({ key: false });
    ok('fc: own section after the add', f.indexOf(`>${W.fc_sec}</h3>`) > f.indexOf('data-a="add"') && f.includes(W.fc), true);
    ok('fc no key: switch disabled, one red line, no Fetch now', /id="fcOn" aria-label="[^"]+" disabled>/.test(f) && f.includes(W.k_missing) && !f.includes('data-a="fcrun"'), true);
    f = C({});
    ok('fc key unverified, off: Fetch now first, Fetch now on the card', f.includes(W.fc_check) && f.includes('data-a="fcrun"') && /id="fcOn"[^>]*disabled>/.test(f), true);
    f = C({ verified: true });
    ok('fc verified, off: the switch usable, no Fetch now on the card', !/id="fcOn"[^>]*disabled>/.test(f) && !f.includes('data-a="fcrun"'), true);
    f = C({ on: true, verified: true, state: 'idle', next_t: '2026-11-16T17:10:00Z' });
    ok('fc on, not fetched yet', /Not fetched yet · the first fetch is at about \d\d:\d\d/.test(f) && /id="fcOn"[^>]*checked>/.test(f), true);
    f = C({ on: true, verified: true, state: 'idle', last: { t: '2026-11-16T16:40:00Z', result: 'ok', status: 200 },
      tomorrow: { day: 'tomorrow', phrase: 'PM Light Rain', hi: 9, lo: 4, pop: 70, t: '2026-11-16T16:40:00Z' } });
    // WU's phrase in the page's words, as the dashboard says it
    ok('fc on, fetched', /<span class="dot"><\/span>Fetched \d\d:\d\d · Tomorrow: Afternoon light rain, 9° \/ 4°/.test(f), true);
    f = C({ on: true, verified: true, state: 'paused', pause: 'key', last: { t: '2026-11-16T15:40:00Z', result: 'refused', status: 401 } });
    // on, nothing shown, a fetch that failed (or a line gone stale): its last result, not "Not fetched yet"
    f = C({ on: true, verified: true, state: 'idle', next_t: '2026-11-16T17:10:00Z', last: { t: '2026-11-16T16:10:00Z', result: 'fail', status: -1 } });
    ok('fc on, last fetch failed', /<span class="dot bad"><\/span>Last fetch (?:yesterday |\d{1,2} \w{3} )?\d\d:\d\d · Server not reachable/.test(f) && !f.includes('Not fetched yet'), true);
    f = C({ on: true, verified: true, state: 'paused', pause: 'memory', last: { t: '2026-11-16T15:40:00Z', result: 'no_memory', status: -1 } });
    ok('fc memory pause: its own sentence', f.includes(W.fp_memory) && !f.includes(W.fp), true);
    f = C({ on: true, verified: true, state: 'paused', pause: 'key', last: { t: '2026-11-16T15:40:00Z', result: 'refused', status: 401 } });
    ok('fc paused: "Paused · last fetch" + key refused', /Paused · last fetch (?:yesterday |\d{1,2} \w{3} )?\d\d:\d\d/.test(f) && f.includes(W.k_refused), true);
    ok('fc info line: the location as sent', f.includes('location 51.5, -0.1 (about 11 km)'), true);
    ok('fc no location', C({ loc: null }).includes(W.fc_noloc), true);
    ok('fc WU note while no WU target sends', C({}).includes(W.fc_wunote) && !R({ enabled: true, stations: ['ST1'], targets: [tgt({ type: 'wu' })], wufc: wf({}) }).includes(W.fc_wunote), true);
    f = R({ enabled: false, stations: ['ST1'], targets: [], wufc: wf({}) }, { fc: { dirty: false } });
    ok('fc edit: masked key, save-first hint hidden, Fetch now', /id="fcKey" type="password" value="" placeholder="unchanged"/.test(f) && /id="fcHint" hidden/.test(f) && f.includes('id="fcrun"'), true);
    f = R({ enabled: false, stations: ['ST1'], targets: [], wufc: wf({}) }, { fc: { dirty: true } });
    ok('fc edit dirty: Fetch now disabled, the hint shown', /id="fcrun" data-a="fcrun" disabled/.test(f) && /id="fcHint">/.test(f), true);
    ok('fc: no card without the firmware object', !R({ enabled: false, stations: ['ST1'], targets: [] }).includes(W.fc), true);
    // the picker: a configured preset is not offered again; WU-compatible and Custom may repeat
    f = R({ enabled: true, stations: ['ST1'], targets: [tgt({ type: 'wu' }), tgt({ slot: 1, type: 'windy' })] }, { pick: true });
    ok('picker: presets in use left out, said once', !f.includes('data-t="wu"') && !f.includes('data-t="windy"') && f.includes('data-t="wu_compat"') && f.includes('Weather Underground and Windy are set up above.'), true);
    ok('header text', W.master_s.startsWith('Sends this station’s readings') && W.on_rule.startsWith('On: one station only'), true);
  }
  // off, nothing configured
  let h = R({ enabled: false, stations: ['ST1'], targets: [] });
  ok('off: master switch unchecked with its sentence', /id="fwdOn">/.test(h) && h.includes(W.master_s), true);
  ok('off: no on-rule line', h.includes(W.on_rule), false);
  ok('0 targets: empty state + add', h.includes(W.none) && h.includes(`data-a="add"`), true);
  // on, one target
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt()] });
  ok('on: switch checked + rule line', /id="fwdOn" checked>/.test(h) && h.includes(W.on_rule), true);
  ok('1 target: name, station, last sent + words', h.includes('Windy') && h.includes('ST1') && /Last sent \d\d:\d\d · Accepted/.test(h), true);
  // the counts only -- no "key set" anywhere; the status leads with the list's dot
  ok('1 target: counts in one quiet line (ok+dup, skip+nodata+heap, drop)', h.includes('>13 sent · 3 skipped · 1 dropped</p>') && !h.includes('key set'), true);
  ok('status with the dot', /<p class="hint fst" role="status"><span class="dot"><\/span>Last sent \d\d:\d\d · Accepted/.test(h), true);
  ok('1 target on: edit + remove, no "Send one now" on the row', ['data-a="edit"', 'data-a="rm"'].every(x => h.includes(x)) && !h.includes('data-a="test"'), true);
  ok('the row switch, checked, labelled', /id="fon0" data-s="0" aria-label="Windy on" checked>/.test(h), true);
  ok('Windy station page after an accepted send', h.includes('href="https://www.windy.com/station/pws-EXAMPLE1"') && h.includes('See your station on Windy') && h.includes('rel="noopener"'), true);
  let ho = R({ enabled: true, stations: ['ST1'], targets: [tgt({ enabled: false, last_ok: null, last: { t: null } })] });
  ok('off and never sent: "Send one now" on the row + the check-first hint, no link', ho.includes('data-a="test"') && ho.includes(W.check_first) && !ho.includes('See your station'), true);
  ok('the row switch unchecked and disabled (never accepted)', /id="fon0" data-s="0" aria-label="Windy on" disabled>/.test(ho), true);
  // `verified` decides -- not the last accepted send (an edit after it clears verified on the board)
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt({ enabled: false, verified: false })] });
  ok('off, accepted before but not verified: disabled switch + the check-first line, no page link', /aria-label="Windy on" disabled>/.test(ho) && ho.includes(W.check_first) && !ho.includes('See your station'), true);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt({ enabled: false, verified: true })] });
  ok('off and verified: the switch can go on, no check-first line, the page link', /aria-label="Windy on">/.test(ho) && !ho.includes(W.check_first) && ho.includes('See your station on Windy'), true);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt({ verified: true })] });
  ok('on and verified: checked, never disabled', /aria-label="Windy on" checked>/.test(ho), true);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt({ type: 'wu', id: 'IABC1' })] });
  ok('WU station page', ho.includes('href="https://www.wunderground.com/dashboard/pws/IABC1"'), true);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt({ type: 'pwsweather' })] });
  ok('no page for PWSweather (another id)', ho.includes('See your station'), false);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt()] }, { ed: Object.assign(tgt(), { keySet: true }) });
  ok('Edit of a running service carries "Send one now", no on/off switch', ho.includes('data-a="test" data-s="0"') && !ho.includes('id="fon"'), true);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt()] }, { ed: Object.assign(tgt(), { keySet: true, dirty: true }) });
  ok('a changed form waits for Save before "Send one now"', /id="fsend" data-a="test" data-s="0" disabled/.test(ho) && ho.includes(W.save_first), true);
  ho = R({ enabled: true, stations: ['ST1'], targets: [tgt(), tgt({ slot: 1, type: 'wu' }), tgt({ slot: 2, type: 'pwsweather' })] });
  ok('three services -> the reason instead of a vanished button', ho.includes(W.full) && !ho.includes('data-a="add"'), true);
  ok('a new service starts off', /enabled: false, path:/.test(fs.readFileSync(require('path').join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'admin-fwd.js'), 'utf8')), true);
  ok('the Windy showcase line', W.h_windy.includes('you can add your name, a website and a logo'), true);
  // three targets: no add
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt(), tgt({ slot: 1, type: 'wu' }), tgt({ slot: 2, type: 'pwsweather' })] });
  ok('3 targets: "Add a service" hidden', h.includes('data-a="add"'), false);
  ok('four equal presets: no tier label', h.includes('community-tested'), false);
  // paused, never sent, key missing
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ paused: 'key', key: '', last_ok: null, last: { t: '2026-11-12T10:05:00Z', status: 401, result: 'refused', ms: 300 } })] });
  ok('paused: says check the key', h.includes(W.paused) && h.includes('dot bad'), true);
  ok('key missing: one red line', h.includes(`<p class="error fst"><span class="dot bad"></span>${W.k_missing}</p>`) && !h.includes(W.k_refused), true);
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ paused: 'key', last: { t: '2026-11-12T10:05:00Z', status: 401, result: 'refused' } })] });
  ok('key refused: one red line, "Paused · last sent"', h.includes(W.k_refused) && /Paused · last sent \d\d:\d\d/.test(h), true);
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ enabled: false })] });
  ok('off: "Off · last sent", the dot off', /<span class="dot off"><\/span>Off · last sent \d\d:\d\d/.test(h), true);
  // the last send with its day when it is not today: a target off for days never reads as sent today
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ last_ok: at(1, 8, 40).toISOString() })] });
  ok('last sent yesterday: the day word', /Last sent yesterday 08:40 · Accepted/.test(h), true);
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ enabled: false, last_ok: at(3, 7, 0).toISOString() })] });
  ok('off, last sent 3 days ago: the date', h.includes(`Off · last sent ${dmOf(at(3, 7, 0))} 07:00`), true);
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ paused: 'key', last_ok: at(1, 23, 50).toISOString() })] });
  ok('paused, last sent yesterday', h.includes('Paused · last sent yesterday 23:50'), true);
  h = R({ enabled: false, stations: ['ST1'], targets: [], wufc: wf({ on: false, verified: true, last: { t: at(2, 6, 15).toISOString(), result: 'ok', status: 200 } }) });
  ok('fc off, last fetch 2 days ago: the date', h.includes(`Off · last fetch ${dmOf(at(2, 6, 15))} 06:15`), true);
  h = R({ enabled: true, tls: true, stations: ['ST1'], targets: [tgt({ paused: 'memory' })] });
  ok('paused for memory: its own sentence', h.includes(W.paused_mem) && !h.includes(W.paused), true);
  // remove: two steps
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt()] }, { rm: 0 });
  ok('remove asks first', h.includes('Remove Windy?') && h.includes('data-a="rmgo"'), true);
  // a running test, then every result in words
  h = R({ enabled: true, stations: ['ST1'], targets: [tgt({ enabled: false })] }, { test: { slot: 0, state: 'running' } });
  ok('running test: "Sending…" and the button disabled', h.includes(W.running) && /data-a="test" data-s="0" disabled/.test(h), true);
  const word = (r, x, extra) => R({ enabled: true, stations: ['ST1'], targets: [tgt(x)] }, { test: Object.assign({ slot: 0, state: 'done', result: r, status: 200, ms: 50 }, extra) });
  ok('result ok', word('ok').includes('Accepted (200, 50 ms)'), true);
  ok('result dup', word('dup').includes('Accepted'), true);
  ok('result refused', word('refused', {}, { status: 401 }).includes('Key refused (401'), true);
  ok('result too soon (Windy, 5 min)', word('too_soon', {}, { status: -1 }).includes('Too soon — Windy takes one reading per 5 minutes'), true);
  ok('result limited (WU, 1 min)', word('limited', { type: 'wu' }, { status: 429 }).includes('Too soon — Weather Underground takes one reading per 1 minutes'), true);
  ok('result fail', word('fail', {}, { status: -1 }).includes('Server not reachable'), true);
  ok('result tls, preset: never plain HTTP', word('tls_untrusted', {}, { status: -1 }).includes('Certificate not trusted — update fauxcloud'), true);
  ok('result tls, custom: plain HTTP', word('tls_untrusted', { type: 'wu_compat' }, { status: -1 }).includes('Certificate not trusted — use plain HTTP'), true);
  ok('result off', word('off', {}, { status: -1 }).includes('Not sent: forwarding is off'), true);
  ok('result nodata', word('nodata', {}, { status: -1 }).includes('Not sent: no fresh reading'), true);
  ok('result busy', word('busy', {}, { status: -1 }).includes('Not sent: fauxcloud is busy, try in a minute'), true);
  ok('result reject', word('reject', {}, { status: 500 }).includes('Unexpected answer 500'), true);
  ok('result no value', word('reject', {}, { status: -1, why: 'no_value' }).includes(W.r_novalue), true);
  ok('result no memory', word('no_memory', {}, { status: -1 }).includes(W.r_nomem), true);
  ok('result no answer', word('noanswer', {}, { status: -1 }).includes(W.r_noanswer), true);
  ok('custom: the masked body excerpt', word('reject', { type: 'template' }, { status: 200, body: 'stored, thanks ***' }).includes('<code>stored, thanks ***</code>'), true);
  // the add list and the forms
  h = R({ enabled: true, stations: ['ST1'], targets: [] }, { pick: true });
  ok('add list: six kinds, no tier label', ['wu', 'windy', 'wowbe', 'pwsweather', 'wu_compat', 'template'].every(t => h.includes(`data-t="${t}"`)) && !h.includes('community-tested'), true);
  const form = (type, extra, tls) => R({ enabled: true, tls: !!tls, stations: ['ST1', 'ST2'], targets: [] }, { ed: Object.assign({ type, station: 'ST1', min_s: 300, https: false, enabled: true, id: '', path: '', host: '', url: '', success: '' }, extra) }, ['ST1', 'ST2']);
  const name = { wu: 'Weather Underground', windy: 'Windy', wowbe: 'WOW-BE', pwsweather: 'PWSweather' };
  for (const t of Object.keys(name)) {
    h = form(t);
    ok(t + ': the common sentence', h.includes(`Every 5 minutes fauxcloud sends this station’s outdoor readings to ${name[t]}. ${name[t]} shows them on a public page and map and may reuse them (its terms).`), true);
    ok(t + ': its help', h.includes(esc(W['h_' + t])), true);
    ok(t + ': its location sentence', h.includes(esc(W[W['loc_' + t] ? 'loc_' + t : 'loc'].replace(/\{svc\}/g, name[t]))), true);
  }
  ok('WOW-BE: to 6 decimals', form('wowbe').includes('as typed, to 6 decimals'), true);
  ok('PWSweather: Neighborhood', form('pwsweather').includes('Choose Precision: Neighborhood.'), true);
  ok('Windy: its request log', form('windy').includes('Show recent requests'), true);
  ok('WU: elevation is public', form('wu').includes('it is public'), true);
  ok('no interval choice (a fixed 5-minute interval)', ['wu', 'windy', 'wowbe', 'pwsweather', 'wu_compat', 'template'].some(t => form(t).includes('id="fevery"') || form(t, {}, true).includes('id="fevery"')), false);
  ok('Windy at 310 s still reads "Every 5 minutes"', form('windy', { min_s: 310 }).includes('Every 5 minutes fauxcloud sends'), true);
  h = form('windy', { slot: 0, id: 'EXAMPLE1', keySet: true });
  ok('edit: the key never prefilled, "unchanged" when set', /id="fkey" type="password" value="" placeholder="unchanged"/.test(h) && !/key set/.test(h), true);
  ok('edit: no key text anywhere', h.includes('examplekey'), false);
  ok('new: no placeholder when no key', /id="fkey" type="password" value=""( |>)/.test(form('wu')) && !/id="fkey"[^>]*placeholder/.test(form('wu')), true);
  ok('station choice with two stations', form('wu').includes('id="fst"'), true);
  ok('TLS board: https toggle, off for a new target, + its hint', /id="fhttps">/.test(form('wu', {}, true)) && form('wu', {}, true).includes(W.f_https_s) && !form('wu', {}, true).includes(W.no_tls), true);
  ok('TLS board: the hint says what HTTPS costs here', W.f_https_s.includes('17 KB') && W.f_https_s.includes('plain HTTP'), true);
  ok('TLS board: a stored HTTPS target shows it on', form('wu', { https: true }, true).includes('id="fhttps" checked'), true);
  ok('the HTTP-only build: no https toggle, the no-encryption sentence', ['wu', 'windy', 'wowbe', 'pwsweather', 'wu_compat', 'template'].every(t => !form(t).includes('id="fhttps"') && form(t).includes(W.no_tls)), true);
  ok('this board: the template example is http://', form('template').includes('placeholder="http://example.org/') && form('template', {}, true).includes('placeholder="https://example.org/'), true);
  h = form('template', {}, true);
  ok('custom: url + success, no https toggle, the variables line', h.includes('id="furl"') && h.includes('id="fsucc"') && !h.includes('id="fhttps"') && h.includes(W.h_template), true);
  ok('custom: no location sentence', h.includes('publishes the location'), false);
  h = form('wu_compat');
  ok('WU-compatible: host, path, success', ['id="fhost"', 'id="fpath"', 'id="fsucc"'].every(x => h.includes(x)), true);
  // the keep views (forwarding with one station only)
  h = R({ enabled: false, stations: ['ST1', 'ST2'], targets: [] }, { keep: true }, ['ST1', 'ST2']);
  ok('keep: asks which, both offered', h.includes(W.keep) && h.includes('data-id="ST1"') && h.includes('data-id="ST2"') && h.includes(W.keep_s), true);
  h = R({ enabled: false, stations: ['ST1', 'ST2'], targets: [] }, { keep: true, sure: 'ST1' }, ['ST1', 'ST2']);
  ok('keep: the confirmation says fauxcloud restarts', h.includes('restarts to free the memory'), true);
  ok('keep: the confirmation names the other station', h.includes('Forget ST2 and turn forwarding on?') && h.includes('Forget ST2, turn on') && h.includes('data-id="ST1"'), true);
  h = R({ enabled: false, stations: ['ST1', 'ST2'], targets: [] }, { keep: true, sure: 'ST1', lanOn: true }, ['ST1', 'ST2']);
  ok('keep + LAN on: the confirmation says the LAN console stops', h.includes(W.lan_s), true);
  h = R({ enabled: false, stations: ['ST1'], targets: [] }, { lan: true });
  ok('one station + LAN on: asks before turning LAN uploads off', h.includes(W.lan_q) && h.includes(W.lan_s) && h.includes('data-a="enable"'), true);
  ok('loading / error', [R(null), R({ err: 1 }).includes('error')], ['<p class="hint loading">Loading…</p>', true]);
  // no unfilled placeholder anywhere
  const all = [R({ enabled: true, stations: ['ST1'], targets: [tgt()] }), form('wu'), form('template'), word('limited')].join('');
  ok('no unfilled placeholder', /\{(n|svc|t|ok|skip|drop|status|name)\}/.test(all), false);
}
// the handlers themselves -- FC.get drops a 400's body, so the sheet checks before it POSTs.
// A synchronous thenable api records each call; toast and noAnswer record what the user would read.
function handlers(label, src) {
  const ok = (name, got, want) => check(label + ': ' + name, got, want);
  const calls = [], toasts = [], els = {}, SHEETS = {}, API = { stations: [{ id: 'ST1' }] }, timers = [];
  let F = { enabled: false, stations: ['ST1'], targets: [], wufc: wf({}) }, fail = false;
  // bad: true = a 400, or the error's message ('HTTP 409', 'HTTP 503', 'Failed to fetch')
  const sync = (v, bad) => ({ then: (y, n) => { if (bad) { if (n) n(new Error(bad === true ? 'HTTP 400' : bad)); } else if (y) y(v); return sync(v, bad); } });
  const api = (m, u, body) => { calls.push([m, u, body]); return m === 'GET' ? sync(F) : sync(/\/run$/.test(u) ? { ok: true, queued: true, eta_s: 64 } : {}, fail); };
  const noAnswer = b => () => { if (b) b.disabled = false; toasts.push('noanswer'); };
  global.document = { getElementById: id => els[id] };
  // the sheet open (SH), its timers recorded (the poll runs when the test says)
  new Function('esc', 'SHEETS', 'API', 'field', 'sel', 'tog', 'icon', 'api', 'draw', 'SH', 'toast', 'noAnswer', 'refresh', 'setTimeout', 'clearTimeout', src)(
    esc, SHEETS, API, field, sel, tog, k => k, api, () => {}, { key: 'fwd' }, t => toasts.push(t), noAnswer, () => {},
    (f, ms) => timers.push([f, ms]), () => {});
  const sh = SHEETS.fwd(), W = SHEETS.fwd.W, posts = () => calls.filter(c => c[0] === 'POST');
  const save = key => { els.fcKey = { type: 'password', value: key }; const b = { dataset: { a: 'fcsave' }, disabled: false }; sh.click(b); return b; };
  // a pasted Station Key (7 characters), 31 hex, a non-hex 32: the whole-sentence hint, no POST, the button usable
  for (const k of ['IEXAMPL1', 'a'.repeat(31), 'g'.repeat(32)]) {
    toasts.length = 0; calls.length = 0;
    const b = save(k);
    ok('fcsave malformed key ' + k.length + ': the format sentence, no POST', [toasts, posts().length, b.disabled], [[W.e_key_format], 0, false]);
  }
  ok('the key format sentence', W.e_key_format, 'Key format: 32 hex characters (the API key, not the Station Key)');
  toasts.length = 0; calls.length = 0;
  save('0123456789abcdefABCDEF0123456789');
  ok('fcsave a well-formed key: POSTed, no toast', [posts(), toasts], [[['POST', '/api/admin/wufc', { key: '0123456789abcdefABCDEF0123456789' }]], []]);  // gitleaks:allow (a fake 32-hex key fixture, not a secret)
  toasts.length = 0; calls.length = 0;
  save('');
  ok('fcsave an empty key (unchanged): POSTed without one', posts(), [['POST', '/api/admin/wufc', {}]]);
  toasts.length = 0; calls.length = 0; fail = true;
  let b = save('0123456789abcdef0123456789abcdef');
  ok('fcsave refused or lost: noAnswer, the button usable again', [toasts, b.disabled], [['noanswer'], false]);
  fail = false;
  // the switch turned on with an unverified key (unreachable in normal use: the switch is disabled then)
  toasts.length = 0; calls.length = 0;
  let t = { id: 'fcOn', checked: true, disabled: false, dataset: {} };
  sh.change(t);
  ok('fcOn unverified: back off, "Fetch now first", no POST', [t.checked, t.disabled, toasts, posts().length], [false, false, [W.fc_check], 0]);
  F = Object.assign({}, F, { wufc: wf({ verified: true }) });
  // the sheet reads F again after a POST (load); turning the switch off needs no check and brings the verified state in
  sh.change({ id: 'fcOn', checked: false, disabled: false, dataset: {} });
  calls.length = 0; toasts.length = 0;
  t = { id: 'fcOn', checked: true, disabled: false, dataset: {} };
  sh.change(t);
  ok('fcOn verified: POSTed on, no toast', [posts(), toasts], [[['POST', '/api/admin/wufc', { on: true }]], []]);
  toasts.length = 0; calls.length = 0; fail = true;
  t = { id: 'fcOn', checked: true, disabled: false, dataset: {} };
  sh.change(t);
  ok('fcOn refused or lost: back off, noAnswer', [t.checked, t.disabled, toasts], [false, false, ['noanswer']]);
  fail = false;
  // Fetch now queued (202 + eta_s), the poll at eta + 5 s (at most 30), then "Tried {t}" for a failure, "Fetched {t}"
  // for a success; a refusal (409 off / 503 low_memory) alone, never "Tried"; anything else: noAnswer
  const run = () => { const b = { dataset: { a: 'fcrun' }, disabled: false }; sh.click(b); return b; };
  F = Object.assign({}, F, { wufc: wf({ run: { state: 'waiting', result: null, eta_s: 64 } }) });
  timers.length = 0; toasts.length = 0; calls.length = 0;
  run();
  let h = sh.body();
  ok('fcrun queued: "Queued · runs in about 64 s", the next poll in 30 s', [h.includes('Queued · runs in about 64 s'), posts(), timers.map(x => x[1])],
    [true, [['POST', '/api/admin/wufc/run', {}]], [30000]]);
  F = Object.assign({}, F, { wufc: wf({ run: { state: 'running', result: null } }) });
  timers.shift()[0]();
  ok('fcrun running: "Fetching…", the next poll in 5 s', [sh.body().includes(W.fc_running), timers.map(x => x[1])], [true, [5000]]);
  F = Object.assign({}, F, { wufc: wf({ run: { state: 'done', result: 'fail', t: '2026-11-16T21:15:00Z' } }) });
  timers.shift()[0]();
  h = sh.body();
  ok('fcrun failed: "Tried {t} · Server not reachable", never "Fetched"', [/Tried \d\d:\d\d · Server not reachable/.test(h), /Fetched \d\d:\d\d · Server/.test(h), timers.length], [true, false, 0]);
  F = Object.assign({}, F, { wufc: wf({ run: { state: 'done', result: 'busy', t: '2026-11-16T21:25:00Z' } }) });
  run();
  ok('fcrun given up after 10 min: "Tried {t} · fauxcloud stayed busy"', /Tried \d\d:\d\d · fauxcloud stayed busy, try again later/.test(sh.body()), true);
  F = Object.assign({}, F, { wufc: wf({ verified: true, run: { state: 'done', result: 'ok', t: '2026-11-16T21:16:00Z' } }) });
  run();
  ok('fcrun ok: "Fetched {t} · …", no "Tried"', [/Fetched \d\d:\d\d · /.test(sh.body()), sh.body().includes('Tried')], [true, false]);
  timers.length = 0;
  for (const [msg, want] of [['HTTP 503', W.fr_low_memory], ['HTTP 409', W.fr_off]]) {
    fail = msg; toasts.length = 0;
    const b = run();
    h = sh.body();
    ok('fcrun refused (' + msg + '): the reason alone, no poll', [h.includes(want), h.includes('Tried'), toasts, b.disabled, timers.length], [true, false, [], false, 0]);
  }
  ok('the low-memory words', W.fr_low_memory, 'fauxcloud is low on memory, try later');
  fail = 'Failed to fetch'; toasts.length = 0;
  const b5 = run();
  ok('fcrun lost: noAnswer, the button usable', [toasts, b5.disabled], [['noanswer'], false]);
  fail = false;
  // the sheet opened while a Fetch now waits / runs / is done shows it (its first load reads wufc.run)
  timers.length = 0;
  F = Object.assign({}, F, { wufc: wf({ run: { state: 'waiting', result: null, eta_s: 12 } }) });
  let sh2 = SHEETS.fwd();
  ok('reopened while queued: "Queued · runs in about 12 s", polling (17 s)', [sh2.body().includes('Queued · runs in about 12 s'), timers.map(x => x[1])], [true, [17000]]);
  timers.length = 0;
  F = Object.assign({}, F, { wufc: wf({ run: { state: 'done', result: 'refused', status: 401, t: '2026-11-16T21:30:00Z' } }) });
  sh2 = SHEETS.fwd();
  ok('reopened after a failed run: "Tried {t} · …", no poll', [/Tried \d\d:\d\d · /.test(sh2.body()), timers.length], [true, 0]);
  F = Object.assign({}, F, { wufc: wf({}) });
  sh2 = SHEETS.fwd();
  ok('opened with no run: no result line, no poll', [/Queued|Tried|Fetched \d/.test(sh2.body()), timers.length], [false, 0]);
  // an old sheet's Fetch-now chain that ends in `done` after a reopen draws nothing (every path of the chain under on());
  // the new sheet still draws its own state
  {
    const S2 = {}, tm = [];
    let draws = 0, G = Object.assign({}, F, { wufc: wf({ run: { state: 'waiting', result: null, eta_s: 3 } }) });
    const api2 = m => m === 'GET' ? sync(G) : sync({});
    new Function('esc', 'SHEETS', 'API', 'field', 'sel', 'tog', 'icon', 'api', 'draw', 'SH', 'toast', 'noAnswer', 'refresh', 'setTimeout', 'clearTimeout', src)(
      esc, S2, API, field, sel, tog, k => k, api2, () => { draws++; }, { key: 'fwd' }, () => {}, noAnswer, () => {},
      (f, ms) => tm.push([f, ms]), () => {});
    S2.fwd();
    const old = tm.shift();
    ok('old chain: the first sheet polls a queued run', [!!old, old && old[1]], [true, 8000]);
    S2.fwd();  // reopened: the old sheet's timer is still in flight
    tm.length = 0;
    G = Object.assign({}, G, { wufc: wf({ run: { state: 'done', result: 'ok', t: '2026-11-16T21:40:00Z' } }) });
    draws = 0;
    old[0]();
    ok('old chain ends in done after a reopen: no draw from the old sheet, no new poll', [draws, tm.length], [0, 0]);
    S2.fwd();
    ok('the open sheet draws its own done state', draws > 0, true);
  }
  delete global.document;
}
run('source', plain);
if (mangled) run('mangled', mangled);
handlers('source', plain);
if (mangled) handlers('mangled', mangled);

// the Publishing sheet greys "Stations kept" and the LAN switch while forwarding is on (admin-station.js)
{
  const src = fs.readFileSync(path.join(web, 'admin-station.js'), 'utf8');
  const SHEETS = {}, API = { bridge: { auto_publish_new_stations: true, mqtt_interval_last_seen_s: 300, max_stations: 1, lan_uploads: false, forwarding: true }, stations: [{ id: 'ST1' }] };
  // every other global of the admin face: a stub that is callable, has every property and prints as ''
  const any = new Proxy(function () { return ''; }, { get: (t, k) => (k === Symbol.toPrimitive ? () => '' : any), apply: () => '' });
  const given = new Set(['stub', 'esc', 'SHEETS', 'API', 'field', 'sel', 'tog', 'clone', 'api']);  // the real ones
  const stub = new Proxy({}, { has: (t, k) => typeof k === 'string' && !given.has(k) && !(k in globalThis), get: (t, k) => (k === Symbol.unscopables ? undefined : any) });
  // the file defines more sheets; only SHEETS.publish is rendered (its helpers are the admin face's)
  new Function('stub', 'esc', 'SHEETS', 'API', 'field', 'sel', 'tog', 'clone', 'api', `with (stub) { ${src} }`)(stub, esc, SHEETS, API, field, sel, tog,
    o => JSON.parse(JSON.stringify(o)), () => new Promise(() => {}));
  let h = SHEETS.publish().body();
  const why = 'Off while forwarding is on: not enough memory on this board.';
  check('stations kept greyed with the reason', /<select disabled id="maxst">/.test(h) && h.includes(`id="maxst-msg">${why}`), true);
  check('LAN switch greyed with the reason', /<input disabled type="checkbox" role="switch" id="lanup">/.test(h) && h.includes(`<small>${why}</small>`), true);
  API.bridge.forwarding = false;
  h = SHEETS.publish().body();
  check('both usable while forwarding is off', /disabled/.test(h), false);
}

console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
