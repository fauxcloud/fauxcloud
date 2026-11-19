// metar_store.cpp -- see metar_store.h.
#include <cmath>
#include <cstdio>
#include "metar_store.h"

#include <string.h>

#include "ha_discovery.h"  // formatIsoUtc
#include "json.h"
#include "sun.h"           // the airport's own day / night
#include "web_render.h"    // Out

namespace fc {

void metarCfgDefault(MetarCfg* c) {
  memset(c, 0, sizeof *c);
  c->ver = METAR_CFG_V1;
}

static bool icaoOk(const char* s) {
  if (strnlen(s, 5) != 4 || s[0] < 'A' || s[0] > 'Z') return false;
  for (int i = 1; i < 4; i++)
    if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
  return true;
}

bool metarCfgValid(const MetarCfg& c) {
  if (c.ver != METAR_CFG_V1 || c.on > 1 || c.n > METAR_AIRPORTS_MAX) return false;
  for (int i = 0; i < c.n; i++) {
    const Airport& a = c.a[i];
    if (!icaoOk(a.icao) || memchr(a.name, 0, sizeof a.name) == nullptr) return false;
    if (a.lat < -9000 || a.lat > 9000 || a.lon < -18000 || a.lon > 18000) return false;
    for (int k = 0; k < i; k++)
      if (!strcmp(c.a[k].icao, a.icao)) return false;
  }
  return true;
}

int metarCfgFind(const MetarCfg& c, const char* icao) {
  for (int i = 0; i < c.n && i < METAR_AIRPORTS_MAX; i++)
    if (!strncmp(c.a[i].icao, icao, 4) && strlen(icao) == 4) return i;
  return -1;
}

void metarCfgShortNames(MetarCfg* c) {
  for (int i = 0; i < c->n && i < METAR_AIRPORTS_MAX; i++)
    if (!metarNameUser(*c, i)) airportShortName(c->a[i].name);
}

bool metarSameSet(const MetarCfg& a, const MetarCfg& b) {
  if (a.n != b.n) return false;
  for (int i = 0; i < a.n && i < METAR_AIRPORTS_MAX; i++)
    if (metarCfgFind(b, a.a[i].icao) < 0) return false;
  return true;
}

void metarStoreClear(MetarStore* s) {
  const uint32_t gen = s->gen;
  memset(s, 0, sizeof *s);
  s->gen = gen + 1;
}

int metarStorePut(MetarStore* s, const MetarCfg& c, const MetarObs& o) {
  if (!(o.flags & MO_VALID)) return -1;
  const int i = metarCfgFind(c, o.icao);
  if (i < 0) return -1;
  const MetarObs& old = s->obs[i];
  const bool have = old.flags & MO_VALID;
  if (have && (o.obs_unix < old.obs_unix || (o.obs_unix == old.obs_unix && !(o.flags & MO_COR)))) return -1;
  s->obs[i] = o;
  s->gen++;
  return i;
}

int metarStorePutTaf(MetarStore* s, const MetarCfg& c, const TafObs& t) {
  const int i = metarCfgFind(c, t.icao);
  if (i < 0) return -1;
  const TafObs& old = s->taf[i];
  if (old.status && t.issue_unix < old.issue_unix) return -1;
  s->taf[i] = t;
  s->gen++;
  return i;
}

void metarStoreTafCycle(MetarStore* s, const MetarCfg& c, MetarOutcome out, const TafObs* got, int n, uint32_t now,
                        uint32_t grid, const char* const* asked, int nasked) {
  s->last_taf = out;
  s->last_taf_any = true;
  if (out != MetarOutcome::Ok && out != MetarOutcome::NoData) {
    s->gen++;
    return;
  }
  if (out == MetarOutcome::Ok)
    for (int i = 0; i < n; i++) metarStorePutTaf(s, c, got[i]);
  for (int i = 0; i < c.n && i < METAR_AIRPORTS_MAX; i++) {
    bool wanted = false;
    for (int k = 0; k < nasked && !wanted; k++) wanted = asked[k] && !strcmp(asked[k], c.a[i].icao);
    if (!wanted) continue;  // added after the job's start: the next cycle asks for it
    bool in = false;
    for (int k = 0; k < n && !in && out == MetarOutcome::Ok; k++) in = !strcmp(got[k].icao, c.a[i].icao);
    if (in) continue;
    TafObs& t = s->taf[i];
    memset(&t, 0, sizeof t);
    memcpy(t.icao, c.a[i].icao, sizeof t.icao - 1);
    t.status = (uint8_t)TafStatus::Absent;
    t.grid_unix = grid;
  }
  s->last_taf_t = now;
  s->gen++;
}

void metarStoreRemap(MetarStore* s, const MetarCfg& before, const MetarCfg& after) {
  MetarObs kept[METAR_AIRPORTS_MAX];
  TafObs keptT[METAR_AIRPORTS_MAX];
  memset(kept, 0, sizeof kept);
  memset(keptT, 0, sizeof keptT);
  for (int i = 0; i < after.n && i < METAR_AIRPORTS_MAX; i++) {
    const int j = metarCfgFind(before, after.a[i].icao);
    if (j >= 0) kept[i] = s->obs[j], keptT[i] = s->taf[j];
  }
  memcpy(s->obs, kept, sizeof kept);
  memcpy(s->taf, keptT, sizeof keptT);
  s->gen++;
}

static bool locSet(float lat, float lon) {
  return lat == lat && lon == lon && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;  // NaN = none
}
static uint32_t kmFrom(const Airport& a, float lat, float lon) {
  return geoKm(a.lat, a.lon, (int32_t)lroundf(lat * 100), (int32_t)lroundf(lon * 100));
}

OutNear nearFromStore(const MetarCfg& c, const MetarStore& s, float lat, float lon, uint32_t now) {
  OutNear n = {};
  if (!c.on || c.n < 1 || !locSet(lat, lon)) return n;
  const MetarObs& o = s.obs[0];
  if (!(o.flags & MO_VALID) || !o.obs_unix || o.obs_unix > now || now - o.obs_unix > METAR_CURRENT_S) return n;
  if (strncmp(o.icao, c.a[0].icao, 4) != 0) return n;  // a reading of the chosen main airport only
  const uint32_t km = kmFrom(c.a[0], lat, lon);
  if (km > METAR_NEAR_KM) return n;
  uint8_t w = 0;
  if (o.wx & MW_SNOW) w |= NW_SNOW;
  else if (o.wx & MW_RAIN) w |= NW_RAIN;
  else if (o.wx & MW_DRIZZLE) w |= NW_DRIZZLE;
  if (o.wx & MW_THUNDER) w |= NW_THUNDER;
  if (o.wx & MW_FOG) w |= NW_FOG;
  else if (o.wx & MW_MIST) w |= NW_MIST;
  n.ok = true;
  n.wx = w;
  n.autom = (o.flags & MO_AUTO) != 0;
  memcpy(n.icao, c.a[0].icao, sizeof n.icao);
  n.icao[4] = 0;
  snprintf(n.name, sizeof n.name, "%s", c.a[0].name);
  n.km = (uint16_t)km;
  n.obs = o.obs_unix;
  return n;
}

bool metarEligible(const MetarCfg& c, float lat, float lon) {
  if (!c.on || !locSet(lat, lon)) return false;
  for (int i = 0; i < c.n && i < METAR_AIRPORTS_MAX; i++)
    if (kmFrom(c.a[i], lat, lon) <= METAR_NEAR_KM) return true;
  return false;
}

static MetarRef refMasked(const MetarCfg& c, const MetarStore& s, uint32_t now, uint8_t allow);

bool relReference(const MetarCfg& c, const MetarStore& s, float lat, float lon, uint32_t now, uint32_t max_age,
                  int16_t* qnh, uint32_t* ref_t) {
  if (!c.on || !locSet(lat, lon)) return false;
  // the reference is built from the airports within 60 km whose report is 0 .. max_age old only
  // (one far airport never refuses the sample while metarEligible says yes; the station is sampled at the time of
  // the reports that make the QNH)
  uint8_t allow = 0;
  for (int i = 0; i < c.n && i < METAR_AIRPORTS_MAX; i++) {
    const MetarObs& o = s.obs[i];
    if (kmFrom(c.a[i], lat, lon) <= METAR_NEAR_KM && o.obs_unix && o.obs_unix <= now && now - o.obs_unix <= max_age)
      allow |= (uint8_t)(1u << i);
  }
  const MetarRef r = refMasked(c, s, now, allow);
  if (r.qnh_dhpa == METAR_I16_UNSET || r.inconsistent || !r.qnh_mask) return false;
  uint32_t newest = 0;
  for (int i = 0; i < c.n && i < METAR_AIRPORTS_MAX; i++)
    if ((r.qnh_mask & (1u << i)) && s.obs[i].obs_unix > newest) newest = s.obs[i].obs_unix;
  if (!newest) return false;
  *qnh = r.qnh_dhpa;
  *ref_t = newest;
  return true;
}

bool metarCurrent(const MetarObs& o, uint32_t now) {
  return (o.flags & MO_VALID) && o.obs_unix && now >= o.obs_unix && now - o.obs_unix <= METAR_STALE_S;
}

MetarRef metarReference(const MetarCfg& c, const MetarStore& s, uint32_t now) { return refMasked(c, s, now, 0xFF); }

// the reference over the airports in `allow` only (metarReference: all of them)
static MetarRef refMasked(const MetarCfg& c, const MetarStore& s, uint32_t now, uint8_t allow) {
  // the user's order is the priority. `local` = the first current airport in that
  // order; QNH = the current reports weighted 3 / 2 / 1 by position (one rule, also without a location); over a
  // 1.5-hPa spread the first current QNH alone with `inconsistent`
  MetarRef r{-1, METAR_I16_UNSET, 0, false};
  const int n = c.n < METAR_AIRPORTS_MAX ? c.n : METAR_AIRPORTS_MAX;
  int qn = 0, firstQ = -1;
  int16_t qmin = INT16_MAX, qmax = INT16_MIN;
  for (int i = 0; i < n; i++) {
    const MetarObs& o = s.obs[i];
    if (!(allow & (1u << i)) || !metarCurrent(o, now)) continue;
    if (r.local < 0) r.local = (int8_t)i;
    if (o.qnh_dhpa == METAR_I16_UNSET) continue;
    if (firstQ < 0) firstQ = i;
    qn++;
    if (o.qnh_dhpa < qmin) qmin = o.qnh_dhpa;
    if (o.qnh_dhpa > qmax) qmax = o.qnh_dhpa;
    r.qnh_mask |= (uint8_t)(1u << i);
  }
  if (!qn) {
    r.qnh_mask = 0;
    return r;
  }
  if (qn > 1 && qmax - qmin > METAR_SPREAD_DHPA) {
    r.inconsistent = true;
    r.qnh_dhpa = s.obs[firstQ].qnh_dhpa;
    r.qnh_mask = (uint8_t)(1u << firstQ);
    return r;
  }
  int32_t sw = 0, sq = 0;
  for (int i = 0; i < n; i++) {
    if (!(r.qnh_mask & (1u << i))) continue;
    const int32_t w = METAR_AIRPORTS_MAX - i;  // 3, 2, 1
    sw += w;
    sq += w * s.obs[i].qnh_dhpa;
  }
  r.qnh_dhpa = (int16_t)((sq + (sq >= 0 ? sw / 2 : -sw / 2)) / sw);
  return r;
}

// ---------------------------------------------------------------- JSON
static void dec1(Out& o, int v) {  // tenths -> "12.3" / "-0.5"
  if (v < 0) o.raw("-"), v = -v;
  o.printf("%d.%d", v / 10, v % 10);
}
static void key(Out& o, const char* k) { o.printf(",\"%s\":", k); }

void metarCountersJson(Out& o, const MetarCounters& n) {
  o.printf(",\"n\":{\"ok\":%lu,\"nodata\":%lu,\"fail\":%lu,\"yield\":%lu,\"skip\":%lu,", (unsigned long)n.ok,
           (unsigned long)n.nodata, (unsigned long)n.fail, (unsigned long)n.yield, (unsigned long)n.skip);
  o.printf("\"heap\":%lu,\"limited\":%lu,\"untrusted\":%lu,\"paused\":%lu,\"missed\":%lu}", (unsigned long)n.heap,
           (unsigned long)n.limited, (unsigned long)n.untrusted, (unsigned long)n.paused, (unsigned long)n.missed);
}
static void i16Dec(Out& o, const char* k, int16_t v) {
  key(o, k);
  if (v == METAR_I16_UNSET) o.raw("null");
  else dec1(o, v);
}
static void u16(Out& o, const char* k, uint16_t v) {
  key(o, k);
  if (v == METAR_U16_UNSET) o.raw("null");
  else o.printf("%u", (unsigned)v);
}
static void iso(Out& o, uint32_t t) {
  char b[24];
  if (t && formatIsoUtc(b, sizeof b, t) > 0) o.jsonStr(b);
  else o.raw("null");
}
static const char* coverName(uint8_t c) {
  static const char* const N[] = {"none", "few", "scattered", "broken", "overcast"};
  return c < 5 ? N[c] : nullptr;
}
static void cloudsVisWx(Out& o, const MetarObs& x) {
  u16(o, "vis_m", x.vis_m);
  o.printf(",\"vis_ge\":%s,\"cavok\":%s", (x.flags & MO_VIS_GE) ? "true" : "false", (x.flags & MO_CAVOK) ? "true" : "false");
  key(o, "cover");
  if (const char* cn = coverName(x.cover)) o.jsonStr(cn);
  else o.raw("null");
  u16(o, "ceil_ft", x.ceil_ft);
  o.printf(",\"cb\":%s,\"low_conf\":%s", (x.flags & MO_CB) ? "true" : "false", (x.flags & MO_LOW_CONF) ? "true" : "false");
  // fog / mist and drizzle / rain apart, so the decided words are reachable
  const auto tf = [](bool b) { return b ? "true" : "false"; };
  o.printf(",\"wx\":{\"precip\":%s,\"rain\":%s,\"drizzle\":%s,\"snow\":%s,\"freezing\":%s,\"thunder\":%s,", tf(x.wx & MW_PRECIP),
           tf(x.wx & MW_RAIN), tf(x.wx & MW_DRIZZLE), tf(x.wx & MW_SNOW), tf(x.wx & MW_FREEZING), tf(x.wx & MW_THUNDER));
  o.printf("\"fog\":%s,\"mist\":%s,\"int\":%d}", tf(x.wx & MW_FOG), tf(x.wx & MW_MIST), (int)x.wx_int);
  i16Dec(o, "t_c", x.t_dc);
  i16Dec(o, "td_c", x.td_dc);
}
// an airport's forecast: null before the first ok / nodata TAF cycle that asked for it; else its
// status (absent = not in that answer), issue, validity, horizon `to` = min(fetch hour + 13 h, valid_to) and the
// windows of the next 12 h (empty = nothing significant; `current` false = expired / NIL / CNL / absent: "no forecast")
static void tafJson(Out& o, const TafObs& t, uint32_t now) {
  key(o, "taf");
  if (!t.status) {
    o.raw("null");
    return;
  }
  o.printf("{\"status\":\"%s\",\"current\":%s,\"issued\":", tafStatusName((TafStatus)t.status), tafCurrent(t, now) ? "true" : "false");
  iso(o, t.issue_unix);
  key(o, "valid_to");
  iso(o, t.valid_to);
  key(o, "to");
  iso(o, t.status == (uint8_t)TafStatus::Ok ? tafHorizon(t) : 0);
  o.raw(",\"next12\":[");
  TafWindow w[8];
  const int n = tafWindows(t, now, w, 8);
  for (int i = 0; i < n; i++) {
    o.printf("%s{\"what\":\"%s\",\"possible\":%s,\"from\":", i ? "," : "", tafWhatName(w[i].what), w[i].possible ? "true" : "false");
    iso(o, w[i].from);
    key(o, "to");
    iso(o, w[i].to);
    if (w[i].cut) o.raw(",\"cut\":true");          // only when true: the window runs to where the data stops
    o.raw(w[i].started ? ",\"started\":true}" : "}");  // only when true: it began before now, `from` is now
  }
  o.raw("]}");
}

static void source(Out& o, const Airport& a, const MetarObs& x, uint32_t now) {
  o.raw("{\"icao\":");
  o.jsonStr(a.icao);
  key(o, "km");
  if (a.km == AIRPORT_KM_UNKNOWN) o.raw("null");
  else o.printf("%u", (unsigned)a.km);
  o.printf(",\"age_s\":%lu}", (unsigned long)(now - x.obs_unix));
}

void metarJson(Out& o, const MetarCfg& c, const MetarStore& s, bool on, uint32_t now, const MetarStationView* sv) {
  o.printf("{\"on\":%s,\"airports\":[", on ? "true" : "false");
  const int n = c.n < METAR_AIRPORTS_MAX ? c.n : METAR_AIRPORTS_MAX;
  for (int i = 0; i < n; i++) {
    const Airport& a = c.a[i];
    const MetarObs& x = s.obs[i];
    const bool have = x.flags & MO_VALID;
    o.raw(i ? ",{\"icao\":" : "{\"icao\":");
    o.jsonStr(a.icao);
    key(o, "name");
    o.jsonStr(a.name[0] ? a.name : a.icao);  // a cleared name before a lookup finds it reads as the code
    uint8_t fp;
    char cc[3];  // the airport sheet's round flag (the country, its flags-<fp>.json)
    if (airportCountry(a.icao, cc, &fp)) o.printf(",\"cc\":\"%s\",\"fp\":%u", cc, (unsigned)fp);
    key(o, "km");
    if (a.km == AIRPORT_KM_UNKNOWN) o.raw("null");
    else o.printf("%u", (unsigned)a.km);
    key(o, "obs");
    iso(o, have ? x.obs_unix : 0);
    if (!have) {
      o.raw(",\"age_s\":null,\"stale\":true,\"cur\":false");
      tafJson(o, s.taf[i], now);
      o.raw("}");
      continue;
    }
    const uint32_t age = now >= x.obs_unix ? now - x.obs_unix : 0;
    o.printf(",\"age_s\":%lu,\"stale\":%s,\"cur\":%s", (unsigned long)age, metarCurrent(x, now) ? "false" : "true",
             metarCurrent(x, now) && age <= METAR_CURRENT_S ? "true" : "false");
    o.printf(",\"cor\":%s,\"auto\":%s", (x.flags & MO_COR) ? "true" : "false", (x.flags & MO_AUTO) ? "true" : "false");
    // night at the airport at the report's time (its own position: the sheet's sky glyph); absent without a position
    // (0, 0 = never stored: no METAR site lies there)
    const SunState sun = a.lat || a.lon ? sunAt(x.obs_unix, a.lat / 100.f, a.lon / 100.f) : SunState::Unknown;
    if (sun != SunState::Unknown) o.printf(",\"night\":%s", sun == SunState::Down ? "true" : "false");
    i16Dec(o, "qnh_hpa", x.qnh_dhpa);
    key(o, "wind_dir");
    if (x.wdir == METAR_WDIR_VRB) o.raw("\"vrb\"");
    else if (x.wdir == METAR_WDIR_NONE) o.raw("null");
    else o.printf("%u", (unsigned)x.wdir);
    key(o, "wind_kmh");
    if (x.wind_dkmh == METAR_U16_UNSET) o.raw("null");
    else dec1(o, x.wind_dkmh);
    key(o, "gust_kmh");
    if (x.gust_dkmh == METAR_U16_UNSET) o.raw("null");
    else dec1(o, x.gust_dkmh);
    cloudsVisWx(o, x);
    tafJson(o, s.taf[i], now);
    o.raw("}");
  }
  o.raw("],\"reference\":");
  const MetarRef r = metarReference(c, s, now);
  if (r.local < 0) {
    o.raw("null");
  } else {
    o.raw("{\"qnh_hpa\":");
    if (r.qnh_dhpa == METAR_I16_UNSET) o.raw("null");
    else dec1(o, r.qnh_dhpa);
    o.raw(",\"qnh_from\":[");
    bool first = true;
    for (int i = 0; i < n; i++) {
      if (!(r.qnh_mask & (1u << i))) continue;
      if (!first) o.raw(",");
      first = false;
      source(o, c.a[i], s.obs[i], now);
    }
    o.printf("],\"qnh_inconsistent\":%s,\"local\":", r.inconsistent ? "true" : "false");
    const MetarObs& x = s.obs[r.local];
    source(o, c.a[r.local], x, now);
    o.raw(",\"local_values\":{\"from\":");
    o.jsonStr(c.a[r.local].icao);
    cloudsVisWx(o, x);
    // the sheet's live comparison -- the station as shown vs the QNH, the number to type while current;
    // the near QNH -- the REL basis (airports within 60 km, a report <= 90 min, consistent)
    // -- once, for the station comparison and the dashboard's "Nearby airports now"; null without a location
    int16_t q = 0;
    uint32_t rt = 0;
    const bool near = sv && relReference(c, s, sv->lat, sv->lon, now, METAR_STALE_S, &q, &rt);
    o.raw("},\"near_qnh_hpa\":");
    if (near) dec1(o, q);
    else o.raw("null");
    o.raw(",\"station\":");
    if (near && sv->ok) {
      o.printf("{\"p_hpa\":%.1f,\"qnh_hpa\":%.1f,\"diff_hpa\":%.1f,\"target_hpa\":", (double)sv->p_hpa, q / 10.0,
               (double)sv->p_hpa - q / 10.0);
      if (now - rt <= METAR_CURRENT_S) o.printf("%.1f", q / 10.0 - (double)sv->offset_hpa);
      else o.raw("null");
      o.printf(",\"active\":%s}", sv->active ? "true" : "false");
    } else {
      o.raw("null");
    }
    o.raw("}");
  }
  o.raw(",\"last\":{\"t\":");
  iso(o, s.last_t);
  key(o, "result");
  if (s.last_t) o.jsonStr(metarOutcomeName(s.last));
  else o.raw("null");
  o.raw("},\"last_taf\":{\"t\":");
  iso(o, s.last_taf_t);
  key(o, "result");
  if (s.last_taf_any) o.jsonStr(metarOutcomeName(s.last_taf));
  else o.raw("null");
  o.raw("}}");
}

// ---------------------------------------------------------------- admin POST
static bool bad(const char** err, const char* why) {
  *err = why;
  return false;
}

bool metarCfgFromJson(const char* body, size_t len, const MetarCfg& cur, const MetarLookup& look, MetarCfg* out,
                      const char** err) {
  *out = cur;
  *err = nullptr;
  JsonReader r(body, len);
  if (!r.beginObject()) return bad(err, "not a JSON object");
  char k[16];
  MetarCfg next = cur;
  bool listed = false;
  uint8_t user = 0;  // the user-name bits, following the airports into their new places
  struct { char icao[5]; char text[64]; } nm[METAR_AIRPORTS_MAX];
  int nn = 0;
  while (r.nextKey(k, sizeof k)) {
    if (!strcmp(k, "on")) {
      bool v;
      if (!r.readBool(&v)) return bad(err, "on: true or false");
      next.on = v ? 1 : 0;
    } else if (!strcmp(k, "airports")) {
      if (!r.beginArray()) return bad(err, "airports: a list of codes");
      next.n = 0;
      while (r.nextElement()) {
        char id[8];
        if (r.peek() != '"' || !r.readString(id, sizeof id) || !icaoOk(id)) return bad(err, "airports: 4-character codes");
        if (next.n >= METAR_AIRPORTS_MAX) return bad(err, "at most 3 airports");
        for (int i = 0; i < next.n; i++)
          if (!strcmp(next.a[i].icao, id)) return bad(err, "an airport twice");
        const int c = metarCfgFind(cur, id);
        const Airport* src = c >= 0 ? &cur.a[c] : nullptr;
        for (int i = 0; !src && i < look.n && i < AIRPORT_NEAR_MAX; i++)
          if (!strcmp(look.a[i].icao, id)) src = &look.a[i];
        if (!src) return bad(err, "look the airport up first");
        if (c >= 0 && metarNameUser(cur, c)) user |= (uint8_t)(1u << next.n);
        next.a[next.n++] = *src;
      }
      if (r.failed()) return bad(err, "airports: a list of codes");
      listed = true;
    } else if (!strcmp(k, "names")) {
      if (!r.beginObject()) return bad(err, "names: an object of code: name");
      char id[8];
      while (r.nextKey(id, sizeof id)) {
        if (nn >= METAR_AIRPORTS_MAX) return bad(err, "names: at most 3");
        bool trunc = false;
        if (!icaoOk(id) || r.peek() != '"' || !r.readString(nm[nn].text, sizeof nm[nn].text, &trunc))
          return bad(err, "names: an object of code: name");
        memcpy(nm[nn].icao, id, 5);
        nn++;
      }
      if (r.failed()) return bad(err, "names: an object of code: name");
    } else if (!r.skipValue()) {
      return bad(err, "not a JSON object");
    }
  }
  if (r.failed()) return bad(err, "not a JSON object");
  if (listed) next.reserved = user;
  for (int k2 = 0; k2 < nn; k2++) {  // the names, after the list (any key order)
    const int i = metarCfgFind(next, nm[k2].icao);
    if (i < 0) return bad(err, "names: not one of your airports");
    char* t = nm[k2].text;
    for (const unsigned char* p = (const unsigned char*)t; *p; p++)
      if (*p < 0x20 || *p == 0x7f) return bad(err, "names: no control characters");
    while (*t == ' ') t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') t[--n] = 0;
    if (n) {
      utf8Truncate(t, sizeof next.a[i].name - 1);
      while ((n = strlen(t)) && t[n - 1] == ' ') t[n - 1] = 0;  // the cut may end on a space
      memset(next.a[i].name, 0, sizeof next.a[i].name);
      memcpy(next.a[i].name, t, strlen(t));
      next.reserved |= (uint8_t)(1u << i);
    } else {  // back to AWC's name: the last lookup's; else the last known name stays, no longer the
      // user's, so the next lookup that has the airport replaces it -- never a bare code in between
      next.reserved &= (uint8_t)~(1u << i);
      for (int j = 0; j < look.n && j < AIRPORT_NEAR_MAX; j++)
        if (!strcmp(look.a[j].icao, next.a[i].icao)) memcpy(next.a[i].name, look.a[j].name, sizeof next.a[i].name);
    }
  }
  next.reserved &= (uint8_t)((1u << next.n) - 1);
  for (int i = next.n; i < METAR_AIRPORTS_MAX; i++) memset(&next.a[i], 0, sizeof next.a[i]);
  next.ver = METAR_CFG_V1;
  *out = next;
  return true;
}

bool metarLookupNames(MetarCfg* c, const MetarLookup& look) {
  bool changed = false;
  for (int i = 0; i < c->n && i < METAR_AIRPORTS_MAX; i++) {
    if (metarNameUser(*c, i)) continue;
    for (int j = 0; j < look.n && j < AIRPORT_NEAR_MAX; j++)
      if (!strcmp(look.a[j].icao, c->a[i].icao) && strcmp(look.a[j].name, c->a[i].name) != 0) {
        memcpy(c->a[i].name, look.a[j].name, sizeof c->a[i].name);
        changed = true;
      }
  }
  return changed;
}

}  // namespace fc
