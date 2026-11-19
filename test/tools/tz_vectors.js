// test/tools/tz_vectors.js -- the picker's zones,
// rebuilt by picker.js from the columnar tz.json, against the vendored sources the generator reads (tzdata zone.tab,
// posix_tz_db zones.json, the generator's FALLBACK map): for every zone the IANA name, the city, lat / lon (2 decimals,
// ISO 6709 as gen_tz_flags.py coord() reads it), the POSIX rule and the country must be the same. Lossless or it fails.
// Usage: node test/tools/tz_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { fcStub } = require('./js_decl');

const ROOT = path.join(__dirname, '..', '..');
const WEB = path.join(ROOT, 'firmware', 'fauxcloud', 'web'), SRC = path.join(WEB, 'data', 'sources');
let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; if (fail < 20) console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}

// the expected zones, straight from the sources
const tab = f => fs.readFileSync(path.join(SRC, 'tzdata', f), 'utf8').split(/\r?\n/).filter(l => l && l[0] !== '#').map(l => l.split('\t'));
const posix = JSON.parse(fs.readFileSync(path.join(SRC, 'posix_tz_db', 'zones.json'), 'utf8'));
const gen = fs.readFileSync(path.join(ROOT, 'tools', 'gen_tz_flags.py'), 'utf8');
const FALLBACK = Object.fromEntries([...gen.slice(gen.indexOf('FALLBACK = {'), gen.indexOf('}', gen.indexOf('FALLBACK = {'))).matchAll(/"([^"]+)": "([^"]+)"/g)].map(m => [m[1], m[2]]));
const countries = Object.fromEntries(tab('iso3166.tab').map(r => [r[0], r[1]]));
// Python's round(x, 2) works on the exact binary value (35.095 is 35.09499...: toFixed does the same, Math.round(x*100) not)
// and rounds an exact tie to even (-86.625 is exact: -86.62; toFixed would give -86.63)
const r2 = v => {
  const s = Math.abs(v).toFixed(20), m = /^(\d+)\.(\d\d)50*$/.exec(s);
  if (!m) return +v.toFixed(2);
  const c = +(m[1] + m[2]) + (+m[2][1] % 2 ? 1 : 0);
  return Math.sign(v) * c / 100;
};
function coord(t) {  // gen_tz_flags.py coord(): +DDMM[SS]+DDDMM[SS] -> degrees, 2 decimals
  const m = /^([+-]\d{2})(\d{2})(\d{2})?([+-]\d{3})(\d{2})(\d{2})?$/.exec(t);
  const deg = (d, mi, s) => { const v = Math.abs(+d) + +mi / 60 + (s ? +s / 3600 : 0); return d[0] === '-' ? -v : v; };
  return [r2(deg(m[1], m[2], m[3])), r2(deg(m[4], m[5], m[6]))];
}
const zt = tab('zone.tab');
const want = zt.map(z => { const [lat, lon] = coord(z[1]); return { cc: z[0], tz: z[2], city: z[2].slice(z[2].lastIndexOf('/') + 1).replace(/_/g, ' '), lat, lon,
  posix: posix[z[2]] || posix[FALLBACK[z[2]]], country: countries[z[0]] }; });

// the zones as the page builds them: picker.js run in a vm with a stub FC.get answering the committed tz.json
const tzjson = JSON.parse(fs.readFileSync(path.join(WEB, 'data', 'tz.json'), 'utf8'));
const ctx = { FC: fcStub({ get: () => Promise.resolve({ data: JSON.parse(JSON.stringify(tzjson)) }) }), Intl, navigator: { language: 'en' }, console, Promise, setTimeout };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(WEB, 'picker.js'), 'utf8') + '\nthis.pk = { pkLoad, get ZONES() { return ZONES; } };', ctx);
ctx.pk.pkLoad(() => {
  const Z = ctx.pk.ZONES;
  check('zone count', Z.length, want.length);
  check('418 zones (tzdata 2026f)', Z.length, 418);
  const byTz = Object.fromEntries(Z.map(z => [z.tz, z]));
  for (const w of want) {
    const z = byTz[w.tz];
    if (!z) { check('zone present: ' + w.tz, false, true); continue; }
    check(w.tz + ' city', z.city, w.city);
    check(w.tz + ' lat / lon', [z.lat, z.lon], [w.lat, w.lon]);
    check(w.tz + ' POSIX', z.posix, w.posix);
    check(w.tz + ' country', [z.cc, z.country], [w.cc, w.country]);
  }
  check('Warsaw', [byTz['Europe/Warsaw'].city, byTz['Europe/Warsaw'].lat, byTz['Europe/Warsaw'].lon], ['Warsaw', 52.25, 21]);
  check('a three-part name keeps its middle', byTz['America/Argentina/Buenos_Aires'].city, 'Buenos Aires');
  check('the columnar shape', Object.keys(tzjson.z).sort(), ['cc', 'la', 'lo', 'n', 'note', 'p', 'r']);
  console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
}, true);
