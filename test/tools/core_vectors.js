// test/tools/core_vectors.js -- firmware/fauxcloud/web/core.js (FC.get / FC.lazy) in a vm with a stub fetch and DOM.
// A lazy script that answers 404 (a tab left open across an OTA asks for an old hash) is an
// error, never injected as a script, and a later lazy() asks again; a JSON 404 stays data (unknown station).
// Usage: node test/tools/core_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'core.js'), 'utf8');

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

// One fresh page per scenario: routes map a URL to { status, body, type, wait? }; every fetch is recorded.
// storage: the page's localStorage (kept across a reload and a relaunch); reloads counts location.reload();
// env(ctx), when given, changes the page's globals before core.js runs.
function page(routes, storage = {}, env = null) {
  const calls = [], scripts = [], html = [], on = {}, nav = { reloads: 0 };
  const res = (r) => ({
    status: r.status, ok: r.status >= 200 && r.status < 300,
    headers: { get: k => ({ 'content-type': r.type || 'application/json', etag: r.etag })[k.toLowerCase()] || null },
    text: async () => r.body, json: async () => JSON.parse(r.body),
  });
  const ctx = {
    // a route may be a list: one answer per call, the last repeats (a 503 then a 200)
    fetch: async (url, init) => {
      calls.push({ url, cache: init.cache, method: init.method });
      let r = routes[url];
      if (!r) throw new Error('no route ' + url);
      if (Array.isArray(r)) r = r[Math.min(calls.filter(c => c.url === url).length - 1, r.length - 1)];
      if (r.wait) await r.wait;  // an answer the scenario releases
      return res(r);
    },
    document: {
      // listeners by event type, with their options (the modality pair: capture + passive)
      visibilityState: 'visible', addEventListener: (t, f, o) => { (on[t] = on[t] || []).push({ f, o }); }, querySelector: () => null,
      documentElement: { dataset: {} },
      createElement: () => ({ textContent: '' }),
      // as the DOM: appendChild returns the node; its text is read when checked (the sky effects fill a <style> after appending it)
      head: { appendChild: s => { scripts.push(s); return s; }, insertAdjacentHTML: (w, h) => html.push(h) },
    },
    localStorage: Object.assign(storage, { getItem: () => null, setItem() {} }), location: { reload() { nav.reloads++; } },
    matchMedia: () => ({ matches: false }),
    AbortController, setTimeout, clearTimeout, setInterval: () => 0, Promise, Map, Object, JSON, Date, Error, String,
  };
  if (env) env(ctx);
  vm.createContext(ctx);
  vm.runInContext(src + '\n;this.FC = FC; this.skyFx = skyFx; this.setSky = setSky;', ctx);
  // fire(type, event): every listener of that type, as the browser would
  const fire = (t, e = {}) => (on[t] || []).forEach(l => l.f(e));
  return { FC: ctx.FC, calls, get scripts() { return scripts.map(e => e.textContent); }, html, ctx, on, fire, get reloads() { return nav.reloads; } };
}

(async () => {
  // 1. a lazy script answering 404 rejects, injects nothing, and the next lazy() asks again
  {
    const p = page({ '/a/0badf00d/dash-sheet.js': { status: 404, body: 'not found', type: 'text/plain' } });
    let rejected = false;
    await p.FC.lazy('/a/0badf00d/dash-sheet.js').catch(() => { rejected = true; });
    check('lazy 404 rejects', rejected, true);
    check('lazy 404 appends no script', p.scripts.length, 0);
    rejected = false;
    await p.FC.lazy('/a/0badf00d/dash-sheet.js').catch(() => { rejected = true; });
    check('lazy 404 again: a new request, rejected again', [p.calls.length, rejected], [2, true]);
  }
  // 2. a lazy script answering 200 runs once and resolves once; a second lazy() reuses it
  {
    const p = page({ '/a/12345678/dash-sheet.js': { status: 200, body: 'window.x = 1;', type: 'text/javascript' } });
    let n = 0;
    await p.FC.lazy('/a/12345678/dash-sheet.js').then(() => n++);
    await p.FC.lazy('/a/12345678/dash-sheet.js').then(() => n++);
    check('lazy 200: one script, one request', [p.scripts, p.calls.length], [['window.x = 1;'], 1]);
    check('lazy 200: both calls resolve', n, 2);
  }
  // 3. a JSON get answering 404 still resolves with its body (an unknown station is data)
  {
    const p = page({ '/api/live?station=ZZ': { status: 404, body: '{"error":"no such station"}' } });
    const r = await p.FC.get('/api/live?station=ZZ', { etag: false });
    check('JSON 404 is data', [r.status, r.data.error], [404, 'no such station']);
  }
  // 4. any other failure of a JSON get rejects (a 503 only after its one retry)
  {
    const p = page({ '/api/live': { status: 503, body: '{"error":"busy"}' } });
    let msg = '';
    await p.FC.get('/api/live').catch(e => { msg = e.message; });
    check('JSON 503 twice rejects after one retry', [msg, p.calls.length], ['HTTP 503', 2]);
    const q = page({ '/api/live': { status: 500, body: '{}' } });
    msg = '';
    await q.FC.get('/api/live').catch(e => { msg = e.message; });
    check('JSON 500: no retry', [msg, q.calls.length], ['HTTP 500', 1]);
  }
  // 4b. a paced GET (503) is asked once more after 1.5 s and resolves with the second answer
  {
    const p = page({ '/api/hourly?station=ZZ': [{ status: 503, body: '' }, { status: 200, body: '{"hours":[1]}' }] });
    const t0 = Date.now();
    const r = await p.FC.get('/api/hourly?station=ZZ', { etag: false });
    check('503 then 200: resolved with the data', [r.status, r.data.hours, p.calls.length], [200, [1], 2]);
    check('503 then 200: waited about 1.5 s', Date.now() - t0 >= 1400, true);
    const w = page({ '/api/admin/x': { status: 503, body: '' } });  // a write is never asked twice
    let msg = '';
    await w.FC.get('/api/admin/x', { method: 'POST', body: '{}' }).catch(e => { msg = e.message; });
    check('503 on a write: no retry', [msg, w.calls.length], ['HTTP 503', 1]);
  }
  // 5. hashed assets (/a/, no ETag) use the browser cache; JSON stays no-store
  {
    const p = page({ '/a/12345678/tz.json': { status: 200, body: '{"z":[]}' }, '/api/live': { status: 200, body: '{}' },
      '/a/12345678/x.js': { status: 200, body: '1;', type: 'text/javascript' } });
    await p.FC.get('/a/12345678/tz.json', { etag: false });
    await p.FC.get('/api/live');
    await p.FC.get('/api/live', { etag: false });
    await p.FC.lazy('/a/12345678/x.js');
    check('cache per URL kind', p.calls.map(c => c.cache), ['default', 'no-store', 'no-store', 'default']);
  }
  // the sky's weather effects -- rain level by the rate, snow at <= 1 °C, nothing outside rain; sky.css once, when not day
  {
    const p = page({ '/*@asset sky.css*/': { status: 200, body: '.sky::before{}', type: 'text/css' }, '/*@asset sky.js*/': { status: 200, body: 'var SKYJ = 1;', type: 'text/javascript' } }), F = p.ctx.skyFx;
    check('skyFx: day and night carry no rain', [F('day', { rain_rate_mmh: 5 }), F('night', { rain_rate_mmh: 5, temp_c: 0 })], [['', ''], ['', '']]);
    check('skyFx: rate bounds (< 2.5 light, > 7.6 heavy)', [1, 2.49, 2.5, 7.6, 7.61, 30].map(r => F('rain', { rain_rate_mmh: r })[0]), ['l', 'l', 'm', 'm', 'h', 'h']);
    check('skyFx: hourly total only = moderate', [F('rain', { rain_mm: 0.4 })[0], F('rain', null)[0]], ['m', 'm']);
    check('skyFx: snow at <= 1 °C', [1, 0, -5, 1.1, null].map(t => F('rain', { rain_rate_mmh: 1, temp_c: t })[1]), ['1', '1', '1', '', '']);
    // an offline or stale station shows no streaks; metric only: 9 mm/h is heavy, 0.35 in/h would read light
    check('skyFx: stale = nothing', F('rain', { rain_rate_mmh: 9, temp_c: 0 }, true), ['', '']);
    check('skyFx: metric input (9 mm/h heavy, 0 °C snow)', F('rain', { rain_rate_mmh: 9, temp_c: 0 }), ['h', '1']);
    const d = p.ctx.document.documentElement.dataset;
    p.ctx.setSky('day', { rain_rate_mmh: 9 });
    check('setSky day: no sky.css, no effects', [p.calls.length, d.sky, d.rain, d.snow], [0, 'day', '', '']);
    p.ctx.setSky('rain', { rain_rate_mmh: 9, temp_c: 0.5 });
    for (let i = 0; i < 5; i++) await new Promise(r => setTimeout(r, 0));
    check('setSky rain: heavy snow; sky.css as one <style>, then sky.js', [d.sky, d.rain, d.snow, p.calls.map(c => c.url), p.scripts], ['rain', 'h', '1', ['/*@asset sky.css*/', '/*@asset sky.js*/'], ['.sky::before{}', 'var SKYJ = 1;']]);
    p.ctx.setSky('night');
    for (let i = 0; i < 5; i++) await new Promise(r => setTimeout(r, 0));
    check('setSky night: no second fetch, effects cleared', [d.sky, d.rain, d.snow, p.calls.length], ['night', '', '', 2]);
  }
  // a 404 (a page open across an OTA asks for an old hash) appends nothing; at most three tries a page life
  {
    const p = page({ '/*@asset sky.css*/': { status: 404, body: 'not found', type: 'text/plain' } });
    for (let i = 0; i < 6; i++) { p.ctx.setSky(i % 2 ? 'night' : 'rain', { rain_rate_mmh: 1 }); for (let k = 0; k < 5; k++) await new Promise(r => setTimeout(r, 0)); }
    check('sky.css 404: no style, three tries at most', [p.scripts.length, p.html.length, p.calls.length], [0, 0, 3]);
  }
  // sky.js 404 after sky.css arrived: one <style>, refilled on the retry (never a second), sky.js asked again, three tries at most
  {
    const p = page({ '/*@asset sky.css*/': { status: 200, body: '.a{}', type: 'text/css' }, '/*@asset sky.js*/': { status: 404, body: 'not found', type: 'text/plain' } });
    for (let i = 0; i < 6; i++) { p.ctx.setSky('night'); for (let k = 0; k < 8; k++) await new Promise(r => setTimeout(r, 0)); }
    check('sky.js 404: one style, three rounds', [p.scripts, p.calls.filter(c => c.url.includes('sky.js')).length, p.calls.filter(c => c.url.includes('sky.css')).length], [['.a{}'], 3, 3]);
  }
  // input modality: a pointer sets html[data-pointer] (app.css hides the focus ring there), a navigation key clears it,
  // a typed letter, a modifier, Enter or Space (a phone keyboard's "Go" and space) leaves it; both listeners in the capture phase
  {
    const p = page({}), d = p.ctx.document.documentElement.dataset, has = () => 'pointer' in d;
    check('modality: one listener each, capture phase', ['pointerdown', 'keydown'].map(t => (p.on[t] || []).map(l => l.o)), [[true], [true]]);
    check('modality: no mark before any input', has(), false);
    p.fire('pointerdown'); check('modality: pointerdown sets data-pointer', d.pointer, '1');
    p.fire('keydown', { key: 'Tab' }); check('modality: Tab clears it', has(), false);
    p.fire('pointerdown'); p.fire('keydown', { key: 'Escape' }); check('modality: Escape clears it', has(), false);
    const left = ['a', 'Z', 'Shift', 'Control', 'Meta', 'F5', 'Backspace', 'Esc ', 'Tabx'].map(k => { p.fire('pointerdown'); p.fire('keydown', { key: k }); return has(); });
    check('modality: a letter, a modifier or a near-miss key leaves it', left, left.map(() => true));
    const keep = ['Enter', ' '].map(k => { p.fire('pointerdown'); p.fire('keydown', { key: k }); return has(); });
    check('modality: Enter and Space (a phone keyboard) leave it', keep, [true, true]);
    const nav = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown'].map(k => { p.fire('pointerdown'); p.fire('keydown', { key: k }); return has(); });
    check('modality: arrows, Home/End, PageUp/Down clear it', nav, nav.map(() => false));
  }
  // a page an OTA left behind: a poll answer naming another shell version reloads once; the session key stops a loop
  {
    const settle = async () => { for (let i = 0; i < 8; i++) await new Promise(r => setTimeout(r, 0)); };
    // one page life: <html data-page> = mine (none: the setup face), one poll of /api/live answering `live`
    const life = async (mine, live, storage = {}) => {
      const p = page({ '/api/live': { status: 200, body: JSON.stringify(live) } }, storage);
      if (mine) p.ctx.document.documentElement.dataset.page = mine;
      p.FC.poll('live', { url: () => '/api/live' });
      await settle();
      return p;
    };
    check('reload: the same version, nothing', (await life('aaaa1111', { page: 'aaaa1111' })).reloads, 0);
    const tab = {};
    const p1 = await life('aaaa1111', { page: 'bbbb2222' }, tab);
    check('reload: another version, one reload, the pair kept', [p1.reloads, tab['fc-reload']], [1, 'bbbb2222 undefined']);
    p1.FC.kick('live'); await settle();
    check('reload: a second answer before the reload took, still one', p1.reloads, 1);
    // the reload brought the old shell back (iOS served it again): the same answer never reloads a second time
    check('reload: the guard, no loop', (await life('aaaa1111', { page: 'bbbb2222' }, tab)).reloads, 0);
    check('reload: the new shell, nothing', (await life('bbbb2222', { page: 'bbbb2222' }, tab)).reloads, 0);
    check('reload: a later update reloads again', (await life('bbbb2222', { page: 'cccc3333' }, tab)).reloads, 1);
    check('reload: no page key (the demo, another poll), nothing', (await life('aaaa1111', { now: 'x' })).reloads, 0);
    check('reload: the setup face (no version), nothing', (await life('', { page: 'bbbb2222' })).reloads, 0);
    // the admin: its own version is diag.page; the dashboard's `page` differs by nature
    check('reload: admin, its version in diag, nothing', (await life('dddd4444', { page: 'aaaa1111', diag: { page: 'dddd4444' } })).reloads, 0);
    const adm = {};
    check('reload: admin, another diag version, one', [(await life('dddd4444', { page: 'aaaa1111', diag: { page: 'eeee5555' } }, adm)).reloads, adm['fc-reload']], [1, 'aaaa1111 eeee5555']);
    // a storage that refuses the key (a private window): no guard possible, so no reload
    const shut = { get 'fc-reload'() { throw new Error('denied'); }, set 'fc-reload'(v) { throw new Error('denied'); } };
    check('reload: storage refused, nothing', (await life('aaaa1111', { page: 'bbbb2222' }, shut)).reloads, 0);
    // never while a write is queued (one request in flight: the write waits behind the poll); the next answer decides
    {
      let release;
      const p = page({ '/api/live': { status: 200, body: '{"page":"bbbb2222"}', wait: new Promise(r => { release = r; }) },
        '/api/admin/x': { status: 200, body: '{"ok":true}' } });
      p.ctx.document.documentElement.dataset.page = 'aaaa1111';
      p.FC.poll('live', { url: () => '/api/live' });
      const w = p.FC.get('/api/admin/x', { method: 'POST', body: '{}' });
      release(); await settle(); await w;
      check('reload: a write queued behind the poll, none', p.reloads, 0);
      p.FC.kick('live'); await settle();
      check('reload: the next answer with nothing in flight, one', p.reloads, 1);
    }
    // nor while a lazy script is queued
    {
      let release;
      const p = page({ '/api/live': { status: 200, body: '{"page":"bbbb2222"}', wait: new Promise(r => { release = r; }) },
        '/a/12345678/x.js': { status: 200, body: '1;', type: 'text/javascript' } });
      p.ctx.document.documentElement.dataset.page = 'aaaa1111';
      p.FC.poll('live', { url: () => '/api/live' });
      const l = p.FC.lazy('/a/12345678/x.js');
      release(); await settle(); await l;
      check('reload: a lazy script queued, none', p.reloads, 0);
      p.FC.kick('live'); await settle();
      check('reload: the lazy script loaded, the next answer reloads', p.reloads, 1);
    }
    // a reload a write deferred fires on the next answer even when that answer is a 304 (the cached body names the version)
    {
      let release;
      const p = page({ '/api/live': [{ status: 200, body: '{"page":"bbbb2222"}', etag: '"e1"', wait: new Promise(r => { release = r; }) }, { status: 304, body: '' }],
        '/api/admin/x': { status: 200, body: '{"ok":true}' } });
      p.ctx.document.documentElement.dataset.page = 'aaaa1111';
      p.FC.poll('live', { url: () => '/api/live' });
      const w = p.FC.get('/api/admin/x', { method: 'POST', body: '{}' });
      release(); await settle(); await w;
      check('reload: deferred by a write', p.reloads, 0);
      p.FC.kick('live'); await settle();
      check('reload: the 304 answer reloads (cached body)', [p.calls.filter(c => c.url === '/api/live').length, p.reloads], [2, 1]);
    }
  }
  // the theme: dark unless the browser keeps a choice, whatever the system says (every page here runs under a light
  // system theme); the toggle keeps its choice in fc-theme, a reload applies it, and the bar tint (theme-color) follows
  {
    const themePage = (store) => {
      const metas = [{ content: '#14366a' }], asked = [];
      const p = page({}, {}, ctx => {
        ctx.localStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } };
        ctx.matchMedia = q => { asked.push(q); return { matches: true, addEventListener() {} }; };
        ctx.document.querySelectorAll = s => (s === 'meta[name=theme-color]' ? metas : []);
      });
      return { FC: p.FC, el: p.ctx.document.documentElement, meta: () => metas[0].content, asked, store };
    };
    const t = themePage({});
    check('theme: no choice under a light system: nothing set, the dark bar', [t.el.dataset.theme, t.meta()], [undefined, '#14366a']);
    t.FC.toggleTheme();
    check('theme: the first toggle from no choice goes to light (the default was dark)', [t.el.dataset.theme, t.store['fc-theme'], t.meta()], ['light', 'light', '#a9cbf6']);
    t.FC.toggleTheme();
    check('theme: toggle back to dark, kept, the bar retints', [t.el.dataset.theme, t.store['fc-theme'], t.meta()], ['dark', 'dark', '#14366a']);
    check('theme: the system theme is never asked', t.asked.filter(q => /prefers-color-scheme/.test(q)), []);
    const l = themePage({ 'fc-theme': 'light' });
    check('theme: a kept light wins at load, light bar', [l.el.dataset.theme, l.meta()], ['light', '#a9cbf6']);
    l.FC.toggleTheme();
    const r = themePage(l.store);  // a reload with the same storage
    check('theme: a toggle to dark survives a reload', [r.el.dataset.theme, r.meta()], ['dark', '#14366a']);
    r.FC.toggleTheme();
    const r2 = themePage(r.store);
    check('theme: and back to light survives a reload', [r2.el.dataset.theme, r2.meta()], ['light', '#a9cbf6']);
    delete r2.store['fc-theme'];  // the user clears the site's storage
    const c = themePage(r2.store);
    check('theme: cleared storage returns to dark', [c.el.dataset.theme, c.meta()], [undefined, '#14366a']);
  }
  console.log(`core_vectors: ${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})();
