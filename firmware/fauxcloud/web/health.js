'use strict';
// health.js -- the bridge's status row of the dashboard and Settings, bundled into status.js and admin.js
// (not core.js: the setup page inlines core.js and shows no status row)
// One clock time for both faces and every sheet (here, not in core.js: setup inlines core.js and needs none): a Date or an ISO string -> "09:05" (local); dm -> "10 Nov"
const p2 = n => String(n).padStart(2, '0');
const hm = t => (t = new Date(t), p2(t.getHours()) + ':' + p2(t.getMinutes()));
const dm = t => t.getDate() + ' ' + (FC.L ? FC.L.mon(t) : ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][t.getMonth()]);
const WH = FC.w('health', {
  yday: 'yesterday {t}', off_since: 'Offline since {w}',
  // the status row's words (title and aria-label), one whole text per state
  clock_ok: 'Clock: synced', clock_approx: 'Clock: approximate', clock_none: 'Clock: not synced',
  mqtt_ok: 'MQTT: connected', mqtt_off: 'MQTT: disconnected', mem: 'Memory: {kb} KB free',
  wifi_good: 'Wi-Fi: good, {dbm} dBm', wifi_weak: 'Wi-Fi: weak, {dbm} dBm', wifi_lost: 'Wi-Fi: lost',
});
// The time the console went quiet, with the day when it is not today: "09:05", "yesterday 23:40",
// "10 Nov 07:00"; '' for no time. Callers use it only with a synced clock (before that, last_seen is seconds since boot:
// they say "Offline" with the age instead). A time before 2020 is uptime, never a clock time (EPOCH_MIN)
function dayTime(iso, now) {
  const t = new Date(iso); if (!iso || isNaN(t) || t < 1577836800e3) return '';
  const day = d => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime(), dd = Math.round((day(now) - day(t)) / 864e5);
  return dd <= 0 ? hm(t) : dd === 1 ? FC.t(WH.yday, { t: hm(t) }) : dm(t) + ' ' + hm(t);
}
const offSince = (iso, now) => { const w = dayTime(iso, now); return w ? FC.t(WH.off_since, { w }) : ''; };  // one template
// The bridge's status row, one renderer for both faces (the dashboard: the row alone; Settings: the row
// over Details). Glyphs only, in the state's colour; the words in title (hover) and aria-label. The AP face has no diag:
// the clock alone (no LAN details on the AP). `mq` = a broker is set (the caller knows it): without one the MQTT glyph
// is left out, never a standing amber "disconnected".
// Every state in the status row carries a Lucide glyph (ISC) in its state's colour, in place of the dot
const STIC = {
  Clock: '<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>',
  MQTT: '<path d="M4.9 16.1C1 12.2 1 5.8 4.9 1.9"/><path d="M7.8 4.7a6.14 6.14 0 0 0-.8 7.5"/><circle cx="12" cy="9" r="2"/><path d="M16.2 4.8c2 2 2.26 5.11.8 7.47"/><path d="M19.1 1.9a9.96 9.96 0 0 1 0 14.1"/><path d="M9.5 18h5"/><path d="m8 22 4-11 4 11"/>',
  Memory: '<rect width="16" height="16" x="4" y="4" rx="2"/><rect width="6" height="6" x="9" y="9" rx="1"/><path d="M15 2v2"/><path d="M15 20v2"/><path d="M2 15h2"/><path d="M2 9h2"/><path d="M20 15h2"/><path d="M20 9h2"/><path d="M9 2v2"/><path d="M9 20v2"/>',
};
const stSvg = ic => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ic}</svg>`;
// The bridge's home Wi-Fi in the status row; good from -67 dBm (picker.js's three bars). Lucide wifi glyphs (ISC).
const WIFI = {
  good: '<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.859a10 10 0 0 1 14 0"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>',
  weak: '<path d="M12 20h.01"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/>',
  lost: '<path d="M12 20h.01"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/><path d="M5 12.859a10 10 0 0 1 5.17-2.69"/><path d="M19 12.859a10 10 0 0 0-2.007-1.523"/><path d="M2 8.82a15 15 0 0 1 4.177-2.643"/><path d="M22 8.82a15 15 0 0 0-11.288-3.764"/><path d="m2 2 20 20"/>',
};
function healthRow(head, mq) {
  const d = head.diag, sync = head.time_synced, q = d && (!d.sta_up ? 'lost' : d.sta_rssi >= -67 ? 'good' : 'weak');
  const L = [[sync, sync ? WH.clock_ok : head.time_src === 'persisted' ? WH.clock_approx : WH.clock_none, STIC.Clock]].concat(d ? [
    ...d.mqtt_up || mq ? [[d.mqtt_up, d.mqtt_up ? WH.mqtt_ok : WH.mqtt_off, STIC.MQTT]] : [], [!d.heap_low, FC.t(WH.mem, { kb: Math.round(d.heap_free / 1024) }), STIC.Memory],
    [q === 'good', FC.t(WH['wifi_' + q], { dbm: String(d.sta_rssi).replace('-', '\u2212') }), WIFI[q]]] : []);  // lost = not joined: no dBm
  return `<span class="states">${L.map(([ok, s, ic]) => `<span class="state${ok ? '' : ' warn'}" role="img" title="${s}" aria-label="${s}">${stSvg(ic)}</span>`).join('')}</span>`;
}
