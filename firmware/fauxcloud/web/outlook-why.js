// outlook-why.js -- lazy: the dashboard's ⓘ explanation and the admin's
// "Weather description" sheet, from the outlook block's codes and its `in` readings. One global FCW. The rules and
// thresholds are the firmware's (src/forecast.h); this file only says them in words: whole sentences with named
// placeholders, one per variant, every number through the page's formatters.
// f: { t (temperature), dt (a temperature change, no offset), w (wind), r (rain amount), rr (rain rate),
//      p (a pressure change, signed), hm (an ISO time as HH:MM), km (a distance in km, shown in the page's unit) }
const FCW = (() => {
  const e = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  // both faces draw the blocks below (the Learning sheet's style)
  const P = FC.w('outlook-why', {
    neutral: 'The faint sun-and-cloud icon is a neutral sign, not a sky reading: sun and cloud are not measured yet.',
    // the note of a temperature block (icy, freezing, severe frost, hot, ground frost)
    sun: 'In direct sun with light wind the sensor can read high.',
    // one sentence per learner whose learned value was used for a line (o.in.learn): the note of that line's block
    u_l1: 'Uses what this station learned about its light sensor on clear days.',
    u_l2: 'Uses what this station learned about its humidity sensor.',
    u_l5: 'Uses what this station learned about how fast its clear evenings cool.',
    u_l6: 'Uses what this station learned about its clear, calm nights.',
  });
  const fill = (k, v, T = P) => T[k].replace(/\{(\w+)\}/g, (_, n) => e(v[n]));
  // the dashboard's ⓘ as blocks: glyph, the strip's own phrase as the title with its key number, one
  // sentence under it; warnings first, then the state, the qualifier, the pressure line. Whole sentences.
  const B = FC.w('outlook-why', {
    rain_since: 'Wet since {since}. Light below {lo}, heavy above {hi}.',
    rain: 'Light below {lo}, heavy above {hi}.',
    sleet_since: 'Wet since {since}. At or below {lim} the gauge’s precipitation may be sleet or snow.',
    sleet: 'At or below {lim} the gauge’s precipitation may be sleet or snow.',
    recent: 'The gauge measured rain in this hour or the one before; none is falling now.',
    clear: 'Cooled more than {cool} in the hour from {at} with wind under {w5}, no rise since.',
    dry_12: 'None at the last reading, none in the last 12 hours.',
    dry: 'None at the last reading.',
    blind_t: 'Rain not judged',
    blind: 'At or below {lim} the gauge may not see snow or sleet.',
    calm: 'Most readings in the last hour had wind under {w1}.',
    windy: 'Mean wind of the last 10 minutes, from {lim}.',
    humid: 'Relative humidity, from {lim}.',
    band: 'At or below {lim}, air temperature as shown.',
    hot: 'At or above {lim}.',
    tend_tide: 'Daily tide taken out. Steady within ±{thr}, fast from {fast}.',
    tend: 'Steady within ±{thr}, fast from {fast}.',
    cooling: 'Fell {dt} in the last hour, at night.',
    collecting: 'Needs 4 hourly pressure averages in a row.',
    paused: 'A reading of the last 3 hours has a calibration or clock mark, so no trend.',
    icy: '{t} with rain now or in the last 12 hours ({rain12}).',
    gusts: 'The strongest gust of the last 10 minutes; the line is {g30}.',
    hoar: 'Calm, clear, dew point {td} within {spread} of the air, at or below {zero}.',
    ground: 'Calm, clear, {t} at 2 m; the grass is about {spread} colder.',
    stale_t: 'Outdoor sensor not updating',
    stale: 'Only the pressure line is shown.',
    sunny: 'The last hour was steady and as bright as this station’s clear days at this sun height.',
    spells: 'The last hour had bright spells between clouds.',
    nosun: 'The last hour stayed dull where this station has seen the sun before.',
    fog: 'Humidity at {rel} of what this sensor shows in saturated air, light wind, no rain in the last 3 hours.',
    min: 'From this evening’s temperature and dew point and the clear, calm nights this station has learned.',
    head: 'From this station’s own readings, as of {at}. No forecast model.',
    // the one source sentence of a "reported nearby" phrase
    near: 'From the {name} ({icao}) report, {age} old, {d} away. A reading from this station comes first.',
  });
  // the pressure notices' short sheet and the Airport weather sheet's comparison; numbers through
  // the page's formatters (pa = a pressure, dp = a difference's size), "about" where the console's step makes it so
  const R = FC.w('outlook-why', {
    rel_ok: 'Your station reads {p}, within {thr} of nearby airports.',
    rel_off: 'Your station reads {p}; nearby airports say {ref}. To match, set REL in the console’s SET menu to about {target}.',
    rel_off_old: 'Your station reads {p}, more than {thr} away from nearby airports. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.',
    rel_since: 'The difference has held since {since}.',
    uncal_last: 'The console’s pressure is at its factory value (after a unit change or a power cut). Set REL in its SET menu to about {target} (the last good reading, {age} ago).',
    uncal: 'The console’s pressure is at its factory value (after a unit change or a power cut). Set REL in its SET menu to the current sea-level pressure of a nearby official station.',
    uncal_ref: 'Nearby airports now: {ref}.',
  });
  // the rest of the words of both faces' sheets (the admin's thresholds and readings: English there, here for the file's pack)
  const W = FC.w('outlook-why', {
    warn: 'Warning:',
    none: 'No outlook: this station has not sent pressure since the restart.',
    src_sun: 'from the sun’s position', src_solar: 'from the light sensor',
    th_steady: 'Pressure steady', th_steady_v: '±{a} / 3 h', th_steady_s: 'Met Office tendency classes',
    th_fast: 'Pressure quickly', th_fast_v: '{a} / 3 h', th_fast_s: 'Met Office',
    th_rain: 'Light / heavy rain', th_rain_v: '{a} / {b}', th_rain_s: 'AMS rain intensity',
    th_cool: 'Clear-night cooling', th_cool_v: '{a} / h, wind < {b}', th_cool_s: 'clear-night cooling',
    th_icy: 'Icy', th_icy_v: '≤ {a} and rain (≥ {b} in 12 h)', th_fixed: 'fixed rule',
    th_gusts: 'Gusts', th_gusts_v: '≥ {a} in 10 min',
    th_hoar: 'Hoar frost', th_hoar_v: 'dew point within {a}, at or below {b}', th_grass: 'grass ≈ 2 °C below 2 m',
    th_ground: 'Ground frost', th_ground_v: '≤ {a} at 2 m',
    th_calm: 'Calm', th_calm_v: 'wind and its 2-min mean < {a}', th_calm_s: 'as in the wind history',
    th_band: 'Freezing / severe frost / hot', th_band_v: '≤ {a} / ≤ {b} / ≥ {c}', th_band_s: 'air temperature as shown',
    th_norain: 'No "No rain."', th_norain_v: '≤ {a}', th_norain_s: 'the gauge may not see snow or sleet',
    th_recent: 'Recent rain', th_recent_v: 'rain in this hour or the one before', th_recent_s: 'the gauge',
    adm_rules: 'Fixed rules; uses what this station learned where a part is in use.', adm_time: 'Waiting for a valid clock.',
    adm_more: 'Readings and thresholds', adm_used: 'Readings used', adm_night: 'Night', unknown: 'unknown',
    night_yes: 'yes ({src})', night_no: 'no ({src})',
    adm_t: 'Temperature / dew point', adm_rain: 'Rain rate / last 12 h', adm_gust: 'Gust, 10 min', adm_wind: 'Wind, last hour',
    wind_calm: '{v}, calm', adm_dt: 'Temperature change, last hour', dt_fell: 'fell {v}', dt_rose: 'rose {v}',
    adm_p: 'Pressure, 3 h', p_fast: '{v}, fast', p_collecting: 'collecting, {h} h left', adm_th: 'Thresholds',
    adm_near: 'Read-only. Phrases marked “reported nearby” come from the main airport’s report; the rest from this station’s readings.',
    adm_own: 'Read-only. The description uses only this station’s readings.',
    sheet: 'Weather description', no_station: 'No station yet.', loading: 'Loading…',
  });
  const REL_THR = 2;  // hPa: the firmware's REL_HINT_DHPA (rel_check.h); test_table_sync.py keeps the two equal
  // the Airport weather sheet: reference.station from /api/metar, compared live (no 3-report wait)
  function rel(s, f) {
    if (!s || s.p_hpa == null) return '';
    const v = { p: f.pa(s.p_hpa), thr: f.dp(REL_THR), ref: f.pa(s.qnh_hpa != null ? s.qnh_hpa : s.p_hpa - s.diff_hpa), target: f.pa(s.target_hpa) };
    return fill(Math.abs(s.diff_hpa) < REL_THR ? 'rel_ok' : s.target_hpa != null ? 'rel_off' : 'rel_off_old', v, R);
  }
  // the tile notice's sheet (baro_ref / baro_uncalibrated): its parameters, the live reading, the airports' QNH or null
  function note(n, p, ref, f) {
    const X = [];
    if (n.id !== 'baro_ref' && n.id !== 'baro_uncalibrated') return `<p class="cap">${e(n.text)}</p>`;  // baro_check: its own sentence
    if (n.id === 'baro_ref') {
      // {ref} is the airports' own QNH (the notice's qnh_hpa), not derived from the live reading
      X.push(fill(n.target_hpa != null ? 'rel_off' : 'rel_off_old', { p: f.pa(p), thr: f.dp(REL_THR), ref: f.pa(n.qnh_hpa != null ? n.qnh_hpa : p - n.d_hpa), target: f.pa(n.target_hpa) }, R));
      if (n.since) X.push(fill('rel_since', { since: dayTime(n.since, new Date()) || '–' }, R));  // the day word when not today
    } else {
      X.push(n.last_hpa != null ? fill('uncal_last', { target: f.pa(n.target_hpa), age: span(Math.max(0, (Date.now() - new Date(n.last_t)) / 1e3)) }, R) : R.uncal);
      if (ref != null) X.push(fill('uncal_ref', { ref: f.pa(ref) }, R));
    }
    return X.map(s => `<p class="cap">${s}</p>`).join('');
  }
  // a report's age as a span (no "ago": the sentence says "old")
  const span = s => s < 5400 ? Math.max(1, Math.round(s / 60)) + ' min' : Math.round(s / 3600) + ' h';
  // the blocks that carry the sun caveat, and the learner behind a block's sentence (B key -> o.in.learn code)
  const SUN = { icy: 1, band: 1, hot: 1, ground: 1 }, LN = { sunny: 'l1', spells: 'l1', nosun: 'l1', humid: 'l2', fog: 'l2', clear: 'l5', min: 'l6' };
  function blocks(o, f) {
    const i = o.in || {}, t = o.tend, X = [], title = c => (FCO.phrase(c, o, { gust: f.g }) || '').replace(/\.$/, ''), lr = (i.learn || []).slice();
    // the loose lines go into the block they belong to -- the sun caveat under a temperature block, a learner's
    // "Uses what..." under the block whose phrase used it
    const add = (g, ti, n, k, v, x) => X.push(Object.assign({ g, ti, n, s: fill(k, v || {}, B),
      note: SUN[k] ? P.sun : lr.includes(LN[k]) ? P['u_' + lr.splice(lr.indexOf(LN[k]), 1)] : '' }, x));  // once per sheet
    const neutral = i.night === true ? 'cloud-moon' : 'cloud-sun';
    if (o.warn === 'icy') add('snowflake', title('icy'), '', 'icy', { t: f.t(i.t), rain12: f.r(i.rain12) }, { w: 1 });
    if (o.warn === 'gusts') add('wind', title('gusts'), '', 'gusts', { g30: f.w(30) }, { w: 1 });
    if (o.warn === 'hoar_frost') add('thermometer-snowflake', title('hoar_frost'), '', 'hoar', { td: f.t(i.td), spread: f.dt(2), zero: f.t(0) }, { w: 1 });
    if (o.warn === 'ground_frost') add('thermometer-snowflake', title('ground_frost'), '', 'ground', { t: f.t(i.t), spread: f.dt(2) }, { w: 1 });
    // the main airport's report; both slots name the same source
    const nr = i.near, nv = nr && { name: nr.name, icao: nr.icao, age: span(nr.age_s || 0), d: f.km(nr.km) };
    if (nr && /_near$/.test(o.warn || '')) add(FCO.WG[o.warn], title(o.warn), '', 'near', nv, { w: 1 });
    const nt = { note: fill('neutral', {}), dim: 1 };
    if (o.warn === 'fog_possible') add('cloud-fog', title('fog_possible'), '', 'fog', { rel: i.rh_rel == null ? '–' : Math.round(i.rh_rel) + ' %' }, { w: 1 });
    if (nr && /_near$/.test(o.state || '')) add(FCO.SG[o.state], title(o.state), '', 'near', nv);
    else if (/^rain_/.test(o.state)) add(FCO.SG[o.state], title(o.state), f.rr(i.rate), o.p && o.p.since ? 'rain_since' : 'rain', { since: f.hm(o.p && o.p.since), lo: f.rr(2.5), hi: f.rr(7.6) });
    else if (o.state === 'sleet_snow') add('snowflake', title(o.state), f.t(i.t), o.p && o.p.since ? 'sleet_since' : 'sleet', { since: f.hm(o.p && o.p.since), lim: f.t(1) });
    else if (o.state === 'night_clear') add('moon', title(o.state), '', 'clear', { cool: f.dt(i.cool_thr == null ? 1 : i.cool_thr), at: f.hm(i.cool_at), w5: f.w(5) });
    else if (/^day_/.test(o.state)) add(FCO.SG[o.state], title(o.state), '', { day_sunny: 'sunny', day_spells: 'spells', day_nosun: 'nosun' }[o.state]);
    else if (o.state === 'recent_rain') add(neutral, title(o.state), '', 'recent', {}, nt);
    else if (o.state === 'dry') add(neutral, title(o.state), '', i.rain12 ? 'dry' : 'dry_12', {}, nt);
    else if (!o.state && o.gate !== 'stale' && i.t != null && i.t <= 2) add(neutral, B.blind_t, '', 'blind', { lim: f.t(2) }, nt);
    const lim = { freezing: 0, severe_frost: -10, hot: 30 }[o.qual];
    if (lim != null) add(o.qual === 'hot' ? 'thermometer' : 'thermometer-snowflake', title(o.qual), f.t(i.t), o.qual === 'hot' ? 'hot' : 'band', { lim: f.t(lim) });
    else if (o.qual === 'calm') add('wind', title('calm'), '', 'calm', { w1: f.w(1) });
    else if (o.qual === 'windy') add('wind', title('windy'), f.w(i.wind10), 'windy', { lim: f.w(20) });
    else if (o.qual === 'humid') add('droplet', title('humid'), i.rh == null ? '' : Math.round(i.rh) + ' %', 'humid', { lim: '90 %' });
    if (t && /^p_(steady|rising|falling)/.test(o.next || '')) add('gauge', title(o.next), f.p(t.d) + ' / 3 h', t.tide ? 'tend_tide' : 'tend', { thr: f.dp(1.6), fast: f.dp(3.6) });
    else if (o.next === 'cooling_fast') add('gauge', title(o.next), '', 'cooling', { dt: f.dt(i.dt_h) });
    else if (o.next === 'min_range') add('thermometer-snowflake', title(o.next), '', 'min');
    else if (o.next === 'collecting' || o.next === 'p_paused') add('gauge', title(o.next), '', o.next === 'collecting' ? 'collecting' : 'paused', {}, { dim: 1 });
    if (o.gate === 'stale') add(neutral, B.stale_t, '', 'stale', {}, { dim: 1 });
    return X;
  }
  const ic = (n, c) => `<svg${c ? ` class="${c}"` : ''} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${FCO.G[n] || LUCIDE[n] || ''}</svg>`;
  // "Now · this station's readings, {at}" over today's blocks, then ONE
  // "Forecast" list in time order (x = FCX.why(): {rows:[{g, w: the time span or the day, t: the words, k: likely, tag, tip}],
  // src: the one source line}); a station alone keeps the single head line ("... No forecast model.") and no headers
  const S2 = FC.w('outlook-why', { now: 'Now · this station’s readings, {at}', fc: 'Forecast' });
  function why(o, f, x) {
    const at = { at: f.hm(o.as_of || o.t) }, more = x && x.rows;
    return (more ? `<p class="cap why-h">${fill('now', at, S2)}</p>` : `<p class="cap">${fill('head', at, B)}</p>`)
      + blocks(o, f).map(b => `<div class="blk${b.w ? ' warn' : ''}">${ic(b.g, b.dim && 'dim')}<b>${b.w ? `<span class="sr">${W.warn} </span>` : ''}${e(b.ti)}${b.n ? ` <span>· ${e(b.n)}</span>` : ''}</b><p>${b.s}</p>${b.note ? `<p class="nt">${b.note}</p>` : ''}</div>`).join('')
      + (more ? `<p class="cap why-h">${S2.fc}</p>${x.rows.map(r => `<div class="blk fcr">${ic(r.g)}<b>${r.w}</b><p>${r.k ? `<b>${r.t}</b>` : r.t}</p><abbr class="tag" title="${r.tip}">${r.tag}</abbr></div>`).join('')}<p class="cap ft">${x.src}</p>` : '');
  }
  // admin: the inputs, a gate in words, and every threshold with its source (the codes stay in /api/live
  // and the HA attributes, not on the page)
  function admin(o, f) {
    if (!o) return `<p class="hint">${W.none}</p>`;
    const i = o.in || {};
    const SRC = { sun: W.src_sun, solar: W.src_solar }, t = FC.t;
    const TH = [[W.th_steady, t(W.th_steady_v, { a: f.dp(1.6) }), W.th_steady_s], [W.th_fast, t(W.th_fast_v, { a: f.dp(3.6) }), W.th_fast_s],
      [W.th_rain, t(W.th_rain_v, { a: f.rr(2.5), b: f.rr(7.6) }), W.th_rain_s], [W.th_cool, t(W.th_cool_v, { a: f.dt(1), b: f.w(5) }), W.th_cool_s],
      [W.th_icy, t(W.th_icy_v, { a: f.t(1), b: f.r(.2) }), W.th_fixed], [W.th_gusts, t(W.th_gusts_v, { a: f.w(30) }), W.th_fixed],
      [W.th_hoar, t(W.th_hoar_v, { a: f.dt(2), b: f.t(0) }), W.th_grass], [W.th_ground, t(W.th_ground_v, { a: f.t(2) }), W.th_grass],
      [W.th_calm, t(W.th_calm_v, { a: f.w(1) }), W.th_calm_s],
      [W.th_band, t(W.th_band_v, { a: f.t(0), b: f.t(-10), c: f.t(30) }), W.th_band_s],
      [W.th_norain, t(W.th_norain_v, { a: f.t(2) }), W.th_norain_s], [W.th_recent, W.th_recent_v, W.th_recent_s]];
    const card = b => `<div class="lb${b.w ? ' w' : ''}">${ic(b.g, b.dim ? 'ic dim' : 'ic')}<b>${b.w ? `<span class="sr">${W.warn} </span>` : ''}${e(b.ti)}${b.n ? ` <span>· ${e(b.n)}</span>` : ''}</b><span></span><p>${b.s}</p>${b.note ? `<p class="nt">${b.note}</p>` : ''}</div>`;
    return `<p class="hint">${fill('head', { at: f.hm(o.as_of || o.t) }, B)} ${W.adm_rules}</p>
      ${o.gate === 'time' ? `<p class="hint">${W.adm_time}</p>` : blocks(o, f).map(card).join('')}
      <details class="adv"><summary>${W.adm_more}</summary>
      <h3>${W.adm_used}</h3><dl class="kv"><dt>${W.adm_night}</dt><dd>${i.night == null ? W.unknown : t(i.night ? W.night_yes : W.night_no, { src: own(SRC, i.night_src) || W.unknown })}</dd>
      <dt>${W.adm_t}</dt><dd>${e(f.t(i.t))} / ${e(f.t(i.td))}</dd><dt>${W.adm_rain}</dt><dd>${e(f.rr(i.rate))} / ${e(f.r(i.rain12))}</dd>
      <dt>${W.adm_gust}</dt><dd>${e(f.w(i.gust10))}</dd><dt>${W.adm_wind}</dt><dd>${i.calm ? t(W.wind_calm, { v: e(f.w(i.wind_h)) }) : e(f.w(i.wind_h))}</dd>
      <dt>${W.adm_dt}</dt><dd>${i.dt_h == null ? '–' : t(i.dt_h < 0 ? W.dt_fell : W.dt_rose, { v: e(f.dt(i.dt_h)) })}</dd>
      <dt>${W.adm_p}</dt><dd>${o.tend ? (o.tend.fast ? t(W.p_fast, { v: e(f.p(o.tend.d)) }) : e(f.p(o.tend.d))) : o.next === 'collecting' ? t(W.p_collecting, { h: e(o.left_h) }) : '–'}</dd></dl>
      <h3>${W.adm_th}</h3><dl class="kv">${TH.map(([a, b, c]) => `<dt>${a}</dt><dd>${e(b)} <span class="hint">${c}</span></dd>`).join('')}</dl></details>
      <p class="hint">${i.near ? W.adm_near : W.adm_own}</p>`;
  }
  // the admin's metric formatters (the dashboard passes its own, unit-aware ones); dt and dp take the size only
  const n1 = (v, u) => v == null || isNaN(v) ? '–' : (Math.round(v * 10) / 10).toFixed(1).replace('-', '−') + ' ' + u;
  const M = { t: v => n1(v, '°C'), dt: v => n1(v == null ? v : Math.abs(v), '°C'), w: v => n1(v, 'km/h'), r: v => n1(v, 'mm'),
    rr: v => n1(v, 'mm/h'), g: v => v == null ? '–' : Math.round(v) + ' km/h', km: v => v + ' km', pa: v => n1(v, 'hPa'), p: v => v == null ? '–' : (v > 0 ? '+' : '') + n1(v, 'hPa'), dp: v => n1(Math.abs(v), 'hPa'),
    hm: s => s ? hm(s) : '–' };
  // admin "Weather description" (lazy like the station sheets): read-only, per station, from /api/live
  if (typeof SHEETS !== 'undefined') SHEETS.weather = function () {
    const d = {};
    let cur = API.stations.length ? API.stations[0].id : '';
    API.stations.forEach(x => FC.get('/api/live?station=' + encodeURIComponent(x.id), { etag: false }).then(r => {
      d[x.id] = (r.data.station && r.data.station.outlook) || null; if (SH && SH.key === 'weather') draw(); }, () => {}));
    return { icon: icon('sun'), title: W.sheet,
      body: () => !API.stations.length ? `<p class="hint">${W.no_station}</p>`
        : (API.stations.length > 1 ? `<div class="tabs" role="tablist" style="margin-bottom:12px">${API.stations.map(x =>
          `<button type="button" role="tab" data-st="${e(x.id)}" aria-selected="${x.id === cur}">${e(x.cfg.name || x.id)}</button>`).join('')}</div>` : '')
          + (cur in d ? admin(d[cur], M) : `<p class="hint loading">${W.loading}</p>`),
      click(b) { if (b.dataset.st) { cur = b.dataset.st; draw(); } } };
  };
  return { why, admin, blocks, rel, note, P, B, R, M };
})();
