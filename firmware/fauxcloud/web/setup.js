// setup.js -- the setup face. Four steps, then
// POST /api/setup/apply; the page polls /api/setup/state every 2 s until joined or failed. Works inside the iOS
// captive mini-browser: no storage, cookies or new windows are needed.
// sky: setup has no station yet, so day or night comes from the phone's clock (the dashboard's fallback rule)
setSky(clockSky());
const COMMON = ['12345678', 'password', 'qwertyui', '11111111', 'password1', '87654321', 'iloveyou', 'admin123'];
const FAIL = { auth: 'the router rejected the password. Check it and try again.', no_ap: 'the network was not found. fauxcloud uses 2.4 GHz only.',
  dhcp: 'it joined, but the router gave fauxcloud no address.', timeout: 'there was no connection within a minute.', busy: 'fauxcloud was busy. Try again.' };

// ------------------------------------------------------------------ state
const S = { step: 0, net: null, manual: false, ssid: '', wpass: '', zone: null, pk: { q: '', region: null, cc: null }, admin: '', admin2: '', ap: '', show: {}, err: {},
  nets: [], scanning: true, mac4: '', canKeep: {}, keep: {} };  // keep.admin / keep.ap: setup again
const keepTog = (id, what) => S.canKeep[id] ? `<label class="toggle" for="keep-${id}"><input type="checkbox" role="switch" id="keep-${id}"${S.keep[id] ? ' checked' : ''}><span>Keep the current password<small>${what}</small></span></label>` : '';
const NETS = () => S.nets;

// ------------------------------------------------------------------ password strength (local, nothing leaves the page)
function strength(p) {
  if (!p) return 0;
  if (p.length < 8 || COMMON.includes(p.toLowerCase())) return 1;
  const kinds = [/[a-z]/, /[A-Z]/, /\d/, /[^A-Za-z0-9]/].filter(r => r.test(p)).length;
  return p.length >= 12 && kinds >= 3 ? 4 : kinds >= 2 ? 3 : 2;
}
function pwError(p, what) {
  if (!p) return `Enter ${what}.`;
  if (p.length < 8) return `Use at least 8 characters (now ${p.length}).`;
  if (COMMON.includes(p.toLowerCase())) return 'This password is on every list of common passwords. Pick another one.';
  return '';
}
function genPass() { // readable: no 0/O, 1/l
  const a = 'abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789', r = new Uint32Array(12);
  crypto.getRandomValues(r); return [...r].map(x => a[x % a.length]).join('').replace(/(.{4})(?=.)/g, '$1-');
}

// ------------------------------------------------------------------ small pieces
const lock = '<svg width="12" height="14" viewBox="0 0 12 14" aria-hidden="true"><rect x="1" y="6" width="10" height="7" rx="2" fill="currentColor"/><path d="M3.5 6V4.5a2.5 2.5 0 0 1 5 0V6" fill="none" stroke="currentColor" stroke-width="1.5"/></svg>';
const pwField = (id, label, value, auto, extra = '') => `
  <label class="f" for="${id}">${label}
    <span class="input${S.err[id] ? ' err' : ''}"><input id="${id}" type="${S.show[id] ? 'text' : 'password'}" value="${esc(value)}" autocomplete="${auto}" autocapitalize="off" spellcheck="false" aria-describedby="${id}-msg">
    <button type="button" class="ghost" data-show="${id}" aria-pressed="${!!S.show[id]}">${S.show[id] ? 'Hide' : 'Show'}</button>${extra}</span>
  </label>
  <p class="${S.err[id] ? 'error' : 'hint'}" id="${id}-msg">${S.err[id] || ''}</p>`;
const meter = (p, id) => `<div class="meter" id="${id}-meter" data-l="${strength(p)}" aria-hidden="true"><i></i><i></i><i></i><i></i></div>`;
const actions = (next, nextLabel = 'Next', back = true) => `<div class="actions">${back ? '<button type="button" class="btn quiet" data-go="back">Back</button>' : '<span></span>'}<button type="button" class="btn primary" data-go="${next}">${nextLabel}</button></div>`;
const apName = () => 'fauxcloud-' + (S.mac4 || 'XXXX');
const ssidOf = () => S.manual ? S.ssid : NETS()[S.net].ssid;

// ------------------------------------------------------------------ steps
const STEPS = [
  function wifi() {
    const list = NETS().map((n, i) => `<li><button type="button" data-net="${i}" aria-pressed="${!S.manual && S.net === i}">${rssiBars(n.rssi)}<span class="name">${esc(n.ssid)}</span>${n.secure ? lock : '<small>open</small>'}</button></li>`).join('');
    const picked = S.manual || S.net != null, secure = S.manual || (S.net != null && NETS()[S.net].secure);
    return `<p class="eyebrow">Step 1 of 4</p><h1>Your home Wi-Fi</h1>
      <p class="lead">This sets up fauxcloud’s own Wi-Fi for your console and joins fauxcloud to your home Wi-Fi, so you can open it from any device at home.</p>
      <div class="row"><span class="hint">Networks nearby (2.4 GHz only)</span><button type="button" class="link" id="rescan"${S.scanning ? ' disabled' : ''}>${S.scanning ? 'Scanning…' : 'Scan again'}</button></div>
      <ul class="nets" aria-label="Networks nearby">${list || (S.scanning ? '' : `<li class="hint">${S.err.scan || 'No networks found. Scan again, or enter the name below.'}</li>`)}</ul>
      ${S.manual ? `<label class="f" for="ssid">Network name<span class="input${S.err.ssid ? ' err' : ''}"><input id="ssid" value="${esc(S.ssid)}" autocomplete="off" autocapitalize="off" spellcheck="false"></span></label><p class="${S.err.ssid ? 'error' : 'hint'}">${S.err.ssid || 'Type it exactly, including capital letters.'}</p>`
        : '<button type="button" class="link" id="manual">My network is not listed</button>'}
      ${picked && secure ? pwField('wpass', 'Wi-Fi password', S.wpass, 'current-password') : ''}
      ${S.err.join ? `<p class="error" role="alert">${S.err.join}</p>` : ''}
      ${actions('next', 'Next', false)}`;
  },
  function zone() {
    const z = S.zone;
    return `<p class="eyebrow">Step 2 of 4</p><h1>Where is the station?</h1>
      <p class="lead">Pick the nearest city. It sets the clock for daily totals and the midnight rain reset, and tells the dashboard when the sun rises and sets.</p>
      ${z ? `<div class="picked">${flag(z.cc)}<span><b>${esc(placeName(z))}</b><span>${esc(clockLine(z))}</span></span></div>` : ''}
      ${z && z.tz === phoneTz && !S.pk.q ? '<p class="hint">Suggested from your phone’s time zone. Change it if the station is somewhere else.</p>' : ''}
      ${pickerHtml(S.pk, z)}
      ${S.err.city ? `<p class="error">${S.err.city}</p>` : ''}
      ${actions('next')}`;
  },
  function admin() {
    return `<p class="eyebrow">Step 3 of 4</p><h1>Admin password</h1>
      <p class="lead">You will use it to change settings later. The settings page opens only from your home network.</p>
      ${keepTog('admin', 'Switch off to set a new one.')}
      ${S.keep.admin ? '' : pwField('admin', 'New admin password', S.admin, 'new-password') + meter(S.admin, 'admin') + pwField('admin2', 'Type it again', S.admin2, 'new-password')}
      ${actions('next')}`;
  },
  function ap() {
    return `<p class="eyebrow">Step 4 of 4</p><h1>Wi-Fi for your weather station</h1>
      <p class="lead">Your weather console will connect to this network, <b>${esc(apName())}</b>. Set its password: you will enter it once when you pair the console.</p>
      ${keepTog('ap', 'Your console stays paired. A new password means pairing the console again.')}
      ${S.keep.ap ? '' : pwField('ap', 'Password for ' + esc(apName()), S.ap, 'new-password', '<button type="button" class="ghost" id="gen">Generate</button>') + meter(S.ap, 'ap') + '<p class="hint">Write it down or take a photo of it. Changing it later means pairing the console again.</p>'}
      ${actions('next')}`;
  },
  function review() {
    return `<p class="eyebrow">Check and apply</p><h1>Ready to go</h1>
      <dl class="review">
        <dt>Home Wi-Fi</dt><dd>${esc(ssidOf())}</dd><dd><button type="button" class="link" data-edit="0">Change</button></dd>
        <dt>Location</dt><dd>${esc(placeName(S.zone))}, ${esc(clockLine(S.zone).split(', summer')[0])}</dd><dd><button type="button" class="link" data-edit="1">Change</button></dd>
        <dt>Admin password</dt><dd>${S.keep.admin ? 'kept' : 'set'}</dd><dd><button type="button" class="link" data-edit="2">Change</button></dd>
        <dt>Station Wi-Fi</dt><dd>${esc(apName())}, password ${S.keep.ap ? 'kept' : 'set'}</dd><dd><button type="button" class="link" data-edit="3">Change</button></dd>
      </dl>
      <p class="hint">fauxcloud will try your home Wi-Fi for up to a minute. If it cannot join, nothing is lost: this setup page stays open and tells you why.</p>
      ${S.err.apply ? `<p class="error" role="alert">${esc(S.err.apply)}</p>` : ''}
      ${actions('apply', 'Apply')}`;
  },
];

// ------------------------------------------------------------------ API
function scan(again, round = 0) {
  S.scanning = true; delete S.err.scan; if (S.step === 0) render();
  FC.get('/api/setup/scan' + (again ? '?again=1' : ''), { etag: false }).then(r => {
    const j = r.data;
    // 20 rounds of "still scanning" (30 s), then the step says so and offers the scan again
    if (j.state === 'running' && round < 20) { setTimeout(() => scan(false, round + 1), 1500); return; }
    if (j.state === 'running') { S.scanning = false; S.err = { scan: 'The scan is taking long. Scan again, or enter the network name.' }; if (S.step === 0) render(); return; }
    const keep = S.net != null && NETS()[S.net] ? NETS()[S.net].ssid : null;
    S.nets = (j.networks || []).map(n => ({ ssid: n.ssid, rssi: n.rssi, secure: !n.open }));
    const want = keep != null ? keep : S.ssid;  // keep the choice, or preselect the network tried before
    const idx = want ? S.nets.findIndex(n => n.ssid === want) : -1;
    S.net = idx < 0 ? null : idx;
    S.scanning = false; if (S.step === 0) render(document.activeElement && document.activeElement.id);
  }, () => { S.scanning = false; if (S.step === 0) render(); });
}
let timer = null, t0 = 0;
function apply() {
  // a kept password is simply not sent: the ESP keeps the stored one
  const body = Object.assign(zoneBody(S.zone), { sta_ssid: ssidOf(), sta_pass: S.wpass }, S.keep.ap ? {} : { ap_pass: S.ap });
  joining();  // feedback first: the hash below takes a moment on a phone
  setTimeout(() => {
  // the admin password never leaves the phone: only salt + PBKDF2 result are sent
  if (!S.keep.admin) { const cred = FCX.credential(S.admin, 10000);
    Object.assign(body, { admin_salt: cred.salt, admin_h: cred.h, admin_iters: cred.iters }); }
  FC.get('/api/setup/apply', { method: 'POST', etag: false, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }).then(r => {
    const e = r.data.errors || [];
    if (e.length) { clearInterval(timer); S.err = { apply: e.join('; ') }; S.step = 4; render(); return; }
    poll();
  }, () => { clearInterval(timer); S.err = { apply: 'No answer from fauxcloud. Check that this phone is still on the fauxcloud setup Wi-Fi.' }; S.step = 4; render(); });
  }, 30);
}
function poll() {
  // after 90 s the page says it is taking long; after 3 min it stops asking and says where to look
  const late = Date.now() - t0, m = $('#joinMsg');
  if (late > 180e3) {
    clearInterval(timer);
    if (m) m.innerHTML = 'No answer from fauxcloud for 3 minutes. If this phone left the fauxcloud setup Wi-Fi, fauxcloud may already be on your home network: open <b>http://fauxcloud.local</b> from a device at home. Otherwise reload this page.';
    return;
  }
  if (late > 90e3 && m) m.textContent = 'Taking long. Keep this page open; check that this phone is still on the fauxcloud setup Wi-Fi.';
  FC.get('/api/setup/state', { etag: false }).then(r => {
    const s = r.data.state || '';
    if (s === 'joined') { clearInterval(timer); success(r.data); return; }
    if (s.indexOf('failed:') === 0) { clearInterval(timer); failed(s.slice(7)); return; }
    if (s === 'joining' && Date.now() - t0 > 3000) { const m = $('#joinMsg'); if (m) m.textContent = 'Connected to the router, waiting for an address…'; }
    setTimeout(poll, 2000);
  }, () => setTimeout(poll, 2000)); // the phone may lose the AP for a moment while the radio joins the home Wi-Fi
}

// ------------------------------------------------------------------ joining, success, failure
function joining() {
  const C = 2 * Math.PI * 42;
  $('#card').innerHTML = `<div class="wait"><svg class="ring" viewBox="0 0 96 96" aria-hidden="true"><circle class="track" cx="48" cy="48" r="42" fill="none" stroke-width="6"/><circle class="arc" id="arc" cx="48" cy="48" r="42" fill="none" stroke-width="6" stroke-linecap="round" stroke-dasharray="${C}" stroke-dashoffset="${C}" transform="rotate(-90 48 48)"/></svg>
    <h1>Joining ${esc(ssidOf())}</h1><p class="lead" id="joinMsg">This takes up to a minute. Keep this page open.</p></div>`;
  dots(5);
  t0 = Date.now(); clearInterval(timer);
  timer = setInterval(() => { const a = $('#arc'); if (a) a.style.strokeDashoffset = C * (1 - Math.min(1, (Date.now() - t0) / 60e3)); }, 1000);
}
// fauxcloud's own Wi-Fi is 192.168.77.0/24; a home network overlaps it when the two agree under the wider of the two masks
// (a 192.168.0.0/16 network holds 192.168.77.x); no mask (an older image): the /24 test on the address alone
function ipNum(s) {  // "a.b.c.d" -> its 32-bit value, null when it is not one
  const p = String(s || '').split('.');
  return p.length === 4 && p.every(x => x !== '' && x >= 0 && x < 256) ? p.reduce((a, x) => a * 256 + +x, 0) : null;
}
function subnetClash(ip, mask) {
  const a = ipNum(ip), k = ipNum(mask) || null, w = Math.min(k == null ? 0xFFFFFF00 : k, 0xFFFFFF00);  // 0.0.0.0: no mask
  return a != null && ((a & w) >>> 0) === ((0xC0A84D00 & w) >>> 0);
}
// a home network wider than /24 as "network/prefix" (192.168.0.0/16): its own advice (the mask, not the address); "" for
// a /24 or narrower one, or no valid mask
function wideNet(ip, mask) {
  const a = ipNum(ip), k = ipNum(mask), b = k == null ? 0 : Math.log2(4294967296 - k);
  if (a == null || !Number.isInteger(b) || b <= 8 || b >= 32) return '';  // 0.0.0.0 is no mask
  const n = (a & k) >>> 0;
  return [24, 16, 8, 0].map(s => n >>> s & 255).join('.') + '/' + (32 - b);
}
function success(j) {
  S.step = 6;  // past the steps, so a scan answering later (a reload during the join) cannot redraw step 1
  const host = (j.hostname || 'fauxcloud') + '.local', ssid = j.sta_ssid || ssidOf();
  // a home network that holds fauxcloud's own 192.168.77.x: the warning stands where the address would be; a network wider
  // than /24 gets the subnet-mask advice (a new address inside it would still overlap)
  const clash = subnetClash(j.sta_ip, j.sta_mask), net = clash ? wideNet(j.sta_ip, j.sta_mask) : '';
  $('#card').innerHTML = `<div class="done">
    <span class="badge"><span class="dot"></span>Joined ${esc(ssid)}</span>
    <h1>fauxcloud is on your home network</h1>
    ${net ? `<p class="error" role="alert">Your home network (${esc(net)}) includes 192.168.77.x, the address range of fauxcloud’s own Wi-Fi, so the dashboard will not open from your home network. Set your router’s subnet mask to 255.255.255.0, then restart fauxcloud from Settings (or unplug it for a few seconds): it joins again with a new address.</p>`
      : clash ? '<p class="error" role="alert">Your home network also uses 192.168.77.x, the same as fauxcloud’s own Wi-Fi, so the dashboard will not open from your home network. Change your router’s network address (for example to 192.168.1.x), then unplug fauxcloud for a few seconds: it joins again with a new address.</p>'
      : `<div class="address"><b>${esc(host)}</b>${j.sta_ip ? `<span>or http://${esc(j.sta_ip)}</span>` : ''}</div>`}
    <p class="lead">This setup network is closing now. Next:</p>
    <ol class="next">
      <li><b>Reconnect your phone</b> to ${esc(ssid)}.</li>
      <li><b>Open ${esc(host)}</b> and sign in with your admin password.</li>
      <li><b>Connect Home Assistant</b> in Settings (MQTT): it needs your home network, so it is done there, not here.</li>
      ${S.keep.ap ? `<li><b>Your console stays paired</b> with ${esc(j.ap_ssid || apName())}: its password did not change.</li>` : `<li><b>Now pair the console:</b> plug in its adapter, hold BARO for 3 seconds until AP shows, join the console’s WWS network with your phone, open 192.168.1.1 and choose <b>${esc(j.ap_ssid || apName())}</b> with the password you set. ${WU_STEP}</li>`}
    </ol>
    ${clash ? '' : `<p class="hint">If ${esc(host)} does not open on your device, use the address with numbers above.</p>`}
    ${S.keep.admin ? '' : '<p class="hint">Your admin password was hashed on this phone; only the hash was sent.</p>'}</div>`;
  dots(6);
}
function failed(code) {
  S.err = { join: `fauxcloud could not join ${esc(ssidOf())}: ${own(FAIL, code) || 'it could not connect.'}` };
  if (code === 'auth') S.err.wpass = 'Rejected by the router.';
  S.step = 0; render();
}

// ------------------------------------------------------------------ validation per step
function valid(step) {
  const e = {};
  if (step === 0) {
    if (S.net == null && !S.manual) e.join = 'Choose your home network, or enter its name.';
    if (S.manual && !S.ssid.trim()) e.ssid = 'Enter the network name.';
    const secure = S.manual || (S.net != null && NETS()[S.net].secure);
    if (secure && (S.manual || S.net != null) && S.wpass.length < 8) e.wpass = S.wpass ? 'Wi-Fi passwords have at least 8 characters. Check it.' : 'Enter the Wi-Fi password.';
  }
  if (step === 1 && !S.zone) e.city = 'Pick a city or a country.';
  if (step === 2 && !S.keep.admin) { const m = pwError(S.admin, 'a password'); if (m) e.admin = m; else if (S.admin !== S.admin2) e.admin2 = 'The two passwords are different.'; }
  if (step === 3 && !S.keep.ap) { const m = pwError(S.ap, 'a password'); if (m) e.ap = m;
    else if (!S.keep.admin && S.ap === S.admin) e.ap = 'Use a different password from the admin one: the console Wi-Fi password is typed on a device you hand around.'; }
  S.err = e; return !Object.keys(e).length;
}

// ------------------------------------------------------------------ render + events
function dots(n) { [...$('#dots').children].forEach((d, i) => d.className = i < n ? 'done' : i === n ? 'on' : ''); }
// What a screen reader hears -- the step title when the step changes, then the first error; never the
// whole card on every keystroke (the card is not a live region)
function say(t) { if (t && $('#say').textContent !== t) $('#say').textContent = t; }
function render(focusId) {
  // never redraw the input the user is typing in: render() keeps its caret
  $('#card').innerHTML = STEPS[S.step]();
  dots(S.step);
  const h = $('#card h1'), e = Object.values(S.err)[0];
  if (S.step !== render.step) { render.step = S.step; say(h && h.textContent); }
  if (e) say(e);
  const f = focusId && document.getElementById(focusId);
  if (f) { f.focus(); const v = f.value; f.value = ''; f.value = v; }
}
$('#card').addEventListener('click', e => {
  const b = e.target.closest('button'); if (!b) return;
  if (b.dataset.net != null) { S.net = +b.dataset.net; S.manual = false; S.err = {}; render('wpass'); return; }
  const pk = pickerClick(S.pk, b); if (pk === 'nav') { render(); return; } if (pk) { S.zone = pk; S.err = {}; S.pk = { q: '', region: pk.region, cc: ZONES.filter(z => z.cc === pk.cc && z.region === pk.region).length > 1 ? pk.cc : null }; render(); return; }
  if (b.dataset.show) { S.show[b.dataset.show] = !S.show[b.dataset.show]; render(b.dataset.show); return; }
  if (b.dataset.edit != null) { S.step = +b.dataset.edit; S.err = {}; render(); return; }
  if (b.id === 'manual') { S.manual = true; S.net = null; render('ssid'); return; }
  if (b.id === 'rescan') { scan(true); return; }
  if (b.id === 'gen') { S.ap = genPass(); S.show.ap = true; render('ap'); return; }
  if (b.dataset.go === 'back') { S.err = {}; S.step = Math.max(0, S.step - 1); render(); return; }
  if (b.dataset.go === 'next') { if (valid(S.step)) { S.step++; if (S.step === 1) pkLoad(() => { if (!S.zone) S.zone = phoneZone; if (S.step === 1) render(); }, true); } render(Object.keys(S.err)[0]); return; }
  if (b.dataset.go === 'apply') apply();
});
$('#card').addEventListener('change', e => { const t = e.target; if (t.id && t.id.indexOf('keep-') === 0) { S.keep[t.id.slice(5)] = t.checked; S.err = {}; render(t.id); } });
$('#card').addEventListener('input', e => {
  const t = e.target; if (!t.id) return;
  if (t.id === 'pkq') { S.pk.q = t.value; if (!pickerRefresh(S.pk, S.zone)) render('pkq'); return; }
  const map = { ssid: 'ssid', wpass: 'wpass', admin: 'admin', admin2: 'admin2', ap: 'ap' };
  if (!map[t.id]) return;
  S[map[t.id]] = t.value; if (S.err[t.id]) delete S.err[t.id];
  if (['admin', 'ap'].includes(t.id)) {  // live strength meter, updated in place
    const m = document.getElementById(t.id + '-meter');  // by id (the next element is the hint)
    if (m) m.dataset.l = strength(t.value); else render(t.id);
  }
});
$('#card').addEventListener('keydown', e => { if (e.key === 'Enter' && e.target.tagName === 'INPUT') { const n = $('#card [data-go="next"], #card [data-go="apply"]'); if (n) n.click(); } });
// The phone's clock seeds fauxcloud's, once, before the first state read (FC.get runs one at a time)
FC.get('/api/setup/time', { method: 'POST', etag: false, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ t: Date.now() }) }).catch(() => {});
FC.get('/api/setup/state', { etag: false }).then(r => {
  const j = r.data;
  S.mac4 = (j.ap_ssid || '').split('-').pop();
  if (j.sta_ssid && !S.ssid) { S.ssid = j.sta_ssid; }
  // setup again: the stored passwords can stay (checked by default); a first run has none
  S.canKeep = { admin: !!j.keep_admin, ap: !!j.keep_ap }; S.keep = Object.assign({}, S.canKeep);
  if (j.state === 'joined') success(j); else render();
}, () => render());
scan(false);
// the time-zone list loads while the user is on step 1, after the scan (not urgent)
pkLoad(() => { if (!S.zone) S.zone = phoneZone; });
render();
