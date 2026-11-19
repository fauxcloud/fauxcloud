// tendency.js -- trend of an hourly mean over a fixed window: pure, no DOM, one global like
// crypto.js's FCX. Pressure: 3 h, ±1.6 hPa, the semi-diurnal tide S2 taken out, "fast" from 3.6 hPa (Met Office
// "quickly"); temperature: 1 h, ±0.5 °C. Input = the raw /api/hourly `hours` rows (metric, before any unit
// conversion); rows are matched by timestamp, never by index, so a missing hour gives no answer instead of a wrong one.
// The reference for the firmware's port (same vectors: test/tools/tendency_vectors.js).
const FCT = (() => {
  const HR = 36e5;
  // S2 (hPa): amplitude 1.16 cos³(lat), maxima at 10:00 and 22:00 local mean solar time (tools/forecast_trial.py)
  const s2 = (ms, lat, lon) => 1.16 * Math.cos(lat * Math.PI / 180) ** 3 * Math.cos(2 * Math.PI * (ms / HR + lon / 15 - 10) / 12);
  // rounded to 0.1 as the tiles show it, half away from zero, so the shown value never contradicts its word
  const r1 = d => Math.sign(d) * Math.round(Math.abs(d) * 10 + 1e-6) / 10;
  // o: { key, win (h), thr, fast (optional), loc ({ lat, lon } or null: no tide), now (ms, optional: freshness) }
  // -> { d, raw, tide, code: 'rising' | 'steady' | 'falling', fast } or null when either end hour is missing, or (the
  // freshness gate) when the newest closed hour starts more than 2 h before `now`: an old trend is no trend
  function trend(hours, o) {
    const by = new Map(), mean = x => Array.isArray(x[o.key]) ? x[o.key][1] : null;
    (hours || []).forEach(x => { if (!x.open && mean(x) != null) by.set(Date.parse(x.h), mean(x)); });
    if (!by.size) return null;
    const t0 = Math.max(...by.keys()), t1 = t0 - o.win * HR;
    if (!by.has(t1) || (o.now != null && t0 < o.now - 2 * HR)) return null;
    // half a location (lat without lon) counts as none: tide 0
    const raw = by.get(t0) - by.get(t1), loc = o.loc && o.loc.lat != null && o.loc.lon != null ? o.loc : null;
    const tide = loc ? s2(t0 + HR / 2, loc.lat, loc.lon) - s2(t1 + HR / 2, loc.lat, loc.lon) : 0;
    const d = r1(raw - tide), code = d >= o.thr ? 'rising' : d <= -o.thr ? 'falling' : 'steady';
    // "fast" only on a rising or falling trend, so a fast threshold below thr never makes a "steady, fast"
    return { d, raw, tide: !!loc, code, fast: !!o.fast && code !== 'steady' && Math.abs(d) >= o.fast };
  }
  const pressure = (hours, loc, now) => trend(hours, { key: 'pressure_hpa', win: 3, thr: 1.6, fast: 3.6, loc, now });
  const temp = (hours, now) => trend(hours, { key: 'temp_c', win: 1, thr: .5, now });
  // humidity over 3 h, ±5 %RH (the daily cycle and dew move it a few % without meaning much)
  const hum = (hours, now) => trend(hours, { key: 'humidity', win: 3, thr: 5, now });
  // the words for a result: whole phrases from one table, covered by the test vectors
  // "fast", the strip's word ("Pressure rising fast.", outlook.js)
  const WORDS = FC.w('tendency', { rising: 'rising', steady: 'steady', falling: 'falling', rising_fast: 'rising fast', falling_fast: 'falling fast' });
  const word = r => WORDS[r.code + (r.fast ? '_fast' : '')];
  return { trend, pressure, temp, hum, s2, word };
})();
