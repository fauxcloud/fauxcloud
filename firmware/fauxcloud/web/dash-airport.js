// dash-airport.js -- the dashboard's airport view, a second page reached by
// the plane tab (dash.js apView). Built from the station page's own parts: the head, the
// outlook strip (.ol: the main airport's weather now + its forecast), five tiles (.lab, .val 40 px, .sub; the other
// airports one line each in the tile's foot, where a station tile has its chart; Wind two rows tall with dash.js's dial) and a strip in the Details bar's shape
// (distances, report ages, the source link). The main airport is the first in the user's order (admin Airport weather
// list). No charts, no history, no tile graphics. Shares dash.js's globals (S, U, IMP, cv, f0, f1, fp, card, hm,
// icon, IC, FCO, esc, ago, dial). Whole phrases with named slots; the forecast phrases are admin-metar.js's family.
const APV = (() => {
  const W = FC.w('dash-airport', {
    title: 'Airport weather', doc: 'Airport weather · fauxcloud',
    // the main airport in the title, one plain subtitle (the others are in the list bar and the sheet)
    title_m: 'Airport weather · {main}', head: 'Main airport. Not this station’s readings.',
    // a tile's foot says whose values they are -- one other airport: a whole line; several: a header, then a row each
    also: '<span>Also {n}:</span><span>{v}</span>', others: 'Others',
    loading: 'Loading airport weather…', fail: 'Could not load the airport weather. It tries again in a minute.',
    off: 'Airport weather is off. Turn it on in Settings, Airport weather.',
    // the strip's line 1: the main airport's weather now, the sheet's headline word (s_*, first match: thunder .. mist, then
    // sky) in one sentence; one template for the 14 sentences, guarded by airport_vectors.js
    n: '{w} at {main}.', n_none: 'No current report from {main}.',
    // the same weather in a visibility line (lower case: a slot inside a line, never re-cased)
    w_thunder: 'thunderstorm', w_freezing: 'freezing rain', w_snow: 'snow', w_rain_l: 'light rain', w_rain: 'rain',
    w_rain_h: 'heavy rain', w_drizzle: 'drizzle', w_fog: 'fog', w_mist: 'mist',
    // the forecast (the main airport's TAF): admin-metar.js's phrases; gusts without a number; one
    // template per certainty, the phenomenon as a word
    taf_head: 'Forecast, next 12 h:', taf_head_to: 'Forecast to {to}:', taf_e: '{what} likely {span}.', taf_p: '{what} possible {span}.',
    taf_none: 'Nothing significant forecast.', taf_na: 'No forecast for this airport.',
    span: '{from}–{to}', span_tm: 'tomorrow {from}–{to}', span_to_tm: '{from}–{to} tomorrow',
    span_o: 'from {from}', span_otm: 'tomorrow from {from}',  // a window cut at the board's 12-h horizon has no end
    // a window that began before now (the board's started flag: its from is now): no start time
    span_on_o: 'throughout', span_on: 'until {to}', span_on_tm: 'until {to} tomorrow',
    // tiles
    // no Humidity tile -- METAR carries none, and whole-degree T / Td only pretended a precision
    t_wind: 'Wind', t_temp: 'Temperature', t_baro: 'Pressure', t_cloud: 'Sky', t_vis: 'Visibility',
    // the wind tile's line under its dial (the dial says the direction): the gust, or a variable wind with its gust
    wv_gust: '<b>variable</b>, gust <b>{g}</b> {u}', wv: '<b>variable</b>', wg: 'gust <b>{g}</b> {u}',
    ow: '<b>{v}</b> {u} {d}', ow_v: '<b>{v}</b> {u} variable', ow_n: '<b>{v}</b> {u}',
    dew: 'dew point <b>{v}</b> {u}', ot: '<b>{v}</b> {u}', sea: 'sea level', op: '<b>{v}</b>',
    oc: '<b>{v}</b> {u} {c}', oc_unk: '{c}, base unknown',
    c_few: 'few clouds', c_scattered: 'scattered clouds', c_broken: 'broken', c_overcast: 'overcast', c_none: 'no cloud',
    // an automatic station's NCD / CLR sees no cloud below about 3.7 km; higher ones may be there, so
    // "No low clouds", never "Clear sky"
    c_cavok: 'clear (CAVOK)', c_nocloud: 'no low clouds',
    // with no ceiling the Clouds headline is the sky in words (a "–" read as missing data)
    hc_clear: 'Clear', hc_nocloud: 'No low clouds', hc_few: 'Few clouds', hc_scattered: 'Scattered clouds',
    hc_broken: 'Broken clouds', hc_overcast: 'Overcast', no_ceil: 'no ceiling',
    // broken or overcast with the base not reported (///): a ceiling of unknown height, never "no ceiling"
    base_unk: 'base unknown',
    // with a ceiling the headline says how high in words (the WMO cloud levels, simplified), the number
    // goes below: under 300 m / under 2 km / under 6 km / above
    hc_vlow: 'Very low clouds', hc_low: 'Low clouds', hc_mid: 'Mid-level clouds', hc_high: 'High clouds', cb_sub: '{c}, base {v} {u}',
    v_ge: '{u} or more', ov: '<b>{v}</b> {u}', ov_w: '<b>{v}</b> {u}, {w}', ov_ge: '<b>{v}</b> {u} or more', ov_ge_w: '<b>{v}</b> {u} or more, {w}',
    o_none: 'no report yet',
    // the strip under the tiles: per airport its distance and its report's age
    f_main: '<b>{n}</b> (main) {km} · {age}', f_main_nokm: '<b>{n}</b> (main) · {age}', f: '<b>{n}</b> {km} · {age}', f_nokm: '<b>{n}</b> · {age}',
    f_main_none: '<b>{n}</b> (main) · no report yet', f_none: '<b>{n}</b> · no report yet',
    src: 'Reports from aviationweather.gov, for reference: not this station’s readings, not stored.',
    // the all-airports sheet; the weather now as a headline word (whole words)
    aps_open: 'All airports', aps_sub: { one: '1 airport', other: '{n} airports, in your order' },
    aps_cap: 'For reference: not this station’s readings.',
    aps_src: 'Reports and forecasts from aviationweather.gov, not stored. The order is yours (Settings, Airport weather).',
    aps_next: 'Next 12 h', aps_to: 'Until {to}',
    s_thunder: 'Thunderstorm', s_freezing: 'Freezing rain', s_snow: 'Snow', s_rain_l: 'Light rain', s_rain: 'Rain', s_rain_h: 'Heavy rain',
    s_drizzle: 'Drizzle', s_fog: 'Fog', s_mist: 'Mist', s_clear: 'Clear', s_nocloud: 'No low clouds', s_partly: 'Partly cloudy',
    s_mostly: 'Mostly cloudy', s_overcast: 'Overcast', s_none: 'No current report',
    // a mini tile: the value on line 1, its addition on line 2 (one pattern in every tile)
    s_v: '{v} {u}', s_dew: 'dew point {v}',  // the sheet's wind is the view's dial and gust line
    // a tile's accessible name -- its measure, the main airport's value, what it opens
    aps_tile: '{t}: {v}. Open all airports',
    // the forecast's phenomena (the TAF codes), the {what} of taf_e / taf_p
    tc_thunder: 'Thunder', tc_snow: 'Snow', tc_rain: 'Rain', tc_fog: 'Fog', tc_gusts: 'Strong gusts',
  });
  const fill = (k, o) => FC.form(W[k], o.n).replace(/\{(\w+)\}/g, (m, n) => o[n] ?? m);
  // glyphs for this view only (Lucide, ISC); fog / mist and thunder are the strip's set (FCO.G), shared
  const GX = {
    cloud: '<path d="M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z"/>',
    eye: '<path d="M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0"/><circle cx="12" cy="12" r="3"/>',
  };
  const svg = (p, c) => `<svg${c ? ` class="${c}"` : ''} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${p}</svg>`;
  const PL = `<svg class="pl" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${IC.plane}</svg>`;

  // ---------------------------------------------------------------- pure formatters (vectors: airport_vectors.js)
  // the weather word key of a report: precipitation and obscuration first, then the sky; '' = nothing to say
  function wxKey(x) {
    const w = x.wx || {};
    if (w.thunder) return 'thunder';
    if (w.freezing) return 'freezing';
    if (w.snow) return 'snow';
    if (w.rain) return w.int < 0 ? 'rain_l' : w.int > 0 ? 'rain_h' : 'rain';
    if (w.drizzle) return 'drizzle';
    if (w.fog) return 'fog';
    if (w.mist) return 'mist';
    return '';
  }
  function skyKey(x) {
    if (x.cavok) return 'clear';
    if (x.cover === 'none') return x.low_conf ? 'nocloud' : 'clear';
    return { few: 'partly', scattered: 'partly', broken: 'mostly', overcast: 'overcast' }[x.cover] || '';
  }
  const GLYPH = { thunder: 'cloud-lightning', freezing: 'cloud-rain', snow: 'snowflake', rain_l: 'cloud-drizzle', rain: 'cloud-rain',
    rain_h: 'cloud-rain-wind', drizzle: 'cloud-drizzle', fog: 'cloud-fog', mist: 'cloud-fog', partly: 'cloud-sun', mostly: 'cloud', overcast: 'cloud' };
  function glyph(k, night) {
    if (k === 'clear' || k === 'nocloud') return night ? svg(FCO.G.moon) : svg(IC.sun);
    if (k === 'partly' && night) return svg(FCO.G['cloud-moon']);
    const n = GLYPH[k];
    return n ? svg(GX[n] || FCO.G[n]) : svg(FCO.G['cloud-sun'], 'dim');
  }
  // a forecast window's span in local clock time (dash.js hm): a window on the next local day says so, one whole
  // template per case; an end at local midnight reads "00:00"
  function span(from, to, now, cut, started) {
    const a = new Date(from), b = new Date(to), day = d => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
    const today = day(now), o = { from: hm(a), to: hm(b) };
    // an end at the next local day's 00:00 (local calendar units: a DST day has 23 or 25 h)
    const midnight = day(b) === new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1).getTime() && b.getHours() === 0 && b.getMinutes() === 0;
    // a window that began before now (started: the board clipped its start to now) and also cut: the whole span
    if (started) return fill(cut ? 'span_on_o' : day(b) > today && !midnight ? 'span_on_tm' : 'span_on', o);
    // a window the board cut at its horizon (cut: the data stops there): no end time
    if (cut) return fill(day(a) > today ? 'span_otm' : 'span_o', o);
    if (day(a) > today) return fill('span_tm', o);
    return fill(day(b) > today && !midnight ? 'span_to_tm' : 'span', o);
  }
  // line 2 of the strip: the main airport's forecast windows; the line only when there is something in it
  // (likely or possible windows) -- no TAF, an old one or "nothing significant" leave the card one line; the all-airports sheet keeps
  // its "Next 12 h" row with those words (a detail view)
  const TAF_CODE = { thunder: 'tc_thunder', snow: 'tc_snow', rain: 'tc_rain', fog: 'tc_fog', gusts: 'tc_gusts' };  // the phenomena's words (W)
  function forecast(x, now) {
    const t = x && x.taf, seen = {};
    const L = t && t.current ? t.next12.filter(w => TAF_CODE[w.what] && !seen[w.what] && (seen[w.what] = 1)).slice(0, 3) : [];
    if (!L.length) return '';
    const to = t.to ? Date.parse(t.to) : NaN;
    const head = to - now.getTime() < 11 * 3600e3 ? fill('taf_head_to', { to: hm(to) }) : W.taf_head;
    return head + ' ' + L.map(w => fill(w.possible ? 'taf_p' : 'taf_e', { what: W[TAF_CODE[w.what]], span: span(w.from, w.to, now, w.cut, w.started) })).join(' ');
  }
  // lengths: cloud base from ft (10 m / 100 ft steps), visibility from m (one decimal under 10, whole above)
  const base = ft => IMP ? [String(Math.round(ft / 100) * 100), 'ft'] : [String(Math.round(ft * 0.3048 / 10) * 10), 'm'];
  function vis(m) {
    const v = IMP ? m / 1609.344 : m / 1000;
    return [v < 10 ? f1(v).replace(/[.,]0$/, '') : f0(v), IMP ? 'mi' : 'km'];  // a decimal point or comma (lang.js)
  }
  const dist = km => IMP ? Math.round(km / 1.609344) + ' mi' : km + ' km';
  const headKey = x => x.cavok ? 'clear' : x.cover === 'none' || (x.cover == null && x.low_conf) ? (x.low_conf ? 'nocloud' : 'clear') : x.cover || '';
  const unk = x => !x.cavok && (x.cover === 'broken' || x.cover === 'overcast');  // with ceil_ft null: base not reported
  const coverWord = x => x.cavok ? W.c_cavok : x.low_conf && (x.cover === 'none' || x.cover == null) ? W.c_nocloud : W['c_' + x.cover] || '';

  // ---------------------------------------------------------------- one tile per field: [main: val, sub], others: line
  const has = x => x && x.obs != null;
  const TILES = [
    // the station page's dial (wnd(), shared with the sheet's cards)
    ['wind', 'var(--wind)', () => icon('wind'), x => wnd(x).concat('aph', f0(cv('wind_kmh', x.wind_kmh)) + ' ' + U.w), x => fill(x.wind_dir === 'vrb' ? 'ow_v' : x.wind_dir == null ? 'ow_n' : 'ow', { v: f0(cv('wind_kmh', x.wind_kmh)), u: U.w, d: x.wind_dir == null || x.wind_dir === 'vrb' ? '' : card(x.wind_dir) })],
    ['temp', 'var(--temp)', () => icon('temp'), x => [`${f1(cv('temp_c', x.t_c))}<small>${U.t}</small>`, x.td_c == null ? '' : fill('dew', { v: f1(cv('temp_c', x.td_c)), u: U.t })],
      x => fill('ot', { v: f1(cv('temp_c', x.t_c)), u: U.t })],
    ['baro', 'var(--baro)', () => icon('baro'), x => [`${fp(cv('pressure_hpa', x.qnh_hpa))}<small>${U.p}</small>`, W.sea], x => fill('op', { v: fp(cv('pressure_hpa', x.qnh_hpa)) })],
    ['cloud', 'var(--ink-2)', () => svg(GX.cloud, 'ic'), x => {
      // no ceiling: the sky in words as the headline (22 px, may wrap), "no ceiling" below ("base unknown" when broken or
      // overcast); nothing known: "–"
      if (x.ceil_ft == null) { const k = headKey(x); return k ? [W['hc_' + k], unk(x) ? W.base_unk : W.no_ceil, 'w'] : ['–', '']; }
      const [v, u] = base(x.ceil_ft), m = x.ceil_ft * 0.3048;
      return [W[m < 300 ? 'hc_vlow' : m < 2000 ? 'hc_low' : m < 6000 ? 'hc_mid' : 'hc_high'], fill('cb_sub', { c: coverWord(x), v, u }), 'w'];
    }, x => { if (x.ceil_ft == null) return unk(x) ? fill('oc_unk', { c: coverWord(x) }) : coverWord(x) || '–'; const [v, u] = base(x.ceil_ft); return fill('oc', { v, u, c: coverWord(x) }); }],
    ['vis', 'var(--ink-2)', () => svg(GX.eye, 'ic'), x => {
      if (x.vis_m == null && !x.cavok) return ['–', ''];
      const [v, u] = x.cavok ? vis(10000) : vis(x.vis_m), ge = x.cavok || x.vis_ge, k = wxKey(x);
      return [`${v}<small>${ge ? fill('v_ge', { u }) : u}</small>`, k ? W['w_' + k] : ''];
    }, x => {
      if (x.vis_m == null && !x.cavok) return '–';
      const [v, u] = x.cavok ? vis(10000) : vis(x.vis_m), ge = x.cavok || x.vis_ge, k = wxKey(x);
      return fill((ge ? 'ov_ge' : 'ov') + (k ? '_w' : ''), { v, u, w: k ? W['w_' + k] : '' });
    }],
  ];
  // the wind as the station page draws it -- dash.js's dial (grey sectors: no hour of history here, the marker
  // where the wind comes from, the speed in the middle), then the gust or a variable wind with its gust: [dial, line under it]
  function wnd(x) {
    const g = x.gust_kmh == null ? null : f0(cv('gust_kmh', x.gust_kmh)), vr = x.wind_dir === 'vrb';
    return [dial({ v: { wind_kmh: cv('wind_kmh', x.wind_kmh), wind_dir: vr ? null : x.wind_dir }, hr: [], hrMin: 60, f: f0 }),
      vr ? fill(g ? 'wv_gust' : 'wv', { g, u: U.w }) : g ? fill('wg', { g, u: U.w }) : ''];
  }
  const tipOf = a => esc(`${a.icao} · ${a.name}${a.km == null ? '' : ' · ' + dist(a.km)}`);
  function tile([key, c, ic, main, other], A) {
    const m = A[0], ok = has(m), [val, sub, vc, lv] = ok ? main(m) : ['–', ''];
    // one row per other airport -- the plane and the name on the left (cut with an ellipsis), the value
    // with its unit on the right, never broken
    const oth = (A.length > 2 ? `<span>${W.others}</span>` : '') + A.slice(1).map(a => { const v = { n: esc(a.name), v: has(a) ? other(a) : W.o_none };
      return `<span class="${has(a) && !a.stale ? '' : 'old'}${A.length > 2 ? '' : ' al'}" title="${tipOf(a)}">${PL}${A.length > 2 ? `<span>${v.n}</span><span>${v.v}</span>` : fill('also', v)}</span>`; }).join('');
    // a tile opens the all-airports sheet, as a station tile opens its history
    const al = fill('aps_tile', { t: W['t_' + key], v: esc(lv || val.replace('<small>', ' ').replace(/<[^>]*>/g, '')) });
    return `<button type="button" class="glass tile apt${vc == 'aph' ? ' aph' : ''}${ok && !m.stale ? '' : ' old'}" style="--c:${c}" data-aps="${key}" aria-haspopup="dialog" aria-label="${al}">
      <span class="lab">${ic()}${W['t_' + key]}</span>${vc == 'aph' ? `<span class="aphd">${val}</span>` : `<span class="val${vc ? ' ' + vc : ''}">${val}</span>`}<span class="sub">${sub ? `<span>${sub}</span>` : ''}</span>
      <span class="sub foot oth">${oth}</span></button>`;
  }
  // the whole view as one string (render test): D = the /api/metar answer, now = a Date
  function html(D, err, now) {
    if (!D) return `<div class="head"><h1>${W.title}</h1><p>${err ? W.fail : W.loading}</p></div>`;
    const A = (D.airports || []).filter(a => a && a.icao);
    if (!D.on || !A.length) return `<div class="head"><h1>${W.title}</h1><p>${W.off}</p></div>`;
    const m = A[0], main = esc(m.name), night = m.night ?? document.documentElement.dataset.sky === 'night';  // the airport's own sun
    // line 1: the main airport's weather now (nothing when its report says nothing about it); no report -> say so
    const cur = has(m) && !m.stale, k = cur ? wxKey(m) || skyKey(m) : '';
    const l1 = !cur ? fill('n_none', { main }) : k ? fill('n', { w: W['s_' + k], main }) : '', l2 = forecast(m, now);
    const strip = A.map((a, i) => {
      const o = { n: esc(a.name), km: a.km == null ? '' : dist(a.km), age: a.age_s == null ? '' : ago(Math.round(a.age_s + (Date.now() - at) / 1e3)) };
      const t = (i ? 'f' : 'f_main') + (!has(a) ? '_none' : a.km == null ? '_nokm' : '');
      // the status dot of the admin list -- green while current (the firmware's `cur`, 75 min),
      // amber to 90, grey after or with no report
      const dot = !has(a) || a.stale ? ' off' : !a.cur ? ' late' : '';
      return `<span class="${has(a) && !a.stale ? '' : 'old'}" title="${tipOf(a)}"><span class="dot${dot}"></span> ${PL}${fill(t, o)}</span>`;
    }).join('');
    return `<div class="head"><h1>${fill('title_m', { main })}</h1><p><span>${PL}${W.head}</span></p></div>
      <div class="glass ol" role="group" data-aps="s">${glyph(k, night)}<div class="olt">${l1 ? `<b>${l1}</b>` : ''}${l2 ? `<span>${l2}</span>` : ''}</div><button type="button" class="oli" data-aps="ol" aria-haspopup="dialog" aria-label="${W.aps_open}">${FCO.svg('info')}</button></div>
      <main class="grid">${TILES.map(t => tile(t, A)).join('')}</main>
      <div class="glass health apstrip">${strip}<a class="src" href="https://aviationweather.gov/" target="_blank" rel="noopener" title="${W.src}">aviationweather.gov</a></div>`;
  }

  // ---------------------------------------------------------------- the all-airports sheet
  // every airport in the user's order, one card each: the round flag (the firmware's `cc`; its pack `fp` is the admin's
  // flags-<fp>.json, fetched on first need), name, code, the list's dot and the age; left the weather now as a word and the
  // forecast windows (likely first, bold); right a mini tile per measure (wind, temperature, pressure, sky, visibility: the
  // value, then its addition). It is dash.js's #sheet in its "why" mode (no range tools): `why` holds this body's render, so
  // the page's refresh, Escape, drag and close treat it as the outlook's.
  const PK = ['/*@asset flags-0.json*/', '/*@asset flags-1.json*/', '/*@asset flags-2.json*/', '/*@asset flags-3.json*/',
    '/*@asset flags-4.json*/', '/*@asset flags-5.json*/', '/*@asset flags-6.json*/', '/*@asset flags-7.json*/',
    '/*@asset flags-8.json*/', '/*@asset flags-9.json*/'];
  const FL = {}, FLP = new Set(), FLF = {}, apsOn = () => why === body && !$('#sheet').hidden;
  function flagOf(a) {
    if (!a.cc) return '';
    if (FL[a.cc]) return `<img class="flag" alt="" src="data:image/svg+xml,${encodeURIComponent(FL[a.cc])}">`;
    const p = a.fp;
    if (PK[p] && !FLP.has(p)) { FLP.add(p); FC.get(PK[p], { etag: false }).then(r => { Object.assign(FL, r.data); if (apsOn()) body(); },
      () => { if ((FLF[p] = (FLF[p] || 0) + 1) < 3) FLP.delete(p); }); }  // three tries, then no flag
    return '';
  }
  // the forecast windows of one airport: [head, [[text, class]]]; null without an hourly cycle yet (as the strip)
  function windows(x, now) {
    const t = x.taf;
    if (!t) return null;
    if (!t.current) return [W.aps_next, [[W.taf_na, 'q']]];
    const to = t.to ? Date.parse(t.to) : NaN, seen = {};
    const L = t.next12.filter(w => TAF_CODE[w.what]).sort((a, b) => !!a.possible - !!b.possible)
      .map(w => [fill(w.possible ? 'taf_p' : 'taf_e', { what: W[TAF_CODE[w.what]], span: span(w.from, w.to, now, w.cut, w.started) }), w.possible ? '' : 'lk'])
      .filter(l => !seen[l[0]] && (seen[l[0]] = 1));
    return [to - now.getTime() < 11 * 3600e3 ? fill('aps_to', { to: hm(to) }) : W.aps_next, L.length ? L : [[W.taf_none, 'q']]];
  }
  // a mini tile's two lines, per TILES key (no humidity: METAR reports T and dew point in whole degrees; a humidity
  // derived from them is too coarse to show)
  const MV = {
    wind: x => x.wind_kmh == null ? ['–'] : wnd(x),
    temp: x => x.t_c == null ? ['–'] : [fill('s_v', { v: f1(cv('temp_c', x.t_c)), u: U.t }), x.td_c == null ? '' : fill('s_dew', { v: f1(cv('temp_c', x.td_c)) })],
    baro: x => x.qnh_hpa == null ? ['–'] : [fill('s_v', { v: fp(cv('pressure_hpa', x.qnh_hpa)), u: U.p })],
    cloud: x => {
      if (x.ceil_ft == null) { const k = headKey(x); return k ? [W['hc_' + k], unk(x) ? W.base_unk : ''] : ['–']; }
      const [v, u] = base(x.ceil_ft), m = x.ceil_ft * 0.3048;
      return [W[m < 300 ? 'hc_vlow' : m < 2000 ? 'hc_low' : m < 6000 ? 'hc_mid' : 'hc_high'], fill('cb_sub', { c: coverWord(x), v, u })];
    },
    vis: x => {
      if (x.vis_m == null && !x.cavok) return ['–'];
      const [v, u] = x.cavok ? vis(10000) : vis(x.vis_m), k = wxKey(x);
      return [fill('s_v', { v, u: x.cavok || x.vis_ge ? fill('v_ge', { u }) : u }), k ? W['w_' + k] : ''];
    },
  };
  function cardOf(a, now, night) {
    const ok = has(a) && !a.stale, k = ok ? wxKey(a) || skyKey(a) : '', L = windows(a, now);
    const age = has(a) ? a.age_s + (Date.now() - at) / 1e3 : 0, dot = !ok ? ' off' : !a.cur ? ' late' : '';  // the firmware's `cur`
    // the view's layout -- Wind two rows tall with the dial (a phone: full width, a small dial left),
    // Temperature, Pressure, Sky, Visibility 2 x 2 beside it; no Humidity (METAR carries none)
    const tiles = TILES.filter(t => MV[t[0]]).map(([key, c, ic]) => { const [m, s] = has(a) ? MV[key](a) : ['–'], d = m[0] == '<';
      return `<li${key == 'wind' ? ' class="apw"' : ''} style="--c:${c}"><span class="apc-l">${ic()}${W['t_' + key]}</span>${d ? m : ''}<span class="apc-v">${d ? '' : `<b>${m}</b>`}${s ? `<small>${s}</small>` : ''}</span></li>`; }).join('');
    return `<section class="apc${ok ? '' : ' old'}" aria-label="${esc(a.name)}"><div class="apc-n">${flagOf(a)}<b>${esc(a.name)}</b> <span class="hint">${esc(a.icao)}</span> <span class="dot${dot}"></span><span class="hint">${has(a) ? ago(Math.round(age)) : W.o_none}</span></div>
      <div class="apc-body"><div><div class="apc-nw">${glyph(k, a.night ?? night)}${!ok ? `<b>${W.s_none}</b>` : k ? `<b>${W['s_' + k]}</b>` : ''}</div>${L ? `<p class="apc-fh">${L[0]}</p><ul class="apc-taf">${L[1].map(([t, c]) => `<li${c ? ` class="${c}"` : ''}>${t}</li>`).join('')}</ul>` : ''}</div>
      <ul class="apc-tiles">${tiles}</ul></div></section>`;
  }
  // the sheet as [subtitle, body] (render test)
  function sheet(D, now, night) {
    const A = D && D.on ? (D.airports || []).filter(a => a && a.icao) : [];
    return [fill('aps_sub', { n: A.length }), `<p class="cap">${W.aps_cap}</p>${A.map(a => cardOf(a, now, night)).join('')}<p class="cap ft">${W.aps_src}</p>`];
  }
  function body() {
    const [sub, h] = sheet(D, new Date(), document.documentElement.dataset.sky === 'night');
    $('#shSub').textContent = sub; $('#shBody').innerHTML = h;
  }
  function openAps(from, byKey) {
    why = body; lastFocus = from; $('#sheet').classList.add('why', 'aps');
    $('#shTitle').innerHTML = svg(IC.plane, 'ic') + W.title; $('#shNow').innerHTML = '';
    $('#scrim').hidden = false; $('#sheet').hidden = false; pageInert(true); document.body.style.overflow = 'hidden';
    body(); $('#shScroll').scrollTop = 0; (byKey ? $('#shClose') : $('#sheet')).focus();
  }
  // the strip (focus returns to its ⓘ) and every tile; a text selection is not a tap
  $('#apv').addEventListener('click', e => {
    const t = e.target.closest('[data-aps]');
    if (t && getSelection().type !== 'Range') openAps(t.dataset.aps === 's' ? t.querySelector('.oli') : t, e.detail === 0);
  });
  // the sheet's CSS, once (not in the initial page: the dashboard's budget)
  document.head.insertAdjacentHTML('beforeend', '<style>@media (min-width:820px){.sheet.aps{max-height:72vh}}' +
    // the Wind tile two rows tall in the 3-column grid; a phone: the full width at a tile's height, a 112-px dial left
    '.aphd{display:grid;place-items:center;padding:4px 0}.aphd .dial{max-width:230px}@media (min-width:820px){.grid>.aph{grid-row:span 2}}' +
    // on a phone the tile spans the width with a large dial (about two thirds of it, at most 200 px), the gust and the foot under it
    '@media (max-width:819px){.grid>.aph{grid-column:1/-1}.aphd .dial{width:65%;max-width:200px}}' +
    // "Also {name}: {value}" keeps the whole name; in a narrow tile the value wraps under it
    '.apt .oth>.al{flex-wrap:wrap}.apt .oth>.al>:nth-child(2){flex:1 0 auto;max-width:100%}' +
    '.apc{border-radius:14px;padding:12px 14px;margin-bottom:10px;background:color-mix(in srgb,var(--ink) 6%,transparent)}.apc ul{list-style:none;margin:0;padding:0}' +
    '.apc-n{display:flex;flex-wrap:wrap;align-items:baseline;gap:4px 8px;margin-bottom:10px}.apc-n>.flag,.apc-n>.dot{align-self:center}.apc-n b{font-size:16px;font-weight:600}' +
    '.apc .flag{width:22px;height:22px;border-radius:50%;flex:none;box-shadow:0 0 0 1px var(--grid)}' +
    '.apc-body{display:grid;grid-template-columns:minmax(170px,1fr) 2fr;gap:12px 18px;align-items:start}' +
    '.apc-nw{display:flex;align-items:center;gap:10px}.apc-nw svg{width:28px;height:28px;flex:none;color:var(--ink)}.apc-nw svg.dim{color:var(--ink-3)}.apc-nw b{font-size:18px;font-weight:600;color:var(--ink)}' +
    '.apc-fh{margin:12px 0 2px;font-size:12px;font-weight:600;letter-spacing:.04em;text-transform:uppercase;color:var(--ink-3)}' +
    '.apc-taf{font-size:14px;color:var(--ink-2);display:grid;gap:2px}.apc-taf .lk{color:var(--ink);font-weight:600}.apc-taf .q{color:var(--ink-3)}' +
    '.apc-tiles{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}' +
    '.apc-tiles li{display:grid;gap:2px;align-content:start;min-width:0;min-height:73px;padding:8px 10px;border-radius:10px;background:color-mix(in srgb,var(--ink) 5%,transparent);box-shadow:inset 0 0 0 1px var(--hair)}' +
    '.apc-l{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--ink-3)}.apc-l svg{width:15px;height:15px;flex:none;color:var(--c)}' +
    '.apc-v{display:grid;line-height:1.35}.apc-v b{font-size:15px;font-weight:600;color:var(--ink)}.apc-v small{font-size:13px;color:var(--ink-3)}.apc.old .apc-v b{color:var(--ink-3)}' +
    '.apw{grid-row:span 2}.apw .dial{max-width:150px}' +
    '@media (max-width:599px){.apc-body{grid-template-columns:1fr}.apc-tiles{grid-template-columns:repeat(2,minmax(0,1fr))}' +
    '.apw{grid-column:1/-1;grid-row:auto}.apw .dial{width:65%;max-width:200px}}</style>');

  // ---------------------------------------------------------------- the view
  // one GET /api/metar on open, then every 5 min while the view is shown (url null otherwise: no request); one request
  // in flight and the hidden-tab pause come from FC.poll (the page-burst rule)
  // `at` = when the last 200 brought `D` (dash.js S.liveAt): a 304 replays the same body, the ages count on from it; a first
  // 304 (the station page's poll fetched the same answer earlier) takes the cached answer's own time
  let D = null, err = false, at = Date.now();
  function render() {
    if (!S.ap) return;
    document.title = W.doc;
    $('#apv').innerHTML = html(D, err, new Date());
    // an open sheet follows the new answer; its way back is the re-rendered twin of the tile it came from
    if (apsOn()) { body(); if (lastFocus && !lastFocus.isConnected) lastFocus = $(`#apv [data-aps="${lastFocus.dataset.aps}"]`) || $('#apTab'); }
  }
  // registered off, so the first open asks once (FC.kick enables it); later opens ask at once too
  FC.poll('metar', { every: 300e3, enabled: false, url: () => S.ap ? '/api/metar' : null,
    // the first answer renders even as a 304 (the station page may hold the same ETag already): no "Loading…" until a change
    on: res => { const d0 = D; if (res.status !== 304 || !D) at = res.at || Date.now(); D = res.data; err = false; if (res.changed || !d0) render(); },
    err: () => { err = true; if (!D) render(); } });
  // the strip's ages count on between polls
  setInterval(() => { if (S.ap && D && document.visibilityState !== 'hidden') render(); }, 30e3);
  return { open() { render(); FC.kick('metar'); }, render, html, W, fmt: { wxKey, skyKey, span, forecast, base, vis, coverWord, headKey }, sheet, windows, MV };
})();
