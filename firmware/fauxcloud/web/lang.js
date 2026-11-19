// lang.js -- the dashboard's and the admin's language (status.js and admin.js; setup stays English). The language is this
// browser's own (one choice for both faces, same origin): a pick in this browser (localStorage fc-lang), else the first of the browser's languages that is English
// or has a pack in this image, else English. English lives in each file's W table; a pack (one per script asset,
// hashed, lazy) overwrites a registered table's values in place, by key, before that file first draws: the initial packs
// go first in the one-request queue (before the first poll), FC.lazy fetches a script's pack before the script.
// A pack that does not load in three tries leaves English (never a loop, never cached as content). Counts, decimal
// commas, relative times and month names come from Intl in the page's language; English keeps its own formatters.
const FCL = (() => {
  const P = /*@langs*/{};  // language -> { n: its own name, <script asset>: pack URL }; _i: this page's initial scripts
  const I = P._i || [], MAIN = I[I.length - 1];
  delete P._i;
  const REG = {}, PK = {}, got = {}, tries = {}, done = {};
  // no pick yet (or 'auto', stored by an earlier image): the browser's language
  const pick = () => { const s = FC.store('fc-lang'); if (s && s !== 'auto') return s;
    for (const l of navigator.languages || [navigator.language || '']) { const b = String(l).slice(0, 2).toLowerCase(); if (b === 'en' || own(P, b)) return b; }
    return 'en'; };
  let lang = pick(), shown = 'en';  // shown: the language the page speaks, once its main pack is in
  if (!own(P, lang)) lang = 'en';
  // only keys the English table has, and only a value of the same kind (a text, or a count entry); a table registered
  // empty (the firmware's notices: English shows the firmware's own words) takes every key its pack brings
  const apply = (t, p) => { const all = !Object.keys(EN.get(t) || t).length;
    for (const k in p) if (all ? k !== '__proto__' : own(t, k) !== undefined && typeof p[k] == typeof t[k]) t[k] = p[k]; };
  // a file may register several tables under its name (their keys differ): a pack's key lands in the one that has it
  const EN = new Map();  // table -> its English, kept before a pack overwrites it
  const w = (f, t) => { (REG[f] = REG[f] || []).push(t); EN.set(t, Object.assign({}, t)); if (own(PK, f) && shown === lang) apply(t, PK[f]); return t; };
  FC.en = (t, k) => (EN.get(t) || t)[k];
  // the shell's own words: data-t="file.key" (the text), data-tl (aria-label), data-tt (title); a word is swapped only
  // while its element still shows the shell's English (a render may have replaced it before a pack lands)
  const SW = [];
  document.querySelectorAll('[data-t],[data-tl],[data-tt]').forEach(e => {
    for (const [a, at] of [['t', ''], ['tl', 'aria-label'], ['tt', 'title']]) if (e.dataset[a]) SW.push([e, a, at, at ? e.getAttribute(at) : e.textContent]); });
  const shell = () => SW.forEach(([e, a, at, en]) => {
    const k = e.dataset[a].split('.'), v = k[1] && own(PK, k[0]) && own(PK[k[0]], k[1]);
    if (typeof v == 'string' && (at ? e.getAttribute(at) : e.textContent) === en) at ? e.setAttribute(at, v) : e.textContent = v; });
  // the packs in hand go into the tables and the shell only once the main pack is in (shown === lang): another script's
  // pack that lands first waits for it, and a page whose main pack never loads stays English, words and formats alike
  const land = () => { if (shown !== lang) return; for (const f in PK) if (own(REG, f)) REG[f].forEach(t => apply(t, PK[f])); shell(); };
  // one script asset's pack: at most three tries a page life, resolves either way
  const pack = a => {
    const u = own(P, lang) && own(P[lang], a);
    if (!u || done[a]) return Promise.resolve();
    if (!got[a]) {
      if ((tries[a] = (tries[a] | 0) + 1) > 3) return Promise.resolve();
      got[a] = FC.get(u, { etag: false, front: true, strict: true, timeout: 15000 }).then(r => {
        done[a] = true; Object.assign(PK, r.data);
        if (a === MAIN) speak();
        land(); menu();
      }, () => { got[a] = null; });
    }
    return got[a];
  };
  const first = a => pack(a).then(() => done[a] || tries[a] >= 3 || first(a));
  FC.w = w;
  w('core', WC);  // core.js and outlook.js run before this file
  if (typeof FCO != 'undefined') w('outlook', FCO.W);
  // a script's pack before the script; outlook.js loads lazily on the admin: its table registers once it is in
  const lz = FC.lazy;
  FC.lazy = u => pack(u.slice(u.lastIndexOf('/') + 1)).then(() => lz(u)).then(r => {
    if (!own(REG, 'outlook') && typeof FCO != 'undefined') w('outlook', FCO.W);
    return r; });
  // the language's counts, decimal mark, relative times and month names, once the main pack (its words) is in. An older
  // browser without one of these (Safari < 14 has no RelativeTimeFormat) keeps the English formatters, all of them
  function speak() {
    try {
      const pr = new Intl.PluralRules(lang), rt = new Intl.RelativeTimeFormat(lang, { style: 'short' }), mo = new Intl.DateTimeFormat(lang, { month: 'short' });
      const dp = new Intl.NumberFormat(lang).formatToParts(.5).find(p => p.type === 'decimal').value;
      FC.pr = n => pr.select(n);
      FC.L = {
        dp,
        ago: s => !isFinite(s) ? '–' : s < 90 ? rt.format(-s, 'second') : s < 5400 ? rt.format(-Math.round(s / 60), 'minute')
          : s < 172800 ? rt.format(-Math.round(s / 3600), 'hour') : rt.format(-Math.round(s / 86400), 'day'),
        mon: t => mo.format(t).replace(/\.$/, ''),
      };
    } catch (e) {}
    document.documentElement.lang = shown = lang;
  }
  if (lang !== 'en') I.forEach(first);
  // the button's label, each language by its own name (English, then each pack's)
  const WL = FC.w('lang', { a: 'Language: {name}. Switch to {next}' });
  const LS = ['en'].concat(Object.keys(P)), next = () => LS[(LS.indexOf(shown) + 1) % LS.length];
  const nameOf = l => l === 'en' ? 'English' : P[l].n;
  // the top bar's two letters, a button like the units one: it shows the language the page speaks, and a tap switches to
  // the other one (with two languages a toggle; a third would want a menu), stores it and reloads. Before the first tap
  // the page follows the browser's language. The admin draws its top bar later (and again): it calls FCL.menu() after
  // each draw; one click listener for both faces
  function menu() {
    const btn = $('#langBtn');
    if (!btn || !Object.keys(P).length) return;
    const t = FC.t(WL.a, { name: nameOf(shown), next: nameOf(next()) });
    btn.textContent = shown.toUpperCase(); btn.setAttribute('aria-label', t); btn.title = t; btn.hidden = false;
  }
  // No reload while an admin sheet holds unsaved changes (it would drop them), nor when the storage refused the choice (a
  // private window): the page would come back in the same language
  document.addEventListener('click', e => { if (!(e.target.closest && e.target.closest('#langBtn'))) return;
    if (typeof changed == 'function' && Object.keys(changed()).length) return;
    const l = next(); FC.store('fc-lang', l);
    if (FC.store('fc-lang') === l) location.reload(); });
  menu();
  return { get lang() { return lang; }, P, pack, menu };
})();
