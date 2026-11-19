// picker.js -- the world location picker, shared by setup and admin.
// Data: tz.json (tzdata zone.tab + iso3166.tab, POSIX rules from posix_tz_db), fetched when the picker
// first opens, and one flag pack per region, flags-<region>.json (circle-flags), fetched only for a region on
// screen and then prefetched while idle, one request at a time (core.js). A missing flag is a neutral circle with
// the country code until its pack lands. Country names also match in the browser's language (Intl.DisplayNames,
// no extra bytes), so "Deutschland" finds Germany.
// signal bars for a Wi-Fi network (setup and admin both list networks; not in core.js, the dashboard has none)
const rssiBars = r => { const n = r > -55 ? 4 : r > -67 ? 3 : r > -78 ? 2 : 1; return `<svg class="bars" width="18" height="14" viewBox="0 0 18 14" aria-hidden="true">${[0, 1, 2, 3].map(i => `<rect x="${i * 5}" y="${10 - i * 3}" width="3" height="${4 + i * 3}" rx="1" fill="currentColor" opacity="${i < n ? 1 : .25}"/>`).join('')}</svg>`; };
const PK_TZ = '/*@asset tz.json*/';
const PK_FLAGS = ['/*@asset flags-0.json*/', '/*@asset flags-1.json*/', '/*@asset flags-2.json*/', '/*@asset flags-3.json*/',
  '/*@asset flags-4.json*/', '/*@asset flags-5.json*/', '/*@asset flags-6.json*/', '/*@asset flags-7.json*/',
  '/*@asset flags-8.json*/', '/*@asset flags-9.json*/'];
let TZ = null, ZONES = [], phoneZone = null, pkLoading = null, pkFails = 0;
const FLAGS = {}, PACKS = new Set(), CC_R = {}, ZONES_PER_CC = {};
let prefetched = false;
const phoneTz = (() => { try { return Intl.DateTimeFormat().resolvedOptions().timeZone; } catch (e) { return ''; } })();
const fold = s => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/ł/g, 'l');
// Loads tz.json once; cb runs when the zones are ready (again on every call once loaded). urgent = the picker is on
// screen: the pack goes first in the queue; otherwise it waits its turn.
function pkLoad(cb, urgent, retry) {
  if (TZ) { cb(); return; }
  if (!retry) pkFails = 0;
  if (!pkLoading) pkLoading = FC.get(PK_TZ, { etag: false, front: !!urgent }).then(r => {
    TZ = r.data;
    const LOCAL = (() => { try { return new Intl.DisplayNames([navigator.language], { type: 'region' }); } catch (e) { return null; } })();
    const q = TZ.z;
    q.cc.forEach((cc, i) => { if (!(cc in CC_R)) CC_R[cc] = q.r[i]; });
    // parallel columns; the IANA name is its region's prefix +
    // the rest, the city that name's last part, lat / lon hundredths -- the same values the rows carried
    ZONES = q.cc.map((cc, i) => { const r = q.r[i], n = q.n[i]; return { i, cc, region: TZ.r[r], ri: r, tz: TZ.x[r] + '/' + n,
      city: n.slice(n.lastIndexOf('/') + 1).replace(/_/g, ' '), note: q.note[i], lat: q.la[i] / 100, lon: q.lo[i] / 100, posix: TZ.p[q.p[i]], country: TZ.c[cc],
      local: LOCAL ? LOCAL.of(cc) || '' : '' }; });
    // tzdata writes "St Helena": "saint" finds it too
    ZONES.forEach(z => { z.key = fold([z.city, z.country, z.local, z.tz, z.note].join(' ')).replace(/\bst\b/g, 'st saint'); ZONES_PER_CC[z.cc] = (ZONES_PER_CC[z.cc] || 0) + 1; });
    phoneZone = ZONES.find(z => z.tz === phoneTz) || null;
  }, e => { pkLoading = null; throw e; });
  // six tries 5 s apart, then the list says why; opening the picker again starts over
  pkLoading.then(cb, () => { if (++pkFails < 6) setTimeout(() => pkLoad(cb, urgent, true), 5000); else cb(); });
}
const needFlags = (ri, urgent) => {
  if (ri == null || PACKS.has(ri)) return;
  PACKS.add(ri);
  FC.get(PK_FLAGS[ri], { etag: false, front: urgent }).then(r => {
    Object.entries(r.data).forEach(([cc, svg]) => { if (cc[0] !== '_') FLAGS[cc] = svg; });  // "_l": the licence notice
    document.querySelectorAll('.flag.none[data-cc]').forEach(s => { if (FLAGS[s.dataset.cc]) s.outerHTML = flagImg(s.dataset.cc); });
  }, () => PACKS.delete(ri));
};
const prefetchFlags = () => { if (prefetched) return; prefetched = true; const go = () => TZ.r.forEach((_, ri) => needFlags(ri));
  window.requestIdleCallback ? requestIdleCallback(go, { timeout: 3000 }) : setTimeout(go, 1500); };
const flagImg = cc => `<img class="flag" alt="" src="data:image/svg+xml,${encodeURIComponent(FLAGS[cc])}">`;
const flag = cc => FLAGS[cc] ? flagImg(cc) : (needFlags(CC_R[cc], true), `<span class="flag none" data-cc="${cc}">${cc}</span>`);
function utc(p) { // "CET-1CEST,…" / "<+05>-5" / "IST-5:30" -> "UTC+1, summer UTC+2"
  const m = String(p || '').match(/^(?:<[^>]+>|[A-Za-z]+)([-+]?\d+)(?::(\d+))?(<[^>]+>|[A-Za-z]+)?/); if (!m) return '';
  const h = -(+m[1] + Math.sign(+m[1] || 1) * (+m[2] || 0) / 60), f = n => 'UTC' + (n >= 0 ? '+' : '−') + Math.floor(Math.abs(n)) + (Math.abs(n) % 1 ? ':' + String(Math.round(Math.abs(n) % 1 * 60)).padStart(2, '0') : '');
  return f(h) + (m[3] ? ', summer ' + f(h + 1) : '');
}
const zoneName = z => z.city + (z.note ? ` (${z.note})` : '');
// what the chosen place is called outside the picker: the country, plus the zone only where the country has several.
// The city is the time zone's reference city, not where the station stands, so it is not shown as "your place".
const zoneNote = z => { // tzdata comments repeat the country ("Spain (mainland)", "most of Ukraine"): drop the repeat
  let n = z.note || z.city; const c = z.country;
  if (n.startsWith(c + ' (') && n.endsWith(')')) n = n.slice(c.length + 2, -1);
  return n.replace(' of ' + c, ' areas').replace(/^most areas$/, 'most areas');
};
const placeName = z => z.country;
const clockLine = (z, posix) => (z && ZONES_PER_CC[z.cc] > 1 ? zoneNote(z) + ', ' : '') + utc(posix || (z && z.posix));
const zoneByName = name => ZONES.find(z => z.tz === name) || null;
// P = { q, region, cc }: what the picker shows; returns HTML. Events: data-pk-region / -cc / -zone / -up.
// typing in the search field re-renders the list and crumbs only, never the field itself
function pickerRefresh(P, sel) {
  const pk = document.querySelector('.pk');
  if (!pk) return false;
  const t = document.createElement('div');
  t.innerHTML = pickerHtml(P, sel);
  while (pk.children.length > 1) pk.lastElementChild.remove();
  [...t.firstElementChild.children].slice(1).forEach(c => pk.appendChild(c));
  return true;
}
const WP = FC.w('picker', {
  fail: 'The list of places does not load. Check that this device is still on fauxcloud’s Wi-Fi, then open this step again.',
  loading: 'Loading the list of places…', none: 'Nothing matches. Try the country name, or browse by region below after clearing the search.',
  countries: { one: '{n} country', other: '{n} countries' }, zones: { other: '{n} zones' }, all: 'All regions',
  search: 'Search a city or country', ph: 'e.g. Warsaw, Poland, Polska', list: 'Locations',
});
function pickerHtml(P, sel) {
  if (!TZ) return pkFails >= 6 ? `<p class="error">${WP.fail}</p>`
    : `<p class="hint">${WP.loading}</p>`;
  prefetchFlags();
  const row = (attr, main, side, f, on) => `<li><button type="button" ${attr} ${on ? 'aria-pressed="true"' : ''}>${f || ''}<span class="pk-main">${main}</span><small>${side || ''}</small></button></li>`;
  let crumbs = '', items = '';
  if (P.q.trim()) {
    const q = fold(P.q.trim()), hits = ZONES.filter(z => z.key.includes(q)).slice(0, 40);
    items = hits.map(z => row(`data-pk-zone="${z.i}"`, `${esc(zoneName(z))}, ${esc(z.country)}`, utc(z.posix).split(',')[0], flag(z.cc), sel === z)).join('')
      || `<li class="hint pk-empty">${WP.none}</li>`;
  } else if (!P.region) {
    items = TZ.r.map(r => { const n = new Set(ZONES.filter(z => z.region === r).map(z => z.cc)).size; return row(`data-pk-region="${esc(r)}"`, esc(r), FC.t(WP.countries, { n })); }).join('');
  } else if (!P.cc) {
    crumbs = `<button type="button" class="link" data-pk-up="region">${WP.all}</button> <span class="hint">/ ${esc(P.region)}</span>`;
    needFlags(TZ.r.indexOf(P.region), true);
    const ccs = [...new Set(ZONES.filter(z => z.region === P.region).map(z => z.cc))].sort((a, b) => TZ.c[a].localeCompare(TZ.c[b]));
    items = ccs.map(cc => { const zs = ZONES.filter(z => z.cc === cc && z.region === P.region);
      return row(zs.length === 1 ? `data-pk-zone="${zs[0].i}"` : `data-pk-cc="${cc}"`, esc(TZ.c[cc]), zs.length === 1 ? utc(zs[0].posix).split(',')[0] : FC.t(WP.zones, { n: zs.length }), flag(cc), zs.length === 1 && sel === zs[0]); }).join('');
  } else {
    crumbs = `<button type="button" class="link" data-pk-up="region">${WP.all}</button> <span class="hint">/</span> <button type="button" class="link" data-pk-up="cc">${esc(P.region)}</button> <span class="hint">/ ${esc(TZ.c[P.cc])}</span>`;
    items = ZONES.filter(z => z.cc === P.cc && z.region === P.region).map(z => row(`data-pk-zone="${z.i}"`, esc(zoneName(z)), utc(z.posix).split(',')[0], '', sel === z)).join('');
  }
  return `<div class="pk"><label class="f" for="pkq">${WP.search}<span class="input"><input id="pkq" type="search" value="${esc(P.q)}" placeholder="${WP.ph}" autocomplete="off"></span></label>
    ${crumbs ? `<div class="pk-crumbs">${crumbs}</div>` : ''}<ul class="pk-list" aria-label="${WP.list}">${items}</ul></div>`;
}
// returns the chosen zone, 'nav' after a navigation step, or null
function pickerClick(P, b) {
  if (b.dataset.pkRegion) { P.region = b.dataset.pkRegion; P.cc = null; return 'nav'; }
  if (b.dataset.pkCc) { P.cc = b.dataset.pkCc; return 'nav'; }
  if (b.dataset.pkUp) { if (b.dataset.pkUp === 'region') P.region = null; P.cc = null; return 'nav'; }
  if (b.dataset.pkZone) return ZONES[+b.dataset.pkZone];
  return null;
}
// what the firmware stores for a zone (POSIX rule + IANA name + the city's coordinates)
const zoneBody = z => ({ tz: z.posix, tz_name: z.tz, lat: z.lat, lon: z.lon });
