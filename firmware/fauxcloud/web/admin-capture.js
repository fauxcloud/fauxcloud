'use strict';
// admin-capture.js -- lazy: the admin "Listen to a new console" sheet (opened from the fauxcloud Wi-Fi sheet). The board keeps only
// its latest records; this file keeps the whole history for the page's life (module level: closing and reopening the
// sheet keeps it) and builds the report the Console report form takes. One poll chain of its own (GET with `since`,
// at once again while `more`, else every 3 s): it keeps polling in a hidden tab, because the console is usually paired
// from the same phone (the documented exception to the pause-when-hidden rule). The AP's name is on the sheet and its
// password through the existing reveal route (the AP sheet's flow, here again).
(() => {
  const W = FC.w('admin-capture', {
    title: 'Listen to a new console',
    lead: 'For a console that never uploads: fauxcloud listens to everything it sends and makes a report you can paste into a Console report on GitHub.',
    c_ha: 'fauxcloud’s sensors show as unavailable in Home Assistant.', c_fwd: 'Online services pause.', c_metar: 'Airport weather pauses.',
    c_save: 'Readings are not saved.', c_time: 'It runs until you press Stop. If this sheet stays closed for an hour, it stops by itself.',
    c_open: 'Keep this page open; lines that arrive while it cannot reach fauxcloud may be lost.',
    notime: 'fauxcloud has no time yet. A console usually sends nothing until it gets the time.',
    ap: 'Start listening, then connect the console to this Wi-Fi:',
    // the two things the console's setup page asks for, each labelled as what it is
    ap_name: 'fauxcloud Wi-Fi name', ap_pw: 'fauxcloud Wi-Fi password',
    start: 'Start listening', starting: 'Starting…', on: 'Listening for {dur}', stop: 'Stop', done: 'Stopped after {dur}.',
    idle: 'Stopped: this sheet was closed for an hour.', restarted: 'Stopped by a restart.', again: 'Listen again',
    copy: 'Copy', dl: 'Download', report: 'Report',
    // while listening the report leads; the Wi-Fi and the devices fold below it (their summaries say what is inside)
    ap_sum: 'fauxcloud Wi-Fi: {ap}', dev_n: { one: '1 device on the fauxcloud Wi-Fi', other: '{n} devices on the fauxcloud Wi-Fi' },
    nojoin: 'No device has joined the fauxcloud Wi-Fi yet.',
    cl_on: '{ip} · {maker} · joined {t} after the start', cl_off: '{ip} · {maker} · joined {t}, left {t2} after the start', cl_pre: '{ip} · {maker} · there before the start',
    cl_rnd: 'random address', cl_noip: 'no address yet',
    empty: 'Nothing received yet.', gap: { one: '… 1 entry not kept', other: '… {n} entries not kept' },
    hint: 'fauxcloud hides the values it recognises as IDs, keys and passwords, and all but the maker’s part of each device’s MAC address. Read the report before you share it.',
    e_ota: 'An update is in progress. Try again after it.', e_restart: 'A restart is planned. Try again after it.',
    e_heap: 'Not enough free memory right now. Try again in a minute.',
  });
  const t = (k, o) => FC.t(W[k], o);
  // mm:ss, h:mm:ss from an hour (digits only: the same in every language)
  const clock = s => { s = Math.max(0, Math.floor(s || 0)); const h = s / 3600 | 0, m = (s / 60 | 0) % 60, p = n => (n < 10 ? '0' : '') + n;
    return (h ? h + ':' + p(m) : p(m)) + ':' + p(s % 60); };

  // ---------------------------------------------------------------- the history (module level)
  const H = { j: null, recs: new Map(), since: 0, lost: 0, busy: false, timer: 0, err: '' };
  function reset() { H.j = null; H.recs = new Map(); H.since = 0; H.lost = 0; H.err = ''; }
  // one GET's answer: records merged by id (n and t_last are totals), the cursor, the devices and names as sent
  function merge(j) {
    for (const e of j.events || []) H.recs.set(e.id, Object.assign(H.recs.get(e.id) || {}, e));
    if (j.events && j.events.length) H.since = Math.max(H.since, ...j.events.map(e => e.seq));
    const run = j.state === 'on' || j.state === 'off';  // "starting": the board's counters are not this run's yet
    if (!j.more && run) H.since = Math.max(H.since, j.seq || 0);
    if (j.state !== 'off' || !H.j || j.clients.length || !H.j.clients.length) H.j = j;  // after the stop: the last lists stay
    else H.j = Object.assign({}, j, { clients: H.j.clients, dns: H.j.dns, clients_over: H.j.clients_over });
    if (run) H.lost = Math.max(H.lost, j.lost || 0);
  }

  // ---------------------------------------------------------------- the report (ASCII, the Console report's text field)
  const ipTag = c => c ? '.' + c : '.?';
  const first = s => ' (first ' + clock(s) + ')';
  function report(version) {
    const j = H.j || { clients: [], dns: { names: [], other: 0 }, ntp: { asked: 0, answered: 0 } }, out = [];
    const st = j.started_utc ? new Date(j.started_utc * 1e3).toISOString().replace('T', ' ').slice(0, 16) + 'Z' : 'time:unknown';
    out.push(`fauxcloud ${version || '?'} listen ${st} ${clock(j.elapsed_s)} clock:${j.clock ? 'synced' : 'none'} ntp:${j.ntp.answered}/${j.ntp.asked} ap:192.168.77.0/24 clients:${j.clients.length}`);
    const cl = j.clients.map(c => `${c.ip ? '.' + c.ip.split('.')[3] : '.?'} vendor:${c.vendor} ${c.joined_s == null ? 'joined before' : 'joined ' + clock(c.joined_s)}${c.left_s != null ? ' left ' + clock(c.left_s) : ''} x${c.n}`);
    out.push('CLIENTS: ' + (cl.join(' | ') || 'none'));
    const dn = j.dns.names.map(d => `${d.name} x${d.n}`).concat(j.dns.other ? ['+' + j.dns.other + ' other'] : []);
    if (dn.length) out.push('DNS: ' + dn.join(' | '));
    const recs = [...H.recs.values()].sort((a, b) => a.id - b.id), kind = k => recs.filter(r => r.kind === k);
    const line = (k, label) => { const r = kind(k); if (r.length) out.push(label + r.map(x => `${ipTag(x.client)} ${x.text} x${x.n}${first(x.t_first_s)}`).join(' | ')); };
    {  // TCP: one item per device and text, the counts summed (each connection's facts are on its own TLS / HTTP line)
      const m = new Map();
      for (const r of kind('tcp')) { const k = r.client + ' ' + r.text, o = m.get(k);
        if (o) { o.n += r.n; o.t_first_s = Math.min(o.t_first_s, r.t_first_s); } else m.set(k, { client: r.client, text: r.text, n: r.n, t_first_s: r.t_first_s }); }
      if (m.size) out.push('TCP: ' + [...m.values()].map(x => `${ipTag(x.client)} ${x.text} x${x.n}${first(x.t_first_s)}`).join(' | '));
    }
    line('tls', 'TLS: ');
    // a request: its http parts joined, its headers on the same line, its body on the next
    const heads = new Map();
    for (const r of recs) if (r.kind === 'http' && !heads.has(r.group)) heads.set(r.group, r);
    for (const h of heads.values()) {
      const g = recs.filter(r => r.group === h.group);
      const ln = g.filter(r => r.kind === 'http').map(r => r.text).reduce((a, b) => a.endsWith('...') && b.startsWith('...') ? a.slice(0, -3) + b.slice(3) : a + b, '');
      const hd = g.find(r => r.kind === 'hdr'), bd = g.find(r => r.kind === 'body');
      out.push(`HTTP: ${ipTag(h.client)} ${ln}${hd ? ' ' + hd.text : ''} x${h.n}${first(h.t_first_s)}`);
      if (bd) out.push('  body: ' + bd.text);
    }
    line('mqtt', 'MQTT: ');
    line('raw', 'RAW: ');
    if (H.lost) out.push('... ' + H.lost + ' entries not kept');
    return out.join('\n');
  }

  // ---------------------------------------------------------------- the poll chain
  function tick() {
    if (H.busy) return;
    H.busy = true; clearTimeout(H.timer);
    api('GET', '/api/admin/capture?since=' + H.since).then(j => {
      H.busy = false; const was = H.j && H.j.state; merge(j);
      if (SH && SH.key === 'capture') { if (was !== j.state) draw(); else live(); }
      if (j.more) tick(); else if (j.state !== 'off') H.timer = setTimeout(tick, 3000);
    }, () => { H.busy = false; H.timer = setTimeout(tick, 3000); });  // no answer: the next poll says more
  }

  // ---------------------------------------------------------------- the sheet
  const R = { asking: false, shown: false, plain: '', pw: '', err: '', busy: false, timer: 0, press: false };
  function devices() {
    const j = H.j, c = j ? j.clients : [];
    if (!c.length) return `<p class="hint">${W.nojoin}</p>`;
    return `<ul class="extras">${c.map(x => { const o = { ip: esc(x.ip || W.cl_noip), maker: x.vendor === 'random' ? W.cl_rnd : esc(x.vendor), t: clock(x.joined_s), t2: clock(x.left_s) };
      return `<li><span>${x.joined_s == null ? t('cl_pre', o) : x.left_s != null ? t('cl_off', o) : t('cl_on', o)}</span></li>`; }).join('')}</ul>`;
  }
  function status() {
    const j = H.j;
    if (!j || j.state === 'starting') return W.starting;
    if (j.state === 'on') return t('on', { dur: clock(j.elapsed_s) });
    return j.ended === 'idle' ? W.idle : j.ended === 'restart' ? W.restarted : j.ended === 'heap' ? W.e_heap : t('done', { dur: clock(j.elapsed_s) });
  }
  const any = () => H.recs.size || (H.j && H.j.dns.names.length);
  const warn = () => H.j && H.j.ntp.asked && !H.j.ntp.answered ? `<div class="callout warn">${W.notime}</div>` : '';
  const gap = () => H.lost ? `<p class="hint">${esc(FC.t(W.gap, { n: H.lost }))}</p>` : '';
  const devSum = () => { const n = H.j ? H.j.clients.length : 0; return n ? t('dev_n', { n }) : W.nojoin; };
  // the report: the big block while listening (taller than the station console: it is what the sheet is for)
  const repHtml = () => any() ? `<pre class="console" id="capOut" tabindex="0" aria-label="${W.report}" style="height:min(62vh,560px)">${esc(report(API.status.fw_version))}</pre>`
    : `<p class="hint">${H.j && H.j.clients.length ? W.empty : W.nojoin}</p>`;
  // what the vectors read: the live parts together
  function liveHtml() { return status() + warn() + devices() + repHtml() + gap(); }
  // a poll updates the live parts in place: the status, the warning, the devices' summary and list, the report (its text
  // only, so a reader scrolled up stays where they are; at the end it follows), the gap line, Copy and Download
  function live() {
    const set = (id, h) => { const e = $(id); if (e) e.innerHTML = h; };
    set('#capSt', status()); set('#capWarn', warn()); set('#capDevN', devSum()); set('#capDev', devices()); set('#capGap', gap());
    const o = $('#capOut');
    if (o && any()) { const end = o.scrollHeight - o.scrollTop - o.clientHeight < 24; o.textContent = report(API.status.fw_version); if (end) o.scrollTop = o.scrollHeight; }
    else set('#capRep', repHtml());
    for (const id of ['#capCopy', '#capDl']) { const b = $(id); if (b) b.disabled = !H.recs.size; }
  }
  // the report's file: the bridge's own id (the four characters of its Wi-Fi name) and the local time, so several
  // captures, from one bridge or two, never share a name
  function fileName(d = new Date()) {
    const p = n => (n < 10 ? '0' : '') + n;
    return `fauxcloud-${(API.mac4 || 'bridge').replace(/[^A-Za-z0-9]/g, '')}-listen-${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}.txt`;
  }
  function consequences() {
    const L = [];
    if (API.settings.mqtt_host) L.push(W.c_ha);
    if (API.bridge.forwarding) L.push(W.c_fwd);
    if (API.metar && API.metar.available && API.metar.on) L.push(W.c_metar);
    L.push(W.c_save, W.c_time, W.c_open);
    return `<ul class="checklist">${L.map(x => `<li>${x}</li>`).join('')}</ul>`;
  }
  // the AP's password, as on the AP sheet: a fresh proof, shown for 60 s
  function apPw() {
    return R.shown ? `<div class="secret" id="capPw">${esc(R.plain)}</div><div class="row"><button type="button" class="btn quiet" id="capPwCopy">${WF.copy}</button><button type="button" class="btn quiet" id="capPwHide">${WF.hide}</button><span class="hint">${WF.hides}</span></div>`
      : R.asking ? `${field('caprev', WF.reveal_pw, text('caprev', R.pw, { type: 'password', ac: 'current-password', err: R.err }), '', R.err)}
          <div class="row"><button type="button" class="btn quiet" id="capPwCancel">${WF.cancel}</button><button type="button" class="btn primary" id="capPwGo" ${A.lock || R.busy ? 'disabled' : ''}>${WF.show_pw}</button></div>`
      : `<div class="secret" aria-label="${WF.pw_hidden}">••••••••••••</div><div class="row"><button type="button" class="btn quiet" id="capPwAsk">${WF.show_pw}</button><span class="hint">${WF.show_why}</span></div>`;
  }
  SHEETS.capture = function () {
    if (!H.j && !H.busy) tick();  // the state as of now (a capture may run from an earlier visit)
    return { icon: icon('activity'), title: W.title,
      body() {
        const j = H.j, on = j && j.state !== 'off', bl = j && !on && j.blocked;
        const err = bl ? `<p class="error" role="alert">${bl === 'ota' ? W.e_ota : W.e_restart}</p>` : H.err ? `<p class="error" role="alert">${H.err}</p>` : '';
        const row = (h, v) => `<div style="display:grid;gap:8px"><h3>${h}</h3>${v}</div>`;
        // both folds start closed and keep what the user did across a redraw; the Wi-Fi is open while its password is asked for or shown
        const isOpen = id => { const d = $(id); return !!(d && d.open); };
        const apOpen = R.asking || R.shown || isOpen('#capAp'), devOpen = isOpen('#capDevD');
        const ap = `<details class="adv" id="capAp"${apOpen ? ' open' : ''}><summary>${t('ap_sum', { ap: esc(API.ap_ssid) })}</summary>
          <div style="display:grid;gap:16px"><p style="margin:0">${W.ap}</p>${row(W.ap_name, `<div class="secret" id="capSsid">${esc(API.ap_ssid)}</div>`)}${row(W.ap_pw, apPw())}</div></details>`;
        if (!on && !H.recs.size) return `<p class="lead">${W.lead}</p>${consequences()}${j && !j.clock ? `<div class="callout warn">${W.notime}</div>` : ''}
          ${err}${j && j.ended === 'heap' ? `<p class="error">${W.e_heap}</p>` : ''}
          <div class="row end"><button type="button" class="btn primary" id="capStart" ${R.press || !j || bl ? 'disabled' : ''}>${W.start}</button></div>
          ${ap}`;
        return `<div class="row"><p class="lead" id="capSt" role="status" style="flex:1;margin:0">${status()}</p>
            ${on ? `<button type="button" class="btn danger" id="capStop">${W.stop}</button>` : `<button type="button" class="btn quiet" id="capAgain">${W.again}</button>`}</div>
          ${err}<div id="capWarn" style="display:contents">${warn()}</div><div id="capRep" style="display:contents">${repHtml()}</div>
          <div id="capGap" style="display:contents">${gap()}</div>
          <div class="row end"><button type="button" class="btn quiet" id="capCopy" ${H.recs.size ? '' : 'disabled'}>${W.copy}</button>
            <button type="button" class="btn quiet" id="capDl" ${H.recs.size ? '' : 'disabled'}>${W.dl}</button></div>
          ${on ? ap : ''}
          <details class="adv" id="capDevD"${devOpen ? ' open' : ''}><summary id="capDevN">${devSum()}</summary><div id="capDev">${devices()}</div></details>
          <p class="hint">${W.hint}</p>`;
      },
      input(e) { if (e.id === 'caprev') R.pw = e.value; },
      click(b) {
        if (b.id === 'capStart' || b.id === 'capAgain') {
          if (b.id === 'capAgain') { reset(); draw(); tick(); return; }  // back to the start view; its Start waits for the board's state
          R.press = true; H.err = ''; b.disabled = true;
          api('POST', '/api/admin/capture/start', {}).then(() => { R.press = false; reset(); tick(); }, e => {
            R.press = false;
            // 409 (an update, a planned restart, or listening already): the next GET says which (`blocked`, `state`)
            if (e && /409/.test(e.message)) { reset(); tick(); } else { H.err = WF.no_answer; draw(); } });
          return;
        }
        if (b.id === 'capStop') { b.disabled = true; api('POST', '/api/admin/capture/stop', {}).then(() => tick(), noAnswer(b)); return; }
        if (b.id === 'capCopy') { copyText(report(API.status.fw_version), $('#capOut')); return; }
        if (b.id === 'capDl') {
          const a = document.createElement('a');
          a.href = URL.createObjectURL(new Blob([report(API.status.fw_version) + '\n'], { type: 'text/plain' }));
          a.download = fileName(); document.body.appendChild(a); a.click();
          setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000); return;
        }
        if (b.id === 'capPwAsk') { R.asking = true; R.err = ''; draw('caprev'); return; }
        if (b.id === 'capPwCancel') { R.asking = false; R.pw = ''; draw(); return; }
        if (b.id === 'capPwHide') { R.shown = false; R.plain = ''; clearTimeout(R.timer); draw(); return; }
        if (b.id === 'capPwCopy') { copyText(R.plain, $('#capPw')); return; }
        if (b.id === 'capPwGo') {
          R.busy = true; draw();
          withProof(R.pw, {}, '/api/admin/ap-pass/reveal').then(j => { R.busy = false; R.pw = '';
            if (j.ok) { A.tries = 5; R.shown = true; R.plain = j.ap_pass; R.asking = false; clearTimeout(R.timer); R.timer = setTimeout(() => { R.shown = false; R.plain = ''; if (SH && SH.key === 'capture') draw(); }, 60000); draw(); }
            else { R.err = failTry(j); draw('caprev'); } }, () => { R.busy = false; R.pw = ''; R.err = WF.no_answer_short; draw('caprev'); });
        }
      } };
  };
  // the vectors' handles (test/tools/capture_vectors.js)
  SHEETS.capture.T = { H, W, merge, report, reset, clock, liveHtml, live, consequences, status, fileName, devSum };
})();
