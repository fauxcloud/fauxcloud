// web_render.cpp -- see web_render.h.
#include "web_render.h"

#include <cmath>

#include "metrics.h"
#include "store.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "config.h"
#include "derive.h"
#include "diag_bench.h"
#include "ha_discovery.h"

namespace fc {

// ---------------------------------------------------------------- Out

void Out::write(const char* s, size_t n) {
  while (n) {
    if (len_ == cap_ && !flush()) return;
    size_t k = cap_ - len_;
    if (k > n) k = n;
    memcpy(buf_ + len_, s, k);
    len_ += k;
    s += k;
    n -= k;
  }
}

void Out::raw(const char* s) { write(s, strlen(s)); }

void Out::printf(const char* fmt, ...) {
  char tmp[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;  // callers keep single printf calls short
  write(tmp, (size_t)n);
}

void Out::jsonStr(const char* s) {
  write("\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      char e[2] = {'\\', (char)c};
      write(e, 2);
    } else if (c < 0x20) {
      printf("\\u%04x", c);
    } else {
      write((const char*)&c, 1);
    }
  }
  write("\"", 1);
}

void Out::htmlStr(const char* s) {
  for (; *s; s++) {
    switch (*s) {
      case '<': raw("&lt;"); break;
      case '>': raw("&gt;"); break;
      case '&': raw("&amp;"); break;
      case '"': raw("&quot;"); break;
      default: write(s, 1);
    }
  }
}

bool Out::flush() {
  if (!ok_) { len_ = 0; return false; }
  if (len_ && !fn_(ctx_, buf_, len_)) ok_ = false;
  len_ = 0;
  return ok_;
}

// ---------------------------------------------------------------- helpers

// a value that does not format (too wide, not finite) is null, never an empty JSON value
static void value(Out& o, int f, float v) {
  char b[24];
  if (formatValue(b, sizeof b, f, v) > 0) o.raw(b);
  else o.raw("null");
}

static void packed(Out& o, int f, int16_t raw) {
  if (hasValue(raw)) value(o, f, unpack(f, raw));
  else o.raw("null");
}

static void iso(Out& o, int64_t t) {
  char b[24];
  if (formatIsoUtc(b, sizeof b, t) > 0) o.raw(b);
}

static void ipStr(char out[16], uint32_t ip_net) {
  const uint8_t* b = (const uint8_t*)&ip_net;
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void hourKey(Out& o, uint32_t hour) {
  char b[24];
  if (formatIsoUtc(b, sizeof b, hour) > 0) {
    b[16] = 0;  // "YYYY-MM-DDTHH:MM"
    o.raw(b);
    o.raw("Z");
  }
}

// ---------------------------------------------------------------- JSON

#if FC_DIAG_BENCH  // the task table and its names exist only in the bench build
const char* const DIAG_TASK_KEY[DIAG_TASKS] = {"loop", "httpd", "mqtt", "udp53_123", "ota", "netout"};
const char* const DIAG_TASK_RTOS[DIAG_TASKS] = {"loopTask", "httpd", "mqtt_task", "udp53_123", "ota", "netout"};
int diagTaskIndex(const char* rtos_name) {
  for (int i = 0; rtos_name && i < DIAG_TASKS; i++)
    if (strcmp(rtos_name, DIAG_TASK_RTOS[i]) == 0) return i;
  return -1;
}
#endif

static void diag(Out& o, const Diag& d) {
  o.printf(",\"diag\":{\"ap_clients\":%d,\"sta_up\":%s,\"sta_rssi\":%d,\"mqtt_up\":%s", d.ap_clients,
           d.sta_up ? "true" : "false", d.sta_rssi, d.mqtt_up ? "true" : "false");
  o.printf(",\"heap_free\":%lu,\"heap_min\":%lu,\"heap_largest\":%lu", (unsigned long)d.heap_free,
           (unsigned long)d.heap_min, (unsigned long)d.heap_largest);
  o.printf(",\"uploads\":%lu,\"queue_drops\":%lu,\"other_hits\":%lu", (unsigned long)d.uploads,
           (unsigned long)d.queue_drops, (unsigned long)d.other_hits);
  o.printf(",\"uploads_dropped_lan\":%lu,\"uploads_dropped_key\":%lu,\"uploads_dropped_blocked\":%lu",
           (unsigned long)d.uploads_dropped_lan, (unsigned long)d.uploads_dropped_key,
           (unsigned long)d.uploads_dropped_blocked);
  o.printf(",\"uploads_too_long\":%lu,\"uploads_no_slot\":%lu,\"uploads_rate_limited\":%lu,"
           "\"uploads_duplicate\":%lu,\"stale_longest_run\":%lu,\"stale_on_count\":%lu", (unsigned long)d.uploads_too_long,
           (unsigned long)d.uploads_no_slot, (unsigned long)d.uploads_rate_limited, (unsigned long)d.uploads_duplicate,
           (unsigned long)d.stale_longest_run, (unsigned long)d.stale_on_count);
  // one printf <= 191 chars (Out::printf truncates): the store counters on their own line
  o.printf(",\"store_fail\":%lu,\"store_drop_old\":%lu,\"store_cut\":%lu,"
           "\"store_fail_append\":%lu,\"store_future_dropped\":%lu", (unsigned long)d.store_fail,
           (unsigned long)d.store_drop_old, (unsigned long)d.store_cut,
           (unsigned long)d.store_fail_append, (unsigned long)d.store_future_dropped);
  o.printf(",\"clock_held\":%s,\"clock_held_n\":%lu,\"clock_ref\":%lu,\"clock_cut_n\":%lu,\"store_future_kept\":%lu",
           d.clock_held ? "true" : "false", (unsigned long)d.clock_held_n, (unsigned long)d.clock_ref,
           (unsigned long)d.clock_cut_n, (unsigned long)d.store_future_kept);
  o.printf(",\"store_retry\":%lu,\"http_stream_abort\":%lu,\"mqtt_init_fail\":%lu,\"snap_from\":%u",
           (unsigned long)d.store_retry, (unsigned long)d.http_stream_abort, (unsigned long)d.mqtt_init_fail,
           (unsigned)d.snap_from);
  o.raw(",\"hist_back\":[");
  for (int i = 0; i < MAX_STATIONS; i++) o.printf("%s%lu", i ? "," : "", (unsigned long)d.hist_back[i]);
  o.raw("]");
  o.printf(",\"dns_answered\":%lu,\"ntp_answered\":%lu,\"ntp_unanswered\":%lu", (unsigned long)d.dns_answered,
           (unsigned long)d.ntp_answered, (unsigned long)d.ntp_unanswered);
  o.printf(",\"mqtt_published\":%lu,\"mqtt_failed\":%lu", (unsigned long)d.mqtt_published,
           (unsigned long)d.mqtt_failed);
  o.printf(",\"fwd_ok\":%lu,\"fwd_fail\":%lu,\"fwd_attempt_fail\":%lu,\"fwd_skip\":%lu,\"fwd_drop\":%lu",
           (unsigned long)d.fwd_ok, (unsigned long)d.fwd_fail, (unsigned long)d.fwd_attempt_fail,
           (unsigned long)d.fwd_skip, (unsigned long)d.fwd_drop);
#if FC_DIAG_BENCH  // the send low-waters are bench keys -- absent from the release JSON
  o.printf(",\"fwd_heap_min\":%lu,\"fwd_largest_min\":%lu,\"fwd_send_ms_max\":%lu", (unsigned long)d.fwd_heap_min,
           (unsigned long)d.fwd_largest_min, (unsigned long)d.fwd_send_ms_max);
#endif
  o.printf(",\"heap_low\":%s,\"heap_min_10m\":%lu,\"http_requests\":%lu,\"http_304\":%lu",
           d.heap_low ? "true" : "false", (unsigned long)d.heap_min_10m, (unsigned long)d.http_requests,
           (unsigned long)d.http_304);
  o.printf(",\"capture\":%s", d.capture ? "true" : "false");
#if FC_DIAG_BENCH  // bench keys
  o.raw(",\"ota_heap\":");
  if (d.ota_heap_have)
    o.printf("{\"start8\":%lu,\"min8\":%lu,\"largest_min8\":%lu}", (unsigned long)d.ota_start8, (unsigned long)d.ota_min8,
             (unsigned long)d.ota_largest_min8);
  else o.raw("null");
#endif
  // the second of the since-boot 8-bit minimum (heap_min): uptime s, who was busy, open httpd sockets
  if (d.heap_dip.have()) {
    o.printf(",\"heap_min8_t\":%lu,\"heap_min8_ctx\":[", (unsigned long)d.heap_dip.up_s);
    for (int b = 0, n = 0; b < HEAP_CTX_BITS; b++)
      if (d.heap_dip.ctx & (1u << b)) o.printf("%s\"%s\"", n++ ? "," : "", heapCtxName(b));
    o.printf("],\"heap_min8_socks\":%d", (int)d.heap_dip.socks);
  } else {
    o.raw(",\"heap_min8_t\":null,\"heap_min8_ctx\":[],\"heap_min8_socks\":null");
  }
#if FC_DIAG_BENCH  // bench -- the release has no "tasks" key at all
  o.raw(",\"tasks\":{");  // only the tasks that exist now (ota during an update, netout with forwarding)
  for (int i = 0, n = 0; i < DIAG_TASKS; i++)
    if (d.task_free[i]) o.printf("%s\"%s\":%lu", n++ ? "," : "", DIAG_TASK_KEY[i], (unsigned long)d.task_free[i]);
  o.raw("}");
  o.printf(",\"http_busy_ms_max\":%lu", (unsigned long)d.http_busy_ms_max);
#endif
  o.printf(",\"http_aborted\":%lu,\"http_purged\":%lu,\"http_shed\":%lu,\"http_paced\":%lu,"
           "\"sta_ip\":", (unsigned long)d.http_aborted, (unsigned long)d.http_purged,
           (unsigned long)d.http_shed, (unsigned long)d.http_paced);
  o.jsonStr(d.sta_ip);
  o.raw(",\"fw_version\":");
  o.jsonStr(d.fw_version ? d.fw_version : "");
  o.raw(",\"hostname\":");
  o.jsonStr(d.hostname ? d.hostname : "");
  o.printf(",\"stations_allocated\":%d,\"stations_used\":%d,\"mode\":", d.stations_allocated, d.stations_used);
  o.jsonStr(d.mode ? d.mode : "run");
  o.printf(",\"admin_lock\":{\"active\":%s,\"until_s\":%lu,\"failures\":%d}", d.admin_locked ? "true" : "false",
           (unsigned long)d.admin_lock_s, d.admin_failures);
  o.printf(",\"store_ms_max\":%ld", (long)d.store_ms_max);
  o.raw(",\"build\":");
  o.jsonStr(d.build ? d.build : "");
  o.raw(",\"page\":");
  o.jsonStr(d.page ? d.page : "");
  o.printf(",\"ota_state\":\"%s\",\"ota_slot\":%lu,\"last_reset\":", d.ota_state ? d.ota_state : "stable",
           (unsigned long)d.ota_slot);
  o.jsonStr(d.last_reset ? d.last_reset : "unknown");
  o.raw(",\"reset_raw\":");
  o.jsonStr(d.reset_raw ? d.reset_raw : "");
  o.printf(",\"restarts\":%lu,\"sta_reinits\":%lu,\"mqtt_reconnects\":%lu,\"last_crash\":", (unsigned long)d.restarts,
           (unsigned long)d.sta_reinits, (unsigned long)d.mqtt_reconnects);
  if (d.crash) {
    o.raw("{\"reason\":");
    o.jsonStr(d.crash_reason ? d.crash_reason : "");
    o.raw(",\"task\":");
    o.jsonStr(d.crash_task ? d.crash_task : "");
    o.printf(",\"pc\":\"0x%08lx\"}", (unsigned long)d.crash_pc);
  } else {
    o.raw("null");
  }
  o.raw("}");
}

void snapOutlook(const Station& st, LiveSnap* s, float lat, float lon, bool time_ok, const OutNear* near) {
  OutlookIn in = outlookInput(st, lat, lon, time_ok, outlookBuffer(), OUT_BUF_H);  // the caller holds gLock
  if (near) in.near = *near;  // T3
  s->outlook = outlook(in);
}

void snapLive(const Station& st, uint32_t now_ms, LiveSnap* s, const PosixTz* tz) {
  memcpy(s->id, st.id, sizeof s->id);
  st.displayName(s->name, sizeof s->name);
  s->configured = stationConfigured(st.cfg);
  s->stored = st.stored;
  s->publish = st.cfg.publish;
  s->online = st.online(now_ms);
  s->profile = st.cfg.profile;
  s->last_seen = st.last_seen;
  s->src_ip = st.src_ip;
  s->uploads = st.uploads;
  s->restored = (st.flash_state & FLASH_LIVE_RESTORED) && !st.uploads;
  s->dropped = st.dropped;
  s->src_if = st.src_if;
  s->live = st.live;
  s->live_rejected = st.live_rejected;
  memcpy(s->rejects, st.rejects, sizeof s->rejects);
  memcpy(s->rej_rule, st.rej_rule, sizeof s->rej_rule);
  s->promoted_mask = st.promoted_mask;
  s->hidden = st.cfg.disabled;
  memcpy(s->field_t, st.field_t, sizeof s->field_t);
  memcpy(s->promo_t, st.promo_t, sizeof s->promo_t);
  memcpy(s->known, st.known, sizeof s->known);
  memcpy(s->promoted, st.promoted, sizeof s->promoted);
  memcpy(s->promote, st.cfg.promote, sizeof s->promote);
  s->derived = st.derived & ~st.cfg.disabled;
  s->warming = st.warming & ~st.cfg.disabled;
  s->rain_hours = st.rain_hours && !((st.cfg.disabled >> F_RAIN_MM) & 1u);
  memcpy(s->derived_n, st.derived_n, sizeof s->derived_n);
  s->w10 = st.wind10_have && !((st.cfg.disabled >> F_WIND_KMH) & 1u), s->w10v = st.wind10, s->w10n = st.wind10_n, s->w10t = st.wind10_t;
  s->sensor_stale = st.sensor_stale;
  s->baro_uncal = st.baro_uncal;
  s->stale_since = st.stale_since;
  s->notices = stationNotices(st);
  s->outlook = Outlook{};  // filled by snapOutlook
  if (tz) s->tz = *tz;
  else parsePosixTz("UTC0", &s->tz);
}

// how long before the station's last upload a value arrived (0 = in it); -1 = unknown (a v1-v3 snapshot). Counted
// from last_seen, not from now, so the body only changes with an upload and the ETag stays valid in between.
static long ageBefore(uint32_t last_seen, uint32_t t) { return t && last_seen >= t ? (long)(last_seen - t) : -1; }

void renderLiveStation(Out& o, const LiveSnap& st, const Clock& c, bool lan) {
  char ip[16];
  ipStr(ip, st.src_ip);
  o.raw("{\"id\":");
  o.jsonStr(st.id);
  o.raw(",\"name\":");
  o.jsonStr(st.name);
  o.printf(",\"configured\":%s,\"stored\":%s,\"publish\":%s,\"profile\":\"%s\"", st.configured ? "true" : "false",
           st.stored ? "true" : "false", st.publish ? "true" : "false", profileRules(st.profile).name);
  o.raw(",\"hidden\":[");  // one rule for the whole page (tiles, sheets, CSV); additive
  for (int f = 0, n = 0; f < F_COUNT; f++)
    if ((st.hidden >> f) & 1u) o.printf("%s\"%s\"", n++ ? "," : "", FIELDS[f].key);
  o.raw("]");
  // computed values with the readings behind each, and the ones still warming up; only when there are
  if (st.derived || st.w10) {
    o.raw(",\"derived\":{");
    int n = 0;
    for (int f = 0; f < F_COUNT; f++)
      if ((st.derived >> f) & 1u) o.printf("%s\"%s\":%u", n++ ? "," : "", FIELDS[f].key, (unsigned)st.derived_n[f]);
    if (st.w10) o.printf("%s\"wind_avg10m_kmh\":%u", n ? "," : "", (unsigned)st.w10n);
    o.raw("}");
  }
  if (st.rain_hours) o.raw(",\"sources\":{\"rain_mm\":\"hours\"}");  // estimated from the hour records (docs/api.md §6.2)
  // the sensor flags + the notices (one table with MQTT, notices.h)
  o.printf(",\"sensor_stale\":%s,\"baro_uncal\":%s", st.sensor_stale ? "true" : "false", st.baro_uncal ? "true" : "false");
  if (st.sensor_stale && st.stale_since >= EPOCH_MIN) {  // never a 1970 time
    o.raw(",\"stale_since\":\"");
    iso(o, st.stale_since);
    o.raw("\"");
  }
  if (st.notices.n) {
    o.raw(",\"notices\":");
    noticesJson(o, st.notices, st.tz, c.synced ? c.now : 0);  // the day word only with a real clock
  }
  if (st.outlook.present) {  // the local outlook: codes + params (omitted without a barometer)
    o.raw(",\"outlook\":");
    outlookJson(o, st.outlook);
  }
  if (st.wufc_on) wufcLiveJson(o, st.wufc, c.now, st.tz);  // the WU line's data while shown (absent otherwise)
  if (st.warming) {
    o.raw(",\"warming\":[");
    for (int f = 0, n = 0; f < F_COUNT; f++)
      if ((st.warming >> f) & 1u) o.printf("%s\"%s\"", n++ ? "," : "", FIELDS[f].key);
    o.raw("]");
  }
  o.printf(",\"online\":%s,\"last_seen\":\"", st.online ? "true" : "false");
  iso(o, st.last_seen);
  o.printf("\",\"age_s\":%lu", (unsigned long)(c.now > st.last_seen ? c.now - st.last_seen : 0));
  if (lan) o.printf(",\"src_ip\":\"%s\",\"src_if\":\"%s\"", ip, srcIfName(st.src_if));
  o.printf(",\"uploads\":%lu,\"restored\":%s,\"dateutc\":", (unsigned long)st.uploads, st.restored ? "true" : "false");
  if (st.live.has_dateutc) {
    o.raw("\"");
    iso(o, st.live.dateutc);
    o.raw("\"");
  } else {
    o.raw("null");
  }
  o.raw(",\"values\":{");
  bool first = true;
  for (int f = 0; f < F_COUNT; f++) {
    if (!st.live.has(f) || ((st.hidden >> f) & 1u)) continue;  // hidden: recorded, never on the faces
    o.printf("%s\"%s\":", first ? "" : ",", FIELDS[f].key);
    value(o, f, st.live.v[f]);
    first = false;
  }
  // null once its window has been empty for more than the window (a value older than 600 s is no
  // 10-min mean of now); the age below says how old
  const long w10a = st.w10 ? ageBefore(st.last_seen, st.w10t) : -1;
  if (st.w10 && std::isfinite(st.w10v)) {
    if (w10a > (long)GUST_MAX_S) o.printf("%s\"wind_avg10m_kmh\":null", first ? "" : ",");
    else o.printf("%s\"wind_avg10m_kmh\":%.1f", first ? "" : ",", (double)st.w10v);
    first = false;
  }
  // seconds before the last upload, per value shown above (absent = unknown); 0 = from the latest upload
  o.raw("},\"age\":{");
  first = true;
  for (int f = 0; f < F_COUNT; f++) {
    const long a = ageBefore(st.last_seen, st.field_t[f]);
    if (!st.live.has(f) || ((st.hidden >> f) & 1u) || a < 0) continue;
    o.printf("%s\"%s\":%ld", first ? "" : ",", FIELDS[f].key, a);
    first = false;
  }
  if (st.w10 && w10a >= 0) o.printf("%s\"wind_avg10m_kmh\":%ld", first ? "" : ",", w10a);
  o.raw("},\"last_rejected\":[");
  first = true;
  for (int f = 0; f < F_COUNT; f++) {
    if (!((st.live_rejected >> f) & 1u)) continue;
    o.printf("%s\"%s\"", first ? "" : ",", FIELDS[f].key);
    first = false;
  }
  o.raw("],\"rejects_since_boot\":{");
  first = true;
  for (int f = 0; f < F_COUNT; f++) {
    if (!st.rejects[f]) continue;
    o.printf("%s\"%s\":%lu", first ? "" : ",", FIELDS[f].key, (unsigned long)st.rejects[f]);
    first = false;
  }
  o.raw("},\"rej\":{");
  for (int r = 0; r < R_COUNT; r++)
    o.printf("%s\"%s\":%lu", r ? "," : "", ruleName(r), (unsigned long)st.rej_rule[r]);
  // promoted extras with what the faces need from the type; live only, no history
  o.printf("},\"dropped\":%lu,\"promoted\":{", (unsigned long)st.dropped);
  first = true;
  // every configured promotion, "v": null when the last upload had no value: a sensor that was set up
  // is always shown, with "–" until it has a reading
  for (int p = 0; p < MAX_PROMOTE; p++) {
    if (!st.promote[p].used) continue;
    const Promote& pr = st.promote[p];
    const PromoTypeDef& t = promoType(pr.type);
    if (!first) o.raw(",");
    first = false;
    o.jsonStr(pr.key);
    // a non-finite value (never taken; a defence) prints null
    if (((st.promoted_mask >> p) & 1u) && std::isfinite(st.promoted[p]))
      o.printf(":{\"v\":%.*f,\"name\":", (int)t.dec, (double)st.promoted[p]);
    else o.raw(":{\"v\":null,\"name\":");
    o.jsonStr(pr.name);
    o.printf(",\"type\":\"%s\",\"unit\":", t.id);
    o.jsonStr(pr.unit);
    o.printf(",\"group\":\"%s\",\"icon\":\"%s\",\"span\":%g,\"max\":", t.group, t.icon, (double)t.span);
    if (t.max != t.max) o.raw("null");
    else o.printf("%g", (double)t.max);
    o.printf(",\"dec\":%u", (unsigned)t.dec);
    const long a = ageBefore(st.last_seen, st.promo_t[p]);
    if (((st.promoted_mask >> p) & 1u) && a >= 0) o.printf(",\"age\":%ld", a);
    o.raw("}");
  }
  o.raw("},\"extras\":{");
  for (int k = 0; k < st.live.n_extras; k++) {
    if (k) o.raw(",");
    o.jsonStr(st.live.extras[k].key);
    o.raw(":");
    o.jsonStr(st.live.extras[k].val);
  }
  // every unknown key the console ever sent (up to 8), last raw value and age, oldest-seen evicted first
  o.raw("},\"known\":[");
  first = true;
  for (const KnownKey& k : st.known) {
    if (!k.t) continue;
    o.raw(first ? "{\"k\":" : ",{\"k\":");
    first = false;
    o.jsonStr(k.key);
    o.raw(",\"v\":");
    o.jsonStr(k.val);
    const long a = ageBefore(st.last_seen, k.t);
    if (a >= 0) o.printf(",\"age\":%ld}", a);
    else o.raw("}");
  }
  o.printf("],\"extras_dropped\":%u,\"id_sanitized\":%s}", (unsigned)st.live.extras_dropped,
           st.live.id_sanitized ? "true" : "false");
}

void renderLiveHead(Out& o, const Clock& c, const Diag* d, const char* page) {
  o.raw("{\"now\":\"");
  iso(o, c.now);
  o.printf("\",\"time_synced\":%s,\"time_src\":\"%s\"", c.synced ? "true" : "false", c.src ? c.src : "none");
  if (page) {
    o.raw(",\"page\":");
    o.jsonStr(page);
  }
  // The AP face passes no diag: no uptime, heap or addresses there
  if (d) {
    o.printf(",\"uptime_s\":%lu", (unsigned long)c.uptime_s);
    diag(o, *d);
  }
}

void renderLive(Out& o, Stations& all, const Clock& c, const Diag* d, const char* page) {
  static LiveSnap snap;  // callers are serialized (httpd task / tests)
  renderLiveHead(o, c, d, page);
  o.raw(",\"stations\":[");
  bool firstSt = true;
  for (int i = 0; i < MAX_STATIONS; i++) {
    const Station* sp = all.at(i);
    if (!sp) continue;
    if (!firstSt) o.raw(",");
    firstSt = false;
    snapLive(*sp, c.ms, &snap);
    renderLiveStation(o, snap, c, d != nullptr);
  }
  o.raw("]}");
}

void renderLiveOne(Out& o, const LiveSnap& s, const Clock& c, const Diag* d, const char* page) {
  renderLiveHead(o, c, d, page);
  o.raw(",\"station\":");
  renderLiveStation(o, s, c, d != nullptr);
  o.raw("}");
}

void liveEtag(char out[48], uint32_t boot, const Station& st, uint32_t now_ms, bool ap, bool synced, uint32_t outlook_in) {
  const uint32_t flags = (st.online(now_ms) ? 0x40000000u : 0) | (ap ? 0x80000000u : 0) | (synced ? 0x20000000u : 0);
  const uint32_t revs = ((st.flash_rev & 0x1fffu) << 16) | (st.cfg_rev & 0xffffu);
  makeEtag(out, boot, st.gen ^ outlook_in, st.uploads, revs | flags);
}

uint32_t outlookNearWord(const OutNear* near) {
  if (!near || !near->ok) return 0;  // module off / not admitted: the null word (changes with /api/metar's on:false)
  uint32_t h = 2166136261u ^ near->wx;
  for (int i = 0; i < 4 && near->icao[i]; i++) h = (h ^ (uint8_t)near->icao[i]) * 16777619u;
  return h | 1u;
}

void histEtag(char out[48], uint32_t boot, const Station& st, HistView h) {
  const uint32_t cfg = st.cfg_rev * 0x9E3779B1u;  // odd multiplier: every revision a distinct word
  switch (h) {
    // flash_rev: hours and days restored from flash change the body without an upload or a close
    case HistView::Recent: makeEtag(out, boot, st.gen ^ cfg, st.uploads, st.flash_rev); break;
    // a calibration log change re-renders every row (calSig)
    case HistView::Hourly:
      makeEtag(out, boot, st.gen ^ calSig(st.cal) ^ cfg, st.uploads, st.hour_closes | (st.flash_rev << 24));
      break;
    case HistView::Daily: makeEtag(out, boot, st.gen ^ calSig(st.cal) ^ cfg, st.hour_closes, st.flash_rev); break;
  }
}

uint32_t outlookEtagIn(const Station& st, float lat, float lon, bool time_ok, const OutNear* near) {
  // + the REL hint (the METAR job steps it between uploads; the notice is in the same body)
  uint32_t w[8] = {st.hour_closes, calSig(st.cal), 0, 0, time_ok ? 1u : 0u, outlookNearWord(near), relSig(st.rel),
                   learnUseSig(st.use)};
  memcpy(&w[2], &lat, 4);
  memcpy(&w[3], &lon, 4);
  uint32_t h = 2166136261u;  // FNV-1a over the words
  for (uint32_t v : w)
    for (int b = 0; b < 4; b++) h = (h ^ ((v >> (8 * b)) & 0xffu)) * 16777619u;
  return h;
}

char stationStateChar(const Station& st, uint32_t now_ms) {
  if (!st.cfg.publish) return 'x';
  if (!stationConfigured(st.cfg)) return 'u';
  return st.online(now_ms) ? 'o' : 's';
}

void renderBridgeStatus(Out& o, const BridgeStatus& b) {
  o.printf("{\"mode\":\"%s\",\"sta\":%d,\"mqtt\":%d,\"time\":%d,\"lock\":%d,\"heap_low\":%d,", b.mode, b.sta ? 1 : 0,
           (int)b.mqtt, b.time ? 1 : 0, b.admin_locked ? 1 : 0, b.heap_low ? 1 : 0);
  o.printf("\"time_src\":\"%s\",", b.time_src ? b.time_src : "none");
  if (b.notices.n) {
    static PosixTz utc;
    static bool init = false;
    if (!init) init = parsePosixTz("UTC0", &utc);
    o.raw("\"notices\":");
    noticesJson(o, b.notices, utc, 0);  // the bridge's texts carry no time
    o.raw(",");
  }
  if (b.ota) o.raw("\"ota\":1,");
  if (b.metar) o.raw("\"metar\":1,");  // airport weather on, an airport chosen
  if (b.store_fail) o.printf("\"store_fail\":%lu,", (unsigned long)b.store_fail);
  if (b.too_long) o.printf("\"too_long\":%lu,", (unsigned long)b.too_long);
  if (b.no_slot) o.printf("\"no_slot\":%lu,", (unsigned long)b.no_slot);
  if (b.no_slot_id[0]) {  // a console turned away for lack of a slot
    o.raw("\"no_slot_id\":");
    o.jsonStr(b.no_slot_id);
    o.raw(",");
  }
  if (b.ap) {
    o.raw("\"ap\":");
    o.jsonStr(b.ap);
    o.raw(",");
  }
  o.raw("\"ids\":[");
  for (int i = 0; i < b.n; i++) {
    if (i) o.raw(",");
    o.jsonStr(b.ids[i]);
  }
  char st[MAX_STATIONS + 1];
  memcpy(st, b.state, (size_t)b.n);
  st[b.n] = 0;
  o.printf("],\"st\":\"%s\"}", st);
}

// ---------------------------------------------------------------- history rows (one shape)

void renderRecent(Out& o, const Station& st) { renderRecent(o, st.id, st.recent, st.cfg.disabled); }

void renderRecent(Out& o, const char* id, const Recent& recent, uint32_t hidden) {
  o.raw("{\"station\":");
  o.jsonStr(id);
  o.raw(",\"fields\":[\"t\"");
  for (int f = 0; f < CORE_FIELDS; f++)
    if (!((hidden >> f) & 1u)) o.printf(",\"%s\"", FIELDS[f].key);
  o.raw("],\"rows\":[");
  for (int i = 0; i < recent.size(); i++) {
    const PackedSample& p = recent.at(i);
    o.printf("%s[%lu", i ? "," : "", (unsigned long)p.t);
    for (int f = 0; f < CORE_FIELDS; f++) {
      if ((hidden >> f) & 1u) continue;  // a hidden field is omitted, the column too
      o.raw(",");
      packed(o, f, p.v[f]);
    }
    o.raw("]");
  }
  o.raw("]}");
}

namespace {
// a number with d decimals, "null" when NaN
void num(Out& o, float v, int d) {
  if (std::isnan(v)) {
    o.raw("null");
    return;
  }
  char b[24];
  snprintf(b, sizeof b, "%.*f", d, (double)v);
  if (!strcmp(b, "-0") || !strcmp(b, "-0.0") || !strcmp(b, "-0.00")) memmove(b, b + 1, strlen(b));
  o.raw(b);
}
float i16f(int16_t x, float s) { return hasI16(x) ? x / s : NAN; }
float u16f(uint16_t x, float s) { return hasU16(x) ? x / s : NAN; }
float u8f(uint8_t x, float s) { return hasU8(x) ? x / s : NAN; }
// "key":[min,avg,max] with null for a missing part; an all-null triple is null
void triple(Out& o, const char* key, float mn, float avg, float mx, int d) {
  o.printf(",\"%s\":", key);
  if (std::isnan(mn) && std::isnan(avg) && std::isnan(mx)) {
    o.raw("null");
    return;
  }
  o.raw("[");
  num(o, mn, d);
  o.raw(",");
  num(o, avg, d);
  o.raw(",");
  num(o, mx, d);
  o.raw("]");
}
void one(Out& o, const char* key, float v, int d) {
  o.printf(",\"%s\":", key);
  num(o, v, d);
}
int decOf(float scale) {
  int d = 0;
  for (float s = scale; s >= 10 && d < 3; s /= 10) d++;
  return d;
}
bool hid(uint32_t hidden, int f) { return (hidden >> f) & 1u; }
void quality(Out& o, uint16_t q) {
  o.raw(",\"q\":[");
  bool first = true;
  for (int b = 0; b < Q_COUNT; b++)
    if ((q >> b) & 1u) {
      o.printf("%s\"%s\"", first ? "" : ",", qualityName(b));
      first = false;
    }
  o.raw("]");
}
const ChMap* mapOf(const RowView& v, uint8_t ver) {
  for (int i = 0; i < v.n_maps; i++)
    if (v.maps[i].ver == ver && ver) return &v.maps[i];
  return nullptr;
}
// the wind's direction and steadiness; direction null when calm / none
void wind(Out& o, uint16_t w, uint32_t hidden) {
  if (hid(hidden, F_WIND_DIR)) return;
  o.raw(",\"wind_dir\":");
  if (dirOf(w) == DIR_NONE) o.raw("null");
  else o.printf("%u", (unsigned)dirOf(w));
  o.raw(",\"wind_steady\":");
  if (dirOf(w) == DIR_NONE) o.raw("null");
  else num(o, steadyOf(w) / 127.0f, 2);
}
}  // namespace

RowView rowView(const Station& st) { return RowView{st.cfg.disabled, &st.chmap, st.chmap.ver ? 1 : 0, &st.cal}; }

void hourRow(Out& o, const HourRec& raw, const RowView& v) {
  HourRec r = raw;  // the stored row is raw; the calibration in force at its hour applies here
  if (v.cal) calHour(&r, calFor(*v.cal, r.hour));
  const uint32_t H = v.hidden;
  o.raw("{\"h\":\"");
  hourKey(o, r.hour);
  o.printf("\",\"n\":%u", (unsigned)r.n);
  quality(o, r.quality);
  o.printf(",\"epoch\":%u,\"rej_fields\":%u", (unsigned)r.epoch, (r.quality >> Q_REJECTED) & 1u);
  if (!hid(H, F_TEMP_C)) triple(o, "temp_c", i16f(r.t_min, 10), i16f(r.t_avg, 10), i16f(r.t_max, 10), 1);
  if (!hid(H, F_DEWPT_C)) triple(o, "dewpt_c", NAN, i16f(r.td_avg, 10), NAN, 1);
  if (!hid(H, F_HUMIDITY)) triple(o, "humidity", u8f(r.rh_min, 1), u8f(r.rh_avg, 1), u8f(r.rh_max, 1), 0);
  if (!hid(H, F_PRESSURE_HPA)) triple(o, "pressure_hpa", NAN, i16f(r.p_avg, 10), NAN, 1);
  if (!hid(H, F_WIND_KMH)) triple(o, "wind_kmh", NAN, u16f(r.wind_avg, 10), NAN, 1);
  if (!hid(H, F_GUST_KMH)) one(o, "gust_kmh", u16f(r.gust_max, 10), 1);
  wind(o, r.wdir_steady, H);
  if (!hid(H, F_WIND_KMH)) o.printf(",\"calm_n\":%u", (unsigned)r.calm_n);
  // the hour's rain comes from the daily counter (dash.js HIST_OF): hiding either field hides it
  if (!hid(H, F_RAIN_MM) && !hid(H, F_RAIN_DAILY_MM)) one(o, "rain_mm", u16f(r.rain_sum, 100), 2);
  if (!hid(H, F_RAIN_DAILY_MM)) o.raw(",\"rain_daily_mm\":null");
  if (!hid(H, F_RAIN_RATE_MMH)) one(o, "rain_rate_max", u8f(r.rain_rate_max, 2), 1);
  if (!hid(H, F_UV)) triple(o, "uv", NAN, NAN, u8f(r.uv_max, 10), 1);
  if (!hid(H, F_SOLAR_WM2)) triple(o, "solar_wm2", NAN, u16f(r.solar_avg, 1), u16f(r.solar_max, 1), 0);
  if (const ChMap* m = mapOf(v, r.ch_ver))
    for (int c = 0; c < HIST_CH; c++) {
      if (!m->key[c][0]) continue;
      const int f = fieldByKey(m->key[c]);
      if (f >= 0 && hid(H, f)) continue;
      const float s = m->scale[c] > 0 ? m->scale[c] : 1;
      triple(o, m->key[c], NAN, i16f(r.ch_avg[c], s), NAN, decOf(s));
    }
  o.raw("}");
}

void dayRow(Out& o, const DayRec& raw, const RowView& v) {
  DayRec r = raw;  // as hourRow, with the entry in force at the day's start
  if (v.cal) calDay(&r, calFor(*v.cal, calDayTime(r.ymd)));
  const uint32_t H = v.hidden;
  o.printf("{\"d\":\"%04lu-%02lu-%02lu\",\"n\":%u,\"hours\":%u,\"clean_hours\":%u", (unsigned long)(r.ymd / 10000),
           (unsigned long)(r.ymd / 100 % 100), (unsigned long)(r.ymd % 100), (unsigned)r.n, (unsigned)r.hours,
           (unsigned)r.clean_hours);
  quality(o, r.quality);
  o.printf(",\"epoch\":%u,\"rej_fields\":%u", (unsigned)r.epoch, (r.quality >> Q_REJECTED) & 1u);
  if (!hid(H, F_TEMP_C)) triple(o, "temp_c", i16f(r.t_min, 10), i16f(r.t_avg, 10), i16f(r.t_max, 10), 1);
  if (!hid(H, F_DEWPT_C)) triple(o, "dewpt_c", i16f(r.td_min, 10), i16f(r.td_avg, 10), i16f(r.td_max, 10), 1);
  if (!hid(H, F_HUMIDITY)) triple(o, "humidity", u8f(r.rh_min, 1), u8f(r.rh_avg, 1), u8f(r.rh_max, 1), 0);
  if (!hid(H, F_PRESSURE_HPA)) triple(o, "pressure_hpa", i16f(r.p_min, 10), i16f(r.p_avg, 10), i16f(r.p_max, 10), 1);
  if (!hid(H, F_WIND_KMH)) triple(o, "wind_kmh", NAN, u16f(r.wind_avg, 10), NAN, 1);
  if (!hid(H, F_GUST_KMH)) one(o, "gust_kmh", u16f(r.gust_max, 10), 1);
  wind(o, r.wdir_steady, H);
  if (!hid(H, F_WIND_KMH)) o.printf(",\"calm_hours\":%u", (unsigned)r.calm_hours);
  if (!hid(H, F_WIND_DIR))
    o.printf(",\"rose\":[%u,%u,%u,%u,%u,%u,%u,%u]", r.rose[0], r.rose[1], r.rose[2], r.rose[3], r.rose[4], r.rose[5],
             r.rose[6], r.rose[7]);
  const bool rain = !hid(H, F_RAIN_MM) && !hid(H, F_RAIN_DAILY_MM);  // either hides the history's rain
  if (rain) one(o, "rain_mm", u16f(r.rain_sum, 100), 2);
  if (!hid(H, F_RAIN_DAILY_MM)) one(o, "rain_daily_mm", u16f(r.rain_sum, 100), 2);
  if (rain) o.printf(",\"rain_hours\":%u", (unsigned)r.rain_hours);
  if (!hid(H, F_RAIN_RATE_MMH)) one(o, "rain_rate_max", u8f(r.rain_rate_max, 2), 1);
  if (!hid(H, F_UV)) triple(o, "uv", NAN, NAN, u8f(r.uv_max, 10), 1);
  if (!hid(H, F_SOLAR_WM2)) triple(o, "solar_wm2", NAN, u16f(r.solar_avg, 1), u16f(r.solar_max, 1), 0);
  if (const ChMap* m = mapOf(v, r.ch_ver))
    for (int c = 0; c < HIST_CH; c++) {
      if (!m->key[c][0]) continue;
      const int f = fieldByKey(m->key[c]);
      if (f >= 0 && hid(H, f)) continue;
      const float s = m->scale[c] > 0 ? m->scale[c] : 1;
      triple(o, m->key[c], i16f(r.ch[c][0], s), i16f(r.ch[c][1], s), i16f(r.ch[c][2], s), decOf(s));
    }
  o.raw("}");
}

void renderHourly(Out& o, const Station& st) { renderHourly(o, st.id, st.hourly, rowView(st)); }

void renderHourly(Out& o, const char* id, const Hourly& h, const RowView& v) {
  o.raw("{\"station\":");
  o.jsonStr(id);
  o.raw(",\"open\":");
  HourRec open;
  if (h.openRecord(&open)) hourRow(o, open, v);
  else o.raw("null");
  o.raw(",\"hours\":[");
  for (int i = 0; i < h.size(); i++) {
    if (i) o.raw(",");
    hourRow(o, h.at(i), v);
  }
  o.raw("]}");
}

void renderDaily(Out& o, const Station& st, int days, const PosixTz& tz) {
  DayRec open;
  const bool haveOpen = st.daily.openRecord(st.hourly, tz, &open);
  renderDaily(o, st.id, st.daily, haveOpen ? &open : nullptr, days, rowView(st));
}

void renderDaily(Out& o, const char* id, const Daily& d, const DayRec* open, int days, const RowView& v) {
  o.raw("{\"station\":");
  o.jsonStr(id);
  o.raw(",\"open\":");
  if (open) dayRow(o, *open, v);
  else o.raw("null");
  o.raw(",\"days\":[");
  const int n = d.size();
  const int from = days > 0 && days < n ? n - days : 0;
  for (int i = from; i < n; i++) {
    if (i > from) o.raw(",");
    dayRow(o, d.at(i), v);
  }
  o.raw("]}");
}

// Records [first key >= from, key < to) of a flash ring, one read at a time (no heap copy); a record with a bad CRC or
// a key out of order (a segment rotated while streaming) is skipped. Returns the rows written.
static uint32_t gStreamAborts = 0;  // httpd task only
constexpr size_t STREAM_RUN_BYTES = 384;  // 8 hourly / 4 daily records per flash read
// one request at a time (httpd task): one buffer for both record kinds (a static inside the template would be two)
alignas(8) static uint8_t gRun[STREAM_RUN_BYTES];
uint32_t streamAborts() { return gStreamAborts; }

template <typename Rec, typename Row>
static int streamRing(Out& o, const FlashRing& ring, uint32_t from, uint32_t to, int newest, Row row, const RowView& v,
                      bool (*valid)(const Rec&)) {
  int i = newest > 0 ? (ring.count() > newest ? ring.count() - newest : 0) : ring.lowerBound(from);
  int written = 0;
  uint32_t last = 0;
  // records come in runs (one file read per run, not one open per record), into gRun
  uint8_t* const run = gRun;
  constexpr int RUN = (int)(STREAM_RUN_BYTES / sizeof(Rec));
  int have = 0, at = 0;  // records in the buffer, and the ring index of its first
  for (; i < ring.count(); i++) {
    if (!o.ok()) {  // the client is gone: stop reading flash
      gStreamAborts++;
      break;
    }
    if (i >= at + have) {
      at = i;
      have = ring.readRun(i, run, RUN);
      if (!have) continue;  // unreadable: the next record tries again
    }
    const Rec& r = *(const Rec*)(run + (size_t)(i - at) * sizeof(Rec));
    if (!valid(r)) continue;
    uint32_t key;
    memcpy(&key, &r, sizeof key);
    if (key > ring.newestKey() || key < ring.oldestKey()) continue;  // rotated in after the copy: not this view's
    if (newest <= 0 && key < from) continue;
    if (newest <= 0 && key >= to) break;
    if (written && key <= last) continue;
    last = key;
    if (written++) o.raw(",");
    row(o, r, v);
  }
  return written;
}

void renderHourlyRing(Out& o, const char* id, const FlashRing& ring, uint32_t from, uint32_t to, const HourRec* open,
                      const RowView& v) {
  o.raw("{\"station\":");
  o.jsonStr(id);
  o.raw(",\"open\":");
  if (open && open->hour >= from && open->hour < to) hourRow(o, *open, v);
  else o.raw("null");
  o.raw(",\"hours\":[");
  streamRing<HourRec>(o, ring, from, to, 0, hourRow, v, hourValid);
  o.raw("]}");
}

void renderDailyRing(Out& o, const char* id, const FlashRing& ring, uint32_t from_ymd, uint32_t to_ymd, int newest,
                     const DayRec* open, const RowView& v) {
  o.raw("{\"station\":");
  o.jsonStr(id);
  o.raw(",\"open\":");
  if (open && (newest > 0 || (open->ymd >= from_ymd && open->ymd < to_ymd))) dayRow(o, *open, v);
  else o.raw("null");
  o.raw(",\"days\":[");
  streamRing<DayRec>(o, ring, from_ymd, to_ymd, newest, dayRow, v, dayValid);
  o.raw("]}");
}

static void errorsJson(Out& o, const Errors* err) {
  o.raw(",\"errors\":[");
  if (err) {
    for (int i = 0; i < err->n; i++) {
      if (i) o.raw(",");
      o.jsonStr(err->at(i));
    }
    if (err->dropped) o.printf("%s\"... %d more\"", err->n ? "," : "", err->dropped);
  }
  o.raw("]");
}

void renderStationConfig(Out& o, const Station& st, const Errors* err) {
  renderStationConfig(o, st.id, st.stored, st.cfg, err);
}

void renderStationConfig(Out& o, const char* id, bool stored, const StationConfig& cfg, const Errors* err,
                         const KeyState* key) {
  o.raw("{\"station\":");
  o.jsonStr(id);
  o.printf(",\"configured\":%s,\"stored\":%s", stationConfigured(cfg) ? "true" : "false",
           stored ? "true" : "false");
  if (key) o.printf(",\"key\":\"%s\",\"key_mismatch\":%lu", key->bound ? "bound" : "none", (unsigned long)key->mismatch);
  if (key && key->has_mac)
    o.printf(",\"src_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"", key->mac[0], key->mac[1], key->mac[2], key->mac[3],
             key->mac[4], key->mac[5]);
  else if (key)
    o.raw(",\"src_mac\":null");
  if (key) o.printf(",\"fp\":\"%s\",\"cal_n\":%u,\"learn_use\":%u", fingerprintName(key->fp), (unsigned)key->cal_n,
                   (unsigned)key->learn_use);
  if (key && key->adopt[0]) {
    o.raw(",\"adopt\":");
    o.jsonStr(key->adopt);
  }
  if (err) errorsJson(o, err);
  o.raw(",\"effective\":");
  stationEffectiveJson(o, cfg);
  o.raw(",\"overrides\":");
  stationToJson(o, cfg);
  o.raw("}");
}

void renderBridgeConfig(Out& o, const BridgeConfig& b, const Errors* err) {
  o.raw("{\"effective\":");
  bridgeEffectiveJson(o, b);
  o.raw(",\"overrides\":");
  bridgeToJson(o, b);
  if (err) errorsJson(o, err);
  o.raw("}");
}

// ---------------------------------------------------------------- HTML

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta http-equiv=\"refresh\" content=\"30\"><title>fauxcloud</title><style>"
    ":root{--bg:#fafafa;--fg:#1d1d1f;--mut:#6e6e73;--line:#d2d2d7;--card:#fff;--a:#0a6cbd;--b:#c2410c;--bad:#b91c1c}"
    "@media(prefers-color-scheme:dark){:root{--bg:#161618;--fg:#ececf0;--mut:#9a9aa2;--line:#3a3a40;"
    "--card:#202024;--a:#5aa9ec;--b:#f08a4b;--bad:#f87171}}"
    "body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px/1.4 system-ui,sans-serif}"
    "h1{font-size:20px;margin:0 0 4px}h2{font-size:16px;margin:0 0 8px}.mut{color:var(--mut)}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin:12px 0;"
    "max-width:760px}table{border-collapse:collapse;width:100%}td,th{padding:3px 6px;text-align:right;"
    "border-bottom:1px solid var(--line)}td:first-child,th:first-child{text-align:left}"
    // only the scrolled hourly table keeps its cells on one line; the field table wraps on a phone
    ".wrap td,.wrap th{white-space:nowrap}td{overflow-wrap:anywhere}"
    ".bad{color:var(--bad)}.on{color:#15803d}.wrap{overflow-x:auto}svg{display:block;width:100%;height:auto}"
    "</style></head><body>";

static void sparkline(Out& o, const Station& st, int f, const char* color) {
  const int W = 360, H = 70, PAD = 4;
  int n = st.recent.size();
  float lo = 0, hi = 0;
  int have = 0;
  for (int i = 0; i < n; i++) {
    int16_t r = st.recent.at(i).v[f];
    if (!hasValue(r)) continue;
    float v = unpack(f, r);
    if (!have || v < lo) lo = v;
    if (!have || v > hi) hi = v;
    have++;
  }
  o.printf("<p class=\"mut\" style=\"margin:8px 0 2px\">%s, last %d uploads", FIELDS[f].label, n);
  if (have) {
    o.raw(" (");
    value(o, f, lo);
    o.raw(" &ndash; ");
    value(o, f, hi);
    o.printf(" %s)", FIELDS[f].unit);
  }
  o.printf("</p><svg viewBox=\"0 0 %d %d\" role=\"img\" aria-label=\"%s\">", W, H, FIELDS[f].label);
  o.printf("<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"var(--line)\"/>", W, H);
  if (have >= 2) {
    float span = hi - lo;
    if (span < 1e-6f) span = 1;
    o.printf("<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"2\" points=\"", color);
    for (int i = 0; i < n; i++) {
      int16_t r = st.recent.at(i).v[f];
      if (!hasValue(r)) continue;
      float x = n > 1 ? (float)i * (W - 2 * PAD) / (n - 1) + PAD : W / 2.0f;
      float y = H - PAD - (unpack(f, r) - lo) / span * (H - 2 * PAD);
      o.printf("%.1f,%.1f ", (double)x, (double)y);
    }
    o.raw("\"/>");
  }
  o.raw("</svg>");
}

// the /tools page's hourly table (from HourRec, raw values)
static void cell3(Out& o, float mn, float avg, float mx, int d) {
  o.raw("<td>");
  if (std::isnan(avg) && std::isnan(mx)) {
    o.raw("&ndash;");
  } else if (!std::isnan(mn)) {
    num(o, mn, d);
    o.raw(" / <b>");
    num(o, avg, d);
    o.raw("</b> / ");
    num(o, mx, d);
  } else {
    num(o, std::isnan(avg) ? mx : avg, d);
  }
  o.raw("</td>");
}

static void hourlyRow(Out& o, const HourRec& r, bool open) {
  o.raw("<tr><td>");
  hourKey(o, r.hour);
  o.printf("%s</td><td>%u</td><td%s>%s</td>", open ? " (open)" : "", (unsigned)r.n,
           r.quality ? " class=\"bad\"" : "", r.quality ? "!" : "");
  cell3(o, i16f(r.t_min, 10), i16f(r.t_avg, 10), i16f(r.t_max, 10), 1);
  cell3(o, u8f(r.rh_min, 1), u8f(r.rh_avg, 1), u8f(r.rh_max, 1), 0);
  cell3(o, NAN, i16f(r.p_avg, 10), NAN, 1);
  cell3(o, NAN, u16f(r.wind_avg, 10), NAN, 1);
  cell3(o, NAN, NAN, u16f(r.gust_max, 10), 1);
  o.raw("<td>");
  if (dirOf(r.wdir_steady) == DIR_NONE) o.raw("&ndash;");
  else o.printf("%u", (unsigned)dirOf(r.wdir_steady));
  o.raw("</td>");
  cell3(o, NAN, u16f(r.rain_sum, 100), NAN, 2);
  o.raw("</tr>");
}

void renderPageHead(Out& o, const Clock& c, const char* sw_version, const Diag* d) {
  o.raw(PAGE_HEAD);
  o.raw("<h1>fauxcloud</h1><p class=\"mut\">");
  o.htmlStr(sw_version);
  o.raw(" &middot; ");
  if (c.synced) iso(o, c.now);
  else o.raw("clock not synced");
  o.printf(" &middot; up %lu s &middot; refreshes every 30 s</p>", (unsigned long)c.uptime_s);
  if (d && d->admin_locked)
    o.printf("<div class=\"card bad\">Admin login locked for %lu s after %d failed attempts.</div>",
             (unsigned long)d->admin_lock_s, d->admin_failures);
}

void renderStationCard(Out& o, const Station& st, const Clock& c) {
    const uint32_t age = c.now - st.last_seen;
    char name[64];
    o.raw("<div class=\"card\"><h2>");
    o.htmlStr(st.displayName(name, sizeof name));
    if (st.cfg.name[0]) {
      o.raw(" <span class=\"mut\">");
      o.htmlStr(st.id);
      o.raw("</span>");
    }
    o.printf(" <span class=\"%s\">%s</span></h2>", st.online(c.ms) ? "on" : "bad",
             st.online(c.ms) ? "online" : "stale");
    o.printf("<p class=\"mut\">last seen %lu s ago &middot; %lu uploads</p>", (unsigned long)age,
             (unsigned long)st.uploads);

    o.raw("<table><tr><th>Field</th><th>Value</th><th>Rejected since boot</th></tr>");
    for (int f = 0; f < F_COUNT; f++) {
      const bool has = st.live.has(f);
      const bool rej = (st.live_rejected >> f) & 1u;
      if (!has && !rej && !st.rejects[f]) continue;
      o.printf("<tr><td>%s</td><td>", FIELDS[f].label);
      if (has) {
        value(o, f, st.live.v[f]);
        o.printf(" %s", FIELDS[f].unit);
      } else {
        o.raw(rej ? "<span class=\"bad\">rejected</span>" : "&ndash;");
      }
      o.printf("</td><td>%lu</td></tr>", (unsigned long)st.rejects[f]);
    }
    o.raw("</table>");

    if (st.live.n_extras) {
      o.raw("<p class=\"mut\" style=\"margin:8px 0 2px\">Unknown parameters (last upload)</p><table>");
      for (int k = 0; k < st.live.n_extras; k++) {
        o.raw("<tr><td>");
        o.htmlStr(st.live.extras[k].key);
        o.raw("</td><td>");
        o.htmlStr(st.live.extras[k].val);
        o.raw("</td></tr>");
      }
      o.raw("</table>");
    }

    sparkline(o, st, F_WIND_KMH, "var(--a)");
    sparkline(o, st, F_TEMP_C, "var(--b)");

    o.raw("<p class=\"mut\" style=\"margin:12px 0 2px\">Hourly, calibrated (min / <b>avg</b> / max; gust = "
          "max; rain = fallen in the hour; ! = quality flags)</p><div class=\"wrap\"><table><tr><th>Hour (UTC)</th>"
          "<th>n</th><th>q</th>");
    for (int f : {F_TEMP_C, F_HUMIDITY, F_PRESSURE_HPA, F_WIND_KMH, F_GUST_KMH, F_WIND_DIR, F_RAIN_MM})
      o.printf("<th>%s</th>", FIELDS[f].label);
    o.raw("</tr>");
    HourRec open;  // calibrated through the same log as the API
    if (st.hourly.openRecord(&open)) {
      calHour(&open, calFor(st.cal, open.hour));
      hourlyRow(o, open, true);
    }
    for (int k = st.hourly.size() - 1; k >= 0; k--) {
      HourRec r = st.hourly.at(k);
      calHour(&r, calFor(st.cal, r.hour));
      hourlyRow(o, r, false);
    }
    o.raw("</table></div></div>");
}

void renderPageTail(Out& o, bool any, bool links) {
  if (!any) o.raw("<div class=\"card\">No upload received yet.</div>");
  if (!links) {  // the AP side answers the API with `success`, so no JSON links there
    o.raw("</body></html>");
    return;
  }
  o.raw("<p class=\"mut\">JSON: <a href=\"/api/live\">/api/live</a> &middot; <a href=\"/api/recent\">/api/recent</a>"
        " &middot; <a href=\"/api/hourly\">/api/hourly</a></p></body></html>");
}

void renderPage(Out& o, Stations& all, const Clock& c, const char* sw_version, const Diag* d) {
  renderPageHead(o, c, sw_version, d);
  bool any = false;
  for (int i = 0; i < MAX_STATIONS; i++)
    if (const Station* sp = all.at(i)) {
      renderStationCard(o, *sp, c);
      any = true;
    }
  renderPageTail(o, any);
}

}  // namespace fc
