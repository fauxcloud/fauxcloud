// dash-wufc.js -- page 2 "Next" of the outlook card, lazy.
// Line 1 = the main airport's TAF: the highest phenomenon's windows in the next 12 h (likely first, then possible),
// thunder > snow > rain > fog > gusts, from /api/metar; line 2 = the WU
// forecast (`wufc` in /api/live). Every line ends with the same text tag, the ICAO code or "WU",
// its tooltip naming the source (no plane glyph, no leading chip). A line is absent without its source; no line, no page 2 (today's
// strip); page 2 never alone. Two dots beside the i (tabs), a 5-s ring around the active dot whose end turns the page (hover / focus
// pause it, a tap or a swipe stops it for good, reduced motion: no ring, no turn). The page and the ring's phase survive a poll's
// re-render. One global FCX: p2(L) -> page 2 or null, card(ol, p2) -> the card, wire() after the grid render, why(L) -> the i sheet's
// Forecast rows and its source line, ph(phrase) -> WU's phrase in the page's words.
const FCX = (() => {
  const W = FC.w('dash-wufc', {
    // page 2 shows a possible window too; its span is the sheet's (day words, "from" at the horizon)
    a: '{what} likely {span}', b: '{what} possible {span}',
    // a window cut at the 12-h horizon has no real end (the board's now + 12 h): no end time, a whole sentence per form
    fr: 'from {from}', fr_tm: 'tomorrow from {from}',
    // a group that began before now (the board's started flag: its from is now): no start time, "throughout" when also cut
    sp_on_o: 'throughout', sp_on: 'until {to}', sp_on_tm: 'until {to} tomorrow',
    // the airport view's day words on a span (dash-airport.js W.span*, kept equal by airport_vectors.js)
    sp: '{from}–{to}', sp_tm: 'tomorrow {from}–{to}', sp_to_tm: '{from}–{to} tomorrow', nw: 'now',
    // the i sheet's Forecast rows -- the span stands in its own column, so a single window says only what and how sure
    l1: '{what} likely', p1: '{what} possible',
    // touching windows of one phenomenon are one row, a whole sentence per order: possible first (pl, plp, plpo) or likely first
    // (lp, lpo), possible again to an end (plp, lp) or to the horizon (plpo, lpo). Its slots are whole time phrases, each from the
    // span words above (tp()): s1 the possible start ("from {from}", "now" for a group that began before now), s2 the likely
    // part, s3 the possible end ("until {to}")
    pl: '{what} possible {s1}, likely {s2}', plp: '{what} possible {s1}, likely {s2}, possible {s3}', plpo: '{what} possible {s1}, likely {s2}, then possible',
    lp: '{what} likely {s2}, possible {s3}', lpo: '{what} likely {s2}, then possible',
    d: '{day}: {phrase} · {hi}° / {lo}°', tomorrow: 'Tomorrow', today: 'Today', d_tomorrow: 'tomorrow', d_today: 'today',
    e: 'Weather Underground forecast', f: '{name} airport forecast (TAF)', g: 'Now', h: 'Next: {what}', i: 'airport, 12 h',
    j: '{phrase}, {hi}° / {lo}°, {pop} % chance of precipitation', k: '{phrase}, {hi}° / {lo}°',
    // the sheet's one source line, a whole sentence per source combination (WU's day: tomorrow; today from 00:00 to 03:00)
    s_t: 'Airport windows from the {name} ({icao}) forecast. Not this station’s readings.',
    s_w: 'Tomorrow from Weather Underground, fetched {t}. Not this station’s readings.',
    s_wt: 'Today from Weather Underground, fetched {t}. Not this station’s readings.',
    s_b: 'Airport windows from the {name} ({icao}) forecast; tomorrow from Weather Underground, fetched {t}. Not this station’s readings.',
    s_bt: 'Airport windows from the {name} ({icao}) forecast; today from Weather Underground, fetched {t}. Not this station’s readings.',
    // the card's name (both pages, its tab list); the TAF phenomena, the {what} of the lines above
    ol: 'Outlook', wt_thunder: 'Thunder', wt_snow: 'Snow', wt_rain: 'Rain', wt_fog: 'Fog', wt_gusts: 'Strong gusts',
  });
  // the glyph for WU's phrase -- ours where one fits, else the cloud
  const WX = [[/thunder|t-storm/i, 'cloud-lightning'], [/snow|sleet|flurr|ice/i, 'snowflake'], [/drizzle/i, 'cloud-drizzle'], [/rain|shower/i, 'cloud-rain'],
    [/fog|mist|haze/i, 'cloud-fog'], [/wind/i, 'wind'], [/partly|mostly (sunny|clear)/i, 'cloud-sun'], [/sunny|clear|fair/i, 'sun']];
  const WHAT = { thunder: 'wt_thunder', snow: 'wt_snow', rain: 'wt_rain', fog: 'wt_fog', gusts: 'wt_gusts' };  // W keys
  const GL = { thunder: 'cloud-lightning', snow: 'snowflake', rain: 'cloud-rain', fog: 'cloud-fog', gusts: 'wind' }, R = Object.keys(GL);
  const fill = (t, v) => t.replace(/\{(\w+)\}/g, (_, k) => esc(v[k]));
  // WU's wxPhraseLong in the page's words, one function for page 2 and the sheet: AM -> Morning,
  // PM -> Afternoon, "/" -> ", ", a plain capitalised word after the first in lower case (T-Storms and the like as received), the
  // first letter upper case. "PM Light Rain" -> "Afternoon light rain", "AM Clouds/PM Sun" -> "Morning clouds, afternoon sun"
  const ph = p => String(p).replace(/\bAM\b/g, 'Morning').replace(/\bPM\b/g, 'Afternoon').replace(/ *\/ */g, ', ')
    .replace(/ [A-Z][a-z]+\b/g, w => w.toLowerCase()).replace(/^./, c => c.toUpperCase());
  let M = null;  // the last /api/metar answer (replaced whole: never kept across an answer without the source)
  // the shown page, its ring's start, stopped by the user, when a hover / focus pause began (0 = running: the CSS pauses the ring
  // on :hover / :focus-within, so the paused span moves t0 on, and a re-render during a pause keeps the phase)
  const X = { i: 0, t0: Date.now(), stop: false, p: 0 };
  // /api/metar while the airport module is on (the station page only; the airport view has its own poll)
  FC.poll('wufcm', { every: 300e3, url: () => !S.ap && S.status && S.status.metar ? '/api/metar' : null, on: r => { M = r.data; if (r.changed) render(); } });
  // the main airport only (never #2), <= 60 km, a current TAF: the airport and its windows of the phenomena we name
  function taf() {
    const a = S.status && S.status.metar && M && M.on && M.airports && M.airports[0], t = a && a.taf;  // module off -> gone
    return !t || !t.current || a.km == null || a.km > 60 ? null : { a, L: (t.next12 || []).filter(w => GL[w.what]) };
  }
  function wu(L) {
    const w = L && L.wufc;
    if (!w || !w.phrase) return null;
    const v = { day: W[w.day] || W.tomorrow, phrase: ph(w.phrase), hi: f0(cv('temp_c', w.hi)), lo: f0(cv('temp_c', w.lo)), pop: w.pop };
    // page 2's line: the words, hi / lo (no bare chance: the sheet says it in a sentence); the sheet's row: the glyph by WU's own
    // phrase, the words, hi / lo, the chance of precipitation
    return { day: w.day, t: w.t, w: v.day, text: fill(W.d, v), row: fill(w.pop == null ? W.k : W.j, v),
      g: (WX.find(x => x[0].test(w.phrase)) || [0, 'cloud'])[1] };
  }
  // touching (or overlapping) windows of one phenomenon as groups, in start order: a group's first likely start is its earliest (lf),
  // its likely end the latest (lt). A likely window joins a group only when it touches the group's likely run -- a
  // possible-only gap between two likely windows is never called likely. o = the group runs to the horizon: its last window
  // carries the board's cut flag (the data stops there, so its end is no forecast end); s = its first window carries the board's
  // started flag (it began before now, so its from is now)
  function groups(T) {
    const G = [];
    T.L.slice().sort((x, y) => x.from < y.from ? -1 : 1).forEach(w => {
      const g = G.filter(g => g.what == w.what && w.from <= g.to && (w.possible || !g.lf || w.from <= g.lt)).pop()
        || G[G.push({ what: w.what, from: w.from, to: '', lf: '', lt: '', s: !!w.started }) - 1];
      if (w.to >= g.to) g.o = w.to > g.to ? w.cut : g.o || w.cut, g.to = w.to;
      if (!w.possible) { g.lf = g.lf || w.from; if (w.to > g.lt) g.lt = w.to; }
    });
    return G;
  }
  // a time phrase in local clock time with the airport view's day rule, from the span words: k 'sp' = {from}–{to}, 'fr' = from
  // {from} (no end: the horizon, or the likely part that follows), 'on' = until {to}. D.t: the day word is still to say -- the first
  // phrase on the next local day says "tomorrow" (a start on it before the times, an end on it after them; an end at its 00:00
  // reads 00:00, local calendar units: a DST day has 23 or 25 h; ends are whole hours); a later phrase of the same sentence does
  // not repeat it
  const day = d => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  function tp(k, f, e, D) {
    const a = new Date(f), b = new Date(e), n = new Date(), td = day(n);
    const t = D.t && (k != 'on' && day(a) > td ? 1 : k != 'fr' && day(b) > td && b - new Date(n.getFullYear(), n.getMonth(), n.getDate() + 1) ? 2 : 0);
    if (t) D.t = 0;
    return fill(W[k == 'fr' ? t ? 'fr_tm' : 'fr' : k == 'on' ? t ? 'sp_on_tm' : 'sp_on' : t == 1 ? 'sp_tm' : t ? 'sp_to_tm' : 'sp'], { from: hm(a), to: hm(b) });
  }
  // a group's span: "{from}–{to}"; cut at the horizon "from ..."; began before now "until ..." ("throughout" when also cut)
  const span = g => g.s && g.o ? W.sp_on_o : tp(g.s ? 'on' : g.o ? 'fr' : 'sp', g.from, g.to, { t: 1 });
  // a group in words; s: the line stands alone (page 2: single windows carry their times; the sheet has a span column).
  // Touching windows: the order's sentence (p = possible first, q = possible after the likely part) with its time phrases
  function say(g, s) {
    const v = { what: W[WHAT[g.what]], span: s && span(g) }, p = g.from < g.lf, q = g.to > g.lt, D = { t: 1 };
    if (!g.lf || !p && !q) return fill(W[!g.lf ? s ? 'b' : 'p1' : s ? 'a' : 'l1'], v);
    v.s1 = p && (g.s ? W.nw : tp('fr', g.from, g.from, D));
    v.s2 = tp(!p && g.s ? 'on' : !q && g.o ? 'fr' : 'sp', g.lf, g.lt, D);  // likely: began before now / to the horizon / a span
    v.s3 = q && tp('on', g.to, g.to, D);  // unused by the "then possible" sentences
    return fill(W[(p ? 'pl' : 'l') + (q ? g.o ? 'po' : 'p' : '')], v);
  }
  function p2(L) {
    const T = taf(), b = wu(L);
    // the highest phenomenon's group, likely ones first; a possible-only line too (plain, a likely one
    // bold) -- page 2 has a line whenever the TAF has a window; no window, no line
    const g = T && groups(T).sort((x, y) => !x.lf - !y.lf || R.indexOf(x.what) - R.indexOf(y.what) || (x.from < y.from ? -1 : 1))[0];
    const a = g && { what: g.what, name: T.a.name, icao: T.a.icao, k: !!g.lf, text: say(g, 1) };
    return a || b ? { a, b, night: !!(L.outlook && L.outlook.in && L.outlook.in.night) } : null;
  }
  const ic = FCO.svg;
  const tag = (t, tip) => ` <abbr class="tag" title="${tip}">${t}</abbr>`;
  const dot = (k, lab) => `<button type="button" role="tab" aria-selected="${!k}" aria-label="${esc(lab)}" title="${esc(lab)}"><svg viewBox="0 0 18 18" aria-hidden="true"><circle class="rg" cx="9" cy="9" r="7"/><circle class="dt" cx="9" cy="9" r="2.6"/></svg></button>`;
  // the card: page 1 = today's strip (dash.js olIn: the warning line belongs to it), page 2 = the lines; both in one grid cell.
  // The shown page is in the markup, so wire() on the new nodes changes nothing and a re-render starts no fade (app.css)
  function card(ol, p) {
    const a = p.a, b = p.b, what = [a && W.i, b && (W['d_' + b.day] || W.d_tomorrow)].filter(Boolean).join(', ');
    return `<div class="glass ol ol2" role="group" aria-label="${W.ol}"><div class="olpages"><div class="olp" aria-hidden="${X.i != 0}">${olIn(ol)}</div>`
      + `<div class="olp" aria-hidden="${X.i != 1}">${a ? ic(GL[a.what]) : ic(p.night ? 'cloud-moon' : 'cloud-sun', 'dim')}<div class="olt">`
      + (a ? `<span class="oln">${a.k ? `<b>${a.text}</b>` : a.text}${tag(esc(a.icao), fill(W.f, a))}</span>` : '')
      + (b ? `<span class="oln">${b.text}${tag('WU', W.e)}</span>` : '')
      + `</div></div></div><div class="olr">${OLI()}<span class="olpg" role="tablist" aria-label="${W.ol}">${dot(0, W.g)}${dot(1, fill(W.h, { what }))}</span></div></div>`;
  }
  // after every grid render: the page and the ring's phase back (a poll never restarts the turn)
  function wire() {
    const ol = document.querySelector('#grid .ol2');
    if (!ol) return void (X.i = 0, X.t0 = Date.now());  // page 2 gone: page 1 next time, the ring from the start (unless stopped)
    const P = ol.querySelectorAll('.olp'), D = ol.querySelectorAll('.olpg button');
    let x0 = null;
    // the pause follows what the CSS sees: on while the card is hovered or holds the focus, its span added to t0 at the end
    const hold = () => { const t = Date.now(); if (ol.matches(':hover,:focus-within')) X.p = X.p || t; else if (X.p) X.t0 += t - X.p, X.p = 0; };
    const show = (n, fresh) => {
      X.i = n;
      if (fresh) X.t0 = Date.now(), X.p = X.p && X.t0;
      P.forEach((p, k) => p.setAttribute('aria-hidden', k != n));
      D.forEach((d, k) => { d.setAttribute('aria-selected', k == n); d.firstChild.firstChild.style.animationDelay = k == n && !fresh ? -Math.min((X.p || Date.now()) - X.t0, 4900) + 'ms' : ''; });
    };
    const stop = n => { X.stop = true; ol.classList.add('stop'); show(n, 1); };
    if (X.stop) ol.classList.add('stop');
    hold();
    show(X.i, 0);
    ['pointerenter', 'pointerleave', 'focusin', 'focusout'].forEach(k => ol.addEventListener(k, () => setTimeout(hold)));  // after :hover / :focus-within settle
    ol.addEventListener('animationend', e => { if (e.animationName == 'olring' && !X.stop && !document.hidden) show(1 - X.i, 1); });
    D.forEach((d, k) => d.addEventListener('click', e => { e.stopPropagation(); stop(k); }));
    ol.addEventListener('pointerdown', e => { x0 = e.clientX; });
    ol.addEventListener('pointerup', e => { if (x0 != null && Math.abs(e.clientX - x0) > 40) ol.dataset.sw = 1, stop(1 - X.i); x0 = null; });
    ol.addEventListener('click', e => { if (ol.dataset.sw) delete ol.dataset.sw, e.stopPropagation(); });  // a swipe is not a tap
  }
  // the i sheet's Forecast (outlook-why.js draws it): {rows: [{s: start ms, g: glyph, w: span or day, t: words, k: likely, tag, tip}]
  // in time order, src: the one source line}, or {} without a row. Touching (or overlapping) windows of one phenomenon are one
  // row spanning the first start to the last end, bold when a part is likely; WU's row is its day word, placed at that day's 07:00
  function why(L) {
    const T = taf(), b = wu(L), rows = [], G = T ? groups(T) : [];
    // a group cut at the horizon spans "from {from}" (no end time)
    G.forEach(g => rows.push({ s: Date.parse(g.from), g: GL[g.what], w: span(g), k: !!g.lf,
      tag: esc(T.a.icao), tip: fill(W.f, T.a), t: say(g) }));
    if (b) {
      const d = new Date(); d.setHours(7, 0, 0, 0); if (b.day !== 'today') d.setDate(d.getDate() + 1);
      rows.push({ s: +d, g: b.g, w: b.w, t: b.row, tag: 'WU', tip: W.e });
    }
    if (!rows.length) return {};
    const k = (G.length ? 's_' + (b ? 'b' : 't') : 's_w') + (b && b.day === 'today' ? 't' : '');
    return { rows: rows.sort((x, y) => x.s - y.s), src: fill(W[k], { name: T && T.a.name, icao: T && T.a.icao, t: b && hm(b.t) }) };
  }
  const css = document.createElement('style');  // the .ol2 block and the sheet's Forecast rows, here rather than in the initial stylesheet
  css.textContent = '.olpages{flex:1;min-width:0;display:grid;align-items:center}.olp{grid-area:1/1;display:flex;align-items:center;gap:14px;min-width:0}'
    + '.olp>svg{width:30px;height:30px;flex:none}.olp>svg.dim{color:var(--ink-3)}.olp[aria-hidden=true]{visibility:hidden}.olr,.olpg{display:flex;align-items:center;gap:8px}'
    + '.olr{align-self:flex-start}.olpg{order:-1}.olpg button{width:20px;height:24px;padding:0;border:0;background:none;cursor:pointer}.olpg svg{width:18px;height:18px;display:block}'
    + '.dt{fill:var(--ink-3)}[aria-selected=true] .dt{fill:var(--ink)}.rg{fill:none;stroke:var(--ink-2);stroke-width:1.6;stroke-dasharray:44;stroke-dashoffset:44;opacity:0;transform:rotate(-90deg);transform-origin:9px 9px}'
    + '[aria-selected=true] .rg{opacity:1;animation:olring 5s linear forwards}.ol2:hover .rg,.ol2:focus-within .rg{animation-play-state:paused}'
    + '.stop [aria-selected=true] .rg{animation:none;stroke-dashoffset:0;opacity:.45}@keyframes olring{to{stroke-dashoffset:0}}'
    + '@media(prefers-reduced-motion:reduce){[aria-selected=true] .rg{animation:none;opacity:0}}.oln{display:block}';
  document.head.appendChild(css);
  return { p2, card, wire, why, ph, W };
})();
