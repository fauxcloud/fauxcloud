// notices.cpp -- see notices.h.
#include "notices.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ha_discovery.h"
#include "history.h"
#include "rel_check.h"
#include "station.h"
#include "web_render.h"

namespace fc {

// clang-format off
// what happened and the one thing to do; ids are stable; every text <= 127 B at its worst-case expansion (the time as
// "yesterday 23:59") except the two pressure ones (<= 191 B; noticeText's buffers are NOTICE_TEXT_MAX; host-tested); ASCII
// apostrophes (HA states, automations)
const NoticeDef NOTICES[NK_COUNT] = {
  {"sensor_stale",      "Sensor not updating",     "Outdoor sensor not updating since %s. A calm night can do this; a long gap means its battery or radio."},
  {"sensor_absent",     "Sensor missing",          "Outdoor sensor missing since %s: check its batteries. A short battery-out of the console helps it find the sensor."},
  {"baro_uncalibrated", "Pressure at factory value", "The console's pressure is at its factory value; set REL once in its SET menu."},
  {"key_mismatch",      "Key changed",             "Another key arrived for this station (%lu uploads ignored). Re-paired the console? Accept it in Settings."},
  {"console_clock",     "Console clock off",       "The console's clock is off by %s; switch NTP off and on in its SET menu."},
  {"time_approximate",  "Time approximate",        "Time is estimated since the restart. Sign in to the admin page once on your phone, or connect fauxcloud to the internet."},
  {"time_none",         "Waiting for time",        "Waiting for the real time. Sign in to the admin page once on your phone, or connect fauxcloud to the internet."},
  {"heap_low",          "Low memory",              "Memory is low; readings still arrive. If this stays, restart fauxcloud in Settings."},
  {"store_fail",        "History not saved",       "History not saved (%lu writes failed); readings still arrive. If this stays, restart fauxcloud in Settings."},
  {"heap_frag_restart", "Restarted",               "fauxcloud restarted itself to free memory; nothing to do."},
  {"profile_changed",   "Profile changed",         "Station profile changed: %s."},
  {"store_layout",      "History not saved",       "History not saved: this firmware needs one USB flash to prepare its storage (see the README)."},
  {"clock_behind",      "Clock behind",            "The time is behind the saved history, so history is paused until the time is confirmed (at most 24 h)."},
  // one chip for baro_ref and baro_check; d, above|below, since, target (custom in noticeText)
  {"baro_ref",          "Pressure looks off",      "This station's pressure reads about %s %s nearby airports since %s. Set REL in the console's SET menu to about %s."},
  // baro_check in the same shape (what, against what, since when + the one thing to do); d, above|below, days
  {"baro_check",        "Pressure looks off",      "This station's pressure averaged about %s %s the usual for this place over %s days. Set REL in the console's SET menu to the current sea-level pressure of a nearby official station."},
};
// baro_ref without a current reference report (the rel_off_old ending): no number to type
static const char BARO_REF_OLD[] = "This station's pressure reads about %s %s nearby airports since %s. Set REL in the console's SET menu to the current sea-level pressure of a nearby official station.";
// the count sentences for a count of one, whole (no plural built from parts)
static const char KEY_MISMATCH_ONE[] = "Another key arrived for this station (1 upload ignored). Re-paired the console? Accept it in Settings.";
static const char STORE_FAIL_ONE[] = "History not saved (1 write failed); readings still arrive. If this stays, restart fauxcloud in Settings.";
// clang-format on

// the skew rounded to what the text shows (min below 1.5 h, h below 2 days, days above), so the
// notice is republished when the text changes, not with every upload's second of jitter
int32_t clockSkewShown(int32_t s) {
  const int64_t a = s < 0 ? -(int64_t)s : s;
  const int64_t u = a >= 2 * 86400 ? 86400 : a >= 5400 ? 3600 : 60;
  const int64_t r = (u == 86400 ? a / u : (a + u / 2) / u) * u;
  return (int32_t)(s < 0 ? -r : r);
}

uint32_t baroCheckCount(int off, uint32_t days) {
  const uint32_t a = (uint32_t)(off < 0 ? -off : off);
  return (days > 0xFFFF ? 0xFFFF : days) | (a > 0x7FFF ? 0x7FFF : a) << 16 | (off < 0 ? 0x80000000u : 0);
}

uint32_t noticesSig(const Notices& ns) {
  uint32_t h = 2166136261u ^ ns.n;
  for (int i = 0; i < ns.n; i++) {
    const uint32_t w[4] = {ns.item[i].kind, ns.item[i].since, ns.item[i].count,
                           (((uint32_t)(uint16_t)ns.item[i].a << 16) | (uint16_t)ns.item[i].b) ^ ((uint32_t)(uint16_t)ns.item[i].q << 8)};
    for (uint32_t x : w)
      for (int b = 0; b < 4; b++) h = (h ^ ((x >> (8 * b)) & 0xffu)) * 16777619u;
  }
  return h;
}

// the notice's time: "14:05" today, "yesterday 14:05", "16 Nov 14:05" further back (as the page's dayTime); HH:MM alone
// without a real `now` (and for a time after it); "--:--" before the clock was set
static void sinceText(char* out, size_t cap, uint32_t since, uint32_t now, const PosixTz& tz) {
  if (since < EPOCH_MIN) {
    snprintf(out, cap, "--:--");
    return;
  }
  const int64_t local = (int64_t)since + localOffset(tz, since);
  const int64_t day = local >= 0 ? local / 86400 : (local - 86399) / 86400, sod = local - day * 86400;
  int64_t dd = 0;
  if (now >= EPOCH_MIN) {
    const int64_t ln = (int64_t)now + localOffset(tz, now);
    dd = (ln >= 0 ? ln / 86400 : (ln - 86399) / 86400) - day;
  }
  const int h = (int)(sod / 3600), m = (int)(sod % 3600 / 60);
  if (dd <= 0) {
    snprintf(out, cap, "%02d:%02d", h, m);
  } else if (dd == 1) {
    snprintf(out, cap, "yesterday %02d:%02d", h, m);
  } else {
    static const char* const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // the civil date of the local day (days from 1970-01-01, Howard Hinnant's algorithm)
    const int64_t z = day + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const int d = (int)(doy - (153 * mp + 2) / 5 + 1), mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    snprintf(out, cap, "%d %s %02d:%02d", d, MON[mo - 1], h, m);
  }
}

int noticeText(char* buf, size_t cap, const Notice& x, const PosixTz& tz, uint32_t now) {
  if (x.kind >= NK_COUNT || !cap) return -1;
  char hm[24];
  sinceText(hm, sizeof hm, x.since, now, tz);
  const char* t = NOTICES[x.kind].text;
  int n;
  if (x.kind == NK_CONSOLE_CLOCK) {  // "... by 23 h" / "... by 12 min"
    const int32_t s = (int32_t)x.count;
    const uint32_t a = s < 0 ? (uint32_t)(-(int64_t)s) : (uint32_t)s;
    char amt[24];
    if (a >= 2 * 86400) snprintf(amt, sizeof amt, "%lu days", (unsigned long)(a / 86400));
    else if (a >= 5400) snprintf(amt, sizeof amt, "%lu h", (unsigned long)((a + 1800) / 3600));
    else snprintf(amt, sizeof amt, "%lu min", (unsigned long)((a + 30) / 60));
    n = snprintf(buf, cap, t, amt);
  } else if (x.kind == NK_BARO_REF) {  // "... about 4.0 hPa above nearby airports since 08:40 ... to about 1009.3 hPa."
    int a = x.a < 0 ? -(int)x.a : x.a;
    if (a > 9999) a = 9999;
    char d[16], tg[16];
    snprintf(d, sizeof d, "%d.%d hPa", a / 10, a % 10);
    const char* dir = x.a < 0 ? "below" : "above";
    if (x.b == REL_NONE) {
      n = snprintf(buf, cap, BARO_REF_OLD, d, dir, hm);
    } else {
      snprintf(tg, sizeof tg, "%.1f hPa", x.b / 10.0);
      n = snprintf(buf, cap, t, d, dir, hm, tg);
    }
  } else if (x.kind == NK_BARO_CHECK) {  // "... about 25 hPa above the usual for this place over 30 days. ..."
    char d[16], days[8];
    snprintf(d, sizeof d, "%lu hPa", (unsigned long)(x.count >> 16 & 0x7FFF));
    // the admitted days, shown up to 60 (a running mean with a 30-day half-life: the last 60 days
    // carry it)
    const unsigned long nd = x.count & 0xFFFF;
    snprintf(days, sizeof days, "%lu", nd > 60 ? 60ul : nd);
    n = snprintf(buf, cap, t, d, x.count >> 31 ? "below" : "above", days);
  } else if (x.count == 1 && (x.kind == NK_KEY_MISMATCH || x.kind == NK_STORE_FAIL)) {
    n = snprintf(buf, cap, "%s", x.kind == NK_KEY_MISMATCH ? KEY_MISMATCH_ONE : STORE_FAIL_ONE);
  } else if (strstr(t, "%s")) n = snprintf(buf, cap, t, hm);
  else if (strstr(t, "%lu")) n = snprintf(buf, cap, t, (unsigned long)x.count);
  else n = snprintf(buf, cap, "%s", t);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

int noticesState(char* buf, size_t cap, const Notices& ns, const PosixTz& tz, uint32_t now) {
  if (cap < 8) return -1;
  const size_t lim = cap - 1 < NOTICE_STATE_MAX ? cap - 1 : NOTICE_STATE_MAX;
  if (!ns.n) return snprintf(buf, cap, "OK");
  static char all[NOTICES_MAX * 128];
  size_t len = 0;
  all[0] = 0;
  for (int i = 0; i < ns.n; i++) {
    char one[NOTICE_TEXT_MAX];
    if (noticeText(one, sizeof one, ns.item[i], tz, now) < 0) continue;
    const int k = snprintf(all + len, sizeof all - len, "%s%s", len ? " " : "", one);  // sentences
    if (k < 0 || len + (size_t)k >= sizeof all) break;
    len += (size_t)k;
  }
  if (len > lim) {  // the sentences do not fit: the chips instead ("Waiting for time. Low memory."), each once
    len = 0;
    all[0] = 0;
    for (int i = 0; i < ns.n; i++) {
      char one[48];
      snprintf(one, sizeof one, "%s.", NOTICES[ns.item[i].kind].chip);
      if (strstr(all, one)) continue;  // two kinds share "History not saved"
      const int k = snprintf(all + len, sizeof all - len, "%s%s", len ? " " : "", one);
      if (k < 0 || len + (size_t)k >= sizeof all) break;
      len += (size_t)k;
    }
  }
  if (len <= lim) {
    memcpy(buf, all, len + 1);
    return (int)len;
  }
  // cut on a UTF-8 boundary, leaving room for "…" (3 bytes)
  size_t cut = lim - 3;
  while (cut && ((uint8_t)all[cut] & 0xC0) == 0x80) cut--;
  memcpy(buf, all, cut);
  memcpy(buf + cut, "\xE2\x80\xA6", 4);
  return (int)(cut + 3);
}

// ,"since":"...","n":N,"skew_s":S -- the parameters of one item; "" when it has none
static int noticeParams(char* buf, size_t cap, const Notice& x) {
  size_t n = 0;
  buf[0] = 0;
  if (x.since >= EPOCH_MIN) {
    char iso[24];
    formatIsoUtc(iso, sizeof iso, x.since);
    n += (size_t)snprintf(buf + n, cap - n, ",\"since\":\"%s\"", iso);
  }
  if (x.kind == NK_KEY_MISMATCH || x.kind == NK_STORE_FAIL)
    n += (size_t)snprintf(buf + n, cap - n, ",\"n\":%lu", (unsigned long)x.count);
  if (x.kind == NK_CONSOLE_CLOCK) n += (size_t)snprintf(buf + n, cap - n, ",\"skew_s\":%ld", (long)(int32_t)x.count);
  const bool rel = x.kind == NK_BARO_REF, uncal = x.kind == NK_BARO_UNCAL && x.a != REL_NONE && x.count >= EPOCH_MIN;
  if (rel || uncal) {  // the pressures in hPa, then the time behind them
    n += (size_t)snprintf(buf + n, cap - n, rel ? ",\"d_hpa\":%.1f" : ",\"last_hpa\":%.1f", x.a / 10.0);
    if (rel) n += (size_t)snprintf(buf + n, cap - n, ",\"qnh_hpa\":%.1f", x.q / 10.0);
    if (x.b == REL_NONE) n += (size_t)snprintf(buf + n, cap - n, ",\"target_hpa\":null");
    else n += (size_t)snprintf(buf + n, cap - n, ",\"target_hpa\":%.1f", x.b / 10.0);
    if (x.count >= EPOCH_MIN) {
      char iso[24];
      formatIsoUtc(iso, sizeof iso, x.count);
      n += (size_t)snprintf(buf + n, cap - n, rel ? ",\"ref_t\":\"%s\"" : ",\"last_t\":\"%s\"", iso);
    }
  }
  if (x.kind == NK_BARO_CHECK)  // the offset (hPa, signed, rounded to 5) and the days
    n += (size_t)snprintf(buf + n, cap - n, ",\"off_hpa\":%ld,\"days\":%lu",
                          (long)(x.count >> 31 ? -(long)(x.count >> 16 & 0x7FFF) : (long)(x.count >> 16 & 0x7FFF)),
                          (unsigned long)(x.count & 0xFFFF));
  return n < cap ? (int)n : -1;
}

int noticesAttributes(char* buf, size_t cap, const Notices& ns) {
  size_t n = 0;
  int k = snprintf(buf, cap, "{\"items\":[");
  if (k < 0 || (size_t)k >= cap) return -1;
  n = (size_t)k;
  bool any = false;  // the comma follows the items written, not the index (an unknown kind is skipped)
  for (int i = 0; i < ns.n; i++) {
    const Notice& x = ns.item[i];
    if (x.kind >= NK_COUNT) continue;
    char par[128];
    if (noticeParams(par, sizeof par, x) < 0) return -1;
    k = snprintf(buf + n, cap - n, "%s{\"id\":\"%s\"%s}", any ? "," : "", NOTICES[x.kind].id, par);
    if (k < 0 || n + (size_t)k >= cap) return -1;
    n += (size_t)k;
    any = true;
  }
  k = snprintf(buf + n, cap - n, "]}");
  if (k < 0 || n + (size_t)k >= cap) return -1;
  return (int)(n + (size_t)k);
}

void noticesJson(Out& o, const Notices& ns, const PosixTz& tz, uint32_t now) {
  o.raw("[");
  bool any = false;  // the comma follows the items written, not the index (an item that fails to format is skipped)
  for (int i = 0; i < ns.n; i++) {
    char t[NOTICE_TEXT_MAX];
    if (noticeText(t, sizeof t, ns.item[i], tz, now) < 0) continue;
    char par[128];
    if (noticeParams(par, sizeof par, ns.item[i]) < 0) continue;
    o.printf("%s{\"id\":\"%s\"%s,\"chip\":\"%s\",\"text\":", any ? "," : "", NOTICES[ns.item[i].kind].id, par,
             NOTICES[ns.item[i].kind].chip);
    o.jsonStr(t);
    o.raw("}");
    any = true;
  }
  o.raw("]");
}

Notices stationNotices(const Station& st) {
  Notices ns = {};
  if (st.sensor_stale) ns.add(st.sensor_absent ? NK_SENSOR_ABSENT : NK_SENSOR_STALE, st.stale_since);
  if (st.baro_uncal) {  // with the last good value (<= 3 h) and the number to type for it
    float p;
    uint32_t t;
    if (lastGoodPressure(st, &p, &t))
      ns.add(NK_BARO_UNCAL, 0, t, (int16_t)lroundf(p * 10), (int16_t)lroundf((p - st.cfg.pressure_hpa_offset) * 10));
    else
      ns.add(NK_BARO_UNCAL, 0, 0, REL_NONE, REL_NONE);
  } else if (relShown(st.rel, st.last_seen)) {  // baro_ref, never under baro_uncalibrated
    ns.add(NK_BARO_REF, st.rel.since, st.rel.ref_t, relMedian(st.rel),
           relTarget(st.rel, st.cfg.pressure_hpa_offset, st.last_seen), st.rel.qnh_dhpa);
  } else if (st.use.l7_flag && !st.use.metar) {
    // L7's pressure check -- the learner's flag (stage >= 1), re-judged with the bridge's offset in force now
    // (a fix in Settings clears it at once; held while >= 9 hPa). One pressure chip at most,
    // baro_uncalibrated > baro_ref > baro_check; silent while METAR is eligible (baro_ref owns pressure then)
    const long off = st.use.l7_off + std::lround(st.cfg.pressure_hpa_offset * 10.0f);  // 0.1 hPa
    if (off >= 90 || off <= -90) {
      const long d5 = (off < 0 ? -1 : 1) * (((off < 0 ? -off : off) + 25) / 50 * 5);  // hPa, to 5
      ns.add(NK_BARO_CHECK, 0, baroCheckCount((int)d5, st.use.l7_days));
    }
  }
  if (st.key_mismatch) ns.add(NK_KEY_MISMATCH, 0, st.key_mismatch);
  if (st.clock_off) ns.add(NK_CONSOLE_CLOCK, 0, (uint32_t)clockSkewShown(st.clock_skew_s));
  // a change after the last upload counts as fresh (negative age)
  if (st.profile_t && (int32_t)(st.last_seen - st.profile_t) < (int32_t)PROFILE_NOTICE_S)
    ns.add(NK_PROFILE_CHANGED, st.profile_t);
  return ns;
}

Notices bridgeNotices(const BridgeNoticeIn& in) {
  Notices ns = {};
  if (in.clock_held) ns.add(NK_CLOCK_BEHIND, in.boot_t);
  else if (!in.time_valid) ns.add(in.time_approx ? NK_TIME_APPROX : NK_TIME_NONE, in.boot_t);
  if (in.heap_low) ns.add(NK_HEAP_LOW, in.heap_low_since);
  if (in.store_fail) ns.add(NK_STORE_FAIL, 0, in.store_fail);
  if (in.frag_restart) ns.add(NK_HEAP_FRAG, in.boot_t);
  if (in.store_layout) ns.add(NK_STORE_LAYOUT, 0);
  return ns;
}

}  // namespace fc
