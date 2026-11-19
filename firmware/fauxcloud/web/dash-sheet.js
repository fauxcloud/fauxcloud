// dash-sheet.js -- the dashboard's history sheet: rows per range, the chart, the legend, the wind rose, the
// table and Export CSV. Loaded by dash.js on the first sheet open (FC.lazy); it shares dash.js's globals (S, M,
// model(), measure, range, table, the formatters) and replaces its renderSheet stub.
// The date range behind the calendar segment. SP: a preset (days back from today, or 'all') or, with p 0,
// a typed From..To (local days, both inclusive). SPC: the answer of the range last asked for, fetched once per choice
// (a flash view's ETag changes every upload). spOpen: the preset row under From / To is open
// (the calendar opens it, a preset closes it, From / To stay in the tools row).
let SP = { p: 90 }, SPZ = {}, RAW = [], spT = 0, spOpen = false;
const SPC = { want: '', at: 0, data: null, err: false, busy: false, days: false, h0: null };
const WS = FC.w('dash-sheet', {
  // the Beaufort bands behind the wind chart (BFT)
  bft1: 'light air', bft2: 'light breeze', bft3: 'gentle', bft4: 'moderate', bft5: 'fresh', bft6: 'strong', bft7: 'near gale',
  bft8: 'gale', bft9: 'strong gale', bft10: 'storm', bft11: 'violent storm', bft12: 'hurricane',
  // the chart's accessible name, one whole sentence per range
  ch_days: '{name}, {a} to {b}. The table view lists every value.', ch_yday: '{name}, yesterday by hour. The table view lists every value.',
  ch_now: '{name}, last readings. The table view lists every value.', ch_today: '{name}, today by hour. The table view lists every value.',
  // the legend: one whole sentence per resolution
  lg_r0: 'Rain per reading', lg_r1: 'Rain per hour', lg_r6: 'Rain per 6 hours', lg_r24: 'Rain per day', lg_r168: 'Rain per week',
  lg_gust: 'Strongest gust', lg_wind: 'Wind', lg_avg: 'Average', lg_gust_arr: 'Strongest gust, arrows point downwind',
  lg_wind_arr: 'Wind, arrows point downwind', lg_avg_arr: 'Average, arrows point downwind', lg_reading: 'Reading', lg_range: 'Range',
  lg_dew: 'Dew point', lg_rejsun: 'Reading rejected',
  lg_rej1: 'Some readings rejected that hour', lg_rej6: 'Some readings rejected in those 6 hours', lg_rej24: 'Some readings rejected that day',
  lg_rej168: 'Some readings rejected that week',
  // the rose's speed classes (ROSE), metric and imperial
  rose_m0: '1–5 km/h', rose_i0: '1–3 mph', rose_m1: '6–19 km/h', rose_i1: '4–12 mph', rose_m2: '20 km/h and more', rose_i2: '12 mph and more',
  // the range row's figures
  s_avg: 'average', s_gust: 'strongest gust', s_from: 'mostly from', s_total: 'total', s_wet_d: 'wettest day, {d}',
  s_wet_hd: 'wettest hour, {d} {t}', s_wet_h: 'wettest hour, {t}', s_high: 'highest', s_sun: 'of sun', s_lohi: 'lowest / highest',
  s_dew: 'dew point average', s_change: 'change', yday: 'Yesterday', today: 'Today', rs_aria: '{head} summary',
  // the Now row
  n_week: 'this week', n_month: 'this month', n_year: 'this year', n_7d: 'last 7 days', n_gust: 'gust', n_read: 'reading', n_calm: 'Calm',
  n_dir: 'direction', n_from: 'from', n_gust10: 'gust, last 10 min', n_today: 'today', n_hour: 'last hour', n_dew: 'dew point',
  n_feels: 'feels like', n_abs: 'absolute', n_rej: 'rejected', n_light: 'light', n_hum: 'humidity',
  // the wind rose
  rose_pct: '{pct}% of the time', rose_part_h: '{k} h at {l}', rose_part_r: '{k} at {l}', rose_tip_h: 'From {p}: {n} h ({parts})',
  rose_tip_r: 'From {p}: {n} ({parts})', rose_head_d: 'Mostly from <b>{p}</b>: {pct}% of the time.',
  rose_head_h: 'Mostly from <b>{p}</b>: {mx} of {n} hours.', rose_head_r: 'Mostly from <b>{p}</b>: {mx} of {n} readings.',
  rose_aria: 'Wind rose: {head} {list}.', rose_aria_calm: 'Wind rose: {head} {list}, calm {calm}.', rose_hours: 'Hours from each direction',
  rose_calm_h: 'Calm: {n} h', rose_calm_r: 'Calm: {n}', rose_calm0: 'Calm: none',
  rose_cap_d: 'Where the wind came from in this range. Each spoke points to a direction the wind blew from; the longer it is, the more hours. Speed shades need hourly data: pick 7 days or less.',
  rose_cap_h: 'Where the wind came from. Each spoke points to a direction the wind blew from; the longer it is, the more hours, and the fuller its colour, the stronger the wind. Each hour counts once, by its average speed and direction.',
  rose_cap_r: 'Where the wind came from. Each spoke points to a direction the wind blew from; the longer it is, the more readings, and the fuller its colour, the stronger the wind. Each reading counts once.',
  // the sheet
  sub_off: '{name}, offline', u_hour: '{u} last hour', u_now: '{u} now',
  nohist: 'Rain history comes from the daily rain, which is hidden for this station.',
  promoted: 'fauxcloud keeps no history for promoted fields yet; Home Assistant records them.', offline: 'Offline', now_h: 'Now',
  e_range: 'No readings in this range.', e_load: 'Could not load this range. Pick it again to retry.', e_yday: 'No data for yesterday.',
  e_now: 'No readings yet.', e_today: 'No hours yet today.',
  fb: 'Hour records reach back to {d}; this range shows days.', fb0: 'No hour records for these dates; this range shows days.',
  latest: { one: 'Showing the latest {n} reading, {from}. Earlier readings ended at {end}; the table lists them.',
    other: 'Showing the latest {n} readings, {from}–{to}. Earlier readings ended at {end}; the table lists them.' },
  // the table
  csv: 'Export CSV', th_time: 'Time', th_1: 'Hour', th_6: 'Hours', th_24: 'Day', th_168: 'Week', th_lo: 'lowest', th_avg: 'average',
  th_hi: 'highest', th_gust: 'gust', th_from: 'from', th_dew: 'dew pt', th_n: 'readings', th_rej: 'rejected', so_far: '{lab} so far',
  no_reading: 'no reading', flag: 'Far above the row’s average: a sensor glitch or a filter gap.',
  // the chart's tooltip
  t_rain: 'Rain', t_avg: 'Average', t_gust: 'Gust', t_from: 'From', t_range: 'Range', t_dew: 'Dew point', t_uv: 'UV index', t_sun: 'Sun',
  t_rej: 'Rejected', t_rej_n: { one: '{n} reading', other: '{n} readings' }, t_open: '{lab} (so far)',
  // the date range row
  from: 'From', to: 'To', dates: 'Date range', sp7: '7 d', sp7a: '7 days', sp30: '30 d', sp30a: '30 days', sp90: '90 d', sp90a: '90 days',
  sp365: '1 y', sp365a: '1 year', spall: 'All', spalla: 'Everything kept',
  // Export CSV's button
  csv_fail: 'Export failed, try again', csv_done: { one: '{n} row exported', other: '{n} rows exported' },
});
const ymd = t => t.getFullYear() + '-' + p2(t.getMonth() + 1) + '-' + p2(t.getDate());
const addD = (t, n) => new Date(t.getFullYear(), t.getMonth(), t.getDate() + n);  // local days: DST-safe
// a..b: local midnights (a null = "All" until its answer is in); nd days; step = hours per point (hour
// records up to 7 days, as 6-hour buckets from 4 days; day records up to a year, weekly buckets above)
function spanOf(sp = SP, now = new Date()) {
  const b = sp.p ? addD(now, 0) : sp.b, a = sp.p === 'all' ? null : sp.p ? addD(b, 1 - sp.p) : sp.a, nd = a ? Math.round((addD(b, 1) - a) / DAY) : 9999;
  return { a, b, nd, step: nd <= 3 ? 1 : nd <= 7 ? 6 : nd <= 366 ? 24 : 168, from: a ? a / 1e3 : 0, to: addD(b, 1) / 1e3, today: +b === +addD(now, 0) };
}
// the request for a span; '' = the 90-day preset, which reads the sheet's own daily poll (days=90)
function spanUrl(z, sp = SP, id = S.cur) {
  const q = '?station=' + encodeURIComponent(id);
  return z.step <= 6 ? `/api/hourly${q}&from=${z.from}&to=${z.to}` : sp.p === 'all' ? `/api/daily${q}&all=1` : sp.p === 90 ? '' : `/api/daily${q}&from=${z.from}&to=${z.to}`;
}
// Only an hour range that includes today refreshes on its own (after 10 min); day and week ranges never do
// (365 / 736 day rows through the one httpd task). The hour ring reaches ~131 / 88 days back and no answer
// says how far, so when an hour answer starts after From the same range's day records are asked once and shown
// instead (step 24) if any lie before the first hour: decided by the data, never by a guessed reach.
function spanSrc(z) {
  const url = spanUrl(z);
  if (!url) return S.daily && S.daily.station === S.cur ? S.daily : null;
  if (SPC.want !== url) Object.assign(SPC, { want: url, data: null, err: false, at: 0, days: false, h0: null });
  if (!SPC.busy && (!SPC.at || z.step <= 6 && z.today && SPC.data && Date.now() - SPC.at > 600e3)) {
    SPC.busy = true;
    const get = u => FC.get(u, { etag: false, timeout: 30000 }).then(r => r.data);
    get(url).then(d => {
      const H = d.hours, h0 = H && H.length ? addD(new Date(H[0].h), 0) : null;
      if (!H || h0 && h0 <= z.a) return [d];
      return get(url.replace('/api/hourly', '/api/daily')).then(dd => dd.days.length && (!h0 || new Date(dd.days[0].d + 'T00:00') < h0) ? [dd, true, h0] : [d]);
    }).then(([d, days = false, h0 = null]) => { if (SPC.want === url) Object.assign(SPC, { data: d, days, h0, err: false }); },
      () => { if (SPC.want === url) SPC.err = true; })
      .finally(() => { SPC.busy = false; if (SPC.want === url) SPC.at = Date.now(); if (sheetOpen() && range === 'days') renderSheet(); });
  }
  return SPC.data;
}
// x labels of a range: dates at round steps; within 3 days every 6 (phone: 12) hours, 5-7 days on a phone every 2nd day. Stepped on the local
// clock, so a DST day keeps 00/06/12/18 and its midnight date
function spanTicks(sp, narrow) {
  const hrs = sp.step > 6 ? 24 * [1, 2, 7, 14, 28, 56, 91, 182, 364].find(k => sp.nd / k <= (narrow ? 4 : 7)) || 8736 : sp.nd <= 3 ? (narrow ? 12 : 6) : narrow && sp.nd > 4 ? 48 : 24, out = [];
  for (let t = new Date(sp.a); t < sp.to * 1e3; t = hrs < 24 ? new Date(t.getFullYear(), t.getMonth(), t.getDate(), t.getHours() + hrs) : addD(t, hrs / 24)) out.push(t);
  return out;
}
// 6-hour (local 00/06/12/18) or Monday-week buckets of hour or day rows: the lowest low, the highest high, averages
// weighted by readings, rain summed, the strongest gust, the speed-weighted mean direction (calm buckets have none)
function bucket(R, step, rain = measure === 'rain', wind = measure === 'wind') {
  const out = [], AV = ['mid', 'dew', 'uv', 'sol'];
  R.forEach(r => {
    const t = r.t, s = step === 6 ? new Date(t.getFullYear(), t.getMonth(), t.getDate(), t.getHours() - t.getHours() % 6) : addD(t, -((t.getDay() + 6) % 7));
    let b = out[out.length - 1];
    if (!b || +b.t !== +s) out.push(b = { t: s, lab: step === 6 ? `${dm(s)} ${hh(s)}–${hh(new Date(s.getFullYear(), s.getMonth(), s.getDate(), s.getHours() + 6))}` : `${dm(s)}–${dm(addD(s, 6))}`,
      n: 0, rej: 0, open: false, lo: null, hi: null, g: null, w: {}, x: 0, y: 0 });
    const n = r.n || 1, f = (a, v, o) => v == null ? a : a == null ? v : o(a, v);
    b.n += r.n || 0; b.rej += r.rej || 0; b.open = b.open || r.open;
    b.lo = f(b.lo, r.lo, Math.min); b.hi = f(b.hi, r.hi, Math.max); b.g = f(b.g, r.g, Math.max);
    AV.forEach(k => { if (r[k] == null) return; const w = b.w[k] = b.w[k] || [0, 0], s1 = rain && k === 'mid'; w[0] += r[k] * (s1 ? 1 : n); w[1] += s1 ? 0 : n; });
    if (r.dir != null) { const wt = (wind ? r.mid || 0 : 1) * n, a = r.dir * Math.PI / 180; b.x += wt * Math.sin(a); b.y += wt * Math.cos(a); }
  });
  return out.map(b => { AV.forEach(k => { const w = b.w[k]; b[k] = w ? w[1] ? w[0] / w[1] : w[0] : null; });
    b.dir = b.x || b.y ? Math.round(Math.atan2(b.x, b.y) * 180 / Math.PI + 360) % 360 : null; return b; });
}
// one history record (hour or day) as a sheet row; k: the measure's field
const rowOf = (x, k) => ({ t: x.t, open: !!x.open, n: x.n, rej: x.rej,
  // hourly/daily UV carries only its max ([null, null, max]): the max stands in for the missing average
  lo: Array.isArray(x[k]) ? x[k][0] : null, mid: Array.isArray(x[k]) ? x[k][1] ?? (k === 'uv' ? x[k][2] : null) : x[k] ?? null, hi: Array.isArray(x[k]) ? x[k][2] : null,
  g: k === 'wind_kmh' ? x.gust_kmh : null, dir: x.wind_dir, rose: x.rose, calm: x.calm_hours, dew: k === 'temp_c' && x.dewpt_c ? x.dewpt_c[1] : null, uv: k !== 'uv' && x.uv ? x.uv[1] ?? x.uv[2] : null,
  sol: k === 'uv' && x.solar_wm2 ? x.solar_wm2[1] : null });
function rows() {
  const m = model(), k = M[measure].k;
  if (range === 'days') {
    const z = SPZ = spanOf(), raw = z.raw = spanSrc(z);
    if (raw && SPC.days && SPC.want === spanUrl(z)) { z.step = 24; z.fb = SPC.h0; }  // the day records stand in
    const rec = raw ? m.recs(raw, z.step > 6).filter(x => !z.a || x.t >= z.a && x.t < addD(z.b, 1)) : [];
    z.err = !raw && SPC.err && !!spanUrl(z);
    if (!z.a && rec.length) { z.a = addD(rec[0].t, 0); z.nd = Math.round((addD(z.b, 1) - z.a) / DAY); z.step = z.nd <= 366 ? 24 : 168; }
    RAW = rec.map(x => rowOf(x, k));
    // the time axis starts at the first bucket: a week bucket starts on the Monday on or before From
    z.t0 = z.a ? +(z.step === 168 ? addD(z.a, -((z.a.getDay() + 6) % 7)) : z.a) : 0;
    return z.step === 6 || z.step === 168 ? bucket(RAW, z.step) : RAW.map(r => Object.assign({}, r, { lab: z.step === 1 ? `${dm(r.t)} ${hh(r.t)}–${hh(new Date(+r.t + HOUR))}` : dm(r.t) }));
  }
  const src = range === 'yday' ? m.yday : m.today;
  // "Now": the recent ring, one row per reading; rain is what fell since the reading before
  if (range === 'now') return m.recent.map((x, i, a) => {
    const p = a[i - 1], d = x.rain_daily_mm, pd = p && p.rain_daily_mm;
    const mid = k === 'rain_mm' ? (d == null || pd == null ? null : d >= pd ? d - pd : d) : x[k] ?? null;
    return { t: x.t, lab: hm(x.t), n: 1, rej: 0, lo: null, mid, hi: null, g: k === 'wind_kmh' ? x.gust_kmh ?? null : null, dir: x.wind_dir ?? null,
      dew: k === 'temp_c' ? x.dewpt_c ?? null : null, uv: k !== 'uv' ? x.uv ?? null : null, sol: k === 'uv' ? x.solar_wm2 ?? null : null };
  });
  return src.map(x => Object.assign(rowOf(x, k), { lab: hh(x.t) }));
}
// "Now" shows the newest run of readings: a run ends where the step to the next reading
// exceeds max(2 min, 3 x the median step over the whole ring), the same gap the chart breaks its line at. Older runs
// stay in the table; `left` = how many readings the chart leaves out.
function newestRun(all) {
  if (range !== 'now' || all.length < 2) return { R: all, left: 0 };
  const st = all.slice(1).map((r, i) => (r.t - all[i].t) / 60e3), med = st.slice().sort((a, b) => a - b)[st.length >> 1] || 1;
  const gap = Math.max(2, 3 * med);
  let start = 0;
  st.forEach((d, i) => { if (d > gap) start = i + 1; });
  return { R: all.slice(start), left: start };
}
// the bands' short names, from BEAU's edges (dash.js); below CALM_KMH merges into "light air"
const BFT = ['bft1', 'bft2', 'bft3', 'bft4', 'bft5', 'bft6', 'bft7', 'bft8', 'bft9', 'bft10', 'bft11', 'bft12'];  // WS keys
// the line's highest and lowest point to label, by the tile chart's rules: P = [x as % of the plot, value, ...] per point,
// `end` when P's last point is the running hour (the headline shows it: no label within 8 % of it), `num` the shown
// number. The lowest only when it differs from the highest as shown and sits 12 % away from it; `hiOnly` (light, as the
// tile's bars) the highest alone and only above 0; a label near an edge grows inwards. Returns [point, text-anchor] per label.
function extOf(P, end, num, hiOnly) {
  if (P.length < 2) return [];
  const hi = P.reduce((a, p) => p[1] > a[1] ? p : a), lo = P.reduce((a, p) => p[1] < a[1] ? p : a), eX = P[P.length - 1][0];
  const mk = p => end && eX - p[0] < 8 ? [] : [[p, p[0] > 80 ? 'end' : p[0] < 20 ? 'start' : 'middle']];
  return hiOnly ? hi[1] > 0 ? mk(hi) : [] : mk(hi).concat(lo[1] < hi[1] && num(lo[1]) !== num(hi[1]) && Math.abs(lo[0] - hi[0]) >= 12 ? mk(lo) : []);
}
function chart(R, fx) {
  // the viewBox follows the real width so 11 px labels stay 11 px on a phone
  const W = Math.max(300, Math.min(780, Math.round($('#shBody').clientWidth || 680))), narrow = W < 520, H = narrow ? 280 : 250, Lm = 44, Rm = 10, T = 12, B = 34, pw = W - Lm - Rm, ph = H - T - B, isBar = measure === 'rain';
  // wind draws average + gust only (no band), so its scale ignores min/max; they stay in the tooltip and table
  const vals = R.flatMap(r => measure === 'wind' ? [r.mid, r.g] : [r.lo, r.mid, r.hi, measure === 'temp' ? r.dew : null]).filter(v => v != null);
  if (!vals.length) vals.push(0);
  let lo = Math.min(...vals), hi = Math.max(...vals);
  if (isBar || measure === 'wind' || measure === 'sun') lo = 0;
  // minimum span per measure: a calm day must not stretch a 0.5 hPa wobble over the whole plot
  const span = M[measure].span || 1;
  if (hi - lo < span) { if (lo === 0) hi = span; else { const c = (hi + lo) / 2; lo = c - span / 2; hi = c + span / 2; } }
  if (M[measure].max != null && hi > M[measure].max) { lo -= hi - M[measure].max; hi = M[measure].max; }
  // round ticks: a 1/2/5 × 10^k step giving about four intervals, the axis snapped outwards to whole steps
  const raw = (hi - lo) / 4, mag = 10 ** Math.floor(Math.log10(raw)), step = mag * [1, 2, 5, 10].find(m => m * mag >= raw);
  lo = Math.floor(lo / step) * step; hi = Math.ceil(hi / step) * step;
  if (hi - lo < step * 3 && !(M[measure].max != null && hi >= M[measure].max)) hi += step; // headroom, never past a hard max
  // "Now" runs on a real time axis in minutes from the first reading, so the span is what the console really sent
  // a date range runs on a real time axis too, one slot per point (hour, 6 hours, day or week) from its first day
  const n = R.length, now = range === 'now', sp = range === 'days' ? SPZ : null, u = sp ? sp.step * HOUR : 0, t0 = sp ? sp.t0 : now && n ? R[0].t.getTime() : 0, mins = now && n ? Math.max(1, (R[n - 1].t - t0) / 60e3) : 0;
  const slot = pw / (sp ? Math.max(1, Math.ceil((sp.to * 1e3 - t0) / u)) : now ? mins + 1 : 24), xi = i => sp ? (R[i].t - t0) / u : now ? (R[i].t - t0) / 60e3 : R[i].t.getHours(), X = i => Lm + slot * (xi(i) + .5), Y = v => T + ph - (v - lo) / (hi - lo) * ph;
  const steps = R.slice(1).map((r, i) => xi(i + 1) - xi(i)).sort((a, b) => a - b), gapMin = now ? Math.max(2, 3 * (steps[steps.length >> 1] || 1)) : sp ? 1.5 : 1;
  const dec = step < 1 ? (step < .1 ? 2 : 1) : 0, tk = v => dsep((Math.round(v * 100) / 100).toFixed(dec)).replace('-', '−');
  let g = '';
  // Beaufort bands behind the grid, every other one tinted, a short name where the band is 14 px or taller
  if (measure === 'wind') BFT.forEach((t, i) => { const a = Math.max(lo, cvv('w', BEAU[i][0] * !!i)), b = Math.min(hi, cvv('w', BEAU[i + 1][0])); if (b <= a) return;
    const y0 = Y(b), y1 = Y(a);
    if (i % 2) g += `<rect x="${Lm}" y="${y0.toFixed(1)}" width="${pw}" height="${(y1 - y0).toFixed(1)}" fill="var(--wind)" opacity=".06"/>`;
    if (y1 - y0 >= 14) g += `<text x="${W - Rm - 4}" y="${((y0 + y1) / 2 + 3.5).toFixed(1)}" text-anchor="end" style="font-size:10px;fill:var(--ink-3)">${WS[t]}</text>`; });
  for (let v = lo; v <= hi + step / 2; v += step) { const y = Y(v).toFixed(1); g += `<line x1="${Lm}" x2="${W - Rm}" y1="${y}" y2="${y}" stroke="var(--grid)"/><text x="${Lm - 8}" y="${(+y + 4)}" text-anchor="end">${tk(v)}</text>`; }
  if (sp) spanTicks(sp, narrow).forEach(t => {  // a label never sits on the y axis or the right edge
    const x = Lm + slot * ((t - t0) / u + (sp.step > 6 ? .5 : 0));
    if (x >= Lm + 14 && x <= W - Rm - 14) g += `<text x="${x.toFixed(1)}" y="${H - 14}" text-anchor="middle">${t.getHours() ? hh(t) : dm(t)}</text>`; });
  else if (now && (n < 2 || R[n - 1].t - t0 < 60e3)) g += `<text x="${X(0).toFixed(1)}" y="${H - 14}" text-anchor="middle">${hm(R[0].t)}</text>`;
  else if (now) { const parts = narrow ? 2 : 4; for (let j = 0; j <= parts; j++) g += `<text x="${(Lm + slot * (mins * j / parts + .5)).toFixed(1)}" y="${H - 14}" text-anchor="middle">${hm(new Date(t0 + mins * j / parts * 60e3))}</text>`; }
  else for (let h = 0; h < 24; h += narrow ? 6 : 3) g += `<text x="${(Lm + slot * (h + .5)).toFixed(1)}" y="${H - 14}" text-anchor="middle">${p2(h)}</text>`;
  // a gap in the hours (no uploads) breaks the line
  const segs = key => { const out = []; let s = []; R.forEach((r, i) => { const gap = i && xi(i) - xi(i - 1) > gapMin; if (r[key] == null || gap) { if (s.length) out.push(s); s = []; } if (r[key] != null) s.push(i); }); if (s.length) out.push(s); return out; };
  if (isBar) {
    const bw = Math.max(2, Math.min(slot, pw / Math.max(n, 1)) - 2);
    R.forEach((r, i) => { if (r.mid > 0) { const y = Y(r.mid); g += `<rect x="${(X(i) - bw / 2).toFixed(1)}" y="${y.toFixed(1)}" width="${bw.toFixed(1)}" height="${(T + ph - y).toFixed(1)}" rx="${Math.min(3, bw / 2)}" fill="var(--rain)"${r.open ? ' opacity=".55"' : ''}/>`; } });
  } else {
    if (measure !== 'wind') segs('hi').forEach(s => { const top = s.map(i => X(i).toFixed(1) + ',' + Y(R[i].hi).toFixed(1)), bot = s.slice().reverse().map(i => X(i).toFixed(1) + ',' + Y(R[i].lo).toFixed(1)); g += `<polygon points="${top.concat(bot).join(' ')}" fill="${M[measure].c}" opacity=".2"/>`; });
    if (measure === 'temp') segs('dew').forEach(s => g += `<polyline points="${s.map(i => X(i).toFixed(1) + ',' + Y(R[i].dew).toFixed(1)).join(' ')}" fill="none" stroke="var(--ink-2)" stroke-width="2" stroke-dasharray="5 4"/>`);
    segs('mid').forEach(s => g += s.length === 1 ? `<circle cx="${X(s[0]).toFixed(1)}" cy="${Y(R[s[0]].mid).toFixed(1)}" r="2.5" fill="${M[measure].c}"/>` : `<polyline points="${s.map(i => X(i).toFixed(1) + ',' + Y(R[i].mid).toFixed(1)).join(' ')}" fill="none" stroke="${M[measure].c}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`);
    // gust as a thin dotted line, the same pair as the tile sparkline (wind solid, gust dotted)
    if (measure === 'wind') segs('g').forEach(s => g += `<polyline points="${s.map(i => X(i).toFixed(1) + ',' + Y(R[i].g).toFixed(1)).join(' ')}" fill="none" stroke="var(--ink-2)" stroke-width="1.5" stroke-dasharray="2 4" stroke-linecap="round"/>`);
    if (measure === 'sun') R.forEach((r, i) => { if (r.mid == null && range !== 'days' && !now) g += `<rect x="${(X(i) - slot / 2 + 1).toFixed(1)}" y="${T}" width="${(slot - 2).toFixed(1)}" height="${ph}" fill="url(#hatch)"/>`; });
  }
  // wind direction sits on the average line (the record's direction is the hour's vector mean, not the gust's):
  // each average point is an arrow pointing downwind, like the dial marker; every 7th day in the 90-day view
  if (measure === 'wind') R.forEach((r, i) => { if (r.dir == null || r.mid == null || (sp && i % Math.ceil(n / 13)) || (now && i % (narrow ? 12 : 6))) return;
    g += `<path transform="translate(${X(i).toFixed(1)} ${Y(r.mid).toFixed(1)}) rotate(${(r.dir + 180) % 360})" d="M0 -8L5 6L0 3L-5 6Z" fill="var(--ink)" stroke="var(--ring)" stroke-width="1.5" stroke-linejoin="round"/>`; });
  R.forEach((r, i) => { if (r.rej) g += `<path d="M${X(i).toFixed(1)} ${T + ph + 4}l4 7h-8z" fill="var(--warn)"/>`; });
  // the average line's highest and lowest value above its point, so the range reads without the scale (below it at the
  // plot's top); none on "Now" (live readings) or on bars; above a wind point's arrow
  const EP = R.flatMap((r, i) => r.mid == null ? [] : [[(X(i) - Lm) / pw * 100, r.mid, i]]), eo = measure === 'wind' ? 11 : 6;
  if (!isBar && !now) extOf(EP, EP.length && R[EP[EP.length - 1][2]].open, fx, measure === 'sun').forEach(([[, v, i], a]) =>
    g += `<text x="${X(i).toFixed(1)}" y="${(Y(v) - eo < 9 ? Y(v) + eo + 8 : Y(v) - eo).toFixed(1)}" text-anchor="${a}" style="fill:var(--ink-2);stroke:var(--ring);stroke-width:3px;paint-order:stroke;stroke-linejoin:round">${fx(v)} <tspan style="font-size:9px">${esc(M[measure].u)}</tspan></text>`);
  g += `<line x1="${Lm}" x2="${W - Rm}" y1="${T + ph}" y2="${T + ph}" stroke="var(--ink-3)" stroke-width="1"/>`;
  g += `<line id="xh" x1="0" x2="0" y1="${T}" y2="${T + ph}" stroke="var(--ink-2)" stroke-width="1" style="display:none"/>`;
  const defs = `<defs><pattern id="hatch" width="7" height="7" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><rect width="7" height="7" fill="none"/><line x1="0" y1="0" x2="0" y2="7" stroke="var(--warn)" stroke-width="2" opacity=".5"/></pattern></defs>`;
  const al = FC.t(sp ? WS.ch_days : range === 'yday' ? WS.ch_yday : now ? WS.ch_now : WS.ch_today, { name: M[measure].name, a: sp && dm(sp.a), b: sp && dm(sp.b) });
  return { svg: `<svg class="chart" id="chart" viewBox="0 0 ${W} ${H}" role="img" aria-label="${al}">${defs}${g}</svg>`, X, W, slot, xi };
}
function legendFor(R) {
  const sw = (c, dash, w = 2) => `<svg viewBox="0 0 22 10" aria-hidden="true"><line x1="0" y1="5" x2="22" y2="5" stroke="${c}" stroke-width="${w}"${dash ? ` stroke-dasharray="${dash}"` : ''}/></svg>`;
  const band = c => `<svg viewBox="0 0 22 10" aria-hidden="true"><rect x="0" y="1" width="22" height="8" rx="2" fill="${c}" opacity=".25"/></svg>`;
  // one whole sentence per resolution: 0 = a reading, else the hours per point
  const c = M[measure].c, st = range === 'now' ? 0 : range === 'days' ? SPZ.step : 1, it = [];
  if (measure === 'rain') it.push(`<span><svg viewBox="0 0 22 10" aria-hidden="true"><rect x="6" y="0" width="10" height="10" rx="2" fill="${c}"/></svg>${WS['lg_r' + st]}</span>`);
  else {
    const H = hidSet(), k = M[measure].k;
    if (measure === 'wind') it.push(H.has('wind_dir') ? `<span>${sw(c)}${k === 'gust_kmh' ? WS.lg_gust : range === 'now' ? WS.lg_wind : WS.lg_avg}</span>` : `<span><svg viewBox="0 0 22 10" aria-hidden="true"><line x1="0" y1="5" x2="22" y2="5" stroke="${c}" stroke-width="2"/><path transform="translate(11 5) rotate(90)" d="M0 -5L3.5 4L0 2L-3.5 4Z" fill="var(--ink)"/></svg>${k === 'gust_kmh' ? WS.lg_gust_arr : range === 'now' ? WS.lg_wind_arr : WS.lg_avg_arr}</span>`);
    if (measure === 'wind' && k === 'wind_kmh' && !H.has('gust_kmh')) it.push(`<span>${sw('var(--ink-2)', '2 4', 1.5)}${WS.lg_gust}</span>`);
    else if (range === 'now') it.push(`<span>${sw(c)}${WS.lg_reading}</span>`);
    else it.push(`<span>${sw(c)}${WS.lg_avg}</span>`, `<span>${band(c)}${WS.lg_range}</span>`);
    if (measure === 'temp' && k === 'temp_c' && !H.has('dewpt_c')) it.push(`<span>${sw('var(--ink-2)', '5 4')}${WS.lg_dew}</span>`);
    if (measure === 'sun' && range !== 'days' && range !== 'now') it.push(`<span><svg viewBox="0 0 22 10" aria-hidden="true"><rect x="0" y="0" width="22" height="10" fill="url(#hatch)" stroke="var(--warn)" stroke-opacity=".5"/></svg>${WS.lg_rejsun}</span>`);
  }
  // only when a ▲ is drawn in this range
  if (range !== 'now' && R.some(r => r.rej)) it.push(`<span><svg viewBox="0 0 22 10" aria-hidden="true"><path d="M11 1l4 8h-8z" fill="var(--warn)"/></svg>${WS['lg_rej' + st]}</span>`);
  return `<div class="legend">${it.join('')}</div>`;
}
// wind rose, the meteorological standard: a spoke per compass point the wind blew FROM, its length = hours,
// its shades = speed classes (one hue, light to dark), classed on km/h. Each hourly record counts once, by its
// average speed and its vector-mean direction; calm hours sit in the centre, not on a spoke.
const ROSE = [[1, 5, 'rose_m0', 'rose_i0', .38], [6, 19, 'rose_m1', 'rose_i1', .62], [20, Infinity, 'rose_m2', 'rose_i2', 1]];  // labels: WS keys
const POINTS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
// the hours (readings) per compass point and speed class, the calm ones apart: day records carry hours per sector only
// ("rose"), so one shade; their calm_hours count the centre
function roseBins(R, days) {
  const cls = v => ROSE.findIndex(([lo, hi]) => Math.round(toKmh(v)) >= lo && Math.round(toKmh(v)) <= hi);
  const bins = POINTS.map(() => [0, 0, 0]); let calm = 0, n = 0;
  if (days) R.forEach(r => { if (r.rose) r.rose.forEach((k, i) => { bins[i][1] += k; n += k; }); calm += r.calm || 0; n += r.calm || 0; });
  else R.forEach(r => { if (r.mid == null || r.dir == null) return; n++; if (Math.round(toKmh(r.mid)) < 1) { calm++; return; } bins[Math.round(r.dir / 45) % 8][cls(r.mid)]++; });
  return { bins, calm, n, tot: bins.map(b => b[0] + b[1] + b[2]) };
}
// the range's summary: [value with its unit, label] per figure, from the records the range was
// built from (hours, or days when `days`), never from 6-h or weekly buckets. Averages weigh each record by its readings (n);
// lowest / highest come from the records' own lo / hi (mid where a record has none); a figure the records cannot support is
// left out rather than shown as zero (a sun-hour count needs light readings, "wettest" needs rain). `meas` the sheet, `k` its
// headline field, `hid` the station's hidden fields, `rej` its rejected ones, `rg` the range (now / today / yday / days); the values
// arrive in the page's units.
const SUN_WM2 = 120;  // the WMO sunshine threshold, applied to the light sensor's hourly mean (a proxy for direct sun)
function rangeStats(meas, R, k, days, hid, rej, rg) {
  const u = (v, unit) => `${v}<small>${unit}</small>`, has = f => !hid.has(f);
  const val = () => R.filter(r => r.mid != null);
  const avg = f => { let s = 0, w = 0; R.forEach(r => { if (r[f] != null) { const n = r.n || 1; s += r[f] * n; w += n; } }); return w ? s / w : null; };
  const lo = () => { const a = R.map(r => r.lo ?? r.mid).filter(x => x != null); return a.length ? Math.min(...a) : null; };
  const hi = () => { const a = R.map(r => r.hi ?? r.mid).filter(x => x != null); return a.length ? Math.max(...a) : null; };
  const X = [], add = (v, l) => { if (v != null) X.push([v, l]); };
  if (!val().length) return X;
  if (meas === 'wind') {
    // with the gust as the headline each record's value is that hour's (day's) maximum gust: an average of maxima is no
    // average gust, so that sheet shows the strongest only
    if (k !== 'gust_kmh') add(u(f1(avg('mid')), U.w), WS.s_avg);
    const g = k === 'gust_kmh' ? hi() : maxOf(R.map(r => r.g));
    if (g != null && has('gust_kmh')) add(u(f1(g), U.w), WS.s_gust);
    if (has('wind_dir')) { const { tot } = roseBins(R, days), mx = Math.max(...tot); if (mx > 0) add(POINTS[tot.indexOf(mx)], WS.s_from); }
  } else if (meas === 'rain') {
    const a = val(), tot = a.reduce((s, r) => s + r.mid, 0);
    add(u(fr(tot), U.r), WS.s_total);
    if (tot > 0) { const w = a.reduce((b, r) => r.mid > b.mid ? r : b);
      add(u(fr(w.mid), U.r), FC.t(days ? WS.s_wet_d : rg === 'days' ? WS.s_wet_hd : WS.s_wet_h, { d: dm(w.t), t: hh(w.t) })); }
  } else if (meas === 'sun') {
    add(u((k === 'uv' ? uvf(R.map(r => r.hi ?? r.mid)) : f0)(hi()), k === 'uv' ? 'UV' : 'W/m²'), WS.s_high);
    // hours of sun: hourly records only, and only with light readings (unknown is not zero)
    const sol = r => k === 'uv' ? r.sol : r.mid, S = R.filter(r => sol(r) != null);
    if (!days && has('solar_wm2') && !rej.includes('solar_wm2') && S.length) add(u(S.filter(r => sol(r) > SUN_WM2).length, 'h'), WS.s_sun);
  } else {  // temperature, humidity, pressure (and any other sheet): average, lowest / highest; pressure: its change instead of an average
    const un = meas === 'hum' ? '%' : meas === 'baro' ? U.p : M[meas].u, f = meas === 'hum' ? f0 : meas === 'baro' ? fp : M[meas].dec === 0 ? f0 : f1;
    if (meas !== 'baro') add(u(f(avg('mid')), un), WS.s_avg);
    add(u(f(lo()) + ' / ' + f(hi()), un), WS.s_lohi);
    if (meas === 'temp' && k === 'temp_c' && has('dewpt_c') && avg('dew') != null) add(u(f1(avg('dew')), U.t), WS.s_dew);
    // the sign follows the shown figure: a change that rounds to zero is "±0.0", never "+0.0"
    const a = val(), d = a.length > 1 ? a[a.length - 1].mid - a[0].mid : null, ds = d != null && fp(Math.abs(d));
    if (meas === 'baro' && d != null) add(u((ds === fp(0) ? '±' : d > 0 ? '+' : '−') + ds, U.p), WS.s_change);
  }
  return X;
}
// the range row under the chart: none on "Now" (live readings, not a range) or for indoor; Today / Yesterday from their hour
// records, From–To from the records the range was built from (RAW: hours, or days past 7 days), never from its 6-h or weekly
// buckets. The head word is the range's own: the seg button's word or the From–To dates. `z` the span (SPZ).
function rangeSum(rg, z, all, raw, meas, k, hid, rej) {
  if (rg === 'now' || meas === 'indoor') return '';
  const d = rg === 'days', X = rangeStats(meas, d ? raw : all, k, d && z.step > 6, hid, rej, rg),
    head = d ? (!z.a ? '' : +z.a === +z.b ? dm(z.a) : `${dm(z.a)}–${dm(z.b)}`) : rg === 'yday' ? WS.yday : WS.today;
  return X.length ? `<div class="rsum" role="group" aria-label="${esc(FC.t(WS.rs_aria, { head }))}"><p class="rsh">${esc(head)}</p><div class="rsv">${X.map(([a, b]) => `<div class="stat"><span class="v">${a}</span><span class="k">${b}</span></div>`).join('')}</div></div>` : '';
}
// the Now row: the station's current values only, each with its unit on the number; [value, label, the field it shows], the
// headline first; a hidden field, or one the headline already shows, drops out. `tr` the tiles' trends (S.tr), `fx` the
// headline's number format, `ext` indoor's lowest / highest. A rejected light reading keeps its name ("light, now").
function nowStats(meas, v, m, k, tr, fx, ext) {
  const u = (a, unit) => `${a}<small>${unit}</small>`, meta = M[meas], seen = new Set();
  // console totals (week / month / year) when the console sends them, else the last 7 days of our own day records
  const rainTot = v.rain_weekly_mm != null
    ? [[u(fr(v.rain_weekly_mm), U.r), WS.n_week, 'rain_weekly_mm'], [u(fr(v.rain_monthly_mm), U.r), WS.n_month, 'rain_monthly_mm'], [u(fr(v.rain_yearly_mm), U.r), WS.n_year, 'rain_yearly_mm']]
    : m.days.length ? [[u(fr(m.days.slice(-7).reduce((s, x) => s + (x.rain_mm || 0), 0)), U.r), WS.n_7d]] : [];
  const tS = t => t ? [[u(trV(t), U[t[1]]) + ' ' + trGl(t).replace('class="tri', 'class="tri in'), trW(t)]] : [];  // the tile's trend, same objects; the glyph inline at the value's size
  return ({
    wind: [[u(f1(v[k]), U.w), k === 'gust_kmh' ? WS.n_gust : WS.n_read, k], k === 'wind_kmh' && calm(v) ? [WS.n_calm, WS.n_dir, 'wind_dir'] : [card(v.wind_dir) + ' ' + (v.wind_dir ?? '–') + '°', WS.n_from, 'wind_dir']].concat(v.gust_10m_kmh != null ? [[u(f1(v.gust_10m_kmh), U.w), WS.n_gust10, 'gust_10m_kmh']] : []),  // the tile's 10-min gust
    rain: [[u(fr(v.rain_daily_mm), U.r), WS.n_today, 'rain_daily_mm'], [u(fr(v.rain_mm), U.r), WS.n_hour, 'rain_mm']].concat(rainTot),
    hum: [[u(f0(v.humidity), '%'), WS.n_read, k]].concat(tS(tr && tr.hum)),
    // feels like has its own tag: it is not the temperature the headline already shows, and it goes with the temperature hidden
    temp: [[u(f1(v[k]), U.t), WS.n_read, k]].concat(k === 'temp_c' ? tS(tr && tr.temp) : [], [[u(f1(v.dewpt_c), U.t), WS.n_dew, 'dewpt_c']], feels(v) != null ? [[u(f1(feels(v)), U.t), WS.n_feels, 'feels']] : []),
    baro: [[u(fp(v.pressure_hpa), U.p), WS.n_read, k]].concat(tS(tr && tr.baro), v.pressure_abs_hpa != null ? [[u(fp(v.pressure_abs_hpa), U.p), WS.n_abs, 'pressure_abs_hpa']] : []),
    sun: [[u(uvf(m.recent.map(x => x.uv).concat(v.uv))(v.uv), 'UV'), WS.n_read, 'uv'], m.rej.includes('solar_wm2') ? [WS.n_rej, WS.n_light, 'solar_wm2'] : [u(f0(v.solar_wm2), 'W/m²'), WS.n_light, 'solar_wm2']],
    indoor: [[u(fx(v[k]), meta.u), WS.n_read, k], [u(f0(v.indoor_humidity), '%'), WS.n_hum, 'indoor_humidity'], [u(ext('lo', fx) + ' / ' + ext('hi', fx), meta.u), WS.s_lohi]],
  }[meas] || [[u(fx(v[k]), meta.u), WS.n_read, k]])
    .filter(([, , f]) => !f || (!m.hid.has(f === 'feels' ? 'temp_c' : f) && !seen.has(f) && seen.add(f)));
}
function windRose(R, days) {
  const S0 = 260, c = S0 / 2, R0 = 16, Rr = 100;
  const { bins, calm, n } = roseBins(R, days);
  const hu = range === 'now' ? '' : ' h', hw = range !== 'now';  // hours, or readings on "Now"
  if (!n) return '';
  const tot = bins.map(b => b[0] + b[1] + b[2]), mx = Math.max(...tot, 1), p10 = 10 ** Math.floor(Math.log10(mx / 3)),
    step = Math.max(1, [1, 2, 2.5, 5, 10].map(k => k * p10).find(k => k >= mx / 3.5)), top = Math.ceil(mx / step) * step;
  const rad = h => R0 + h / top * (Rr - R0), pt = (deg, r) => [(c + Math.sin(deg * Math.PI / 180) * r).toFixed(1), (c - Math.cos(deg * Math.PI / 180) * r).toFixed(1)];
  const wedge = (deg, r0, r1) => { const [a, b] = [deg - 18, deg + 18], p = [pt(a, r0), pt(a, r1), pt(b, r1), pt(b, r0)];
    return `M${p[0]}L${p[1]}A${r1.toFixed(1)} ${r1.toFixed(1)} 0 0 1 ${p[2]}L${p[3]}A${r0.toFixed(1)} ${r0.toFixed(1)} 0 0 0 ${p[0]}Z`; };
  let g = '';
  for (let h = step; h <= top; h += step) g += `<circle cx="${c}" cy="${c}" r="${rad(h).toFixed(1)}" fill="none" stroke="var(--grid)"/>`;
  g += POINTS.map((_, i) => { const [x1, y1] = pt(i * 45, R0), [x2, y2] = pt(i * 45, Rr); return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="var(--grid)"/>`; }).join('');
  // ring values sit in the gap between N and NE, drawn over the spokes with a halo
  let lab = '';
  for (let h = step; h <= top; h += step) { const [x, y] = pt(22.5, rad(h)); lab += `<text x="${(+x + 2).toFixed(1)}" y="${y}" dy="3" style="font-size:10px" paint-order="stroke" stroke="var(--ring)" stroke-width="3" stroke-linejoin="round">${h}${hu}</text>`; }
  [['N', 0], ['E', 90], ['S', 180], ['W', 270]].forEach(([l, d]) => { const [x, y] = pt(d, Rr + 14); g += `<text x="${x}" y="${(+y + 4).toFixed(1)}" text-anchor="middle" font-weight="600">${l}</text>`; });
  const label = j => WS[ROSE[j][IMP ? 3 : 2]];
  bins.forEach((b, i) => { if (!tot[i]) return; let acc = 0;
    const parts = days ? FC.t(WS.rose_pct, { pct: Math.round(tot[i] / n * 100) }) : b.map((k, j) => k ? FC.t(hw ? WS.rose_part_h : WS.rose_part_r, { k, l: label(j) }) : '').filter(Boolean).join(', ');
    g += `<g><title>${FC.t(hw ? WS.rose_tip_h : WS.rose_tip_r, { p: POINTS[i], n: tot[i], parts })}</title>` + b.map((k, j) => { if (!k) return ''; const r0 = rad(acc), r1 = rad(acc += k);
      return `<path d="${wedge(i * 45, r0, r1)}" fill="var(--wind)" fill-opacity="${ROSE[j][4]}" stroke="var(--ring)" stroke-width="2" stroke-linejoin="round"/>`; }).join('') + '</g>'; });
  g += lab + `<circle cx="${c}" cy="${c}" r="${R0 - 3}" fill="none" stroke="var(--ink-3)"/>${calm ? `<text x="${c}" y="${c + 4}" text-anchor="middle" style="font-size:10px">${calm}</text>` : ''}`;
  const best = tot.indexOf(mx), sw = o => `<svg viewBox="0 0 22 10" aria-hidden="true"><rect x="0" y="1" width="22" height="8" rx="2" fill="var(--wind)" fill-opacity="${o}"/></svg>`;
  const head = FC.t(days ? WS.rose_head_d : hw ? WS.rose_head_h : WS.rose_head_r, { p: POINTS[best], pct: Math.round(mx / n * 100), mx, n });
  const list = bins.map((b, i) => tot[i] ? POINTS[i] + ' ' + tot[i] + hu : '').filter(Boolean).join(', ');
  return `<div><svg class="chart" viewBox="0 0 260 260" style="max-width:300px;margin:0 auto" role="img" aria-label="${FC.t(calm ? WS.rose_aria_calm : WS.rose_aria, { head: head.replace(/<[^>]+>/g, ''), list, calm: calm + hu })}">${g}</svg></div>
    <div><p class="cap" style="margin-top:0">${head}</p>
    <div class="legend" style="flex-direction:column;align-items:flex-start;gap:8px">${days ? `<span>${sw(ROSE[1][4])}${WS.rose_hours}</span>` : ROSE.map((_, j) => `<span>${sw(ROSE[j][4])}${label(j)}</span>`).join('')}<span><svg viewBox="0 0 22 10" aria-hidden="true"><circle cx="11" cy="5" r="4" fill="none" stroke="var(--ink-3)"/></svg>${calm ? FC.t(hw ? WS.rose_calm_h : WS.rose_calm_r, { n: calm }) : WS.rose_calm0}</span></div>
    ${days ? `<p class="cap">${WS.rose_cap_d}</p></div>` : `
    <p class="cap">${hw ? WS.rose_cap_h : WS.rose_cap_r}</p></div>`}`;
}
renderSheet = function () {
  const m = model(), meta = M[measure];
  if (!meta) { closeSheet(); return; }
  // the chart, legend, rose, stat row and crosshair use the newest run ("Now"); the table lists every reading
  const all = rows(), run = newestRun(all), R = run.R, v = m.v, cnt = R.filter(r => r.mid != null);
  $('#sheet').style.setProperty('--c', meta.c);
  $('#shTitle').innerHTML = icon(measure) + esc(meta.name);
  // the wind sheet names the force too, like the tile
  $('#shSub').textContent = (m.live.online ? m.name : FC.t(WS.sub_off, { name: m.name })) + (measure === 'wind' && v[meta.k] != null ? ' · ' + beaufort(v[meta.k]) : '');
  // UV readings ("Now" and the live value) without a decimal when all are whole; hourly averages keep one
  const k = meta.k, fu = range === 'now' ? uvf(all.map(r => k === 'uv' ? r.mid : r.uv)) : f1;
  const fx = k === 'uv' && range === 'now' ? fu : meta.dec === 0 ? f0 : meta.dec >= 2 ? f2 : f1;
  // the range's lowest / highest are readings, so UV takes the same integer rule there
  const fe = k === 'uv' ? uvf(cnt.flatMap(r => [r.lo ?? r.mid, r.hi ?? r.mid])) : fx;
  const ext = (k, f) => cnt.length ? f(k === 'lo' ? Math.min(...cnt.map(r => r.lo ?? r.mid)) : Math.max(...cnt.map(r => r.hi ?? r.mid))) : '–';
  // no "computed from N readings" line under a stat: it tells a user nothing and adds a
  // line; the tile's tooltip and Details keep it
  // promoted fields are live only: no history kept; nor for rain when its daily counter is hidden
  if (meta.promoted || meta.nohist) {
    $('#shNow').innerHTML = meta.nohist ? `<div class="stat"><span class="v">${fr(v.rain_mm)}</span><span class="k">${FC.t(WS.u_hour, { u: U.r })}</span></div>`
      : `<div class="stat"><span class="v">${fx(v[measure])}</span><span class="k">${FC.t(WS.u_now, { u: esc(meta.u) })}</span></div>`;
    $('.tools').hidden = true; $('#shRange').hidden = true;
    $('#shBody').innerHTML = `<p class="cap">${meta.nohist ? WS.nohist : WS.promoted}</p>`;
    return;
  }
  $('.tools').hidden = false; $('#shRange').hidden = range !== 'days';
  if (range === 'days') rangeRow(SPZ);
  const off = m.live.online ? '' : (S.live && S.live.time_synced && offSince(m.live.last_seen, new Date())) || WS.offline;
  $('#shOff').hidden = !off; $('#shOff').innerHTML = off && `<span class="dot off"></span>${off}`; $('#shOff').title = WD.off_why;
  const nowV = nowStats(measure, v, m, k, S.tr, fx, ext);
  const sws = swOf(measure), on = f => sws.length > 1 && sws.includes(f);
  $('#shNow').innerHTML = `<p class="nowh">${WS.now_h}</p>` + nowV.map(([a, b, f]) => on(f) ? `<button type="button" class="stat sw" data-f="${f}" aria-pressed="${f === k}"><span class="v">${a}</span><span class="k">${b}</span></button>`
    : `<div class="stat"><span class="v">${a}</span><span class="k">${b}</span></div>`).join('');
  if (!R.length) {
    $('#shBody').innerHTML = `<p class="cap">${range === 'days' ? (SPZ.raw ? WS.e_range : SPZ.err ? WS.e_load : loadingCap()) : range === 'yday' ? WS.e_yday : range === 'now' ? (S.recent ? WS.e_now : loadingCap()) : S.hourly ? WS.e_today : loadingCap()}</p>`;
    return;
  }
  const ch = chart(R, fx);
  let h = legendFor(R) + `<div class="cw" id="cw">${ch.svg}<div class="tip" id="tip" hidden></div></div>`;
  h += rangeSum(range, SPZ, all, RAW, measure, k, m.hid, m.rej);
  // a short range older than the hour records shows their days instead, and says so
  if (range === 'days' && SPZ.fb !== undefined) h += `<p class="cap">${SPZ.fb ? FC.t(WS.fb, { d: dm(SPZ.fb) }) : WS.fb0}</p>`;
  if (run.left) { const n = R.length;
    h += `<p class="cap">${FC.t(WS.latest, { n, from: hm(R[0].t), to: hm(R[n - 1].t), end: hm(all[run.left - 1].t) })}</p>`; }
  if (measure === 'wind' && !m.hid.has('wind_dir')) h += `<div class="twin">${windRose(range === 'days' ? RAW : R, range === 'days' && SPZ.step > 6)}</div>`;
  if (table) {
    const sp = R.map(r => r.hi != null && r.mid != null ? r.hi - r.mid : 0), med = sp.slice().sort((a, b) => a - b)[sp.length >> 1] || 0;
    const Hd = m.hid;  // a column per extra field, unless hidden or the headline itself
    const cg = measure === 'wind' && k === 'wind_kmh' && !Hd.has('gust_kmh'), cd = measure === 'wind' && !Hd.has('wind_dir');
    const cw = measure === 'temp' && k === 'temp_c' && !Hd.has('dewpt_c'), cu = measure === 'sun' && k !== 'uv' && !Hd.has('uv'), cs = measure === 'sun' && k === 'uv' && !Hd.has('solar_wm2');
    const nt = range === 'now', TR = nt ? all.slice().reverse() : R;  // "Now": every reading in the ring, newest first
    // Export CSV sits on the table's own row, right-aligned, only while the table is open
    h += `<div class="csv"><button type="button" class="seg" id="csvBtn" style="padding:7px 14px;font-size:13px;cursor:pointer;background:transparent">${WS.csv}</button></div><div class="tw"><table><thead><tr><th>${nt ? WS.th_time : WS['th_' + (range === 'days' ? SPZ.step : 1)]}</th>${measure === 'rain' ? `<th>${U.r}</th>` : nt ? `<th>${esc(meta.u)}</th>` : `<th>${WS.th_lo}</th><th>${WS.th_avg}</th><th>${WS.th_hi}</th>`}${cg ? `<th>${WS.th_gust}</th>` : ''}${cd ? `<th>${WS.th_from}</th>` : ''}${cw ? `<th>${WS.th_dew}</th>` : ''}${cu ? '<th>UV</th>' : ''}${cs ? '<th>W/m²</th>' : ''}${nt ? '' : `<th>${WS.th_n}</th><th>${WS.th_rej}</th>`}</tr></thead><tbody>` +
      TR.map((r, i) => `<tr><td>${r.open ? FC.t(WS.so_far, { lab: r.lab }) : r.lab}</td>${measure === 'rain' ? `<td>${fr(r.mid)}</td>` : nt ? `<td>${fx(r.mid)}</td>` : r.mid == null ? `<td colspan="3">${WS.no_reading}</td>` : `<td>${fx(r.lo)}</td><td>${fx(r.mid)}</td>${med > 0 && sp[i] > med * 3 ? `<td class="flag" title="${WS.flag}">` : '<td>'}${fx(r.hi)}</td>`}${cg ? `<td>${f1(r.g)}</td>` : ''}${cd ? `<td>${card(r.dir)}</td>` : ''}${cw ? `<td>${f1(r.dew)}</td>` : ''}${cu ? `<td>${fu(r.uv)}</td>` : ''}${cs ? `<td>${f0(r.sol)}</td>` : ''}${nt ? '' : `<td>${r.n}</td><td>${r.rej || ''}</td>`}</tr>`).join('') + '</tbody></table></div>';
  }
  $('#shBody').innerHTML = h;
  // crosshair + tooltip, mouse and touch
  const svg = $('#chart'), tip = $('#tip'), xh = $('#xh');
  const show = e => {
    const b = svg.getBoundingClientRect(), px = (e.clientX - b.left) / b.width * ch.W;
    const slotI = Math.floor((px - 44) / ch.slot);
    let i = R.reduce((best, r, j) => Math.abs(ch.xi(j) - slotI) < Math.abs(ch.xi(best) - slotI) ? j : best, 0);
    i = Math.max(0, Math.min(R.length - 1, i));
    const r = R[i], x = ch.X(i);
    xh.setAttribute('x1', x); xh.setAttribute('x2', x); xh.style.display = '';
    // a small labelled card: period, then one row per value (label left, value right)
    const u = ' ' + meta.u, rowsT = [];
    const row = (k, val, cls) => rowsT.push(`<dt>${k}</dt><dd${cls ? ` class="${cls}"` : ''}>${val}</dd>`);
    if (measure === 'rain') row(WS.t_rain, fr(r.mid) + u);
    else if (r.mid == null) row(meta.name, WS.no_reading, 'warn');  // rejected, or the sensor was not updating
    else {
      row(range === 'now' ? meta.name : WS.t_avg, fx(r.mid) + u);
      if (measure === 'wind') {
        if (r.g != null) row(WS.t_gust, f1(r.g) + u);
        if (r.dir != null) row(WS.t_from, `<svg viewBox="-8 -8 16 16" aria-hidden="true"><path transform="rotate(${(r.dir + 180) % 360})" d="M0 -6L4 5L0 2.5L-4 5Z" fill="currentColor"/></svg>${card(r.dir)} <small>${r.dir}°</small>`);
      }
      // a temperature can be negative: ' / ' as in the sheet's 'lowest / highest', never '−6.5–−1.3'
      if (r.lo != null) row(WS.t_range, fx(r.lo) + (measure === 'temp' || measure === 'indoor' ? ' / ' : '–') + fx(r.hi) + u);
      if (measure === 'temp' && r.dew != null) row(WS.t_dew, f1(r.dew) + u);
      if (measure === 'sun' && r.uv != null) row(WS.t_uv, fu(r.uv));
      if (measure === 'sun' && r.sol != null) row(WS.t_sun, f0(r.sol) + ' W/m²');
    }
    if (r.rej) row(WS.t_rej, FC.t(WS.t_rej_n, { n: r.rej }), 'warn');
    const end = range === 'days' || range === 'now' ? '' : '–' + hh(new Date(r.t.getTime() + HOUR));
    tip.innerHTML = `<b>${r.open ? FC.t(WS.t_open, { lab: r.lab + end }) : r.lab + end}</b><dl>${rowsT.join('')}</dl>`;
    tip.hidden = false;
    // beside the crosshair, flipped to the left near the right edge, so it never hides the point being read
    const cx = x / ch.W * b.width, w = tip.offsetWidth, left = cx + 14 + w <= b.width ? cx + 14 : Math.max(0, cx - 14 - w);
    tip.style.left = left + 'px'; tip.style.top = '4px';
  };
  svg.addEventListener('pointermove', show); svg.addEventListener('pointerdown', show);
  svg.addEventListener('pointerleave', () => { tip.hidden = true; xh.style.display = 'none'; });
};

// From / To stay in the sheet's tools row while the calendar segment is on (native date fields: the OS draws the
// picker in the page's colour scheme); the preset row under them opens with the calendar and closes on a pick.
// Built once; a field being edited is never overwritten by a re-render.
function rangeRow(z) {
  const el = $('#shRange');
  if (!el.firstChild) el.innerHTML = `<div class="dr"><label>${WS.from} <input type="date" id="spA"></label><label>${WS.to} <input type="date" id="spB"></label></div><div class="seg" id="spP" role="group" aria-label="${WS.dates}">${[7, 30, 90, 365, 'all'].map(v => [v, WS['sp' + v], WS['sp' + v + 'a']])
    .map(([v, t, a]) => `<button type="button" data-sp="${v}" aria-label="${a}">${t}</button>`).join('')}</div>`;
  el.querySelectorAll('[data-sp]').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.sp === String(SP.p))));
  $('#spP').hidden = !spOpen;
  const A = $('#spA'), B = $('#spB'), act = document.activeElement;
  A.max = B.max = ymd(new Date()); A.min = B.min = ymd(addD(new Date(), -735));
  if (act !== A) A.value = z.a ? ymd(z.a) : '';
  if (act !== B) B.value = ymd(z.b);
}
// the calendar segment: switching to it opens the presets; pressed again it opens or closes them (capture: this runs
// before dash.js sets the range)
$('#seg').addEventListener('click', e => { const b = e.target.closest('button'); if (b && b.dataset.r === 'days') spOpen = range !== 'days' || !spOpen; }, true);
// a preset closes the row; the active one picked again fetches again only after an error or 10 min;
// a pending typed date is dropped
$('#shRange').addEventListener('click', e => { const b = e.target.closest('[data-sp]'); if (!b) return;
  clearTimeout(spT); spOpen = false;
  if (b.dataset.sp !== String(SP.p) || SPC.err || Date.now() - SPC.at > 600e3) { SP = { p: b.dataset.sp === 'all' ? 'all' : +b.dataset.sp }; SPC.want = ''; }
  renderSheet(); });
// typed dates wait 0.7 s (a desktop field reports every complete date while the year is typed), reversed dates swap,
// nothing after today or before the rings' reach; the same dates typed again after an error fetch again
$('#shRange').addEventListener('change', () => { clearTimeout(spT); spT = setTimeout(() => {
  const td = addD(new Date(), 0), lo = addD(td, -735), d = id => { const v = $(id).value, t = v && new Date(v + 'T00:00'); return t && !isNaN(t) ? t < lo ? lo : t > td ? td : t : null; };
  let a = d('#spA'), b = d('#spB');
  if (!a || !b) return;
  if (a > b) [a, b] = [b, a];
  if (SPC.err) SPC.want = '';
  SP = { p: 0, a, b }; renderSheet(); }, 700); });

// Export CSV: the records behind the shown range, every field, in the units chosen on this page: the
// recent ring for "Now", hour records for Today, Yesterday and ranges up to 7 days, day records above (All: every day
// on flash). They are the answer the sheet already holds, so an export costs no request of its own;
// the ESP never formats CSV. Machine interface: English headers, '.' decimals, ISO dates, never tr().
// one set of column words for the three files; a reading's rain is what fell since the reading before
const CSV_NM = { temp_c: 'temperature', dewpt_c: 'dew point', humidity: 'humidity', pressure_hpa: 'pressure', wind_kmh: 'wind', gust_kmh: 'gust',
  wind_dir: 'wind direction', rain_mm: 'rain since the reading before', rain_daily_mm: 'rain today', uv: 'UV index', solar_wm2: 'solar' };
const csvNum = (c, v) => { if (v == null) return ''; const x = cvv(c || null, v); return String(Math.round(x * 100) / 100); };
// the record files' figures: [field, unit kind, which of min / avg / max (bits 1 / 2 / 4) an hour record keeps, a day record
// keeps] as src/web_render.cpp hourRow / dayRow write them: a column the record never stores is left out, not written empty
const CSV_MAM = [['temp_c', 't', 7, 7], ['dewpt_c', 't', 2, 7], ['humidity', '', 7, 7], ['pressure_hpa', 'p', 2, 7], ['wind_kmh', 'w', 2, 2], ['uv', '', 4, 4], ['solar_wm2', '', 6, 6]];
// the hour (d false) or day file's header and rows from the records, `on(k)` the station shows the field, `open` the open record
function recCsv(recs, d, on, open) {
  const unit = c => c ? U[c] : '', loc = t => ymd(t) + ' ' + hm(t), mx = r => Math.max(...r);
  const MAM = CSV_MAM.filter(([k]) => on(k)).map(([k, c, h, dk]) => [k, CSV_NM[k], c, [0, 1, 2].filter(i => (d ? dk : h) >> i & 1)]);
  // the compass point beside the degrees; a day without a steady direction takes its rose's leading sector (the first of a
  // tie), the degrees then stay empty: no number is made up
  const cmp = x => x.wind_dir != null ? card(x.wind_dir) : d && x.rose && mx(x.rose) > 0 ? POINTS[x.rose.indexOf(mx(x.rose))] : '';
  // the day's calm hours (the rose's centre) and the peak rain rate ride along
  const rest = [[on('rain_daily_mm'), `rain (${U.r})`, x => csvNum('r', x.rain_mm)], [on('rain_daily_mm'), `rain rate max (${U.r}/h)`, x => csvNum('r', x.rain_rate_max)],
    [on('gust_kmh'), `gust max (${U.w})`, x => csvNum('w', x.gust_kmh)], [on('wind_dir'), 'wind direction (°)', x => x.wind_dir ?? ''],
    [on('wind_dir'), 'wind direction (compass)', cmp], [d && on('wind_kmh'), 'calm hours', x => x.calm_hours ?? '']].filter(c => c[0]);
  const rose = d && on('wind_dir');
  // the hour file also carries the UTC hour, so the autumn DST day's two 02:00 rows stay apart
  return [(d ? ['date'] : ['hour', 'hour_utc']).concat(MAM.flatMap(([, l, c, I]) => I.map(i => `${l} ${['min', 'avg', 'max'][i]}${unit(c) ? ' (' + unit(c) + ')' : l === 'humidity' ? ' (%)' : l === 'solar' ? ' (W/m²)' : ''}`)),
    rest.map(c => c[1]), ['samples', 'rejected'], rose ? POINTS.map(p => 'hours from ' + p) : [], ['status']),
  recs.map(x => (d ? [x.d] : [loc(new Date(x.h)), x.h]).concat(
    MAM.flatMap(([k, , c, I]) => I.map(i => x[k] ? csvNum(c, x[k][i]) : '')),
    rest.map(c => c[2](x)), [x.n, x.rej_fields || 0], rose ? x.rose || ['', '', '', '', '', '', '', ''] : [],
    [x === open ? 'open' : 'closed']).join(','))];
}
function exportCsv(btn) {
  const H = hidSet(), on = k => !H.has(k), q = t => '"' + String(t).replace(/"/g, '""') + '"', z = range === 'days' ? SPZ : null;
  const loc = t => ymd(t) + ' ' + hm(t);
  const cu = k => CONV[k] ? U[CONV[k]] : { humidity: '%', solar_wm2: 'W/m²', wind_dir: '°' }[k] || '';
  // never another station's rows under this station's name (a switch between two polls)
  const mine = x => { if (!x || x.station !== S.cur) throw new Error('station'); return x; };
  let head, lines, kind;
  try {
    if (range === 'now') {
      const Rr = mine(S.recent), f = Rr.fields.slice(1);
      kind = 'readings';
      head = ['time'].concat(f.map(k => (CSV_NM[k] || k) + (cu(k) ? ` (${cu(k)})` : '')));
      lines = Rr.rows.filter(r => r[0] >= 1e9).sort((a, b) => a[0] - b[0]).map(r => { const t = new Date(r[0] * 1e3);
        return [loc(t) + ':' + p2(t.getSeconds())].concat(f.map((k, j) => csvNum(CONV[k], r[j + 1]))).join(','); });
    } else {
      const d = !!z && z.step > 6, raw = mine(z ? z.raw : S.hourly), mid = addD(new Date(), 0);
      const a = z ? z.a : range === 'yday' ? addD(mid, -1) : mid, b = z ? addD(z.b, 1) : range === 'yday' ? mid : addD(mid, 1);
      const recs = (d ? raw.days : raw.hours).concat(raw.open ? [raw.open] : []).filter(x => { const t = new Date(d ? x.d + 'T00:00' : x.h); return t >= a && t < b; });
      kind = d ? 'daily' : 'hourly';
      [head, lines] = recCsv(recs, d, on, raw.open);
    }
  } catch (e) { btn.textContent = WS.csv_fail; return; }
  const el = document.createElement('a');
  // the BOM tells Excel the file is UTF-8 (°, ², names with diacritics); other programs ignore it
  el.href = URL.createObjectURL(new Blob([String.fromCharCode(0xFEFF) + [head.map(q).join(',')].concat(lines).join('\r\n') + '\r\n'], { type: 'text/csv;charset=utf-8' }));
  // the export's own date and time in the name: several exports stay apart and sort by time
  const t = new Date();
  el.download = `fauxcloud-${S.cur}-${kind}-${ymd(t)}-${p2(t.getHours())}${p2(t.getMinutes())}.csv`; el.click();
  setTimeout(() => URL.revokeObjectURL(el.href), 1000);
  btn.textContent = FC.t(WS.csv_done, { n: lines.length });
}

// the Now header and the range row under the chart: injected once here, not in the initial stylesheet
document.head.appendChild(Object.assign(document.createElement('style'), { textContent: '.nowh,.rsh{margin:0;flex-basis:100%;font-size:12px;font-weight:600;letter-spacing:.04em;text-transform:uppercase;color:var(--ink-3)}'
  + '.rsum{margin:10px 0 4px;padding-top:10px;border-top:1px solid var(--grid)}.rsv{display:flex;flex-wrap:wrap;gap:8px 26px;margin-top:6px}.rsv .stat .v{font-size:18px;color:var(--ink-2)}.rsv .stat .v small{font-size:12px}' }));
