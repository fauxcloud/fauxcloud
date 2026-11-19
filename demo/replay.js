// demo/replay.js -- the data half of the /try/ demo's shim: every per-time body the dashboard and the admin read, derived
// from the fixtures (tools/demo_data.py) and a time t0 anywhere in the replayable year. Pure: no DOM, no network, no
// clock, no randomness beyond a stateless hash, so the same (path, t0, switches) always gives the same body.
// The site's shim replaces FC.get with answer(); the time shift to "now" is the shim's (demo/README.md).
//   const R = DemoReplay.create(files)   files: { 'index.json': {...}, 'history/series.json': {...}, ... } (parsed)
//   R.answer('/api/live?station=DEMO1', t0, { metar: true, wufc: true, late: 0 })   t0: unix seconds
// node demo/replay.js --write [fixtures dir]: writes snap/<key>/live.json and frames.json for every example day and
// scenario of index.json (the committed snapshots; test/tools/demo_vectors.js checks they are a fresh run).
'use strict';
(function (root, make) {
  if (typeof module === 'object' && module.exports) module.exports = make();
  else root.DemoReplay = make();
})(this, function () {
  const HR = 3600, STEP = 65;
  const Q = ['stale', 'baro_uncal', 'clock_approx', 'restart', 'baro_boundary', 'baro_bridge', 'gap', 'console_reset', 'rejected', 'rain_partial', 'epoch_split'];
  const UNCLEAN = 0x7f;
  // half away from zero at d decimals, never -0
  const r = (x, d) => { if (x == null || !isFinite(x)) return null; const m = 10 ** d, v = Math.sign(x) * Math.floor(Math.abs(x) * m + 0.5 + 1e-9) / m; return v || 0; };
  const clamp = (x, a, b) => Math.min(b, Math.max(a, x));
  const iso = t => new Date(t * 1e3).toISOString().slice(0, 19) + 'Z';
  const hkey = t => new Date(t * 1e3).toISOString().slice(0, 16) + 'Z';
  const sec = s => Date.parse(s) / 1e3;
  const hf = t => t - ((t % HR) + HR) % HR;
  // the station's zone: CET-1CEST,M3.5.0,M10.5.0/3 (the EU rule: last Sundays of March and October, 01:00 UTC)
  const lastSun = (y, m) => { const d = Date.UTC(y, m - 1, 31, 1) / 1e3; return d - new Date(d * 1e3).getUTCDay() * 86400; };
  const off = t => { const y = new Date(t * 1e3).getUTCFullYear(); return t >= lastSun(y, 3) && t < lastSun(y, 10) ? 7200 : 3600; };
  const lt = t => new Date((t + off(t)) * 1e3);  // local wall time in the UTC getters
  const dkey = t => lt(t).toISOString().slice(0, 10);
  const midnight = t => { const l = lt(t), u = Date.UTC(l.getUTCFullYear(), l.getUTCMonth(), l.getUTCDate()) / 1e3; return u - off(u - 7200); };
  const hm = t => lt(t).toISOString().slice(11, 16);
  // a stateless hash in [0, 1) and value noise in [-1, 1] with period P seconds (smoothstep between knots)
  function u32(k, n) {
    let x = (Math.imul(k + 1, 0x9e3779b1) ^ Math.imul(n | 0, 0x85ebca6b) ^ Math.floor(n / 4294967296)) >>> 0;
    x = Math.imul(x ^ (x >>> 16), 0x7feb352d) >>> 0; x = Math.imul(x ^ (x >>> 15), 0x846ca68b) >>> 0;
    return ((x ^ (x >>> 16)) >>> 0) / 4294967296;
  }
  function vn(k, t, P) { const i = Math.floor(t / P), f = t / P - i, s = f * f * (3 - 2 * f), a = 2 * u32(k, i) - 1; return a + (2 * u32(k, i + 1) - 1 - a) * s; }
  const magnus = t => Math.exp(17.625 * t / (243.04 + t));
  const tdOf = (T, rh) => { const x = Math.log(rh / 100) + 17.625 * T / (243.04 + T); return 243.04 * x / (17.625 - x); };
  const rad = Math.PI / 180;

  function create(F) {
    const I = F['index.json'], S = F[I.series.file], DAILY = F[I.daily.file].days, C = S.cols, LAT = I.lat, LON = I.lon, TIP = I.tip_mm;
    const H0 = sec(S.from), NH = S.hours, U0 = H0;
    const sun = t => {  // elevation and azimuth (degrees), as tools/demo_data.py
      const d = (t - 946728000) / 86400, g = (357.529 + 0.98560028 * d) * rad;
      const lam = (280.459 + 0.98564736 * d + 1.915 * Math.sin(g) + 0.020 * Math.sin(2 * g)) * rad, e = (23.439 - 3.6e-7 * d) * rad;
      const ra = Math.atan2(Math.cos(e) * Math.sin(lam), Math.cos(lam)), dec = Math.asin(Math.sin(e) * Math.sin(lam));
      const ha = ((((18.697374558 + 24.06570982441908 * d) % 24) * 15 + LON) * rad) - ra, la = LAT * rad;
      const el = Math.asin(Math.sin(la) * Math.sin(dec) + Math.cos(la) * Math.cos(dec) * Math.cos(ha));
      const az = Math.atan2(-Math.sin(ha), Math.tan(dec) * Math.cos(la) - Math.sin(la) * Math.cos(ha));
      return [el / rad, ((az / rad) % 360 + 360) % 360];
    };
    const clearSky = el => { const s = Math.sin(el * rad); return s > 0.01 ? 1098 * s * Math.exp(-0.057 / s) : 0; };
    const s2 = t => 1.16 * Math.cos(LAT * rad) ** 3 * Math.cos(2 * Math.PI * (t / HR + LON / 15 - 10) / 12);
    const lst = t => ((t / HR + LON / 15) % 24 + 24) % 24;
    const ix = h => Math.round((h - H0) / HR);
    const has = i => i >= 0 && i < NH && C.n[i] > 0;
    const rel = t => t >= sec(I.rel_err.from) && t < sec(I.rel_err.to) ? I.rel_err.hpa : 0;

    // ---- one hour record from the series (web_render.cpp hourRow's keys and order)
    function hourRow(i) {
      if (!has(i)) return null;
      const g = k => C[k][i];
      return { h: hkey(H0 + i * HR), n: g('n'), q: Q.filter((_, b) => g('q') >> b & 1), epoch: 0, rej_fields: g('q') >> 8 & 1,
        temp_c: [g('tlo'), g('tav'), g('thi')], dewpt_c: [null, g('td'), null], humidity: [g('rhlo'), g('rhav'), g('rhhi')],
        pressure_hpa: [null, g('p'), null], wind_kmh: [null, g('w'), null], gust_kmh: g('g'), wind_dir: g('dir'), wind_steady: g('st'),
        calm_n: g('calm'), rain_mm: g('rain'), rain_daily_mm: null, rain_rate_max: g('rmax'), uv: [null, null, g('uv')], solar_wm2: [null, g('sol'), g('solx')] };
    }
    // ---- one day record from its hour records (history.cpp; tools/demo_data.py day_row)
    function vecMean(P) {
      let x = 0, y = 0, tot = 0;
      P.forEach(([v, d]) => { if (v >= 1) { x += v * Math.sin(d * rad); y += v * Math.cos(d * rad); tot += v; } });
      return tot > 0 ? [((Math.round(Math.atan2(x, y) / rad) % 360) + 360) % 360, r(Math.hypot(x, y) / tot, 2)] : [null, null];
    }
    function dayRow(d, H) {
      const n = H.reduce((s, x) => s + x.n, 0), wavg = k => r(H.reduce((s, x) => s + x[k][1] * x.n, 0) / n, 1);
      const [wd, st] = vecMean(H.filter(x => x.wind_dir != null).map(x => [x.wind_kmh[1], x.wind_dir]));
      const rose = [0, 0, 0, 0, 0, 0, 0, 0];
      H.forEach(x => { if (x.wind_dir != null && x.calm_n * 2 < x.n) rose[Math.round(x.wind_dir / 45) % 8]++; });
      const qs = new Set(H.flatMap(x => x.q)), rain = r(H.reduce((s, x) => s + x.rain_mm, 0), 2), mn = a => Math.min(...a), mx = a => Math.max(...a);
      return { d, n, hours: H.length, clean_hours: H.filter(x => !x.q.some(f => Q.indexOf(f) < 7)).length, q: Q.filter(f => qs.has(f)), epoch: 0,
        rej_fields: mx(H.map(x => x.rej_fields)), temp_c: [mn(H.map(x => x.temp_c[0])), wavg('temp_c'), mx(H.map(x => x.temp_c[2]))],
        dewpt_c: [mn(H.map(x => x.dewpt_c[1])), wavg('dewpt_c'), mx(H.map(x => x.dewpt_c[1]))],
        humidity: [mn(H.map(x => x.humidity[0])), r(H.reduce((s, x) => s + x.humidity[1] * x.n, 0) / n, 0), mx(H.map(x => x.humidity[2]))],
        pressure_hpa: [mn(H.map(x => x.pressure_hpa[1])), wavg('pressure_hpa'), mx(H.map(x => x.pressure_hpa[1]))],
        wind_kmh: [null, wavg('wind_kmh'), null], gust_kmh: mx(H.map(x => x.gust_kmh)), wind_dir: wd, wind_steady: st,
        calm_hours: H.length - rose.reduce((a, b) => a + b, 0), rose, rain_mm: rain, rain_daily_mm: rain, rain_hours: H.filter(x => x.rain_mm > 0).length,
        rain_rate_max: mx(H.map(x => x.rain_rate_max)), uv: [null, null, mx(H.map(x => x.uv[2]))],
        solar_wm2: [null, r(H.reduce((s, x) => s + x.solar_wm2[1], 0) / H.length, 0), mx(H.map(x => x.solar_wm2[2]))] };
    }

    // ---- the 1-minute texture: the hourly means interpolated between hour middles, a bounded value noise on top,
    // limited to the hour record's own lo / hi (the frame rule, demo/README.md)
    function base(k, t) {
      const x = (t - H0 - 1800) / HR, j = Math.floor(x), f = x - j, a = C[k][j], b = C[k][j + 1];
      return a == null ? b : b == null ? a : a + (b - a) * f;
    }
    function baseDir(t) {
      const x = (t - H0 - 1800) / HR, j = Math.floor(x), f = x - j, a = C.dir[j], b = C.dir[j + 1];
      if (a == null || b == null) return a ?? b ?? 250;
      return a + ((((b - a) % 360) + 540) % 360 - 180) * f;
    }
    const hourOf = t => { let i = ix(hf(t)); while (i > 0 && !has(i)) i--; return i; };
    const tipsAt = i => { const k = Math.round((C.rain[i] || 0) / TIP), out = []; for (let j = 0; j < k; j++) out.push(Math.floor(HR * (j + u32(12, i * 97 + j)) / k)); return out; };
    const tipsIn = (a, b) => { let n = 0; for (let h = hf(a); h <= b; h += HR) { const i = ix(h); if (has(i)) tipsAt(i).forEach(s => { if (h + s > a && h + s <= b) n++; }); } return n; };
    function frame(t) {
      const i = hourOf(t), lo = C.tlo[i], hi = C.thi[i];
      const T = clamp(base('tav', t) + 0.35 * vn(1, t, 600) + 0.12 * vn(2, t, 120), lo, hi);
      const rh = clamp(Math.round(base('rhav', t) + 1.5 * vn(3, t, 900)), C.rhlo[i], C.rhhi[i]);
      const gmax = C.g[i], sp = clamp((base('w', t) || 0) * (1 + 0.45 * vn(5, t, 60) + 0.2 * vn(6, t, 300)), 0, gmax);
      const m = Math.floor(t / 60), gust = clamp(sp * (1.25 + 0.35 * u32(7, m)) + 1.5 * u32(8, m), sp, gmax);
      const sol = C.sol[i] ? clamp(base('sol', t) * (1 + 0.2 * vn(11, t, 300)), 0, C.solx[i]) : 0;
      const rmx = Math.max(C.rmax[i] || 0, has(i - 1) ? C.rmax[i - 1] : 0);
      return { temp_c: r(T, 1), dewpt_c: r(tdOf(T, rh), 1), humidity: rh, pressure_hpa: r(base('p', t) + 0.12 * vn(4, t, 1200), 1), wind_kmh: r(sp, 1),
        gust_kmh: r(gust, 1), wind_dir: ((Math.round(baseDir(t) + 12 * vn(9, t, 1800) + 9 * vn(10, t, 60)) % 360) + 360) % 360,
        rain_daily_mm: r(tipsIn(midnight(t) - 1, t) * TIP, 1), uv: r(clamp(base('uv', t) * 0.9, 0, C.uv[i]), 1), solar_wm2: r(sol, 0),
        rate: r(Math.min(tipsIn(t - 600, t) * TIP * 6, rmx), 1), hour_mm: r(tipsIn(t - HR, t) * TIP, 2) };
    }
    // the console uploads every 65 s; none in a lost hour
    function lastUp(t) {
      let u = U0 + Math.floor((t - U0) / STEP) * STEP;
      if (!has(ix(hf(u)))) { const i = hourOf(u); u = U0 + Math.floor((H0 + i * HR + 3570 - U0) / STEP) * STEP; }
      return u;
    }
    const FIELDS = ['temp_c', 'dewpt_c', 'humidity', 'pressure_hpa', 'wind_kmh', 'gust_kmh', 'wind_dir', 'rain_mm', 'rain_daily_mm', 'uv', 'solar_wm2'];
    function recent(t) {
      const u = lastUp(t), rows = [];
      let prev = frame(u - 60 * STEP).rain_daily_mm;
      for (let k = 59; k >= 0; k--) {
        const ti = u - k * STEP, f = frame(ti), d = f.rain_daily_mm;
        rows.push([ti, f.temp_c, f.dewpt_c, f.humidity, f.pressure_hpa, f.wind_kmh, f.gust_kmh, f.wind_dir, r(d >= prev ? d - prev : d, 2), d, f.uv, f.solar_wm2]);
        prev = d;
      }
      return { station: I.station, fields: ['t', ...FIELDS], rows };
    }

    // ---- history answers, cut at t (closed records before t, the open ones at t)
    function openHour(t) {
      const h = hf(t), i = ix(h), x = hourRow(i);
      if (!x) return null;
      const rain = r(tipsIn(h - 1, t) * TIP, 2);
      return Object.assign(x, { n: Math.max(1, Math.round(x.n * (t - h) / HR)), rain_mm: rain, rain_rate_max: rain ? x.rain_rate_max : 0 });
    }
    function openDay(t) {
      const H = [];
      for (let h = midnight(t); h < hf(t); h += HR) { const x = hourRow(ix(h)); if (x) H.push(x); }
      const o = openHour(t); if (o) H.push(o);
      return H.length ? dayRow(dkey(t), H) : null;
    }
    function hourly(t, q) {
      const from = q.from != null ? +q.from : null, to = q.to != null ? +q.to : null, inR = h => (from == null || h >= from) && (to == null || h < to);
      let rows = [];
      for (let h = Math.max(H0, from == null ? hf(t) - 48 * HR : from); h < hf(t); h += HR) if (inR(h)) { const x = hourRow(ix(h)); if (x) rows.push(x); }
      if (from == null && to == null) rows = rows.slice(-48);
      return { station: I.station, open: inR(hf(t)) ? openHour(t) : null, hours: rows };
    }
    function daily(t, q) {
      const from = q.from != null ? +q.from : null, to = q.to != null ? +q.to : null, mid = d => midnight(sec(d + 'T12:00:00Z'));
      const inR = d => (from == null || mid(d) >= from) && (to == null || mid(d) < to), today = dkey(t);
      let D = DAILY.filter(x => x.d < today && inR(x.d));
      if (q.all == null && from == null && to == null) D = D.slice(-Math.min(q.days ? +q.days : 30, 736));
      return { station: I.station, open: inR(today) ? openDay(t) : null, days: D };
    }

    // ---- airport weather (two real airports, values derived from the station's series), the TAF and the WU line
    const PH = ['thunder', 'snow', 'rain', 'fog', 'gusts'];
    function phen(i) {
      if (!has(i)) return new Set();
      const s = new Set(), pr = C.pr[i] || 0;
      if (C.ts[i]) s.add('thunder');
      if (pr > 0.3) s.add(C.tav[i] <= 0.5 ? 'snow' : 'rain');
      if (C.rhav[i] >= 97 && C.w[i] < 5 && !pr) s.add('fog');
      if (C.g[i] >= 40) s.add('gusts');
      return s;
    }
    function taf(t) {
      const H = hf(t), P = [], out = [];
      for (let k = -2; k <= 14; k++) P[k + 2] = phen(ix(H + k * HR));
      PH.forEach(w => {
        const on = k => P[k + 2].has(w), lik = k => on(k) && (on(k - 1) || on(k + 1));
        const kind = k => lik(k) ? 'l' : on(k) || lik(k - 1) || lik(k + 1) ? 'p' : '';
        for (let k = 0; k < 12;) {
          const c = kind(k); if (!c) { k++; continue; }
          let e = k; while (e < 12 && kind(e) === c) e++;
          const x = { what: w, possible: c === 'p', from: iso(H + k * HR), to: iso(H + e * HR) };
          if (e === 12) x.cut = true;
          out.push(x); k = e;
        }
      });
      const issued = (() => { let x = hf(t - 2400); while (((x / HR) % 24 + 1) % 6) x -= HR; return x; })();
      return { status: 'ok', current: true, issued: iso(issued), valid_to: iso(issued + 30 * HR), to: iso(H + 12 * HR), next12: out };
    }
    function airport(a, t) {
      const obs = t - ((t % 1800) + 1800) % 1800, i = hourOf(obs), T = base('tav', obs) + a.dt, rh = base('rhav', obs);
      const td = Math.min(T, tdOf(base('tav', obs), Math.min(99, rh)) + a.dt), w = base('w', obs) || 0, kt = Math.round(w * 1.2 / 1.852), gk = Math.round(C.g[i] * 1.2 / 1.852);
      const pr = C.pr[i] || 0, prec = pr > 0.1, snow = prec && T <= 0.5, c = base('cloud', obs) / 100, fog = !prec && rh >= 99 && w < 4, mist = !prec && !fog && rh > 96;
      const cover = c < 0.1 ? null : c < 0.35 ? 'few' : c < 0.6 ? 'scattered' : c < 0.85 ? 'broken' : 'overcast';
      const vis = fog ? 400 : snow ? 2500 : prec && pr > 2.5 ? 5000 : mist ? 4000 : 9999;
      return { icao: a.icao, name: a.name, cc: a.cc, fp: a.fp, km: a.km, obs: iso(obs), age_s: t - obs, stale: false, cur: t - obs <= 4500, cor: false, auto: a.auto,
        qnh_hpa: Math.floor(base('p', obs) - rel(obs) - a.dp), wind_dir: kt === 0 ? null : kt <= 2 ? 'vrb' : (Math.round(baseDir(obs) / 10) % 36) * 10 || 360,
        wind_kmh: r(kt * 1.852, 1), gust_kmh: gk - kt >= 10 ? r(gk * 1.852, 1) : null, vis_m: vis, vis_ge: vis === 9999, cavok: !cover && vis === 9999,
        cover: cover || 'none', ceil_ft: cover === 'broken' || cover === 'overcast' ? (prec ? 800 : Math.round((1200 + 3000 * (1 - c)) / 100) * 100) : null,
        cb: !!C.ts[i], low_conf: false, wx: { precip: prec, rain: prec && !snow, drizzle: false, snow, freezing: false, thunder: !!C.ts[i], fog, mist,
          int: prec ? pr < 2.5 ? -1 : pr < 7.6 ? 0 : 1 : 0 }, t_c: Math.round(T), td_c: Math.round(td), taf: taf(t) };
    }
    function metar(t, sw, p) {
      if (sw.metar === false) return { on: false, airports: [], reference: null, last: { t: null, result: null }, last_taf: { t: null, result: null } };
      const A = I.airports.map(a => airport(a, t)), cur = A.filter(a => a.cur), w = [3, 2];
      const qnh = r(cur.reduce((s, a, k) => s + a.qnh_hpa * w[k], 0) / w.slice(0, cur.length).reduce((a, b) => a + b, 0), 1), L = cur[0];
      const ref = baroRef(t, sw);
      return { on: true, airports: A, reference: { qnh_hpa: qnh, qnh_from: cur.map(a => ({ icao: a.icao, km: a.km, age_s: a.age_s })), qnh_inconsistent: false,
        local: { icao: L.icao, km: L.km, age_s: L.age_s }, local_values: Object.assign({ from: L.icao }, ...['vis_m', 'vis_ge', 'cavok', 'cover', 'ceil_ft', 'cb', 'low_conf', 'wx', 't_c', 'td_c'].map(k => ({ [k]: L[k] }))),
        near_qnh_hpa: qnh, station: { p_hpa: p, qnh_hpa: qnh, diff_hpa: r(p - qnh, 1), target_hpa: qnh, active: !!ref } },
        last: { t: iso(t - ((t - 300) % 900 + 900) % 900), result: 'ok' }, last_taf: { t: iso(hf(t - 3000) + 3000), result: 'ok' } };
    }
    // the REL hint (baro_ref): the last three half-hourly reports all 2 hPa or more off, the same way
    function baroRef(t, sw) {
      if (sw.metar === false) return null;
      const obs = t - ((t % 1800) + 1800) % 1800, D = [0, 1, 2].map(k => { const o = obs - k * 1800, q = I.airports.map(a => Math.floor(base('p', o) - rel(o) - a.dp));
        return base('p', o) - (3 * q[0] + 2 * q[1]) / 5; });
      if (!D.every(d => Math.abs(d) >= 2 && Math.sign(d) === Math.sign(D[0]))) return null;
      return { d: r(D.slice().sort((a, b) => a - b)[1], 1), since: obs - 3600, ref_t: obs };
    }
    function wufc(t) {
      const l = lt(t), today = l.getUTCHours() < 3, a = today ? midnight(t) : midnight(midnight(t) + 30 * HR), b = midnight(a + 30 * HR), H = [];
      for (let h = a; h < b; h += HR) if (has(ix(h))) H.push(ix(h));
      const T = H.map(i => C.tav[i]), mm = H.reduce((s, i) => s + (C.pr[i] || 0), 0), pm = H.filter(i => lst(H0 + i * HR) >= 12).reduce((s, i) => s + (C.pr[i] || 0), 0);
      const day = H.filter(i => C.sol[i] > 0), cl = day.reduce((s, i) => s + C.cloud[i], 0) / Math.max(1, day.length) / 100;
      const phrase = Math.min(...T) < 1 && mm > 0.5 ? 'Snow Showers' : pm > 0.5 && pm > mm * 0.7 ? Math.max(...T) > 24 ? 'PM T-Storms' : 'PM Showers'
        : mm > 3 ? 'Rain' : mm > 0.5 ? 'Light Rain' : cl > 0.75 ? 'Cloudy' : cl > 0.4 ? 'Partly Cloudy' : 'Sunny';
      return { day: today ? 'today' : 'tomorrow', phrase, hi: Math.round(Math.max(...C.thi.filter((_, i) => H.includes(i)))), lo: Math.round(Math.min(...C.tlo.filter((_, i) => H.includes(i)))),
        pop: mm > 3 ? 80 : mm > 0.5 ? 60 : mm > 0 ? 30 : 10, t: iso(hf(t) + 600 - (((t % HR) + HR) % HR < 600 ? HR : 0)) };
    }

    // ---- the learners: the newest snapshot at or before t (index.json `learn`)
    function learn(t, sw) {
      const e = I.learn.filter(x => sec(x.t) <= t).pop() || I.learn[0], L = JSON.parse(JSON.stringify(F[e.file]));
      L.use.metar = sw.metar !== false;
      return L;
    }

    // ---- the local outlook (docs/api.md §3.2.2-3.2.3) from the hour records before u, the reading at u, the main
    // airport and the learners in use
    function outlook(u, v, gust10, near, use) {
      const hn = hf(u) - HR, R = {}, oh = openHour(u);
      for (let h = hn - 22 * HR; h <= hn; h += HR) { const x = hourRow(ix(h)); if (x) R[h] = x; }
      if (!R[hn] || !R[hn - 3 * HR]) return null;
      const mean = (h, k) => R[h] ? R[h][k][1] : null, t = v.temp_c, td = v.dewpt_c, rh = v.humidity, rate = v.rain_rate_mmh;
      const night = sun(u)[0] < 0, cool = use.l5 ? use.cool : 1.0, learnt = [];
      const rain12 = r(oh.rain_mm + Object.keys(R).filter(h => +h >= hn - 11 * HR).reduce((s, h) => s + R[h].rain_mm, 0), 2);
      const raw = mean(hn, 'pressure_hpa') - mean(hn - 3 * HR, 'pressure_hpa'), d0 = raw - (s2(hn + 1800) - s2(hn - 3 * HR + 1800));
      const d = Math.sign(d0) * Math.round(Math.abs(d0) * 10 + 1e-6) / 10 || 0, code = d >= 1.6 ? 'rising' : d <= -1.6 ? 'falling' : 'steady';
      const tend = { d, raw: r(raw, 2), tide: true, code, fast: code !== 'steady' && Math.abs(d) >= 3.6 };
      const dt = h => R[h] && R[h - HR] ? R[h].temp_c[1] - R[h - HR].temp_c[1] : 0, dtH = r(dt(hn), 1), windH = R[hn].wind_kmh[1], calm = R[hn].calm_n * 2 > R[hn].n;
      let state = null, coolAt = null;
      if (night && !rate && !oh.rain_mm && !R[hn].rain_mm) {
        const hs = Object.keys(R).map(Number).sort((a, b) => a - b), dusk = Math.max(hn - 22 * HR, ...hs.filter(h => sun(h + 1800)[0] >= 0));
        for (const h of hs) if (h > dusk && R[h - HR] && coolAt == null && dt(h) <= -cool && R[h].wind_kmh[1] < 5) coolAt = h;
        if (coolAt != null && hs.filter(h => h > coolAt).every(h => dt(h) < 0.5 && !R[h].rain_mm)) { state = 'night_clear'; if (use.l5) learnt.push('l5'); } else coolAt = null;
      }
      if (rate > 0) state = t != null && t <= 1.0 ? 'sleet_snow' : rate < 2.5 ? 'rain_light' : rate <= 7.6 ? 'rain_moderate' : 'rain_heavy';  // SLEET_T
      else if (oh.rain_mm || R[hn].rain_mm) state = 'recent_rain';
      else if (state == null && !night && use.l1 && sun(hn + 1800)[0] >= 10) {
        let c = 0; for (let k = 0; k < 6; k++) c += clearSky(sun(hn + k * 600 + 300)[0]) / 6;
        const f = R[hn].solar_wm2[1] / c; state = f >= 0.75 ? 'day_sunny' : f >= 0.35 ? 'day_spells' : 'day_nosun'; learnt.push('l1');
      }
      if (state == null && t > 2.0) state = 'dry';
      const w3 = near && near.wx, st3 = w3 && (w3.snow ? 'wx_snow_near' : w3.rain ? 'wx_rain_near' : w3.drizzle ? 'wx_drizzle_near' : null);
      let shown3 = false;
      if (st3 && [null, 'dry', 'day_nosun'].includes(state)) { if (state === 'day_nosun') learnt.splice(learnt.indexOf('l1'), 1); state = st3; shown3 = true; }
      const qual = shown3 ? null : t <= -10 ? 'severe_frost' : t <= 0 ? 'freezing' : t >= 30 && mean(hn, 'temp_c') >= 30 ? 'hot'
        : windH >= 20 && v.wind_avg10m_kmh >= 16 ? 'windy' : !night && !/^(rain|recent|sleet)/.test(state || '') && R[hn].humidity[1] >= 90 && rh >= 88 ? 'humid' : calm ? 'calm' : null;
      let next = code === 'steady' ? 'p_steady' : 'p_' + code + (tend.fast ? '_fast' : '');
      if (next === 'p_steady' && night && dtH <= -cool) { next = 'cooling_fast'; if (use.l5) learnt.push('l5'); }
      const p = { gust: null, since: null };
      if (state === 'night_clear' && use.l6 && use.min_lo != null && ['p_steady', 'cooling_fast'].includes(next)) {
        next = 'min_range'; p.min_lo = use.min_lo; p.min_hi = use.min_hi; learnt.push('l6');
      }
      if (/^(rain|sleet)/.test(state || '')) { let h = hn; while (R[h] && R[h].rain_mm > 0) h -= HR; p.since = iso(h + HR); }
      const clearNight = state === 'night_clear' && windH < 5;
      const fogP = use.l2 && R[hn].humidity[1] >= 0.98 * use.rh_sat && windH < 5 && !/^(rain|sleet)/.test(state || '') && rh >= 97 && ![1, 2].some(k => R[hn - k * HR] && R[hn - k * HR].rain_mm) && !R[hn].rain_mm;
      const n3 = k => !!(near && near.wx[k]);
      const chain = [['icy', t <= 1.0 && (rain12 >= 0.2 || rate > 0)], ['thunder_near', n3('thunder')], ['gusts', gust10 >= 30], ['hoar_frost', clearNight && t - 2 <= td],
        ['ground_frost', clearNight && t <= 2], ['fog_near', n3('fog')], ['fog_possible', fogP], ['mist_near', n3('mist')]];
      const warn = (chain.find(c => c[1]) || [null])[0];
      if (warn === 'gusts') p.gust = r(gust10, 1);
      if (warn === 'fog_possible') learnt.push('l2');
      const lr = [...new Set(learnt)].sort(), shownNear = shown3 || ['thunder_near', 'fog_near', 'mist_near'].includes(warn);
      const ins = { night, night_src: 'sun', t, td, rate: r(rate, 1), gust10: r(gust10, 1), wind_h: windH, calm, dt_h: dtH, rain12, rh, wind10: v.wind_avg10m_kmh,
        cool_at: coolAt != null ? iso(coolAt) : null, near: shownNear ? { icao: near.icao, name: near.name, km: near.km, age_s: near.age_s, auto: near.auto } : null };
      if (lr.length) { ins.learn = lr; if (lr.includes('l5')) ins.cool_thr = use.cool; if (lr.includes('l2')) Object.assign(ins, { rh_sat: use.rh_sat, rh_rel: r(100 * R[hn].humidity[1] / use.rh_sat, 1) }); }
      return { v: 1, t: iso(u), as_of: iso(hn + HR), gate: null, state, next, warn, qual, conf: lr.length ? 'learned' : 'default', tend, left_h: 0, p, in: ins };
    }

    // ---- /api/live at t: the newest upload (t - late with the console off), its values, the outlook, the sources
    const boot = t => U0 + Math.floor((t - U0) / (11 * 86400)) * 11 * 86400 + 5400;
    function station(t, sw) {
      const u = lastUp(t - (sw.late || 0)), rows = recent(u).rows, f = frame(u), rt = rows.slice(-10).filter(x => x[0] >= u - 600);
      const v = {}; FIELDS.forEach((k, j) => { v[k] = rows[59][j + 1]; });
      // no indoor values: the YT60234 console never uploads them (docs/station-profiles.md)
      Object.assign(v, { rain_mm: f.hour_mm,
        wind_avg2m_kmh: r((rows[58][5] + rows[59][5]) / 2, 1), rain_rate_mmh: f.rate, wind_avg10m_kmh: r(rt.reduce((s, x) => s + x[5], 0) / rt.length, 1) });
      const m = metar(u, sw, v.pressure_hpa), use = learn(u, sw).use, a0 = m.on && m.airports[0].cur && m.airports[0].km <= 60 ? m.airports[0] : null;
      const L = learn(u, sw), up = Math.floor((u - boot(u)) / STEP) + 1;
      const st = { id: I.station, name: I.name, configured: true, stored: true, publish: true, profile: 'yt60234', hidden: [],
        derived: { rain_mm: rows.filter(x => x[0] > u - HR).length, wind_avg2m_kmh: 2, wind_avg10m_kmh: rt.length }, sensor_stale: false, baro_uncal: false };
      const ref = baroRef(u, sw), notes = [];
      if (ref) notes.push({ id: 'baro_ref', since: iso(ref.since), d_hpa: ref.d, qnh_hpa: m.reference.near_qnh_hpa, target_hpa: m.reference.near_qnh_hpa, ref_t: iso(ref.ref_t),
        chip: 'Pressure looks off', text: `This station's pressure reads about ${Math.abs(ref.d).toFixed(1)} hPa ${ref.d < 0 ? 'below' : 'above'} nearby airports since ${hm(ref.since)}. Set REL in the console's SET menu to about ${m.reference.near_qnh_hpa.toFixed(1)} hPa.` });
      else if (!use.metar && L.l7.flag !== 'none' && L.l7.stage) {
        const o = Math.round(L.l7.off / 5) * 5;
        notes.push({ id: 'baro_check', off_hpa: o, days: L.l7.days, chip: 'Pressure looks off',
          text: `This station's pressure averaged about ${Math.abs(o)} hPa ${o < 0 ? 'below' : 'above'} the usual for this place over ${Math.min(60, L.l7.days)} days. Set REL in the console's SET menu to the current sea-level pressure of a nearby official station.` });
      }
      if (notes.length) st.notices = notes;
      const ol = outlook(u, v, Math.max(...rt.map(x => x[6])), a0, use);
      if (ol) st.outlook = ol;
      if (sw.wufc !== false) st.wufc = wufc(u);
      Object.assign(st, { online: t - u < 240, last_seen: iso(u), age_s: t - u, src_ip: '192.168.77.2', src_if: 'ap', uploads: up, restored: false, dateutc: iso(u - 2),
        values: v, age: Object.fromEntries(Object.keys(v).map(k => [k, 0])), last_rejected: [], rejects_since_boot: { wind_kmh: 1 },
        rej: { parse: 0, bounds: 0, profile_wind: 1, profile_humidity: 0, limits: 0, jump: 0, rain_glitch: 0 }, dropped: 0, promoted: {}, extras: {}, known: [], extras_dropped: 0, id_sanitized: false });
      return { st, u, m, up };
    }
    function head(t, up) {
      const d = Object.assign({}, F['static/diag.json'], { uploads: up, mqtt_published: up * 13, http_requests: up * 3, http_304: up * 2 });
      return { now: iso(t), time_synced: true, time_src: 'sntp', uptime_s: t - boot(t), diag: d };
    }
    function live(t, sw, all) {
      const s = station(t, sw), h = head(t, s.up);
      return Object.assign(h, all ? { stations: [s.st] } : { station: s.st });
    }
    function status(t, sw) {
      const s = station(t, sw), o = { mode: 'run', sta: 1, mqtt: 1, time: 1, lock: 0, heap_low: 0, time_src: 'sntp' };
      if (sw.metar !== false) o.metar = 1;
      return Object.assign(o, { ids: [I.station], st: s.st.online ? 'o' : 's' });
    }
    // live frames: the bodies' values at every upload of a window, one array per field (shared parts once: live())
    function frames(t, sw, n = 120) {
      const u1 = lastUp(t - (sw.late || 0)), cols = { t: [] };
      ['temp_c', 'dewpt_c', 'humidity', 'pressure_hpa', 'wind_kmh', 'gust_kmh', 'wind_dir', 'rain_daily_mm', 'rain_rate_mmh', 'uv', 'solar_wm2'].forEach(k => cols[k] = []);
      for (let k = -Math.floor(n / 2); k < n - Math.floor(n / 2); k++) {
        const u = u1 + k * STEP, f = frame(u);
        cols.t.push(u); Object.keys(cols).forEach(c => { if (c !== 't') cols[c].push(c === 'rain_rate_mmh' ? f.rate : f[c]); });
      }
      return { station: I.station, t0: iso(t), step_s: STEP, n, cols };
    }

    // ---- the admin GETs: the static files, with the parts that follow the time
    function admin(name, t, sw) {
      const file = I.static['/api/admin/' + name];
      if (name === 'station/console') {
        const rows = recent(t).rows.slice(-6), first = Math.floor((rows[0][0] - U0) / STEP);
        return { first, last: first + 5, demo: true, items: rows.map((x, k) => ({ seq: first + k, t: x[0],
          line: `GET /weatherstation/updateweatherstation.php?ID=***&PASSWORD=***&tempf=${(x[1] * 1.8 + 32).toFixed(1)}&humidity=${x[3]}&action=updateraw&realtime=1` })) };
      }
      if (!file) return null;
      const o = JSON.parse(JSON.stringify(F[file]));
      if (name === 'forwarding') {
        const w = sw.wufc !== false ? wufc(t) : null;
        o.targets.forEach((x, k) => { x.last.t = x.last_ok = iso(lastUp(t) - 65 * (2 + 3 * k)); });
        Object.assign(o.wufc, { on: !!w, state: w ? 'idle' : 'off', tomorrow: w, last: w && { t: w.t, result: 'ok', status: 200 }, next_t: w && iso(sec(w.t) + HR) });
      }
      if (name === 'metar') {
        o.on = sw.metar !== false; o.state = o.on ? 'idle' : 'off';
        o.last = o.on ? { t: iso(t - ((t - 300) % 900 + 900) % 900), kind: 'fetch', result: 'ok', status: 200, why: null, lines: 2, garbage: 0, unwanted: 0, nil: 0, old: 0, field_bad: 0, trunc: 0, dropped: 0 } : null;
      }
      if (name === 'station') { const u = learn(t, sw).use; o.learn_use = ['l1', 'l2', 'l5', 'l6'].filter(k => u[k]).length; }
      return o;
    }

    // ---- one request: path with its query, the demo's time t, the scenario's switches
    function answer(path, t, sw = {}) {
      const url = new URL(path, 'http://demo'), q = Object.fromEntries(url.searchParams), p = url.pathname;
      if (q.station && q.station !== I.station) return { status: 404, body: { error: 'no such station' } };
      const b = p === '/api/live' ? live(t, sw, !q.station) : p === '/api/bridge/status' ? status(t, sw) : p === '/api/recent' ? recent(t - (sw.late || 0))
        : p === '/api/hourly' ? hourly(t, q) : p === '/api/daily' ? daily(t, q) : p === '/api/metar' ? metar(t, sw, station(t, sw).st.values.pressure_hpa)
        : p === '/api/learn' ? learn(t, sw) : p.startsWith('/api/admin/') ? admin(p.slice(11), t, sw) : I.static[p] ? F[I.static[p]] : null;
      return b ? { status: 200, body: b } : { status: 200, text: 'success' };
    }
    return { answer, frame, frames, live, status, recent, hourly, daily, hourRow, dayRow, openHour, openDay, metar, taf, wufc, learn, outlook, lastUp,
      sun, midnight, dkey, hkey, iso, ix, has, H0, NH };
  }

  // node demo/replay.js --write [dir]: the snapshots of index.json's example days and scenarios
  if (typeof require === 'function' && typeof module === 'object' && require.main === module) {
    const fs = require('fs'), path = require('path'), dir = process.argv[3] || path.join(__dirname, 'fixtures');
    const read = f => JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8')), I = read('index.json'), F = { 'index.json': I };
    [I.series.file, I.daily.file, 'static/diag.json', ...I.learn.map(x => x.file), ...Object.values(I.static)].forEach(f => { F[f] = read(f); });
    const R = create(F), put = (f, o) => { fs.mkdirSync(path.dirname(path.join(dir, f)), { recursive: true }); fs.writeFileSync(path.join(dir, f), JSON.stringify(o) + '\n'); };
    const jobs = [];
    I.example_days.forEach(e => { jobs.push([e.day, e.day_t0, {}], [e.night, e.night_t0, {}]); });
    Object.values(I.scenarios).forEach(s => jobs.push([s, s.t0, s.switches]));
    if (process.argv[2] === '--write') jobs.forEach(([o, t, sw]) => { put(o.live, R.live(Date.parse(t) / 1e3, sw)); put(o.frames, R.frames(Date.parse(t) / 1e3, sw)); });
    console.log(`demo replay: ${jobs.length} snapshots ${process.argv[2] === '--write' ? 'written to' : 'checked in'} ${dir}`);
  }
  return { create };
});
