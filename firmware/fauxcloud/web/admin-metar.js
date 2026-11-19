// admin-metar.js -- lazy: the admin "Airport weather" sheet. Its own switch (off by
// default, explained), up to three airports (found near the configured location, or by a typed code), each with its
// report's age, pressure and wind in the user's units; the one reference reading; "Fetch now" and the last fetch.
// Whole phrases with named placeholders. R&D figures stay in the JSON (/api/admin/metar), never on this sheet.
(() => {
  const W = FC.w('admin-metar', {
    on: 'Airport weather', on_s: 'Reports every 15 min and forecasts every hour, from aviationweather.gov (US weather service).',
    on_s2: 'It sees your internet address and the airport codes; “Find airports near you” also sends your area, rounded to about 10 km.',
    clock: 'Waiting for the clock (internet time).', loc: 'Set the location first (Location and time) to find the airports near you. A code you type works anyway.',
    find: 'Find airports near you', code: 'Airport code', look: 'Look up', looking: 'Looking up…', add: 'Add', remove: 'Remove',
    // the user's own name for an airport; empty = the name from aviationweather.gov
    rename: 'Rename {id}', name_l: 'Name for {id}', name_h: 'About 24 characters, fewer with accents. Leave it empty for the name from aviationweather.gov.', save: 'Save', cancel: 'Cancel',
    none: 'No airport with reports near your location. Add one by its code.', unknown: 'No airport {id} in the aviation weather list.',
    have: '{id} is already in your airports.', partial: 'More airports than fauxcloud could read: these are the nearest of the ones it read.',
    yours: 'Your airports (up to three)', found: 'Add an airport', no_air: 'No airport yet.',
    near: 'about {d} away', far: '{d} away — too far to compare pressure',
    no_rep: 'No report yet',
    // the airport's forecast in the outlook's own words, one phrase per code
    taf_badge: '{d} · forecast', taf_badge_only: 'forecast',
    // one template per certainty, the phenomenon as a word (dash-airport.js has the same family with {span})
    taf_e: '{what} likely {from}–{to}.', taf_p: '{what} possible {from}–{to}.',
    // a window cut at the board's horizon (the data stops there): no end time
    taf_e_o: '{what} likely from {from}.', taf_p_o: '{what} possible from {from}.',
    // a window that began before now (the board's started flag: its from is now): no start time; cut too: the whole span
    taf_e_n: '{what} likely throughout.', taf_p_n: '{what} possible throughout.',
    taf_e_u: '{what} likely until {to}.', taf_p_u: '{what} possible until {to}.',
    // one wording family with the dashboard -- "Strong gusts", no number (the TAF windows carry
    // none; the threshold is >= 40 km/h)
    taf_none: 'Nothing significant forecast.', taf_na: 'No forecast for this airport.', taf_wait: 'Forecast comes with the next hourly fetch.',
    wind_dir: '{d}° {s}', wind_vrb: 'variable {s}', gusts: '{w}, gusts {g}',
    ref: 'Pressure reference: {p} (from {list})', mixed: 'The airports differ by more than 1.5 hPa: the first one in your list is used.',
    // the list's order is the priority (the dashboard's main airport, the reference weights 3 / 2 / 1)
    main: 'Main airport', main_km: 'Main airport, {d}', vals: '{p} · wind {w}', last_rep: 'Last report {age}', up: 'Move {id} up', down: 'Move {id} down',
    // the Online services shape -- a fetch that brought an answer is "Fetched", a failed one "Tried"
    run: 'Fetch now', fetching: 'Fetching…', last: 'Fetched {t} · {r}', tried: 'Tried {t} · {r}',
    r_ok: 'OK', r_nodata: 'no new report', r_fail: 'could not reach the service; the last reports are kept', r_yield: 'paused for a page load, tried again',
    r_limited: 'the service asked to wait', r_untrusted: 'certificate not trusted — update fauxcloud', r_busy: 'fauxcloud is busy, try in a minute',
    r_too_soon: 'wait a minute between requests', r_off: 'turn it on first', r_no_location: 'set the location first (Location and time)', r_no_memory: 'not enough memory right now', r_other: 'unexpected answer',
    off_first: 'Turn it on to look airports up.',
    tc_thunder: 'Thunder', tc_snow: 'Snow', tc_rain: 'Rain', tc_fog: 'Fog', tc_gusts: 'Strong gusts',
    loading: 'Loading…', load_fail: 'Could not load this page, try again.',
  });
  const fill = (k, o) => W[k].replace(/\{(\w+)\}/g, (m, n) => esc(o[n] ?? m));
  const U = FC.UNITS, imp = () => FC.imperial();
  const num = (v, d) => (Math.round(v * 10 ** d) / 10 ** d).toFixed(d).replace('-', '−');
  // the user's units: distance km / mi, pressure hPa / inHg, wind km/h / mph
  const dist = km => imp() ? Math.round(km / 1.609344) + ' mi' : km + ' km';
  const press = h => h == null ? '–' : imp() ? num(U.p.f(h), 2) + ' ' + U.p.i : num(h, 1) + ' ' + U.p.m;
  // one template per shape, so a translation can reorder the parts
  const spd = k => imp() ? Math.round(U.w.f(k)) + ' ' + U.w.i : Math.round(k) + ' ' + U.w.m;
  const raw = (k, o) => W[k].replace(/\{(\w+)\}/g, (m, n) => o[n] ?? m);  // escaped once, where the line is drawn
  const wind = x => {
    if (x.wind_kmh == null) return '–';
    const s = spd(x.wind_kmh), w = x.wind_dir === 'vrb' ? raw('wind_vrb', { s }) : x.wind_dir == null ? s : raw('wind_dir', { d: x.wind_dir, s });
    return x.gust_kmh == null ? w : raw('gusts', { w, g: imp() ? Math.round(U.w.f(x.gust_kmh)) : Math.round(x.gust_kmh) });
  };
  const away = km => km == null ? '' : fill(km > 60 ? 'far' : 'near', { d: dist(km) });
  const result = r => own(W, 'r_' + r) || W.r_other;
  // the forecast line: the first window of each phenomenon (the board sends them in priority order); "likely" for the
  // forecast's main line, "possible" for TEMPO / PROB / the BECMG transition. taf null = not asked
  // in an hourly cycle yet ("comes with the next hourly fetch" while on); absent / nil / cnl / not current = no forecast
  const TAF_CODE = { thunder: W.tc_thunder, snow: W.tc_snow, rain: W.tc_rain, fog: W.tc_fog, gusts: W.tc_gusts };  // the phenomena's words (a lazy script: in the page's language at load)
  function forecast(x, on) {
    const t = x && x.taf;
    if (!t) return on ? W.taf_wait : '';
    if (!t.current) return W.taf_na;
    if (!t.next12.length) return W.taf_none;
    const seen = {};
    return t.next12.filter(w => !seen[w.what] && (seen[w.what] = 1) && TAF_CODE[w.what]).map(w => fill((w.possible ? 'taf_p' : 'taf_e') + (w.started ? w.cut ? '_n' : '_u' : w.cut ? '_o' : ''),
      { what: TAF_CODE[w.what], from: hm(w.from), to: hm(w.to) })).join(' ');
  }
  // no "Forecast, next 12 h:" head on the row -- the phrases ("Rain possible 00:00–06:00") say it; "throughout" = the board's 12 h
  // the row's values without the age (the age stands by the status dot)
  function values(x) {
    if (!x || x.obs == null) return W.no_rep;
    return fill('vals', { p: press(x.qnh_hpa), w: wind(x) });
  }
  const status = x => x && x.obs != null ? `<span class="dot${x.stale ? ' off' : !x.cur ? ' late' : ''}" title="${fill('last_rep', { age: ago(x.age_s) })}"></span><span class="hint">${esc(ago(x.age_s))}</span>` : '<span class="dot off"></span>';
  // the row shows the age by the dot and values() below
  // a row names the airport first (its display name, then the code), after its country's flag; the
  // country (`cc`) comes from the firmware's ICAO-prefix table (airport.cpp, shared with the dashboard), none = no flag
  const fl = cc => cc && typeof flag === 'function' && typeof TZ !== 'undefined' && TZ ? flag(cc) : '';
  // the empty field's placeholder is the name that comes back -- the stored one unless the user typed
  // it (`own`), else the last lookup's, else none
  const awc = (a, M) => !a.own ? a.name : ((M.lookup.list || []).find(l => l.icao === a.icao) || {}).name || '';
  // a chosen airport's name is the rename control itself (no Rename word on the row)
  const nameLine = (a, ren) => `${fl(a.cc)}${ren ? `<button type="button" class="nmb" data-a="ren" data-id="${esc(a.icao)}" aria-label="${fill('rename', { id: a.icao })}">` : ''}<b>${esc(a.name || a.icao)}</b>${ren ? '</button>' : ''} <span class="hint">${esc(a.icao)}</span>`;
  function render(M, P, v) {
    if (!M) return `<p class="hint loading">${W.loading}</p>`;
    if (M.err) return `<p class="error">${W.load_fail}</p>`;
    const rows = (P && P.airports) || [], byId = id => rows.find(r => r.icao === id), n = M.airports.length;
    let h = tog('mOn', M.on, W.on, `${W.on_s}<br>${W.on_s2}`);  // the switch the other sheets use; two short lines
    if (M.on && !M.time) h += `<p class="hint">${W.clock}</p>`;
    // three sizes only -- the name 16 px, what the airport says now and its forecast 15 px, the rest 13 px;
    // the report's age beside a status dot in the name line (green while current -- the firmware's `cur`, 75 min --,
    // amber to 90, grey after or none)
    h += `<h3>${W.yours}</h3>` + (n ? '<ul class="list">' + M.airports.map((a, i) => { const x = byId(a.icao), f = forecast(x, !!M.on);
      return `<li class="glass"><div>${v.ren === a.icao ? `<div class="row"><span class="input" style="flex:1;min-width:10em"><input id="mName" value="${esc(a.name || '')}" aria-label="${fill('name_l', { id: a.icao })}" placeholder="${esc(awc(a, M))}"></span><button type="button" class="pill" data-a="nsave" data-id="${esc(a.icao)}">${W.save}</button><button type="button" class="pill" data-a="ncancel">${W.cancel}</button></div><p class="hint">${W.name_h}</p>`
        : `<div class="an">${nameLine(a, !v.ren)} ${status(x)}</div>`}
        <div class="hint">${esc(i ? away(a.km) : a.km == null ? W.main : fill('main_km', { d: away(a.km) }))}</div>
        <div class="av${x && x.obs != null && !x.stale ? '' : ' hint'}">${esc(values(x))}</div>${f ? `<div class="av">${esc(f)}</div>` : ''}</div>
        ${i ? `<button type="button" class="pill" data-a="up" data-id="${esc(a.icao)}" aria-label="${fill('up', { id: a.icao })}"${v.moving ? ' disabled' : ''}>↑</button>` : ''}${i < n - 1 ? `<button type="button" class="pill" data-a="down" data-id="${esc(a.icao)}" aria-label="${fill('down', { id: a.icao })}"${v.moving ? ' disabled' : ''}>↓</button>` : ''}<button type="button" class="pill" data-a="rm" data-id="${esc(a.icao)}"${v.moving ? ' disabled' : ''}>${W.remove}</button></li>`; }).join('') + '</ul>' : `<p class="hint">${W.no_air}</p>`);
    const ref = P && P.reference;
    // the reference only from an airport close enough to compare pressure (the rows' 60-km rule)
    const nearRef = ref && ref.qnh_hpa != null && ref.qnh_from.some(s => s.km != null && s.km <= 60);
    if (nearRef) h += `<p>${fill('ref', { p: press(ref.qnh_hpa), list: ref.qnh_from.map(s => s.icao).join(', ') })}</p>` + (ref.qnh_inconsistent ? `<p class="hint">${W.mixed}</p>` : '');
    // this station against them, live; the words with the dashboard's notice sheet (outlook-why.js)
    if (nearRef && ref.station) {
      if (typeof FCW !== 'undefined') h += `<p>${FCW.rel(ref.station, FCW.M)}</p>`;
      else FC.lazy('/*@asset outlook-why.js*/').then(() => { if (SH && SH.key === 'metar') draw(); }, () => {});
    }
    if (M.on && n) {
      const busy = M.run.state === 'waiting' || M.run.state === 'running';
      // the last fetch in the button's tooltip (the rows show each airport's state); a result on the
      // sheet only right after "Fetch now"
      const last = M.last && M.last.kind !== 'lookup' ? ` title="${fill(/^(ok|nodata)$/.test(M.last.result) ? 'last' : 'tried', { t: hm(M.last.t), r: result(M.last.result) })}"` : '';
      h += `<div class="row"><button type="button" class="btn" data-a="run"${last}${busy ? ' disabled' : ''}>${busy ? W.fetching : W.run}</button></div>`;
      if (v.run) h += `<p class="hint">${esc(result(v.run))}</p>`;
    }
    // adding is rare -- one button opens the search (open at once with no airport, or after a lookup here)
    if (n < 3 && !(v.add || !n || v.asked)) h += `<div class="row"><button type="button" class="btn" data-a="addo">${W.found}</button></div>`;
    else if (n < 3) {
      h += `<h3>${W.found}</h3>`;
      if (!M.on) h += `<p class="hint">${W.off_first}</p>`;
      else {
        h += M.location ? `<div class="row"><button type="button" class="btn" data-a="near">${W.find}</button></div>` : `<p class="hint">${W.loc}</p>`;
        h += `<div class="row"><span class="input" style="width:8em"><input id="mCode" maxlength="4" autocapitalize="characters" aria-label="${W.code}" placeholder="${W.code}"></span><button type="button" class="pill" data-a="code">${W.look}</button></div>`;
        const L = M.lookup;
        // the found list belongs to this sheet's visit -- shown only for a lookup asked here, gone when
        // the sheet closes (v is per visit), hidden at three airports (this block), replaced by the next Find
        if (v.asked && (L.state === 'waiting' || L.state === 'running')) h += `<p class="hint loading">${W.looking}</p>`;
        else if (v.asked && L.state === 'done') {
          const list = L.list.filter(a => !M.airports.some(b => b.icao === a.icao)), ok = L.result === 'ok' || L.result === 'partial';
          if (ok && list.length) h += '<ul class="list">' + list.map(a => `<li class="glass"><div>${nameLine(a)}<br><span class="hint">${esc(!a.taf ? away(a.km) : a.km == null ? W.taf_badge_only : raw('taf_badge', { d: away(a.km) }))}</span></div><button type="button" class="pill" data-a="add" data-id="${esc(a.icao)}"${v.moving ? ' disabled' : ''}>${W.add}</button></li>`).join('') + '</ul>';
          // a code already in the list is found, not unknown
          else if (ok && L.list.length && !L.near) h += `<p class="hint">${fill('have', { id: v.code || L.list[0].icao })}</p>`;
          else if (L.result === 'none' || ok) h += `<p class="hint">${L.near ? W.none : fill('unknown', { id: v.code || '' })}</p>`;
          else h += `<p class="hint">${esc(result(L.result))}</p>`;
          if (L.result === 'partial') h += `<p class="hint">${W.partial}</p>`;
        }
        if (v.err) h += `<p class="error">${esc(result(v.err))}</p>`;
      }
    }
    return h;
  }
  // The chosen ids with `add` placed after the last one nearer than it (km from the chosen list or the
  // lookup; unknown = farthest): in a list still in distance order that is its distance place, and a list the user
  // reordered keeps its rows' order
  function insertByKm(ids, add, m) {
    const all = (m.airports || []).concat((m.lookup && m.lookup.list) || []);
    const km = id => { const x = all.find(a => a.icao === id); return x && x.km != null ? x.km : Infinity; };
    let i = -1;
    ids.forEach((id, j) => { if (km(id) <= km(add)) i = j; });
    return ids.slice(0, i + 1).concat(add, ids.slice(i + 1));
  }
  SHEETS.metar = function () {
    let M = null, P = null, poll = 0, tries = 0;
    const v = {};
    const load = () => Promise.all([api('GET', '/api/admin/metar'), FC.get('/api/metar', { etag: false }).then(r => r.data, () => null)])
      // the home tile reads API.metarLive too: keep it current, or the tile shows the page load's state until a reload
      .then(([m, p]) => { M = m; P = p; API.metar = m; API.metarLive = m && m.on ? p : null; if (SH && SH.key === 'metar') draw(); }, () => { M = { err: 1 }; if (SH && SH.key === 'metar') draw(); });
    const busyNow = () => M && (['waiting', 'running'].includes(M.lookup.state) || ['waiting', 'running'].includes(M.run.state));
    const watch = () => { clearTimeout(poll); poll = setTimeout(() => load().then(() => { if (busyNow() && ++tries < 15 && SH && SH.key === 'metar') watch(); else tries = 0; }), 5000); };
    const save = body => api('POST', '/api/admin/metar', body).then(j => { if (j.errors) toast(j.errors[0]); load(); refresh(); }, noAnswer());
    // every list edit (remove, add, ↑ / ↓) shows at once, from the list on screen; the list's buttons wait for the reload, so a
    // quick second tap starts from the new list and is never lost or undone
    const setList = ids => { const all = M.airports.concat(M.lookup.list || []);
      v.moving = true; M.airports = ids.map(id => all.find(x => x.icao === id)).filter(Boolean); draw();
      const done = () => load().then(() => { v.moving = false; if (SH && SH.key === 'metar') draw(); });
      api('POST', '/api/admin/metar', { airports: ids }).then(j => { if (j.errors) toast(j.errors[0]); done(); refresh(); }, e => { noAnswer()(e); done(); }); };
    const ask = (path, b) => { v.err = ''; b.disabled = true; api('POST', path, {}).then(() => { tries = 0; load().then(watch); }, e => { b.disabled = false; v.err = /409/.test(e.message) ? 'busy' : 'other'; draw(); }); };
    load();
    return { icon: icon('plane'), title: W.on, noSave: true,
      body: () => render(M, P, v),
      change(t) { if (t.id === 'mOn') save({ on: t.checked }); },
      // the board keeps 24 bytes of UTF-8; the field stops there, on a character, so the cut is seen
      input(t) { if (t.id === 'mName') { const c = [...t.value]; while (new TextEncoder().encode(c.join('')).length > 24) c.pop(); if (c.join('') !== t.value) t.value = c.join(''); } },
      click(b) {
        const a = b.dataset.a, ids = M ? M.airports.map(x => x.icao) : [];
        if (a === 'rm' && !v.moving) setList(ids.filter(x => x !== b.dataset.id));
        // one name at a time, in the row; Save sends {names: {ICAO: text}} (the board trims and cuts it)
        if (a === 'ren') { v.ren = b.dataset.id; draw('mName'); }
        if (a === 'addo') { v.add = true; draw(); }
        if (a === 'ncancel') { v.ren = ''; draw(); }
        // an empty name goes back to aviationweather.gov's: one lookup of that code right away fills it (else the code shows
        // until a later lookup)
        if (a === 'nsave') { const id = b.dataset.id, t = $('#mName').value.trim(); v.ren = '';
          if (t) save({ names: { [id]: t } });
          else api('POST', '/api/admin/metar', { names: { [id]: '' } }).then(() => api('POST', '/api/admin/metar/lookup?ids=' + id, {})).then(() => { tries = 0; load().then(watch); }, () => load()); }
        // a new airport goes in by distance (nearest first, unknown last); ↑ / ↓ then set the order
        if (a === 'add' && !v.moving) setList(insertByKm(ids, b.dataset.id, M));
        if ((a === 'up' || a === 'down') && !v.moving) { const i = ids.indexOf(b.dataset.id), j = a === 'up' ? i - 1 : i + 1;
          if (i >= 0 && j >= 0 && j < ids.length) { [ids[i], ids[j]] = [ids[j], ids[i]]; setList(ids); } }
        if (a === 'near') { v.asked = true; ask('/api/admin/metar/lookup?near=1', b); }
        if (a === 'code') { const c = ($('#mCode').value || '').trim().toUpperCase(); if (!/^[A-Z][A-Z0-9]{3}$/.test(c)) return; v.code = c; v.asked = true; ask('/api/admin/metar/lookup?ids=' + c, b); }
        if (a === 'run') { v.run = ''; ask('/api/admin/metar/run', b); }
      } };
  };
  SHEETS.metar.render = render;  // the node render test
  SHEETS.metar.insertByKm = insertByKm;
  SHEETS.metar.W = W;
  SHEETS.metar.fmt = { dist, press, wind, away, values, result, forecast };
})();
