'use strict';
// admin-station.js -- the station sheet of the admin face: its reference tables, the
// Console tab and the sheet itself, loaded on first use through FC.lazy() by adminface.js (openSheet). Same
// global scope as admin.js: it adds SHEETS.station and uses the helpers defined there.
// this file's words (a lazy script: its pack, if any, is in before it runs)
const WA = FC.w('admin-station', {
  block_q: 'Block <b>{w}</b>?', forget_q: 'Forget <b>{w}</b>?',
  f_deletes: 'fauxcloud deletes this station’s settings, filters, extra sensors and the readings kept for the dashboard, and removes its sensors from Home Assistant. Readings already in Home Assistant’s history stay there.',
  f_block: 'From now on this console’s uploads are ignored. Unblock it under Publishing.',
  f_online: '<b>This console is still uploading.</b>It will come back as a new station with default settings within a minute. Switch it off, connect it to another network, or block it instead.',
  f_back: 'If this console uploads again, it comes back as a new station with default settings.',
  f_type: 'Type {w} to confirm', keep_it: 'Keep it', block_st: 'Block station', forget_st: 'Forget station',
  t_general: 'General', t_filters: 'Filters', t_extra: 'Extra sensors', t_calib: 'Calibration', t_console: 'Console',
  st_none: '{id}. No upload since restart. {n} uploads since restart.', st_on: '{id}. Online, last upload {ago}. {n} uploads since restart.',
  st_off: '{id}. Offline, last upload {ago}. {n} uploads since restart.', st_mac: ' Console MAC {mac}.', st_bat: ' <b>The console reports a low battery.</b>',
  name: 'Name', name_h: 'Shown on the dashboard and in Home Assistant.', model: 'Console model',
  model_yt: 'This console’s field meanings and known fixes (rain rate, wind spikes, humidity drops).',
  model_gen: 'The standard upload meanings, no model-specific fixes.', fp_none: 'Not checked yet.',
  stale: 'Show as offline after', stale_2: '2 minutes without an upload', stale_4: '4 minutes (recommended)', stale_n: '{n} minutes',
  stale_h: 'The console uploads about once a minute.',
  publish: 'Send to Home Assistant', publish_h: 'Off: dashboard only, nothing published over MQTT.',
  backup: 'Backup: export or import this station’s settings', export: 'Export as a file', import: 'Import from a file',
  backup_h: 'The file holds this station’s settings. An import shows here first; nothing changes until you save.',
  km: { one: 'Key changed: 1 upload ignored since restart.', other: 'Key changed: {n} uploads ignored since restart.' },
  km_h: 'The console now sends another Weather Underground key. If you changed it, accept it; if not, someone else may be using this station’s ID.',
  km_accept: 'Accept the new key', key_bound: 'Console key: bound to this console (uploads with another key are ignored).',
  key_free: 'Console key: not bound yet; the next upload binds it.',
  replaced: 'Replaced this console? Forget it to free its place. Not yours? Block it.', block: 'Block', forget_this: 'Forget this station',
  hidden: 'Hidden', lim_both: '{a}…{b} {u}', lim_min: 'at least {a} {u}', lim_max: 'at most {b} {u}', lim_jump: 'max change {m} {u}/min',
  no_filters: 'no filters',
  filters_h: 'Everything this console sends is listed. Open a measurement to reject readings outside a range, or changes faster than a limit, or to hide it.',
  rej_now: 'rejected now', min: 'Minimum', max: 'Maximum', jump: 'Max change per minute', empty_h: 'Leave a box empty for no limit.',
  hide: 'Hide this measurement', hide_h: 'Hidden: still recorded, not shown and not sent to Home Assistant.',
  not_sent: 'Not sent by this console: {list}.', rej_since: 'Rejected since restart', rej_n: { one: '{n} reading', other: '{n} readings' },
  extra_h: 'Values your console sends that fauxcloud does not know yet. Add one as a sensor to show it on the dashboard and in Home Assistant (up to 4).',
  custom: 'Custom', from_key: '{type} from {key}, {seen}', edit: 'Edit', remove: 'Remove', forget: 'Forget', four: '4 sensors already',
  text_vals: 'text values cannot become sensors', add_sensor: 'Add as sensor', none_yet: 'None so far. They appear here after an upload that contains one.',
  calib_warn: '<b>Calibrate in one place only.</b>If your console has its own calibration (the YT60234 has one on its ADVANCED page), set it there and leave these at their defaults. Both together would correct twice.',
  calib_h: 'From {lo} to {hi}. Empty = {d}.',
  title_block: 'Block this station', title_forget: 'Forget this station', not_done: 'Not done.', blocked: 'Blocked {name}', forgot: 'Forgot {name}', forgot_rec: 'Forgot {name}',
  key_toast: 'The next upload binds the new key', forgot_key: 'Forgot {p}',
  e_name: 'Give it a name.', e_long31: 'Too long: at most 31 bytes (Polish letters count as 2).', e_key: 'Use 1 to 23 lowercase letters, digits or _.',
  e_taken: 'This key is taken by a built-in measurement. Choose another.', e_used: 'Another sensor of this station already uses this key.',
  e_minmax: '{l}: the minimum is above the maximum. Fix one of them.', e_range: 'Use a value from {lo} to {hi}.',
  e_long47: 'Too long: at most 47 bytes (Polish letters count as 2).',
  e_file: 'This file is not a station settings file. Nothing was changed.', imported: 'Imported. Review the tabs, then save.',
  seen_now: 'now {v}', seen_ago: '{v}, {ago}',
  p_add: 'Add <code>{p}</code> as a sensor', p_edit: 'Edit <code>{p}</code> as a sensor', p_type: 'What does it measure?',
  p_type_h: 'Sets the unit, the conversion and how Home Assistant shows it.', p_ph: 'e.g. Soil, bed 1',
  p_name_h: 'Shown on the dashboard tile and in Home Assistant.', p_preview: 'Console sends <b>{raw}</b> → shown as {shown}',
  p_wrong: 'If this looks wrong, pick another type or open Advanced.', advanced: 'Advanced', p_key: 'Key in Home Assistant and MQTT',
  p_key_h: 'Made from the name. Lowercase letters, digits and _ only, up to 23.', unit: 'Unit', conv: 'Conversion', c_none: 'None',
  c_scale: 'Multiply by…', scale: 'Multiply by', p_dc: 'Home Assistant device class', cancel: 'Cancel', p_ok_add: 'Add sensor', done: 'Done',
  p_save: 'Takes effect when you save the station.',
  // FIELDS: the core fields
  fl_temp_c: 'Temperature', fl_dewpt_c: 'Dew point', fl_humidity: 'Humidity', fl_pressure_hpa: 'Pressure', fl_wind_kmh: 'Wind speed',
  fl_gust_kmh: 'Wind gust', fl_wind_dir: 'Wind direction', fl_rain_mm: 'Rain, last hour', fl_rain_daily_mm: 'Rain today', fl_uv: 'UV index',
  fl_solar_wm2: 'Solar radiation',
  // FP: what the last upload looked like
  fp_match: 'Detected from the upload: Vevor/Youtong 7-in-1.',
  fp_unknown: 'Detected from the upload: looks like the 7-in-1 (console MAC not confirmed).',
  fp_mismatch: 'Detected from the upload: another console (Generic WU).',
  // RULES: each filter rule, its name and one line
  rl_parse: 'Unreadable value', rl_parse_d: 'The console sent something that is not a number.', rl_bounds: 'Impossible value',
  rl_bounds_d: 'Outside what any sensor can measure (e.g. humidity above 100 %).', rl_profile_wind: 'Wind spike',
  rl_profile_wind_d: 'Above the console model’s limit: a known sensor glitch, wind data dropped.', rl_profile_humidity: 'Humidity drop',
  rl_profile_humidity_d: 'A sudden drop typical of a sensor reset: the whole reading is dropped.', rl_limits: 'Outside your limits',
  rl_limits_d: 'Below the minimum or above the maximum set in Filters.', rl_jump: 'Too fast a change',
  rl_jump_d: 'Changed faster than the maximum change per minute set in Filters.', rl_rain_glitch: 'Rain counter glitch',
  rl_rain_glitch_d: 'Today’s rain went back outside midnight or jumped by over 100 mm at once: ignored.',
  // CALIB
  cl_temp_c_offset: 'Temperature offset', cl_humidity_offset: 'Humidity offset', cl_pressure_hpa_offset: 'Pressure offset',
  cl_wind_dir_offset: 'Wind direction offset', cl_rain_factor: 'Rain factor', cl_wind_factor: 'Wind factor',
  // TYPES: the promotion types (PM2.5, PM10 and CO₂ are names)
  ty_temperature: 'Temperature', ty_soil_temperature: 'Soil temperature', ty_humidity: 'Humidity', ty_soil_moisture: 'Soil moisture',
  ty_leaf_wetness: 'Leaf wetness', ty_pressure: 'Pressure', ty_wind_speed: 'Wind speed', ty_rain: 'Rain amount', ty_uv: 'UV index',
  ty_illuminance: 'Light level', ty_solar: 'Solar radiation', ty_battery: 'Battery level', ty_voltage: 'Voltage', ty_other: 'Something else',
  // the Console tab
  con_h: 'What this console sends, as it arrives. The Weather Underground ID and password are hidden, so a copy is safe to share.', con_copy: 'Copy',
  con_aria: 'Raw uploads of {id}', con_wait: 'Waiting for the next upload…', con_gap: { one: '… {n} upload not kept', other: '… {n} uploads not kept' },
  // Set north from the vane
  n_btn: 'Set north from the vane', n_wait: 'Turn the vane so it points north and hold it there until two readings agree (up to 3 minutes).',
  n_got: 'Reading {d}°: set the offset to {o}°?', n_none: 'No two matching readings yet. Is the vane held still and the console uploading?',
  n_set: 'Set', n_retry: 'Try again',
  // One template per message: the confirmation that a console with a new ID is the same station
  adopt_ask: '<p class="lead">Move <b>{from}</b> to <b>{to}</b>?</p><p class="hint">History, settings, learning and forwarding move to {to}, and fauxcloud accepts {to}’s key from now on. {from} stops. Home Assistant shows {to} as a new device. fauxcloud restarts.</p>',
  adopt_keep: 'Keep {from}', adopt_go: 'Move to {to}', adopt_done: 'Restarting… this page reconnects by itself.', adopt_title: 'Same console, new ID',
  // the Publishing sheet
  pub_title: 'Publishing', fq_set_q: 'Forget its settings?', fq_hist_q: 'Forget its history?',
  fq_set_h: 'Its saved settings on this bridge are deleted; the console keeps uploading.',
  fq_hist_h: 'Its hourly and daily history on this bridge is deleted; the console keeps uploading.', keep: 'Keep',
  pub_why: 'Off while forwarding is on: not enough memory on this board.', pub_auto: 'Send new stations to Home Assistant automatically',
  pub_auto_h: 'Off: a new station shows on the dashboard only, until you switch it on in its settings. Existing stations are not affected.',
  pub_lsi: '“Last seen” update in Home Assistant', lsi_60: 'Every minute', lsi_300: 'Every 5 minutes (recommended)', lsi_900: 'Every 15 minutes',
  pub_lsi_h: 'How often the “last seen” time moves while a station is quiet. Readings are always sent as they arrive.',
  pub_maxst: 'Stations kept', pub_maxst_h: 'A console beyond this number is ignored. {n} in use now; forget one to go lower.',
  pub_who: 'Who may upload', pub_lan: 'Accept uploads from my home network',
  pub_lan_h: 'Off: only consoles on the fauxcloud Wi-Fi can send readings. Only for advanced setups: lets devices on your home network send readings.',
  bl_loading: 'Blocked stations: loading…', bl_some: 'Blocked stations:', bl_none: 'Blocked stations: none.', unblock: 'Unblock', unblocked: 'Unblocked {name}',
  // English keeps "days" for 1 as before; the count form is for the languages that need it
  pub_fl: 'History without a station:', pub_fl_n: { one: '{h} h, {n} days', other: '{h} h, {n} days' }, pub_pk: 'Saved settings without a slot:',
  // LBL: the field keys as a person reads them in Details
  lb_temp_c: 'Temperature', lb_dewpt_c: 'Dew point', lb_humidity: 'Humidity', lb_pressure_hpa: 'Pressure', lb_wind_kmh: 'Wind', lb_gust_kmh: 'Gust',
  lb_wind_dir: 'Wind direction', lb_rain_mm: 'Rain, last hour', lb_rain_daily_mm: 'Rain today', lb_uv: 'UV index', lb_solar_wm2: 'Sun',
  lb_wind_avg2m_kmh: 'Wind, 2-min average', lb_wind_dir_avg2m: 'Wind direction, 2-min average', lb_gust_10m_kmh: 'Gust, 10 min',
  lb_wind_avg10m_kmh: 'Wind, 10-min average', lb_indoor_temp_c: 'Indoor temperature', lb_indoor_humidity: 'Indoor humidity',
  lb_windchill_c: 'Wind chill', lb_heatindex_c: 'Heat index', lb_pressure_abs_hpa: 'Absolute pressure', lb_rain_weekly_mm: 'Rain this week',
  lb_rain_monthly_mm: 'Rain this month', lb_rain_yearly_mm: 'Rain this year', lb_battery_low: 'Battery low', lb_gust_dir: 'Gust direction',
  lb_gust_dir_10m: 'Gust direction, 10 min', lb_visibility_km: 'Visibility', lb_rain_rate_mmh: 'Rain rate',
  // the rows of Details
  hd_other: 'other readings', hd_uploads: 'Uploads', hd_conn: 'Connection', hd_ap: 'direct, fauxcloud Wi-Fi', hd_sta: 'through your router',
  hd_addr: 'Address', hd_rej: 'Rejected: {k}', hd_rej_l: 'Rejected readings', hd_none: 'none', hd_extras: 'Other data from the console',
  hd_derived: 'Computed by fauxcloud', hd_bridge: 'Bridge', hd_fw: 'Firmware', hd_uptime: 'Uptime', hd_wifi: 'Home Wi-Fi',
  hd_joined: 'joined, {rssi} dBm', hd_not_joined: 'not joined', hd_sta_ip: 'Address on home network', hd_ap_n: 'Consoles on fauxcloud Wi-Fi',
  hd_mem: 'Memory free / lowest', hd_services: 'Services', hd_up_rx: 'Uploads received', hd_mqtt: 'MQTT sent / failed',
});
// ------------------------------------------------------------------ reference data
// core fields (fields.cpp) with the dashboard tile's icon and measure hue
const FIELDS = [['temp_c', WA.fl_temp_c, '°C', 'thermometer', 'temp'], ['dewpt_c', WA.fl_dewpt_c, '°C', 'thermometer', 'temp'], ['humidity', WA.fl_humidity, '%', 'droplet', 'hum'],
  ['pressure_hpa', WA.fl_pressure_hpa, 'hPa', 'gauge', 'baro'], ['wind_kmh', WA.fl_wind_kmh, 'km/h', 'wind', 'wind'], ['gust_kmh', WA.fl_gust_kmh, 'km/h', 'wind', 'wind'],
  ['wind_dir', WA.fl_wind_dir, '°', 'wind', 'wind'], ['rain_mm', WA.fl_rain_mm, 'mm', 'cloud-rain', 'rain'], ['rain_daily_mm', WA.fl_rain_daily_mm, 'mm', 'cloud-rain', 'rain'],
  ['uv', WA.fl_uv, '', 'sun', 'sunc'], ['solar_wm2', WA.fl_solar_wm2, 'W/m²', 'sun', 'sunc']];
// the 7-in-1 fingerprint of the last upload (src/config.h stationFingerprint)
const FP = { match: WA.fp_match, unknown: WA.fp_unknown,
  mismatch: WA.fp_mismatch };
const RULES = [['parse', WA.rl_parse, WA.rl_parse_d],
  ['bounds', WA.rl_bounds, WA.rl_bounds_d],
  ['profile_wind', WA.rl_profile_wind, WA.rl_profile_wind_d],
  ['profile_humidity', WA.rl_profile_humidity, WA.rl_profile_humidity_d],
  ['limits', WA.rl_limits, WA.rl_limits_d],
  ['jump', WA.rl_jump, WA.rl_jump_d],
  ['rain_glitch', WA.rl_rain_glitch, WA.rl_rain_glitch_d]];
const CALIB = [['temp_c_offset', WA.cl_temp_c_offset, '°C', -20, 20, 0], ['humidity_offset', WA.cl_humidity_offset, '%', -50, 50, 0],
  ['pressure_hpa_offset', WA.cl_pressure_hpa_offset, 'hPa', -100, 100, 0], ['wind_dir_offset', WA.cl_wind_dir_offset, '°', -360, 360, 0],
  ['rain_factor', WA.cl_rain_factor, '×', .1, 10, 1], ['wind_factor', WA.cl_wind_factor, '×', .1, 10, 1]];
// promotion types: one choice fills the technical fields (the firmware table, config.cpp PROMO_TYPES)
const TYPES = [
  { id: 'temperature', label: WA.ty_temperature, unit: '°C', conv: 'FtoC', dc: 'temperature', sc: 'measurement', group: 'other', icon: 'thermometer', span: 5, dec: 1 },
  { id: 'soil_temperature', label: WA.ty_soil_temperature, unit: '°C', conv: 'FtoC', dc: 'temperature', sc: 'measurement', group: 'garden', icon: 'thermometer', span: 5, dec: 1 },
  { id: 'humidity', label: WA.ty_humidity, unit: '%', conv: 'none', dc: 'humidity', sc: 'measurement', group: 'other', icon: 'droplet', span: 20, max: 100, dec: 0 },
  { id: 'soil_moisture', label: WA.ty_soil_moisture, unit: '%', conv: 'none', dc: 'moisture', sc: 'measurement', group: 'garden', icon: 'sprout', span: 10, max: 100, dec: 0 },
  { id: 'leaf_wetness', label: WA.ty_leaf_wetness, unit: '%', conv: 'none', dc: 'moisture', sc: 'measurement', group: 'garden', icon: 'leaf', span: 20, max: 100, dec: 0 },
  { id: 'pressure', label: WA.ty_pressure, unit: 'hPa', conv: 'InHgToHpa', dc: 'atmospheric_pressure', sc: 'measurement', group: 'other', icon: 'gauge', span: 10, dec: 1 },
  { id: 'wind_speed', label: WA.ty_wind_speed, unit: 'km/h', conv: 'MphToKmh', dc: 'wind_speed', sc: 'measurement', group: 'other', icon: 'wind', span: 20, dec: 1 },
  { id: 'rain', label: WA.ty_rain, unit: 'mm', conv: 'InToMm', dc: 'precipitation', sc: 'measurement', group: 'other', icon: 'cloud-rain', span: 1, dec: 1 },
  { id: 'pm25', label: 'PM2.5', unit: 'µg/m³', conv: 'none', dc: 'pm25', sc: 'measurement', group: 'air', icon: 'haze', span: 20, dec: 0 },
  { id: 'pm10', label: 'PM10', unit: 'µg/m³', conv: 'none', dc: 'pm10', sc: 'measurement', group: 'air', icon: 'haze', span: 20, dec: 0 },
  { id: 'co2', label: 'CO₂', unit: 'ppm', conv: 'none', dc: 'carbon_dioxide', sc: 'measurement', group: 'air', icon: 'haze', span: 200, dec: 0 },
  { id: 'uv', label: WA.ty_uv, unit: 'UV', conv: 'none', dc: '', sc: 'measurement', group: 'other', icon: 'sun', span: 2, dec: 1 },
  { id: 'illuminance', label: WA.ty_illuminance, unit: 'lx', conv: 'none', dc: 'illuminance', sc: 'measurement', group: 'other', icon: 'sun', span: 1000, dec: 0 },
  { id: 'solar', label: WA.ty_solar, unit: 'W/m²', conv: 'none', dc: 'irradiance', sc: 'measurement', group: 'other', icon: 'sun', span: 100, dec: 0 },
  { id: 'battery', label: WA.ty_battery, unit: '%', conv: 'none', dc: 'battery', sc: 'measurement', group: 'other', icon: 'battery', span: 20, max: 100, dec: 0 },
  { id: 'voltage', label: WA.ty_voltage, unit: 'V', conv: 'none', dc: 'voltage', sc: 'measurement', group: 'other', icon: 'battery', span: 1, dec: 2 },
  { id: 'other', label: WA.ty_other, unit: '', conv: 'none', dc: '', sc: 'measurement', group: 'other', icon: 'activity', span: 1, dec: 1 }];
// the type a known WU key is: the numbered families of the WU article come pre-filled with unit and conversion
// a type's English label: the Home Assistant key made from it is the same whatever the page's language
const enLabel = ty => FC.en(WA, Object.keys(WA).find(k => /^ty_/.test(k) && WA[k] === ty.label)) || ty.label;
const guessType = p => { p = p.toLowerCase();
  return /^soiltemp\d*f$/.test(p) ? 'soil_temperature' : /^soilmoisture\d*$/.test(p) ? 'soil_moisture' : /^leafwetness\d*$/.test(p) ? 'leaf_wetness'
    : /^temp\d+f$/.test(p) ? 'temperature' : /pm2\.?5/.test(p) ? 'pm25' : /pm10/.test(p) ? 'pm10' : /co2/.test(p) ? 'co2' : /batt/.test(p) ? 'voltage' : 'other'; };
const CONV = { none: x => x, FtoC: x => (x - 32) / 1.8, InHgToHpa: x => x * 33.8639, MphToKmh: x => x * 1.609344, InToMm: x => x * 25.4, scale: (x, s) => x * s };
// what a promotion PUT may carry (the rest is derived from the type by the firmware)
const PROMO_KEYS = ['name', 'type', 'as', 'unit', 'conv', 'scale', 'device_class', 'state_class'];

// ------------------------------------------------------------------ station console: the raw uploads, with Copy
// fauxcloud keeps the last raw uploads of all stations in one small ring, each numbered by its station's upload
// counter. While the Console tab
// is open the page asks only for new ones (GET /api/admin/station/console?station=…&since=…) every 5 s and keeps up
// to 500 lines; a jump in numbers becomes one "not kept" line. ID and PASSWORD are replaced with *** by the firmware
// before it keeps an upload, so a copy is safe to share.
const PAGE = 500;
const hms = t => new Date(t * 1000).toTimeString().slice(0, 8);
function consoleView(id) {
  if (!CON || CON.id !== id) { conStop(); CON = { id, lines: [], last: 0 }; conPoll(); }
  if (!CON.timer && !document.hidden) CON.timer = setInterval(conPoll, 5000);
  return `<div class="row logbar"><span class="sp hint">${WA.con_h}</span><button type="button" class="btn quiet" id="conCopy">${WA.con_copy}</button></div>
    <pre class="console" id="conOut" tabindex="0" aria-label="${FC.t(WA.con_aria, { id: esc(id) })}">${CON.lines.length ? '' : WA.con_wait}</pre>`;
}
function conPoll() {
  const C = CON; if (!C || C.busy) return; C.busy = true;
  api('GET', `/api/admin/station/console?station=${encodeURIComponent(C.id)}&since=${C.last}`).then(j => {
    C.busy = false; if (CON !== C || !j.items) return;
    const first = j.items.length ? j.items[0].seq : 0;
    if (C.last && first > C.last + 1) C.lines.push(FC.t(WA.con_gap, { n: first - C.last - 1 }));
    j.items.forEach(e => { C.lines.push(`${hms(e.t)}  #${e.seq}  ${e.line}`); C.last = e.seq; });
    if (C.lines.length > PAGE) C.lines.splice(0, C.lines.length - PAGE); conDraw();
  }, () => { C.busy = false; });
}
function conDraw() { // stays at the bottom unless you scrolled up to read
  const el = $('#conOut'); if (!CON || !el || !CON.lines.length) return; const atEnd = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  el.textContent = CON.lines.join('\n\n'); if (atEnd) el.scrollTop = el.scrollHeight;
}
document.addEventListener('visibilitychange', () => { if (!CON || !$('#conOut')) return; if (document.hidden) conStop(); else if (!CON.timer) { conPoll(); CON.timer = setInterval(conPoll, 5000); } });
function conCopy() {
  const x = API.stations.find(s => s.id === CON.id), t = `fauxcloud ${API.status.fw_version}, ${x.id} (${x.cfg.profile})\n` + CON.lines.join('\n') + '\n';
  copyText(t, $('#conOut'));
}

// limits / jumps / calib merge per key on the ESP: a removed key goes as null
const objDiff = (base, draft) => { const o = Object.assign({}, draft); Object.keys(base).forEach(k => { if (!(k in draft)) o[k] = null; }); return o; };
const promoBody = p => { const o = {}; Object.entries(p).forEach(([k, v]) => { o[k] = {}; PROMO_KEYS.forEach(x => { if (v[x] != null && v[x] !== '') o[k][x] = v[x]; }); if (o[k].conv !== 'scale') delete o[k].scale; }); return o; };

// "Set north from the vane" -- the user holds the vane pointing north; the next reading's direction (already
// corrected by the saved offset) gives the new offset, in (-180, 180]; it fills the field, and Save stores it as any other field
const northOffset = (saved, dir) => { const r = (((saved - dir) % 360) + 360) % 360; return r > 180 ? r - 360 : r; };
// two new readings agree when they are within 11° of each other (one of the 16 compass points)
const northAgree = (a, b) => Math.abs(((a - b) % 360 + 540) % 360 - 180) <= 11;

  // ---- station: tabs General / Filters / Extra sensors / Calibration / Console (+ backup)
SHEETS.station = function (key) {
  const i = +key.slice(2), x = API.stations[i], base = clone(x.cfg), draft = clone(x.cfg);
  // pe: promotion being added/edited; forget: the confirm view ('forget' / 'block'; 'adopt', opened from the home line)
  let tab = 'general', pe = null, forget = API.adoptAsk && x.adopt ? 'adopt' : false;
  API.adoptAsk = false;
  // The north flow. The baseline is the first answer after the press (the sheet's own copy may be
  // minutes old); then it waits for new uploads until two in a row agree within 11° (the first one may carry a reading from
  // before the hold). One GET at a time, every 5 s, none while the tab is hidden; 3 minutes at most.
  let nv = null;  // { st: 'wait' | 'got' | 'none', up, last, off0, t0, d, o, timer }
  const nStop = () => { if (nv && nv.timer) clearTimeout(nv.timer); };
  const nStep = () => {
    if (!nv || nv.st !== 'wait') return;
    if (!$('#nview')) { nStop(); nv = null; return; }
    if (Date.now() - nv.t0 > 180e3) { nv.st = 'none'; draw(); return; }
    const again = () => { if (nv && nv.st === 'wait') nv.timer = setTimeout(nStep, 5000); };
    if (document.hidden) { again(); return; }
    FC.get('/api/live?station=' + encodeURIComponent(x.id), { etag: false }).then(r => { const s = r.data.station;
      if (!nv || nv.st !== 'wait') return;
      if (s && nv.up == null) nv.up = s.uploads;  // the baseline: the first answer after the press
      else if (s && s.uploads > nv.up && s.values && s.values.wind_dir != null) {
        nv.up = s.uploads;
        const d = Math.round(s.values.wind_dir);
        if (nv.last != null && northAgree(nv.last, d)) { nv.st = 'got'; nv.d = d; nv.o = northOffset(nv.off0, d); draw(); return; }
        nv.last = d;
      }
      again(); }, again);
  };
  const nStart = () => { nStop(); nv = { st: 'wait', up: null, last: null, off0: (x.cfg.calib && x.cfg.calib.wind_dir_offset) || 0, t0: Date.now() }; nStep(); };
  // only for a station whose console sends a wind direction
  const hasVane = () => x.values && x.values.wind_dir != null;
  const northView = () => !hasVane() ? '' : `<div id="nview">${!nv ? `<div class="row"><button type="button" class="btn" id="nstart">${WA.n_btn}</button></div>`
    : nv.st === 'wait' ? `<p class="hint loading">${WA.n_wait}</p><div class="row"><button type="button" class="btn quiet" id="ncancel">${WA.cancel}</button></div>`
    : nv.st === 'got' ? `<p>${FC.t(WA.n_got, { d: nv.d, o: nv.o })}</p><div class="row"><button type="button" class="btn quiet" id="ncancel">${WA.cancel}</button><button type="button" class="btn primary" id="nset">${WA.n_set}</button></div>`
    : `<p class="hint">${WA.n_none}</p><div class="row"><button type="button" class="btn quiet" id="ncancel">${WA.cancel}</button><button type="button" class="btn" id="nstart">${WA.n_retry}</button></div>`}</div>`;
  const openF = new Set(); // open measurement rows in Filters
  const S = {
    icon: icon('station'), title: forget === 'adopt' ? WA.adopt_title : x.cfg.name || x.id, noSave: forget === 'adopt', base, draft,
    body() {
      const c = draft, E = this.err, lim = (f, m) => c.limits[f + '_' + m] ?? '', jmp = f => c.jumps[f + '_per_min'] ?? '';
      if (forget === 'adopt') return FC.t(WA.adopt_ask, { from: esc(x.id), to: esc(x.adopt) })
        + `<div class="row"><button type="button" class="btn quiet" id="fback">${FC.t(WA.adopt_keep, { from: esc(x.id) })}</button><button type="button" class="btn danger" id="ago">${FC.t(WA.adopt_go, { to: esc(x.adopt) })}</button></div>`;
      if (forget) { const word = x.cfg.name || x.id, blk = forget === 'block';
        // confirm by typing, like the factory reset; the station's name (or its ID when unnamed) is the word
        return `<p class="lead">${FC.t(blk ? WA.block_q : WA.forget_q, { w: esc(word) })}</p>
        <p class="hint">${WA.f_deletes}</p>
        ${blk ? `<p class="hint">${WA.f_block}</p>` : x.online ? `<div class="callout warn">${WA.f_online}</div>` : `<p class="hint">${WA.f_back}</p>`}
        ${field('ftype', FC.t(WA.f_type, { w: esc(word) }), text('ftype', ''))}
        <div class="row"><button type="button" class="btn quiet" id="fback">${WA.keep_it}</button><button type="button" class="btn danger" id="fgo" disabled>${blk ? WA.block_st : WA.forget_st}</button></div>`; }
      const promos = Object.entries(c.promote), unpro = Object.keys(x.extras).filter(p => !c.promote[p] && p !== 'stationtype');
      const tabs = [['general', WA.t_general], ['filters', WA.t_filters], ['extra', WA.t_extra], ['calib', WA.t_calib], ['console', WA.t_console]];
      let h = `<p class="hint">${FC.t(!x.uploads ? WA.st_none : x.online ? WA.st_on : WA.st_off, { id: esc(x.id), ago: ago(x.age_s), n: x.uploads })}${x.mac ? FC.t(WA.st_mac, { mac: esc(x.mac) }) : ''}${x.battery_low ? WA.st_bat : ''}</p>
        <div class="tabs" role="tablist">${tabs.map(([k, l]) => `<button type="button" role="tab" data-tab="${k}" aria-selected="${tab === k}">${l}</button>`).join('')}</div>`;
      if (tab === 'general') h += `<div class="grid2">
        ${field('name', WA.name, text('name', c.name, { ph: x.id, err: E.name }), WA.name_h, E.name)}
        ${field('profile', WA.model, sel('profile', c.profile, [['yt60234', 'Vevor/Youtong 7-in-1'], ['generic', 'Generic WU']]), (c.profile === 'yt60234' ? WA.model_yt : WA.model_gen) + ' ' + (FP[x.fp] || WA.fp_none))}
        ${field('stale', WA.stale, sel('stale', c.stale_s, [[120, WA.stale_2], [240, WA.stale_4], [600, FC.t(WA.stale_n, { n: 10 })], [1800, FC.t(WA.stale_n, { n: 30 })]]), WA.stale_h)}
        <div style="align-self:center">${tog('publish', c.publish, WA.publish, WA.publish_h)}</div></div>
        <details class="adv"><summary>${WA.backup}</summary>
          <div class="row"><button type="button" class="btn quiet" id="export">${WA.export}</button><label class="btn quiet" for="importFile" style="display:inline-flex;align-items:center">${WA.import}</label><input type="file" id="importFile" accept="application/json,.json" hidden></div>
          <p class="hint">${WA.backup_h}</p></details>
        ${x.km ? `<div class="callout warn"><b>${FC.t(WA.km, { n: x.km })}</b>${WA.km_h}<div class="row"><button type="button" class="btn quiet" id="akey">${WA.km_accept}</button></div></div>` : `<p class="hint">${x.key === 'bound' ? WA.key_bound : WA.key_free}</p>`}
        <div class="row forget"><span class="sp hint">${WA.replaced}</span><button type="button" class="btn quiet bad" id="block">${WA.block}</button><button type="button" class="btn quiet bad" id="forget">${WA.forget_this}</button></div>`;
      if (tab === 'filters') {
        // one row per measurement the console sends; a row opens to its limits and the hide switch (no inner scrolling table)
        const sent = FIELDS.filter(([f]) => f in x.values || x.last_rejected.includes(f) || c.disabled.includes(f)), notSent = FIELDS.filter(([f]) => !sent.some(y => y[0] === f));
        const fmt = v => String(v).replace('-', '−'), total = Object.values(x.rej).reduce((a, b) => a + b, 0);
        const summary = (f, u) => { if (c.disabled.includes(f)) return `<span class="rej">${WA.hidden}</span>`;
          const a = c.limits[f + '_min'], b = c.limits[f + '_max'], m = c.jumps[f + '_per_min'], p = [];
          if (a != null || b != null) p.push(FC.t(a != null && b != null ? WA.lim_both : a != null ? WA.lim_min : WA.lim_max, { a: fmt(a), b: fmt(b), u }));
          if (m) p.push(FC.t(WA.lim_jump, { m: fmt(m), u }));
          return p.length ? esc(p.join(', ')) : `<span class="hint">${WA.no_filters}</span>`; };
        h += `<p class="hint">${WA.filters_h}</p>
        <ul class="acc">${sent.map(([f, l, u, ic, hue]) => { const on = openF.has(f), bad = E[f + '_min'] || E[f + '_max'];
          return `<li class="${on ? 'open' : ''}${bad ? ' bad' : ''}"><button type="button" class="acc-h" data-fx="${f}" aria-expanded="${on}"><span class="acc-i" style="color:var(--${hue})">${icon(ic)}</span><span class="acc-l">${l}</span><span class="acc-v">${x.last_rejected.includes(f) ? WA.rej_now : x.values[f] != null ? esc(x.values[f]) + (u ? ' ' + u : '') : '–'}</span><span class="acc-s">${summary(f, u)}</span><span class="acc-c" aria-hidden="true">›</span></button>
            ${on ? `<div class="acc-b"><div class="grid3">${field('lim_' + f + '_min', WA.min, text('lim_' + f + '_min', lim(f, 'min'), { num: 1, unit: u, err: E[f + '_min'] }))}${field('lim_' + f + '_max', WA.max, text('lim_' + f + '_max', lim(f, 'max'), { num: 1, unit: u, err: E[f + '_max'] }))}${field('jmp_' + f, WA.jump, text('jmp_' + f, jmp(f), { num: 1, unit: u }))}</div>
              ${bad ? `<p class="error">${E.$limits}</p>` : `<p class="hint">${WA.empty_h}</p>`}
              ${tog('dis_' + f, c.disabled.includes(f), WA.hide, WA.hide_h)}</div>` : ''}</li>`; }).join('')}</ul>
        ${notSent.length ? `<p class="hint">${FC.t(WA.not_sent, { list: notSent.map(([, l]) => l).join(', ') })}</p>` : ''}
        <ul class="acc"><li class="${openF.has('$rej') ? 'open' : ''}"><button type="button" class="acc-h" data-fx="$rej" aria-expanded="${openF.has('$rej')}"><span class="acc-i" style="color:var(--warn)">${icon('shield')}</span><span class="acc-l">${WA.rej_since}</span><span class="acc-v">${FC.t(WA.rej_n, { n: total })}</span><span class="acc-s"></span><span class="acc-c" aria-hidden="true">›</span></button>
          ${openF.has('$rej') ? `<div class="acc-b"><ul class="rules">${RULES.map(([k, l, d]) => `<li class="${x.rej[k] ? 'hot' : ''}"><span>${l}</span><b>${x.rej[k] || 0}</b><small>${d}</small></li>`).join('')}</ul></div>` : ''}</li></ul>`;
      }
      if (tab === 'extra') h += `<p class="hint">${WA.extra_h}</p>
        ${promos.length ? `<ul class="extras">${promos.map(([p, v]) => `<li>${icon(v.icon || (TYPES.find(t => t.id === v.type) || {}).icon)}<span class="sp"><b>${esc(v.name)}</b><br><small class="hint">${FC.t(WA.from_key, { type: esc((TYPES.find(t => t.id === v.type) || {}).label || WA.custom), key: `<code>${esc(p)}</code>`, seen: seen(p, prev(p, v)) })}</small></span><button type="button" class="link" data-pedit="${esc(p)}">${WA.edit}</button><button type="button" class="link" data-prm="${esc(p)}">${WA.remove}</button></li>`).join('')}</ul>` : ''}
        ${unpro.length ? `<ul class="extras">${unpro.map(p => `<li><code>${esc(p)}</code><span class="sp hint">${seen(p, `<b>${esc(x.extras[p])}</b>`)}</span><button type="button" class="link" data-kforget="${esc(p)}">${WA.forget}</button>${promos.length >= 4 ? `<small class="hint">${WA.four}</small>` : isNaN(parseFloat(x.extras[p])) ? `<small class="hint">${WA.text_vals}</small>` : `<button type="button" class="btn quiet" data-padd="${esc(p)}">${WA.add_sensor}</button>`}</li>`).join('')}</ul>` : promos.length ? '' : `<p class="hint">${WA.none_yet}</p>`}
        ${pe ? promoForm() : ''}`;
      if (tab === 'calib') h += `<div class="callout warn">${WA.calib_warn}</div>
        <div class="grid2">${CALIB.map(([k, l, u, lo, hi, d]) => field('cal_' + k, l, text('cal_' + k, c.calib[k] ?? '', { num: 1, unit: u, ph: String(d), err: E[k] }), FC.t(WA.calib_h, { lo, hi, d }), E[k])).join('')}</div>${northView()}`;
      if (tab === 'console') h += consoleView(x.id);
      return h;
    },
    input(t) { const c = draft, id = t.id;
      if (id === 'name') c.name = t.value;
      else if (id.startsWith('lim_')) { const k = id.slice(4), v = num(t.value); v == null || isNaN(v) ? delete c.limits[k] : c.limits[k] = v; }
      else if (id.startsWith('jmp_')) { const k = id.slice(4) + '_per_min', v = num(t.value); v == null || isNaN(v) || v === 0 ? delete c.jumps[k] : c.jumps[k] = v; }
      else if (id.startsWith('cal_')) { const k = id.slice(4), v = num(t.value); v == null || isNaN(v) ? delete c.calib[k] : c.calib[k] = v; }
      else if (id === 'ftype') { $('#fgo').disabled = t.value.trim() !== (x.cfg.name || x.id); return; }
      else if (pe && id === 'pname') { pe.v.name = t.value; if (!pe.keyTouched) { pe.v.as = slug(t.value); const k = $('#pkey'); if (k) k.value = pe.v.as; } return; }
      else if (pe && id === 'pkey') { pe.v.as = t.value; pe.keyTouched = true; return; }
      else if (pe && id === 'punit') { pe.v.unit = t.value; return; }
      else if (pe && id === 'pdc') { pe.v.device_class = t.value; return; }
      else if (pe && id === 'pscale') { pe.v.scale = num(t.value) || 1; draw('pscale'); return; }
      sync(); },
    change(t) { const c = draft;
      if (t.id === 'profile') { c.profile = t.value; draw('profile'); return; }
      if (t.id === 'stale') { c.stale_s = +t.value; sync(); return; }
      if (t.id === 'publish') { c.publish = t.checked; sync(); return; }
      if (t.id && t.id.startsWith('dis_')) { const f = t.id.slice(4); c.disabled = t.checked ? [...new Set([...c.disabled, f])] : c.disabled.filter(y => y !== f); draw('dis_' + f); return; }
      if (t.id === 'ptype') { const ty = TYPES.find(y => y.id === t.value), v = pe.v;
        Object.assign(v, { type: ty.id, unit: ty.unit, conv: ty.conv, device_class: ty.dc, state_class: ty.sc, group: ty.group, icon: ty.icon, span: ty.span, max: ty.max, dec: ty.dec });
        if (!v.name || TYPES.some(y => y.label === v.name || enLabel(y) === v.name)) { v.name = ty.label; if (!pe.keyTouched) v.as = slug(enLabel(ty)); } draw('ptype'); return; }
      if (t.id === 'pconv') { pe.v.conv = t.value; draw('pconv'); return; }
      if (t.id === 'importFile' && t.files[0]) t.files[0].text().then(s => { let o; try { o = JSON.parse(s); } catch (e) { o = null; }
        if (!o || typeof o !== 'object' || Array.isArray(o)) { this.msg = `<span class="error">${WA.e_file}</span>`; draw(); this.msg = ''; return; }
        Object.keys(base).forEach(k => { if (k in o) draft[k] = clone(o[k]); }); this.msg = WA.imported; draw(); this.msg = ''; });
    },
    click(b) { const c = draft;
      if (b.id === 'nstart') { nStart(); draw(); return; }
      if (b.id === 'ncancel') { nStop(); nv = null; draw(); return; }
      if (b.id === 'nset') { c.calib.wind_dir_offset = nv.o; nStop(); nv = null; sync(); draw(); return; }
      if (b.dataset.tab) { tab = b.dataset.tab; draw(); return; }
      if (b.id === 'forget' || b.id === 'block' || b.id === 'fback') { forget = b.id === 'fback' ? false : b.id; SH.noSave = !!forget; SH.title = forget ? (forget === 'block' ? WA.title_block : WA.title_forget) : x.cfg.name || x.id; draw(forget ? 'ftype' : 'forget'); return; }
      if (b.id === 'ago') { b.disabled = true; api('POST', '/api/admin/station/adopt', { from: x.id, to: x.adopt }).then(j => {
        if (!j.ok) { SH.msg = `<span class="error">${esc(errs(j) || WA.not_done)}</span>`; draw(); SH.msg = ''; return; }
        // the OTA pattern -- a message that stays in the sheet until the page reloads
        SH.draft = null; SH.msg = WA.adopt_done; draw(); setTimeout(() => location.reload(), 15000); }, noAnswer(b)); return; }
      if (b.id === 'fgo') { const blk = forget === 'block'; b.disabled = true; api('POST', `/api/admin/station/${blk ? 'block' : 'forget'}?station=` + encodeURIComponent(x.id), {}).then(j => {
        if (!j.ok) { SH.msg = `<span class="error">${esc(errs(j) || WA.not_done)}</span>`; draw(); SH.msg = ''; return; }
        SH.draft = null; closeSheet(); toast(FC.t(blk ? WA.blocked : WA.forgot, { name: x.cfg.name || x.id })); setTimeout(refresh, 1500); }, noAnswer(b)); return; }
      if (b.id === 'akey') { b.disabled = true; api('POST', '/api/admin/station/accept-key?station=' + encodeURIComponent(x.id), {}).then(j => {
        if (j.ok) { x.km = 0; x.key = 'none'; toast(WA.key_toast); } draw(); }, noAnswer(b)); return; }
      if (b.dataset.fx) { const k = b.dataset.fx; openF.has(k) ? openF.delete(k) : openF.add(k); draw(); return; }
      if (b.dataset.padd) { const p = b.dataset.padd, ty = TYPES.find(y => y.id === guessType(p));
        pe = { param: p, isNew: true, v: { name: ty.label, type: ty.id, as: slug(enLabel(ty)), unit: ty.unit, conv: ty.conv, scale: 1, device_class: ty.dc, state_class: ty.sc, group: ty.group, icon: ty.icon, span: ty.span, max: ty.max, dec: ty.dec } }; draw('ptype'); return; }
      if (b.dataset.pedit) { pe = { param: b.dataset.pedit, keyTouched: true, v: clone(c.promote[b.dataset.pedit]) }; draw('ptype'); return; }
      if (b.dataset.prm) { delete c.promote[b.dataset.prm]; draw(); return; }
      if (b.dataset.kforget) { const p = b.dataset.kforget; b.disabled = true;  // gone until the console sends it again
        api('POST', '/api/admin/station/forget-key?station=' + encodeURIComponent(x.id) + '&key=' + encodeURIComponent(p), {}).then(j => {
          if (j.ok) { delete x.extras[p]; toast(FC.t(WA.forgot_key, { p })); } draw(); }, noAnswer(b)); return; }
      if (b.id === 'pcancel') { pe = null; this.err = {}; draw(); return; }
      if (b.id === 'pok') { const v = pe.v, E = {};
        if (!v.name.trim()) E.pname = WA.e_name;
        else if (new TextEncoder().encode(v.name).length > 31) E.pname = WA.e_long31;
        if (!/^[a-z0-9_]{1,23}$/.test(v.as)) E.pkey = WA.e_key;
        else if (FIELDS.some(f => f[0] === v.as) || ['last_seen', 'availability'].includes(v.as) || v.as.startsWith('rej_')) E.pkey = WA.e_taken;
        else if (Object.entries(c.promote).some(([p, o]) => p !== pe.param && o.as === v.as)) E.pkey = WA.e_used;
        this.err = E; if (Object.keys(E).length) { draw(); if (E.pkey) $('#promoForm details').open = true; return; }
        c.promote[pe.param] = v; pe = null; draw(); return; }
      if (b.id === 'export') { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([JSON.stringify(c, null, 2)], { type: 'application/json' })); a.download = `fauxcloud-${x.id}-${slug(c.name || x.id)}.json`; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 1000); }
    },
    validate() { const c = draft, E = {};
      FIELDS.forEach(([f, l]) => { const a = c.limits[f + '_min'], b = c.limits[f + '_max']; if (a != null && b != null && a > b) { E[f + '_min'] = E[f + '_max'] = ' '; E.$limits = FC.t(WA.e_minmax, { l }); openF.add(f); } });
      CALIB.forEach(([k, l, u, lo, hi]) => { const v = c.calib[k]; if (v != null && (v < lo || v > hi)) E[k] = FC.t(WA.e_range, { lo, hi }); });
      if (new TextEncoder().encode(c.name).length > 47) E.name = WA.e_long47;
      if (E.$limits) tab = 'filters'; else if (E.name) tab = 'general'; else if (Object.keys(E).length) tab = 'calib';
      return E; },
    save(body) { // PUT /api/admin/station?station=… with only the changed keys
      ['limits', 'jumps', 'calib'].forEach(k => { if (k in body) body[k] = objDiff(base[k], body[k]); });
      if ('promote' in body) body.promote = promoBody(body.promote);
      if ('name' in body && !body.name) body.name = null;
      return api('PUT', '/api/admin/station?station=' + encodeURIComponent(x.id), body); },
  };
  // "now 41 %" from the latest upload, else "41 %, 6 min ago" (a key the console sends rarely stays listed)
  function seen(p, val) { const a = x.kage && x.kage[p]; return a == null || a < 60 ? FC.t(WA.seen_now, { v: val }) : FC.t(WA.seen_ago, { v: val, ago: ago(a) }); }
  function prev(p, v) { const raw = parseFloat(x.extras[p]); if (isNaN(raw)) return `<b>${esc(x.extras[p] ?? '–')}</b>`; return `<b>${(CONV[v.conv] || CONV.none)(raw, v.scale || 1).toFixed(v.dec ?? 1)} ${esc(v.unit)}</b>`; }
  function promoForm() { const p = pe.param, v = pe.v, E = SH.err;
    return `<div class="promo" id="promoForm"><h3>${FC.t(pe.isNew ? WA.p_add : WA.p_edit, { p: esc(p) })}</h3><div class="grid2">
      ${field('ptype', WA.p_type, sel('ptype', v.type, TYPES.map(y => [y.id, y.label])), WA.p_type_h)}
      ${field('pname', WA.name, text('pname', v.name, { ph: WA.p_ph, err: E.pname }), WA.p_name_h, E.pname)}</div>
      <div class="preview">${FC.t(WA.p_preview, { raw: esc(x.extras[p] ?? '–'), shown: prev(p, v) })} <span class="hint">${WA.p_wrong}</span></div>
      <details class="adv"><summary>${WA.advanced}</summary><div class="grid2">
        ${field('pkey', WA.p_key, text('pkey', v.as, { err: E.pkey }), WA.p_key_h, E.pkey)}
        ${field('punit', WA.unit, text('punit', v.unit))}
        ${field('pconv', WA.conv, sel('pconv', v.conv, [['none', WA.c_none], ['FtoC', '°F → °C'], ['InHgToHpa', 'inHg → hPa'], ['MphToKmh', 'mph → km/h'], ['InToMm', 'in → mm'], ['scale', WA.c_scale]]))}
        ${v.conv === 'scale' ? field('pscale', WA.scale, text('pscale', v.scale ?? 1, { num: 1 })) : ''}
        ${field('pdc', WA.p_dc, text('pdc', v.device_class, { ph: 'none' }))}</div></details>
      <div class="row end"><button type="button" class="btn quiet" id="pcancel">${WA.cancel}</button><button type="button" class="btn primary" id="pok">${pe.isNew ? WA.p_ok_add : WA.done}</button></div>
      <p class="hint">${WA.p_save}</p></div>`; }
  return S;
};

// ------------------------------------------------------------------ Publishing (lazy): Home Assistant, who may upload
SHEETS.publish = function () {
  // the board's ceiling; at 1 the “Stations kept” field is not shown and max_stations is never sent
  const b = API.bridge, cap = b.max_stations_cap || 2, base = Object.assign({ auto_publish_new_stations: b.auto_publish_new_stations, mqtt_interval_last_seen_s: b.mqtt_interval_last_seen_s }, cap > 1 ? { max_stations: b.max_stations } : {}, { lan_uploads: !!b.lan_uploads }), draft = clone(base);
  const bl = { ids: null };  // blocked station IDs, from the bridge
  let fq = null;  // the confirm view for a history / saved settings without a station ({id, pk})
  const loadBl = () => api('GET', '/api/admin/station/blocked').then(j => { bl.ids = j.blocked || []; return api('GET', '/api/admin/rings'); }).then(j => { bl.fl = j.rings.filter(r => !r.slot); bl.pk = (j.parked || []).filter(id => !bl.fl.some(r => r.id === id)); if (SH && SH.key === 'publish') draw(); }, () => {});
  loadBl();
  return { icon: icon('activity'), title: WA.pub_title, base, draft,
    body() { const inUse = API.stations.length, fw = !!b.forwarding, off = s => fw ? s.replace(/<(select|input) /, '<$1 disabled ') : s;
      // the question, the item, what is lost, Keep / Forget
      if (fq) return `<p class="lead">${fq.pk ? WA.fq_set_q : WA.fq_hist_q}</p><p class="hint"><code>${esc(fq.id)}</code></p>
        <p class="hint">${fq.pk ? WA.fq_set_h : WA.fq_hist_h}</p>
        <div class="row"><button type="button" class="btn quiet" data-fq="no">${WA.keep}</button><button type="button" class="btn danger" data-fq="go">${WA.forget}</button></div>`;
      const why = WA.pub_why;  // greyed with one line why
      return `${tog('autopub', draft.auto_publish_new_stations, WA.pub_auto, WA.pub_auto_h)}
      ${field('lsi', WA.pub_lsi, sel('lsi', draft.mqtt_interval_last_seen_s, [[60, WA.lsi_60], [300, WA.lsi_300], [900, WA.lsi_900]]), WA.pub_lsi_h)}
      ${cap > 1 ? field('maxst', WA.pub_maxst, off(sel('maxst', draft.max_stations, Array.from({ length: cap }, (_, i) => i + 1).filter(k => k >= inUse).map(k => [k, k]))), fw ? why : FC.t(WA.pub_maxst_h, { n: inUse })) : ''}
      <h3>${WA.pub_who}</h3>
      ${off(tog('lanup', draft.lan_uploads, WA.pub_lan, fw ? why : WA.pub_lan_h))}
      <p class="hint">${bl.ids === null ? WA.bl_loading : bl.ids.length ? WA.bl_some + ' ' : WA.bl_none}</p>
      ${bl.ids && bl.ids.length ? `<ul class="extras">${bl.ids.map(id => `<li><code>${esc(id)}</code><span class="sp"></span><button type="button" class="btn quiet" data-unblock="${esc(id)}">${WA.unblock}</button></li>`).join('')}</ul>` : ''}
      ${bl.fl && bl.fl.length ? `<p class="hint">${WA.pub_fl}</p><ul class="extras">${bl.fl.map(r => `<li><code>${esc(r.id)}</code><span class="sp hint">${FC.t(WA.pub_fl_n, { h: r.hours, n: r.days })}</span><button type="button" class="btn quiet bad" data-flf="${esc(r.id)}">${WA.forget}</button></li>`).join('')}</ul>` : ''}
      ${bl.pk && bl.pk.length ? `<p class="hint">${WA.pub_pk}</p><ul class="extras">${bl.pk.map(id => `<li><code>${esc(id)}</code><span class="sp"></span><button type="button" class="btn quiet bad" data-flf="${esc(id)}" data-pk="1">${WA.forget}</button></li>`).join('')}</ul>` : ''}`; },
    change(t) { if (t.id === 'autopub') draft.auto_publish_new_stations = t.checked; if (t.id === 'lanup') draft.lan_uploads = t.checked; if (t.id === 'lsi') draft.mqtt_interval_last_seen_s = +t.value; if (t.id === 'maxst') { draft.max_stations = +t.value; draw('maxst'); return; } sync(); },
    click(btn) { const d = btn.dataset, q = fq;
      // data destroyed -> the confirm view (the question, the item, Keep / Forget)
      if (d.flf) { fq = { id: d.flf, pk: d.pk }; SH.noSave = true; return void draw(); }
      if (d.fq) { fq = null; SH.noSave = false; draw(); }
      const id = d.unblock || d.fq === 'go' && q.id;
      if (id) { btn.disabled = true; api('POST', `/api/admin/station/${d.unblock ? 'unblock' : 'forget'}?station=` + encodeURIComponent(id), {}).then(() => { toast(FC.t(d.unblock ? WA.unblocked : WA.forgot_rec, { name: id })); setTimeout(loadBl, 1500); }, noAnswer(btn)); } },
    save(body) { return api('PUT', '/api/admin/bridge', body); } };
};
// The field keys as a person reads them in Details (no raw key on the page)
const LBL = { temp_c: WA.lb_temp_c, dewpt_c: WA.lb_dewpt_c, humidity: WA.lb_humidity, pressure_hpa: WA.lb_pressure_hpa, wind_kmh: WA.lb_wind_kmh, gust_kmh: WA.lb_gust_kmh,
  wind_dir: WA.lb_wind_dir, rain_mm: WA.lb_rain_mm, rain_daily_mm: WA.lb_rain_daily_mm, uv: WA.lb_uv, solar_wm2: WA.lb_solar_wm2,
  wind_avg2m_kmh: WA.lb_wind_avg2m_kmh, wind_dir_avg2m: WA.lb_wind_dir_avg2m, gust_10m_kmh: WA.lb_gust_10m_kmh,
  wind_avg10m_kmh: WA.lb_wind_avg10m_kmh,
  // every firmware field has a label (test_table_sync.py), so none reaches Details as a raw key
  indoor_temp_c: WA.lb_indoor_temp_c, indoor_humidity: WA.lb_indoor_humidity, windchill_c: WA.lb_windchill_c, heatindex_c: WA.lb_heatindex_c,
  pressure_abs_hpa: WA.lb_pressure_abs_hpa, rain_weekly_mm: WA.lb_rain_weekly_mm, rain_monthly_mm: WA.lb_rain_monthly_mm, rain_yearly_mm: WA.lb_rain_yearly_mm,
  battery_low: WA.lb_battery_low, gust_dir: WA.lb_gust_dir, gust_dir_10m: WA.lb_gust_dir_10m, visibility_km: WA.lb_visibility_km,
  rain_rate_mmh: WA.lb_rain_rate_mmh };
// Details = the bridge always, the station only when there is one (the empty state shows the bridge too)
// The Details sit in Settings under the status row: the bridge, then every station
function healthDetails(live) {
  const d = live.diag, kb = b => Math.round(b / 1024) + ' KB';
  const st = x => {
    const hid = new Set(x.hidden || []), rej0 = Object.entries(x.rejects_since_boot || {}).filter(([k]) => !hid.has(k)), ex = Object.entries(x.extras || {});
    // keys without a label sum into one row
    const other = rej0.filter(([k]) => !own(LBL, k)).reduce((a, [, n]) => a + n, 0);
    const rej = rej0.filter(([k]) => own(LBL, k)).map(([k, n]) => [LBL[k], n]).concat(other ? [[WA.hd_other, other]] : []), der = Object.keys(x.derived || {});
    return `<div><h3>${esc(x.name || x.id)} (${esc(x.id)})</h3><dl><dt>${WA.hd_uploads}</dt><dd>${x.uploads}</dd><dt>${WA.hd_conn}</dt><dd>${x.src_if === 'ap' ? WA.hd_ap : WA.hd_sta}</dd>${x.src_ip ? `<dt>${WA.hd_addr}</dt><dd>${esc(x.src_ip)}</dd>` : ''}
      ${rej.length ? rej.map(([k, n]) => `<dt>${FC.t(WA.hd_rej, { k: esc(k) })}</dt><dd>${n}</dd>`).join('') : `<dt>${WA.hd_rej_l}</dt><dd>${WA.hd_none}</dd>`}
      <dt>${WA.hd_extras}</dt><dd>${ex.length ? ex.map(([k, v]) => esc(k + '=' + v)).join(', ') : WA.hd_none}</dd>
      ${der.length ? `<dt>${WA.hd_derived}</dt><dd>${der.map(k => esc(own(LBL, k) ? LBL[k] : k)).join(', ')}</dd>` : ''}</dl></div>`;
  };
  return (d ? `<div><h3>${WA.hd_bridge}</h3><dl><dt>${WA.hd_fw}</dt><dd>${esc(d.fw_version || '')}</dd><dt>${WA.hd_uptime}</dt><dd>${dur(live.uptime_s || 0)}</dd><dt>${WA.hd_wifi}</dt><dd>${d.sta_up ? FC.t(WA.hd_joined, { rssi: String(d.sta_rssi).replace('-', '\u2212') }) : WA.hd_not_joined}</dd><dt>${WA.hd_sta_ip}</dt><dd>${d.sta_up ? esc(d.sta_ip) : '\u2013'}</dd><dt>${WA.hd_ap_n}</dt><dd>${d.ap_clients}</dd><dt>${WA.hd_mem}</dt><dd>${kb(d.heap_free)} / ${kb(d.heap_min)}</dd></dl></div>
    <div><h3>${WA.hd_services}</h3><dl><dt>${WA.hd_up_rx}</dt><dd>${d.uploads}</dd><dt>${WA.hd_mqtt}</dt><dd>${d.mqtt_published} / ${d.mqtt_failed}</dd></dl></div>` : '')
    + (live.stations || []).map(st).join('');
}
