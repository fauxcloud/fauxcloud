// dash.js -- the status face.
// Data: live for the active station every 30 s, bridge status
// every 60 s; hourly + recent for the active station once at idle and again only when live changed (a new
// upload); daily only while a history sheet is open. One request in flight (core.js).
const HOUR = 36e5, DAY = 864e5;

// ------------------------------------------------------------------ data
const S = { status: null, live: null, liveAt: 0, recent: null, hourly: null, daily: null, loc: null, stale: false,
  cur: FC.store('fc-station') || '', names: {} };
try { S.names = JSON.parse(FC.store('fc-names') || '{}') || {}; } catch (e) { S.names = {}; }
// This tab showed the airport view last (applied once the bridge says the module is there)
try { S.apWant = sessionStorage.getItem('fc-view') === 'ap'; } catch (e) {}
const nameOf = id => S.names[id] || id;

// units: values are converted once when the model is built; labels and spans follow
let IMP = FC.imperial();
const CONV = { temp_c: 't', dewpt_c: 't', indoor_temp_c: 't', windchill_c: 't', heatindex_c: 't', wind_kmh: 'w', gust_kmh: 'w',
  wind_avg2m_kmh: 'w', gust_10m_kmh: 'w', rain_mm: 'r', rain_rate_mmh: 'r', rain_daily_mm: 'r', rain_weekly_mm: 'r', rain_monthly_mm: 'r', rain_yearly_mm: 'r', pressure_hpa: 'p', pressure_abs_hpa: 'p' };
const PROMO_CONV = { temperature: 't', soil_temperature: 't', pressure: 'p', wind_speed: 'w', rain: 'r' };
const U = {};
function setUnits() {
  IMP = FC.imperial();
  for (const k of ['t', 'w', 'r', 'p']) U[k] = IMP ? FC.UNITS[k].i : FC.UNITS[k].m;
  U.h = '%';  // the humidity trend's unit (no conversion)
}
setUnits();
const cvv = (c, x) => x == null || !IMP || !c ? x : Array.isArray(x) ? x.map(y => y == null ? y : FC.UNITS[c].f(y)) : FC.UNITS[c].f(x);
const cv = (k, x) => cvv(CONV[k], x);
const toKmh = v => IMP ? v * 1.609344 : v;

// ------------------------------------------------------------------ helpers
// the page language's decimal mark (lang.js, from Intl); English keeps the point. CSV never comes through here
const dsep = s => FC.L ? s.replace('.', FC.L.dp) : s;
const f1 = v => v == null || isNaN(v) ? '–' : dsep((Math.round(v * 10) / 10).toFixed(1)).replace('-', '−');
const f2 = v => v == null || isNaN(v) ? '–' : dsep((Math.round(v * 100) / 100).toFixed(2)).replace('-', '−');
const f0 = v => v == null || isNaN(v) ? '–' : String(Math.round(v)).replace('-', '−');
const fr = v => IMP ? f2(v) : f1(v), fp = v => IMP ? f2(v) : f1(v);
// UV readings without a decimal when every value seen is whole (the console sends integers); averages keep one
const uvf = a => a.every(x => x == null || Number.isInteger(x)) ? f0 : f1;
const CARD = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
const card = d => d == null ? '–' : CARD[Math.round(((d % 360) + 360) % 360 / 22.5) % 16];
// the status face's words (whole templates with named slots; a count entry picks its form by n)
const WD = FC.w('dash', {
  off_why: 'The console sends nothing on batteries or during a power cut; readings resume on their own.',
  tr_in: 'in {n} h, {w}', beaufort: 'Beaufort {i}, {name}',
  bf0: 'Calm', bf1: 'Light air', bf2: 'Light breeze', bf3: 'Gentle breeze', bf4: 'Moderate breeze', bf5: 'Fresh breeze', bf6: 'Strong breeze',
  bf7: 'Near gale', bf8: 'Gale', bf9: 'Strong gale', bf10: 'Storm', bf11: 'Violent storm', bf12: 'Hurricane force',
  // the measures' names (tiles, sheets, charts)
  m_wind: 'Wind', m_rain: 'Rain', m_hum: 'Humidity', m_temp: 'Temperature', m_baro: 'Pressure', m_sun: 'UV and sun', m_indoor: 'Indoor',
  m_gust: 'Gust', m_dew: 'Dew point', m_uv: 'UV index', m_inh: 'Indoor humidity',
  // the same names inside a sentence (the tile's "Open {m} history"): their own words, never a lower-cased label
  lo_wind: 'wind', lo_rain: 'rain', lo_hum: 'humidity', lo_temp: 'temperature', lo_baro: 'pressure', lo_sun: 'uv and sun',
  lo_indoor: 'indoor', lo_gust: 'gust', lo_dew: 'dew point', lo_uv: 'uv index', lo_inh: 'indoor humidity',
  der_hours: 'estimated from the hour records since the restart', der_n: { other: 'computed from {n} readings' },
  // the wind dial: its unit line, its calm word, its accessible name (a sentence, then the outer ring's)
  d_from: '{u} from {d}', d_calm: 'Calm', dl_calm: '{name} {v} {u}, calm.', dl_nd: '{name} {v} {u}.',
  dl_from: '{name} {v} {u} from {d}, {deg} degrees.', dl_ring_min: 'Outer ring: directions over the last {m} min.',
  dl_ring_h: 'Outer ring: directions over the last hour.',
  // the tile charts' hover line
  sp_gust: '{t} · {v} {u} (gust {g})', t24_now: '{a}–now · {v}',
  // the chips over the page
  slot0: 'A console was turned away: no free station place', slot: 'Console {id} turned away',
  slot_one: 'This fauxcloud keeps one console. To switch, forget {id} in Settings.',
  slot_all: 'All station places are in use. To take it, forget a station in Settings.',
  sta: 'Home Wi-Fi not joined', mqtt: 'MQTT disconnected', too_long: 'Upload too long, ignored', stale: 'No answer from fauxcloud',
  st_off: '{name}: offline', st_bat: '{name}: battery low', st_note: '{name}: {chip}', st_show: '{t}. Show this station',
  ap: 'Airport weather',
  // no console yet
  e_none: 'No console paired yet.', e_net: 'fauxcloud’s Wi-Fi', e_head: 'Pair your weather console',
  e_s1: '<b>On the console</b>, running on its adapter, hold <b>BARO</b> for 3 seconds until <b>AP</b> shows on its screen.',
  e_s2: '<b>On a phone</b>, join the console’s Wi-Fi <b>WWS-…</b>, open <b>http://192.168.1.1</b> and choose {net} as its network.',
  e_pw_ap: 'The password is in fauxcloud’s Settings, from your home network.',
  e_pw: 'The password is in <a href="/admin">Settings</a>, under “fauxcloud Wi-Fi for your station”.',
  e_sub: 'Meanwhile the light on fauxcloud flashes briefly every 6 seconds: it is running and listening. Readings appear here after the first upload.',
  e_open: 'Open Settings',
  // the outlook strip
  warn: 'Warning:', why: 'Why this outlook', ol: 'Outlook', o_range: '{a} to {b} {u}',
  // the tiles
  rained: 'rained {a}–{b}', dry: 'dry so far', peak: 'Peak gust today', peak_at: 'at {t}', gust10: 'Gust, last 10 min',
  avg_gust_m: 'Average gust, last {m} min', avg_m: 'Average, last {m} min', avg_gust_h: 'Average gust, last hour', avg_h: 'Average, last hour',
  lg_gust: 'Gust', tile_open: '{aria}. Open {m} history',
  a_w: '{name} {v} {u}', a_w_calm: '{name} {v} {u}, calm', a_w_from: '{name} {v} {u} from {d}',
  a_w_g: '{name} {v} {u}, gust {g}', a_w_calm_g: '{name} {v} {u}, calm, gust {g}', a_w_from_g: '{name} {v} {u} from {d}, gust {g}',
  r_hour: 'Rain, last hour', r_today: 'Rain today', r_rate: 'rate <b>{v}</b> {u}/h', r_last: 'last hour <b>{v}</b> {u}',
  r_warm: 'Available about an hour after the first readings', r_last0: 'last hour <b>–</b>',
  a_t: '{l} {v} degrees', a_t_tr: '{l} {v} degrees, {tr}', t_today: 'today <b>{lo} / {hi}</b> {u}', t_dew: 'dew point <b>{v}</b> {u}',
  t_feels: 'feels like <b>{v}</b> {u}',
  a_h: 'Humidity {v} percent', a_h_tr: 'Humidity {v} percent, {tr}', a_h_day: 'Humidity {v} percent, today {lo} to {hi}',
  a_h_tr_day: 'Humidity {v} percent, {tr}, today {lo} to {hi}', h_today: 'today <b>{lo}–{hi}</b> %',
  a_p: 'Pressure {v} {u}', a_p_tr: 'Pressure {v} {u}, {tr}', a_warn: 'Warning: {chip}',
  a_uv: 'UV index {v}, highest in the last 10 minutes', a_uv_rej: 'UV index {v}, highest in the last 10 minutes, solar reading rejected',
  uv_title: 'Highest UV index in the last 10 minutes', solar_rej: 'Solar rejected', uv_solar: 'solar <b>{v}</b> W/m²', l_sun: 'Sun',
  a_sun_rej: 'Sun reading rejected', a_sun: 'Sun {v} watts per square metre',
  a_in: 'Indoor {v} degrees', a_in_h: 'Indoor {v} degrees, humidity {h} percent', in_hum: 'humidity <b>{v}</b> %',
  a_inh: 'Indoor humidity {v} percent',
  none: 'No readings yet', none_sub: 'The console\'s first upload fills this page.',
  health: 'Bridge health',
  // the sheets
  loading: 'Loading…', no_chart: 'Could not load the chart. Close the sheet and try again.', show_table: 'Show table', hide_table: 'Hide table',
  retrying: 'Could not load, retrying.', no_why: 'Could not load the explanation. Close and try again.',
  // the station's state line
  s_ota: 'Updating firmware… back in a minute', s_off_since: '{since}. {why}', s_off_ago: 'Offline. Last upload {ago}. {why}',
  s_on: 'Online. Updated {ago}', s_rest_from: 'Offline. No upload since the restart; showing the last reading from {t}',
  s_rest_flash: 'Offline. No upload since the restart; showing the last reading kept on flash',
  s_kept: 'Offline. No upload since the restart; showing the last hour kept on flash', s_wait: 'Waiting for the first upload',
  bat: 'Battery low',
});
// The firmware's notices (src/notices.cpp, by id). English shows the chip and text as the firmware sends them, so this
// table is empty here: a language pack brings it whole (namespace notice, in status.js's pack; its English lives in the
// pack source, equal to the C++ table key by key: test_notices_sync.py), the parameters as named slots in the C format's order
const NT = FC.w('notice', {});
const hh = t => p2(t.getHours()) + ':00';
// p2, hm, dm, dayTime, offSince: health.js (in both bundles, not in setup's inline core.js)
// Below this the wind has no direction worth naming; the firmware's calm threshold must equal it
const CALM_KMH = 1;
// calm = the live speed and its 2-min mean (when sent) both below CALM_KMH, compared in km/h
const calm = v => v.wind_kmh != null && toKmh(v.wind_kmh) < CALM_KMH && (v.wind_avg2m_kmh == null || toKmh(v.wind_avg2m_kmh) < CALM_KMH);
// Trend codes (tendency.js, which also holds the words) -> Lucide glyphs (ISC; notices: licenses.txt from THIRD_PARTY_LICENSES.md)
const TRG = {
  rising: '<path d="M16 7h6v6"/><path d="m22 7-8.5 8.5-5-5L2 17"/>', steady: '<path d="M5 12h14"/><path d="m12 5 7 7-7 7"/>',
  falling: '<path d="M16 17h6v-6"/><path d="m22 17-8.5-8.5-5 5L2 7"/>',
  rising_fast: '<path d="m17 11-5-5-5 5"/><path d="m17 18-5-5-5 5"/>', falling_fast: '<path d="m7 6 5 5 5-5"/><path d="m7 13 5 5 5-5"/>',
};
const trIc = r => `<svg class="tr" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${TRG[r.code + (r.fast ? '_fast' : '')]}</svg>`;
// The trend is a glyph after the headline value (dim when steady), its sentence in the glyph's
// title, the tile's aria-label and the sheet's "now" row. tr = [result, unit class, formatter, hours] from render().
// trW: the trend's words without the unit (the sheet's Now row puts the unit on the number); trL: with it (the tile)
const trV = ([r, c, f]) => `${r.d > 0 ? '+' : ''}${f(dlt(c, r.d))}`, trW = ([r, , , n]) => FC.t(WD.tr_in, { n, w: FCT.word(r) }), trL = t => `${U[t[1]]} ${trW(t)}`;
const trGl = t => t ? `<span class="tri${t[0].code === 'steady' ? ' st' : ''}" title="${trV(t)} ${trL(t)}">${trIc(t[0])}</span>` : '';
// a change, not a reading: converted by the unit's scale only (°F has no +32 for a difference)
const dlt = (c, d) => IMP && FC.UNITS[c] ? d * FC.UNITS[c].s : d;  // humidity has no unit conversion
const BEAU = [[CALM_KMH, 'bf0'],[6, 'bf1'], [12, 'bf2'], [20, 'bf3'], [29, 'bf4'], [39, 'bf5'], [50, 'bf6'], [62, 'bf7'], [75, 'bf8'], [89, 'bf9'], [103, 'bf10'], [118, 'bf11'], [1e9, 'bf12']];  // names: WD keys
const beaufort = v => { const k = toKmh(v), i = BEAU.findIndex(b => k < b[0]); return FC.t(WD.beaufort, { i, name: WD[BEAU[i][1]] }); };  // display case
const minOf = a => { const x = a.filter(v => v != null); return x.length ? Math.min(...x) : null; };
const maxOf = a => { const x = a.filter(v => v != null); return x.length ? Math.max(...x) : null; };
let M = {};
function measures() {
  const sc = (c, v) => IMP ? v * FC.UNITS[c].s : v;
  M = {
    wind: { name: WD.m_wind, lo: WD.lo_wind, c: 'var(--wind)', u: U.w, k: 'wind_kmh', span: sc('w', 20) }, rain: { name: WD.m_rain, lo: WD.lo_rain, c: 'var(--rain)', u: U.r, k: 'rain_mm', span: sc('r', 1), dec: IMP ? 2 : 1 },
    hum: { name: WD.m_hum, lo: WD.lo_hum, c: 'var(--hum)', u: '%', k: 'humidity', dec: 0, span: 20, max: 100 }, temp: { name: WD.m_temp, lo: WD.lo_temp, c: 'var(--temp)', u: U.t, k: 'temp_c', span: sc('t', 5) },
    baro: { name: WD.m_baro, lo: WD.lo_baro, c: 'var(--baro)', u: U.p, k: 'pressure_hpa', span: sc('p', 10), dec: IMP ? 2 : 1 }, sun: { name: WD.m_sun, lo: WD.lo_sun, c: 'var(--sunc)', u: 'W/m²', k: 'solar_wm2', dec: 0, span: 100 },
    // beyond the six validated hues, tiles and charts stay neutral: identity comes from the label, not a seventh colour
    indoor: { name: WD.m_indoor, lo: WD.lo_indoor, c: 'var(--ink-2)', u: U.t, k: 'indoor_temp_c', span: sc('t', 2) },
  };
}
// Tile order: wind, then temperature with indoor beside it, then water (rain with humidity beside it),
// then pressure, sun, environment and the rest: the same quantity side by side, the way you look for it
const ORDER = ['wind', 'temp', 'indoor', 'rain', 'hum', 'baro', 'sun', 'air', 'garden', 'other'];

// ------------------------------------------------------------------ station model from the API documents
// A hidden field is hidden everywhere on the page: tiles, sheets, tables, Details and the CSV. The
// server drops it from /api/live; history keeps what was recorded before it was hidden, so every history document
// goes through strip() here. History names that differ from the live ones: the hour's / day's rain comes from the
// daily counter, the wind rose from the direction.
const HIST_OF = { rain_daily_mm: ['rain_mm'], rain_mm: [], wind_dir: ['wind_dir', 'rose'] };
const hidSet = () => new Set((S.live && S.live.station && S.live.station.hidden) || []);
const histHidden = H => [...H].flatMap(k => HIST_OF[k] || [k]);
const strip = (o, ks) => { ks.forEach(k => delete o[k]); return o; };
// a tile with two fields: the first one not hidden is its headline (big value, range and chart)
const HEADS = { wind: ['wind_kmh', 'gust_kmh'], rain: ['rain_daily_mm', 'rain_mm'], temp: ['temp_c', 'dewpt_c'], sun: ['uv', 'solar_wm2'],
  indoor: ['indoor_temp_c', 'indoor_humidity'] };
// A value fauxcloud computed (not sent by the console), with the readings behind it in the title
// A rain hour estimated from the hour records after a restart says so ("sources"); derWhy is the
// sentence alone: a title on the tiles and Details, a small line in the sheets; no "derived" word is shown
const derWhy = (k, s = (S.live && S.live.station) || {}) => { const n = (s.derived || {})[k];
  return n == null ? '' : (s.sources || {})[k] === 'hours' ? WD.der_hours : FC.t(WD.der_n, { n }); };
const headOf = (id, H) => HEADS[id].find(k => !H.has(k)) || null;
// A two-field sheet switches between its fields; a field is offered only when not hidden and the history carries
// it (indoor fields are not history fields, so indoor has no switch)
const PAIR = { sun: ['uv', 'solar_wm2'], indoor: ['indoor_temp_c', 'indoor_humidity'] };
const swOf = id => { const H = hidSet(), rs = S.hourly && S.live && S.hourly.station === S.live.station.id ? S.hourly.hours : [];
  return (PAIR[id] || []).filter(f => !H.has(f) && rs.some(x => x[f] != null)); };
// Seconds since a carried value arrived (it was not in the latest upload); 0 = from the latest upload or unknown.
// The body's ages count back from the station's last upload, so they stay true between polls (304s).
function carried(k) {
  const L = S.live && S.live.station; if (!L) return 0;
  const a = own(L.age || {}, k) ?? (own(L.promoted || {}, k) || {}).age;
  return a ? a + (L.age_s || 0) + (Date.now() - S.liveAt) / 1e3 : 0;
}
function model() {
  measures();
  const L = S.live.station, v = {}, hid = hidSet(), HK = histHidden(hid);
  Object.entries(L.values || {}).forEach(([k, x]) => v[k] = cv(k, x));
  // the sheets follow the tile headline: a hidden main field hands its sheet to the next field; in a two-field
  // sheet the field the reader switched to wins. FM = the second fields' metas, used by both.
  const FM = { gust_kmh: { k: 'gust_kmh', name: WD.m_gust, lo: WD.lo_gust }, dewpt_c: { k: 'dewpt_c', name: WD.m_dew, lo: WD.lo_dew }, uv: { k: 'uv', name: WD.m_uv, lo: WD.lo_uv, u: 'UV', dec: 1, span: 2 },
    indoor_humidity: { k: 'indoor_humidity', name: WD.m_inh, lo: WD.lo_inh, u: '%', dec: 0, span: 20, max: 100 } };
  Object.keys(HEADS).forEach(id => { const f = id === measure && swOf(id).includes(alt) ? alt : headOf(id, hid); if (own(FM, f)) Object.assign(M[id], FM[f]); });
  if (hid.has('rain_daily_mm')) M.rain.nohist = true;  // the rain history is the daily counter's
  const promoted = [];
  Object.entries(L.promoted || {}).forEach(([k, p]) => {
    const c = PROMO_CONV[p.type], u = c ? U[c] : p.unit;
    v[k] = cvv(c, p.v);
    const sc = x => x != null && c && IMP ? x * FC.UNITS[c].s : x;
    promoted.push({ k, name: p.name || k, u, group: p.group, icon: p.icon, dec: c && IMP ? Math.max(p.dec, c === 't' ? 1 : 2) : p.dec, span: sc(p.span), max: c ? null : p.max });
  });
  promoted.forEach(p => M[p.k] = { name: p.name, c: 'var(--ink-2)', u: p.u, k: p.k, dec: p.dec, group: p.group, icon: p.icon, span: p.span, max: p.max, promoted: true });
  const conv = x => { const o = strip(Object.assign({}, x), HK); Object.keys(o).forEach(k => o[k] = cv(k, o[k])); o.rej = x.rej_fields || 0; return o; };
  const H = S.hourly && S.hourly.station === L.id ? S.hourly : null;
  // hour (h) or day (d) records of a history answer, converted, oldest first, the open one last; the sheet's date
  // ranges run their own answers through it too
  const recs = (D, d) => D ? (d ? D.days : D.hours).concat(D.open ? [D.open] : []).map((x, j, a) => Object.assign(conv(x), { t: new Date(d ? x.d + 'T00:00' : x.h), open: !!D.open && j === a.length - 1 })) : [];
  const hours = recs(H);
  const mid = new Date(); mid.setHours(0, 0, 0, 0);
  const R = S.recent && S.recent.station === L.id ? S.recent : null;
  // readings stamped before the clock synced (seconds since boot) are skipped; the rest in time order
  const recent = R ? R.rows.filter(r => r[0] >= 1e9).sort((a, b) => a[0] - b[0]).map(r => { const o = { t: new Date(r[0] * 1e3) }; R.fields.slice(1).forEach((k, j) => o[k] = cv(k, r[j + 1])); return strip(o, HK); }) : [];
  const days = recs(S.daily && S.daily.station === L.id ? S.daily : null, 1);
  // "last hour" by time, not by the ring's 60 samples (a 16-30 s console fills it in 16-30 min); hrMin =
  // the minutes the kept samples really span, so a short ring says so instead of claiming an hour
  const tn = recent.length ? +recent[recent.length - 1].t : 0, hr = recent.filter(x => x.t >= tn - HOUR), hrMin = hr.length > 1 ? Math.max(1, Math.round((tn - hr[0].t) / 6e4)) : 60;  // under two samples: the label stays "last hour"
  // after a restart, before the first upload: the tiles show the last hour kept on flash, dimmed as stale,
  // instead of "no readings" while the history is right there
  let fromFlash = !L.uploads && !!L.restored;  // the last reading itself came back from flash
  if (!L.uploads && !Object.keys(v).length && hours.length) {
    const last = hours[hours.length - 1];
    ['temp_c', 'dewpt_c', 'humidity', 'pressure_hpa', 'wind_kmh', 'gust_kmh', 'wind_dir', 'rain_mm', 'rain_daily_mm', 'uv', 'solar_wm2'].forEach(k => {
      const x = last[k]; if (x != null && !hid.has(k)) v[k] = Array.isArray(x) ? x[1] ?? x[2] : x; });  // uv: max only
    fromFlash = true;
  }
  return { live: L, v, hid, old: carried, fromFlash, raw: H ? H.hours : [], rej: (L.last_rejected || []).filter(k => !hid.has(k)), recent, promoted, today: hours.filter(x => x.t >= mid), yday: hours.filter(x => x.t < mid && x.t >= mid - DAY),
    days, recs, tn, hr, hrMin, name: L.name || L.id };
}

// ------------------------------------------------------------------ small marks
function dial(m, k = 'wind_kmh') {
  const fv = m.f || f1;  // an airport's dial rounds to whole units (m.f = f0)
  const C = 150, dir = m.v.wind_dir ?? 0, a = d => (d - 90) * Math.PI / 180, P = (d, r) => [C + Math.cos(a(d)) * r, C + Math.sin(a(d)) * r];
  // a calm reading (wind < 1 km/h, CALM_KMH as the history's calm rule) draws no arc: a parked vane is no direction
  const bins = new Array(16).fill(0); m.hr.forEach(x => { if (x.wind_dir != null && x.wind_kmh >= cv('wind_kmh', 1)) bins[Math.round(x.wind_dir / 22.5) % 16]++; });
  // a sector lights only from 2 readings and 5 % of the hour's non-calm ones, else grid grey: at a
  // light, wandering wind single strays do not light the whole ring
  const tot = bins.reduce((a, n) => a + n, 0), lit = n => n >= 2 && n >= tot * .05, mx = Math.max(...bins.map(n => lit(n) ? n : 0), 1);
  let g = '';
  for (let i = 0; i < 72; i++) { const big = i % 6 === 0, [x1, y1] = P(i * 5, big ? 131 : 135), [x2, y2] = P(i * 5, 142); g += `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}" stroke="var(--ink-3)" stroke-width="${big ? 1.6 : 1}" opacity="${big ? .9 : .45}"/>`; }
  bins.forEach((n, i) => {
    const d0 = i * 22.5 - 10, d1 = i * 22.5 + 10, [ax, ay] = P(d0, 124), [bx, by] = P(d1, 124);
    g += `<path d="M${ax.toFixed(1)} ${ay.toFixed(1)}A124 124 0 0 1 ${bx.toFixed(1)} ${by.toFixed(1)}" fill="none" stroke="${lit(n) ? 'var(--wind)' : 'var(--grid)'}" stroke-width="9" stroke-linecap="round" opacity="${lit(n) ? (.25 + .75 * n / mx).toFixed(2) : 1}"/>`;
  });
  [['N', 0], ['E', 90], ['S', 180], ['W', 270]].forEach(([t, d]) => { const [x, y] = P(d, 104); g += `<text x="${x.toFixed(1)}" y="${(y + 5).toFixed(1)}" text-anchor="middle" font-size="14" font-weight="600" fill="var(--ink-2)">${t}</text>`; });
  // the marker sits where the wind comes from and points at the centre; calm (C): dimmed, and no "from"
  const cm = k === 'wind_kmh' && calm(m.v);
  if (m.v.wind_dir != null) { const [tx, ty] = P(dir, 119), [lx, ly] = P(dir - 5, 137), [rx, ry] = P(dir + 5, 137);
    g += `<path d="M${tx.toFixed(1)} ${ty.toFixed(1)}L${lx.toFixed(1)} ${ly.toFixed(1)}L${rx.toFixed(1)} ${ry.toFixed(1)}Z" fill="var(--ink)" stroke="var(--ring)" stroke-width="2" stroke-linejoin="round"${cm ? ' opacity=".35"' : ''}/>`; }
  g += `<text x="150" y="160" text-anchor="middle" font-size="64" font-weight="300" letter-spacing="-2" fill="var(--ink)">${fv(m.v[k])}</text>`;
  // no direction (the airport view's variable wind) -> the unit alone and no degrees, never "from –"
  const nd = m.v.wind_dir == null;
  g += `<text x="150" y="186" text-anchor="middle" font-size="14" fill="var(--ink-2)">${cm || nd ? U.w : FC.t(WD.d_from, { u: U.w, d: card(m.v.wind_dir) })}</text>`;
  g += `<text x="150" y="208" text-anchor="middle" font-size="13" fill="var(--ink-3)">${cm ? WD.d_calm : nd ? '' : m.v.wind_dir + '°'}</text>`;
  const dv = { name: k === 'gust_kmh' ? WD.m_gust : WD.m_wind, v: fv(m.v[k]), u: U.w, d: card(m.v.wind_dir), deg: m.v.wind_dir };
  return `<svg class="dial" viewBox="0 0 300 300" role="img" aria-label="${FC.t(cm ? WD.dl_calm : nd ? WD.dl_nd : WD.dl_from, dv)} ${!m.hr.length ? '' : m.hrMin < 55 ? FC.t(WD.dl_ring_min, { m: m.hrMin }) : WD.dl_ring_h}">${g}</svg>`;
}
// Hover values on the tile charts: desktop only; each chart drawn by this render registers
// [start, span, at(t) -> its label or ''] and carries its index in data-i. A tap or click still opens the sheet.
const CH = [], chI = (t0, sp, at) => ` data-i="${CH.push([t0, sp, at]) - 1}"`;
// clock labels under a tile chart, placed by time: [left %, text, hidden on narrow tiles]; none within 4 % of an edge
const axl = L => `<span class="axisrow ax">${L.filter(([p]) => p >= 4 && p <= 96).map(([p, t, q]) => `<span${q ? ' class="q"' : ''} style="left:${p.toFixed(1)}%">${t}</span>`).join('')}</span>`;
// the wind tile's last hour on a time axis: labels at the quarter hours where they fall; a gap > 5 min breaks
// the line. k: the headline (gust when the speed is hidden); gust dotted only beside the speed
function windSpark(rec, k = 'wind_kmh') {
  rec = rec.filter(x => x[k] != null);
  if (rec.length < 2) return '<svg class="spark" aria-hidden="true"></svg>';
  const tn = +rec[rec.length - 1].t, t0 = tn - HOUR;
  rec = rec.filter(x => x.t >= t0);
  const gl = k === 'wind_kmh' && rec.some(x => x.gust_kmh != null);
  const W = 600, H = 56, mx = Math.max(...rec.map(x => (gl ? x.gust_kmh : null) ?? x[k]), 1) * 1.08, X = t => (t - t0) / HOUR * W, Y = v => H - 2 - (v ?? 0) / mx * (H - 6);
  const line = (s, k) => s.map(x => X(+x.t).toFixed(1) + ',' + Y(x[k]).toFixed(1)).join(' ');
  const segs = [];
  rec.forEach((x, i) => { if (!i || x.t - rec[i - 1].t > 3e5) segs.push([]); segs[segs.length - 1].push(x); });
  let g = '';
  segs.forEach(s => { g += `<polygon points="${X(+s[0].t).toFixed(1)},${H} ${line(s, k)} ${X(+s[s.length - 1].t).toFixed(1)},${H}" fill="var(--wind)" opacity=".16"/>`
    + (gl ? `<polyline points="${line(s, 'gust_kmh')}" fill="none" stroke="var(--ink-3)" stroke-width="1.5" stroke-dasharray="2 4" vector-effect="non-scaling-stroke"/>` : '')
    + `<polyline points="${line(s, k)}" fill="none" stroke="var(--wind)" stroke-width="2" stroke-linejoin="round" vector-effect="non-scaling-stroke"/>`; });
  const lab = [], d = new Date(t0);
  d.setSeconds(0, 0); d.setMinutes(Math.ceil(d.getMinutes() / 15) * 15);
  for (let t = +d; t <= tn; t += 9e5) lab.push([X(t) / W * 100, hm(new Date(t))]);
  // the nearest reading within 5 min
  const at = t => { const x = rec.reduce((a, y) => Math.abs(y.t - t) < Math.abs(a.t - t) ? y : a); return Math.abs(x.t - t) > 3e5 ? ''
    : gl && x.gust_kmh != null ? FC.t(WD.sp_gust, { t: hm(x.t), v: f1(x[k]), u: U.w, g: f1(x.gust_kmh) }) : `${hm(x.t)} · ${f1(x[k])} ${U.w}`; };
  return `<svg class="spark" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" aria-hidden="true"${chI(t0, HOUR, at)}>${g}</svg>${axl(lab)}`;
}
// Every tile chart is the last 24 h on one clock axis. Hours sit at their time (mid-hour; the open
// hour spans h..now and is drawn dimmed), never in getHours() slots, so a DST day or a missing hour falls right.
// Labels at the local clock hours 00/06/12/18 where they fall, a faint line at midnight. kind: 'bars' (rain, sun),
// 'area' (temperature, humidity: area + line) or 'line' (pressure, indoor); the end dot is "now".
function t24(m, f, kind, c, H, fmt) {
  const W = 240, now = Date.now(), t0 = now - DAY, X = t => (t - t0) / DAY * W, d = new Date(t0), lab = [];
  let g = '';
  d.setMinutes(0, 0, 0);
  for (let t = +d + HOUR; t <= now; t += HOUR) {
    const x = new Date(t), h = x.getHours();
    if (h % 6 || x.getMinutes()) continue;
    lab.push([X(t) / W * 100, p2(h), h % 12 > 0]);
    if (!h) g += `<line x1="${X(t).toFixed(1)}" x2="${X(t).toFixed(1)}" y1="0" y2="${H}" stroke="var(--grid)" vector-effect="non-scaling-stroke"/>`;
  }
  const rs = m.yday.concat(m.today).map(x => ({ a: +x.t, b: x.open ? now : +x.t + HOUR, v: f(x), o: x.open })).filter(r => r.b > t0 && r.v != null);
  let yOf;  // a row's point height in chart units
  if (rs.length && kind === 'bars') {
    const mx = Math.max(...rs.map(r => r.v), .1);
    yOf = r => H - Math.max(3, r.v / mx * (H - 2));
    rs.forEach(r => { const x0 = X(Math.max(r.a, t0)), w = X(r.b) - x0, h = Math.max(3, r.v / mx * (H - 2));
      g += !r.v ? `<rect x="${(x0 + w / 2 - 1.5).toFixed(1)}" y="${H - 3}" width="3" height="3" rx="1.5" fill="var(--grid)"/>`
        : `<rect x="${(x0 + 1).toFixed(1)}" y="${(H - h).toFixed(1)}" width="${Math.max(1, w - 2).toFixed(1)}" height="${h.toFixed(1)}" rx="2" fill="${c}"${r.o ? ' opacity=".55"' : ''}/>`; });
  } else if (rs.length) {
    const lo = Math.min(...rs.map(r => r.v)), sp = Math.max(...rs.map(r => r.v)) - lo || 1, pad = kind === 'area' ? 10 : 8;
    const P = r => [X((r.a + r.b) / 2).toFixed(1), (H - 4 - (r.v - lo) / sp * (H - pad)).toFixed(1)], segs = [];
    yOf = r => +P(r)[1];
    // a missing hour breaks the line; the piece to the open hour is dimmed
    rs.forEach((r, i) => { if (!i || r.a > rs[i - 1].b) segs.push([]); segs[segs.length - 1].push(r); });
    segs.forEach(s => { const pts = s.map(P), cl = s[s.length - 1].o ? pts.slice(0, -1) : pts, pl = (p, o = '') => `<polyline points="${p.join(' ')}" fill="none" stroke="${c}" stroke-width="2" stroke-linejoin="round"${o} vector-effect="non-scaling-stroke"/>`;
      if (kind === 'area') g += `<polygon points="${pts[0][0]},${H} ${pts.join(' ')} ${pts[pts.length - 1][0]},${H}" fill="${c}" opacity=".16"/>`;
      if (cl.length > 1) g += pl(cl);
      if (cl.length < pts.length && pts.length > 1) g += pl(pts.slice(-2), ' opacity=".55"');
      if (pts.length === 1) g += `<circle cx="${pts[0][0]}" cy="${pts[0][1]}" r="2" fill="${c}"/>`; });
    const e = P(rs[rs.length - 1]);
    g += `<circle cx="${e[0]}" cy="${e[1]}" r="3.5" fill="${c}"/>`;
  }
  // the window's highest and lowest value at its point, so the range reads without a scale; bars (rain,
  // sun) carry the highest only. HTML over the chart: the svg is stretched (preserveAspectRatio). Decluttered
  // a number only (the unit is in the tile), none at the "now" end (the headline shows it), and
  // the lowest only when it does not crowd the highest; the label lane above the chart is .chw's margin.
  let ex = '';
  if (rs.length) {
    const hi = rs.reduce((a, r) => r.v > a.v ? r : a), lo = rs.reduce((a, r) => r.v < a.v ? r : a), bar = kind === 'bars';
    const xp = r => X((Math.max(r.a, t0) + r.b) / 2) / W * 100, endX = xp(rs[rs.length - 1]), num = v => fmt(v).split(' ')[0];
    // near an edge the label grows inwards (right-aligned near the "now" end), so it never sits on the end dot
    const mark = r => endX - xp(r) < 8 ? '' : `<span class="ext${xp(r) > 80 ? ' r' : xp(r) < 20 ? ' l' : ''}" style="left:${xp(r).toFixed(1)}%;top:${(yOf(r) / H * 100).toFixed(1)}%">${num(r.v)}</span>`;
    if (hi.v > 0 || !bar) ex += mark(hi);
    if (!bar && lo.v < hi.v && num(lo.v) !== num(hi.v) && Math.abs(xp(lo) - xp(hi)) >= 12) ex += mark(lo);
  }
  // the hour under the pointer: "14:00–15:00 · 6.4 °C", the open hour "22:00–now"
  const at = t => { const r = rs.find(r => t >= r.a && t < r.b); return !r ? '' : r.o ? FC.t(WD.t24_now, { a: hh(new Date(r.a)), v: fmt(r.v) }) : `${hh(new Date(r.a))}–${hh(new Date(r.b))} · ${fmt(r.v)}`; };
  return `<span class="chw"><svg class="bars" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" aria-hidden="true"${chI(t0, DAY, at)}>${g}</svg>${ex}</span>${axl(lab)}`;
}
// tile icons: Lucide (ISC; notices: licenses.txt from THIRD_PARTY_LICENSES.md), inner elements copied from the Lucide SVGs and drawn in the
// measure hue so the icon ties the tile to its chart colour. Apple's SF Symbols may not be used off Apple platforms.
const IC = {
  wind: LUCIDE.wind,
  temp: LUCIDE.thermometer,
  rain: LUCIDE['cloud-rain'],
  hum: LUCIDE.droplet,
  baro: LUCIDE.gauge,
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/><path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="m19.07 4.93-1.41 1.41"/>', // sun
  indoor: '<path d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8"/><path d="M3 10a2 2 0 0 1 .709-1.528l7-6a2 2 0 0 1 2.582 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>', // house
  air: '<path d="m5.2 6.2 1.4 1.4"/><path d="M2 13h2"/><path d="M20 13h2"/><path d="m17.4 7.6 1.4-1.4"/><path d="M22 17H2"/><path d="M22 21H2"/><path d="M16 13a4 4 0 0 0-8 0"/><path d="M12 5V2.5"/>', // haze
  garden: '<path d="M14 9.536V7a4 4 0 0 1 4-4h1.5a.5.5 0 0 1 .5.5V5a4 4 0 0 1-4 4 4 4 0 0 0-4 4c0 2 1 3 1 5a5 5 0 0 1-1 3"/><path d="M4 9a5 5 0 0 1 8 4 5 5 0 0 1-8-4"/><path d="M5 21h14"/>', // sprout
  other: '<path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"/>', // activity
  plane: '<path d="M17.8 19.2 16 11l3.5-3.5C21 6 21.5 4 21 3c-1-.5-3 0-4.5 1.5L13 8 4.8 6.2c-.5-.1-.9.1-1.1.5l-.3.5c-.2.5-.1 1 .3 1.3L9 12l-2 3H4l-1 1 3 2 2 3 1-1v-3l3-2 3.5 5.3c.3.4.8.5 1.3.3l.5-.2c.4-.3.6-.7.5-1.2z"/>', // plane (3d(i) tab)
  leaf: '<path d="M11 20A7 7 0 0 1 9.8 6.1C15.5 5 17 4.48 19 2c1 2 2 4.18 2 8 0 5.5-4.78 10-10 10Z"/><path d="M2 21c0-3 1.85-5.36 5.08-6C9.5 14.52 12 13 13 12"/>', // leaf
  battery: '<rect width="16" height="10" x="2" y="7" rx="2" ry="2"/><line x1="22" x2="22" y1="11" y2="13"/>', // battery
};
// promotion icons by Lucide name (firmware type table) -> the keys above
const ICN = { wind: 'wind', thermometer: 'temp', 'cloud-rain': 'rain', droplet: 'hum', gauge: 'baro', sun: 'sun', haze: 'air', sprout: 'garden', activity: 'other', leaf: 'leaf', battery: 'battery' };
const icon = key => { const m = own(M, key) || {}; return `<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${own(IC, key) || own(IC, own(ICN, m.icon)) || own(IC, m.group) || IC.other}</svg>`; };
const rejChip = t => `<span class="rej">${t}</span>`;
// feels like: wind chill or heat index from the console, shown only when it differs from the air temperature
const feels = v => [v.windchill_c, v.heatindex_c].find(x => x != null && v.temp_c != null && Math.abs(x - v.temp_c) >= .5) ?? null;
// stretch the last tile of a part-filled last row to the row end, so no hole is left whatever the tile count
function fillRow() {
  const g = $("#grid"), ts = [...g.children];
  ts.forEach(t => t.style.gridColumn = "");
  const last = ts[ts.length - 1];
  if (!last || last.classList.contains("hero")) return;
  const gb = g.getBoundingClientRect(), lb = last.getBoundingClientRect();
  if (gb.right - lb.right < 8) return;
  const cols = getComputedStyle(g).gridTemplateColumns.split(" ").length, gap = parseFloat(getComputedStyle(g).columnGap);
  const start = Math.round((lb.left - gb.left) / ((gb.width + gap) / cols)) + 1;
  last.style.gridColumn = start + " / -1";
}
// Each tile notice sits over its tile's spacer (the label's width varies with the font)
function placeNotes() {
  document.querySelectorAll('#grid .tw').forEach(w => { const s = w.querySelector('.nsp'), b = w.querySelector('.tnb'); if (!s || !b) return;
    const r = w.getBoundingClientRect(), q = s.getBoundingClientRect();
    b.style.left = q.left - r.left - 2 + 'px'; b.style.top = q.top - r.top + q.height / 2 - 14 + 'px'; });
}

// ------------------------------------------------------------------ sky
// Rain wins (rain in the last hour); otherwise day or night from the solar sensor (> 5 W/m²), and only when that is
// missing or rejected (the YT60234 quirk) from sunrise/sunset at the bridge's location (computed here), else a
// rough month table for ~50° N.
// NOAA sunrise equation, hours UTC (null = polar day/night)
function sunTimes(date, lat, lon) {
  // the day by local mean solar time (a UTC day is wrong far from 0° E), = src/sun.cpp
  const rad = Math.PI / 180, n = Math.floor(date.getTime() / DAY + lon / 360) - 10957 + .0008, J = n - lon / 360;
  const Mn = (357.5291 + .98560028 * J) % 360, Cc = 1.9148 * Math.sin(Mn * rad) + .02 * Math.sin(2 * Mn * rad) + .0003 * Math.sin(3 * Mn * rad);
  const lam = (Mn + Cc + 180 + 102.9372) % 360, Jt = J + .0053 * Math.sin(Mn * rad) - .0069 * Math.sin(2 * lam * rad);
  const dec = Math.asin(Math.sin(lam * rad) * Math.sin(23.44 * rad)), cw = (Math.sin(-.833 * rad) - Math.sin(lat * rad) * Math.sin(dec)) / (Math.cos(lat * rad) * Math.cos(dec));
  if (cw < -1) return 'day'; if (cw > 1) return 'night';
  const w = Math.acos(cw) / rad / 360, toMs = j => (j + 10957 - .0008 + .5) * DAY;
  return [toMs(Jt - w), toMs(Jt + w)];
}
function skyState(m) {
  const v = m.v;
  if ((v.rain_rate_mmh ?? v.rain_mm) > 0) return 'rain';  // a rate console's rain now, else the last hour
  if (v.solar_wm2 != null && !m.rej.includes('solar_wm2')) return v.solar_wm2 > 5 ? 'day' : 'night';
  const t = new Date(Date.now() - (m.live.age_s || 0) * 1e3);
  if (S.loc && S.loc.lat != null) {
    const st = sunTimes(t, S.loc.lat, S.loc.lon);
    if (typeof st === 'string') return st;
    return t >= st[0] && t < st[1] ? 'day' : 'night';
  }
  locNeed();
  return clockSky(t);
}
// once per page load (~300 B): a location change in admin shows on the next load (no stored copy).
// Asked by the sky (no solar sensor) and by the pressure tile (the tide correction, T): the same one request.
const locNeed = () => { if (S.loc === null) { S.loc = false; loadLoc(); } };
function loadLoc() {
  FC.get('/api/bridge').then(r => { const e = r.data.effective || {}; if (e.lat != null) { S.loc = { lat: e.lat, lon: e.lon }; render(); } },
    () => { S.loc = null; });  // asked again on the next render, not given up
}

// ------------------------------------------------------------------ page
const ap = () => FC.face === 'ap';
$('#foot').insertAdjacentHTML('beforeend', repoLink());  // the GitHub link, likewise
// The console turned away for lack of a slot, by its id (the last hour's); the tooltip names the one to forget
// (no station loaded -> the plain chip; the dashboard has no cap, one station means the one place)
const noSlotChip = st => !st.no_slot_id || !st.ids.length ? WD.slot0
  : [FC.t(WD.slot, { id: st.no_slot_id }), st.ids.length === 1 ? FC.t(WD.slot_one, { id: st.ids[0] })
    : WD.slot_all];
// a notice in the page's language (NT); English, or an id or a parameter this page does not know: as the firmware sent it
function ntx(n) {
  const c = own(NT, 'nc_' + n.id), t = own(NT, 'nt_' + n.id), hpa = v => dsep(Math.abs(v).toFixed(1)) + ' hPa';
  if (!FC.L || !c || !t) return n;
  const o = { since: dayTime(n.since, new Date()) || '--:--', n: n.n }, a = Math.abs(n.skew_s);  // "yesterday 14:05" when not today
  if (n.id === 'console_clock') o.amt = a >= 172800 ? FC.t(NT.n_days, { n: Math.floor(a / 86400) }) : FC.t(a >= 5400 ? NT.n_h : NT.n_min, { n: a >= 5400 ? Math.floor((a + 1800) / 3600) : Math.floor((a + 30) / 60) });
  if (n.id === 'baro_ref') { if (n.d_hpa == null) return n; Object.assign(o, { d: hpa(n.d_hpa), dir: n.d_hpa < 0 ? NT.n_below : NT.n_above, target: n.target_hpa != null && hpa(n.target_hpa) }); }
  if (n.id === 'baro_check') { if (n.off_hpa == null) return n; Object.assign(o, { d: Math.abs(n.off_hpa) + ' hPa', dir: n.off_hpa < 0 ? NT.n_below : NT.n_above, days: Math.min(n.days, 60) }); }
  return Object.assign({}, n, { chip: c, text: FC.t(n.id === 'baro_ref' && n.target_hpa == null ? NT.nt_baro_ref_old : t, o) });
}
// Notices that belong to a tile show there, not as chips (while that tile exists); one at a time, by
// the order of NOTICE_TILE: baro_uncalibrated > baro_ref > baro_check (the firmware sends one of them at most)
const NOTICE_TILE = { baro_uncalibrated: 'baro', baro_ref: 'baro', baro_check: 'baro' }, TILE_HAS = { baro: 'pressure_hpa' };
const tileNote = (k, l = S.live && S.live.station) => { if (!l || !l.values || l.values[TILE_HAS[k]] == null) return null;
  const ns = (l.notices || []).filter(n => own(NOTICE_TILE, n.id) === k), o = Object.keys(NOTICE_TILE);
  return ns.sort((a, b) => o.indexOf(a.id) - o.indexOf(b.id))[0] || null; };
function renderBar() {
  const st = S.status;
  // clock, memory and history texts come from the bridge's notices (one table with Home Assistant, notices.cpp)
  // a notice is a short chip with its sentence as the tooltip
  const issues = st ? [!st.sta && !ap() && WD.sta, ...(st.notices || []).map(ntx).map(n => [n.chip, n.text]), st.mqtt === 0 && WD.mqtt,
    st.too_long && WD.too_long, st.no_slot && noSlotChip(st)].filter(Boolean) : [];
  if (S.stale) issues.unshift(WD.stale);  // the page lost fauxcloud, not the sensor
  // station problems from every station, not only the one on screen: a tap jumps to that station
  const ids = st ? st.ids : [], stt = st ? st.st : '';
  const bat = S.live && S.live.station.values && S.live.station.values.battery_low, notes = (S.live && S.live.station.notices) || [];
  const stIssues = ids.flatMap((id, i) => [stt[i] === 's' && [id, FC.t(WD.st_off, { name: nameOf(id) }), WD.off_why, 'chip'], id === S.cur && bat && [id, FC.t(WD.st_bat, { name: nameOf(id) })],
    ...(id === S.cur ? notes.filter(n => !(own(NOTICE_TILE, n.id) && tileNote(NOTICE_TILE[n.id]))).map(ntx).map(n => [id, FC.t(WD.st_note, { name: nameOf(id), chip: n.chip }), n.text]) : [])].filter(Boolean));  // no re-casing
  $('#bridge').innerHTML = issues.map(t => { const [c, d] = Array.isArray(t) ? t : [t]; return `<span class="rej"${d ? ` title="${esc(d)}"` : ''}>${esc(c)}</span>`; }).join('')
    + stIssues.map(([id, t, d, cls]) => `<button type="button" class="${cls || 'rej'}" data-id="${esc(id)}"${d ? ` title="${esc(d)}"` : ''} aria-label="${esc(FC.t(WD.st_show, { t: (d || t).replace(/\.$/, '') }))}">${esc(t)}</button>`).join('');
  // the chips bar shows only with more than one station, or with the airport tab:
  // the bridge's `metar` (module on, an airport chosen) adds an icon-only plane after the chips
  const el = $('#stations'), apOn = !!(st && st.metar);
  if (!apOn && S.ap) apView(false);
  if (apOn && S.apWant) { S.apWant = false; apView(true); }
  el.hidden = ids.length < 2 && !apOn;
  el.innerHTML = ids.map((id, i) => `<button type="button" role="tab" aria-selected="${id === S.cur && !S.ap}" data-id="${esc(id)}"><span class="dot ${stt[i] === 's' || stt[i] === 'x' ? 'off' : id === S.cur && bat ? 'bad' : ''}"></span>${esc(nameOf(id))}</button>`).join('')
    + (apOn ? `<button type="button" role="tab" class="apb" id="apTab" aria-selected="${!!S.ap}" aria-label="${WD.ap}" title="${WD.ap}">${icon('plane')}</button>` : '');
  $('#unitBtn').textContent = IMP ? '°F' : '°C';
  $('#admin').hidden = FC.face !== 'lan';  // only on a positive LAN signal
}
// no station yet (first boot, before the console pairs): say so instead of "Loading…"
// zero stations: how to pair, in two steps from the console's manual; the sky follows the clock
function renderEmpty() {
  document.title = 'fauxcloud';
  setSky(clockSky());
  $('#stName').textContent = 'fauxcloud';
  $('#stState').innerHTML = `<span class="dot off"></span><span>${WD.e_none}</span>`;
  const net = S.status && S.status.ap ? `<b>${esc(S.status.ap)}</b>` : WD.e_net;
  $('#grid').innerHTML = `<section class="glass empty" aria-label="${WD.e_head}">
    <h2>${WD.e_head}</h2>
    <ol class="steps">
      <li><span>${WD.e_s1}</span></li>
      <li><span>${FC.t(WD.e_s2, { net })}
        ${ap() ? WD.e_pw_ap : WD.e_pw}</span></li>
      <li><span>${WC.wu_step}</span></li>
    </ol>
    <p class="sub">${WD.e_sub}</p>
    ${ap() ? '' : `<a class="admin go" href="/admin">${WD.e_open}</a>`}
  </section>`;
  if (S.head) renderHealth(S.head); else $('#health').innerHTML = '';  // the bridge part, from /api/live
  lgAll();
}
// the strip's page 1 (glyph, line 1, line 2, the warning) and its i; dash-wufc.js's two-page card reuses both
const olIn = ol => `${ol.glyph}<div class="olt">${ol.l1 ? `<b>${esc(ol.l1)}</b>` : ''}${ol.l2 ? `<span>${esc(ol.l2)}</span>` : ''}${ol.warn ? `<span class="olw">${ol.wglyph}<span class="sr">${WD.warn} </span>${esc(ol.warn)}</span>` : ''}</div>`;
const OLI = () => `<button type="button" class="oli" id="olWhy" aria-label="${WD.why}">${FCO.svg('info')}</button>`;  // drawn with the page's words
// the strip's shown glyph + words (on the card: the shown page's, marked aria-hidden=false), or null without a strip
const olNow = () => { const t = $('#grid').querySelector('.ol>.olt,[aria-hidden=false]>.olt'); return t && t.previousElementSibling.outerHTML + t.outerHTML; };
// a poll that changes the shown words (a: before the grid render, b: after) fades the new ones in: .fd for one style pass, then
// the CSS transition back to opacity 1 (app.css); the same words or a first strip, no class. A page turn fades by its aria-hidden
function olFade(a, b, o) { if (a && b && a !== b) o.classList.add('fd'), void o.offsetWidth, o.classList.remove('fd'); }
function render() {
  if (!S.live) { if (S.status && !S.status.ids.length) renderEmpty(); return; }
  const m = model(), v = m.v, L = m.live, today = m.today, stale = !L.online || S.stale;
  document.title = m.name + ' · fauxcloud';
  setSky(skyState(m), L.values, stale);  // the raw metric values (m.v may be imperial); no streaks when stale
  $('#stName').textContent = m.name;
  stateLine();
  const st = stale ? ' data-stale' : '';
  const H = m.hid, hd = id => { const k = headOf(id, H); return k && (v[k] != null || m.rej.includes(k)) ? k : null; };
  const wk = hd('wind'), gustOn = wk === 'wind_kmh' && !H.has('gust_kmh');  // gust: headline, or a sub-value beside the speed
  const gusts = today.filter(x => x.gust_kmh != null), peak = gusts.length ? gusts.reduce((a, x) => x.gust_kmh > a.gust_kmh ? x : a) : null;
  const rw = m.hr.filter(x => wk && x[wk] != null), hourAvg = rw.length ? rw.reduce((s, x) => s + x[wk], 0) / rw.length : null;
  const gust10 = v.gust_10m_kmh ?? maxOf(m.recent.filter(x => x.t >= m.tn - 6e5).map(x => x.gust_kmh)) ?? v.gust_kmh;
  const wet = today.filter(x => x.rain_mm > 0), rainTxt = wet.length ? FC.t(WD.rained, { a: hh(wet[0].t), b: hh(new Date(wet[wet.length - 1].t.getTime() + HOUR)) }) : WD.dry;
  const tk = hd('temp'), tLo = minOf(today.map(x => x[tk] && x[tk][0])), tHi = maxOf(today.map(x => x[tk] && x[tk][2]));
  const hLo = minOf(today.map(x => x.humidity && x.humidity[0])), hHi = maxOf(today.map(x => x.humidity && x.humidity[2]));
  // The 3-h pressure tendency from the raw hourly means (hPa), tide taken out where the location is known; temperature
  // over 1 h; judged before any unit conversion, shown in the page's unit
  if (!H.has('pressure_hpa')) locNeed();
  // one computation per render; the open sheet reads the same objects (S.tr)
  // the trend is judged at the reading's time (freshness gate: the newest closed hour at most 2 h before it)
  const lt = Date.now() - (L.age_s || 0) * 1e3, pr = FCT.pressure(m.raw, S.loc || null, lt), tr = tk === 'temp_c' ? FCT.temp(m.raw, lt) : null;
  // the humidity arrow only when it moves (no glyph when steady)
  const hr = FCT.hum(m.raw, lt), ht = hr && hr.code !== 'steady' ? [hr, 'h', f0, 3] : null;
  const pt = pr && [pr, 'p', fp, 3], tt = tr && [tr, 't', f1, 1];
  S.tr = { baro: pt, temp: tt, hum: ht };
  const solarRej = m.rej.includes('solar_wm2'), has = k => v[k] != null || m.rej.includes(k);
  // every tile exists only if its station sends its headline field: the first of the tile's fields that is not
  // hidden (HEADS); a hidden field leaves the tile, its sub-line and its chart. Then sorted by ORDER.
  // g: the trend glyph, shown beside the title instead of the value on a narrow tile (app.css container query)
  const lab = (t, g = '') => `<span class="lab"><i></i>${t}${g}<span class="more" aria-hidden="true">›</span></span>`;
  // a tile whose headline value is carried (not in the latest upload) is dimmed and says how old it is
  const HKF = { wind: () => wk, rain: () => rk, temp: () => tk, hum: () => 'humidity', baro: () => 'pressure_hpa', sun: () => sk, indoor: () => ik };
  const tile = (key, cls, aria, inner) => { const a = m.old(HKF[key] ? HKF[key]() : key);
    if (a > 0) { cls += ' carried'; inner = inner.replace('<span class="more"', `<span class="ago" data-k="${esc(HKF[key] ? HKF[key]() : key)}">${ago(Math.round(a))}</span><span class="more"`); }
    return tile0(key, cls, aria, inner); };
  const tile0 = (key, cls, aria, inner) => `<button type="button" class="glass tile ${cls} lg" data-m="${esc(key)}" style="--c:${M[key].c}"${st} aria-label="${FC.t(WD.tile_open, { aria: esc(aria), m: esc(M[key].lo) })}">${inner.replace('<i></i>', icon(key))}</button>`;
  const T = [];
  CH.length = 0;
  // hero fallback: temperature leads when the station has no wind
  const windHero = !!wk, wName = wk === 'gust_kmh' ? WD.m_gust : WD.m_wind, stat = (k, val, n = '', t = '') => `<div class="stat"${t ? ` title="${t}"` : ''}><span class="k">${k}</span><span class="v">${f1(val)}<small>${U.w}</small></span>${n}</div>`;
  const peakStat = stat(WD.peak, peak && peak.gust_kmh, peak ? `<span class="n">${FC.t(WD.peak_at, { t: hh(peak.t) })}</span>` : '');
  const wc = wk === 'wind_kmh' && calm(v) ? 'calm' : v.wind_dir != null ? 'from' : '';  // the hero's accessible name, one whole template per case
  if (windHero) T.push(['wind', tile('wind', 'hero', FC.t(WD['a_w' + (wc && '_' + wc) + (gustOn ? '_g' : '')], { name: wName, v: f1(v[wk]), u: U.w, d: card(v.wind_dir), g: f1(gust10) }), `
    <span class="lab"><i></i>${wName}<span style="color:var(--ink-3)">· ${v[wk] != null ? beaufort(v[wk]) : ''}</span><span class="more" aria-hidden="true">›</span></span>
    <div class="body">
      <div>${dial(m, wk)}</div>
      <div class="stats">
        ${gustOn ? stat(WD.gust10, gust10, '', derWhy('gust_10m_kmh')) + peakStat : wk === 'gust_kmh' ? peakStat : ''}
        ${stat(m.hrMin < 55 ? FC.t(wk === 'gust_kmh' ? WD.avg_gust_m : WD.avg_m, { m: m.hrMin }) : wk === 'gust_kmh' ? WD.avg_gust_h : WD.avg_h, hourAvg)}
      </div>
    </div>
    <div>
      <div class="legend"><span><svg viewBox="0 0 22 10" aria-hidden="true"><line x1="0" y1="5" x2="22" y2="5" stroke="var(--wind)" stroke-width="2"/></svg>${wName}</span>${gustOn ? '<span><svg viewBox="0 0 22 10" aria-hidden="true"><line x1="0" y1="5" x2="22" y2="5" stroke="var(--ink-3)" stroke-width="1.5" stroke-dasharray="2 4"/></svg>' + WD.lg_gust + '</span>' : ''}</div>
      ${windSpark(m.recent, wk)}
    </div>`)]);
  const rk = hd('rain'), rLab = rk === 'rain_mm' ? WD.r_hour : WD.r_today, rHist = !H.has('rain_daily_mm');
  const LS = (S.live && S.live.station) || {}, der = LS.derived || {}, warm = LS.warming || [];
  if (rk) T.push(['rain', tile('rain', 't-rain', `${rLab} ${fr(v[rk])} ${U.r}`, `${lab(rLab)}
    <span class="val">${fr(v[rk])}<small>${U.r}</small></span>
    <span class="sub">${v.rain_rate_mmh != null && !H.has('rain_rate_mmh') ? `<span>${FC.t(WD.r_rate, { v: fr(v.rain_rate_mmh), u: U.r })}</span>` : ''}${rk !== 'rain_mm' && !H.has('rain_mm') ? (der.rain_mm != null ? `<span title="${derWhy('rain_mm')}">${FC.t(WD.r_last, { v: fr(v.rain_mm), u: U.r })}</span>` : warm.includes('rain_mm') ? `<span title="${WD.r_warm}">${WD.r_last0}</span>` : `<span>${FC.t(WD.r_last, { v: fr(v.rain_mm), u: U.r })}</span>`) : ''}${rHist ? `<span>${rainTxt}</span>` : ''}</span>
    <span class="foot">${rHist ? t24(m, x => x.rain_mm, 'bars', 'var(--rain)', 44, v => fr(v) + ' ' + U.r) : ''}</span>`)]);
  const tLab = tk === 'dewpt_c' ? WD.m_dew : WD.m_temp;
  if (tk) T.push(['temp', tile('temp', windHero ? '' : 'hero', FC.t(tt ? WD.a_t_tr : WD.a_t, { l: tLab, v: f1(v[tk]), tr: tt && trV(tt) + ' ' + trL(tt) }), `${lab(tLab, trGl(tt))}
    <span class="val">${f1(v[tk])}<small>${U.t}${trGl(tt)}</small></span>
    <span class="sub">${tLo != null ? `<span>${FC.t(WD.t_today, { lo: f1(tLo), hi: f1(tHi), u: U.t })}</span>` : ''}${tk === 'temp_c' && v.dewpt_c != null ? `<span>${FC.t(WD.t_dew, { v: f1(v.dewpt_c), u: U.t })}</span>` : ''}${feels(v) != null ? `<span>${FC.t(WD.t_feels, { v: f1(feels(v)), u: U.t })}</span>` : ''}</span>
    <span class="foot">${t24(m, x => x[tk] && x[tk][1], 'area', 'var(--temp)', 44, v => f1(v) + ' ' + U.t)}</span>`)]);
  if (has('humidity')) T.push(['hum', tile('hum', '', FC.t(WD['a_h' + (ht ? '_tr' : '') + (hLo != null ? '_day' : '')], { v: f0(v.humidity), tr: ht && trV(ht) + ' ' + trL(ht), lo: f0(hLo), hi: f0(hHi) }), `${lab(WD.m_hum, trGl(ht))}
    <span class="val">${f0(v.humidity)}<small>%${trGl(ht)}</small></span>
    <span class="sub">${hLo != null ? `<span>${FC.t(WD.h_today, { lo: f0(hLo), hi: f0(hHi) })}</span>` : ''}</span>
    <span class="foot">${t24(m, x => x.humidity && x.humidity[1], 'area', 'var(--hum)', 44, v => f0(v) + ' %')}</span>`)]);
  const bn = tileNote('baro');  // the amber notice after "Pressure", its own button placed over the tile
  const bnx = bn && ntx(bn);  // its chip in the page's words
  if (has('pressure_hpa')) T.push(['baro', (bn ? '<div class="tw">' : '') + tile('baro', '', FC.t(pt ? WD.a_p_tr : WD.a_p, { v: fp(v.pressure_hpa), u: U.p, tr: pt && trV(pt) + ' ' + trL(pt) }), `${lab(WD.m_baro + (bn ? '<span class="nsp"></span>' : ''), trGl(pt))}
    <span class="val">${fp(v.pressure_hpa)}<small>${U.p}${trGl(pt)}</small></span>
    <span class="foot">${t24(m, x => x.pressure_hpa && x.pressure_hpa[1], 'line', 'var(--baro)', 40, v => fp(v) + ' ' + U.p)}</span>`)
    + (bn ? `<button type="button" class="tnb" data-n="${esc(bn.id)}" aria-label="${FC.t(WD.a_warn, { chip: esc(bnx.chip) })}" title="${esc(bnx.chip)}">${FCO.svg('circle-alert')}</button></div>` : '')]);
  const sk = hd('sun'), solarOn = !H.has('solar_wm2'), solarBars = () => t24(m, x => x.solar_wm2 ? x.solar_wm2[1] : null, 'bars', 'var(--sunc)', 34, v => f0(v) + ' W/m²');
  const r10 = m.recent.length ? m.recent[m.recent.length - 1].t - 6e5 : 0, uvs = [v.uv].concat(m.recent.filter(x => x.t >= r10).map(x => x.uv)), uvMax = maxOf(uvs), fu = uvf(uvs);
  if (sk === 'uv') T.push(['sun', tile('sun', '', FC.t(solarOn && solarRej ? WD.a_uv_rej : WD.a_uv, { v: fu(uvMax) }), `${lab(WD.m_sun)}
    <span class="val" title="${WD.uv_title}">${fu(uvMax)}<small>UV</small></span>
    <span class="sub">${!solarOn ? '' : solarRej ? rejChip(WD.solar_rej) : `<span>${FC.t(WD.uv_solar, { v: f0(v.solar_wm2) })}</span>`}</span>
    <span class="foot">${solarOn ? solarBars() : ''}</span>`)]);
  if (sk === 'solar_wm2') T.push(['sun', tile('sun', '', solarRej ? WD.a_sun_rej : FC.t(WD.a_sun, { v: f0(v.solar_wm2) }), `${lab(WD.l_sun)}
    <span class="val">${solarRej ? '–' : f0(v.solar_wm2)}<small>W/m²</small></span>
    <span class="sub">${solarRej ? rejChip(WD.solar_rej) : ''}</span>
    <span class="foot">${solarBars()}</span>`)]);
  const trace = k => m.yday.concat(today).some(x => x[k]) ? t24(m, x => x[k] ? x[k][1] : null, 'line', 'var(--ink-2)', 40, v => (k === 'indoor_humidity' ? f0(v) + ' %' : f1(v) + ' ' + U.t)) : '';
  const ik = hd('indoor');
  if (ik === 'indoor_temp_c') T.push(['indoor', tile('indoor', '', FC.t(v.indoor_humidity != null ? WD.a_in_h : WD.a_in, { v: f1(v.indoor_temp_c), h: f0(v.indoor_humidity) }), `${lab(WD.m_indoor)}
    <span class="val">${f1(v.indoor_temp_c)}<small>${U.t}</small></span>
    <span class="sub">${v.indoor_humidity != null ? `<span>${FC.t(WD.in_hum, { v: f0(v.indoor_humidity) })}</span>` : ''}</span>
    <span class="foot">${trace('indoor_temp_c')}</span>`)]);
  if (ik === 'indoor_humidity') T.push(['indoor', tile('indoor', '', FC.t(WD.a_inh, { v: f0(v.indoor_humidity) }), `${lab(WD.m_inh)}
    <span class="val">${f0(v.indoor_humidity)}<small>%</small></span>
    <span class="foot">${trace('indoor_humidity')}</span>`)]);
  // a configured extra sensor always has its tile, "–" until it has a reading
  m.promoted.forEach(p => {
    const fx = p.dec === 0 ? f0 : p.dec >= 2 ? f2 : f1;
    T.push([ORDER.includes(p.group) ? p.group : 'other', tile(p.k, '', `${p.name} ${fx(v[p.k])} ${p.u}`, `${lab(esc(p.name))}
      <span class="val">${fx(v[p.k])}<small>${esc(p.u)}</small></span>
      <span class="foot"></span>`)]);
  });
  if (!T.length) T.push(['other', `<div class="glass tile lg" style="cursor:default"><span class="lab">${WD.none}</span><span class="sub">${WD.none_sub}</span></div>`]);
  // the outlook strip (codes from the firmware, words + glyphs from outlook.js) above the wind tile
  // in one lead block that keeps the wind tile's place (the right column stays aligned); without a wind tile it leads
  // a station with fewer than 3 tiles gets no strip (too little to describe); fillRow() widens its tiles
  const ol = !stale && T.length > 2 && FCO.lines(L.outlook, { gust: g => f0(cv('gust_kmh', g)) + ' ' + U.w,
    range: (a, b) => FC.t(WD.o_range, { a: f0(cv('temp_c', a)).replace('-', '−'), b: f0(cv('temp_c', b)).replace('-', '−'), u: U.t }) });
  if (ol) {
    // no clock on the strip: "as of" lives in the ⓘ explanation
    // page 2 "Next" (lazy dash-wufc.js) while the WU line or the airport module exists; its card replaces the strip
    if (!S.wfx && !ap() && (L.wufc || (S.status && S.status.metar))) { S.wfx = 1; FC.lazy('/*@asset dash-wufc.js*/').then(render, () => {}); }
    const p2 = typeof FCX != 'undefined' && FCX.p2(L);
    const strip = p2 ? FCX.card(ol, p2) : `<div class="glass ol" role="group" aria-label="${WD.ol}">${olIn(ol)}${OLI()}</div>`;
    const w = T.find(x => x[0] === 'wind');
    if (w) w[1] = `<div class="lead">${strip}${w[1]}</div>`;
    else T.push(['wind', `<div class="lead solo">${strip}</div>`]);
    // the explanation's script once per page load, ~5 s after the strip first shows, so the ⓘ opens at once
    if (!whyPre) { whyPre = true; setTimeout(() => FC.lazy('/*@asset outlook-why.js*/').catch(() => {}), 5000); }
  }
  const fm = document.activeElement && document.activeElement.closest && document.activeElement.closest('.tile[data-m]');  // the focused tile, focused again after the grid is rebuilt
  const ow = olNow();
  $('#grid').innerHTML = T.sort((a, b) => ORDER.indexOf(a[0]) - ORDER.indexOf(b[0])).map(x => x[1]).join('');
  if (fm) { const t = document.querySelector(`.tile[data-m="${fm.dataset.m}"]`); if (t) t.focus({ preventScroll: true }); }
  fillRow(); placeNotes(); if (typeof FCX != 'undefined') FCX.wire();
  olFade(ow, olNow(), $('#grid .ol'));
  renderHealth();
  lgAll();
  refreshSheet();
}
// The dashboard's status row is the glyphs only, their words on hover (title) and for screen readers;
// the Details live in Settings (adminface.js), under the same row
function renderHealth(head = S.live) { $('#health').innerHTML = `<h2 class="sr">${WD.health}</h2>` + healthRow(head, S.status && S.status.mqtt >= 0); }


// ------------------------------------------------------------------ history sheet
// alt: the field a two-field sheet shows, null = its headline; kept across ranges and polls, reset on open
let measure = 'wind', range = 'today', table = false, lastFocus = null, alt = null, why = false, whyPre = false;  // why: the ⓘ sheet is open
// The sheet machinery (rows, charts, rose, table, Export CSV) is dash-sheet.js, fetched on the first open;
// until it is here the sheet says "Loading…". dash-sheet.js replaces renderSheet.
let renderSheet = () => {
  $('#shTitle').innerHTML = icon(measure) + esc((M[measure] || {}).name || ''); $('#shNow').innerHTML = '';
  $('#shBody').innerHTML = `<p class="cap loading">${WD.loading}</p>`;
  FC.lazy('/*@asset dash-sheet.js*/').then(() => { if (sheetOpen()) renderSheet(); },
    () => { $('#shBody').innerHTML = `<p class="cap">${WD.no_chart}</p>`; });
};
function syncSeg() { document.querySelectorAll('#seg button').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.r === range))); }
function openSheet(ms, from, byKey) {
  measure = ms; range = 'today'; table = false; lastFocus = from; alt = null; syncSeg();
  $('#tblBtn').setAttribute('aria-pressed', 'false'); $('#tblBtn').textContent = WD.show_table;
  // unhide first: the chart sizes its viewBox from the sheet's real width
  $('#scrim').hidden = false; $('#sheet').hidden = false; pageInert(true); renderSheet(); $('#shScroll').scrollTop = 0; (byKey ? $('#shClose') : $('#sheet')).focus();
  document.body.style.overflow = 'hidden';
  // while a sheet is open: history every 60 s, daily every 10 min
  FC.enable('hourly', true); FC.enable('recent', true); FC.enable('daily', true);
}
function closeSheet() {
  $('#scrim').hidden = true; $('#sheet').hidden = true; document.body.style.overflow = ''; pageInert(false);
  // a poll re-rendered the grid -- back to the re-rendered notice button, else the strip's ⓘ
  if (why) { why = false; $('#sheet').classList.remove('why', 'aps', 'short'); if (lastFocus && lastFocus.isConnected) lastFocus.focus({ preventScroll: true }); else { const b = (lastFocus && lastFocus.classList && lastFocus.classList.contains('tnb') && $('#grid .tnb')) || $('#olWhy'); if (b) b.focus({ preventScroll: true }); } return; }
  // the tile it came from, or its re-rendered twin (a live poll rebuilds the grid)
  const t = lastFocus && lastFocus.isConnected ? lastFocus : document.querySelector(`.tile[data-m="${measure}"]`); if (t) t.focus({ preventScroll: true });
  // a closed sheet stops its history polling at once; a changed live kicks hourly/recent again
  FC.enable('daily', false); FC.enable('hourly', false); FC.enable('recent', false);
}

// ------------------------------------------------------------------ polling (one request at a time, core.js)
// The airport view, a second page of the dashboard (dash-airport.js, fetched on the first tap of the plane).
// body[data-v=ap] hides the station's head, tiles and Details (app.css); the choice is kept per tab (sessionStorage),
// a new tab opens on the station. No DOM work here beyond the switch: the view renders itself into #apv.
function apView(on) {
  S.ap = on;
  try { sessionStorage.setItem('fc-view', on ? 'ap' : ''); } catch (e) {}
  document.body.dataset.v = on ? 'ap' : '';
  if (on) FC.lazy('/*@asset dash-airport.js*/').then(() => { if (S.ap) APV.open(); }, () => { S.ap = false; document.body.dataset.v = ''; renderBar(); });
  else { if ($('#sheet').classList.contains('aps') && !$('#sheet').hidden) { lastFocus = $('#apTab'); closeSheet(); } if (S.live) render(); }
}
function pick(id) {
  if (S.ap) { apView(false); renderBar(); }
  if (!id || id === S.cur) return;
  S.cur = id; FC.store('fc-station', id);
  S.live = S.live && S.live.station.id === id ? S.live : null;
  renderBar(); FC.kick('live');
}
function onLive(res) {
  const d = res.data;
  if (!d.station && !d.error) { S.head = d; if (S.status && !S.status.ids.length) renderEmpty(); return; }  // no station: bridge only
  if (d.error) { // a forgotten station: fall back to the first one the bridge lists
    const first = S.status && S.status.ids[0];
    S.cur = first && first !== S.cur ? first : ''; FC.store('fc-station', S.cur);
    if (!S.status) FC.kick('status');
    return;
  }
  S.stale = false;
  const fresh = res.changed || !S.live;
  // the age counts from the fetch that brought this body: a 304 replays it with that fetch's time (also after a station switch)
  S.liveAt = res.at || Date.now();
  S.live = d;
  if (d.station.name && S.names[d.station.id] !== d.station.name) { S.names[d.station.id] = d.station.name; FC.store('fc-names', JSON.stringify(S.names)); }
  renderBar();
  if (fresh) render(); else stateLine();  // a 304 changes nothing on screen but the age
  // tile history: once at idle, then only when live changed
  if (fresh) { FC.kick('hourly'); FC.kick('recent'); }
}
const failed = () => { S.stale = true; renderBar(); if (S.live) render(); };
// no station yet: /api/live without a station still carries the bridge's Details (clock, diag)
FC.poll('live', { every: 30e3, url: () => S.cur ? '/api/live?station=' + encodeURIComponent(S.cur) : S.status && !S.status.ids.length ? '/api/live' : null, on: onLive, err: failed });
FC.poll('status', { every: 60e3, url: () => '/api/bridge/status', on: res => {
  const m = S.status && S.status.metar, q = S.status && S.status.mqtt;
  S.status = res.data;
  if (S.live) stateLine();  // "Updating firmware…" comes and goes with the bridge status
  if (S.live && q !== S.status.mqtt) renderHealth();  // a broker set or removed: the MQTT glyph comes or goes
  // the airport module switched: the card's TAF line goes now; on an off -> on switch the forecast poll asks at once
  // (a no-op until dash-wufc.js is loaded: it registers the poll and asks then)
  if (S.live && !m != !S.status.metar) { render(); if (!m) FC.kick('wufcm'); }
  if (!S.status.ids.length) { S.cur = ''; S.live = null; renderBar(); renderEmpty(); if (!S.head) FC.kick('live'); return; }
  if (!S.cur || !S.status.ids.includes(S.cur)) { const first = S.status.ids[0]; if (first && first !== S.cur) { S.cur = first; FC.store('fc-station', first); FC.kick('live'); } }
  renderBar();
}, err: failed });
// history: queued after live/status; kicked by a changed live; polled every 60 s only while a sheet is open
// a measure sheet is open (the ⓘ explanation shares #sheet but needs no history)
const sheetOpen = () => !$('#sheet').hidden && !why;
function refreshSheet() { if (!$('#sheet').hidden) why ? why() : renderSheet(); }  // why = the open body's render (outlook or airports)
// the 90-day sheet reads the daily file (days=90); the RAM ring keeps 30 days
const hist = (name, key, every, q = '') => FC.poll(name, { every, enabled: false, url: () => S.cur && S.live ? `/api/${name}?station=` + encodeURIComponent(S.cur) + q : null,
  on: res => { S.histErr = false; S[key] = res.data; if (!sheetOpen() && name !== 'daily') FC.enable(name, false); if (res.changed) render(); },
  err: () => { S.histErr = true; refreshSheet(); } });  // never "Loading…" for ever
// what an empty sheet says while its data is not there (dash-sheet.js)
const loadingCap = () => S.histErr ? WD.retrying : WD.loading;
hist('hourly', 'hourly', 60e3); hist('recent', 'recent', 60e3); hist('daily', 'daily', 600e3, '&days=90');
// first load: status first only when there is no remembered station (live needs an id)
if (!S.cur) FC.kick('status');

// ------------------------------------------------------------------ events
const fine = matchMedia('(hover: hover) and (pointer: fine)'), tipOff = () => document.querySelectorAll('#grid .ttip, #grid .txh').forEach(e => e.remove());
$('#grid').addEventListener('pointermove', e => {
  const s = fine.matches && e.target.closest('svg[data-i]'), c = s && CH[s.dataset.i];
  if (!c) return tipOff();
  const b = s.getBoundingClientRect(), fx = (e.clientX - b.left) / b.width, txt = c[2](c[0] + fx * c[1]), par = s.parentNode;
  if (!txt) return tipOff();
  let tip = par.querySelector('.ttip'), xh = par.querySelector('.txh');
  if (!tip) { tipOff(); par.style.position = 'relative'; tip = par.appendChild(document.createElement('span')); tip.className = 'ttip'; xh = par.appendChild(document.createElement('span')); xh.className = 'txh'; }
  tip.textContent = txt;
  const pr = par.getBoundingClientRect(), px = b.left - pr.left + fx * b.width, w = tip.offsetWidth;
  tip.style.left = Math.max(0, Math.min(par.clientWidth - w, px - w / 2)) + 'px';
  Object.assign(xh.style, { left: px + 'px', top: b.top - pr.top + 'px', height: b.height + 'px' });
});
$('#grid').addEventListener('pointerleave', tipOff);
$('#grid').addEventListener('click', e => { if (e.target.closest('#olWhy')) { openWhy(e.target.closest('#olWhy'), e.detail === 0); return; }
  if (e.target.closest('.tnb')) { openNote(e.target.closest('.tnb'), e.detail === 0); return; }
  // anywhere on the strip opens it too (focus returns to the ⓘ), not at the end of a text selection
  if (e.target.closest('.ol') && getSelection().type !== 'Range') { openWhy($('#olWhy'), e.detail === 0); return; }
  const t = e.target.closest('.tile[data-m]'); if (t) openSheet(t.dataset.m, t, e.detail === 0); });
// The strip's ⓘ in the same sheet, without its range tools; the words come with outlook-why.js on first use
// the explanation's numbers in the page's units; dt and dp are changes (no offset: °C x 1.8 = °F), sizes only
const nil = v => v == null || isNaN(v);
const WHYF = { t: v => nil(v) ? '–' : f1(cv('temp_c', v)) + ' ' + U.t, dt: v => nil(v) ? '–' : f1(Math.abs(v) * (IMP ? 1.8 : 1)) + ' ' + U.t,
  w: v => nil(v) ? '–' : f1(cv('wind_kmh', v)) + ' ' + U.w, r: v => nil(v) ? '–' : fr(cv('rain_mm', v)) + ' ' + U.r,
  rr: v => nil(v) ? '–' : fr(cv('rain_rate_mmh', v)) + ' ' + U.r + '/h', p: v => nil(v) ? '–' : (v > 0 ? '+' : '') + fp(cv('pressure_hpa', v)).replace('-', '−') + ' ' + U.p,
  dp: v => fp(cv('pressure_hpa', Math.abs(v))) + ' ' + U.p, hm: s => s ? hm(s) : '–',
  g: v => f0(cv('gust_kmh', v)) + ' ' + U.w,  // the gust as the strip says it (whole units)
  km: v => IMP ? Math.round(v / 1.609344) + ' mi' : v + ' km' };  // the airport report's distance
function whyBody() {
  const o = S.live && S.live.station && S.live.station.outlook;
  $('#shBody').innerHTML = typeof FCW === 'undefined' ? `<p class="cap loading">${WD.loading}</p>` : o ? FCW.why(o, WHYF, typeof FCX != 'undefined' ? FCX.why(S.live.station) : null) : '';
}
function openWhy(from, byKey) {
  why = whyBody; lastFocus = from; $('#sheet').classList.add('why');
  $('#shTitle').innerHTML = FCO.svg('info') + WD.ol; $('#shNow').innerHTML = ''; $('#shSub').textContent = '';  // the card and its sheet; the admin tile keeps "Local outlook"
  $('#scrim').hidden = false; $('#sheet').hidden = false; pageInert(true); document.body.style.overflow = 'hidden';
  whyBody(); (byKey ? $('#shClose') : $('#sheet')).focus();
  FC.lazy('/*@asset outlook-why.js*/').then(() => { if (why === whyBody) whyBody(); }, () => { $('#shBody').innerHTML = `<p class="cap">${WD.no_why}</p>`; });
}
// A tile notice opens a short read-only sheet (<= 72dvh), the words from outlook-why.js; for the factory
// value with the airport module on, the nearby airports' pressure from /api/metar
const WHYP = Object.assign({ pa: v => nil(v) ? '–' : fp(cv('pressure_hpa', v)) + ' ' + U.p }, WHYF);
function noteBody() {
  const n = tileNote('baro') && ntx(tileNote('baro'));
  if (!n) return closeSheet();
  $('#shTitle').innerHTML = FCO.svg('circle-alert') + esc(n.chip);
  $('#shBody').innerHTML = typeof FCW === 'undefined' ? `<p class="cap loading">${WD.loading}</p>` : FCW.note(n, S.live.station.values.pressure_hpa, S.noteRef, WHYP);
}
function openNote(from, byKey) {
  why = noteBody; lastFocus = from; S.noteRef = null; $('#sheet').classList.add('why', 'short'); $('#shNow').innerHTML = ''; $('#shSub').textContent = '';
  $('#scrim').hidden = false; $('#sheet').hidden = false; pageInert(true); document.body.style.overflow = 'hidden';
  noteBody(); (byKey ? $('#shClose') : $('#sheet')).focus();
  FC.lazy('/*@asset outlook-why.js*/').then(() => { if (why === noteBody) noteBody(); }, () => { $('#shBody').innerHTML = `<p class="cap">${WD.no_why}</p>`; });
  if (S.status && S.status.metar) FC.get('/api/metar').then(r => { S.noteRef = nearQnh(r.data); if (why === noteBody) noteBody(); }, () => {});
}
// the nearby airports' QNH on the firmware's one basis (/api/metar's near_qnh_hpa -- within 60 km, consistent)
const nearQnh = d => d && d.reference && d.reference.near_qnh_hpa != null ? d.reference.near_qnh_hpa : null;
$('#bridge').addEventListener('click', e => { const b = e.target.closest('button[data-id]'); if (b) pick(b.dataset.id); });
$('#stations').addEventListener('click', e => { const b = e.target.closest('button'); if (!b) return;
  if (b.id === 'apTab') { if (!S.ap) { apView(true); renderBar(); } return; }
  pick(b.dataset.id); });
$('#scrim').addEventListener('click', closeSheet); $('#shClose').addEventListener('click', closeSheet);
sheetDrag($('#sheet'), $('#scrim'), closeSheet);  // drag down to close (phone sheet, core.js)
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#sheet').hidden) closeSheet(); });
$('#seg').addEventListener('click', e => { const b = e.target.closest('button'); if (!b) return; range = b.dataset.r; syncSeg(); renderSheet(); });
$('#shBody').addEventListener('click', e => { const b = e.target.closest('#csvBtn'); if (b) exportCsv(b); });
$('#shNow').addEventListener('click', e => { const b = e.target.closest('button[data-f]'); if (b) { alt = b.dataset.f; renderSheet(); const n = $(`#shNow button[data-f="${alt}"]`); if (n) n.focus(); } });
$('#tblBtn').addEventListener('click', () => { table = !table; $('#tblBtn').setAttribute('aria-pressed', String(table)); $('#tblBtn').textContent = table ? WD.hide_table : WD.show_table; renderSheet(); });
$('#themeBtn').addEventListener('click', FC.toggleTheme);
$('#unitBtn').addEventListener('click', () => { FC.store('fc-units', IMP ? 'metric' : 'imperial'); setUnits(); renderBar(); render(); if (S.ap && typeof APV !== 'undefined') APV.render(); });  // a lazy script's const is no window property
// "updated N s ago" keeps counting between polls
function stateLine() {
  const L = S.live.station, age = Math.round(L.age_s + (Date.now() - S.liveAt) / 1e3), stale = !L.online || S.stale;
  const kept = S.hourly && S.hourly.station === L.id && (S.hourly.hours.length || S.hourly.open);
  const rw = S.live.time_synced ? dayTime(L.last_seen, new Date()) : '';  // a time only with a synced clock
  const os = S.live.time_synced && offSince(L.last_seen, new Date());
  const txt = S.status && S.status.ota ? WD.s_ota
    : L.uploads ? (!L.online ? FC.t(os ? WD.s_off_since : WD.s_off_ago, { since: os, ago: ago(age), why: WD.off_why }) : FC.t(WD.s_on, { ago: ago(age) }))
    : L.restored ? (rw ? FC.t(WD.s_rest_from, { t: rw }) : WD.s_rest_flash)
    : kept ? WD.s_kept : WD.s_wait;
  $('#stState').innerHTML = `<span class="dot ${stale ? 'off' : ''}"></span><span>${/[.!?]$/.test(txt) ? txt : txt + '.'}</span>${L.values && L.values.battery_low ? rejChip(WD.bat) : ''}`;
}
setInterval(() => { if (S.live && document.visibilityState !== 'hidden') { stateLine();
  document.querySelectorAll('.ago[data-k]').forEach(e => { e.textContent = ago(Math.round(carried(e.dataset.k))); }); } }, 10e3);

// ------------------------------------------------------------------ edge refraction (Chromium only, progressive)
// The field itself is dash-lg.js, fetched once on Chromium after the first render; until then lgAll does
// nothing and the CSS blur shows. Notices in index.html.
const lgOK = (() => {
  if (matchMedia('(prefers-reduced-transparency: reduce)').matches) return false;
  const b = navigator.userAgentData && navigator.userAgentData.brands;
  return b ? b.some(x => /Chromium|Google Chrome|Microsoft Edge/i.test(x.brand)) : /Chrome\//.test(navigator.userAgent);
})();
let lgAll = () => {};
if (lgOK) setTimeout(() => FC.lazy('/*@asset dash-lg.js*/').catch(() => {}), 1500);
let lgT; addEventListener('resize', () => { clearTimeout(lgT); lgT = setTimeout(() => { fillRow(); placeNotes(); lgAll(); refreshSheet(); }, 180); }, { passive: true });
renderBar();
