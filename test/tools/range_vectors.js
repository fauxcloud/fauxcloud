// test/tools/range_vectors.js -- the history sheet's date ranges, taken out of
// firmware/fauxcloud/web/dash-sheet.js by name (the js_decl.js pattern): the span of a preset or a typed range and its
// resolution (hourly up to 3 days, 6-hour buckets to 7, daily to a year, weekly above), the request it makes, and the
// bucket arithmetic (lowest low, highest high, readings-weighted average, rain summed, strongest gust, speed-weighted
// direction, open flag). Runs in Europe/Paris so a DST day is covered. Usage: node test/tools/range_vectors.js
'use strict';
process.env.TZ = 'Europe/Paris';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { decl } = require('./js_decl');

const WEB = path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web');
const dash = fs.readFileSync(path.join(WEB, 'dash.js'), 'utf8');
const sheet = fs.readFileSync(path.join(WEB, 'dash-sheet.js'), 'utf8');
const health = fs.readFileSync(path.join(WEB, 'health.js'), 'utf8');  // p2, dm

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

const ctx = { measure: 'wind', S: { cur: 'ST 1' }, Math, Date, String, Array, JSON, encodeURIComponent, FC: {} };
vm.createContext(ctx);
vm.runInContext(['p2', 'dm'].map(n => decl(health, n)).concat(['HOUR', 'hh'].map(n => decl(dash, n))).concat(['SPC', 'ymd', 'addD', 'spanOf', 'spanUrl', 'spanSrc', 'spanTicks', 'bucket'].map(n => decl(sheet, n))).join('\n') +
  '\nthis.f = { spanOf, spanUrl, spanSrc, spanTicks, bucket, ymd, addD, SPC };', ctx);
const { spanOf, spanUrl, spanSrc, spanTicks, bucket, ymd, SPC } = ctx.f;
const D = s => new Date(s);  // local time (TZ above)

// 1. Spans: presets end today, From..To inclusive, resolution by length
{
  const now = D('2026-11-13T15:20:00');
  const z7 = spanOf({ p: 7 }, now);
  check('7 d: from 7 Nov', ymd(z7.a), '2026-11-07');
  check('7 d: to today', ymd(z7.b), '2026-11-13');
  check('7 d: 6-hour buckets', [z7.nd, z7.step, z7.today], [7, 6, true]);
  check('7 d: [from, to) in unix s', [z7.from, z7.to], [D('2026-11-07T00:00') / 1e3, D('2026-11-14T00:00') / 1e3]);
  check('3 d typed: hourly', spanOf({ p: 0, a: D('2026-11-01T00:00'), b: D('2026-11-03T00:00') }, now).step, 1);
  check('3 d typed: not today', spanOf({ p: 0, a: D('2026-11-01T00:00'), b: D('2026-11-03T00:00') }, now).today, false);
  check('8 d: daily', spanOf({ p: 0, a: D('2026-11-01T00:00'), b: D('2026-11-08T00:00') }, now).step, 24);
  check('30 d / 90 d / 1 y: daily', [30, 90, 365].map(p => spanOf({ p }, now).step), [24, 24, 24]);
  check('366 d daily, 367 d weekly', [366, 367].map(n => spanOf({ p: 0, a: new Date(2026, 10, 14 - n), b: D('2026-11-13T00:00') }, now)).map(z => [z.nd, z.step]), [[366, 24], [367, 168]]);
  check('All: no start until the answer, weekly first', [spanOf({ p: 'all' }, now).a, spanOf({ p: 'all' }, now).step], [null, 168]);
  // DST: 25 Oct 2026 has 25 hours in Paris; local days still count whole
  const zd = spanOf({ p: 0, a: D('2026-10-24T00:00'), b: D('2026-10-26T00:00') }, now);
  check('DST weekend: 3 days, hourly', [zd.nd, zd.step], [3, 1]);
  check('DST weekend: to = local midnight after To', zd.to, D('2026-10-27T00:00') / 1e3);
}

// 2. Requests: hourly up to 7 days, the 90-day preset reads the sheet's own poll, All the whole flash ring
{
  const now = D('2026-11-13T15:20:00'), u = sp => spanUrl(spanOf(sp, now), sp);
  check('7 d → hourly with from/to', u({ p: 7 }), `/api/hourly?station=ST%201&from=${D('2026-11-07T00:00') / 1e3}&to=${D('2026-11-14T00:00') / 1e3}`);
  check('30 d → daily with from/to', u({ p: 30 }), `/api/daily?station=ST%201&from=${D('2026-10-15T00:00') / 1e3}&to=${D('2026-11-14T00:00') / 1e3}`);
  check('90 d → none (days=90 poll)', u({ p: 90 }), '');
  check('All → all=1', u({ p: 'all' }), '/api/daily?station=ST%201&all=1');
}

// 3. Buckets
{
  const r = (t, o) => Object.assign({ t: D(t), n: 60, rej: 0, open: false, lo: null, mid: null, hi: null, g: null, dir: null, dew: null, uv: null, sol: null }, o);
  // wind: 6-hour buckets at 00/06/12/18 local; weights by readings; direction weighted by speed
  const W = [r('2026-11-12T04:00', { mid: 10, g: 20, dir: 90, lo: 5, hi: 15 }), r('2026-11-12T05:00', { mid: 20, g: 30, dir: 0, n: 30, lo: 8, hi: 25, rej: 1 }),
    r('2026-11-12T06:00', { mid: 0, dir: 180 }), r('2026-11-12T07:00', { mid: 4, g: 9, dir: 270, open: true })];
  const b = bucket(W, 6, false, true);
  check('two buckets', b.length, 2);
  check('bucket start + label', [b[0].t.getHours(), b[0].lab, b[1].lab], [0, '12 Nov 00:00–06:00', '12 Nov 06:00–12:00']);
  check('readings, rejected', [b[0].n, b[0].rej], [90, 1]);
  check('weighted average', Math.round(b[0].mid * 1000) / 1000, Math.round((10 * 60 + 20 * 30) / 90 * 1000) / 1000);
  check('lowest / highest / gust', [b[0].lo, b[0].hi, b[0].g], [5, 25, 30]);
  // speed-weighted: east 10 km/h x 60, north 20 km/h x 30 -> equal weights 600 / 600 -> NE
  check('speed-weighted direction', b[0].dir, 45);
  check('a calm hour adds no direction', b[1].dir, 270);
  check('open flag', [b[0].open, b[1].open], [false, true]);
  // rain: summed, not averaged
  const R = bucket([r('2026-11-12T13:00', { mid: 0.4 }), r('2026-11-12T14:00', { mid: 1.2, n: 10 }), r('2026-11-12T19:00', { mid: null })], 6, true, false);
  check('rain summed', Math.round(R[0].mid * 10) / 10, 1.6);
  check('no reading stays empty', R[1].mid, null);
  // calm bucket: no direction at all
  check('calm bucket', bucket([r('2026-11-12T01:00', { mid: 0, dir: 200 })], 6, false, true)[0].dir, null);
  // other measures: direction by readings, not by the measure's value
  check('temperature: direction by readings', bucket([r('2026-11-12T01:00', { mid: -3, dir: 350 }), r('2026-11-12T02:00', { mid: 5, dir: 10 })], 6, false, false)[0].dir, 0);
  // weeks start on Monday; label first–last day
  const wk = bucket([r('2026-11-08T00:00', { mid: 1 }), r('2026-11-09T00:00', { mid: 3 }), r('2026-11-15T00:00', { mid: 5 }), r('2026-11-16T00:00', { mid: 7 })], 168, false, false);
  check('weeks: Sunday closes the week, Monday opens one', wk.map(x => x.lab), ['2 Nov–8 Nov', '9 Nov–15 Nov', '16 Nov–22 Nov']);
  check('weeks: averages', wk.map(x => x.mid), [1, 4, 7]);
  // across the DST change (25 Oct): the 6-hour slots stay on the local clock
  const dst = bucket([r('2026-10-25T01:00', { mid: 1 }), r('2026-10-25T05:00', { mid: 1 }), r('2026-10-25T06:00', { mid: 1 })], 6, false, false);
  check('DST: local 00-06 then 06-12', dst.map(x => x.lab), ['25 Oct 00:00–06:00', '25 Oct 06:00–12:00']);
}

// 4. A short range's labels step on the local clock across the DST change (25 Oct 2026 has 25 hours)
{
  const z = spanOf({ p: 0, a: D('2026-10-24T00:00'), b: D('2026-10-26T00:00') }, D('2026-11-13T12:00'));
  const lab = t => t.getHours() ? String(t.getHours()).padStart(2, '0') : `${t.getDate()} Oct`;
  check('24-26 Oct labels, wide', spanTicks(z, false).map(lab), ['24 Oct', '06', '12', '18', '25 Oct', '06', '12', '18', '26 Oct', '06', '12', '18']);
  check('24-26 Oct labels, phone', spanTicks(z, true).map(lab), ['24 Oct', '12', '25 Oct', '12', '26 Oct', '12']);
}

// 5. An hour answer starting after From asks the day records once; days stand in only when some lie
// before the first hour; day ranges never refresh on their own, hour ranges that include today do after 10 min
(async () => {
  const h = t => ({ h: new Date(t).toISOString().slice(0, 13) + ':00Z' });
  async function run(z, hours, days) {
    const asked = [];
    ctx.FC = { get: u => { asked.push(u.split('?')[0]); return Promise.resolve({ data: /hourly/.test(u) ? { station: 'ST 1', open: null, hours } : { station: 'ST 1', open: null, days } }); } };
    ctx.sheetOpen = () => false; ctx.range = 'days'; ctx.renderSheet = () => {}; ctx.SP = { p: 0 };
    Object.assign(SPC, { want: '', at: 0, data: null, err: false, busy: false, days: false, h0: null });
    spanSrc(z);
    for (let i = 0; i < 10; i++) await new Promise(r => setImmediate(r));
    return { asked, days: SPC.days, h0: SPC.h0 && ymd(SPC.h0) };
  }
  const z3 = spanOf({ p: 0, a: D('2026-06-01T00:00'), b: D('2026-06-03T00:00') });
  check('hours from From: one request', await run(z3, [h('2026-06-01T01:00'), h('2026-06-01T05:00')], []), { asked: ['/api/hourly'], days: false, h0: null });
  check('no hours, days on flash: days', await run(z3, [], [{ d: '2026-06-01' }]), { asked: ['/api/hourly', '/api/daily'], days: true, h0: null });
  check('hours start later, earlier days exist: days', await run(z3, [h('2026-06-03T08:00')], [{ d: '2026-06-01' }]), { asked: ['/api/hourly', '/api/daily'], days: true, h0: '2026-06-03' });
  check('hours start later, days start the same day (the station began then): hours', await run(z3, [h('2026-06-02T08:00')], [{ d: '2026-06-02' }]), { asked: ['/api/hourly', '/api/daily'], days: false, h0: null });
  check('no hours, no days: hours (it says no readings)', await run(z3, [], []), { asked: ['/api/hourly', '/api/daily'], days: false, h0: null });
  const stale = (z, sp) => { ctx.SP = sp; Object.assign(SPC, { want: spanUrl(z, sp), at: Date.now() - 11 * 60e3, data: {}, busy: false, err: false });
    let n = 0; ctx.FC = { get: () => { n++; return new Promise(() => {}); } }; spanSrc(z); return n; };
  check('30 d with today, 11 min old: no refresh', stale(spanOf({ p: 30 }), { p: 30 }), 0);
  check('3 d with today, 11 min old: refresh', stale(spanOf({ p: 3 }), { p: 3 }), 1);
  check('3 d with today, 5 min old: no refresh', (() => { const z = spanOf({ p: 3 }); ctx.SP = { p: 3 }; Object.assign(SPC, { want: spanUrl(z, { p: 3 }), at: Date.now() - 5 * 60e3, data: {}, busy: false });
    let n = 0; ctx.FC = { get: () => { n++; return new Promise(() => {}); } }; spanSrc(z); return n; })(), 0);
  console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
