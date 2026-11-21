// test/tools/capture_vectors.js -- runs firmware/fauxcloud/web/admin-capture.js (the admin "Listen to a new console"
// sheet) against fixtures in the GET /api/admin/capture shape: records merged by id (a repeat is a total, never a
// second line), the cursor, the gap line, the poll chain (at once again while `more`, every 3 s otherwise, also in a
// hidden tab), the history kept across a close and a reopen, the report's text byte for byte, the status words, the
// empty states, the device rows, the consequence lines by what is switched on, the refusals. Each check runs on the
// source and on the terser-mangled bundle. Usage: node test/tools/capture_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const root = path.join(__dirname, '..', '..');
const plain = fs.readFileSync(path.join(root, 'firmware', 'fauxcloud', 'web', 'admin-capture.js'), 'utf8');
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
  else { fail++; console.log('FAIL ' + name + ':\n  got  ' + JSON.stringify(got) + '\n  want ' + JSON.stringify(want)); }
}
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const FC = require('./js_decl').fcStub({});
const WF = { copy: 'Copy', hide: 'Hide', hides: 'Hides in 60 s', reveal_pw: 'Admin password', cancel: 'Cancel', show_pw: 'Show password',
  pw_hidden: 'Password hidden', password: 'Password', show_why: 'Asks for your admin password first.', no_answer: 'No answer from fauxcloud', no_answer_short: 'No answer.' };

// One load of the sheet with a scripted API: `answers` are the GET bodies in order (a function may stand for one),
// POSTs answer `post` (or reject with an Error like core.js's), timers are recorded, never run by themselves.
function load(src, o = {}) {
  const env = { gets: [], posts: [], timers: [], draws: 0, answers: (o.answers || []).slice(), post: o.post || (() => Promise.resolve({ ok: true })) };
  const SHEETS = {}, API = Object.assign({ settings: {}, bridge: {}, status: { fw_version: '1.1.0-dev.2' }, ap_ssid: 'fauxcloud-1A2B' }, o.API || {});
  const api = (method, url) => {
    if (method === 'GET') {
      env.gets.push(url);
      const a = env.answers.shift();
      return a === undefined ? new Promise(() => {}) : typeof a === 'function' ? a() : Promise.resolve(a);
    }
    env.posts.push(url);
    return env.post(url);
  };
  const SH = { key: 'capture' };
  const timers = { setTimeout: (f, ms) => { env.timers.push(ms); return env.timers.length; }, clearTimeout: () => {} };
  const field = (id, l, inner) => `<label for="${id}">${l}${inner}</label>`, text = id => `<input id="${id}">`;
  new Function('esc', 'FC', 'SHEETS', 'API', 'icon', 'api', 'draw', 'SH', 'toast', 'noAnswer', '$', 'field', 'text', 'WF', 'A',
    'withProof', 'failTry', 'copyText', 'setTimeout', 'clearTimeout', 'document', 'URL', 'Blob', src)(
    esc, FC, SHEETS, API, () => '<svg></svg>', api, () => { env.draws++; }, SH, () => {}, () => () => {}, o.$ || (() => null), field, text, WF,
    { tries: 5, lock: 0 }, () => Promise.resolve({ ok: false }), () => 'wrong', () => {}, timers.setTimeout, timers.clearTimeout,
    undefined, undefined, undefined);
  env.S = SHEETS.capture;
  env.T = SHEETS.capture.T;
  env.API = API;
  return env;
}
const flush = () => new Promise(r => setImmediate(r));

const ev = (id, kind, text, o = {}) => Object.assign({ id, seq: id, group: id, kind, client: 23, n: 1, t_first_s: 0, t_last_s: 0, text }, o);
const J = o => Object.assign({ state: 'on', ended: '', blocked: '', elapsed_s: 0, idle_left_s: 3600, started_utc: null, clock: true,
  ntp: { asked: 0, answered: 0 }, uploads_skipped: 0, vendor: null, seq: 0, lost: 0, more: false, clients: [], clients_over: 0,
  dns: { other: 0, names: [] }, events: [] }, o);

const GOLD = J({ elapsed_s: 1350, started_utc: 1795000000, ntp: { asked: 14, answered: 14 }, vendor: 'random', seq: 9, lost: 3,
  clients: [{ ip: '192.168.77.23', vendor: 'random', joined_s: 3, left_s: null, n: 41, active: true },
    { ip: '192.168.77.24', vendor: 'random', joined_s: 1, left_s: 250, n: 3, active: false }],
  dns: { other: 0, names: [{ name: 'api.example.net', n: 15, types: 'A', client: 23 }, { name: 'time.example.net', n: 3, types: 'A', client: 23 }] },
  events: [
    ev(1, 'tcp', ':443', { n: 15, t_first_s: 12 }),
    ev(2, 'tls', ':443 verdict=ecdhe-rsa-gcm v=1.2 sni=api.example.net suites=c02f groups=p256 sigalg=yes mfl=- alpn=-', { n: 15, t_first_s: 12 }),
    ev(3, 'http', 'GET /generate_204', { client: 24, n: 2, t_first_s: 5 }),
    ev(4, 'hdr', 'host=connectivitycheck.example.net ua=Dalvik/2.1 closed-before-reply', { group: 3, client: 24, n: 2, t_first_s: 5 }),
    ev(5, 'http', 'POST /api/v1/up...', { t_first_s: 30 }),
    ev(6, 'http', '...?a=1', { group: 5, t_first_s: 30 }),
    ev(7, 'body', 'json 20B: {"token":***}', { group: 5, t_first_s: 30 }),
    ev(8, 'mqtt', ':1883 MQTT 3.1.1 flags=user,pass keepalive=60 clientid=12B', { t_first_s: 31 }),
    ev(9, 'raw', ':8080 20B: 80 81', { t_first_s: 32 })] });
const GOLD_TEXT = [
  'fauxcloud 1.1.0-dev.2 listen 2026-11-18 11:06Z 22:30 clock:synced ntp:14/14 ap:192.168.77.0/24 clients:2',
  'CLIENTS: .23 vendor:random joined 00:03 x41 | .24 vendor:random joined 00:01 left 04:10 x3',
  'DNS: api.example.net x15 | time.example.net x3',
  'TCP: .23 :443 x15 (first 00:12)',
  'TLS: .23 :443 verdict=ecdhe-rsa-gcm v=1.2 sni=api.example.net suites=c02f groups=p256 sigalg=yes mfl=- alpn=- x15 (first 00:12)',
  'HTTP: .24 GET /generate_204 host=connectivitycheck.example.net ua=Dalvik/2.1 closed-before-reply x2 (first 00:05)',
  'HTTP: .23 POST /api/v1/up?a=1 x1 (first 00:30)',
  '  body: json 20B: {"token":***}',
  'MQTT: .23 :1883 MQTT 3.1.1 flags=user,pass keepalive=60 clientid=12B x1 (first 00:31)',
  'RAW: .23 :8080 20B: 80 81 x1 (first 00:32)',
  '... 3 entries not kept'].join('\n');

async function run(label, src) {
  // the report, byte for byte; ASCII only
  {
    const e = load(src);
    e.T.merge(GOLD);
    const r = e.T.report('1.1.0-dev.2');
    check(label + 'report golden', r, GOLD_TEXT);
    check(label + 'report ascii', /^[\x20-\x7e\n]*$/.test(r), true);
    e.T.reset();
    e.T.merge(J({ state: 'off', ended: 'stop' }));
    check(label + 'report empty', e.T.report('x'), 'fauxcloud x listen time:unknown 00:00 clock:synced ntp:0/0 ap:192.168.77.0/24 clients:0\nCLIENTS: none');
  }
  // merge by id: a repeat is a total; the cursor; a repeated poll appends nothing
  {
    const e = load(src);
    e.T.merge(J({ seq: 3, events: [ev(1, 'tcp', ':443'), ev(2, 'tcp', ':8080'), ev(3, 'raw', ':8080 1B: 00')] }));
    check(label + 'cursor', e.T.H.since, 3);
    e.T.merge(J({ seq: 5, events: [ev(1, 'tcp', ':443', { seq: 5, n: 3 })] }));
    check(label + 'merged by id', [e.T.H.recs.size, e.T.H.recs.get(1).n, e.T.H.since], [3, 3, 5]);
    e.T.merge(J({ seq: 5, events: [ev(1, 'tcp', ':443', { seq: 5, n: 3 })] }));
    check(label + 'same poll again', [e.T.H.recs.size, e.T.H.recs.get(1).n], [3, 3]);
    check(label + 'x3 in the report', e.T.report('v').split('\n').find(l => l.startsWith('TCP')), 'TCP: .23 :443 x3 (first 00:00) | .23 :8080 x1 (first 00:00)');
    // more: the cursor is the last record returned, not the board's newest
    e.T.merge(J({ seq: 20, more: true, events: [ev(6, 'raw', ':80 1B: 01', { seq: 6 })] }));
    check(label + 'more keeps the cursor', e.T.H.since, 6);
    // lost: the gap grows, never shrinks
    e.T.merge(J({ seq: 20, lost: 2 }));
    e.T.merge(J({ seq: 20, lost: 1 }));
    check(label + 'lost total', e.T.H.lost, 2);
    check(label + 'gap line', e.T.liveHtml().includes('… 2 entries not kept'), true);
  }
  // a second capture in one page: a "starting" answer still carries the last run's counters; they never become the
  // cursor or the gap, so the new run's records (seq from 1) arrive
  {
    const e = load(src);
    e.T.merge(J({ state: 'off', ended: 'stop', seq: 120, lost: 7 }));
    e.T.reset();
    e.T.merge(J({ state: 'starting', seq: 120, lost: 7 }));
    check(label + 'starting: no cursor, no gap', [e.T.H.since, e.T.H.lost], [0, 0]);
    e.T.merge(J({ state: 'on', seq: 3, events: [ev(1, 'tcp', ':443'), ev(2, 'tcp', ':8080'), ev(3, 'raw', ':8080 1B: 00')] }));
    check(label + 'second run: its records', [e.T.H.recs.size, e.T.H.since, e.T.H.lost], [3, 3, 0]);
    check(label + 'second run: no stale gap', e.T.report('v').includes('not kept'), false);
  }
  // a request on a peek port: tcp heads the group, the request line follows; it reaches the report with its parts
  {
    const e = load(src);
    e.T.merge(J({ state: 'on', seq: 4, events: [ev(1, 'tcp', ':8080'), ev(2, 'http', ':8080 POST /data/report/?PASSKEY=***&t=1', { group: 1 }),
      ev(3, 'hdr', 'host=192.168.77.1:8080 content-length=44', { group: 1 }), ev(4, 'body', 'form 44B: PASSKEY=***&name=Z??rich', { group: 1 })] }));
    const r = e.T.report('v').split('\n');
    check(label + 'peek http line', r.filter(l => l.startsWith('HTTP: ')),
      ['HTTP: .23 :8080 POST /data/report/?PASSKEY=***&t=1 host=192.168.77.1:8080 content-length=44 x1 (first 00:00)']);
    check(label + 'peek http body', r.includes('  body: form 44B: PASSKEY=***&name=Z??rich'), true);
    check(label + 'peek tcp kept', r.includes('TCP: .23 :8080 x1 (first 00:00)'), true);
  }
  // a device's many HTTPS connections: one TCP item per device and text, the counts summed, the earliest first
  {
    const e = load(src);
    e.T.merge(J({ state: 'on', seq: 5, events: [ev(1, 'tcp', ':443', { n: 2, t_first_s: 19 }), ev(2, 'tls', ':443 verdict=chacha', { group: 1 }),
      ev(3, 'tcp', ':443', { n: 3, t_first_s: 12 }), ev(4, 'tls', ':443 verdict=tls13-only', { group: 3 }),
      ev(5, 'tcp', ':443 busy, closed', { client: 0, n: 19, t_first_s: 20 })] }));
    check(label + 'tcp summed', e.T.report('v').split('\n').find(l => l.startsWith('TCP: ')),
      'TCP: .23 :443 x5 (first 00:12) | .? :443 busy, closed x19 (first 00:20)');
  }
  // the chain: at once again while more, every 3 s otherwise; nothing after "off"; one GET per open
  {
    const e = load(src, { answers: [J({ state: 'on', seq: 9, more: true, events: [ev(1, 'tcp', ':443')] }), J({ state: 'on', seq: 9 }), J({ state: 'off', ended: 'stop', seq: 9 })] });
    const sh = e.S();
    await flush(); await flush(); await flush();
    check(label + 'chain gets', e.gets, ['/api/admin/capture?since=0', '/api/admin/capture?since=1']);
    check(label + 'chain timer', e.timers, [3000]);
    // the history stays across a close and a reopen; the reopen asks nothing while the chain runs
    const n = e.gets.length;
    e.S();
    check(label + 'reopen keeps history', [e.T.H.recs.size, e.gets.length], [1, n]);
    check(label + 'running view', /id="capStop"/.test(sh.body()), true);
  }
  // the start view: the consequences by what is on, the AP name, the clock warning, the refusals
  {
    const e = load(src, { API: { settings: { mqtt_host: 'broker.lan' }, bridge: { forwarding: true }, metar: { available: true, on: true } } });
    e.T.merge(J({ state: 'off', clock: false }));
    const b = e.S().body();
    check(label + 'all consequences', [b.includes(e.T.W.c_ha), b.includes(e.T.W.c_fwd), b.includes(e.T.W.c_metar), b.includes(e.T.W.c_save)], [true, true, true, true]);
    check(label + 'ap name and password, labelled', [b.includes('id="capSsid">fauxcloud-1A2B</div>'), b.includes(e.T.W.ap_name),
      b.includes(e.T.W.ap_pw), b.includes('id="capPwAsk"')], [true, true, true, true]);
    check(label + 'no time', b.includes(e.T.W.notime), true);
    check(label + 'start before the folded wifi', [b.indexOf('id="capStart"') < b.indexOf('id="capAp"'), /<details class="adv" id="capAp"><summary>/.test(b)], [true, true]);
    const e2 = load(src);
    e2.T.merge(J({ state: 'off' }));
    const b2 = e2.S().body();
    check(label + 'no broker, no forwarding', [b2.includes(e2.T.W.c_ha), b2.includes(e2.T.W.c_fwd), b2.includes(e2.T.W.c_metar), b2.includes(e2.T.W.c_save)], [false, false, false, true]);
    e2.T.merge(J({ state: 'off', blocked: 'ota' }));
    check(label + 'blocked by an update', [e2.S().body().includes(e2.T.W.e_ota), /id="capStart" disabled/.test(e2.S().body())], [true, true]);
    e2.T.merge(J({ state: 'off', blocked: 'restart' }));
    check(label + 'blocked by a restart', e2.S().body().includes(e2.T.W.e_restart), true);
    e2.T.merge(J({ state: 'off', ended: 'heap' }));
    check(label + 'heap refusal', e2.S().body().includes(e2.T.W.e_heap), true);
  }
  // "Listen again": back to the start view, and its Start comes alive once the board answered (bench 2026-11-21: it stayed grey)
  {
    const e = load(src, { answers: [J({ state: 'off', ended: 'stop', seq: 3 })] });
    e.T.merge(J({ state: 'off', ended: 'stop', seq: 3, events: [ev(1, 'tcp', ':443')] }));
    const sh = e.S();
    sh.click({ id: 'capAgain' });
    await flush(); await flush();
    check(label + 'again asks the board', e.gets.slice(-1)[0], '/api/admin/capture?since=0');
    check(label + 'again: start enabled', /id="capStart" >/.test(sh.body()), true);
  }
  // a 409 asks the board what blocked it; no answer says so
  {
    const err = new Error('HTTP 409');
    const e = load(src, { post: () => Promise.reject(err), answers: [J({ state: 'off' }), J({ state: 'on' })] });
    e.T.merge(J({ state: 'off' }));
    const sh = e.S();
    sh.click({ id: 'capStart', disabled: false });
    await flush(); await flush();
    check(label + '409 polls the state', e.gets.slice(-1)[0], '/api/admin/capture?since=0');
    const e3 = load(src, { post: () => Promise.reject(new Error('network')) });
    e3.T.merge(J({ state: 'off' }));
    const s3 = e3.S();
    s3.click({ id: 'capStart', disabled: false });
    await flush();
    check(label + 'no answer', e3.T.H.err, WF.no_answer);
  }
  // a live poll (no full redraw) enables Copy and Download once records exist
  {
    const el = { '#capRep': { innerHTML: '' }, '#capCopy': { disabled: true }, '#capDl': { disabled: true }, '#capDevN': { innerHTML: '' } };
    const e = load(src, { $: q => el[q] || null });
    e.T.merge(J({ state: 'on', seq: 1, events: [ev(1, 'tcp', ':443')], clients: [{ ip: '192.168.77.23', vendor: 'a4:cf:12', joined_s: 6, left_s: null, n: 1, active: true }] }));
    e.T.live();
    check(label + 'live enables copy', [el['#capCopy'].disabled, el['#capDl'].disabled, el['#capRep'].innerHTML.includes(':443'), el['#capDevN'].innerHTML],
      [false, false, true, '1 device on the fauxcloud Wi-Fi']);
  }
  // while listening: the status and Stop first, the report before the folded Wi-Fi and devices; both folds start
  // closed (the Wi-Fi opens for the password's reveal); the file name carries the bridge's id and the local time
  {
    const e = load(src, { API: { mac4: '1A2B' } });
    e.T.merge(J({ state: 'on', seq: 1, events: [ev(1, 'tcp', ':443')] }));
    const b = e.S().body(), at = s => b.indexOf(s);
    check(label + 'order', [at('id="capSt"') < at('id="capStop"'), at('id="capStop"') < at('id="capOut"'), at('id="capOut"') < at('<details'),
      /<details class="adv" id="capAp"><summary>fauxcloud Wi-Fi: fauxcloud-1A2B<\/summary>/.test(b)], [true, true, true, true]);
    e.T.merge(J({ state: 'on', seq: 1, clients: [{ ip: '192.168.77.23', vendor: 'a4:cf:12', joined_s: 6, left_s: null, n: 1, active: true }, { ip: null, vendor: 'random', joined_s: 2, left_s: null, n: 0, active: false }] }));
    const b2 = e.S().body();
    check(label + 'wifi still folded, devices counted', [b2.includes('<details class="adv" id="capAp"><summary>fauxcloud Wi-Fi'), b2.includes('2 devices on the fauxcloud Wi-Fi')], [true, true]);
    e.S().click({ id: 'capPwAsk' });
    check(label + 'wifi open for the reveal', e.S().body().includes('<details class="adv" id="capAp" open>'), true);
    e.S().click({ id: 'capPwCancel' });
    check(label + 'file name', e.T.fileName(new Date(2026, 10, 21, 9, 3)), 'fauxcloud-1A2B-listen-2026-11-21-0903.txt');
    e.T.merge(J({ state: 'off', ended: 'stop', seq: 1 }));
    check(label + 'stopped: no wifi section', e.S().body().includes('fauxcloud Wi-Fi:'), false);
  }
  // the status words and the ticker
  {
    const e = load(src);
    e.T.merge(J({ state: 'on', elapsed_s: 760 }));
    check(label + 'listening for', e.T.status(), 'Listening for 12:40');
    e.T.merge(J({ state: 'on', elapsed_s: 3725 }));
    check(label + 'over an hour', e.T.status(), 'Listening for 1:02:05');
    e.T.merge(J({ state: 'off', ended: 'stop', elapsed_s: 140 }));
    check(label + 'stopped after', e.T.status(), 'Stopped after 02:20.');
    e.T.merge(J({ state: 'off', ended: 'idle' }));
    check(label + 'idle', e.T.status(), e.T.W.idle);
    e.T.merge(J({ state: 'starting' }));
    check(label + 'starting', e.T.status(), 'Starting…');
  }
  // the empty states, the device rows, the live clock warning (asked and not answered)
  {
    const e = load(src);
    e.T.merge(J({ state: 'on' }));
    check(label + 'nobody joined', e.T.liveHtml().includes(e.T.W.nojoin), true);
    e.T.merge(J({ state: 'on', clients: [{ ip: null, vendor: 'random', joined_s: null, left_s: null, n: 0, active: false }] }));
    const h = e.T.liveHtml();
    check(label + 'joined, nothing sent', [h.includes(e.T.W.empty), h.includes('no address yet · random address · there before the start')], [true, true]);
    e.T.merge(J({ state: 'on', clients: [{ ip: '192.168.77.23', vendor: 'a4:cf:12', joined_s: 65, left_s: 130, n: 2, active: true }] }));
    check(label + 'left row', e.T.liveHtml().includes('192.168.77.23 · a4:cf:12 · joined 01:05, left 02:10 after the start'), true);
    e.T.merge(J({ state: 'on', ntp: { asked: 2, answered: 0 } }));
    check(label + 'asked for time, none', e.T.liveHtml().includes(e.T.W.notime), true);
    e.T.merge(J({ state: 'on', ntp: { asked: 2, answered: 2 } }));
    check(label + 'time answered', e.T.liveHtml().includes(e.T.W.notime), false);
  }
}

(async () => {
  await run('', plain);
  if (mangled) await run('mangled: ', mangled);
  // the fauxcloud Wi-Fi sheet's button and the admin's home line (wired in adminface.js); nothing left in Publishing
  const st = fs.readFileSync(path.join(root, 'firmware', 'fauxcloud', 'web', 'admin-station.js'), 'utf8');
  const af = fs.readFileSync(path.join(root, 'firmware', 'fauxcloud', 'web', 'adminface.js'), 'utf8');
  check('wi-fi sheet button', [af.includes('id="capOpen"'), af.includes("openSheet('capture', b)"), st.includes('capOpen')], [true, true, false]);
  check('lazy sheet', [af.includes("CAPTURE_JS = '/*@asset admin-capture.js*/'"), /LAZY_SHEETS = \[[^\]]*'capture'/.test(af)], [true, true]);
  console.log(`${fail ? 'FAILED' : 'OK'}: ${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})();
