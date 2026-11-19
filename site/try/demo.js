// demo.js -- the /try/ demo's shim: loaded before the page scripts, it answers every /api/* request from the demo data
// (replay.js over data/) and leaves everything else to the network (the hashed assets under /a/ are the release's own
// bytes). It also draws the demo bar. The page's own code is untouched: no Date patch, no page function replaced.
//
// Time: a choice (an example day or a scenario) names a demo time t0. The demo runs from about t0 at the wall clock's
// pace, and every time value in an answer is moved by d, a whole number of hours, so the page's real clock reads the
// demo's newest upload as "now": ages, "today", the 24-hour charts and the forecast's "tomorrow" all agree with Date.
// Whole hours keep the hour records on whole hours in every zone; the price is that the demo starts up to 30 minutes
// from the chosen t0. Day keys move by k whole days so the demo's open day is the viewer's today. A query in page time
// (the history ranges) is moved back before replay.js answers it.
'use strict';
(() => {
  const BASE = '/try/', DATA = BASE + 'data/', HR = 3600, KEEP_H = 6;  // a demo runs 6 h, then starts again at its t0
  // ---- the bar's words (pinned in site/try/demo_text.py)
  const W = {
    region: 'Demo controls',
    pick: 'Example day',
    dayNight: 'Time of day',
    day: 'Day',
    night: 'Night',
    scenarios: 'Scenarios',
    next: 'Live · next upload in {n} s',
    off: 'Live · the console has stopped uploading',
    loading: 'Loading the demo data…',
    failed: 'The demo data did not load. Reload the page to try again.',
    note: 'Demo data: a synthetic year for a station in central Warsaw. Nothing is saved.',
    refused: 'demo: not saved',
  };
  // the example days and the scenarios by their ids in data/index.json
  const EX = {
    winter: 'Winter day',
    spring: 'Spring day',
    summer: 'Summer day',
    autumn: 'Autumn day',
  };
  const SC = {
    today: 'Today',
    sunny: 'Sunny',
    rain: 'Rain',
    snow: 'Snow',
    thunderstorm: 'Thunderstorm',
    'frost-night': 'Frost night',
    'pressure-off': 'Pressure off',
    offline: 'Offline',
    'no-sources': 'No sources',
  };

  const realFetch = window.fetch.bind(window);
  const store = (k, v) => { try { if (v === undefined) return sessionStorage.getItem(k); sessionStorage.setItem(k, v); } catch (e) {} return null; };
  const css = document.createElement('link');
  css.rel = 'stylesheet'; css.href = BASE + 'demo.css';
  document.head.appendChild(css);

  // ---- data
  let I = null, R = null, A = null;  // index.json, the replay, the anchor { key, w0 (ms), ts (s), d (s), k (days) }
  const getJson = f => realFetch(DATA + f).then(r => { if (!r.ok) throw new Error(f + ': HTTP ' + r.status); return r.json(); });
  const script = src => new Promise((ok, no) => { const s = document.createElement('script'); s.src = src; s.onload = ok; s.onerror = () => no(new Error(src)); document.head.appendChild(s); });
  const ready = (async () => {
    const [idx] = await Promise.all([getJson('index.json'), window.DemoReplay ? null : script(BASE + 'replay.js')]);
    I = idx;
    const F = { 'index.json': I }, files = new Set([I.series.file, I.daily.file, I.diag, ...I.learn.map(x => x.file), ...Object.values(I.static)]);
    await Promise.all([...files].map(f => getJson(f).then(j => { F[f] = j; })));
    R = window.DemoReplay.create(F);
    anchor();
  })();

  // ---- the choices: '<example id>-day' / '<example id>-night' or a scenario id -> { t0 (s), sw }
  function choice(key) {
    for (const e of I.example_days) for (const half of ['day', 'night']) if (key === e.id + '-' + half) return { t0: Date.parse(e[half + '_t0']) / 1e3, sw: {} };
    const s = Object.prototype.hasOwnProperty.call(I.scenarios, key) ? I.scenarios[key] : null;
    return s ? { t0: Date.parse(s.t0) / 1e3, sw: s.switches || {} } : null;
  }
  const keyOf = () => { let h = location.hash.slice(1); try { h = decodeURIComponent(h); } catch (e) {} return choice(h) ? h : null; };
  const saved = () => { try { const a = JSON.parse(store('fc-demo')); return a && choice(a.key) ? a : null; } catch (e) { return null; } };
  const expired = (a, w) => !(a.w0 <= w) || a.ts + (w - a.w0) / 1e3 > choice(a.key).t0 + KEEP_H * HR;
  // the anchor of the choice in the address (else the one this tab ran, else the default); kept across the two faces
  function anchor(fresh) {
    const s = saved(), key = keyOf() || (s && s.key) || I.default, w = Date.now();
    let a = s && s.key === key && !fresh && !expired(s, w) ? s : null;
    if (!a) {
      const t0 = choice(key).t0, d = Math.round((w / 1e3 - t0) / HR) * HR, ts = w / 1e3 - d;
      const n = new Date(w), today = Date.UTC(n.getFullYear(), n.getMonth(), n.getDate()) / 864e5;
      a = { key, w0: w, ts, d, k: today - Date.parse(R.dkey(ts) + 'T00:00:00Z') / 864e5 };
      store('fc-demo', JSON.stringify(a));
    }
    A = a;
    if (location.hash.slice(1) !== key) history.replaceState(null, '', '#' + key);
  }
  const tNow = () => {
    if (expired(A, Date.now())) { anchor(true); setTimeout(() => location.reload()); }  // the page's history caches are of the old run
    return Math.floor(A.ts + (Date.now() - A.w0) / 1e3);
  };

  // ---- moving the answers into page time
  const ISO = /^\d{4}-\d\d-\d\dT\d\d:\d\d(:\d\d)?Z$/, DAY = /^\d{4}-\d\d-\d\d$/;
  const isoAt = (ms, sec) => new Date(ms).toISOString().slice(0, sec ? 19 : 16) + 'Z';
  function shift(o) {
    if (Array.isArray(o)) return o.map(shift);
    if (o && typeof o === 'object') { const r = {}; for (const k of Object.keys(o)) r[k] = shift(o[k]); return r; }
    if (typeof o === 'string') return ISO.test(o) ? isoAt(Date.parse(o) + A.d * 1e3, o.length > 17) : DAY.test(o) ? isoAt(Date.parse(o + 'T00:00:00Z') + A.k * 864e5).slice(0, 10) : o;
    // unix seconds (every other number of the API stays far below 1e9)
    if (typeof o === 'number' && Number.isInteger(o) && o >= 1e9 && o < 4e9) return o + A.d;
    return o;
  }
  const p2 = n => String(n).padStart(2, '0');
  function fixLive(st) {
    // the REL hint names its start as a local clock time: the page's zone after the move
    (st && st.notices || []).forEach(n => { if (n.id === 'baro_ref' && n.since) { const t = new Date(n.since);
      n.text = n.text.replace(/ since \d\d:\d\d\./, ` since ${p2(t.getHours())}:${p2(t.getMinutes())}.`); } });
  }
  function fixLearn(L) {
    // unix days and a YYYYMMDD day: whole days
    ((L.l1 && L.l1.cells) || []).forEach(c => { if (typeof c.last === 'number') c.last += A.k; });
    if (L.evening && typeof L.evening.label_day === 'number') L.evening.label_day += A.k;
    if (typeof L.last_day === 'number') { const s = String(L.last_day); L.last_day = +isoAt(Date.parse(`${s.slice(0, 4)}-${s.slice(4, 6)}-${s.slice(6)}T00:00:00Z`) + A.k * 864e5).slice(0, 10).replace(/-/g, ''); }
  }
  // a history range in page time -> demo time: hours by d, a day bound (a page-local midnight) to the station's midnight k days back
  function demoPath(u) {
    const q = new URLSearchParams(u.search);
    if (u.pathname === '/api/hourly') ['from', 'to'].forEach(n => { if (q.has(n)) q.set(n, String(+q.get(n) - A.d)); });
    if (u.pathname === '/api/daily') ['from', 'to'].forEach(n => { if (!q.has(n)) return;
      const t = new Date(+q.get(n) * 1e3); q.set(n, String(R.midnight(Date.UTC(t.getFullYear(), t.getMonth(), t.getDate() - A.k, 12) / 1e3))); });
    const s = q.toString();
    return u.pathname + (s ? '?' + s : '');
  }

  // ---- answers: the board's headers, an ETag per content (the times that count by themselves left out) and a real 304
  const VOLATILE = new Set(['now', 'uptime_s', 'age_s']);
  const fnv = s => { let h = 0x811c9dc5; for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 0x1000193) >>> 0; return h.toString(16).padStart(8, '0'); };
  const json = (status, body, h = {}) => new Response(JSON.stringify(body), { status, headers: Object.assign({ 'Content-Type': 'application/json', 'Cache-Control': 'no-store' }, h) });
  const etagged = (p, q) => (p === '/api/live' && q.has('station')) || ['/api/bridge/status', '/api/recent', '/api/hourly', '/api/daily', '/api/metar'].includes(p);
  // writes the page follows by their status alone: the board's refusal status (a 200 would read as done, e.g. "Erased")
  const JOBS = ['/api/admin/wufc/run', '/api/admin/forwarding/test', '/api/admin/metar/run', '/api/admin/metar/lookup', '/api/admin/factory-reset'];
  let flashT = 0;
  function refusedFlash() {
    const n = bar && bar.querySelector('.demo-note');
    if (!n) return;
    n.classList.remove('flash'); void n.offsetWidth; n.classList.add('flash');
    clearTimeout(flashT); flashT = setTimeout(() => n.classList.remove('flash'), 2400);
  }
  const hex = n => Array.from(crypto.getRandomValues(new Uint8Array(n)), b => p2(b.toString(16))).join('');
  function answer(u, init) {
    const method = (init.method || 'GET').toUpperCase(), p = u.pathname, q = new URLSearchParams(u.search);
    if (method !== 'GET') {
      // signed in without a password; every other write is refused the way the board refuses one: the error body for
      // a save, the refusal status for a job the page follows by its status alone (it never reads that body)
      if (p === '/api/admin/login' || p === '/api/admin/logout') return json(200, { ok: true });
      refusedFlash();
      // a refused change answers the unchanged view with its errors, as the board does (the sheets redraw from it)
      const view = JOBS.includes(p) ? null : R.answer(demoPath(u), tNow(), choice(A.key).sw).body;
      const base = view && typeof view === 'object' && !Array.isArray(view) ? shift(view) : {};
      return json(JOBS.includes(p) ? 409 : 200, Object.assign(base, { ok: false, error: W.refused, errors: [W.refused], detail: W.refused }));
    }
    if (p === '/api/admin/challenge') return json(200, { nonce: hex(16), salt: hex(16), iters: 10000 });
    const sw = choice(A.key).sw, r = R.answer(demoPath(u), tNow(), sw);
    // an admin read the demo has no data for (Wi-Fi scan, boot log): the refusal body, never the catch-all text, which
    // the admin face would take for a session that ended
    if (r.body == null && p.startsWith('/api/admin/')) return json(200, { ok: false, error: W.refused, errors: [W.refused] });
    if (r.body == null) return new Response(r.text, { status: r.status, headers: { 'Content-Type': 'text/plain' } });
    const body = shift(r.body);
    if (p === '/api/live') [body.station, ...(body.stations || [])].forEach(fixLive);
    if (p === '/api/learn') fixLearn(body);
    const h = {};
    if (p === '/api/live' || p === '/api/bridge/status') h['X-Fauxcloud-Face'] = 'lan';
    if (r.status === 200 && etagged(p, q)) {
      const tag = '"' + fnv(A.key + JSON.stringify(body, (k, v) => VOLATILE.has(k) ? undefined : v)) + '"';
      h.ETag = tag;
      if (new Headers(init.headers || {}).get('If-None-Match') === tag) return new Response(null, { status: 304, headers: h });
    }
    return json(r.status, body, h);
  }
  window.fetch = function (input, init) {
    const req = input instanceof Request ? input : null;
    const u = new URL(req ? req.url : String(input), location.href);
    if (u.origin !== location.origin || !u.pathname.startsWith('/api/')) return realFetch(input, init);
    // a Request object keeps its own method, headers and signal unless init names them
    const o = Object.assign(req ? { method: req.method, headers: req.headers, signal: req.signal } : {}, init || {});
    return ready.then(() => {
      if (o.signal && o.signal.aborted) throw new DOMException('The request was aborted.', 'AbortError');
      return answer(u, o);
    });
  };

  // the firmware upload goes by XMLHttpRequest (upload progress): the same answers for an /api/ request there
  const XO = XMLHttpRequest.prototype.open, XS = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (method, url, ...rest) {
    const u = new URL(url, location.href);
    this.demo = u.origin === location.origin && u.pathname.startsWith('/api/') ? { u, method } : null;
    return XO.call(this, method, url, ...rest);
  };
  XMLHttpRequest.prototype.send = function (body) {
    const d = this.demo;
    if (!d) return XS.call(this, body);
    const set = (k, v) => Object.defineProperty(this, k, { value: v, configurable: true });
    const done = (status, text, events) => { set('readyState', 4); set('status', status); set('responseText', text); set('response', text);
      events.forEach(t => this.dispatchEvent(new Event(t))); };
    // no data or a failed answer: the request fails visibly (error), as a lost connection would, never hangs
    ready.then(() => answer(d.u, { method: d.method })).then(r => r.text().then(text => [r.status, text]))
      .then(([status, text]) => done(status, text, ['readystatechange', 'load', 'loadend']),
        () => done(0, '', ['readystatechange', 'error', 'loadend']));
  };

  // ---- the pages' links between the faces, inside /try/
  document.addEventListener('click', e => {
    const a = e.target.closest && e.target.closest('a[href]'), h = a && a.getAttribute('href');
    const to = h === '/admin' ? BASE + 'admin.html' : h === '/' ? BASE : null;
    if (to) { e.preventDefault(); location.href = to + location.hash; }
  }, true);

  // ---- the demo bar
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  let bar = null;
  function pickKey(key) { store('fc-demo', ''); history.replaceState(null, '', '#' + key); location.reload(); }
  function draw() {
    if (!A) { bar.innerHTML = `<p class="demo-note">${esc(W.loading)}</p>`; return; }
    const key = A.key, ex = I.example_days.find(e => key === e.id + '-day' || key === e.id + '-night'), half = ex && key.endsWith('-night') ? 'night' : 'day';
    bar.innerHTML = `<div class="demo-row">
      <select class="demo-pick" aria-label="${esc(W.pick)}">${ex ? '' : `<option value="" selected disabled>${esc(W.pick)}</option>`}${I.example_days.map(e =>
        `<option value="${esc(e.id)}"${ex === e ? ' selected' : ''}>${esc(EX[e.id] || e.id)}</option>`).join('')}</select>
      <span class="demo-seg" role="group" aria-label="${esc(W.dayNight)}">${['day', 'night'].map(x =>
        `<button type="button" data-half="${x}" aria-pressed="${!!ex && half === x}"${ex ? '' : ' disabled'}>${esc(W[x])}</button>`).join('')}</span>
      <span class="demo-chips" role="group" aria-label="${esc(W.scenarios)}">${Object.keys(I.scenarios).map(s =>
        `<button type="button" class="demo-chip" data-sc="${esc(s)}" aria-pressed="${s === key}">${esc(SC[s] || s)}</button>`).join('')}</span></div>
      <p class="demo-note"><span class="demo-live"></span> <span>${esc(W.note)}</span></p>`;
    bar.querySelector('.demo-pick').addEventListener('change', e => pickKey(e.target.value + '-' + half));
    bar.querySelectorAll('[data-half]').forEach(b => b.addEventListener('click', () => { if (ex && b.dataset.half !== half) pickKey(ex.id + '-' + b.dataset.half); }));
    bar.querySelectorAll('[data-sc]').forEach(b => b.addEventListener('click', () => { if (b.dataset.sc !== key) pickKey(b.dataset.sc); }));
    // the chosen scenario in view on a narrow phone (the row scrolls sideways)
    const row = bar.querySelector('.demo-row'), on = bar.querySelector('.demo-chip[aria-pressed="true"]');
    if (on) row.scrollLeft += Math.max(0, on.getBoundingClientRect().right - row.getBoundingClientRect().right + 8);
    live();
  }
  function live() {
    const el = bar && bar.querySelector('.demo-live');
    if (!el || !A) return;
    const late = choice(A.key).sw.late || 0, t = tNow() - late, n = R.lastUp(t) + I.upload_s - t;
    el.classList.toggle('off', late >= 240);
    el.textContent = late >= 240 ? W.off : W.next.replace('{n}', n);
  }
  function mount() {
    bar = document.createElement('div');
    bar.className = 'demo-bar'; bar.setAttribute('role', 'region'); bar.setAttribute('aria-label', W.region);
    document.body.appendChild(bar);
    document.documentElement.classList.add('demo');
    const fit = () => document.documentElement.style.setProperty('--demo-h', bar.offsetHeight + 'px');
    if (window.ResizeObserver) new ResizeObserver(fit).observe(bar);
    draw(); fit();
    ready.then(() => { draw(); setInterval(live, 1000); }, () => { bar.innerHTML = `<p class="demo-note">${esc(W.failed)}</p>`; });
  }
  if (document.body) mount(); else document.addEventListener('DOMContentLoaded', mount);
  window.addEventListener('hashchange', () => { if (keyOf() && A && keyOf() !== A.key) pickKey(keyOf()); });
})();
