// forecast_replay.cpp -- the local outlook's evaluation hook: reads a saved /api/hourly, /api/live
// (?station=, or the whole /api/live: its first station) and /api/bridge of one station and prints the outlook codes the
// same src/forecast.cpp gives for them at the same moment (the live block's `t`) -- gate, state, next, warn, qual, conf,
// tend -- next to the board's own block; MATCH exits 0, DIFF 1. run.ps1 / run.sh build it and check the
// fixtures in test/host/fixtures/replay-*.json. By hand:
//   cl /nologo /std:c++17 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /I firmware\fauxcloud\src
//      firmware\fauxcloud\src\*.cpp test\host\forecast_replay.cpp /Fe:forecast_replay.exe
//   forecast_replay --what-if hourly.json bridge.json learn.json [--all]   (per hour, the fixed rules against
//     the learners in use (--all: every learner's estimate) from the board's /api/learn; the changed hours and a summary)
//   forecast_replay hourly.json live.json bridge.json [learn.json]   (the board's /api/learn: its `use` block feeds
//     the outlook, so the learners' codes and conf are compared too)
//   forecast_replay --learn hourly_raw.json daily_raw.json bridge.json   (the learners from priors; prints the
//     /api/learn body, comparable field by field with the board's at the same last_hour; L1b, L3 and the gate counts of
//     live-only paths excluded). hourly_raw = /api/hourly?raw=1&from=<oldest>, daily_raw = /api/daily?all=1&raw=1.
//     `--nights out.csv` after the three files writes one row per closed night (L6's admission, L5's evening, the
//     prior's and the learned prediction of the minimum against the night's own minimum: the offline frost scoring).
//     `--migrate-at <unix hour>` emulates a board with a V1 learner blob until that hour (the learners new in V2
//     masked out, the core packed as V1), then migrated: the replay pass of l5/l6 over the hours up to it, the merge,
//     the rest with every learner. Its output is the same shape; compare it with a plain --learn run.
//     With --nights too, rows are written only for the nights after the migration hour.
//   forecast_replay hourly.json live.json bridge.json --metar metar.json   (a saved /api/metar of the same moment;
//     its main airport (airports[0]) goes through nearFromStore at the live block's `t`, so the T3 codes compare too;
//     the airport is placed `km` north of the bridge's location)
// The API rows are calibrated already (= what outlookInput builds from the ring); the counter's 10-min rise is not in
// the API, so a station without a rate sensor replays "no rain now" from the rate alone.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "forecast.h"
#include "learn.h"
#include "ha_discovery.h"  // formatIsoUtc
#include "sun.h"
#include "web_render.h"
#include "json.h"
#include "metar_store.h"
#include "records.h"
#include "tz.h"
#include "wu_parse.h"

using namespace fc;

static std::string slurp(const char* path) {
  std::string s;
  FILE* f = fopen(path, "rb");
  if (!f) return s;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  fclose(f);
  return s;
}

static uint32_t isoUnix(const char* s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5) return 0;
  return (uint32_t)unixFromUtc(y, mo, d, h, mi, se);
}

// [min, avg, max] -> avg (or a plain number); NaN for null
static double mid(JsonReader& r) {
  double v = NAN;
  if (r.peek() == '[') {
    r.beginArray();
    for (int i = 0; r.nextElement(); i++) {
      double x;
      if (r.peek() == 'n') r.skipValue();
      else if (r.readNumber(&x) && i == 1) v = x;
    }
  } else if (r.peek() == 'n') {
    r.skipValue();
  } else {
    r.readNumber(&v);
  }
  return v;
}

// [min, avg, max] -> avg and max (a plain number: both)
static void avgMax(JsonReader& r, float* avg, float* mx) {
  *avg = *mx = NAN;
  if (r.peek() != '[') {
    const double v = mid(r);
    *avg = *mx = (float)v;
    return;
  }
  r.beginArray();
  for (int i = 0; r.nextElement(); i++) {
    double x;
    if (r.peek() == 'n') r.skipValue();
    else if (r.readNumber(&x)) { if (i == 1) *avg = (float)x; else if (i == 2) *mx = (float)x; }
  }
}

// [min, avg, max] (or a number) into three values x 10^d, rounded; the sentinel for null
static void tri(JsonReader& r, double sc, int32_t out[3]) {
  out[0] = out[1] = out[2] = INT32_MIN;
  if (r.peek() == '[') {
    r.beginArray();
    for (int i = 0; r.nextElement(); i++) {
      double x;
      if (r.peek() == 'n') r.skipValue();
      else if (r.readNumber(&x) && i < 3) out[i] = (int32_t)std::lround(x * sc);
    }
  } else if (r.peek() == 'n') {
    r.skipValue();
  } else {
    double x;
    if (r.readNumber(&x)) out[0] = out[1] = out[2] = (int32_t)std::lround(x * sc);
  }
}
static int16_t i16(int32_t v) { return v == INT32_MIN ? I16_NONE : (int16_t)v; }
static uint16_t u16(int32_t v) { return v == INT32_MIN ? U16_NONE : (uint16_t)v; }
static uint8_t u8(int32_t v) { return v == INT32_MIN ? U8_NONE : (uint8_t)v; }

// one hourly or daily row of the API (raw=1) back into a record
static void row(JsonReader& r, bool day, HourRec* h, DayRec* d) {
  char f[32], v[40];
  int32_t t[3];
  r.beginObject();
  while (r.nextKey(f, sizeof f)) {
    double x;
    if (!strcmp(f, "h") && r.readString(v, sizeof v)) h->hour = isoUnix(v);
    else if (!strcmp(f, "d") && r.readString(v, sizeof v)) { int y, m, dd; if (sscanf(v, "%d-%d-%d", &y, &m, &dd) == 3) d->ymd = (uint32_t)(y * 10000 + m * 100 + dd); }
    else if (!strcmp(f, "q")) {
      r.beginArray();
      uint16_t q = 0;
      while (r.nextElement()) {
        if (!r.readString(v, sizeof v)) break;
        for (int b = 0; b < Q_COUNT; b++)
          if (!strcmp(qualityName(b), v)) q |= (uint16_t)(1u << b);
      }
      (day ? d->quality : h->quality) = q;
    } else if (!strcmp(f, "n") && r.readNumber(&x)) { if (day) d->n = (uint16_t)x; else h->n = (uint8_t)(x > 255 ? 255 : x); }
    else if (!strcmp(f, "hours") && r.readNumber(&x)) d->hours = (uint8_t)x;
    else if (!strcmp(f, "epoch") && r.readNumber(&x)) (day ? d->epoch : h->epoch) = (uint8_t)x;
    else if (!strcmp(f, "temp_c")) { tri(r, 10, t); if (day) d->t_min = i16(t[0]), d->t_avg = i16(t[1]), d->t_max = i16(t[2]); else h->t_min = i16(t[0]), h->t_avg = i16(t[1]), h->t_max = i16(t[2]); }
    else if (!strcmp(f, "dewpt_c")) { tri(r, 10, t); if (day) d->td_avg = i16(t[1]); else h->td_avg = i16(t[1]); }
    else if (!strcmp(f, "humidity")) { tri(r, 1, t); if (!day) h->rh_min = u8(t[0]), h->rh_avg = u8(t[1]), h->rh_max = u8(t[2]); }
    else if (!strcmp(f, "pressure_hpa")) { tri(r, 10, t); if (day) d->p_avg = i16(t[1]); else h->p_avg = i16(t[1]); }
    else if (!strcmp(f, "wind_kmh")) { tri(r, 10, t); if (!day) h->wind_avg = u16(t[1]); }
    else if (!strcmp(f, "gust_kmh")) { tri(r, 10, t); if (!day) h->gust_max = u16(t[0]); }
    else if (!strcmp(f, "rain_mm")) { tri(r, 100, t); if (!day) h->rain_sum = u16(t[0]); }
    else if (!strcmp(f, "solar_wm2")) { tri(r, 1, t); if (!day) h->solar_avg = u16(t[1]), h->solar_max = u16(t[2]); }
    else if (!strcmp(f, "calm_n") && r.readNumber(&x)) h->calm_n = (uint8_t)x;
    else if (!strcmp(f, "wind_dir") && r.peek() == 'n') { r.skipValue(); h->wdir_steady = packDir(DIR_NONE, 0); }
    else r.skipValue();
  }
}
static bool toStdout(void*, const char* data, size_t len) { return fwrite(data, 1, len, stdout) == len; }

// a version 1 core (the V1 fields only, sealed): the input of the migration emulation
static LearnCoreV1 packV1(const LearnCore& c) {
  LearnCoreV1 v;
  memset(&v, 0, sizeof v);
#define RP_PACK(f) memcpy(&v.f, &c.f, sizeof v.f);
  LEARN_V1_SHARED(RP_PACK)
#undef RP_PACK
  v.magic[0] = 'L', v.magic[1] = 'R', v.version = 1;
  v.crc = 0;
  v.crc = crc32(&v, sizeof v);
  return v;
}
static uint32_t ymdUtc(uint32_t unix) { return dayNumberToYmd((int32_t)(unix / 86400)); }

static int learnMain(int argc, char** argv) {
  const char* nightsPath = nullptr;
  uint32_t migrateAt = 0;
  bool argsOk = argc >= 4;
  for (int a = 4; argsOk && a < argc; a += 2) {
    if (a + 1 >= argc) argsOk = false;
    else if (!strcmp(argv[a], "--nights")) nightsPath = argv[a + 1];
    else if (!strcmp(argv[a], "--migrate-at")) migrateAt = (uint32_t)strtoul(argv[a + 1], nullptr, 10);
    else argsOk = false;
  }
  if (!argsOk) {
    fprintf(stderr, "usage: forecast_replay --learn hourly_raw.json daily_raw.json bridge.json [--nights out.csv] "
                    "[--migrate-at <unix hour>]\n");
    return 2;
  }
  FILE* nf = nightsPath ? fopen(nightsPath, "w") : nullptr;
  if (nightsPath && !nf) {
    fprintf(stderr, "cannot write %s\n", nightsPath);
    return 2;
  }
  if (nf) fprintf(nf, "start,admitted,l6,label,evening_rate,thr,t0,td0,l_h,tmin,pred_prior,pred_learned,residual\n");
  const std::string hj = slurp(argv[1]), dj = slurp(argv[2]), bj = slurp(argv[3]);
  static HourRec hours[3200];
  static DayRec days[800];
  int nh = 0, nd = 0;
  for (int pass = 0; pass < 2; pass++) {  // {"hours":[rows]} / {"days":[rows]}; the open row is not a closed record
    const std::string& js = pass ? dj : hj;
    JsonReader r(js.data(), js.size());
    char k[32];
    r.beginObject();
    while (r.nextKey(k, sizeof k)) {
      if (strcmp(k, pass ? "days" : "hours")) { r.skipValue(); continue; }
      r.beginArray();
      while (r.nextElement()) {
        HourRec h;
        DayRec d;
        clearHour(&h, 0);
        clearDay(&d, 0);
        h.quality = 0, h.n = 0, h.epoch = 0, d.quality = 0, d.hours = 0, d.epoch = 0;
        // a row with a direction had a steady one (the JSON gives no steadiness); "wind_dir":null keeps none,
        // so calmHour() judges the hour as the board did
        h.wdir_steady = packDir(0, 100);
        row(r, pass == 1, &h, &d);
        if (!pass && nh < 3200) hours[nh++] = h;
        if (pass && nd < 800) days[nd++] = d;
      }
    }
  }
  float lat = NAN, lon = NAN;
  {
    JsonReader r(bj.data(), bj.size());
    char k[32];
    r.beginObject();
    while (r.nextKey(k, sizeof k)) {
      if (strcmp(k, "effective")) { r.skipValue(); continue; }
      r.beginObject();
      while (r.nextKey(k, sizeof k)) {
        double x;
        if ((!strcmp(k, "lat") || !strcmp(k, "lon")) && r.peek() != 'n' && r.readNumber(&x)) (k[1] == 'a' ? lat : lon) = (float)x;
        else r.skipValue();
      }
    }
  }
  for (int i = 1; i < nh; i++)
    for (int j = i; j > 0 && hours[j - 1].hour > hours[j].hour; j--) std::swap(hours[j], hours[j - 1]);
  for (int i = 1; i < nd; i++)
    for (int j = i; j > 0 && days[j - 1].ymd > days[j].ymd; j--) std::swap(days[j], days[j - 1]);
  static LearnState s;
  learnInit(&s);
  LearnCtx c;
  memset(&c, 0, sizeof c);
  c.lat = lat, c.lon = lon, c.profile = 1, c.wind_f = 1000;
  static const char* const LBL[6] = {"", "clear", "overcast", "mixed", "nocell", "none"};
  static const char* const WHY[8] = {"ok", "rain", "wind", "rise", "quality", "not_clear", "evening", "missing"};
  // a V1 board until migrateAt (its hours, then its closed days, without l5/l6), the migration and the
  // replay pass of l5/l6, then the rest with every learner
  int di = 0;
  if (migrateAt) {
    const uint16_t old = LM_ALL & ~LM_SINCE_V1;
    for (int i = 0; i < nh && hours[i].hour <= migrateAt; i++) learnHour(s, hours[i], c, old);
    for (; di < nd && days[di].ymd < ymdUtc(migrateAt); di++) learnDay(s, days[di], c, old);
    const LearnCoreV1 v1 = packV1(s.c);
    static LearnState pass;
    uint8_t from = 0;
    uint16_t rep = 0;
    learnInit(&s);
    if (!learnMigrateCore(&v1, sizeof v1, &s.c, &from, &rep)) {
      fprintf(stderr, "migration failed\n");
      return 2;
    }
    learnPassStart(&pass, s.c);
    const uint32_t end = s.c.last_hour;
    int ph = 0;
    for (int i = 0; i < nh && hours[i].hour <= end; i++) learnHour(pass, hours[i], c, rep), ph++;
    learnPassMerge(s.c, pass.c, rep, end);
    fprintf(stderr, "migrated at %lu (last_hour %lu): replay of l5,l6 over %d hour(s)\n", (unsigned long)migrateAt,
            (unsigned long)end, ph);
  }
  for (int i = 0; i < nh; i++) {
    if (!(learnHour(s, hours[i], c) & LE_NIGHT) || !nf) continue;
    const LearnNight& n = s.last_night;
    L5Src src;
    bool hit;
    const int thr = l5Threshold(s.c, &src, &hit);
    auto t1 = [&](int16_t v) {
      if (hasI16(v)) fprintf(nf, ",%.1f", v / 10.0);
      else fprintf(nf, ",");
    };
    // the label only when it is the evening's own (the day of E0)
    const uint16_t dayE0 = (uint16_t)((learnDayOf(n.start - 1800, lon) - LDAY_BASE) & 0x3FFF);
    fprintf(nf, "%lu,%d,%s,%s", (unsigned long)n.start, n.admitted ? 1 : 0, WHY[n.l6 < 8 ? n.l6 : 0],
            s.c.lbl_day == dayE0 ? LBL[s.c.lbl < 6 ? s.c.lbl : 0] : "");
    if (hasI16(s.c.e_dt3) && !(s.c.e_flags & EB_BAD)) fprintf(nf, ",%.2f", -(s.c.e_dt3 * 10) / 3 / 100.0);
    else fprintf(nf, ",");
    fprintf(nf, ",%.2f", thr / 100.0);
    t1(n.t2), t1(n.td2);
    fprintf(nf, ",%.1f", n.l_h / 10.0);
    t1(n.tmin);
    // the fixed prior (a 1.00, b 0.40, c 0.20) through the firmware's own integer path
    t1(l6Predict(n.t2, n.td2, n.l_h, 100, 4000, 2000)), t1(n.pred);
    if (hasI16(n.tmin) && hasI16(n.pred)) fprintf(nf, ",%.1f\n", (n.tmin - n.pred) / 10.0);
    else fprintf(nf, ",\n");
  }
  if (nf) fclose(nf);
  for (int i = di; i < nd; i++) learnDay(s, days[i], c);
  static LearnView v;
  learnView(s, &v);
  LearnMeta m = {"replay", nullptr, (uint32_t)nh, true, 0, 0, 0, 0, nh ? hours[nh - 1].hour : 0, lat, lon, (uint32_t)nd, 0,
                 0, 0, 0, 0};
  char buf[256];
  Out o(toStdout, nullptr, buf, sizeof buf);
  learnJson(o, v, m);
  o.flush();
  printf("\n");
  return 0;
}

// the /api/hourly rows (calibrated), oldest first; the open hour's rain into *openRain
static int readHours(const std::string& hj, OutHour* h, int cap, float* openRain) {
  int n = 0;
  *openRain = NAN;
  JsonReader r(hj.data(), hj.size());
  char k[32];
  r.beginObject();
  while (r.nextKey(k, sizeof k)) {
    const bool open = strcmp(k, "open") == 0, hours = strcmp(k, "hours") == 0;
    if (!open && !hours) { r.skipValue(); continue; }
    if (open && r.peek() == 'n') { r.skipValue(); continue; }
    if (hours) r.beginArray();
    while (open || r.nextElement()) {
      OutHour x = {};
      x.p = NAN, x.t = x.td = x.wind = x.rain = x.solar = x.rh = x.solar_max = NAN;
      double calm_n = 0, nn = 0;
      bool dirNull = false;
      r.beginObject();
      char f[32], v[40];
      while (r.nextKey(f, sizeof f)) {
        if (!strcmp(f, "h") && r.readString(v, sizeof v)) x.hour = isoUnix(v);
        else if (!strcmp(f, "q")) {
          r.beginArray();
          while (r.nextElement()) {
            if (!r.readString(v, sizeof v)) break;
            for (int b = 0; b < Q_COUNT; b++)
              if (!strcmp(qualityName(b), v)) x.quality |= (uint16_t)(1u << b);
          }
        } else if (!strcmp(f, "temp_c")) x.t = (float)mid(r);
        else if (!strcmp(f, "dewpt_c")) x.td = (float)mid(r);
        else if (!strcmp(f, "pressure_hpa")) x.p = mid(r);
        else if (!strcmp(f, "wind_kmh")) x.wind = (float)mid(r);
        else if (!strcmp(f, "rain_mm")) x.rain = (float)mid(r);
        else if (!strcmp(f, "solar_wm2")) avgMax(r, &x.solar, &x.solar_max);  // L1's k_max
        else if (!strcmp(f, "humidity")) x.rh = (float)mid(r);              // `humid`; L2's RH'
        else if (!strcmp(f, "calm_n")) r.readNumber(&calm_n);
        else if (!strcmp(f, "n")) r.readNumber(&nn);
        else if (!strcmp(f, "wind_dir") && r.peek() == 'n') { dirNull = true; r.skipValue(); }
        else r.skipValue();
      }
      x.calm = calm_n * 2 > nn || dirNull;
      if (open) { *openRain = x.rain; break; }
      if (n < cap) h[n++] = x;
    }
  }
  for (int i = 1; i < n; i++)  // oldest first, as outlookInput gives them
    for (int j = i; j > 0 && h[j - 1].hour > h[j].hour; j--) {
      const OutHour t = h[j];
      h[j] = h[j - 1], h[j - 1] = t;
    }
  return n;
}
static void readBridge(const std::string& bj, float* lat, float* lon) {
  *lat = *lon = NAN;
  JsonReader r(bj.data(), bj.size());
  char k[32];
  r.beginObject();
  while (r.nextKey(k, sizeof k)) {
    if (strcmp(k, "effective")) { r.skipValue(); continue; }
    r.beginObject();
    while (r.nextKey(k, sizeof k)) {
      double x;
      if ((!strcmp(k, "lat") || !strcmp(k, "lon")) && r.peek() != 'n' && r.readNumber(&x)) *(k[1] == 'a' ? lat : lon) = (float)x;
      else r.skipValue();
    }
  }
}

// --what-if hourly.json bridge.json learn.json [--all]. Per closed hour, the outlook with the
// fixed rules against the outlook with the learners in use (/api/learn's states: in_use only; --all: every learner's
// current estimate), the live values taken from the hour itself (no rain rate, gust or live solar: the hour's own
// means). L1's dome / geo / indoor gates are not modelled (the cell state does not carry them). Prints the hours whose codes differ and a summary per code. Tools only: nothing here is in the firmware.
namespace {
struct WiLearn {
  float e[L1_CELLS];
  bool cellUse[L1_CELLS];
  uint8_t mask[(L1B_CELLS + 3) / 4];
  bool l2, l5, l6, stuck;
  float s, thr, a, b, c, sd;
  float useRhSat;  // the snapshot's calibrated S (use.rh_sat), 0 = none: its difference to `s` = cal_rh
};
int b64v(char ch) {
  return ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
         : ch == '+' ? 62 : ch == '/' ? 63 : -1;
}
bool readLearn(const std::string& js, bool all, WiLearn* w) {
  memset(w, 0, sizeof *w);
  JsonReader r(js.data(), js.size());
  char k[24], v[16];
  static char mask[400];
  mask[0] = 0;
  bool any = false;
  const auto st = [&](const char* s) { return all ? strcmp(s, "default") != 0 : strcmp(s, "in_use") == 0; };
  r.beginObject();
  while (r.nextKey(k, sizeof k)) {
    if (!strcmp(k, "l1") && r.peek() == '{') {
      any = true;
      r.beginObject();
      while (r.nextKey(k, sizeof k)) {
        if (strcmp(k, "cells")) { r.skipValue(); continue; }
        r.beginArray();
        for (int i = 0; r.nextElement(); i++) {
          r.beginObject();
          while (r.nextKey(k, sizeof k)) {
            double x;
            if (!strcmp(k, "e") && r.readNumber(&x)) { if (i < L1_CELLS) w->e[i] = (float)x; }
            else if (!strcmp(k, "state") && r.readString(v, sizeof v)) { if (i < L1_CELLS) w->cellUse[i] = st(v); }
            else r.skipValue();
          }
        }
      }
    } else if (!strcmp(k, "l1b") && r.peek() == '{') {
      r.beginObject();
      while (r.nextKey(k, sizeof k))
        if (!strcmp(k, "mask")) r.readString(mask, sizeof mask);
        else r.skipValue();
    } else if ((!strcmp(k, "l2") || !strcmp(k, "l5") || !strcmp(k, "l6")) && r.peek() == '{') {
      const char which = k[1];
      r.beginObject();
      while (r.nextKey(k, sizeof k)) {
        double x;
        if (!strcmp(k, "state") && r.readString(v, sizeof v)) (which == '2' ? w->l2 : which == '5' ? w->l5 : w->l6) = st(v);
        else if (which == '2' && !strcmp(k, "s") && r.readNumber(&x)) w->s = (float)x;
        else if (which == '2' && !strcmp(k, "stuck")) { bool b = false; r.readBool(&b); w->stuck = b; }
        else if (which == '5' && !strcmp(k, "thr") && r.readNumber(&x)) w->thr = (float)x;
        else if (which == '6' && !strcmp(k, "a") && r.readNumber(&x)) w->a = (float)x;
        else if (which == '6' && !strcmp(k, "b") && r.readNumber(&x)) w->b = (float)x;
        else if (which == '6' && !strcmp(k, "c") && r.readNumber(&x)) w->c = (float)x;
        else if (which == '6' && !strcmp(k, "sd") && r.peek() != 'n' && r.readNumber(&x)) w->sd = (float)x;
        else r.skipValue();
      }
    } else if (!strcmp(k, "use") && r.peek() == '{') {  // the calibrated S of the snapshot
      r.beginObject();
      while (r.nextKey(k, sizeof k)) {
        double x;
        if (!strcmp(k, "rh_sat") && r.readNumber(&x)) w->useRhSat = (float)x;
        else r.skipValue();
      }
    } else r.skipValue();
  }
  // the mask: base64 of 2 bits per cell
  size_t o = 0;
  for (size_t i = 0; mask[i] && mask[i + 1] && o < sizeof w->mask; i += 4) {
    const int q[4] = {b64v(mask[i]), b64v(mask[i + 1]), mask[i + 2] ? b64v(mask[i + 2]) : -1, mask[i + 2] && mask[i + 3] ? b64v(mask[i + 3]) : -1};
    if (q[0] < 0 || q[1] < 0) break;
    const uint32_t x = (uint32_t)q[0] << 18 | (uint32_t)q[1] << 12 | (uint32_t)(q[2] < 0 ? 0 : q[2]) << 6 | (uint32_t)(q[3] < 0 ? 0 : q[3]);
    if (o < sizeof w->mask) w->mask[o++] = (uint8_t)(x >> 16);
    if (q[2] >= 0 && o < sizeof w->mask) w->mask[o++] = (uint8_t)(x >> 8);
    if (q[3] >= 0 && o < sizeof w->mask) w->mask[o++] = (uint8_t)x;
  }
  w->l6 = w->l6 && w->sd > 0;
  w->l2 = w->l2 && !w->stuck;  // as learnUseOf, never while RH is stuck at its cap (--all too)
  return any;
}
// the LearnUse of the newest closed hour h[i] (as learnUseOf on the board, from the JSON values)
LearnUse wiUse(const WiLearn& w, const OutHour* h, int n, int i, float lat, float lon) {
  LearnUse u = {};
  u.sky_cls = 255;
  u.hour = h[i].hour;
  int cell, bcell;
  double ca, cm;
  if (learnHourSky(h[i].hour, lat, lon, &cell, &bcell, &ca, &cm) && w.cellUse[cell]) {
    u.bits |= LU_L1;
    u.sky_ref = (uint16_t)std::lround(w.e[cell] * ca), u.sky_ref_max = (uint16_t)std::lround(w.e[cell] * cm);
    u.sky_cls = bcell >= 0 ? (uint8_t)(w.mask[bcell / 4] >> (2 * (bcell % 4)) & 3) : 0;
  }
  if (w.l2 && w.s > 0) {  // calibrated as learnUseOf (cal_rh from the snapshot), clamped 85 .. 100 %
    const float cal = w.useRhSat > 0 ? w.useRhSat - w.s : 0;
    const long v = std::lround((w.s + cal) * 10);
    u.bits |= LU_L2, u.rh_sat = (uint16_t)(v < 850 ? 850 : v > 1000 ? 1000 : v);
  }
  if (w.l5 && w.thr > 0) u.bits |= LU_L5, u.cool = (int16_t)std::lround(w.thr * 100);
  if (w.l6) {  // tonight: the night that started at the first hour >= sunset + 1 h; T / Td at its second hour
    uint32_t rise = 0, set = 0;
    const uint32_t t = h[i].hour + 3600u;
    if (sunTimes(t, lat, lon, &rise, &set) == SunDay::Times) {
      if (t < rise) sunTimes(t - 86400u, lat, lon, &rise, &set);
      uint32_t r2 = 0, s2 = 0;
      sunTimes(set + 86400u, lat, lon, &r2, &s2);
      const uint32_t start = (set + 3600 + 3599) / 3600 * 3600, end = (r2 + 3600 + 3599) / 3600 * 3600;
      for (int j = 0; j <= i; j++)
        if (h[j].hour == start + 3600u && t >= start + 7200u && t < end && !std::isnan(h[j].t) && !std::isnan(h[j].td)) {
          const int16_t t2 = (int16_t)std::lround(h[j].t * 10), td2 = (int16_t)std::lround(h[j].td * 10);
          const uint32_t lh = (r2 - set + 180) / 360;
          const int p = l6Predict(t2, td2, (uint8_t)(lh > 255 ? 255 : lh), (int)std::lround(w.a * 100), (int)std::lround(w.b * 1e4),
                                  (int)std::lround(w.c * 1e4));
          const int sd = (int)std::lround(w.sd * 10);
          const auto fl = [](int v) { return v >= 0 ? v / 10 * 10 : -((-v + 9) / 10 * 10); };
          u.bits |= LU_L6, u.min_lo = (int16_t)fl(p - sd), u.min_hi = (int16_t)-fl(-(p + sd)), u.night_end = end;
        }
    }
  }
  (void)n;
  return u;
}
const char* codeOf(uint8_t c, const char* const* t, int cnt) { return c == FC_NONE || c >= cnt ? "-" : t[c]; }
}  // namespace

static int whatIfMain(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: forecast_replay --what-if hourly.json bridge.json learn.json [--all]  (L1's dome / geo / "
                    "indoor gates are not modelled)\n");
    return 2;
  }
  const bool all = argc >= 5 && !strcmp(argv[4], "--all");
  static OutHour h[4096];
  float openRain = NAN, lat = NAN, lon = NAN;
  const int n = readHours(slurp(argv[1]), h, 4096, &openRain);
  readBridge(slurp(argv[2]), &lat, &lon);
  static WiLearn w;
  if (!readLearn(slurp(argv[3]), all, &w)) {
    fprintf(stderr, "%s: no /api/learn body (firmware 0.1.0-b23 or later)\n", argv[3]);
    return 2;
  }
  printf("what-if: %d hours, learners %s:%s%s%s%s\n", n, all ? "every estimate (--all)" : "in use", w.l2 ? " l2" : "",
         w.l5 ? " l5" : "", w.l6 ? " l6" : "", [&] { for (bool b : w.cellUse) if (b) return " l1"; return ""; }());
  int counts[2][OS_COUNT + ON_COUNT + OW_COUNT] = {}, changed = 0, judged = 0;
  for (int i = 0; i < n; i++) {
    OutlookIn in = {};
    in.h = h, in.n = i + 1, in.open_rain = 0, in.time_ok = true, in.has_baro = true, in.lat = lat, in.lon = lon;
    in.now = h[i].hour + 3600u + 600u;
    in.t = h[i].t, in.td = h[i].td, in.rh = h[i].rh, in.solar = h[i].solar;
    in.rate = in.rain10 = in.gust10 = in.wind10 = NAN;
    const Outlook a = outlook(in);
    const LearnUse u = wiUse(w, h, n, i, lat, lon);
    in.use = &u;
    const Outlook b = outlook(in);
    if (!a.present) continue;
    judged++;
    const Outlook* ab[2] = {&a, &b};
    for (int k = 0; k < 2; k++) {
      if (ab[k]->state < OS_COUNT) counts[k][ab[k]->state]++;
      if (ab[k]->next < ON_COUNT) counts[k][OS_COUNT + ab[k]->next]++;
      if (ab[k]->warn < OW_COUNT) counts[k][OS_COUNT + ON_COUNT + ab[k]->warn]++;
    }
    if (a.state == b.state && a.next == b.next && a.warn == b.warn) continue;
    changed++;
    char iso[24];
    formatIsoUtc(iso, sizeof iso, h[i].hour);
    printf("%s  fixed %s / %s / %s  ->  learned %s / %s / %s\n", iso, codeOf(a.state, OUT_STATE, OS_COUNT),
           codeOf(a.next, OUT_NEXT, ON_COUNT), codeOf(a.warn, OUT_WARN, OW_COUNT), codeOf(b.state, OUT_STATE, OS_COUNT),
           codeOf(b.next, OUT_NEXT, ON_COUNT), codeOf(b.warn, OUT_WARN, OW_COUNT));
  }
  printf("summary: %d of %d hours changed\n", changed, judged);
  for (int c = 0; c < OS_COUNT + ON_COUNT + OW_COUNT; c++)
    if (counts[0][c] || counts[1][c])
      printf("  %-14s fixed %5d  learned %5d\n", c < OS_COUNT ? OUT_STATE[c] : c < OS_COUNT + ON_COUNT ? OUT_NEXT[c - OS_COUNT]
                                                  : OUT_WARN[c - OS_COUNT - ON_COUNT], counts[0][c], counts[1][c]);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "--learn")) return learnMain(argc - 1, argv + 1);
  if (argc > 1 && !strcmp(argv[1], "--what-if")) return whatIfMain(argc - 1, argv + 1);
  // [--metar metar.json] and [learn.json] (the board's /api/learn), in any order after the three files
  const char* metarPath = nullptr;
  const char* learnPath = nullptr;
  bool argsOk = argc >= 4;
  for (int a = 4; argsOk && a < argc; a++) {
    if (!strcmp(argv[a], "--metar") && a + 1 < argc) metarPath = argv[++a];
    else if (!learnPath && argv[a][0] != '-') learnPath = argv[a];
    else argsOk = false;
  }
  if (!argsOk) {
    fprintf(stderr, "usage: forecast_replay hourly.json live.json bridge.json [--metar metar.json] [learn.json]\n");
    return 2;
  }
  static OutHour h[64];
  int n = 0;
  float openRain = NAN;
  const std::string hj = slurp(argv[1]), lj = slurp(argv[2]), bj = slurp(argv[3]);
  n = readHours(hj, h, 64, &openRain);
  OutlookIn in = {};
  in.h = h, in.n = n, in.open_rain = openRain;
  in.t = in.td = in.rate = in.rain10 = in.gust10 = in.solar = in.rh = in.wind10 = NAN;
  in.time_ok = true;
  // the board's codes -- gate, state, next, warn, qual, conf and tend.code
  constexpr int NK = 7;
  char board[NK][32] = {"null", "null", "null", "null", "null", "null", "null"};
  bool found = false;
  {  // /api/live?station= ({"station":{..}}) or the whole /api/live ({"stations":[{..},..]}, the first station: the
     // captures saved the whole document, and the tool looked for "station" only -- every board column read null)
    JsonReader r(lj.data(), lj.size());
    char k[32], v[40];
    r.beginObject();
    while (r.nextKey(k, sizeof k)) {
      const bool one = !strcmp(k, "station"), all = !strcmp(k, "stations");
      if ((!one && !all) || found) { r.skipValue(); continue; }
      if (all) {
        r.beginArray();
        if (!r.nextElement()) continue;
      }
      found = true;
      r.beginObject();
      while (r.nextKey(k, sizeof k)) {
        if (!strcmp(k, "sensor_stale")) { bool b = false; r.readBool(&b); in.stale = b; }
        else if (!strcmp(k, "values")) {
          r.beginObject();
          while (r.nextKey(k, sizeof k)) {
            double x = NAN;
            if (r.peek() == 'n' || !r.readNumber(&x)) { r.skipValue(); continue; }
            if (!strcmp(k, "temp_c")) in.t = (float)x;
            else if (!strcmp(k, "dewpt_c")) in.td = (float)x;
            else if (!strcmp(k, "rain_rate_mmh")) in.rate = (float)x;
            else if (!strcmp(k, "gust_10m_kmh")) in.gust10 = (float)x;
            else if (!strcmp(k, "gust_kmh") && std::isnan(in.gust10)) in.gust10 = (float)x;
            else if (!strcmp(k, "solar_wm2")) in.solar = (float)x;
            else if (!strcmp(k, "humidity")) in.rh = (float)x;  // `humid`, `windy`
            else if (!strcmp(k, "wind_avg10m_kmh")) in.wind10 = (float)x;
            else if (!strcmp(k, "pressure_hpa")) in.has_baro = true;
          }
        } else if (!strcmp(k, "outlook") && r.peek() == '{') {
          r.beginObject();
          while (r.nextKey(k, sizeof k)) {
            static const char* const OK[NK - 1] = {"gate", "state", "next", "warn", "qual", "conf"};
            int slot = -1;
            for (int i = 0; i < NK - 1; i++)
              if (!strcmp(k, OK[i])) slot = i;
            if (!strcmp(k, "t") && r.readString(v, sizeof v)) in.now = isoUnix(v);
            else if (slot >= 0 && r.peek() == '"') r.readString(board[slot], sizeof board[slot]);
            else if (!strcmp(k, "tend") && r.peek() == '{') {
              r.beginObject();
              while (r.nextKey(k, sizeof k))
                if (!strcmp(k, "code") && r.peek() == '"') r.readString(board[NK - 1], sizeof board[NK - 1]);
                else r.skipValue();
            } else r.skipValue();
          }
        } else r.skipValue();
      }
      if (all)
        while (r.nextElement()) r.skipValue();  // the other stations
    }
  }
  if (!found) {
    fprintf(stderr, "%s: no \"station\" object or \"stations\" array\n", argv[2]);
    return 2;
  }
  in.lat = in.lon = NAN;
  readBridge(bj, &in.lat, &in.lon);
  for (int i = 0; i < n; i++) in.has_baro = in.has_baro || !std::isnan(h[i].p);
  if (metarPath) {  // /api/metar's main airport -> a one-airport store, admitted by the firmware's own rule
    const std::string mj = slurp(metarPath);
    JsonReader r(mj.data(), mj.size());
    MetarCfg c;
    metarCfgDefault(&c);
    MetarStore st;
    metarStoreClear(&st);
    double km = -1;
    char k[32], v[40];
    r.beginObject();
    while (r.nextKey(k, sizeof k)) {
      if (!strcmp(k, "on")) { bool b = false; r.readBool(&b); c.on = b ? 1 : 0; continue; }
      if (strcmp(k, "airports") || !r.beginArray()) { r.skipValue(); continue; }
      for (int i = 0; r.nextElement(); i++) {
        if (i) { r.skipValue(); continue; }
        MetarObs& o = st.obs[0];
        r.beginObject();
        while (r.nextKey(k, sizeof k)) {
          if (!strcmp(k, "icao") && r.readString(v, sizeof v)) memcpy(c.a[0].icao, v, 5), memcpy(o.icao, v, 4);
          else if (!strcmp(k, "name") && r.readString(v, sizeof v)) snprintf(c.a[0].name, sizeof c.a[0].name, "%s", v);
          else if (!strcmp(k, "km") && r.peek() != 'n') r.readNumber(&km);
          else if (!strcmp(k, "obs") && r.peek() == '"' && r.readString(v, sizeof v)) o.obs_unix = isoUnix(v), o.flags |= MO_VALID;
          else if (!strcmp(k, "auto")) { bool b = false; r.readBool(&b); if (b) o.flags |= MO_AUTO; }
          else if (!strcmp(k, "wx") && r.peek() == '{') {
            static const struct { const char* key; uint8_t bit; } WX[] = {{"precip", MW_PRECIP}, {"rain", MW_RAIN},
                {"drizzle", MW_DRIZZLE}, {"snow", MW_SNOW}, {"freezing", MW_FREEZING}, {"thunder", MW_THUNDER},
                {"fog", MW_FOG}, {"mist", MW_MIST}};
            r.beginObject();
            while (r.nextKey(k, sizeof k)) {
              bool b = false;
              const char* key = k;
              if (r.peek() == 't' || r.peek() == 'f') r.readBool(&b); else r.skipValue();
              for (const auto& w : WX) if (b && !strcmp(key, w.key)) o.wx |= w.bit;
            }
          } else r.skipValue();
        }
        c.n = 1;
      }
    }
    if (c.n && km >= 0 && !std::isnan(in.lat)) {  // `km` north of the bridge (1 km ~ 0.009 degrees)
      c.a[0].lat = (int16_t)lround(in.lat * 100 + km / 1.112);
      c.a[0].lon = (int16_t)lround(in.lon * 100);
    }
    in.near = nearFromStore(c, st, in.lat, in.lon, in.now);
    printf("near   %s\n", in.near.ok ? in.near.icao : "none");
  }
  // an optional 4th file, the board's /api/learn at the same moment: its `use` block (the LearnUse
  // snapshot, calibrated) feeds the outlook, so MATCH covers the learners' codes and conf
  static LearnUse lu;
  const bool useGiven = learnPath != nullptr;
  if (useGiven) {
    const std::string uj = slurp(learnPath);
    JsonReader r(uj.data(), uj.size());
    char k[32];
    r.beginObject();
    bool got = false;
    while (r.nextKey(k, sizeof k)) {
      if (strcmp(k, "use") || r.peek() != '{') { r.skipValue(); continue; }
      got = true;
      r.beginObject();
      while (r.nextKey(k, sizeof k)) {
        double x = 0;
        bool b = false;
        static const char* const BIT[4] = {"l1", "l2", "l5", "l6"};
        int bit = -1;
        for (int i = 0; i < 4; i++)
          if (!strcmp(k, BIT[i])) bit = i;
        if (bit >= 0 && r.readBool(&b)) { if (b) lu.bits |= (uint8_t)(1u << bit); continue; }
        if (r.peek() == 'n' || !r.readNumber(&x)) { r.skipValue(); continue; }
        if (!strcmp(k, "hour")) lu.hour = (uint32_t)x;
        else if (!strcmp(k, "sky_ref")) lu.sky_ref = (uint16_t)x;
        else if (!strcmp(k, "sky_ref_max")) lu.sky_ref_max = (uint16_t)x;
        else if (!strcmp(k, "sky_cls")) lu.sky_cls = (uint8_t)x;
        else if (!strcmp(k, "rh_sat")) lu.rh_sat = (uint16_t)std::lround(x * 10);
        else if (!strcmp(k, "cool")) lu.cool = (int16_t)std::lround(x * 100);
        else if (!strcmp(k, "min_lo")) lu.min_lo = (int16_t)std::lround(x * 10);
        else if (!strcmp(k, "min_hi")) lu.min_hi = (int16_t)std::lround(x * 10);
        else if (!strcmp(k, "night_end")) lu.night_end = (uint32_t)x;
      }
    }
    if (!got) {
      fprintf(stderr, "%s: no \"use\" block (firmware 0.1.0-b23 or later)\n", learnPath);
      return 2;
    }
    in.use = &lu;
  }
  const Outlook o = outlook(in);
  const auto nm = [](uint8_t c, const char* const* t, int cnt) { return c == FC_NONE || c >= cnt ? "null" : t[c]; };
  static const char* const TEND[3] = {"rising", "steady", "falling"};  // forecast.cpp TEND_CODE (TendCode order)
  const char* mine[NK] = {nm(o.gate, OUT_GATE, OG_COUNT), nm(o.state, OUT_STATE, OS_COUNT), nm(o.next, OUT_NEXT, ON_COUNT),
                          nm(o.warn, OUT_WARN, OW_COUNT), nm(o.qual, OUT_QUAL, OQ_COUNT), o.learned ? "learned" : "default",
                          o.has_tend && o.tend.code < 3 ? TEND[o.tend.code] : "null"};
  const char* keys[NK] = {"gate", "state", "next", "warn", "qual", "conf", "tend"};
  int diff = 0;
  printf("hours %d, now %lu\n", n, (unsigned long)in.now);
  for (int i = 0; i < NK; i++) {
    // conf is replayed only with the use block (the learners' conf is in no other saved file): without it, printed,
    // never compared
    const bool shown = i == 5 && !useGiven, same = !strcmp(mine[i], board[i]);
    if (!shown) diff += !same;
    printf("%-6s replay %-18s board %-18s %s\n", keys[i], shown ? "-" : mine[i], board[i],
           shown ? "(not replayed)" : same ? "=" : "DIFF");
  }
  printf(diff ? "DIFF\n" : "MATCH\n");
  return diff ? 1 : 0;
}
