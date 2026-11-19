// test/tools/tendency_vectors.js -- runs firmware/fauxcloud/web/tendency.js (the dashboard's pressure and temperature
// trend) under node against hand-made /api/hourly rows. The firmware's port (src/forecast.cpp) must pass the same
// cases. Usage: node test/tools/tendency_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'tendency.js'), 'utf8');
const FCT = new Function('FC', src + '\nreturn FCT;')({ w: (f, t) => t });  // FC.w: the English table as it is

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

// hourly rows as the API sends them: h = the hour's start (UTC), [min, mean, max]
const H = (iso, p, k = 'pressure_hpa') => ({ h: iso, n: 12, [k]: [p - .2, p, p + .2] });
const rows = (start, means, k) => means.map((p, i) => p == null ? null
  : H(new Date(Date.parse(start) + i * 36e5).toISOString().slice(0, 16) + 'Z', p, k)).filter(Boolean);
const pick = r => r && { d: r.d, code: r.code, fast: r.fast };
const P = (means, loc = null, start = '2026-11-08T00:00Z') => pick(FCT.pressure(rows(start, means), loc));

// pressure, no location (tide 0)
check('steady +0.5', P([1013, 1013.2, 1013.4, 1013.5]), { d: .5, code: 'steady', fast: false });
check('rising +1.7', P([1010, 1010.5, 1011, 1011.7]), { d: 1.7, code: 'rising', fast: false });
check('boundary +1.54 -> steady', P([1000, 1000, 1000, 1001.54]), { d: 1.5, code: 'steady', fast: false });
// +1.55 shows as "+1.6" on the tile, so it is rising (the shown value never contradicts its word)
check('boundary +1.55 -> rising (shown +1.6)', P([1000, 1000, 1000, 1001.55]), { d: 1.6, code: 'rising', fast: false });
check('boundary +1.6 -> rising', P([1000, 1000, 1000, 1001.6]), { d: 1.6, code: 'rising', fast: false });
check('boundary -1.55 -> falling', P([1000, 1000, 1000, 998.45]), { d: -1.6, code: 'falling', fast: false });
check('-3.6 -> falling fast', P([1000, 999, 998, 996.4]), { d: -3.6, code: 'falling', fast: true });
check('+3.5 -> rising, not fast', P([1000, 1001, 1002, 1003.5]), { d: 3.5, code: 'rising', fast: false });
check('no location -> tide false', FCT.pressure(rows('2026-11-08T00:00Z', [1000, 1000, 1000, 1001])).tide, false);
// index gap: the hour 3 h before the newest is missing; by index the 4th-last row would be 4 h back
check('missing hour -> null', P([1000, null, 1002, 1003, 1004]), null);
check('fewer than 4 hours -> null', P([1000, 1001, 1002]), null);
check('empty -> null', FCT.pressure([], null), null);
// the open hour is never used, even when it is in the list
const op = rows('2026-11-08T00:00Z', [1000, 1000, 1000, 1001]).concat([Object.assign(H('2026-11-08T04:00Z', 1010), { open: true })]);
check('open row ignored', pick(FCT.pressure(op, null)), { d: 1, code: 'steady', fast: false });
// a row without a pressure mean is skipped, the newest row with one decides
const nm = rows('2026-11-08T00:00Z', [1000, 1000, 1000, 1001]).concat([{ h: '2026-11-08T04:00Z', n: 3 }]);
check('row without pressure skipped', pick(FCT.pressure(nm, null)), { d: 1, code: 'steady', fast: false });

// tide flip at a generic point (lat 52, lon 20): raw -1.7 between 10:00 and 13:00 UTC; expected tide part from the
// forecast_trial.py formula, evaluated at mid-hour, written out here independently
const LOC = { lat: 52, lon: 20 };
const s2 = (iso, lat, lon) => { const t = new Date(Date.parse(iso) + 18e5), sh = t.getUTCHours() + t.getUTCMinutes() / 60 + lon / 15;
  return 1.16 * Math.cos(lat * Math.PI / 180) ** 3 * Math.cos(2 * Math.PI * (sh - 10) / 12); };
const tp = s2('2026-11-08T13:00Z', 52, 20) - s2('2026-11-08T10:00Z', 52, 20);
check('tide part at 13:00 vs 10:00 is <= -0.15', tp <= -.15, true);
const tf = FCT.pressure(rows('2026-11-08T10:00Z', [1001.7, 1001.2, 1000.6, 1000]), LOC);
check('tide flip: raw -1.7 -> steady', { d: tf.d, code: tf.code, tide: tf.tide }, { d: Math.round((-1.7 - tp) * 10) / 10, code: 'steady', tide: true });
check('tide flip: raw kept', Math.round(tf.raw * 100) / 100, -1.7);
check('same rows, no location -> falling', P([1001.7, 1001.2, 1000.6, 1000], null, '2026-11-08T10:00Z'), { d: -1.7, code: 'falling', fast: false });

// an inHg console: its means are stored in hPa (30.00 -> 30.06 inHg x 33.8639); judged in hPa it is rising, the tile
// shows +0.06 inHg. The regression: the same rows judged in inHg against the hPa thresholds say "steady".
const inHgRows = [30.00, 30.02, 30.04, 30.06];
const ri = FCT.pressure(rows('2026-11-08T00:00Z', inHgRows.map(x => x * 33.8639)), null);
check('inHg console: +2.0 hPa rising', { d: ri.d, code: ri.code }, { d: 2, code: 'rising' });
check('inHg console: shown +0.06 inHg', (Math.round(ri.d * 0.02953 * 100) / 100).toFixed(2), '0.06');
const rInHg = FCT.pressure(rows('2026-11-08T00:00Z', inHgRows), null);
check('inHg rows judged in inHg against ±1.6 -> steady (the regression)', rInHg.code, 'steady');

// half a location (lat, no lon) counts as none
check('lat without lon -> no tide', FCT.pressure(rows('2026-11-08T10:00Z', [1001.7, 1001.2, 1000.6, 1000]), { lat: 52 }).tide, false);
// a misused fast threshold below thr never marks a steady trend fast
const mf = FCT.trend(rows('2026-11-08T00:00Z', [1000, 1000.3]), { key: 'pressure_hpa', win: 1, thr: 1.6, fast: .2 });
check('fast < thr: steady is never fast', { code: mf.code, fast: mf.fast }, { code: 'steady', fast: false });

// temperature, 1 h, ±0.5 °C, no tide, no fast
const T = (means, start = '2026-11-08T00:00Z') => pick(FCT.temp(rows(start, means, 'temp_c')));
check('temp +0.44 -> steady', T([5, 5.44]), { d: .4, code: 'steady', fast: false });
check('temp -0.44 -> steady', T([5, 4.56]), { d: -.4, code: 'steady', fast: false });
// ±0.45 shows as "0.5" (one decimal, like every temperature): judged as shown, it is an arrow
check('temp +0.45 -> rising (shown +0.5)', T([5, 5.45]), { d: .5, code: 'rising', fast: false });
check('temp +0.5 -> rising', T([5, 5.5]), { d: .5, code: 'rising', fast: false });
check('temp -0.5 -> falling', T([5, 4.5]), { d: -.5, code: 'falling', fast: false });
check('temp -4 -> falling, never fast', T([5, 1]), { d: -4, code: 'falling', fast: false });
check('temp gap -> null', T([5, null, 6]), null);
check('temp one hour -> null', T([5]), null);

// the words (the phrase table lives in tendency.js, shown in the tile title, aria and the sheet)
check('word steady', FCT.word(FCT.pressure(rows('2026-11-08T00:00Z', [1000, 1000, 1000, 1000.5]), null)), 'steady');
check('word rising', FCT.word(FCT.pressure(rows('2026-11-08T00:00Z', [1000, 1000, 1000, 1001.7]), null)), 'rising');
// the tile's word is the strip's ("Pressure rising fast.")
check('word falling fast', FCT.word(FCT.pressure(rows('2026-11-08T00:00Z', [1000, 999, 998, 996.4]), null)), 'falling fast');
check('word rising fast', FCT.word(FCT.pressure(rows('2026-11-08T00:00Z', [1000, 1001, 1002, 1003.6]), null)), 'rising fast');
check('word temp falling', FCT.word(FCT.temp(rows('2026-11-08T00:00Z', [5, 4.5], 'temp_c'))), 'falling');

// the freshness gate: the newest closed hour must start at most 2 h before `now`; no `now` = no gate
const fr = rows('2026-11-08T00:00Z', [1000, 1000, 1000, 1001.7]);  // newest closed hour 03:00
check('fresh: now 04:59 -> trend', pick(FCT.pressure(fr, null, Date.parse('2026-11-08T04:59Z'))), { d: 1.7, code: 'rising', fast: false });
check('fresh: now 05:00 -> trend (exactly 2 h)', pick(FCT.pressure(fr, null, Date.parse('2026-11-08T05:00Z'))), { d: 1.7, code: 'rising', fast: false });
check('stale: now 05:01 -> null', FCT.pressure(fr, null, Date.parse('2026-11-08T05:01Z')), null);
check('stale temp: now 05:01 -> null', FCT.temp(rows('2026-11-08T02:00Z', [5, 4.5], 'temp_c'), Date.parse('2026-11-08T05:01Z')), null);
check('no now -> no gate', pick(FCT.pressure(fr, null)), { d: 1.7, code: 'rising', fast: false });

// humidity over 3 h, ±5 %RH dead band (the dashboard shows no glyph when steady)
const Hm = means => pick(FCT.hum(rows('2026-11-08T00:00Z', means, 'humidity')));
check('hum +8 over 3 h: rising', Hm([70, 72, 75, 78]), { d: 8, code: 'rising', fast: false });
check('hum -8 over 3 h: falling', (Hm([80, 78, 75, 72]) || {}).code, 'falling');
check('hum +2: steady', (Hm([70, 71, 71, 72]) || {}).code, 'steady');
check('hum exactly +5: rising', (Hm([70, 71, 73, 75]) || {}).code, 'rising');
check('hum: a missing end hour -> null', FCT.hum(rows('2026-11-08T00:00Z', [70, 75], 'humidity')), null);
console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
