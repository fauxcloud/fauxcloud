// admin-fwd.js -- lazy: the admin "Online services" sheet. The master
// switch (on => one station, no LAN uploads; with two stations the user picks the one to keep, the other is
// forgotten after a confirmation that names it), up to three services with their last result and counts, "Send one
// now", and the add/edit form with each service's help and location sentences. Whole phrases with named placeholders.
// Keys are write-only: the form never shows one. No interval choice (the interval is fixed); HTTPS only on a
// board that has it (F.tls, the FWD_TLS build), otherwise one sentence says why not.
(() => {
  const W = FC.w('admin-fwd', {
    master: 'Send readings to online services',
    // the header says what sends and what does not
    master_s: 'Sends this station’s readings to the services below. The time, airport weather and the forecast work without it.',
    on_rule: 'On: one station only, no uploads over your home network (LAN mode).',
    keep: 'Keep which station?', keep_s: 'Forwarding works with one station. The other one is forgotten: its settings and history are deleted.',
    sure: 'Forget {name} and turn forwarding on?', sure_s: 'Its settings and history are deleted. This cannot be undone. fauxcloud then restarts to free the memory (about a minute).',
    sure_go: 'Forget {name}, turn on', back: 'Back',
    lan_q: 'Turn off uploads from your home network?', lan_s: 'A console that sends through your router (LAN mode) stops reaching fauxcloud. Consoles on the fauxcloud Wi-Fi are not affected.',
    lan_go: 'Turn off, turn forwarding on',
    none: 'No service yet.', add: 'Add a service', addq: 'Which service?',
    last: 'Last sent {t}', never: 'Not sent yet', paused: 'Paused: check the key', off_last: 'Off · last sent {t}', off: 'Off',
    paused_t: 'Paused · last sent {t}', k_missing: 'Key missing: Edit to add it', k_refused: 'Key refused: Edit to check it',
    set_up: '{list} are set up above.', set_up1: '{list} is set up above.',
    // the forecast (its own section; the key in Edit; Fetch now checks the key, as Send one now)
    fc_sec: 'Forecast', fc: 'Weather Underground forecast', fc_sw: 'Show WU’s forecast on the dashboard',
    fc_off_last: 'Off · last fetch {t}', fc_wait: 'Not fetched yet · the first fetch is at about {t}', fc_wait0: 'Not fetched yet',
    fc_ok: 'Fetched {t} · {day}: {phrase}, {hi}° / {lo}°', fc_paused: 'Paused · last fetch {t}', fc_check: 'Fetch now first: it checks the key.',
    fc_info: 'On the dashboard: tomorrow, from WU once an hour · location {lat}, {lon} (about 11 km)', fc_noloc: 'Needs this station’s location (Settings, Location).',
    fc_wunote: 'WU gives API keys to stations that upload to it. Your console can upload to WU itself, or turn Weather Underground on above.',
    fc_e1: 'Shows tomorrow’s weather from Weather Underground on the outlook card’s second page, marked WU. fauxcloud asks WU once an hour.',
    fc_e2: 'The API key is on wunderground.com → API keys (free for stations that upload to WU). It is not the Station Key.',
    fc_e3: 'WU gets this station’s location with each request, rounded to {lat}, {lon} (about 11 km).', fc_key: 'API key',
    fc_save_first: 'Save first, then fetch one.', fc_run: 'Fetch now', fc_running: 'Fetching…', fc_done: 'Fetched {t} · {r}', fc_tried: 'Tried {t} · {r}', fc_queued: 'Queued · runs in about {n} s',
    fp: 'Paused: WU’s answer could not be used. Save to try again.', fp_untrusted: 'Paused: WU’s certificate is not trusted. Update fauxcloud.',
    fp_memory: 'Paused: not enough memory for the fetch. Save to try again.', fc_last: 'Last fetch {t} · {r}', d_today: 'Today', d_tomorrow: 'Tomorrow',
    e_key_format: 'Key format: 32 hex characters (the API key, not the Station Key)',
    fr_nodata: 'No forecast for tomorrow', fr_limited: 'WU asked to wait; it tries again later',
    fr_busy: 'fauxcloud stayed busy, try again later', fr_off: 'Needs the key, the location and the time', fr_low_memory: 'fauxcloud is low on memory, try later',
    paused_mem: 'Paused: not enough memory for an encrypted send on this board. Turn off Encrypted (HTTPS).',
    counts: '{ok} sent · {skip} skipped · {drop} dropped',
    test: 'Send one now', edit: 'Edit', remove: 'Remove', remove_q: 'Remove {svc}?',
    // the switch on the row; "Send one now" on the row while off; the station page
    save_first: 'Save first, then send one.', full: 'Up to three services: remove one to add another.',
    on_l: '{svc} on', check_first: 'Send one now first: it checks the id and key.', see: 'See your station on {svc}',
    common: 'Every {n} minutes fauxcloud sends this station’s outdoor readings to {svc}. {svc} shows them on a public page and map and may reuse them (its terms).',
    h_wu: 'Copy the Station ID and the Station Key from wunderground.com → My devices. Check the elevation WU shows; it is public.',
    h_windy: 'On stations.windy.com open your station: copy the Station id and the Station password from its Connection box (not an account API key). Windy takes one reading per 5 minutes. Its “Show recent requests” shows what arrived. On Windy’s station page you can add your name, a website and a logo; none of it is needed.',
    h_wowbe: 'Copy the 8-character Short ID from your site’s page (not the long page id) and the password you chose. Set your own time zone there (it starts on Brussels).',
    h_pwsweather: 'Copy the Station ID and the 32-character API key from the station page (not your account password).',
    h_template: 'The variables a URL may use are listed in the fauxcloud documentation (Forwarding).',
    loc: '{svc} publishes the location you entered there: enter a rounded one (2 decimals ≈ 1 km).',
    loc_wowbe: '{svc} publishes the location you entered there as typed, to 6 decimals: enter a rounded one (2 decimals ≈ 1 km).',
    loc_pwsweather: '{svc} publishes the location you entered there. Choose Precision: Neighborhood.',
    f_station: 'Station', f_id: 'Station ID', f_key: 'Key or password', f_host: 'Server', f_path: 'Path', f_url: 'URL template',
    f_success: 'Answer that means “accepted” (optional)', f_https: 'Encrypted (HTTPS)',
    f_https_s: 'Uses about 17 KB of memory per send on this board. Off: sent as plain HTTP, as the console itself does.',
    no_tls: 'Sent without encryption (HTTP): this build has no encryption.',
    unchanged: 'unchanged', save: 'Save', cancel: 'Cancel',
    r_ok: 'Accepted', r_refused: 'Key refused', r_soon: 'Too soon — {svc} takes one reading per {n} minutes',
    r_fail: 'Server not reachable', r_tls: 'Certificate not trusted — use plain HTTP', r_tls_p: 'Certificate not trusted — update fauxcloud', r_off: 'Not sent: forwarding is off', r_nodata: 'Not sent: no fresh reading',
    r_busy: 'Not sent: fauxcloud is busy, try in a minute', r_other: 'Unexpected answer {status}', r_novalue: 'Not sent: a value the URL needs is missing',
    r_nomem: 'Not sent: not enough memory for an encrypted send', r_noanswer: 'No answer from fauxcloud — try again',
    running: 'Sending…', restarting: 'Forwarding is on. fauxcloud restarts now; this page comes back in about a minute.',
    n_compat: 'WU-compatible server', n_custom: 'Custom service', title: 'Online services',
    loading: 'Loading…', load_fail: 'Could not load this page, try again.',
  });
  const fill = (k, o) => W[k].replace(/\{(\w+)\}/g, (m, n) => esc(o[n] ?? m));
  // WU's phrase in the page's words, as the dashboard says it -- dash-wufc.js's ph(), the
  // same text (fwd_vectors.js keeps the two copies equal: the admin does not load the dashboard's file)
  const ph = p => String(p).replace(/\bAM\b/g, 'Morning').replace(/\bPM\b/g, 'Afternoon').replace(/ *\/ */g, ', ')
    .replace(/ [A-Z][a-z]+\b/g, w => w.toLowerCase()).replace(/^./, c => c.toUpperCase());
  const NAME = { wu: 'Weather Underground', windy: 'Windy', wowbe: 'WOW-BE', pwsweather: 'PWSweather', wu_compat: W.n_compat, template: W.n_custom };
  // the public page of a station, where it follows from the id (WOW-BE and PWSweather use other ids: no link)
  const PAGE = { windy: id => 'https://www.windy.com/station/pws-' + encodeURIComponent(id), wu: id => 'https://www.wunderground.com/dashboard/pws/' + encodeURIComponent(id) };
  const floor = t => (t === 'windy' || t === 'pwsweather' ? 300 : 60);
  const interval = t => (floor(t) >= 300 ? floor(t) + 10 : 300);  // fwdIntervalS: fixed, the service minimum + 10 s
  const svc = x => x.type === 'wu_compat' && x.host ? x.host.split(':')[0] : NAME[x.type];
  // a result (the target's last, or a test's) in words
  function words(r, x, status, why) {
    const n = Math.round(floor(x.type) / 60);
    if (r === 'ok' || r === 'dup') return W.r_ok;
    if (r === 'refused') return W.r_refused;
    if (r === 'limited' || r === 'too_soon') return fill('r_soon', { svc: svc(x), n });
    if (r === 'fail') return W.r_fail;
    if (r === 'tls_untrusted') return x.type === 'wu_compat' || x.type === 'template' ? W.r_tls : W.r_tls_p;  // a preset: never plain HTTP
    if (r === 'off') return W.r_off;
    if (r === 'nodata') return W.r_nodata;
    if (r === 'busy') return W.r_busy;
    if (r === 'no_memory') return W.r_nomem;
    if (r === 'noanswer') return W.r_noanswer;
    if (why === 'no_value') return W.r_novalue;
    return fill('r_other', { status: status > 0 ? status : '' }).trim();
  }
  const help = (t, n) => [fill('common', { n, svc: NAME[t] })].concat(W['h_' + t] ? [W['h_' + t]] : [],
    t in { wu: 1, windy: 1, wowbe: 1, pwsweather: 1 } ? [fill(W['loc_' + t] ? 'loc_' + t : 'loc', { svc: NAME[t] })] : []);
  const inp = (id, v, ph, type) => `<span class="input"><input id="${id}" type="${type || 'text'}" value="${esc(v || '')}"${ph ? ` placeholder="${esc(ph)}"` : ''} autocomplete="off" autocapitalize="off" spellcheck="false"></span>`;

  function row(x, rm, test) {
    const l = x.last || {}, n = x.n || {};
    const lt = dayTime(x.last_ok, new Date()), red = !x.key ? W.k_missing : x.paused === 'key' ? W.k_refused : '';
    const state = x.paused ? `<span class="dot bad"></span>${x.paused === 'memory' ? W.paused_mem : lt ? fill('paused_t', { t: lt }) : W.paused}`
      : !x.enabled ? `<span class="dot off"></span>${lt ? fill('off_last', { t: lt }) : W.off}`
      : `<span class="dot"></span>${lt ? fill('last', { t: lt }) : W.never}${l.t ? ' · ' + esc(words(l.result, x, l.status, l.why)) : ''}`;
    const t = test && test.slot === x.slot ? test : null;
    // the public station page where it follows from the id (Windy, WU), once a send was accepted (the id exists there)
    // `verified` (an accepted send with these settings) gates the switch and the station page; a board
    // without the field keeps the last-accepted rule
    const ok = x.verified ?? !!x.last_ok, page = ok && PAGE[x.type] ? `<a class="link" href="${esc(PAGE[x.type](x.id))}" target="_blank" rel="noopener" style="margin-right:auto">${fill('see', { svc: NAME[x.type] })}</a>` : '';
    return `<section class="glass card fwr"><div class="row"><h3 style="flex:1">${esc(svc(x))}</h3><span class="hint">${esc(x.station)}</span>
      <label class="toggle rowsw"><input type="checkbox" role="switch" id="fon${x.slot}" data-s="${x.slot}" aria-label="${fill('on_l', { svc: svc(x) })}"${x.enabled ? ' checked' : ok ? '' : ' disabled'}></label></div>
      <p class="hint fst" role="status">${state}</p>${red ? `<p class="error fst"><span class="dot bad"></span>${red}</p>` : ''}${!x.enabled && !ok && !red ? `<p class="hint">${W.check_first}</p>` : ''}<p class="hint q">${fill('counts', { ok: (n.ok || 0) + (n.dup || 0), skip: (n.skip || 0) + (n.nodata || 0) + (n.heap || 0), drop: n.drop || 0 })}</p>
      ${t ? `<p class="${t.state === 'done' && t.result !== 'ok' && t.result !== 'dup' ? 'error' : 'hint'}" role="status">${t.state === 'running' ? W.running : esc(words(t.result, x, t.status, t.why)) + (t.status > 0 ? ` (${t.status}, ${t.ms} ms)` : '')}${t.body ? `<br><code>${esc(t.body)}</code>` : ''}</p>` : ''}
      <div class="row end">${page}${rm === x.slot ? `<button type="button" class="btn quiet" data-a="norm">${W.cancel}</button><button type="button" class="btn quiet bad" data-a="rmgo" data-s="${x.slot}">${fill('remove_q', { svc: svc(x) })}</button>`
        : `${x.enabled ? '' : `<button type="button" class="btn quiet" data-a="test" data-s="${x.slot}"${t && t.state === 'running' ? ' disabled' : ''}>${W.test}</button>`}<button type="button" class="btn quiet" data-a="edit" data-s="${x.slot}">${W.edit}</button><button type="button" class="btn quiet bad" data-a="rm" data-s="${x.slot}">${W.remove}</button>`}</div></section>`;
  }

  function form(d, stations, tls) {
    const t = d.type, n = Math.round((d.min_s || interval(t)) / 60);
    return `<h3>${esc(NAME[t])}</h3>${help(t, n).map(s => `<p class="hint">${esc(s)}</p>`).join('')}
      ${stations.length > 1 ? field('fst', W.f_station, sel('fst', d.station, stations.map(s => [s, s]))) : ''}
      ${t === 'template' ? field('furl', W.f_url, inp('furl', d.url, (tls ? 'https' : 'http') + '://example.org/up?t={temp_c}&id={id}&k={key}')) : ''}
      ${t === 'wu_compat' ? field('fhost', W.f_host, inp('fhost', d.host, 'example.org')) + field('fpath', W.f_path, inp('fpath', d.path)) : ''}
      ${field('fid', W.f_id, inp('fid', d.id))}
      ${field('fkey', W.f_key, inp('fkey', '', d.keySet ? W.unchanged : '', 'password'))}
      ${t === 'wu_compat' || t === 'template' ? field('fsucc', W.f_success, inp('fsucc', d.success, 'success')) : ''}
      ${!tls ? `<p class="hint">${W.no_tls}</p>` : t === 'template' ? '' : tog('fhttps', d.https, W.f_https, W.f_https_s)}
      ${d.slot != null && d.enabled ? `<p class="hint" id="fsendHint"${d.dirty ? '' : ' hidden'}>${W.save_first}</p>` : ''}
      <div class="row end">${d.slot != null && d.enabled ? `<button type="button" class="btn quiet" id="fsend" data-a="test" data-s="${d.slot}"${d.dirty ? ' disabled' : ''}>${W.test}</button>` : ''}<button type="button" class="btn quiet" data-a="norm">${W.cancel}</button><button type="button" class="btn primary" data-a="save">${W.save}</button></div>`;
  }

  // the forecast card -- the switch only with a verified key; Fetch now on the card only
  // while off and unverified; one red line for a key problem; the info line with the location as sent (1 decimal)
  const fword = r => W['fr_' + r] || (r === 'ok' ? W.r_ok : r === 'refused' ? W.r_refused : r === 'fail' ? W.r_fail : fill('r_other', { status: '' }).trim());
  // queued (with the firmware's estimate), fetching, "Fetched {t}" (ok / nodata), "Tried {t}" (a failed attempt), a refusal alone
  const rl = v => v.frun ? (v.frun > 1 ? W.fc_running : fill('fc_queued', { n: v.feta })) : v.ft ? fill(v.fres === 'ok' || v.fres === 'nodata' ? 'fc_done' : 'fc_tried', { t: hm(v.ft), r: fword(v.fres) }) : fword(v.fres);
  function fcard(w, v, T) {
    const l = w.last || {}, lt = dayTime(l.t, new Date()), run = v.frun ? v.frun : w.run && w.run.state !== 'idle' && w.run.state !== 'done' ? 'running' : '';
    const red = !w.key ? W.k_missing : w.pause === 'key' ? W.k_refused : '', pz = w.pause && w.pause !== 'none' && w.pause !== 'key' ? W['fp_' + w.pause] || W.fp : '';
    const st = w.state === 'paused' ? `<span class="dot bad"></span>${lt ? fill('fc_paused', { t: lt }) : W.paused}`
      : !w.on ? `<span class="dot off"></span>${lt ? fill('fc_off_last', { t: lt }) : W.off}`
      : w.tomorrow ? `<span class="dot"></span>${fill('fc_ok', { t: hm(w.tomorrow.t), day: W[w.tomorrow.day === 'today' ? 'd_today' : 'd_tomorrow'], phrase: ph(w.tomorrow.phrase), hi: w.tomorrow.hi, lo: w.tomorrow.lo })}`
      : l.t ? `<span class="dot${l.result === 'ok' || l.result === 'nodata' ? ' off' : ' bad'}"></span>${fill('fc_last', { t: lt, r: fword(l.result) })}`
      : `<span class="dot off"></span>${w.next_t ? fill('fc_wait', { t: hm(w.next_t) }) : W.fc_wait0}`;
    const check = !w.on && w.key && !w.verified;
    const wuSends = T.some(x => x.type === 'wu' && x.enabled);
    return `<section class="glass card fwr"><div class="row"><h3 style="flex:1">${W.fc}</h3><label class="toggle rowsw"><input type="checkbox" role="switch" id="fcOn" aria-label="${W.fc_sw}"${w.on ? ' checked' : w.verified ? '' : ' disabled'}></label></div>
      <p class="hint fst" role="status">${st}</p>${red ? `<p class="error fst"><span class="dot bad"></span>${red}</p>` : pz ? `<p class="error">${pz}</p>` : check ? `<p class="hint">${W.fc_check}</p>` : ''}
      ${v.frun || v.fres ? `<p class="${v.fres && v.fres !== 'ok' && v.fres !== 'nodata' ? 'error' : 'hint'}" role="status">${rl(v)}</p>` : ''}
      <p class="hint q">${w.loc ? fill('fc_info', { lat: w.loc.lat, lon: w.loc.lon }) : W.fc_noloc}</p>${wuSends ? '' : `<p class="hint">${W.fc_wunote}</p>`}
      <div class="row end">${check ? `<button type="button" class="btn quiet" data-a="fcrun"${run ? ' disabled' : ''}>${W.fc_run}</button>` : ''}<button type="button" class="btn quiet" data-a="fced">${W.edit}</button></div></section>`;
  }
  function fform(w, v) {
    const loc = w.loc ? fill('fc_e3', { lat: w.loc.lat, lon: w.loc.lon }) : W.fc_noloc;
    return `<h3>${W.fc}</h3><p class="hint">${W.fc_e1}</p><p class="hint">${W.fc_e2}</p><p class="hint">${loc}</p>
      ${field('fcKey', W.fc_key, inp('fcKey', '', w.key ? W.unchanged : '', 'password'))}
      ${w.key ? `<p class="hint" id="fcHint"${v.fc.dirty ? '' : ' hidden'}>${W.fc_save_first}</p>` : ''}
      <div class="row end">${w.key ? `<button type="button" class="btn quiet" id="fcrun" data-a="fcrun"${v.fc.dirty || v.frun ? ' disabled' : ''}>${W.fc_run}</button>` : ''}<button type="button" class="btn quiet" data-a="norm">${W.cancel}</button><button type="button" class="btn primary" data-a="fcsave">${W.save}</button></div>
      ${v.frun || v.fres ? `<p class="hint" role="status">${rl(v)}</p>` : ''}`;
  }

  function render(F, v, st) {  // F = /api/admin/forwarding, v = the view, st = the stations' ids
    if (!F) return `<p class="hint loading">${W.loading}</p>`;
    if (F.err) return `<p class="error">${W.load_fail}</p>`;
    if (v.keep && !v.sure) return `<h3>${W.keep}</h3><p class="hint">${W.keep_s}</p><div class="row">${st.map(id => `<button type="button" class="btn quiet" data-a="keep" data-id="${esc(id)}">${esc(id)}</button>`).join('')}</div><div class="row end"><button type="button" class="btn quiet" data-a="norm">${W.back}</button></div>`;
    if (v.lan) return `<h3>${W.lan_q}</h3><p class="hint">${W.lan_s}</p><div class="row end"><button type="button" class="btn quiet" data-a="norm">${W.back}</button><button type="button" class="btn primary" data-a="enable">${W.lan_go}</button></div>`;
    if (v.sure) { const other = st.find(id => id !== v.sure); return `<h3>${fill('sure', { name: other })}</h3><p class="hint">${W.sure_s}${v.lanOn ? ' ' + W.lan_s : ''}</p><div class="row end"><button type="button" class="btn quiet" data-a="norm">${W.back}</button><button type="button" class="btn primary bad" data-a="enable" data-id="${esc(v.sure)}">${fill('sure_go', { name: other })}</button></div>`; }
    const T = F.targets || [], used = Object.keys(NAME).filter(k => k !== 'wu_compat' && k !== 'template' && T.some(x => x.type === k));
    if (v.pick) return `<h3>${W.addq}</h3><ul class="extras">${Object.keys(NAME).filter(k => !used.includes(k)).map(k => `<li><button type="button" class="link" data-a="pick" data-t="${k}">${esc(NAME[k])}</button></li>`).join('')}</ul>${used.length ? `<p class="hint">${fill(used.length > 1 ? 'set_up' : 'set_up1', { list: used.map(k => NAME[k]).join(' and ') })}</p>` : ''}<div class="row end"><button type="button" class="btn quiet" data-a="norm">${W.cancel}</button></div>`;
    if (v.ed) return form(v.ed, st, !!F.tls);
    if (v.fc && F.wufc) return fform(F.wufc, v);
    return `${tog('fwdOn', F.enabled, W.master, W.master_s)}${F.enabled ? `<p class="hint">${W.on_rule}</p>` : ''}
      ${T.length ? T.map(x => row(x, v.rm, v.test)).join('') : `<p class="hint">${W.none}</p>`}
      ${T.length < 3 ? `<div class="row"><button type="button" class="btn" data-a="add">${W.add}</button></div>` : `<p class="hint">${W.full}</p>`}
      ${F.wufc ? `<h3 class="sec">${W.fc_sec}</h3>${fcard(F.wufc, v, T)}` : ''}`;
  }

  // one poll chain for the sheet: an open stops the chain of a sheet closed and reopened within one interval
  let fwdPoll = 0, fwdGen = 0;
  SHEETS.fwd = function () {
    let F = null, tries = 0;
    const g = ++fwdGen, on = () => SH && SH.key === 'fwd' && g === fwdGen;
    clearTimeout(fwdPoll);
    const v = {}, ids = () => API.stations.map(x => x.id);
    const load = () => api('GET', '/api/admin/forwarding').then(j => { F = j; API.fwd = j; if (on()) draw(); }, () => { F = { err: 1 }; if (on()) draw(); });
    const dirty = t => {
      if (v.ed && !v.ed.dirty && t.closest && t.closest('.fld')) { v.ed.dirty = true; const b = $('#fsend'), h = $('#fsendHint'); if (b) b.disabled = true; if (h) h.hidden = false; }
      if (v.fc && !v.fc.dirty && t.id === 'fcKey') { v.fc.dirty = true; const b = $('#fcrun'), h = $('#fcHint'); if (b) b.disabled = true; if (h) h.hidden = false; }  // save first
    };
    const reset = () => { Object.keys(v).forEach(k => { if (k !== 'test' && k !== 'lanOn' && k !== 'fres') delete v[k]; }); };
    // Fetch now -- the result comes with the forwarding GET (wufc.run), asked every 2 s, about 40 s at most
    // also the sheet's first load -- a run queued, running or done before the sheet opened is shown
    const pollRun = n => load().then(() => { const r = F && F.wufc && F.wufc.run, s = r && r.state;
      if (s === 'done') { v.frun = 0; v.fres = r.result; v.ft = r.t; if (on()) draw(); }
      else if (s && s !== 'idle' && n < 150 && on()) { v.frun = s === 'running' ? 2 : 1; v.feta = r.eta_s; draw(); fwdPoll = setTimeout(() => pollRun(n + 1), Math.min(30, (r.eta_s || 0) + 5) * 1e3); }
      else v.frun = 0; });
    const val = id => { const e = document.getElementById(id); return e ? (e.type === 'checkbox' ? e.checked : e.value) : undefined; };
    const pollTest = () => api('GET', '/api/admin/forwarding/test').then(t => {
      tries = 0; v.test = t; if (on()) draw();
      if (t.state === 'running' && on()) fwdPoll = setTimeout(pollTest, 1000); else load();
    }, e => {  // a 503 during a send: ask again, about 20 s at most; signed out: the sign-in, no more asking
      if (!on()) return;
      if (e && /HTTP 40[13]/.test(e.message)) { login(); return; }
      if (++tries < 10) { fwdPoll = setTimeout(pollTest, 2000); return; }
      tries = 0; v.test = { slot: v.test ? v.test.slot : -1, state: 'done', result: 'noanswer' }; draw();
    });
    pollRun(0);
    return { icon: icon('upload'), title: W.title, noSave: true,
      body: () => render(F, v, ids()),
      // a change in the Edit form waits for Save before "Send one now" (it tests the stored settings)
      input: dirty,
      change(t) {
        dirty(t);
        // a row's switch saves the target as it is with `enabled` flipped (the key stays: never sent back, never resent)
        if (t.dataset.s != null && /^fon\d$/.test(t.id)) {
          const x = F && (F.targets || []).find(y => y.slot === +t.dataset.s), on = t.checked;
          if (!x) return;
          const body = { type: x.type, station: x.station, id: x.id, enabled: on };
          ['host', 'path', 'url', 'success'].forEach(k => { if (x[k] != null) body[k] = x[k]; });
          if (F.tls && x.type !== 'template') body.https = !!x.https;
          t.disabled = true;
          api('PUT', '/api/admin/forwarding?slot=' + x.slot, body).then(j => { F = j; API.fwd = j; if (j.errors && j.errors.length) toast(j.errors[0]); draw(); },
            e => { t.checked = !on; t.disabled = false; noAnswer()(e); });
          return;
        }
        if (t.id === 'fcOn') {  // the forecast switch (the firmware refuses "on" without a verified key)
          // a 400 reaches the sheet without its body -- the unverified key is caught here, before the POST
          if (t.checked && !(F && F.wufc && F.wufc.verified)) { t.checked = false; toast(W.fc_check); return; }
          t.disabled = true;
          api('POST', '/api/admin/wufc', { on: t.checked }).then(load, e => { t.checked = !t.checked; t.disabled = false; noAnswer()(e); });
          return;
        }
        if (t.id !== 'fwdOn') return;
        if (!t.checked) { api('PUT', '/api/admin/bridge', { forwarding: false }).then(() => { load(); refresh(); }, noAnswer()); return; }
        v.lanOn = !!API.bridge.lan_uploads;
        if (API.stations.length > 1) { t.checked = false; v.keep = true; draw(); return; }
        if (v.lanOn) { t.checked = false; v.lan = true; draw(); return; }
        api('POST', '/api/admin/forwarding/enable', {}).then(j => { if (j.errors) toast(j.errors[0]); load(); refresh(); }, noAnswer());
      },
      click(b) {
        const a = b.dataset.a, s = +b.dataset.s, x = F && (F.targets || []).find(y => y.slot === s);
        if (a === 'norm') { reset(); draw(); }
        if (a === 'keep') { v.sure = b.dataset.id; draw(); }
        if (a === 'enable') { b.disabled = true; api('POST', '/api/admin/forwarding/enable', b.dataset.id ? { keep: b.dataset.id } : {}).then(j => { if (j.errors) toast(j.errors[0]); if (j.restart) toast(W.restarting); reset(); load(); refresh(); }, noAnswer(b)); }
        if (a === 'add') { v.pick = true; draw(); }
        if (a === 'fced') { reset(); delete v.fres; v.fc = { dirty: false }; draw(); }
        // queued at once (202 with eta_s); refused only with 409 off / 503 low_memory (the status, the body never reaches here)
        if (a === 'fcrun') { b.disabled = true; clearTimeout(fwdPoll); delete v.fres; delete v.ft; api('POST', '/api/admin/wufc/run', {}).then(j => { v.frun = 1; v.feta = j.eta_s; draw(); pollRun(0); },
          e => { const m = / (409|503)/.exec(e && e.message || ''); if (!m) return noAnswer(b)(); v.fres = m[1] > 500 ? 'low_memory' : 'off'; b.disabled = false; draw(); }); }
        if (a === 'fcsave') {  // the key's format is checked here (a 400's body never reaches the sheet)
          const k = val('fcKey');
          if (k && !/^[0-9a-f]{32}$/i.test(k)) { toast(W.e_key_format); return; }
          b.disabled = true;
          api('POST', '/api/admin/wufc', k ? { key: k } : {}).then(() => { reset(); load(); }, noAnswer(b));
        }
        if (a === 'pick') { const t = b.dataset.t; reset(); v.ed = { type: t, station: ids()[0] || '', min_s: interval(t), https: !!F.https_default, enabled: false, path: t === 'wu_compat' ? '/weatherstation/updateweatherstation.php' : '' }; draw(); }
        if (a === 'edit' && x) { reset(); v.ed = Object.assign({}, x, { keySet: !!x.key }); draw(); }
        if (a === 'rm') { v.rm = s; draw(); }
        if (a === 'rmgo') { b.disabled = true; api('PUT', '/api/admin/forwarding?slot=' + s, { remove: true }).then(j => { F = j; reset(); draw(); }, noAnswer(b)); }
        if (a === 'test') { if (v.ed) reset(); b.disabled = true; clearTimeout(fwdPoll); tries = 0; api('POST', '/api/admin/forwarding/test?slot=' + s, {}).then(() => { v.test = { slot: s, state: 'running' }; draw(); pollTest(); }, () => { b.disabled = false; v.test = { slot: s, state: 'done', result: 'busy' }; draw(); }); }
        if (a === 'save') {
          const d = v.ed, body = { type: d.type, station: val('fst') ?? d.station, id: val('fid'), enabled: !!d.enabled };  // on / off is the row's switch
          if (d.type === 'wu_compat') Object.assign(body, { host: val('fhost'), path: val('fpath') });
          if (d.type === 'template') body.url = val('furl');
          if (d.type === 'wu_compat' || d.type === 'template') body.success = val('fsucc');
          if (F.tls && d.type !== 'template') body.https = val('fhttps');
          const k = val('fkey'); if (k) body.key = k;
          b.disabled = true;
          api('PUT', '/api/admin/forwarding' + (d.slot != null ? '?slot=' + d.slot : ''), body).then(j => {
            F = j; if (j.errors && j.errors.length) { b.disabled = false; toast(j.errors[0]); return; } reset(); draw(); }, noAnswer(b));
        }
      } };
  };
  SHEETS.fwd.render = render;  // the node render test
  SHEETS.fwd.W = W;
})();
