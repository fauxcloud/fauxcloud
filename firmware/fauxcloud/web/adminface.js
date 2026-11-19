// adminface.js -- the admin face on the admin API (challenge-response sign-in, sessions, partial PUTs). Every
// call goes through core.js's one-request queue
// with the X-Fauxcloud header; admin JSON is never cached. The page exists only on the STA side.
// ------------------------------------------------------------------ icons: Lucide (ISC; notices: licenses.txt from THIRD_PARTY_LICENSES.md)
const IC = Object.assign({}, LUCIDE, {
  station: '<path d="M4.9 16.1C1 12.2 1 5.8 4.9 1.9"/><path d="M7.8 4.7a6.14 6.14 0 0 0-.8 7.5"/><circle cx="12" cy="9" r="2"/><path d="M16.2 4.8c2 2 2.26 5.11.8 7.47"/><path d="M19.1 1.9a9.96 9.96 0 0 1 0 14.1"/><path d="M9.5 18h5"/><path d="m8 22 4-11 4 11"/>', // radio-tower
  wifi: '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.859a10 10 0 0 1 14 0"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>',
  pin: '<path d="M20 10c0 4.993-5.539 10.193-7.399 11.799a1 1 0 0 1-1.202 0C9.539 20.193 4 14.993 4 10a8 8 0 0 1 16 0"/><circle cx="12" cy="10" r="3"/>', // map-pin
  wrench: '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.106-3.105c.32-.322.863-.22.983.218a6 6 0 0 1-8.259 7.057l-7.91 7.91a1 1 0 0 1-2.999-3l7.91-7.91a6 6 0 0 1 7.057-8.259c.438.12.54.662.219.984z"/>',
  shield: '<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="m9 12 2 2 4-4"/>', // shield-check
  house: '<path d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8"/><path d="M3 10a2 2 0 0 1 .709-1.528l7-6a2 2 0 0 1 2.582 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  activity: '<path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/><path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="m19.07 4.93-1.41 1.41"/>',
  haze: '<path d="m5.2 6.2 1.4 1.4"/><path d="M2 13h2"/><path d="M20 13h2"/><path d="m17.4 7.6 1.4-1.4"/><path d="M22 17H2"/><path d="M22 21H2"/><path d="M16 13a4 4 0 0 0-8 0"/><path d="M12 5V2.5"/>',
  sprout: '<path d="M14 9.536V7a4 4 0 0 1 4-4h1.5a.5.5 0 0 1 .5.5V5a4 4 0 0 1-4 4 4 4 0 0 0-4 4c0 2 1 3 1 5a5 5 0 0 1-1 3"/><path d="M4 9a5 5 0 0 1 8 4 5 5 0 0 1-8-4"/><path d="M5 21h14"/>',
  leaf: '<path d="M11 20a10 10 0 0010-10 25.9 25.9 0 00-1.04-7.281 1 1 0 00-1.755-.325C15.833 5.5 13 5.5 9.8 6.1A7 7 0 0011 20"/><path d="M2 21a5 5 0 012.911-4.544C7.613 15.212 8.351 15.24 11 13"/>',
  battery: '<path d="M22 14v-4"/><rect x="2" y="6" width="16" height="12" rx="2"/>',
  upload: '<path d="M12 13v8"/><path d="M4 14.9A7 7 0 1 1 15.7 8h1.8a4.5 4.5 0 0 1 2.5 8.2"/><path d="m8 17 4-4 4 4"/>', // cloud-upload
  plane: '<path d="M17.8 19.2 16 11l3.5-3.5C21 6 21.5 4 21 3c-1-.5-3 0-4.5 1.5L13 8 4.8 6.2c-.5-.1-.9.1-1.1.5l-.3.5c-.2.5-.1 1 .3 1.3L9 12l-2 3H4l-1 1 3 2 2 3 1-1v-3l3-2 3.5 5.3c.3.4.8.5 1.3.3l.5-.2c.4-.3.6-.7.5-1.2z"/>', // plane
});
const icon = k => `<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${own(IC, k) || IC.activity}</svg>`;

// the station sheet, its reference tables and the Console tab, and the Publishing sheet: admin-station.js,
// loaded on first use
const STATION_JS = '/*@asset admin-station.js*/', WHY_JS = '/*@asset outlook-why.js*/', OUTLOOK_JS = '/*@asset outlook.js*/', LEARN_JS = '/*@asset admin-learn.js*/',
  FWD_JS = '/*@asset admin-fwd.js*/', METAR_JS = '/*@asset admin-metar.js*/', SHEETS_JS = '/*@asset admin-sheets.js*/',
  // mqtt, name, security and maint arrive with admin-sheets.js
  LAZY_SHEETS = ['station', 'publish', 'weather', 'learn', 'fwd', 'metar', 'mqtt', 'name', 'security', 'maint'], LATER = ['mqtt', 'name', 'security', 'maint'];
const WF = FC.w('adminface', {
  // sign-in and the shared failure lines
  no_answer: 'No answer from fauxcloud. Try again.', ended: 'Your session ended. Sign in again.', copied: 'Copied',
  selected: 'Selected: copy it with your keyboard', pw_locked: 'Too many wrong passwords.', pw_busy: 'fauxcloud is busy. Try again in a moment.',
  pw_wrong: { one: 'Wrong password. {n} try left before a 1-minute pause.', other: 'Wrong password. {n} tries left before a 1-minute pause.' },
  signin_h: 'Sign in to settings', admin_pw: 'Admin password', lock: 'Try again in {s} s.', signin: 'Sign in', signing_in: 'Signing in…',
  signin_where: 'Settings open only from your home network. On fauxcloud’s own Wi-Fi, the one the console uses, this page does not exist.',
  signin_forgot: 'Forgot the password? Hold the BOOT button for 5 seconds to reset fauxcloud and run setup again. Station settings are lost.',
  oss_licenses: 'Open-source licenses', signin_none: 'No answer from fauxcloud. Is this device on your home network?',
  load_none: 'fauxcloud did not answer.', load_none_h: 'It may be restarting. Check that this device is on your home network.', again: 'Try again',
  loading: 'Loading…', loading_t: 'Loading',
  // the sessions list
  this_browser: 'This browser', unknown_ua: 'unknown browser', now: 'now',
  // the console turned away for lack of a slot, and the adopt offer
  adopt_line: 'Console <b>{to}</b> uploads from the same device as <b>{from}</b>. Did its ID change? {btn}', adopt_btn: 'Keep the history under {to}',
  slot_all: 'Console <b>{id}</b> tried to upload, but all station places are in use. To take it, forget a station first.',
  slot_one: 'Console <b>{id}</b> tried to upload, but this fauxcloud keeps one console. To switch to it, forget {btn}.',
  slot_none: "Console <b>{id}</b> tried to upload, but this fauxcloud's one station place is not free. Restart fauxcloud in Settings.",
  // the home: header, sections, tiles
  settings: 'Settings', dashboard: 'Dashboard', theme: 'Switch theme', sign_out: 'Sign out',
  mem_1: '<b>Memory is low.</b>fauxcloud keeps working, but open pages may fail. Close dashboard pages you are not using.',
  mem_n: '<b>Memory is low.</b>fauxcloud keeps working, but more stations or open pages may fail. Forget stations you no longer use, or close dashboard pages you are not using.',
  next_steps: 'Next steps', hide: 'Hide', step_net: 'fauxcloud is on your home network', step_mqtt: 'Connect an MQTT broker, for Home Assistant (optional)',
  step_pair: 'Pair the weather console with {ap}', step_name: 'Give each station a name',
  sect_st: 'Stations', sect_out: 'Home Assistant and online services', sect_conn: 'Connections', sect_bridge: 'This bridge',
  unnamed: 'Unnamed', bat_low: 'Battery low', rejected: '{n} rejected', extra: { one: '{n} extra sensor', other: '{n} extra sensors' },
  st_none: 'No upload since restart', st_on: 'Online', st_off: 'Offline, {ago}',
  pair_h: 'No station yet', pair_1: 'On the console, running on its adapter, hold <b>BARO</b> for 3 seconds until <b>AP</b> shows on its screen.',
  pair_2: 'On a phone, join its <b>WWS-…</b> Wi-Fi, open <b>http://192.168.1.1</b> and choose <b>{ap}</b>.',
  pair_after: 'It appears here after its first upload.', pair_link: 'Password and pairing',
  weather: 'Weather description', local_ol: 'Local outlook', weather_sub: 'From this station\u2019s own readings',
  mqtt_on: 'Connected', not_conn: 'Not connected', mqtt_none: 'Not set up', mqtt_check: 'Check the broker address and that it is running',
  mqtt_opt: 'Optional: for Home Assistant or any MQTT broker',
  publishing: 'Publishing', new_on: 'New stations: on', new_off: 'New stations: off', last_seen: '“Last seen” every {n} min',
  ap_t: 'fauxcloud Wi-Fi for your station', ap_sub: 'Connect your weather station’s console to this network. {count} connected.',
  wifi_t: 'Home Wi-Fi', host_t: 'Name on your network', host_sub: 'Where the dashboard opens',
  loc_t: 'Location and time', not_set: 'Not set', sec_t: 'Password and devices', signed_in: '{n} signed in', sec_sub: 'Change the admin password',
  maint_t: 'Maintenance', maint_sub: 'Restart, setup again, reset',
  details: 'Details', hide_details: 'Hide details', health: 'Bridge health', licenses: 'Open source · Licenses',
  learn: 'Learning', learn_paused: 'Paused: set the location', learn_n: '{n} of 4 in use', learn_sub: 'Each part feeds the outlook once learned',
  fwd_t: 'Online services', fwd_n: { one: '{n} service', other: '{n} services' }, off: 'Off', fwd_refused: 'Key refused', fwd_last: 'Last sent {t}',
  fwd_sub: 'Your readings to Weather Underground, Windy and more',
  metar_t: 'Airport weather', metar_none: 'No airport yet', metar_old: 'Not current', metar_wait: 'Waiting for a report',
  metar_sub: 'Nearby airports’ reports, for reference', metar_add: 'Add an airport',
  reload_fail: 'Saved, but the page could not reload. Pull to refresh.',
  // the sheet frame
  lazy_fail: 'Could not load this page, try again.', unsaved: 'Unsaved changes',
  unsaved_drag: '<b>Unsaved changes.</b> Save them, or close with × to discard.', unsaved_close: '<b>Unsaved changes.</b> Save them, or close again to discard.',
  fix_fields: 'Not saved. Fix the marked fields.', saving: 'Saving…', not_saved: 'Not saved: {e}', saved: 'Saved',
  save_none: 'No answer from fauxcloud. Nothing may have been saved; try again.',
  // home Wi-Fi; why a change failed, by the firmware's code
  wf_auth: 'wrong password', wf_no_ap: 'network not found (fauxcloud uses 2.4 GHz only)', wf_dhcp: 'the router gave no address',
  wf_timeout: 'no connection within 60 s', wf_busy: 'busy', wf_failed: 'failed',
  joined: 'Joined {net}.', join_fail: 'Could not join {net}: {why}. Back on {home}.',
  join_wait: 'No answer yet. If fauxcloud joined the new network, open it at its new address.',
  last_fail: 'The last change failed: {why}. The previous network was restored, so nothing else changed.',
  network: 'Network', signal: 'Signal', address: 'Address',
  trying: '<b>Trying {net}…</b>Up to 60 seconds. This page loses its connection meanwhile. If the new network does not work, fauxcloud goes back to {home} by itself.',
  change_net: 'Change network', scanning: 'Scanning…', pass_for: 'Password for {net}',
  join_note: 'If fauxcloud cannot join within 60 seconds, it goes back to {home} and tells you why. You will not lose access.',
  cancel: 'Cancel', join: 'Join', pw_short: 'Wi-Fi passwords have at least 8 characters.', not_changed: 'Not changed: {e}',
  // fauxcloud's own Wi-Fi
  ap_lead: 'fauxcloud’s own Wi-Fi, <b>{ap}</b>. Connect your weather station’s console to this network (on its adapter, hold BARO on the console for 3 seconds until AP shows, then pick this network on the console’s setup page).',
  ap_n: { one: '{n} console connected now.', other: '{n} consoles connected now.' },
  password: 'Password', copy: 'Copy', hides: 'Hides by itself after 60 seconds.', reveal_pw: 'Your admin password, to show it',
  show_pw: 'Show password', pw_hidden: 'Password hidden', show_why: 'You need it to pair a console. Asks for your admin password first.',
  change_pw: 'Change password', new_pw: 'New password', min8: 'At least 8 characters.',
  ap_warn: '<b>Your console will stop uploading.</b>It keeps the old password until you pair it again: hold BARO for 3 seconds until AP shows and enter the new password on its setup page.',
  ap_ack: 'I will pair the console again', no_answer_short: 'No answer from fauxcloud.', ap_changed: 'Console Wi-Fi password changed. Pair the console again.',
  // location and time
  loc_h: 'Sets the clock for daily totals and the midnight rain reset, and the sunrise and sunset the dashboard sky follows.',
  loc_exact: 'Exact location (optional)', lat: 'Latitude', lat_h: 'From −90 to 90. North is positive.', lon: 'Longitude',
  lon_h: 'From −180 to 180. East is positive.',
  loc_pick: 'Picking a place fills these with its main city. Change them only if the station is far from it: sunrise and sunset move by about 4 minutes per degree of longitude.',
  out_of_range: 'Out of range.',
});
// read when drawn: admin.js runs before its language pack arrives
const WIFI_FAIL = c => own({ auth: WF.wf_auth, no_ap: WF.wf_no_ap, dhcp: WF.wf_dhcp, timeout: WF.wf_timeout, busy: WF.wf_busy }, c);

// ------------------------------------------------------------------ API (core.js queue; admin calls carry X-Fauxcloud)
function api(method, path, body, x) {
  const o = Object.assign({ method, etag: false, headers: { 'X-Fauxcloud': '1' } }, x);
  if (body !== undefined) { o.headers['Content-Type'] = 'application/json'; o.body = JSON.stringify(body); }
  // a session that ended (restart, sign-out elsewhere, 30 days) answers like an unknown request: back to the sign-in,
  // whatever was open (no frozen sheet); the caller's promise then never settles
  return FC.get(path, o).then(r => r.data, e => { if (notSignedIn(e) && !$('#loginForm')) { signedOut(); return new Promise(() => {}); } throw e; });
}
// The one failure path of an action button -- usable again, and said why (a network error, a restart)
const noAnswer = b => () => { if (b) b.disabled = false; toast(WF.no_answer); };
function signedOut() {
  $('#scrim').hidden = $('#sheet').hidden = true; document.body.style.overflow = ''; pageInert(false); SH = null;
  login(WF.ended);
}
// challenge-response: the password never leaves the browser
function withProof(password, body, path, method, x) {
  return api('GET', '/api/admin/challenge').then(c => {
    if (!c.nonce) return { ok: false, error: c.error === 'rate' ? 'busy' : 'no challenge' };
    body.nonce = c.nonce; body.proof = FCX.proof(password, c.salt, c.iters, c.nonce); body.t = Date.now();
    return api(method || 'POST', path, body, x);
  });
}
const API = { settings: {}, status: {}, bridge: {}, stations: [], sessions: [], mac4: '', ap_ssid: '' };
const live2st = (l, cfg) => ({ id: l.id, online: l.online, age_s: l.age_s, uploads: l.uploads, values: l.values || {}, last_rejected: l.last_rejected || [],
  rej: l.rej || {}, battery_low: !!(l.values && l.values.battery_low), cfg,
  // every key the console ever sent (not only the last upload's), age = seconds since it arrived
  extras: Object.fromEntries((l.known || []).map(k => [k.k, k.v])),
  kage: Object.fromEntries((l.known || []).map(k => [k.k, k.age == null ? null : k.age + (l.age_s || 0)])) });
// cfg in the sheets' shape: names from the effective view, filters from the overrides (empty = default)
function cfgOf(j) {
  const e = j.effective || {}, o = j.overrides || {};
  const promote = {};
  Object.entries(e.promote || {}).forEach(([p, v]) => { promote[p] = Object.assign({ scale: 1 }, v); });
  return { name: o.name || '', profile: e.profile, publish: e.publish, stale_s: e.stale_s, limits: Object.assign({}, o.limits), jumps: Object.assign({}, o.jumps),
    calib: Object.assign({}, o.calib), disabled: (e.disabled || []).slice(), promote };
}
async function loadAll() {
  const s = await api('GET', '/api/admin/settings');
  API.settings = Object.assign({}, s.settings, { wifi_change: s.wifi_change || '' }); API.ap_ssid = s.ap_ssid || ''; API.mac4 = API.ap_ssid.split('-').pop();
  const live = (await FC.get('/api/live', { etag: false })).data, d = live.diag || {};
  API.live = live;  // the status row and its Details on the home
  API.status = { sta_ip: d.sta_ip, sta_rssi: d.sta_rssi, mqtt_up: d.mqtt_up, fw_version: (d.fw_version || '').replace(/^fauxcloud /, ''), uptime_s: live.uptime_s || 0,
    heap_free: d.heap_free, heap_low: d.heap_low, ap_clients: d.ap_clients, build: d.build || '', ota: d.ota_state, slot: d.ota_slot,
    reset: d.last_reset, restarts: d.restarts, crash: d.last_crash };
  API.bridge = (await api('GET', '/api/admin/bridge')).effective || {};
  // the console last turned away for lack of a slot (an hour at most), for the line under the Stations tiles
  API.noSlot = await FC.get('/api/bridge/status', { etag: false }).then(r => r.data.no_slot_id || '', () => '');
  const st = [];
  for (const l of live.stations || []) { const j = await api('GET', '/api/admin/station?station=' + encodeURIComponent(l.id));
    st.push(Object.assign(live2st(l, cfgOf(j)), { key: j.key, km: j.key_mismatch || 0, mac: j.src_mac, fp: j.fp, adopt: j.adopt || '', lu: j.learn_use || 0 })); }
  API.stations = st;
  API.sessions = await loadSessions();
  API.fwd = API.bridge.forwarding ? await api('GET', '/api/admin/forwarding').catch(() => null) : null;  // the Online services tile
  // the airport-weather tile, only on a build that has it (an HTTP-only build answers no JSON: null, not signed out)
  API.metar = await FC.get('/api/admin/metar', { etag: false, headers: { 'X-Fauxcloud': '1' } }).then(r => r.data, () => null);
  API.metarLive = API.metar && API.metar.on ? await FC.get('/api/metar', { etag: false }).then(r => r.data, () => null) : null;
}
async function loadSessions() {
  const list = await api('GET', '/api/admin/sessions');
  return (Array.isArray(list) ? list : []).map(x => ({ id: x.id, cur: !!x.current, who: x.current ? WF.this_browser : x.ip, ua: x.ua || WF.unknown_ua, ip: x.ip, last: x.current ? WF.now : ago(x.last_s) + '' }));
}

// ------------------------------------------------------------------ sky: the dashboard's rule for the first station
function skyFor(x) {
  if (x && (x.values.rain_rate_mmh ?? x.values.rain_mm) > 0) return 'rain';  // the dashboard's rule
  if (x && x.values.solar_wm2 != null && !x.last_rejected.includes('solar_wm2')) return x.values.solar_wm2 > 5 ? 'day' : 'night';
  return clockSky();
}

// ------------------------------------------------------------------ helpers
const clone = o => JSON.parse(JSON.stringify(o));
const slug = s => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/ł/g, 'l').replace(/₂/g, '2').replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '').slice(0, 23) || 'sensor';
const field = (id, label, inner, hint, err) => `<div class="fld"><label class="f" for="${id}">${label}${inner}</label>${err ? `<p class="error" id="${id}-msg">${err}</p>` : hint ? `<p class="hint" id="${id}-msg">${hint}</p>` : ''}</div>`;
const text = (id, v, o = {}) => `<span class="input${o.err ? ' err' : ''}"><input id="${id}" ${o.type ? `type="${o.type}"` : ''} value="${esc(v)}" ${o.ph ? `placeholder="${esc(o.ph)}"` : ''} ${o.num ? 'inputmode="decimal"' : ''} autocomplete="${o.ac || 'off'}" autocapitalize="off" spellcheck="false" aria-describedby="${id}-msg">${o.unit != null ? `<span class="unit">${o.unit}</span>` : ''}${o.after || ''}</span>`;
const sel = (id, v, opts) => `<span class="input"><select id="${id}">${opts.map(([val, l]) => `<option value="${esc(val)}"${String(val) === String(v) ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select></span>`;
const tog = (id, on, l, s) => `<label class="toggle" for="${id}"><input type="checkbox" role="switch" id="${id}"${on ? ' checked' : ''}><span>${l}${s ? `<small>${s}</small>` : ''}</span></label>`;
const num = v => String(v).trim() === '' ? null : Number(String(v).replace(',', '.'));
const errs = j => j && j.errors && j.errors.length ? j.errors.join('; ') : '';
// A console turned away for lack of a slot, named; at one station the one to forget is named too (its sheet opens).
// The same device as the loaded station (its ID changed after a console reset): the offer to keep the history
const ADOPT_LINE = () => WF.adopt_line;  // read when drawn (the pack comes after this script)
const ADOPT_BTN = () => WF.adopt_btn;
const NO_SLOT = (id, sts, cap) => cap !== 1 ? FC.t(WF.slot_all, { id: esc(id) })
  : sts.length && sts[0].adopt === id ? FC.t(ADOPT_LINE(), { to: esc(id), from: esc(sts[0].id),
    btn: `<button type="button" class="link" data-sheet="st0" data-adopt="1">${FC.t(ADOPT_BTN(), { to: esc(id) })}</button>` })
  : sts.length ? FC.t(WF.slot_one, { id: esc(id), btn: `<button type="button" class="link" data-sheet="st0">${esc(sts[0].id)}</button>` })
  : FC.t(WF.slot_none, { id: esc(id) });
let toastT; function toast(t) { const el = $('#toast'); el.textContent = t; el.hidden = false; clearTimeout(toastT); toastT = setTimeout(() => el.hidden = true, 2600); }
// copy on plain HTTP, where navigator.clipboard is missing: a hidden textarea, else select the text for the keyboard
function copyText(t, selEl) {
  const old = () => { const ta = document.createElement('textarea'); ta.value = t; ta.setAttribute('readonly', ''); ta.style.cssText = 'position:fixed;opacity:0'; document.body.appendChild(ta); ta.select();
    let ok = false; try { ok = document.execCommand('copy'); } catch (e) {} ta.remove();
    if (ok) toast(WF.copied); else { const s = getSelection(), r = document.createRange(); r.selectNodeContents(selEl); s.removeAllRanges(); s.addRange(r); toast(WF.selected); } };
  navigator.clipboard && window.isSecureContext ? navigator.clipboard.writeText(t).then(() => toast(WF.copied), old) : old();
}

// the station console (admin-station.js) is on screen while CON is set; the base only needs to stop it
let CON = null; // the open console: { id, lines (oldest first), last, timer, busy }
function conStop() { if (CON && CON.timer) { clearInterval(CON.timer); CON.timer = null; } }

// ------------------------------------------------------------------ app state and sign-in (shared 5-try / 60 s lockout, also used by the password reveal)
const A = { tries: 5, lock: 0 };
let lockT = null;
function startLock(s) {
  A.lock = s; A.tries = 5; clearInterval(lockT);
  lockT = setInterval(() => { A.lock--; document.querySelectorAll('[data-lock]').forEach(e => e.textContent = A.lock); if (A.lock <= 0) { clearInterval(lockT); if ($('#loginForm')) login(); } }, 1000);
}
// a failed proof: the firmware counts too (5 per minute per address, 10 per 10 minutes for the account)
function failTry(j) {
  if (j && j.error === 'locked') { startLock(j.retry_s || 60); return WF.pw_locked; }
  if (j && j.error === 'busy') return WF.pw_busy;
  A.tries--; if (A.tries > 0) return FC.t(WF.pw_wrong, { n: A.tries });
  startLock(60); return WF.pw_locked;
}
function login(msg) {
  conStop(); CON = null;
  $('#app').innerHTML = `<form class="glass signin" id="loginForm" novalidate>
    <span class="brand">${MARK}fauxcloud</span><h1>${WF.signin_h}</h1>
    ${field('pw', WF.admin_pw, text('pw', '', { type: 'password', ac: 'current-password', err: msg }), '', msg)}
    ${A.lock ? `<p class="error" role="alert">${FC.t(WF.lock, { s: `<b data-lock>${A.lock}</b>` })}</p>` : ''}
    <button class="btn primary" type="submit" ${A.lock ? 'disabled' : ''}>${WF.signin}</button>
    <p class="hint">${WF.signin_where}</p>
    <p class="hint">${WF.signin_forgot}</p>
    <p class="hint"><a class="link" href="/*@asset licenses.txt*/" target="_blank" rel="noopener">${WF.oss_licenses}</a></p></form>`;
  $('#loginForm').addEventListener('submit', e => { e.preventDefault(); if (A.lock) return;
    const b = $('#loginForm button[type=submit]'); b.disabled = true; b.textContent = WF.signing_in;
    withProof($('#pw').value, {}, '/api/admin/login').then(j => {
      if (j.ok) { A.tries = 5; start(); return; }
      login(failTry(j)); $('#pw') && $('#pw').focus();
    }, () => login(WF.signin_none)); });
  setTimeout(() => $('#pw') && $('#pw').focus());
}
// Not signed in: the gate answers like an unknown request (plain "success"), which is not JSON -> sign-in.
// Anything else (network, timeout) is a load failure with a retry, not a silent sign-out.
const notSignedIn = e => e instanceof SyntaxError || e.message === 'HTTP 401';  // the 401 of a signed-out call
function loadFailed(e) {
  if (notSignedIn(e)) { login(); return; }
  $('#app').innerHTML = `<div class="wrap"><section class="glass card"><p class="lead">${WF.load_none}</p><p class="hint">${WF.load_none_h}</p><div class="row"><button type="button" class="btn primary" id="retry">${WF.again}</button></div></section></div>`;
  $('#retry').onclick = start;
}
function start() {
  // the first call keeps the shell's line (lang.js swaps its word when the pack lands); a retry draws it from the table
  if (!$('#app [data-t]')) $('#app').innerHTML = `<div class="wrap"><p class="lead">${WF.loading}</p></div>`;
  loadAll().then(home, loadFailed);
}

// ------------------------------------------------------------------ home: tiles
const stepsHidden = () => FC.store('fc-hide-steps') === '1';
// The boot log's detail switch says what it does, and its state stands beside the download (else it reads as dead)
// The mark alone beside the text in the header (text headers, the mark the
// only brand element; the dashboard's own copy is in index.html)
const MARK = '<svg class="mark" viewBox="0 0 256 256" aria-hidden="true"><g fill="none" stroke="currentColor" stroke-width="20" stroke-linecap="round" stroke-linejoin="round"><path d="M62 180C41 180 26 164 26 144C26 124 41 108 62 106C67 76 90 55 119 55C148 55 170 74 176 101C183 97 190 95 197 95"/><path d="M216 116C225 124 230 135 230 147C230 166 214 180 194 180H62"/><path d="M78 141C100 128 119 128 136 138C151 147 167 147 182 138" stroke-width="17"/></g></svg>';
// a station tile's state line (also redrawn in place by the home poll); offline as the dashboard says it,
// "Offline since {t}" with a synced clock (the time the console went quiet), the age before that
const stState = x => `<span class="dot ${x.online ? '' : 'off'}"></span>${!x.uploads ? WF.st_none : x.online ? WF.st_on
  : (API.live && API.live.time_synced && offSince(new Date(Date.now() - x.age_s * 1e3), new Date())) || FC.t(WF.st_off, { ago: ago(x.age_s) })}`;
// data going out has a section of its own (read when drawn)
const SECT_OUT = () => WF.sect_out;
function home() {
  const s = API.settings, st = API.status, b = API.bridge, z = TZ ? zoneByName(b.tz_name) : null;
  const x0 = API.stations[0]; setSky(skyFor(x0), x0 && x0.values, !(x0 && x0.online));
  const steps = [[!!st.sta_ip, WF.step_net], [st.mqtt_up, WF.step_mqtt, 'mqtt'],
    [API.stations.length > 0, FC.t(WF.step_pair, { ap: esc(API.ap_ssid) }), 'ap'], [API.stations.length > 0 && API.stations.every(x => x.cfg.name), WF.step_name]];
  const left = steps.filter(x => !x[0]).length, rej = x => Object.values(x.rej).reduce((a, c) => a + c, 0);
  const T = (key, ic, label, val, sub, warn, idle) => `<button type="button" class="glass tile${idle ? ' idle' : ''}" data-sheet="${key}"><span class="lab">${ic}${label}<span class="more" aria-hidden="true">›</span></span><span class="val">${val}</span><span class="sub">${warn ? '<span class="dot bad"></span>' : ''}${sub}</span></button>`;
  const wc = s.wifi_change || '', wfail = wc.indexOf('failed:') === 0;
  // Learning and the Weather description are per station: their tiles sit in the Stations section,
  // in ONE row with the station's card (side by side, stacked on a narrow phone). Home Assistant and online services
  // come before Connections; both tiles only once a first station exists
  $('#app').innerHTML = `<div class="wrap">
    <header class="glass top"><span class="brand"><a href="${SITE_URL}" target="_blank" rel="noopener">${MARK}fauxcloud</a> <span class="what">${WF.settings}</span></span><span class="sp"></span>
      <a class="pill" href="/" id="toDash">${WF.dashboard}</a><button type="button" class="pill" id="langBtn" hidden>EN</button><button type="button" class="pill" id="theme" aria-label="${WF.theme}">◐</button><button type="button" class="pill" id="signout">${WF.sign_out}</button></header>
    ${st.heap_low ? `<div class="callout warn" role="status">${b.max_stations_cap === 1 ? WF.mem_1 : WF.mem_n}</div>` : ''}
    ${left && !stepsHidden() ? `<section class="glass card"><div class="row"><h2 style="flex:1">${WF.next_steps}</h2><button type="button" class="pill" id="hideSteps">${WF.hide}</button></div><ul class="checklist">${steps.map(([ok, l, to]) => `<li class="${ok ? 'done' : ''}"><span class="box">${ok ? '✓' : ''}</span>${to && !ok ? `<button type="button" class="link" data-sheet="${to}">${l}</button>` : l}</li>`).join('')}</ul></section>` : ''}
    <h2 class="sect">${WF.sect_st}</h2><div class="tiles">${API.stations.map((x, i) => T('st' + i, icon('station'), esc(x.id),
      // a station problem reads like on the dashboard: the hatched chip beside the name; offline = grey dot + words
      `<span class="nm">${esc(x.cfg.name || WF.unnamed)}${x.battery_low ? ` <span class="rej">${WF.bat_low}</span>` : ''}</span>`,
      `<span class="state">${stState(x)}</span>${rej(x) ?`<span>${FC.t(WF.rejected, { n: rej(x) })}</span>` : ''}${Object.keys(x.cfg.promote).length ? `<span>${FC.t(WF.extra, { n: Object.keys(x.cfg.promote).length })}</span>` : ''}`)).join('')
      || `<section class="glass card pair"><h3>${WF.pair_h}</h3><ol class="steps">
        <li><span>${WF.pair_1}</span></li>
        <li><span>${FC.t(WF.pair_2, { ap: esc(API.ap_ssid) })}</span></li>
        <li><span>${WC.wu_step}</span></li></ol>
        <p class="hint">${WF.pair_after} <button type="button" class="link" data-sheet="ap">${WF.pair_link}</button></p></section>`}
      ${API.stations.length ? learnTile(T, b) + T('weather', icon('sun'), WF.weather, WF.local_ol, WF.weather_sub) : ''}</div>
    ${API.noSlot ? `<p class="hint" role="status">${NO_SLOT(API.noSlot, API.stations, API.bridge.max_stations_cap)}</p>` : ''}
    <h2 class="sect">${SECT_OUT()}</h2><div class="tiles">
      ${T('mqtt', icon('house'), 'MQTT', st.mqtt_up ? WF.mqtt_on : s.mqtt_host ? WF.not_conn : WF.mqtt_none, st.mqtt_up ? esc(s.mqtt_host) : s.mqtt_host ? WF.mqtt_check : WF.mqtt_opt, !st.mqtt_up && !!s.mqtt_host, !s.mqtt_host)}
      ${T('publish', icon('activity'), WF.publishing, b.auto_publish_new_stations ? WF.new_on : WF.new_off, FC.t(WF.last_seen, { n: Math.round(b.mqtt_interval_last_seen_s / 60) }))}
      ${fwdTile(T)}
      ${metarTile(T)}
    </div>
    <h2 class="sect">${WF.sect_conn}</h2><div class="tiles">
      ${T('ap', icon('station'), WF.ap_t, esc(API.ap_ssid), FC.t(WF.ap_sub, { count: `<span id="apN">${st.ap_clients ?? 0}</span>` }))}
      ${T('wifi', icon('wifi'), WF.wifi_t, esc(s.sta_ssid), st.sta_ip ? `${rssiBars(st.sta_rssi)} ${String(st.sta_rssi).replace('-', '−')} dBm, ${esc(st.sta_ip)}` : WF.not_conn, wfail)}
      ${T('name', icon('wifi'), WF.host_t, esc(s.hostname) + '.local', WF.host_sub)}
    </div>
    <h2 class="sect">${WF.sect_bridge}</h2><div class="tiles">
      ${T('location', z ? flag(z.cc) : icon('pin'), WF.loc_t, z ? esc(placeName(z)) : esc(b.tz_name || WF.not_set), z ? esc(clockLine(z, b.tz)) : esc(utc(b.tz)))}
      ${T('security', icon('shield'), WF.sec_t, FC.t(WF.signed_in, { n: API.sessions.length }), WF.sec_sub)}
      ${T('maint', icon('wrench'), WF.maint_t, 'fauxcloud ' + esc(st.fw_version), WF.maint_sub)}
    </div>
    <details class="glass health" id="health"><summary data-more="${WF.details}" data-less="${WF.hide_details}"><h2 class="sr">${WF.health}</h2>${healthRow(API.live, !!API.settings.mqtt_host)}</summary><div class="hgrid" id="hgrid"></div></details>
    <p class="note"><a href="/*@asset licenses.txt*/" target="_blank" rel="noopener">${WF.licenses}</a>${repoLink()}</p></div>`;
  FCL.menu();  // the language pill in the top bar just drawn
  // the Details body comes with admin-station.js (lazy), on the first open
  $('#health').ontoggle = () => { if ($('#health').open) healthOpen(); };
  $('#app').onclick = e => { const t = e.target.closest('[data-sheet]'); if (t) { API.adoptAsk = !!t.dataset.adopt; openSheet(t.dataset.sheet, t); return; }
    if (e.target.id === 'signout') { api('POST', '/api/admin/logout', {}).finally(() => login()); }
    if (e.target.id === 'hideSteps') { FC.store('fc-hide-steps', '1'); home(); return; }
    if (e.target.id === 'theme') FC.toggleTheme(); };
  // the location tile names the country once the zone list is here (one request, after the page is up)
  if (!TZ) pkLoad(() => { if (!SH && $('[data-sheet="location"]')) home(); });
}
// "Learning" -- a status like every other tile: paused without the location (grey), else the learners in use
// (L1, L2, L5, L6) of the station that has the most, else still learning
function learnTile(T, b) {
  const noLoc = b.lat == null || b.lon == null, n = Math.max(0, ...API.stations.map(x => x.lu || 0));
  return T('learn', icon('activity'), WF.learn, noLoc ? WF.learn_paused : n ? FC.t(WF.learn_n, { n }) : WF.learn,
    WF.learn_sub, false, noLoc);
}
// "Online services" -- Off / N services; the last send, or a refused key
function fwdTile(T) {
  const f = API.fwd, t = (f && f.targets) || [], p = t.find(x => x.paused), ok = t.map(x => x.last_ok).filter(Boolean).sort().pop();
  return T('fwd', icon('upload'), WF.fwd_t, f ? FC.t(WF.fwd_n, { n: t.length }) : WF.off,
    p ? WF.fwd_refused : ok ? FC.t(WF.fwd_last, { t: dayTime(ok, new Date()) }) : WF.fwd_sub, !!p, !f);
}
// "Airport weather" -- Off / the first current airport in the user's order and its report's age / Not
// current; the airports by their display names
function metarTile(T) {
  const m = API.metar;
  if (!m || !m.available) return '';
  const r = (API.metarLive && API.metarLive.airports || []).filter(a => a.obs), x = r.find(a => !a.stale);
  return T('metar', icon('plane'), WF.metar_t, !m.on ? WF.off : !m.airports.length ? WF.metar_none : x ? `${esc(x.name || x.icao)} · ${ago(x.age_s)}` : r.length ? WF.metar_old : WF.metar_wait,
    !m.on ? WF.metar_sub : m.airports.map(a => esc(a.name || a.icao)).join(', ') || WF.metar_add, !!(m.on && r.length && !x), !m.on);
}
// reload the data behind the tiles after a save (one request at a time)
function refresh() { return loadAll().then(() => { if (!SH) home(); }, e => { if (notSignedIn(e)) login(); else toast(WF.reload_fail); }); }

// ------------------------------------------------------------------ sheets: one form each, saving only its own fields
// Every sheet: title + icon, body(), and optionally draft/base for a Save button that sends only the changed keys.
let SH = null, lastFocus = null;
function openSheet(key, from) {
  lastFocus = from;
  const name = key.startsWith('st') ? 'station' : key;
  if (!SHEETS[name] && LAZY_SHEETS.includes(name)) { // these sheets arrive with admin-station.js on first use
    SH = { key, err: {}, msg: '', icon: icon('station'), title: WF.loading_t, body: () => `<p class="hint loading">${WF.loading}</p>` };
    (name === 'weather' ? FC.lazy(OUTLOOK_JS) : Promise.resolve()).then(() => FC.lazy(name === 'weather' ? WHY_JS : name === 'learn' ? LEARN_JS : name === 'fwd' ? FWD_JS : name === 'metar' ? METAR_JS : LATER.includes(name) ? SHEETS_JS : STATION_JS)).then(() => { if (SH && SH.key === key) openSheet(key, from); }, () => {
      if (!SH || SH.key !== key) return;
      SH.body = () => `<p class="error">${WF.lazy_fail}</p><div class="row end"><button type="button" class="btn primary" id="lazyAgain">${WF.again}</button></div>`;
      SH.click = b => { if (b.id === 'lazyAgain') openSheet(key, from); }; draw();
    });
  } else if (name === 'station' && !openSheet.fresh) { // the station's live line as of now, not of the page load
    const i = +key.slice(2), x = API.stations[i];
    openSheet.fresh = true;
    FC.get('/api/live?station=' + encodeURIComponent(x.id), { etag: false, front: true }).then(r => { if (r.data.station) API.stations[i] = Object.assign(live2st(r.data.station, x.cfg), { key: x.key, km: x.km, lu: x.lu }); }, () => {})
      .finally(() => { openSheet(key, from); openSheet.fresh = false; });
    return;
  } else SH = Object.assign({ key, err: {}, msg: '' }, SHEETS[name](key));
  $('#scrim').hidden = $('#sheet').hidden = false; document.body.style.overflow = 'hidden'; pageInert(true);
  draw(); $('#sheet').focus();
}
function closeSheet() { conStop(); CON = null; $('#scrim').hidden = $('#sheet').hidden = true; document.body.style.overflow = ''; pageInert(false); SH = null; home(); if (lastFocus && lastFocus.dataset) { const t = document.querySelector(`[data-sheet="${lastFocus.dataset.sheet}"]`); if (t) t.focus(); } }
function changed() { if (!SH || !SH.draft) return {}; const o = {}; for (const k in SH.draft) if (JSON.stringify(SH.draft[k]) !== JSON.stringify(SH.base[k])) o[k] = SH.draft[k]; return o; }
function draw(focusId) {
  const top = $('#shBody').scrollTop;
  $('#shIcon').innerHTML = SH.icon; $('#shTitle').textContent = SH.title; $('#shBody').innerHTML = SH.body();
  const c = Object.keys(changed()).length, savable = !!SH.draft && !SH.noSave;
  $('#shFoot').hidden = !savable && !SH.msg; $('#shSave').hidden = $('#shCancel').hidden = !savable; $('#shSave').disabled = !c || !!SH.saving;
  $('#shMsg').innerHTML = SH.msg || (c ? WF.unsaved : '');
  $('#shBody').scrollTop = top;
  if (CON && $('#conOut')) conDraw(); else conStop(); // the console polls only while it is on screen
  const tb = document.querySelector('#shBody .tabs'), ts = tb && tb.querySelector('[aria-selected="true"]'); // keep the chosen tab in view on a phone
  if (ts && (ts.offsetLeft < tb.scrollLeft || ts.offsetLeft + ts.offsetWidth > tb.scrollLeft + tb.clientWidth)) tb.scrollLeft = ts.offsetLeft - 8;
  if (focusId) { const f = document.getElementById(focusId); if (f) { f.focus(); if (f.setSelectionRange && f.type !== 'number' && f.type !== 'checkbox') try { f.setSelectionRange(f.value.length, f.value.length); } catch (e) {} } }
}
function sync() { const c = Object.keys(changed()).length; $('#shSave').disabled = !c; if (!SH.msg) $('#shMsg').textContent = c ? WF.unsaved : ''; }
$('#scrim').addEventListener('click', () => tryClose());
$('#shClose').addEventListener('click', () => tryClose());
$('#shCancel').addEventListener('click', () => closeSheet());
document.addEventListener('keydown', e => { if (e.key === 'Escape' && SH) tryClose(); });
// drag down to close (phone sheet), the dashboard's code: follows the finger, closes past a quarter of its height or
// on a flick. Admin rule: with unsaved changes the sheet snaps back and says so, like the × button.
sheetDrag($('#sheet'), $('#scrim'), closeSheet, () => !Object.keys(changed()).length, () => {
  closeArmed = true; $('#shMsg').innerHTML = WF.unsaved_drag; setTimeout(() => closeArmed = false, 4000);
});
let closeArmed = false;
function tryClose() { if (SH && Object.keys(changed()).length && !closeArmed) { closeArmed = true; $('#shMsg').innerHTML = WF.unsaved_close; setTimeout(() => closeArmed = false, 4000); return; } closeArmed = false; closeSheet(); }
$('#shSave').addEventListener('click', () => {
  const E = SH.validate ? SH.validate() : {}; SH.err = E;
  if (Object.keys(E).length) { SH.msg = `<span class="error">${WF.fix_fields}</span>`; draw(); SH.msg = ''; return; }
  // PUT with only the changed keys; the firmware merges it and answers with the effective config + errors
  const sh = SH; sh.saving = true; $('#shSave').disabled = true; $('#shMsg').textContent = WF.saving;
  sh.save(changed()).then(j => {
    sh.saving = false;
    if (SH !== sh) return;
    const e = errs(j);
    if (e) { SH.msg = `<span class="error">${FC.t(WF.not_saved, { e: esc(e) })}</span>`; draw(); SH.msg = ''; return; }
    toast(j && j.notes && j.notes.length ? j.notes.join(' ') : WF.saved); SH.draft = null; closeSheet(); refresh();
  }, () => { sh.saving = false; if (SH === sh) { SH.msg = `<span class="error">${WF.save_none}</span>`; draw(); SH.msg = ''; } });
});
$('#shBody').addEventListener('input', e => SH && SH.input && SH.input(e.target));
$('#shBody').addEventListener('change', e => SH && SH.change && SH.change(e.target));
// Enter in a text field presses the next primary (or danger) button below it in the sheet, else the footer Save
$('#shBody').addEventListener('keydown', e => {
  const t = e.target;
  if (e.key !== 'Enter' || e.isComposing || t.tagName !== 'INPUT' || t.type === 'checkbox' || t.type === 'file') return;
  e.preventDefault();
  if (t.id === 'pkq') return; // the picker filters as you type
  const next = [...$('#shBody').querySelectorAll('.btn.primary, .btn.danger')].find(b => !b.disabled && !b.hidden && b.offsetParent &&
    t.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING);
  if (next) next.click(); else if (!$('#shSave').hidden && !$('#shSave').disabled) $('#shSave').click();
});
$('#shBody').addEventListener('click', e => { const b = e.target.closest('button'); if (b && b.id === 'conCopy') { conCopy(); return; } if (b && SH && SH.click) SH.click(b); });

const SHEETS = {
  // ---- home Wi-Fi: change with the 60 s revert
  wifi() {
    const s = API.settings, st = API.status, n = { scan: false, scanning: false, nets: [], net: null, pass: '', trying: null, result: '' };
    const scan = again => { n.scanning = true; api('GET', '/api/admin/wifi/scan' + (again ? '?again=1' : '')).then(j => {
      if (j.state === 'running') { setTimeout(() => scan(false), 1500); return; }
      n.scanning = false; n.nets = (j.networks || []).map(x => ({ ssid: x.ssid, rssi: x.rssi, open: x.open })); if (SH && SH.key === 'wifi') draw(); }, () => { n.scanning = false; }); };
    // after PUT the ESP tries the new network for up to 60 s; poll the result every 5 s, one request at a time
    const watch = t0 => api('GET', '/api/admin/settings').then(j => {
      const w = j.wifi_change || '';
      if (w === 'ok') { n.result = FC.t(WF.joined, { net: esc(n.trying) }); n.bad = false; n.trying = null; refresh(); }
      else if (w.indexOf('failed:') === 0) { n.result = FC.t(WF.join_fail, { net: esc(n.trying), why: WIFI_FAIL(w.slice(7)) || WF.wf_failed, home: esc(s.sta_ssid) }); n.bad = true; n.trying = null; }
      else if (Date.now() - t0 < 75e3) { setTimeout(() => watch(t0), 5000); return; }
      else { n.trying = null; n.result = WF.join_wait; n.bad = false; }
      if (SH && SH.key === 'wifi') draw();
    }, () => { if (Date.now() - t0 < 75e3) setTimeout(() => watch(t0), 5000); });
    const wc = s.wifi_change || '';
    if (wc.indexOf('failed:') === 0) { n.result = FC.t(WF.last_fail, { why: WIFI_FAIL(wc.slice(7)) || WF.wf_failed }); n.bad = true; }
    return { icon: icon('wifi'), title: WF.wifi_t,
      body() { const E = this.err;
        return `<dl class="kv"><dt>${WF.network}</dt><dd>${esc(s.sta_ssid)}</dd><dt>${WF.signal}</dt><dd>${String(st.sta_rssi).replace('-', '−')} dBm</dd><dt>${WF.address}</dt><dd>${esc(st.sta_ip)}</dd></dl>
        ${n.result ? `<div class="callout${n.bad ? ' warn' : ''}" role="status">${n.result}</div>` : ''}
        ${n.trying ? `<div class="callout" role="status">${FC.t(WF.trying, { net: esc(n.trying), home: esc(s.sta_ssid) })}</div>`
        : n.scan ? `<h3>${WF.change_net}</h3>${n.scanning && !n.nets.length ? `<p class="hint">${WF.scanning}</p>` : ''}<ul class="nets">${n.nets.map((x, i) => `<li><button type="button" data-nnet="${i}" aria-pressed="${n.net === i}">${rssiBars(x.rssi)}<span class="name">${esc(x.ssid)}</span></button></li>`).join('')}</ul>
          ${n.net != null ? field('npass', FC.t(WF.pass_for, { net: esc(n.nets[n.net].ssid) }), text('npass', n.pass, { type: 'password', err: E.npass }), '', E.npass) : ''}
          <div class="callout">${FC.t(WF.join_note, { home: esc(s.sta_ssid) })}</div>
          <div class="row end"><button type="button" class="btn quiet" id="ncancel">${WF.cancel}</button><button type="button" class="btn primary" id="njoin" ${n.net == null ? 'disabled' : ''}>${WF.join}</button></div>`
        : `<div class="row"><button type="button" class="btn quiet" id="nscan">${WF.change_net}</button></div>`}`; },
      input(t) { if (t.id === 'npass') n.pass = t.value; },
      click(b) {
        if (b.id === 'nscan') { n.scan = true; scan(true); draw(); return; }
        if (b.id === 'ncancel') { n.scan = false; n.net = null; draw(); return; }
        if (b.dataset.nnet) { n.net = +b.dataset.nnet; draw('npass'); return; }
        if (b.id === 'njoin') { const net = n.nets[n.net];
          if (!net.open && n.pass.length < 8) { this.err = { npass: WF.pw_short }; draw('npass'); return; }
          n.trying = net.ssid; n.scan = false; n.result = ''; n.bad = false; this.err = {}; draw();
          api('PUT', '/api/admin/settings', { sta_ssid: net.ssid, sta_pass: n.pass }).then(j => {
            const e = errs(j); n.pass = '';
            if (e) { n.trying = null; n.result = FC.t(WF.not_changed, { e: esc(e) }); n.bad = /Could not|failed/.test(e); draw(); return; }  // e: the firmware's English
            setTimeout(() => watch(Date.now()), 5000);
          }, () => setTimeout(() => watch(Date.now()), 5000)); }
      } };
  },

  // ---- console Wi-Fi (AP): reveal (the one secret that can be read back) + change
  ap() {
    const st = API.status; const r = { asking: false, shown: false, plain: '', pw: '', err: '', newp: '', ack: false, timer: null, busy: false };
    return { icon: icon('station'), title: WF.ap_t,
      body() {
        return `<p class="lead">${FC.t(WF.ap_lead, { ap: esc(API.ap_ssid) })} ${WC.wu_step} ${FC.t(WF.ap_n, { n: st.ap_clients ?? 0 })}</p>
        <h3>${WF.password}</h3>
        ${r.shown ? `<div class="secret" id="apShown">${esc(r.plain)}</div><div class="row"><button type="button" class="btn quiet" id="apCopy">${WF.copy}</button><button type="button" class="btn quiet" id="apHide">${WF.hide}</button><span class="hint">${WF.hides}</span></div>`
        : r.asking ? `${field('aprev', WF.reveal_pw, text('aprev', r.pw, { type: 'password', ac: 'current-password', err: r.err }), '', r.err)}
            ${A.lock ? `<p class="error">${FC.t(WF.lock, { s: `<b data-lock>${A.lock}</b>` })}</p>` : ''}<div class="row"><button type="button" class="btn quiet" id="apCancel">${WF.cancel}</button><button type="button" class="btn primary" id="apGo" ${A.lock || r.busy ? 'disabled' : ''}>${WF.show_pw}</button></div>`
        : `<div class="secret" aria-label="${WF.pw_hidden}">••••••••••••</div><div class="row"><button type="button" class="btn quiet" id="apAsk">${WF.show_pw}</button><span class="hint">${WF.show_why}</span></div>`}
        <h3 style="margin-top:8px">${WF.change_pw}</h3>
        ${field('apnew', WF.new_pw, text('apnew', r.newp, { type: 'password', ac: 'new-password' }), WF.min8)}
        <div class="callout warn">${WF.ap_warn}</div>
        <label class="toggle" for="apack"><input type="checkbox" id="apack"${r.ack ? ' checked' : ''}><span>${WF.ap_ack}</span></label>
        <div class="row end"><button type="button" class="btn primary" id="apsave" ${r.ack && r.newp.length >= 8 ? '' : 'disabled'}>${WF.change_pw}</button></div>`; },
      input(t) { if (t.id === 'aprev') r.pw = t.value; if (t.id === 'apnew') { r.newp = t.value; $('#apsave').disabled = !(r.ack && r.newp.length >= 8); } },
      change(t) { if (t.id === 'apack') { r.ack = t.checked; $('#apsave').disabled = !(r.ack && r.newp.length >= 8); } },
      click(b) {
        if (b.id === 'apAsk') { r.asking = true; r.err = ''; draw('aprev'); return; }
        if (b.id === 'apCancel') { r.asking = false; r.pw = ''; draw(); return; }
        if (b.id === 'apGo') { // POST /api/admin/ap-pass/reveal {nonce, proof}; shares the sign-in lockout
          r.busy = true; draw();
          withProof(r.pw, {}, '/api/admin/ap-pass/reveal').then(j => { r.busy = false; r.pw = '';
            if (j.ok) { A.tries = 5; r.shown = true; r.plain = j.ap_pass; r.asking = false; clearTimeout(r.timer); r.timer = setTimeout(() => { r.shown = false; r.plain = ''; if (SH && SH.key === 'ap') draw(); }, 60000); draw(); }
            else { r.err = failTry(j); draw('aprev'); } }, () => { r.busy = false; r.pw = ''; r.err = WF.no_answer_short; draw('aprev'); }); return; }
        if (b.id === 'apHide') { r.shown = false; r.plain = ''; clearTimeout(r.timer); draw(); return; }
        if (b.id === 'apCopy') { copyText(r.plain, $('#apShown')); return; }
        if (b.id === 'apsave') { b.disabled = true; api('PUT', '/api/admin/settings', { ap_pass: r.newp }).then(j => { const e = errs(j);
          if (e) { toast(FC.t(WF.not_changed, { e })); draw(); return; }  // toast uses textContent
          r.newp = ''; r.ack = false; r.shown = false; r.plain = ''; toast(WF.ap_changed); draw(); }, noAnswer(b)); }
      } };
  },

  // ---- location and time: the world picker
  location() {
    const b = API.bridge, base = { tz: b.tz, tz_name: b.tz_name, lat: b.lat, lon: b.lon }, draft = clone(base), P = { q: '', region: null, cc: null };
    pkLoad(() => { if (SH && SH.key === 'location') draw(); }, true);
    return { icon: icon('pin'), title: WF.loc_t, base, draft,
      body() { const z = TZ ? zoneByName(draft.tz_name) : null;
        return `<p class="hint">${WF.loc_h}</p>
        <div class="callout"><span class="picked">${z ? flag(z.cc) : ''}<span><b>${z ? esc(placeName(z)) : esc(draft.tz_name || WF.not_set)}</b><br>${esc(z ? clockLine(z, draft.tz) : utc(draft.tz))}<br><small class="hint">${esc(draft.tz_name)}</small></span></span></div>
        ${pickerHtml(P, z)}
        <details class="adv"><summary>${WF.loc_exact}</summary><div class="grid2">
          ${field('lat', WF.lat, text('lat', draft.lat ?? '', { num: 1, unit: '°', err: this.err.lat }), WF.lat_h, this.err.lat)}
          ${field('lon', WF.lon, text('lon', draft.lon ?? '', { num: 1, unit: '°' }), WF.lon_h)}</div>
          <p class="hint">${WF.loc_pick}</p></details>`; },
      input(t) { if (t.id === 'pkq') { P.q = t.value; if (!pickerRefresh(P, TZ ? zoneByName(draft.tz_name) : null)) draw('pkq'); return; } if (t.id === 'lat' || t.id === 'lon') { const v = num(t.value); if (v != null && !isNaN(v)) draft[t.id] = v; sync(); } },
      click(bt) { const r = pickerClick(P, bt); if (r === 'nav') { draw(); return; } if (r) { Object.assign(draft, zoneBody(r)); P.q = ''; draw(); } },
      validate() { const E = {}; if (draft.lat != null && !(draft.lat >= -90 && draft.lat <= 90) || draft.lon != null && !(draft.lon >= -180 && draft.lon <= 180)) E.lat = WF.out_of_range; return E; },
      save(body) { return api('PUT', '/api/admin/bridge', body); } }; // PUT /api/admin/bridge {tz, tz_name, lat, lon}
  },
};

// the sign-in screen gets the same clock sky as the home that follows (no background jump on login)
setSky(clockSky());
// a valid session skips the sign-in (the settings call answers only with a session)
start();
// no station yet: the home asks the bridge every 10 s while it shows "No station yet" and no sheet is
// open, and reloads once a station has uploaded; with a station the poll asks nothing
// back on this tab: one check that the session still holds (a sign-in screen at once instead of a frozen page)
document.addEventListener('visibilitychange', () => { if (!document.hidden && !$('#loginForm') && API.settings.sta_ssid !== undefined) api('GET', '/api/admin/sessions').catch(() => {}); });
// While the home shows (no sheet open, the tab visible: FC.poll pauses a hidden tab), one /api/live every 30 s
// keeps the station tiles' state and the fauxcloud Wi-Fi count current, updated in place (no re-render, no focus jump).
// One request in flight like every poll here: the page-burst rule (lwIP holds an answer until it is acknowledged).
const healthOpen = () => FC.lazy(STATION_JS).then(() => { const g = $('#hgrid'); if (g) g.innerHTML = healthDetails(API.live); }, () => {});
FC.poll('home', { every: 30e3, due: Date.now() + 30e3, url: () => !SH && API.stations && $('#apN') ? '/api/live' : null, on: res => {
  const d = res.data, n = $('#apN');
  if (n && d.diag) { n.textContent = d.diag.ap_clients ?? 0; API.status.ap_clients = d.diag.ap_clients; }
  API.live = d; const h = $('#health');
  if (h) { h.querySelector('.states').outerHTML = healthRow(d, !!API.settings.mqtt_host); if (h.open) healthOpen(); }
  (d.stations || []).forEach(l => { const i = API.stations.findIndex(x => x.id === l.id), t = $(`[data-sheet="st${i}"] .state`);
    if (i < 0) return;
    Object.assign(API.stations[i], { online: l.online, age_s: l.age_s, uploads: l.uploads });
    if (t) t.innerHTML = stState(API.stations[i]); });
} });
FC.poll('pairing', { every: 10e3, url: () => !API.stations.length && !SH && $('#app .pair') ? '/api/bridge/status' : null,
  on: res => { if (res.data.ids && res.data.ids.length) refresh(); } });
