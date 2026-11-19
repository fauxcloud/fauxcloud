'use strict';
// core.js -- shared by the faces: one request in flight for the whole
// page, If-None-Match on every poll (a 304 keeps the previous data), polling tasks that pause while the tab is
// hidden and back off 15 -> 30 -> 60 s on failure, the theme key fc-theme and the units key fc-units.
const FC = (() => {
  const cache = new Map();   // url -> { etag, data }
  const queue = [];          // { url, opts, resolve, reject }
  let busy = null;           // the job in flight
  const hidden = () => document.visibilityState === 'hidden';

  function pump() {
    if (busy || !queue.length) return;
    const job = busy = queue.shift();
    run(job).then(job.resolve, job.reject).finally(() => { busy = null; pump(); });
  }
  async function run(job) {
    const { url, opts } = job, h = Object.assign({}, opts.headers || {});
    const c = cache.get(url);
    if (opts.etag !== false && c && c.etag) h['If-None-Match'] = c.etag;
    const ctl = job.ctl = new AbortController(), timer = setTimeout(() => ctl.abort(), opts.timeout || 15000);
    try {
      // hashed assets (/a/, no ETag) use the browser cache; every JSON stays no-store
      const go = () => fetch(url, { method: opts.method || 'GET', headers: h, body: opts.body,
        cache: opts.etag === false && /^\/a\//.test(url) ? 'default' : 'no-store',
        credentials: 'same-origin', signal: ctl.signal });
      let r = await go();
      // a 503 on a GET is the board pacing a burst of pages (robust.h pagePace) -- asked once more after 1.5 s,
      // by when the earlier answers are acknowledged; never a write, never a second time
      if (r.status === 503 && !opts.method) { await new Promise(res => setTimeout(res, 1500)); r = await go(); }
      const face = r.headers.get('X-Fauxcloud-Face');
      if (face) FC.face = face;
      if (r.status === 304 && c) return { data: c.data, changed: false, status: 304, at: c.at };  // at: when the 200 came
      // a 404 body is data (unknown station), except for a strict get (lazy scripts: an old hash after an OTA)
      if (!r.ok && (opts.strict || r.status - 404)) throw new Error('HTTP ' + r.status);
      const data = opts.text ? await r.text() : opts.blob && !/json/.test(r.headers.get('Content-Type')) ? await r.blob() : await r.json();
      const et = r.headers.get('ETag');
      if (et && opts.etag !== false) cache.set(url, { etag: et, data, at: Date.now() });
      return { data, changed: true, status: r.status };
    } finally { clearTimeout(timer); }
  }
  // GET (or any method with opts.method) through the one-request queue; opts.front puts it first in line.
  function get(url, opts = {}) {
    return new Promise((resolve, reject) => {
      const job = { url, opts, resolve, reject };
      if (opts.front) queue.unshift(job); else queue.push(job);
      pump();
    });
  }

  // ---------------------------------------------------------------- polling tasks
  const tasks = new Map();   // name -> { url(), every, on, err, due, fails, queued, enabled }
  const BACKOFF = [15e3, 30e3, 60e3];
  function poll(name, t) {
    tasks.set(name, Object.assign({ due: 0, fails: 0, queued: false, enabled: true, every: 30e3 }, t));
    tick();
  }
  // a kick while the task is in flight runs it once more after that answer
  function kick(name) { const t = tasks.get(name); if (t) { if (t.queued) t.again = true; t.due = 0; t.enabled = true; tick(); } }
  function enable(name, on) { const t = tasks.get(name); if (t) { t.enabled = on; if (on) { t.due = 0; tick(); } } }
  function tick() {
    if (hidden()) return;
    const now = Date.now();
    for (const t of tasks.values()) {
      if (!t.enabled || t.queued || now < t.due) continue;
      const url = t.url();
      if (!url) continue;
      t.queued = true;
      get(url, { poll: t }).then(res => {
        t.fails = 0; t.due = Date.now() + t.every;
        fresh(res.data);
        if (t.on) t.on(res);
        if (t.again) { t.again = false; t.enabled = true; t.due = 0; setTimeout(tick); }
      }, e => {
        // only our own abort (its own `drop` mark) is asked again without backoff; a kicked task's real failure
        // (`again` from kick()) is reported like any other
        if (t.drop) { t.drop = t.again = false; t.due = 0; setTimeout(tick); return; }
        t.due = Date.now() + BACKOFF[Math.min(t.fails, BACKOFF.length - 1)]; t.fails++;
        if (t.err) t.err(e, t.fails);
      }).finally(() => { t.queued = false; });
    }
  }
  setInterval(tick, 1000);
  // A home-screen app resumes the page it had without asking the server. So every poll answer that names the shells
  // (/api/live: `page` = the dashboard's version, `diag.page` = the admin's) is compared with this page's own
  // (<html data-page>): matching neither, the page reloads once. A stored key holds the pair that caused the reload, so a
  // reload that brings the old shell back never loops, nor does a home-screen app relaunched on it (a new session each
  // time). Never while a lazy script or a write is queued or in flight: the next poll answer decides again.
  function fresh(d) {
    const m = document.documentElement.dataset.page, p = d && d.page, q = d && d.diag && d.diag.page, k = p + ' ' + q;
    if (!p || !m || p == m || q == m || [busy, ...queue].some(j => j && !j.opts.poll)) return;
    // a storage that refuses the key (a private window) leaves no guard, so no reload either
    try { if (localStorage['fc-reload'] != k) localStorage['fc-reload'] = k, location.reload(); } catch (e) {}
  }
  // Back on screen, every poll asks at once (a timeout while hidden would leave a backoff and "Not updating").
  // A poll GET still hanging from before is dropped and asked again; never a lazy script or an admin write.
  document.addEventListener('visibilitychange', () => { if (hidden()) return;
    tasks.forEach(t => { if (t.enabled) t.due = 0; });
    const p = busy && busy.opts.poll; if (p) { p.again = p.drop = true; busy.ctl.abort(); }
    tick(); });

  // ---------------------------------------------------------------- theme, units
  // dark unless the user chose light with the toggle (kept in fc-theme); the system preference is never read
  function theme() { return document.documentElement.dataset.theme || 'dark'; }
  function toggleTheme() {
    const next = theme() === 'dark' ? 'light' : 'dark'; document.documentElement.dataset.theme = next;
    try { localStorage.setItem('fc-theme', next); } catch (e) {}
    bar();
  }
  // the browser / home-screen bar tint follows the in-app theme on every face (each page ships one dark theme-color
  // meta, right until a choice)
  function bar() { const t = document.documentElement.dataset.theme; if (t) for (const m of document.querySelectorAll('meta[name=theme-color]')) m.content = t < 'l' ? '#14366a' : '#a9cbf6'; }
  function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { return null; } return null; }
  try { const t = localStorage.getItem('fc-theme'); if (t) document.documentElement.dataset.theme = t; } catch (e) {}
  bar();
  // input modality: a touch, mouse or pen sets html[data-pointer] and app.css hides the focus ring there (iOS draws it for
  // the focus a closed sheet returns to its tile); a navigation key clears it, so keyboard users keep every ring. Not Enter
  // or Space: a phone keyboard sends them ("Go" saves and closes an admin sheet), and a keyboard user Tabs to a control first
  const dd = document.documentElement.dataset;
  document.addEventListener('pointerdown', () => { dd.pointer = '1'; }, true);
  document.addEventListener('keydown', e => { if (/^(Tab|Arrow\w+|Escape|Home|End|Page\w+)$/.test(e.key)) delete dd.pointer; }, true);
  // metric by default; imperial converts for display only (MQTT stays metric, HA converts itself)
  const imperial = () => store('fc-units') === 'imperial';
  const UNITS = {
    t: { m: '°C', i: '°F', f: v => v * 1.8 + 32, s: 1.8 },
    w: { m: 'km/h', i: 'mph', f: v => v / 1.609344, s: 1 / 1.609344 },
    r: { m: 'mm', i: 'in', f: v => v / 25.4, s: 1 / 25.4 },
    p: { m: 'hPa', i: 'inHg', f: v => v * 0.02953, s: 0.02953 },
  };
  // A script asset loaded on first use (admin-station.js, dash-lg.js, ...): one request through the same queue, the
  // hashed URL is cached for a year like the others. Resolves once the script has run; a failure can be retried.
  const lazies = new Map();
  function lazy(url) {
    if (!lazies.has(url)) {
      lazies.set(url, get(url, { text: true, etag: false, front: true, timeout: 30000, strict: true }).then(res => {
        const s = document.createElement('script'); s.textContent = res.data; document.head.appendChild(s);
      }, e => { lazies.delete(url); throw e; }));
    }
    return lazies.get(url);
  }
  // A text from a W table with its {slots} filled in, values as given (the caller escapes); a count entry
  // ({ one, other, … }) gives its form for o.n (form). English counts here; the dashboard's lang.js sets pr for a pack's language
  const form = (s, n) => typeof s == 'object' ? s[FC.pr(n)] ?? s.other : s;
  const fill = (s, o = {}) => form(s, o.n).replace(/\{(\w+)\}/g, (m, k) => k in o ? o[k] : m);
  // w registers a file's W table: English in the code; lang.js replaces it to apply a language pack. en: a key's English
  // (for what must not follow the page's language: a Home Assistant key made from a label)
  return { get, poll, kick, enable, toggleTheme, store, imperial, UNITS, lazy, face: '', w: (f, t) => t, t: fill, form, pr: n => n == 1 ? 'one' : 'other', en: (t, k) => t[k] };
})();
const $ = s => document.querySelector(s);
// a lookup in a table by a key that came from the network: never the prototype ("constructor", "__proto__")
const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k) ? o[k] : undefined;
// the one documentation link of the faces
const REPO_URL = 'https://github.com/fauxcloud/fauxcloud';  // the public repository
const SITE_URL = 'https://fauxcloud.org/';  // the mark links to the project's site on the dashboard and in Settings
// " · GitHub" after Open source · Licenses on the dashboard's and Settings' footer
const repoLink = () => ` · <a href="${REPO_URL}" target="_blank" rel="noopener">GitHub</a>`;
const WC = FC.w('core', {
  // Pairing step 3, one text for the dashboard, the admin and setup: the page named
  // (not "the same page"), and the key binding (the first key per station is kept; another one waits for Accept)
  wu_step: 'On the console’s page (192.168.1.1), tick <b>Weather Underground</b> and enter a station ID and a key of your choice. fauxcloud remembers the first key it sees; if you change it later, accept the new key in Settings.',
  // ago(): a page in another language says these through Intl (lang.js)
  ago_s: '{n} s ago', ago_min: '{n} min ago', ago_h: '{n} h ago', ago_d: '{n} days ago',
});
const WU_STEP = WC.wu_step;  // setup (English only); the dashboard and the admin read WC.wu_step when they draw
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ---------------------------------------------------------------- shared by the faces
// Day or night from the clock and a rough month table for ~50° N: the sky's fallback when no station tells.
const DAYLIGHT = [[8, 16], [7, 17], [6, 18], [6, 20], [5, 20], [5, 21], [5, 21], [6, 20], [7, 19], [7, 18], [7, 16], [8, 16]];
const clockSky = (t = new Date()) => { const [a, b] = DAYLIGHT[t.getMonth()], h = t.getHours() + t.getMinutes() / 60; return h >= a && h < b ? 'day' : 'night'; };
// The sky's weather beside its state (app.css draws them): [data-rain, data-snow]; rain only -- l / m / h by the rate (< 2.5 mm/h
// light, > 7.6 heavy; a console with only the hourly total = moderate), snow when it rains at <= 1 °C
// v: the station's raw METRIC values (never the page's converted ones); stale: an offline or stale station shows no streaks
const skyFx = (s, v, stale) => { const r = v && v.rain_rate_mmh, t = v && v.temp_c;
  return s !== 'rain' || stale ? ['', ''] : [r == null ? 'm' : r < 2.5 ? 'l' : r > 7.6 ? 'h' : 'm', t != null && t <= 1 ? '1' : '']; };
// sky.css comes the first time the sky is not day (night: moon + stars; rain: streaks or snow), through the one-request queue;
// its three pictures are fetched by the browser when shown, once a year (cached)
// Strict (a 404 after an OTA is an error, never a style), at most three tries a page life, a <style> element;
// then sky.js, which keeps the effects out from under the glass tiles (one <style>, refilled on a retry)
let skyCss = 0, skyTry = 0, skyEl;
const setSky = (s, v, stale) => { const d = document.documentElement.dataset; d.sky = s; [d.rain, d.snow] = skyFx(s, v, stale);
  if (s !== 'day' && !skyCss && skyTry < 3) { skyCss = ++skyTry;
    FC.get('/*@asset sky.css*/', { text: true, etag: false, strict: true }).then(r => {
      (skyEl = skyEl || document.head.appendChild(document.createElement('style'))).textContent = r.data; return FC.lazy('/*@asset sky.js*/'); }).catch(() => { skyCss = 0; }); } };
// A duration, floored, the larger unit pair only: "42 s", "17 min", "5 h 12 min", "1 d 2 h"; "–" if invalid
const dur = s => s == null || !(s >= 0 && s < 1 / 0) ? '–' : (s = Math.floor(s)) < 60 ? s + ' s' : s < 3600 ? (s / 60 | 0) + ' min'
  : s < 86400 ? (s / 3600 | 0) + ' h' + (s % 3600 >= 60 ? ' ' + (s % 3600 / 60 | 0) + ' min' : '') : (s / 86400 | 0) + ' d' + (s % 86400 >= 3600 ? ' ' + (s % 86400 / 3600 | 0) + ' h' : '');
const ago = s => FC.L ? FC.L.ago(s) : s < 90 ? FC.t(WC.ago_s, { n: s }) : s < 5400 ? FC.t(WC.ago_min, { n: Math.round(s / 60) }) : s < 172800 ? FC.t(WC.ago_h, { n: Math.round(s / 3600) }) : FC.t(WC.ago_d, { n: Math.round(s / 86400) });
// Wi-Fi signal as four bars
// Lucide icon paths both the status and the admin face use (ISC; notices: licenses.txt from THIRD_PARTY_LICENSES.md)
const LUCIDE = {
  wind: '<path d="M12.8 19.6A2 2 0 1 0 14 16H2"/><path d="M17.5 8a2.5 2.5 0 1 1 2 4H2"/><path d="M9.8 4.4A2 2 0 1 1 11 8H2"/>',
  thermometer: '<path d="M14 4v10.54a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z"/>',
  'cloud-rain': '<path d="M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242"/><path d="M16 14v6"/><path d="M8 14v6"/><path d="M12 16v6"/>',
  droplet: '<path d="M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15a7 7 0 0 0 7 7z"/>',
  gauge: '<path d="m12 14 4-4"/><path d="M3.34 19a10 10 0 1 1 17.32 0"/>',
};
// Drag a bottom sheet down to close it (phones, < 820 px): follows the finger, closes past a quarter of its height
// or on a flick. canClose() false (admin: unsaved changes) snaps it back and calls blocked().
// While a sheet is open the page behind it is inert (no focus, no clicks, not read out): aria-modal
// alone does not stop a keyboard or a screen reader from leaving the dialog. Both faces call it.
function pageInert(on) {
  for (const el of document.body.children) if (!['sheet', 'scrim', 'toast'].includes(el.id) && el.tagName !== 'SCRIPT') el.inert = on;
}
function sheetDrag(sh, sc, onClose, canClose = () => true, blocked = () => {}) {
  let y0 = null, t0 = 0, dy = 0;
  const reset = () => { sh.style.transform = sh.style.transition = sc.style.opacity = ''; };
  const start = e => {
    if (innerWidth >= 820 || e.target.closest('button')) return;
    y0 = e.clientY; t0 = e.timeStamp; dy = 0; sh.style.transition = 'none'; sh.setPointerCapture(e.pointerId);
  };
  const move = e => {
    if (y0 == null) return;
    dy = Math.max(0, e.clientY - y0); sh.style.transform = `translateY(${dy}px)`;
    sc.style.opacity = String(1 - Math.min(1, dy / sh.offsetHeight));
  };
  const end = e => {
    if (y0 == null) return;
    const flick = dy / Math.max(1, e.timeStamp - t0) > .6; y0 = null;
    sh.style.transition = 'transform .2s ease';
    if ((dy > sh.offsetHeight / 4 || (flick && dy > 30)) && canClose()) { sh.style.transform = 'translateY(100%)'; setTimeout(() => { onClose(); reset(); }, 190); }
    else { reset(); if (dy > 30 && !canClose()) blocked(); }
  };
  [sh.querySelector('.grab') || $('.grab'), sh.querySelector('header')].forEach(el => el && el.addEventListener('pointerdown', start));
  sh.addEventListener('pointermove', move); sh.addEventListener('pointerup', end); sh.addEventListener('pointercancel', end);
}
