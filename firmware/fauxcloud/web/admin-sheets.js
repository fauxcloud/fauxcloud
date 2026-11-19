'use strict';
// admin-sheets.js -- the admin sheets a visit seldom opens -- MQTT,
// Name on your network, Password and devices, Maintenance -- loaded on first use through FC.lazy() by adminface.js
// (openSheet), like admin-station.js. Same global scope as admin.js: it adds SHEETS.<name> and uses the helpers defined
// there (field, text, draft helpers, api, withProof, ...). Home Wi-Fi, the station's Wi-Fi and Location stay in admin.js.
// the four sheets' words (a lazy script: its pack, if any, is in before it runs)
const WY = FC.w('admin-sheets', {
  // MQTT
  m_ok: 'Connected. Save to keep these settings.', m_failed: 'Failed: {d} Check the address and that the broker is running.',
  m_noans: 'No answer from {h}:{p}. Check the address and that the broker is running.',
  m_up: 'Connected', m_down: 'Not connected', m_none: 'Not set up yet',
  m_intro: 'fauxcloud publishes each station to an MQTT broker. Optional: the dashboard works without it. For Home Assistant, install its <b>Mosquitto broker</b> add-on (Settings → Add-ons) and enter your Home Assistant address and a Home Assistant user here. Any other MQTT broker works too.',
  m_host: 'Broker address', m_host_ph: 'e.g. 192.168.1.10 or homeassistant.local', m_port: 'Port', m_port_h: 'Usually 1883.',
  m_user: 'User name', m_pass: 'Password', m_pass_ph: 'Saved, type to change', m_pass_h: 'Leave empty to keep the saved password.',
  m_test: 'Test connection', m_need_host: 'Enter the broker address first.', m_testing: 'Testing…', no_answer: 'No answer from fauxcloud.',
  m_e_host: 'An address like 192.168.1.10 or homeassistant.local, without http://.', m_e_port: 'A port is a number from 1 to 65535.',
  // Name on your network
  n_title: 'Name on your network', n_dev: 'Device name', n_opens: 'The dashboard opens at http://{h}.local. Letters, digits and dashes.',
  n_after: 'After saving, open the new address. The old one stops working within a minute.',
  n_e_host: 'Use lowercase letters, digits and dashes, not at the start or end.',
  // Password and devices
  s_title: 'Password and devices', s_admin: 'Admin password', s_cur: 'Current password', s_new: 'New password', s_new_h: 'At least 8 characters.',
  s_again: 'Type it again', s_change: 'Change password', s_devices: 'Signed-in devices',
  s_devices_h: 'Up to 4 at a time. A device stays signed in while it is used. A restart signs everyone out.',
  s_line: '{ua}, {ip}, last used {last}', s_this: 'this device', s_out: 'Sign out', s_all_out: 'Sign out everywhere',
  s_e_short: 'Use at least 8 characters.', s_e_diff: 'The two new passwords are different.', s_e_cur: 'Enter the current password.',
  s_checking: 'Checking…', s_changed: 'Password changed. Other devices were signed out.', s_e_wrong: 'This is not the current password.',
  // Maintenance
  x_title: 'Maintenance',
  x_rolled: '<b>The last update was rolled back.</b>The new firmware did not start properly, so fauxcloud went back to the version it ran before.',
  x_fw: 'Firmware', x_up: 'Running for', x_last: 'Last restart', x_restarts: '{r}, {n} so far', x_mem: 'Free memory', x_id: 'Device ID',
  x_upd: 'Firmware update',
  x_upd_h: 'Choose a fauxcloud <code>.bin</code> file. fauxcloud checks it, installs it next to the running version and restarts; if the new version does not start properly, it goes back by itself.',
  x_choose: 'Choose a file', x_install: 'Install', x_restart: 'Restart',
  x_restart_h: 'About 10 seconds. Settings and stations are kept; everyone is signed out.', x_restart_btn: 'Restart',
  x_bug: 'Bug report', x_logv: 'Log every upload and page request',
  x_logv_h: 'Only when asked for a bug report: the log then covers only the last few minutes.',
  x_bl_h: 'The boot log shows station ids and addresses; check it before you post it.', x_bl: 'Download the boot log',
  x_crash: '<b>Crash data available.</b>{reason} (task {task}). Contains your passwords. Never share this file — paste the decoded backtrace from tools/decode-coredump.ps1. Downloading it clears it.',
  x_cd_pw: 'Your admin password, to download it', x_download: 'Download', x_clear: 'Clear', x_reset: 'Factory reset',
  x_reset_h: 'Erases everything: Wi-Fi, passwords, Home Assistant settings, stations and their settings. Readings already in Home Assistant stay there.',
  x_reset_type: 'Type RESET to confirm', x_erase: 'Erase everything',
  x_file: '{name}: {kb} KB of {max} KB available', x_file_big: '{name}: {kb} KB of {max} KB available, too large',
  x_uploading: 'Uploading… {pct} %', x_installed: 'Installed {v}. Restarting… this page reconnects by itself.',
  x_unexpected: 'unexpected answer', x_not_inst: 'Not installed: {e}. The running version stays.', x_failed: 'failed',
  x_broke: 'The upload broke off. The running version stays; try again.', x_busy: 'An update is already running. Wait for it to finish.',
  x_nomem: 'Not enough free memory for an update. Restart fauxcloud and try again.', x_cd_gone: 'The crash data was already cleared.',
  x_cd_retry: 'Not downloaded. Try again.', x_cd_done: 'Downloaded. The crash data on fauxcloud is cleared.',
  x_rebooting: 'Restarting… sign in again in a few seconds.',
  x_erased: 'Erased. fauxcloud is restarting into setup mode. Join <b>{ap}</b> with your phone (setup password: <b>fauxcloud</b>) to set it up again.',
});
// ---- Home Assistant (MQTT): partial PUT of mqtt_* only; empty password = unchanged for the same broker, none for a new one
SHEETS.mqtt = function () {
  const s = API.settings, st = API.status, base = { mqtt_host: s.mqtt_host, mqtt_port: s.mqtt_port, mqtt_user: s.mqtt_user, mqtt_pass: '' }, draft = clone(base); let test = '';
  const poll = j => {
    if (j.state === 'running') { setTimeout(() => api('GET', '/api/admin/mqtt/test').then(poll), 1000); return; }
    test = j.state === 'ok' ? `<span class="okmsg">${WY.m_ok}</span>` : `<span class="error">${esc(FC.t(j.detail ? WY.m_failed : WY.m_noans, { d: j.detail, h: draft.mqtt_host, p: draft.mqtt_port }))}</span>`;
    if (SH && SH.key === 'mqtt') draw();
  };
  // the saved password stays only with the saved broker (a new host or port gets none), so the "keep it" words go once they differ
  const keep = () => !!s.mqtt_pass && draft.mqtt_host === s.mqtt_host && +draft.mqtt_port === +s.mqtt_port;
  // the hint exists only while it holds (a hidden one would still be read out through aria-describedby)
  const passUi = () => { const p = $('#mpass'), h = $('#mpass-msg'), k = keep(); if (!p) return; p.placeholder = k ? WY.m_pass_ph : '';
    if (k && !h) p.closest('.fld').insertAdjacentHTML('beforeend', `<p class="hint" id="mpass-msg">${WY.m_pass_h}</p>`); if (!k && h) h.remove(); };
  return { icon: icon('house'), title: 'MQTT', base, draft,
    body() { const E = this.err;
      return `<p class="lead"><span class="state"><span class="dot ${st.mqtt_up ? '' : 'bad'}"></span>${st.mqtt_up ? WY.m_up : s.mqtt_host ? WY.m_down : WY.m_none}</span></p>
      <p class="hint">${WY.m_intro}</p>
      <div class="grid2">${field('mhost', WY.m_host, text('mhost', draft.mqtt_host, { ph: WY.m_host_ph, err: E.mhost }), '', E.mhost)}
      ${field('mport', WY.m_port, text('mport', draft.mqtt_port, { num: 1, err: E.mport }), WY.m_port_h, E.mport)}
      ${field('muser', WY.m_user, text('muser', draft.mqtt_user, { ac: 'username' }))}
      ${field('mpass', WY.m_pass, text('mpass', draft.mqtt_pass, { type: 'password', ac: 'new-password', ph: keep() ? WY.m_pass_ph : '' }), keep() ? WY.m_pass_h : '')}</div>
      <div class="row"><button type="button" class="btn quiet" id="mtest">${WY.m_test}</button><span role="status">${test}</span></div>`; },
    input(t) { const m = { mhost: 'mqtt_host', muser: 'mqtt_user', mpass: 'mqtt_pass' }; if (m[t.id]) draft[m[t.id]] = t.value; if (t.id === 'mport') draft.mqtt_port = num(t.value) ?? ''; passUi(); sync(); },
    click(b) { if (b.id !== 'mtest') return; if (!draft.mqtt_host) { test = `<span class="error">${WY.m_need_host}</span>`; draw(); return; }
      test = `<span class="hint">${WY.m_testing}</span>`; draw();
      api('POST', '/api/admin/mqtt/test', draft).then(poll, () => { test = `<span class="error">${WY.no_answer}</span>`; draw(); }); },
    validate() { const E = {}; if (draft.mqtt_host && !/^[A-Za-z0-9.-]+$/.test(draft.mqtt_host)) E.mhost = WY.m_e_host; if (!(draft.mqtt_port > 0 && draft.mqtt_port < 65536)) E.mport = WY.m_e_port; return E; },
    save(body) { if ('mqtt_pass' in body && !body.mqtt_pass) delete body.mqtt_pass; return api('PUT', '/api/admin/settings', body); } };
};

SHEETS.name = function () {
  const s = API.settings, base = { hostname: s.hostname }, draft = clone(base);
  return { icon: icon('wifi'), title: WY.n_title, base, draft,
    body() { const E = this.err; return `${field('host', WY.n_dev, text('host', draft.hostname, { unit: '.local', err: E.host }), FC.t(WY.n_opens, { h: esc(draft.hostname || 'fauxcloud') }), E.host)}
      <p class="hint">${WY.n_after}</p>`; },
    input(t) { if (t.id === 'host') { draft.hostname = t.value; const h = $('#host-msg'); if (h && !this.err.host) h.textContent = FC.t(WY.n_opens, { h: t.value || 'fauxcloud' }); sync(); } },
    validate() { return /^[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?$/.test(draft.hostname) ? {} : { host: WY.n_e_host }; },
    save(body) { return api('PUT', '/api/admin/settings', body); } };
};
SHEETS.security = function () {
  let res = '';
  return { icon: icon('shield'), title: WY.s_title,
    body() { const E = this.err;
      return `<h3>${WY.s_admin}</h3><div class="grid2">${field('pcur', WY.s_cur, text('pcur', '', { type: 'password', ac: 'current-password', err: E.pcur }), '', E.pcur)}<span></span>
        ${field('pnew', WY.s_new, text('pnew', '', { type: 'password', ac: 'new-password', err: E.pnew }), WY.s_new_h, E.pnew)}
        ${field('pnew2', WY.s_again, text('pnew2', '', { type: 'password', ac: 'new-password', err: E.pnew2 }), '', E.pnew2)}</div>
        <div class="row end"><span role="status">${res}</span><button type="button" class="btn primary" id="pwsave">${WY.s_change}</button></div>
        <h3 style="margin-top:8px">${WY.s_devices}</h3><p class="hint">${WY.s_devices_h}</p>
        <ul class="sessions">${API.sessions.map(x => `<li><span class="sp"><b>${esc(x.who)}</b><br><small>${FC.t(WY.s_line, { ua: esc(x.ua), ip: esc(x.ip), last: esc(x.last) })}</small></span>${x.cur ? `<small>${WY.s_this}</small>` : `<button type="button" class="link" data-sout="${esc(x.id)}">${WY.s_out}</button>`}</li>`).join('')}</ul>
        <div class="row"><button type="button" class="btn quiet" id="allout">${WY.s_all_out}</button></div>`; },
    click(b) {
      if (b.id === 'pwsave') { const c = $('#pcur').value, a = $('#pnew').value, a2 = $('#pnew2').value, E = {};
        if (a.length < 8) E.pnew = WY.s_e_short; else if (a !== a2) E.pnew2 = WY.s_e_diff;
        if (!c) E.pcur = WY.s_e_cur;
        this.err = E; res = ''; if (Object.keys(E).length) { draw(); return; }
        b.disabled = true; res = `<span class="hint">${WY.s_checking}</span>`; draw();
        // the new password never leaves the browser either: only a new salt + PBKDF2 result go out
        const cred = FCX.credential(a, 10000);
        withProof(c, { salt: cred.salt, h: cred.h, iters: cred.iters }, '/api/admin/password', 'PUT').then(j => {
          if (j.ok) { res = `<span class="okmsg">${WY.s_changed}</span>`; this.err = {}; loadSessions().then(l => { API.sessions = l; if (SH && SH.key === 'security') draw(); }); }
          else { res = ''; this.err = { pcur: esc(errs(j)) || (j.error ? failTry(j) : WY.s_e_wrong) }; }
          draw(); }, () => { res = `<span class="error">${WY.no_answer}</span>`; draw(); }); }
      if (b.dataset.sout) { api('POST', '/api/admin/sessions/revoke', { id: b.dataset.sout }).then(() => loadSessions()).then(l => { API.sessions = l; draw(); }, noAnswer()); }
      if (b.id === 'allout') { api('POST', '/api/admin/sessions/revoke-all', {}).finally(() => { SH = null; $('#scrim').hidden = $('#sheet').hidden = true; document.body.style.overflow = ''; pageInert(false); login(); }); }
    } };
};
SHEETS.maint = function () {
  const st = API.status, setupName = 'fauxcloud-setup-' + API.mac4, cd = { asking: false, pw: '', err: '' };
  return { icon: icon('wrench'), title: WY.x_title,
    body() { const up = dur(st.uptime_s), c = st.crash;
      // Every section reads the same way -- the words first, its buttons below them, left;
      // one order, top to bottom -- what runs now, its update, a restart, what a bug report
      // needs (the detail switch, the log, the crash data), and the one destructive action last
      return `${st.ota === 'rolled_back' ? `<div class="callout warn">${WY.x_rolled}</div>` : ''}
      <dl class="kv"><dt>${WY.x_fw}</dt><dd>fauxcloud ${esc(st.build || st.fw_version)}</dd><dt>${WY.x_up}</dt><dd>${up}</dd><dt>${WY.x_last}</dt><dd>${FC.t(WY.x_restarts, { r: esc(st.reset), n: st.restarts })}</dd><dt>${WY.x_mem}</dt><dd>${Math.round(st.heap_free / 1024)} KB</dd><dt>${WY.x_id}</dt><dd>${esc(API.mac4)}</dd></dl>
      <h3>${WY.x_upd}</h3><p class="hint">${WY.x_upd_h}</p>
      <div class="row"><label class="btn quiet" for="otaFile">${WY.x_choose}</label><input type="file" id="otaFile" accept=".bin,application/octet-stream" hidden><button type="button" class="btn primary" id="otaGo" disabled>${WY.x_install}</button></div>
      <p class="hint" id="otaMsg" role="status"></p><progress id="otaBar" max="100" value="0" hidden style="width:100%"></progress>
      <h3>${WY.x_restart}</h3><p class="hint" id="rebootMsg">${WY.x_restart_h}</p><div class="row"><button type="button" class="btn quiet" id="reboot">${WY.x_restart_btn}</button></div>
      <h3>${WY.x_bug}</h3>${tog('logv', API.bridge.log_verbose, WY.x_logv, WY.x_logv_h)}
      <p class="hint">${WY.x_bl_h}</p><div class="row"><button type="button" class="btn quiet" id="blGet">${WY.x_bl}</button></div>
      ${c ? `<div class="callout warn">${FC.t(WY.x_crash, { reason: esc(c.reason), task: esc(c.task) })}
        ${cd.asking ? field('cdpw', WY.x_cd_pw, text('cdpw', cd.pw, { type: 'password', ac: 'current-password', err: cd.err }), '', cd.err) : ''}<div class="row"><button type="button" class="btn quiet" id="cdGet" ${A.lock ? 'disabled' : ''}>${WY.x_download}</button><button type="button" class="btn quiet" id="cdDel">${WY.x_clear}</button></div></div>` : ''}
      <h3>${WY.x_reset}</h3><p class="hint">${WY.x_reset_h}</p>
      ${field('rtype', WY.x_reset_type, text('rtype', ''))}
      <div class="row"><button type="button" class="btn danger" id="reset" disabled>${WY.x_erase}</button></div>`; },
    input(t) { if (t.id === 'rtype') $('#reset').disabled = t.value !== 'RESET'; if (t.id === 'cdpw') cd.pw = t.value; },
    change(t) { if (t.id === 'logv') { const on = t.checked; // the boot log's chatter lines (NVS, no restart)
        api('PUT', '/api/admin/bridge', { log_verbose: on }).then(j => { if (errs(j)) { t.checked = !on; toast(errs(j)); } else { API.bridge.log_verbose = on; } },
          () => { t.checked = !on; noAnswer()(); }); return; }
      if (t.id !== 'otaFile') return; const f = t.files[0], big = f && f.size > st.slot;
      $('#otaGo').disabled = !f || big;
      $('#otaMsg').textContent = f ? FC.t(big ? WY.x_file_big : WY.x_file, { name: f.name, kb: (f.size / 1024).toFixed(0), max: (st.slot / 1024).toFixed(0) }) : ''; },
    click(b) {
      if (b.id === 'otaGo') { // fetch has no upload progress: XMLHttpRequest, outside the request queue (one long upload)
        const f = $('#otaFile').files[0], msg = $('#otaMsg'), bar = $('#otaBar'), x = new XMLHttpRequest();
        b.disabled = $('#otaFile').disabled = true; bar.hidden = false;
        x.open('POST', '/api/admin/ota'); x.setRequestHeader('X-Fauxcloud', '1'); x.setRequestHeader('Content-Type', 'application/octet-stream');
        x.upload.onprogress = e => { if (e.lengthComputable) { bar.value = e.loaded / e.total * 100; msg.textContent = FC.t(WY.x_uploading, { pct: Math.round(bar.value) }); } };
        x.onload = () => { if (x.status === 401) { signedOut(); return; } let j; try { j = JSON.parse(x.responseText); } catch (e) { j = { error: WY.x_unexpected }; }
          if (j.ok) { msg.textContent = FC.t(WY.x_installed, { v: j.version }); setTimeout(() => location.reload(), 15000); }
          else { msg.textContent = FC.t(WY.x_not_inst, { e: j.error || WY.x_failed }); b.disabled = $('#otaFile').disabled = false; bar.hidden = true; } };
        // a refusal closes the connection before the browser can read why: ask the bridge what happened
        x.onerror = () => { const early = bar.value < 25; b.disabled = $('#otaFile').disabled = false; bar.hidden = true;
          msg.textContent = WY.x_broke;
          FC.get('/api/bridge/status', { etag: false, front: true }).then(r => {
            if (r.data.ota) msg.textContent = WY.x_busy;
            else if (early) msg.textContent = WY.x_nomem;
          }, () => {}); };
        x.send(f); return; }
      if (b.id === 'cdGet') { // POST {nonce, proof}: the dump holds the passwords; it is cleared once sent
        if (!cd.asking) { cd.asking = true; draw('cdpw'); return; }
        b.disabled = true; withProof(cd.pw, {}, '/api/admin/coredump', 'POST', { blob: true }).then(bl => { cd.pw = '';
          if (!(bl instanceof Blob)) { const e = bl && bl.error; // only a refused proof counts as a try
            if (e === 'no crash data') { st.crash = null; draw(); toast(WY.x_cd_gone); return; }
            cd.err = ['wrong password', 'locked', 'busy'].includes(e) ? failTry(bl) : WY.x_cd_retry; draw('cdpw'); return; }
          A.tries = 5; const a = document.createElement('a'); a.href = URL.createObjectURL(bl); a.download = 'fauxcloud-coredump.bin'; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000);
          st.crash = null; draw(); toast(WY.x_cd_done); }, () => { b.disabled = false; cd.err = WY.no_answer; draw('cdpw'); }); return; }
      if (b.id === 'cdDel') { api('DELETE', '/api/admin/coredump').then(j => { if (j.ok) { st.crash = null; draw(); } else toast(errs(j)); }, noAnswer(b)); return; }
      if (b.id === 'blGet') { b.disabled = true; api('GET', '/api/admin/bootlog', undefined, { blob: true }).then(bl => { b.disabled = false; if (!(bl instanceof Blob)) return; // a fetch with the header, never a link
          const a = document.createElement('a'); a.href = URL.createObjectURL(bl); a.download = 'fauxcloud-bootlog.txt'; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000); }, noAnswer(b)); return; }
      if (b.id === 'reboot') { b.disabled = true; api('POST', '/api/admin/reboot', {}).then(() => { $('#rebootMsg').textContent = WY.x_rebooting; setTimeout(() => location.reload(), 12000); }, noAnswer(b)); }
      if (b.id === 'reset') { b.disabled = true; api('POST', '/api/admin/factory-reset', {}).then(() => {
        $('#shBody').innerHTML = `<p class="lead">${FC.t(WY.x_erased, { ap: esc(setupName) })}</p>`; }, noAnswer(b)); }
    } };
};
