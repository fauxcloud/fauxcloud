// test_learn.inc -- the self-calibrating learners (L1-L7), included by test_fauxcloud.cpp.
// The generic point 52 N 20 E; synthetic hours built here.

// the gate counters exist in bench builds only (LearnState::gates): a gate assertion is true in the release and a gate
// read is 0 there; the st2 variant (FC_DIAG_BENCH=1) runs every one of them
#if FC_DIAG_BENCH
#define GT(cond) (cond)
#define GV(x) (x)
#else
#define GT(cond) true
#define GV(x) 0u
#endif

static uint32_t gLcg = 12345;
static double lrand() {  // 0 .. 1
  gLcg = gLcg * 1103515245u + 12345u;
  return ((gLcg >> 8) & 0xFFFFFF) / 16777216.0;
}
static HourRec lhour(uint32_t hour, float t = 8) {
  HourRec r;
  clearHour(&r, hour);
  r.quality = 0, r.n = 60, r.epoch = 0;
  r.t_avg = (int16_t)lround(t * 10), r.t_min = (int16_t)(r.t_avg - 3), r.t_max = (int16_t)(r.t_avg + 3);
  r.td_avg = (int16_t)(r.t_avg - 40);
  r.rh_avg = 80, r.rh_max = 85, r.rh_min = 75;
  r.wind_avg = 30, r.gust_max = 60, r.rain_sum = 0, r.calm_n = 0, r.wdir_steady = packDir(200, 100);
  return r;
}
static LearnCtx lctx(bool live = false) {
  LearnCtx c;
  memset(&c, 0, sizeof c);
  c.lat = 52, c.lon = 20, c.profile = 1, c.wind_f = 1000, c.live = live;
  return c;
}

// the sky at a site: f = the site's clear-sky factor; shade(): the obstacles (AM 13 deg SE, PM 10 deg SW, a noon roof band)
static bool synthShade(double el, double az) {
  if (az >= 90 && az < 170 && el < 13) return true;    // the AM horizon to the SE
  if (az >= 190 && az < 270 && el < 10) return true;   // the PM horizon to the SW
  return az >= 176 && az < 184 && el >= 16;            // a roof band at noon
}
// one hour of a clear (k = f, shade x 0.2) or a cloudy (steady k) sky: the record's solar + the live bins
static void synthSolar(uint32_t hour, double f, bool shade, HourRec* r, LearnCtx* c) {
  SunGeo g;
  sunGeo(hour + 1800, &g);
  double sum = 0, mx = 0;
  for (int i = 0; i < LEARN_BINS; i++) {
    double el, az;
    sunPosG(g, hour + 150 + 300u * (uint32_t)i, 52, 20, &el, &az);
    double v = clearSky(el) * f;
    if (shade && synthShade(el, az)) v *= .2;
    sum += v, mx = v > mx ? v : mx;
    c->bin_sum[i] = (uint16_t)lround(v * 5), c->bin_n[i] = 5;
  }
  r->solar_avg = (uint16_t)lround(sum / LEARN_BINS), r->solar_max = (uint16_t)lround(mx);
}

static void testLearnSun() {
  struct V { uint32_t t; double el, az, cs; };
  const V v[] = {{1794297600u, 14.328, 144.493, 215.83}, {1794306600u, 21.066, 181.511, 336.79},
                 {1782036000u, 60.373, 160.568, 893.87}, {1773986400u, 10.482, 104.652, 146.03}};  // tools/forecast_trial.py
  for (const V& x : v) {
    double el, az;
    sunPos(x.t, 52, 20, &el, &az);
    CHECK(std::fabs(el - x.el) < .5 && std::fabs(az - x.az) < .5 && std::fabs(clearSky(el) - x.cs) < 6);
  }
  CHECK(clearSky(-1) == 0 && clearSky(0) == 0);
  CHECK(l1bCell(2.9, 100) == -1 && l1bCell(3, 0) == 0 && l1bCell(20.99, 359.9) == 647 && l1bCell(21, 0) == 648 &&
        l1bCell(40.9, 359) == 827 && l1bCell(41, 0) == 828 && l1bCell(61, 0) == 924 && l1bCell(80.9, 359) == 947 &&
        l1bCell(85, 10) == 948);
  // b = 1 with 10 b >= v is neither sun nor shade
  CHECK(l1bClass(0x52) == 0 && l1bClass(0x62) == 1 && l1bClass(0x61) == 0 && l1bClass(0xA1) == 0 && l1bClass(0x60) == 2 &&
        l1bClass(0xF2) == 1 && l1bClass(0xF1) == 2);
}

static void testLearnBasics() {
  // confidence levels and the 60-day drops (nmin 5, high 20 in 1/64)
  CHECK(learnConfOf(0, 320, 1280, 0, 0) == LC_DEFAULT && learnConfOf(320, 320, 1280, 10, 10) == LC_LOW);
  CHECK(learnConfOf(700, 320, 1280, 10, 10) == LC_MEDIUM && learnConfOf(1300, 320, 1280, 10, 70) == LC_HIGH);
  CHECK(learnConfOf(1300, 320, 1280, 10, 71) == LC_LOW && learnConfOf(1300, 320, 1280, 10, 131) == LC_DEFAULT);
  // blobs: round trip, CRC corruption, older, newer
  static LearnState s;
  learnInit(&s);
  s.c.l1[3].e = 6400, s.c.last_hour = 1794297600u;
  LearnCore c = s.c;
  learnSealCore(&c);
  CHECK(learnCheckCore(c, sizeof c) == BlobCheck::Ok && learnCheckCore(c, sizeof c - 1) == BlobCheck::Bad);
  LearnCore bad = c;
  bad.l1[3].e ^= 1;
  CHECK(learnCheckCore(bad, sizeof bad) == BlobCheck::Bad);
  LearnCore older = c, newer = c;
  older.version = 1, older.crc = 0, older.crc = crc32(&older, sizeof older);  // a version 1 blob
  newer.version = 4, newer.crc = 0, newer.crc = crc32(&newer, sizeof newer);
  CHECK(learnCheckCore(older, sizeof older) == BlobCheck::Older && learnCheckCore(newer, sizeof newer) == BlobCheck::Newer);
  older.version = 2, older.crc = 0, older.crc = crc32(&older, sizeof older);  // a version 2 blob
  CHECK(learnCheckCore(older, sizeof older) == BlobCheck::Older);
  // a stored blob longer than ours is a newer image's (read-only), a shorter one is bad
  CHECK(learnCheckCore(c, sizeof c + 8) == BlobCheck::Newer && learnCheckCore(c, 100) == BlobCheck::Bad);
  const uint32_t here = learnLocSig(52, 20);
  s.c.loc_sig = here;
  s.mask[17] = 0x62;
  static LearnMaskBlob mb;
  learnPackMask(s, &mb);
  static LearnState t;
  learnInit(&t);
  CHECK(learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here) == BlobCheck::Ok && t.mask[17] == 0x62);
  CHECK(learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here ^ 1) == BlobCheck::Bad);  // learned at another place
  CHECK(learnUnpackMask(&t, mb, LEARN_MASK_BYTES + 4, here) == BlobCheck::Newer && learnUnpackMask(&t, mb, 500, here) == BlobCheck::Bad);
  // a core that is older (or missing) keeps a valid mask, which is not rewritten, and the first hour fed at
  // the same place keeps it
  learnInit(&t);
  CHECK(learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here) == BlobCheck::Ok);
  learnLoaded(&t, BlobCheck::Older, BlobCheck::Ok);
  CHECK(!t.mask_dirty && t.mask[17] == 0x62 && t.c.loc_sig == here);
  learnHour(t, lhour(1794297600u), lctx());
  CHECK(t.mask[17] == 0x62);
  mb.c[5] ^= 1;
  CHECK(learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here) == BlobCheck::Bad);
  // a fresh station's empty mask written before its first hour carries loc_sig 0; the first
  // hour adopts the place and marks the mask, so the next write stores it with the place and it loads back as Ok
  learnInit(&t);
  learnLoaded(&t, BlobCheck::Bad, BlobCheck::Bad);
  learnPackMask(t, &mb);
  t.mask_dirty = false;  // that write happened
  CHECK(mb.loc_sig == 0 && learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here) == BlobCheck::Bad);
  learnHour(t, lhour(1794297600u), lctx());
  CHECK(t.mask_dirty);
  learnPackMask(t, &mb);
  CHECK(mb.loc_sig == here && learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here) == BlobCheck::Ok);
  // a fresh or blob-less load marks the (empty) mask for the first write, so that write stores both blobs; a
  // loaded mask or a newer image's blobs (read-only) do not
  learnInit(&t);
  CHECK(!t.mask_dirty);
  learnLoaded(&t, BlobCheck::Bad, BlobCheck::Bad);
  CHECK(t.mask_dirty);
  learnInit(&t);
  learnLoaded(&t, BlobCheck::Ok, BlobCheck::Bad);
  CHECK(t.mask_dirty);
  learnInit(&t);
  learnLoaded(&t, BlobCheck::Ok, BlobCheck::Ok);
  CHECK(!t.mask_dirty);
  learnLoaded(&t, BlobCheck::Newer, BlobCheck::Bad);
  CHECK(!t.mask_dirty);
  learnLoaded(&t, BlobCheck::Ok, BlobCheck::Newer);
  CHECK(!t.mask_dirty);
  learnInit(&t);  // the empty mask written by that first write loads back as Ok
  t.c.loc_sig = here;
  learnPackMask(t, &mb);
  CHECK(learnUnpackMask(&t, mb, LEARN_MASK_BYTES, here) == BlobCheck::Ok);
  // every hour key once, in order
  learnInit(&s);
  const uint32_t h0 = 1794297600u;
  learnHour(s, lhour(h0), lctx());
  const uint32_t fed = s.hours_fed;
  learnHour(s, lhour(h0), lctx());
  learnHour(s, lhour(h0 - 3600), lctx());
  CHECK(s.hours_fed == fed && s.c.last_hour == h0);
  // an approximate-clock hour is not learned and does not move the resume point; a resume point in the
  // future (one wrong-forward record) is pulled back to now, after which the normal hours are fed
  HourRec ap = lhour(h0 + 3600);
  ap.quality = 1u << Q_CLOCK_APPROX;
  learnHour(s, ap, lctx());
  CHECK(s.c.last_hour == h0 && s.hours_fed == fed);
  learnHour(s, lhour(h0 + 400 * 86400u), lctx());  // a far-future key
  CHECK(s.c.last_hour == h0 + 400 * 86400u);
  s.core_dirty = false;
  CHECK(learnClampResume(s, h0 + 3600, 20261110) && s.c.last_hour == h0 + 3600 && s.core_dirty);
  CHECK(!learnClampResume(s, h0 + 3600, 20261110));
  const uint32_t fed2 = s.hours_fed;
  for (int i = 2; i < 6; i++) learnHour(s, lhour(h0 + 3600u * (uint32_t)i), lctx());
  CHECK(s.hours_fed == fed2 + 4 && s.c.last_hour == h0 + 5 * 3600);
  s.c.last_day = 20270101;
  CHECK(learnClampResume(s, h0 + 6 * 3600, 20261110) && s.c.last_day == 20261110);
}

// rings to 10:00, the clock back to 06:00, then right again: every hour fed once (the live pass feeds
// the RAM hours above the resume point, which never moves below the newest stored key)
static void testLearnClampRings() {
  static LearnState a, b;
  learnInit(&a);
  learnInit(&b);
  const uint32_t d0 = 1794268800u;  // 2026-11-10 00:00Z
  std::vector<HourRec> ring;
  auto live = [&](LearnState& s, uint32_t now_hour, bool clampToRings) {  // one live pass of app_learn.cpp
    learnClampResume(s, now_hour, 20261110, clampToRings && !ring.empty() ? ring.back().hour : 0, 0);
    for (const HourRec& r : ring)
      if (r.hour > s.c.last_hour) learnHour(s, r, lctx(true));
  };
  for (int h = 0; h <= 10; h++) {
    ring.push_back(lhour(d0 + 3600u * (uint32_t)h, 4 + h * .5f));
    live(a, d0 + 3600u * (uint32_t)h, true);
    live(b, d0 + 3600u * (uint32_t)h, true);
  }
  live(a, d0 + 6 * 3600, true);  // the clock back to 06:00 (a: clamped to the newest stored key)
  CHECK(a.c.last_hour == d0 + 10 * 3600);
  for (int h = 11; h <= 14; h++) {
    ring.push_back(lhour(d0 + 3600u * (uint32_t)h, 9));
    live(a, d0 + 3600u * (uint32_t)h, true);
    live(b, d0 + 3600u * (uint32_t)h, true);
  }
  CHECK(a.hours_fed == b.hours_fed && memcmp(&a.c, &b.c, sizeof a.c) == 0);
  // a clamp to the clock only feeds hours 06..10 a second time
  static LearnState old;
  learnInit(&old);
  ring.clear();
  for (int h = 0; h <= 10; h++) ring.push_back(lhour(d0 + 3600u * (uint32_t)h, 4 + h * .5f)), live(old, d0 + 3600u * (uint32_t)h, false);
  const uint32_t fed = old.hours_fed;
  live(old, d0 + 6 * 3600, false);
  CHECK(old.hours_fed == fed + 4);
}

// a cleared location is an unknown place: nothing reset, the location learners pause, L4 goes on;
// the same place again resets nothing; another place resets as before; a mask blob loads against its own place
static void testLearnNoLocation() {
  static LearnState a;
  learnInit(&a);
  const uint32_t d0 = 1794268800u;
  LearnCtx c = lctx();
  learnHour(a, lhour(d0), c);
  a.c.l1[2].e = 7000, a.c.l1[2].n = 900, a.c.l5_ce = 170, a.c.l6_w = 20u * 65536u, a.mask[3] = 0x62;
  const LearnCore before = a.c;
  uint8_t mask0[sizeof a.mask];
  memcpy(mask0, a.mask, sizeof mask0);
  const uint16_t l4n = a.c.l4_n;
  [[maybe_unused]] const uint32_t loc0 = GV(a.gates[LG_LOCATION]);
  LearnCtx none = c;
  none.lat = NAN, none.lon = NAN;
  uint8_t ev = 0;
  for (int h = 1; h <= 48; h++) ev |= learnHour(a, lhour(d0 + 3600u * (uint32_t)h), none);
  CHECK(!(ev & LE_LOC) && memcmp(mask0, a.mask, sizeof mask0) == 0 && a.c.loc_sig == before.loc_sig &&
        memcmp(a.c.l1, before.l1, sizeof a.c.l1) == 0 && a.c.l5_ce == 170 && a.c.l6_w == before.l6_w && a.c.l4_n > l4n &&
        GT(a.gates[LG_LOCATION] > loc0));
  CHECK(!(learnHour(a, lhour(d0 + 49 * 3600), c) & LE_LOC) && a.c.l1[2].e == 7000 && a.mask[3] == 0x62);  // the same place
  LearnCtx far = c;
  far.lon = 21;
  CHECK((learnHour(a, lhour(d0 + 50 * 3600), far) & LE_LOC) && a.c.l1[2].e == 5500 && a.mask[3] == 0);  // a move
  // the mask blob, read with no location set, against its own place
  static LearnState b;
  learnInit(&b);
  learnHour(b, lhour(d0), c);
  b.mask[5] = 0x21;
  static LearnMaskBlob blob;
  learnPackMask(b, &blob);
  static LearnState r;
  learnInit(&r);
  CHECK(learnUnpackMask(&r, blob, LEARN_MASK_BYTES, blob.loc_sig) == BlobCheck::Ok && r.mask[5] == 0x21);
  learnHour(r, lhour(d0 + 3600), none);  // the fresh core's first record, no place: the mask's place kept
  CHECK(r.c.loc_sig == blob.loc_sig && r.mask[5] == 0x21);
}

static void testLearnGates() {
  static LearnState s;
  const uint32_t day = 1794268800u;  // 2026-11-10 00:00Z
  // indoor: the sun up, solar 0, 22.7 °C, RH 36 (indoor hours)
  learnInit(&s);
  for (int h = 6; h < 14; h++) {
    HourRec r = lhour(day + 3600u * (uint32_t)h, 22.7f);
    r.rh_avg = 36, r.solar_avg = 0, r.solar_max = 0;
    learnHour(s, r, lctx());
  }
  CHECK(s.c.indoor >= 2 && GT(s.gates[LG_INDOOR] > 0));
  // frozen T for 6 hours
  learnInit(&s);
  for (int h = 0; h < 8; h++) {
    HourRec r = lhour(day + 3600u * (uint32_t)h);
    r.t_min = r.t_max = r.t_avg;
    learnHour(s, r, lctx());
  }
  CHECK(GT(s.gates[LG_FROZEN] == 3));  // hours 6, 7, 8 (the 6th frozen hour on)
  // solar at night -> geo, three such days pause L1
  learnInit(&s);
  for (int d = 0; d < 4; d++) {
    HourRec r = lhour(day + 86400u * (uint32_t)d + 23 * 3600);
    r.solar_avg = 60, r.solar_max = 80;
    learnHour(s, r, lctx());
  }
  CHECK(s.c.geo_days >= 3 && GT(s.gates[LG_GEO] >= 4));
  // freezing rain -> iced anemometer: calm-zero hours not admitted to L4 until the wind turns
  learnInit(&s);
  HourRec ice = lhour(day + 3600 * 3, .5f);
  ice.rain_sum = 30;
  learnHour(s, ice, lctx());
  HourRec z = lhour(day + 3600 * 4, .5f);
  z.wind_avg = z.gust_max = 0;
  learnHour(s, z, lctx());
  CHECK(GT(s.gates[LG_ICED] == 1) && (s.c.gbits & LB_ICED));
  learnHour(s, lhour(day + 3600 * 5), lctx());
  CHECK(!(s.c.gbits & LB_ICED) && (s.c.gbits & LB_DOME));  // the dome flag rides with icy rain too
  // RH stuck at the cap while T rises with sun -> L2 paused
  learnInit(&s);
  for (int h = 0; h < 14; h++) {
    HourRec r = lhour(day + 3600u * (uint32_t)h, 2 + h * .5f);
    r.rh_min = r.rh_max = r.rh_avg = 99, r.solar_avg = 40, r.solar_max = 50;
    learnHour(s, r, lctx());
  }
  CHECK(s.c.gbits & LB_STUCK);
  HourRec w1 = lhour(day + 3600 * 14), w2 = lhour(day + 3600 * 15);
  w1.rain_sum = w2.rain_sum = 50, w1.rh_max = w2.rh_max = 99, w1.rh_min = w2.rh_min = 99;
  learnHour(s, w1, lctx());
  learnHour(s, w2, lctx());
  CHECK(GT(s.gates[LG_STUCK] >= 1) && s.c.l2_wet_n == 0);
  // the factory pressure day resets L7 and is not admitted; normal days build it (against a flat test normal of
  // 1013.0 hPa; without a built-in table L7 never flags)
  static const int8_t flat[2 * MSLP_ROWS * MSLP_COLS] = {};
  learnSetMslpTable(flat);
  learnInit(&s);
  DayRec d;
  for (int i = 0; i < 100; i++) {
    clearDay(&d, dayNumberToYmd(20400 + i));
    d.quality = 0, d.hours = 24, d.epoch = 0, d.p_avg = 10132;
    learnDay(s, d, lctx());
  }
  CHECK(s.c.l7_n >= 30 * 64 && s.c.l7_flag == 0 && std::abs((int)((s.c.l7_e + 32768) >> 16) - 10132) <= 1);
  clearDay(&d, dayNumberToYmd(20500));
  d.quality = 1u << Q_BARO_UNCAL, d.hours = 24, d.p_avg = 10132;
  learnDay(s, d, lctx());
  CHECK(s.c.l7_n == 0 && GT(s.gates[LG_BARO] == 1));
  for (int i = 1; i < 100; i++) {  // a pressure 40 hPa above the normal: flagged "high" from stage 1 (14 days)
    clearDay(&d, dayNumberToYmd(20500 + i));
    d.quality = 0, d.hours = 24, d.epoch = 0, d.p_avg = 10532;
    learnDay(s, d, lctx());
    if (i == 13) CHECK(s.c.l7_flag == 0 && s.c.l7_n < 12 * 64);
    if (i == 14) CHECK(s.c.l7_flag == 2 && s.c.l7_n >= 12 * 64);
  }
  CHECK(s.c.l7_flag == 2);
  learnSetMslpTable(nullptr);
  // a hoar night closes admitted: the dome is set, the next morning's dull hours are rejected until k >= 0.8 E
  learnInit(&s);
  const uint32_t ev = 1794330000u - 1794330000u % 3600;  // 2026-11-10 ~17:00Z
  uint8_t evs = 0;
  for (int h = 0; h < 20; h++) {
    HourRec r = lhour(ev + 3600u * (uint32_t)h, -1);
    r.td_avg = -25, r.wind_avg = 2, r.gust_max = 5, r.calm_n = 50, r.rh_max = 96;
    r.solar_avg = r.solar_max = 0;
    evs |= learnHour(s, r, lctx());
  }
  CHECK((evs & LE_NIGHT) && s.last_night.admitted && (s.last_night.flags & LN_HOAR) && (s.c.gbits & LB_DOME));
  CHECK(s.last_night.rhmax == 96 && s.c.l2_night_n > 0);
}

static void testLearnSunnyNovember() {
  // 20 clear days (site factor 0.65, AM horizon 13 deg SE, PM 10 deg SW, a noon roof band), live bins for L1b
  static LearnState s;
  learnInit(&s);
  const uint32_t d0 = 1793491200u;  // 2026-11-01 00:00Z
  for (int d = 0; d < 20; d++)
    for (int h = 0; h < 24; h++) {
      const uint32_t hr = d0 + 86400u * (uint32_t)d + 3600u * (uint32_t)h;
      HourRec r = lhour(hr);
      LearnCtx c = lctx(true);
      synthSolar(hr, .65, true, &r, &c);
      learnHour(s, r, c);
    }
  int medium = 0, visited = 0, far = 0;
  for (const LearnCell& x : s.c.l1) {
    if (!x.n) continue;
    visited++;
    medium += learnConfOf(x.n, 5 * 64, 20 * 64, x.day & 0x3FFF, x.day & 0x3FFF) >= LC_MEDIUM;
    // within one x4 up-step (0.072) of the truth at medium: this fixture has no hour-to-hour noise, so a
    // warm-up step past 0.65 settles only at the x1 down-step (0.002 per hour); testLearnNoisyTrials checks the noisy case
    far += x.n >= 10 * 64 && std::abs(x.e - 6500) > 750;
  }
  CHECK(visited >= 3 && medium >= 2 && far == 0);
  // L1b: the cells whose centre the synthetic obstacles cover are shade, the open ones sun
  int sun = 0, shade = 0, wrong = 0;
  for (int i = 0; i < L1B_CELLS; i++) {
    const uint8_t k = l1bClass(s.mask[i]);
    if (!k) continue;
    k == 1 ? sun++ : shade++;
    // the cell's centre
    double el, az;
    if (i < 648) el = 3 + (i / 72) * 2 + 1, az = (i % 72) * 5 + 2.5;
    else if (i < 828) el = 21 + ((i - 648) / 36) * 4 + 2, az = ((i - 648) % 36) * 10 + 5;
    else el = 45, az = 180;
    wrong += (k == 2) != synthShade(el, az);
  }
  printf("  info: sunny November: L1 cells visited %d (>= medium %d); L1b sun %d / shade %d, off-centre %d\n", visited, medium,
         sun, shade, wrong);
  CHECK(sun > 10 && shade > 3 && wrong * 5 <= sun + shade);  // edge cells may straddle an obstacle
  // cloudy only: 30 days of steady k 0.1-0.4 -> every L1 cell stays default, the mask untouched
  learnInit(&s);
  for (int d = 0; d < 30; d++)
    for (int h = 0; h < 24; h++) {
      const uint32_t hr = d0 + 86400u * (uint32_t)d + 3600u * (uint32_t)h;
      HourRec r = lhour(hr);
      LearnCtx c = lctx(true);
      synthSolar(hr, .1 + .3 * lrand(), false, &r, &c);
      learnHour(s, r, c);
    }
  int notDefault = 0, maskSet = 0;
  for (const LearnCell& x : s.c.l1) notDefault += learnConfOf(x.n, 5 * 64, 20 * 64, x.day & 0x3FFF, x.day & 0x3FFF) != LC_DEFAULT;
  for (uint8_t m : s.mask) maskSet += m != 0;
  CHECK(notDefault == 0 && maskSet == 0);
  // 60 days of steady bright overcast (k 0.30-0.40, under every band's floor) never move a
  // cell: no count, no step down
  learnInit(&s);
  for (int d = 0; d < 60; d++)
    for (int h = 0; h < 24; h++) {
      const uint32_t hr = d0 + 86400u * (uint32_t)d + 3600u * (uint32_t)h;
      HourRec r = lhour(hr);
      LearnCtx c = lctx(true);
      synthSolar(hr, .30 + .10 * lrand(), false, &r, &c);
      learnHour(s, r, c);
    }
  notDefault = 0;
  int moved = 0;
  for (const LearnCell& x : s.c.l1) {
    notDefault += learnConfOf(x.n, 5 * 64, 20 * 64, x.day & 0x3FFF, x.day & 0x3FFF) != LC_DEFAULT;
    moved += x.e != 5500 || x.n;
  }
  CHECK(notDefault == 0 && moved == 0);
  // ... while a dim clear site (k 0.45, above the floors) pulls a 0.60 prior down to its level: every visited cell
  // moves down (75 steps of 0.002 at most 2 a day), and each cell with enough clear hours to count (n >= 8) ends within
  // 0.05. From August, so the high bands are visited too.
  learnInit(&s);
  for (LearnCell& x : s.c.l1) x.e = 6000;
  const uint32_t aug = 1785542400u;  // 2026-08-01 00:00Z
  for (int d = 0; d < 120; d++)
    for (int h = 0; h < 24; h++) {
      const uint32_t hr = aug + 86400u * (uint32_t)d + 3600u * (uint32_t)h;
      HourRec r = lhour(hr);
      LearnCtx c = lctx();
      synthSolar(hr, .45, false, &r, &c);
      learnHour(s, r, c);
    }
  int conv = 0, counted = 0, seen = 0, down = 0;
  for (int i = 0; i < L1_CELLS; i++) {
    const LearnCell& x = s.c.l1[i];
    if (x.e == 6000 && !x.n) continue;
    seen++;
    down += x.e < 5600;
    if (x.n >= 8 * 64) counted++, conv += std::abs(x.e - 4500) <= 500;
  }
  printf("  info: dim clear site (k 0.45, prior 0.60): %d cells visited, %d moved down, %d of %d counted cells within 0.05\n",
         seen, down, conv, counted);
  CHECK(seen >= 6 && down == seen && counted >= 4 && conv == counted);
  // a low-scaled site (f 0.45): report the clear hours until a cell is confident
  learnInit(&s);
  int hours = 0, firstMedium = -1;
  for (int d = 0; d < 40 && firstMedium < 0; d++)
    for (int h = 0; h < 24; h++) {
      const uint32_t hr = d0 + 86400u * (uint32_t)d + 3600u * (uint32_t)h;
      HourRec r = lhour(hr);
      LearnCtx c = lctx();
      synthSolar(hr, .45, false, &r, &c);
      learnHour(s, r, c);
      if (r.solar_avg > 20) hours++;
      for (const LearnCell& x : s.c.l1)
        if (firstMedium < 0 && learnConfOf(x.n, 5 * 64, 20 * 64, x.day & 0x3FFF, x.day & 0x3FFF) >= LC_MEDIUM) firstMedium = hours;
    }
  printf("  info: low-scaled site (f 0.45): first cell at medium after %d clear daylight hours\n", firstMedium);
  CHECK(firstMedium > 0);
  // a clear June day at 52 N (the longest sun path) fits the day scratch, one entry per cell
  learnInit(&s);
  const uint32_t june = 1782000000u;  // 2026-06-21 00:00Z
  int most = 0;
  for (int h = 0; h < 24; h++) {
    const uint32_t hr = june + 3600u * (uint32_t)h;
    HourRec r = lhour(hr, 20);
    LearnCtx c = lctx(true);
    synthSolar(hr, .7, false, &r, &c);
    learnHour(s, r, c);
    most = s.sc_n > most ? s.sc_n : most;
  }
  int committed = 0;
  for (uint8_t m : s.mask) committed += m != 0;
  printf("  info: clear June day at 52 N: %d cells in the day scratch (of %d), %d committed\n", most, L1B_SCRATCH, committed);
  CHECK(GT(s.gates[LG_SCRATCH] == 0) && most <= L1B_SCRATCH && committed == most && committed > 40);
}

static void testLearnQuantiles() {
  static LearnState s;
  learnInit(&s);
  const uint32_t h0 = 1793491200u;
  // L4: hourly mean wind uniform 0..10 km/h -> P50 ~ 5, P90 ~ 9 within 3 x memory; L3: readings 0.8..9 km/h -> P5
  // the frugal estimate wanders by one step per sample: judged on its mean over the last memory
  double m50 = 0, m90 = 0, m5 = 0;
  const int N = 3 * 1440 + 500;
  for (int i = 0; i < N; i++) {
    HourRec r = lhour(h0 + 3600u * (uint32_t)i);
    r.wind_avg = (uint16_t)lround(lrand() * 100);
    r.gust_max = (uint16_t)(r.wind_avg + 20);
    LearnCtx c = lctx(true);
    c.lat = c.lon = NAN;
    c.wind_n = 30;
    for (int k = 0; k < 30; k++) c.wind[k] = (uint8_t)lround(8 + lrand() * 82);  // 0.8 .. 9.0 km/h
    learnHour(s, r, c);
    if (i >= N - 1440) m50 += s.c.l4_p50 / 1e3 / 1440, m90 += s.c.l4_p90 / 1e3 / 1440, m5 += s.c.l3_e / 1e4 / 1440;
  }
  const auto below = [](double e, double lo, double hi) { return (e - lo) / (hi - lo); };  // uniform CDF
  const double q50 = below(m50, 0, 10), q90 = below(m90, 0, 10), q5 = below(m5, .8, 9);
  printf("  info: L4 P50 %.2f (q %.3f), P90 %.2f (q %.3f); L3 P5 %.2f km/h (q %.3f), means over the last memory\n", m50, q50,
         m90, q90, m5, q5);
  CHECK(std::fabs(q50 - .5) <= .03 && std::fabs(q90 - .9) <= .03 && std::fabs(q5 - .05) <= .03);
  CHECK(learnConfOf(s.c.l4_n, 240 * 16, 960 * 16, s.c.l4_day, s.c.l4_day) == LC_HIGH);
  // L2 a: a sensor 8 % low (92 in rain) within +-2 % after 40 wet hours (cap 2 per day: 3 wet hours a day)
  learnInit(&s);
  int counted = 0;
  double at8 = 0;
  for (int d = 0; d < 40 && counted < 40; d++)
    for (int h = 0; h < 4; h++) {
      HourRec r = lhour(h0 + 86400u * (uint32_t)d + 3600u * (uint32_t)(10 + h));
      r.rain_sum = 50, r.rh_avg = 92, r.rh_max = 93, r.rh_min = 90;
      const uint16_t n0 = s.c.l2_wet_n;
      learnHour(s, r, lctx());
      if (s.c.l2_wet_n != n0 && ++counted == 8) at8 = s.c.l2_wet_e / 100.0;
    }
  printf("  info: L2 a after 8 wet hours %.2f %%, after 40 %.2f %% (true 92)\n", at8, s.c.l2_wet_e / 100.0);
  CHECK(counted >= 40 && std::fabs(s.c.l2_wet_e / 100.0 - 92) <= 2);
  // per-day cap: three identical clear hours of one cell on one day -> the third is capped
  learnInit(&s);
  const uint32_t dd = 1794268800u + 10 * 3600 + 600;  // around solar noon: the mean elevation stays in one band
  LearnCtx c = lctx();
  for (int i = 0; i < 3; i++) {
    HourRec r = lhour(dd + 86400u * 0);
    r.hour = dd + 60u * (uint32_t)i;  // the same hour band (keys must still increase)
    synthSolar(dd, .65, false, &r, &c);
    learnHour(s, r, c);
  }
  if (GT(s.gates[LG_CAP] != 1) && FC_DIAG_BENCH)
    for (int g = 0; g < L1_CELLS; g++) printf("  cell %d e %u n %u day %04x\n", g, s.c.l1[g].e, s.c.l1[g].n, s.c.l1[g].day);
  CHECK(GT(s.gates[LG_CAP] == 1));
  // property: 10 years of random hours never leave the bounds
  learnInit(&s);
  for (uint32_t i = 0; i < 87600; i++) {
    HourRec r = lhour(h0 + 3600u * i, (float)(-30 + 70 * lrand()));
    r.solar_avg = (uint16_t)(lrand() * 1200), r.solar_max = (uint16_t)(r.solar_avg + lrand() * 100);
    r.rh_avg = (uint8_t)(1 + lrand() * 99), r.rain_sum = (uint16_t)(lrand() < .1 ? lrand() * 300 : 0);
    r.wind_avg = (uint16_t)(lrand() * 600), r.gust_max = (uint16_t)(r.wind_avg + lrand() * 300);
    LearnCtx k = lctx(true);
    k.wind_n = 5;
    for (int j = 0; j < 5; j++) k.wind[j] = (uint8_t)(lrand() * 255);
    learnHour(s, r, k);
  }
  bool in = s.c.l2_wet_e >= 8500 && s.c.l2_wet_e <= 10000 && s.c.l3_e >= 3000 && s.c.l3_e <= 50000 && s.c.l4_p50 >= 500 &&
            s.c.l4_p50 <= 20000 && s.c.l4_p90 >= 3000 && s.c.l4_p90 <= 50000;
  for (const LearnCell& x : s.c.l1) in = in && x.e >= 3500 && x.e <= 13000;
  CHECK(in);
}

static void testLearnResetsAndReplay() {
  static LearnState a, b;
  const uint32_t d0 = 1793491200u;
  // live == replay: the same hours with live bins and without; the ring-fed state is bit-identical
  learnInit(&a);
  learnInit(&b);
  for (int i = 0; i < 24 * 5; i++) {
    const uint32_t hr = d0 + 3600u * (uint32_t)i;
    HourRec r = lhour(hr);
    LearnCtx live = lctx(true), ring = lctx();
    synthSolar(hr, .65, true, &r, &live);
    synthSolar(hr, .65, true, &r, &ring);
    learnHour(a, r, live);
    learnHour(b, r, ring);
  }
  CHECK(memcmp(&a.c, &b.c, sizeof a.c) == 0);
  // catch-up == uninterrupted: a blob from 6 h earlier + the ring
  static LearnState full, part;
  learnInit(&full);
  learnInit(&part);
  LearnCore saved = {};
  for (int i = 0; i < 24 * 4; i++) {
    const uint32_t hr = d0 + 3600u * (uint32_t)i;
    HourRec r = lhour(hr);
    LearnCtx c = lctx();
    synthSolar(hr, .6, false, &r, &c);
    learnHour(full, r, c);
    if (i < 24 * 3) learnHour(part, r, c);
    if (i == 24 * 3 - 7) saved = part.c;  // the last write, 6 h before the cut
  }
  learnSealCore(&saved);
  CHECK(learnCheckCore(saved, sizeof saved) == BlobCheck::Ok);
  learnInit(&part);
  part.c = saved;
  for (int i = 0; i < 24 * 4; i++) {
    const uint32_t hr = d0 + 3600u * (uint32_t)i;
    HourRec r = lhour(hr);
    LearnCtx c = lctx();
    synthSolar(hr, .6, false, &r, &c);
    learnHour(part, r, c);  // the keys up to last_hour are skipped
  }
  full.c.crc = part.c.crc = 0;
  part.c.magic[0] = full.c.magic[0], part.c.magic[1] = full.c.magic[1];
  CHECK(memcmp(&full.c, &part.c, sizeof full.c) == 0);
  // resets: location -> L1 + mask priors; profile -> all; console MAC -> L7; epoch -> re-learning
  learnInit(&a);
  LearnCtx c = lctx();
  c.mac_hash = 7;
  learnHour(a, lhour(d0), c);
  a.c.l1[2].e = 7000, a.c.l1[2].n = 900, a.c.l4_p50 = 9000, a.c.l7_n = 500, a.mask[3] = 0x62;
  c.lon = 21;
  // L4 kept (one step down, x4 below its first bar)
  CHECK((learnHour(a, lhour(d0 + 3600), c) & LE_LOC) && a.c.l1[2].e == 5500 && a.mask[3] == 0 && a.c.l4_p50 == 8800);
  c.mac_hash = 8;
  CHECK((learnHour(a, lhour(d0 + 7200), c) & LE_MAC) && a.c.l7_n == 0);
  c.profile = 0;
  a.mask[9] = 0x62;
  CHECK((learnHour(a, lhour(d0 + 3 * 3600), c) & LE_PROFILE) && a.c.l4_p50 == 3000 && a.mask[9] == 0);  // L1b too
  a.c.l1[2].n = 900;
  HourRec ep = lhour(d0 + 4 * 3600);
  ep.epoch = 1;
  // every n back to 0 (below the first bar), the value kept
  CHECK((learnHour(a, ep, c) & LE_RELEARN) && a.c.epoch == 1 && a.c.l1[2].n == 0 && a.c.l1[2].e == 5500 &&
        a.c.relearn_until != 0);
  // no location: L1 waits (location gate), L4 goes on
  learnInit(&a);
  c = lctx();
  c.lat = c.lon = NAN;
  for (int i = 0; i < 30; i++) learnHour(a, lhour(d0 + 3600u * (uint32_t)i), c);
  CHECK(GT(a.gates[LG_LOCATION] == 30) && a.c.l4_n > 0);
  // the backfill order (every hour, then every day) with an epoch change in the window re-learns once: the
  // days of the old epoch do not pull it back
  learnInit(&a);
  c = lctx();
  uint8_t evs = 0;
  for (int i = 0; i < 24 * 4; i++) {
    HourRec r = lhour(d0 + 3600u * (uint32_t)i);
    r.epoch = i < 24 * 2 ? 0 : 1;
    evs = (uint8_t)(evs | learnHour(a, r, c));
  }
  int relearns = (evs & LE_RELEARN) ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(20758 + i));  // 2026-11-01 ..
    dr.quality = 0, dr.hours = 24, dr.p_avg = 10132, dr.epoch = i < 2 ? 0 : 1;
    if (learnDay(a, dr, c) & LE_RELEARN) relearns++;
  }
  HourRec more = lhour(d0 + 3600u * 24 * 4);
  more.epoch = 1;
  if (learnHour(a, more, c) & LE_RELEARN) relearns++;
  CHECK(relearns == 1 && a.c.epoch == 1 && a.c.l7_n > 0);
  // `hit` is a clamped step with samples: false at the priors (L2's 100 % is its bound), true once a
  // learner is pushed against a bound
  learnInit(&a);
  {
    std::string js0;
    char b0[96];
    Out o0(toString, &js0, b0, sizeof b0);
    static LearnView v0;
    learnView(a, &v0);
    LearnMeta m0 = {"learning", nullptr, 0, true, 0, 0, 0, 0, d0, 52, 20, 0, 0, 0, 0, 0, 0};
    learnJson(o0, v0, m0);
    o0.flush();
    CHECK(contains(js0, "\"conf\":\"default\",\"hit\":false,\"need\":9,\"state\":\"default\"},\"night\"") &&
          !contains(js0, "\"hit\":true"));
  }
  for (int d = 0; d < 45; d++)  // 2 counted wet hours a day, 0.25 % per step: 60 steps from 100 to the 85 % bound
    for (int h = 0; h < 3; h++) {
      HourRec r = lhour(d0 + 86400u * (uint32_t)d + 3600u * (uint32_t)(10 + h));
      r.rain_sum = 50, r.rh_avg = 70, r.rh_max = 72, r.rh_min = 68;  // a sensor far under 85 % in rain
      learnHour(a, r, c);
    }
  CHECK(a.c.l2_wet_e == 8500 && (a.hit & LH_L2W));
  // the JSON renders and parses (shape only)
  std::string js;
  char buf[96];
  Out o(toString, &js, buf, sizeof buf);
  static LearnView v;
  learnView(full, &v);
  LearnMeta m = {"learning", nullptr, 96, true, 0, 0, 10, 400, d0 + 4 * 86400, 52, 20, 30, 1234, 0, 0, 0, 0};
  learnJson(o, v, m);
  o.flush();
  CHECK(contains(js, "{\"v\":1,\"state\":\"learning\",\"why\":null") && contains(js, "\"l1\":{\"def\":0.5500") &&
        contains(js, "\"l1b\":{") && contains(js, "\"l7\":{\"e\":") && contains(js, "\"night\":{\"open\":"));
  // the backfill figures, key order exactly so
#if FC_DIAG_BENCH
  CHECK(contains(js, "\"backfill\":{\"hours\":96,\"days\":30,\"ms\":1234,\"done\":true,\"replay\":{"));
  // migrated_from just before the backfill; replay always present (bench build)
  CHECK(contains(js, "\"relearn\":false,\"migrated_from\":0,\"backfill\":{") &&
        contains(js, "\"done\":true,\"replay\":{\"hours\":0,\"ms\":0,\"learners\":[]}},\"written\""));
#else  // backfill.ms, replay{} and gates{} are absent from the release body
  CHECK(contains(js, "\"relearn\":false,\"migrated_from\":0,\"backfill\":{\"hours\":96,\"days\":30,\"done\":true},\"written\""));
  CHECK(!contains(js, "\"replay\"") && !contains(js, "\"gates\"") && !contains(js, "\"ms\":1234"));
#endif
#if FC_DIAG_BENCH
  {
    LearnMeta mm = m;
    mm.migrated_from = 1, mm.replay_mask = LM_SINCE_V1, mm.replay_hours = 2853, mm.replay_ms = 140123;
    std::string j2;
    Out o2(toString, &j2, buf, sizeof buf);
    learnJson(o2, v, mm);
    o2.flush();
    CHECK(contains(j2, "\"migrated_from\":1,\"backfill\":{") &&
          contains(j2, "\"replay\":{\"hours\":2853,\"ms\":140123,\"learners\":[\"l5\",\"l6\"]}}"));
  }
#endif
  printf("  info: /api/learn body %u B\n", (unsigned)js.size());
}

// ---------------------------------------------------------------- evening / night, L5, L6
static uint16_t lday14(uint32_t unix) { return (uint16_t)((learnDayOf(unix, 20) - LDAY_BASE) & 0x3FFF); }
static double lgauss() {  // SD 1, no libm
  double v = 0;
  for (int i = 0; i < 12; i++) v += lrand();
  return v - 6;
}
// One synthetic evening + night at 52 N 20 E, fed noon to noon (the hours of [d0 + 12 h, d0 + 36 h), d0 a UTC midnight).
struct NightP {
  float te = 8;         // the afternoon / E0 temperature
  float rate = 1.5f;    // evening cooling, °C per hour over E0 .. E0 + 3
  float drop = -1;      // T0 - Tmin (°C); < 0: 5 °C
  float spread = 4;     // T0 - Td0 (°C)
  float sky = 0;        // afternoon clear-sky factor (0 = no solar values)
  bool rain = false, wind = false, rise = false, restart = false, gap = false, partial = false;
  bool eveWind = false, eveRain = false, eveGap = false;
  int riseK = 5;        // the night hour of the rise (-1: the last one, after sunrise)
  uint8_t epoch = 0;    // the sensor epoch every hour carries
  int rhmax = -1;       // the night hours' rh_max (-1: lhour's 85)
};
struct NightGeo {
  uint32_t set, e0, start, end, rise2;
  int len, lq;  // hours, 0.1 h
};
static NightGeo nightGeo(uint32_t d0) {
  NightGeo g;
  uint32_t r = 0;
  sunTimes(d0 + 12 * 3600, 52, 20, &r, &g.set);
  g.e0 = (g.set + 3599) / 3600 * 3600, g.start = g.e0 + 3600;
  uint32_t s2 = 0;
  sunTimes(g.start + 86400, 52, 20, &g.rise2, &s2);
  g.end = (g.rise2 + 3600 + 3599) / 3600 * 3600;
  g.len = (int)((g.end - g.start) / 3600), g.lq = (int)((g.rise2 - g.set + 180) / 360);
  return g;
}
static uint8_t feedNight(LearnState& s, uint32_t d0, const NightP& p, LearnCtx c = lctx(), uint16_t mask = LM_ALL,
                         std::vector<HourRec>* sink = nullptr) {
  const NightGeo g = nightGeo(d0);
  const float t0 = p.te - 2 * p.rate, t2 = p.te - 3 * p.rate, tmin = t0 - (p.drop < 0 ? 5 : p.drop);
  const float td0 = t0 - p.spread;
  uint8_t ev = 0;
  for (uint32_t h = d0 + 12 * 3600; h < d0 + 36 * 3600; h += 3600) {
    float t = p.te;
    bool nightH = h >= g.start && h < g.end;
    const int k = nightH ? (int)((h - g.start) / 3600) : -1;
    if (k == 0) t = p.te - p.rate;
    else if (k == 1) t = t0;
    else if (k == 2) t = t2;
    else if (k > 2) t = t2 - (t2 - tmin) * (float)(k - 2) / (float)(g.len - 3);
    if (p.rise && k == (p.riseK < 0 ? g.len - 1 : p.riseK)) t += 2.0f;  // +2 °C in an hour
    if ((p.gap && k == 5) || (p.partial && k >= 0 && k < 2) || (p.eveGap && h == g.e0)) continue;
    HourRec r = lhour(h, t);
    r.t_avg = (int16_t)lround(t * 10);
    r.epoch = p.epoch;
    if (h >= g.e0 && h < g.end) {
      r.t_min = r.t_avg, r.t_max = (int16_t)(r.t_avg + 3);
      const int16_t td = (int16_t)lround(td0 * 10);
      r.td_avg = td < r.t_avg ? td : r.t_avg;
    }
    LearnCtx cc = c;
    if (p.sky > 0 && h < g.e0) synthSolar(h, p.sky, false, &r, &cc);
    if (p.rain && k == 5) r.rain_sum = 20;
    if (p.wind && nightH) r.wind_avg = 80, r.gust_max = 120;
    if (p.restart && k == 5) r.quality = 1u << Q_RESTART;
    if (p.eveWind && h == g.e0) r.wind_avg = 80, r.gust_max = 120;
    if (p.eveRain && k == 1) r.rain_sum = 20;
    if (p.rhmax >= 0 && nightH) r.rh_max = (uint8_t)p.rhmax;
    if (sink) sink->push_back(r);
    ev |= learnHour(s, r, cc, mask);
  }
  return ev;
}
// the PM label cells (5-10 and 10-15 deg) made confident at e 0.65; the whole sky "sun" in the L1b mask (an
// overcast label needs a known sun cell)
static void confidentPm(LearnState& s, uint32_t d0) {
  for (int ci : {1, 3}) s.c.l1[ci] = LearnCell{6500, 20 * 64, lday14(d0 + 12 * 3600)};
  memset(s.mask, 0x62, sizeof s.mask);
}

static void testLearnWindows() {
  static LearnState s;
  const uint32_t nov = 1794268800u;  // 2026-11-10 00:00Z
  // E0 = the first hour at or after sunset; the night opens at E0 + 1 h; over 60 days the sunset walks through the hour
  learnInit(&s);
  int opened = 0, e0ok = 0, lenOk = 0;
  for (int d = 0; d < 60; d++) {
    const uint32_t d0 = nov - 30 * 86400u + 86400u * (uint32_t)d;
    const NightGeo g = nightGeo(d0);
    for (uint32_t h = d0 + 12 * 3600; h < d0 + 36 * 3600; h += 3600) {
      learnHour(s, lhour(h), lctx());
      if (h == g.e0) e0ok += (s.c.e_flags & EB_E0) && s.c.e_t0 == 80 && g.e0 >= g.set && g.e0 < g.set + 3600;
      if (h == g.start) opened += s.c.n_start == g.e0 + 3600, lenOk += std::fabs(s.c.n_L / 10.0 - (g.rise2 - g.set) / 3600.0) <= .05;
    }
  }
  CHECK(opened == 60 && e0ok == 60 && lenOk == 60);
  // a sunset exactly on the hour is its own E0; one second later the next hour is
  const uint32_t hh = 1794330000u - 1794330000u % 3600;
  CHECK(learnE0(hh) == hh && learnE0(hh + 1) == hh + 3600 && learnE0(hh + 3599) == hh + 3600 && learnE0(hh - 1) == hh);
  // night length: 21 Dec and 21 Jun at 52 N (sunset -> next sunrise)
  const NightGeo dec = nightGeo(1797811200u), jun = nightGeo(1782000000u);  // 2026-12-21, 2026-06-21
  printf("  info: night length at 52 N: 21 Dec %.1f h, 21 Jun %.1f h\n", dec.lq / 10.0, jun.lq / 10.0);
  CHECK(dec.lq >= 160 && dec.lq <= 168 && jun.lq >= 70 && jun.lq <= 78);
  learnInit(&s);
  feedNight(s, 1797811200u - 86400, NightP());
  feedNight(s, 1797811200u, NightP());
  CHECK(s.last_night.start == dec.start && s.last_night.l_h == dec.lq && s.last_night.len == dec.len);
  // LN_RISE: +2 °C at night (cloud arriving) is a rise; the same jump in the last hour (after sunrise) is not
  learnInit(&s);
  NightP pr;
  pr.rise = true;
  feedNight(s, nov, pr);
  CHECK(s.last_night.start == nightGeo(nov).start && (s.last_night.flags & LN_RISE) && s.last_night.w5 == s.last_night.len);
  learnInit(&s);
  pr.riseK = -1;
  feedNight(s, nov, pr);
  CHECK(s.last_night.start == nightGeo(nov).start && !(s.last_night.flags & LN_RISE));
  // the evening: a valid E0..E0+3 gives e_dt3 = -3 x rate; a windy E0, a rainy k = 1 or a missing E0 mark it bad
  learnInit(&s);
  feedNight(s, nov, NightP());
  CHECK((s.c.e_flags & EB_DONE) && !(s.c.e_flags & EB_BAD) && s.c.e_dt3 == -45);
  for (int which = 0; which < 3; which++) {
    learnInit(&s);
    NightP pe;
    (which == 0 ? pe.eveWind : which == 1 ? pe.eveRain : pe.eveGap) = true;
    feedNight(s, nov, pe);
    const uint8_t want = which == 0 ? EB_WIND : which == 1 ? EB_RAIN : EB_MISS;
    CHECK((s.c.e_flags & EB_DONE) && (s.c.e_flags & want) && GT(s.gates[LG_L5_EVENING] == 1));
  }
}

static void testLearnLabels() {
  static LearnState s;
  const uint32_t nov = 1794268800u;
  struct Case { float sky; uint8_t want; };
  const Case cases[] = {{.65f, LBL_CLEAR}, {.15f, LBL_OVERCAST}, {.40f, LBL_MIXED}};
  for (const Case& k : cases) {
    learnInit(&s);
    learnHour(s, lhour(nov + 11 * 3600), lctx());  // adopt the context (a midnight hour would open a partial night)
    confidentPm(s, nov);
    NightP p;
    p.sky = k.sky;
    feedNight(s, nov, p);
    CHECK(s.c.lbl == k.want && s.c.lbl_day == lday14(nov + 12 * 3600));
  }
  // a default cell: nocell; a dome or a geo pause: none
  learnInit(&s);
  NightP p;
  p.sky = .65f;
  feedNight(s, nov, p);
  CHECK(s.c.lbl == LBL_NOCELL);
  learnInit(&s);
  learnHour(s, lhour(nov + 11 * 3600), lctx());
  confidentPm(s, nov);
  s.c.gbits |= LB_DOME, s.c.icy_h = 0;  // the dome does not melt on a dim afternoon
  p.sky = .15f;
  feedNight(s, nov, p);
  CHECK(s.c.lbl == LBL_NONE);
  learnInit(&s);
  learnHour(s, lhour(nov + 11 * 3600), lctx());
  confidentPm(s, nov);
  s.c.geo_days = 3;
  p.sky = .65f;
  feedNight(s, nov, p);
  CHECK(s.c.lbl == LBL_NONE);
  // yesterday's label is not used by today's evening
  learnInit(&s);
  learnHour(s, lhour(nov + 11 * 3600), lctx());
  confidentPm(s, nov);
  p.sky = .65f;
  feedNight(s, nov, p);  // a clear afternoon: one clear evening counted
  CHECK(s.c.l5_cn == 64 && GT(s.gates[LG_L5_LABEL] == 0));
  feedNight(s, nov + 86400, NightP());  // no afternoon solar today: the label stays yesterday's
  CHECK(s.c.l5_cn == 64 && GT(s.gates[LG_L5_LABEL] == 1));
}

static void testLearnL5() {
  static LearnState s;
  const uint32_t d0 = 1791763200u;  // 2026-10-12 00:00Z
  // 30 clear evenings at 1.8 °C/h and 30 overcast at 0.3 (+-0.2 noise), alternating
  learnInit(&s);
  learnHour(s, lhour(d0 + 11 * 3600), lctx());
  int nc = 0, no = 0, learnedAtC = -1, learnedAtO = -1;
  for (int d = 0; d < 60; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    confidentPm(s, day);
    NightP p;
    const bool clear = d % 2 == 0;
    p.sky = clear ? .65f : .15f, p.te = 12;  // above freezing all night: no hoar dome on the next afternoon
    p.rate = (float)((clear ? 1.8 : 0.3) + .2 * (2 * lrand() - 1));
    feedNight(s, day, p);
    (clear ? nc : no)++;
    L5Src src;
    bool hit;
    l5Threshold(s.c, &src, &hit);
    if (src == L5Src::Learned && learnedAtC < 0) learnedAtC = nc, learnedAtO = no;
  }
  L5Src src;
  bool hit;
  const int thr = l5Threshold(s.c, &src, &hit);
  printf("  info: L5 clear %.2f / overcast %.2f °C/h, thr %.2f, learned after %d clear + %d overcast evenings\n",
         s.c.l5_ce / 100.0, s.c.l5_oe / 100.0, thr / 100.0, learnedAtC, learnedAtO);
  CHECK(src == L5Src::Learned && !hit && std::abs(thr - 105) <= 10 && learnedAtC > 0 && learnedAtC <= 8 && learnedAtO <= 8);
  CHECK(GT(s.gates[LG_L5_LABEL] == 0 && s.gates[LG_L5_EVENING] == 0));
  // inverted classes: inseparable, the default stays
  learnInit(&s);
  learnHour(s, lhour(d0 + 11 * 3600), lctx());
  for (int d = 0; d < 30; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    confidentPm(s, day);
    NightP p;
    p.sky = d % 2 ? .65f : .15f, p.te = 12;
    p.rate = d % 2 ? .3f : 1.8f;
    feedNight(s, day, p);
  }
  CHECK(l5Threshold(s.c, &src, &hit) == 100 && src == L5Src::Inseparable);
  // a windy, a rainy and a gapped evening: l5_evening, nothing learned
  learnInit(&s);
  learnHour(s, lhour(d0 + 11 * 3600), lctx());
  for (int d = 0; d < 3; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    confidentPm(s, day);
    NightP p;
    p.sky = .65f;
    (d == 0 ? p.eveWind : d == 1 ? p.eveRain : p.eveGap) = true;
    feedNight(s, day, p);
  }
  CHECK(GT(s.gates[LG_L5_EVENING] == 3) && s.c.l5_cn == 0 && s.c.l5_ce == 150);
  // bounds: a warming evening clamps at -2, an extreme cooling at +5; a learned midpoint above 2.5 clamps (hit)
  learnInit(&s);
  learnHour(s, lhour(d0 + 11 * 3600), lctx());
  for (int d = 0; d < 20; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    confidentPm(s, day);
    NightP p;
    p.sky = d % 2 ? .65f : .15f;
    p.rate = d % 2 ? 9.0f : 4.0f;  // clear 9 °C/h -> 5.00; overcast 4.0
    p.drop = 20, p.te = 40;
    feedNight(s, day, p);
  }
  const int thr2 = l5Threshold(s.c, &src, &hit);
  CHECK(s.c.l5_ce == 500 && src == L5Src::Learned && thr2 == 250 && hit);
  learnInit(&s);
  learnHour(s, lhour(d0 + 11 * 3600), lctx());
  confidentPm(s, d0);
  NightP warm;
  warm.sky = .65f, warm.rate = -3.0f, warm.drop = 0;  // a 3 °C/h warming evening: clamped at -2
  feedNight(s, d0, warm);
  CHECK(s.c.l5_ce == -200 && s.c.l5_cn == 64);  // the first sample replaces the prior (running mean)
}

static void testLearnL6() {
  static LearnState s;
  const uint32_t sep = 1788220800u;  // 2026-09-01 00:00Z
  // a season Sep -> Mar: drop = 2.0 + 0.5 s + 0.3 L + N(0, 0.8), s 0.5 .. 12 °C
  learnInit(&s);
  int admitted = 0, readyAt = -1;
  for (int d = 0; d < 200; d++) {
    const uint32_t day = sep + 86400u * (uint32_t)d;
    const NightGeo g = nightGeo(day);
    NightP p;
    p.spread = (float)(.5 + 11.5 * lrand());
    p.te = (float)(4 + 10 * lrand());
    p.drop = (float)(2.0 + .5 * p.spread + .3 * g.lq / 10.0 + .8 * lgauss());
    if (p.drop < 1.6f) p.drop = 1.6f;
    feedNight(s, day, p);
    if (s.last_night.l6 == L6R_OK) admitted++;
    const bool ready = s.c.l6_w >= 15u * 65536u && (int64_t)s.c.l6_e2 * 256 <= 625ll * s.c.l6_w;
    if (ready && readyAt < 0) readyAt = admitted;
  }
  // held-out: the learned expectation vs the true one over the season's range
  double se = 0;
  int nh = 0;
  for (int i = 0; i < 400; i++) {
    const double sp = .5 + 11.5 * lrand(), L = 11 + 5.5 * lrand();
    const double truth = 2.0 + .5 * sp + .3 * L, got = s.c.l6_a / 100.0 + s.c.l6_b / 1e4 * sp + s.c.l6_c / 1e4 * L;
    se += (got - truth) * (got - truth), nh++;
  }
  const double rms = std::sqrt(se / nh);
  printf("  info: L6 season: %d nights admitted, ready after %d, a %.2f b %.3f c %.3f (true 2.0 / 0.5 / 0.3), held-out RMS "
         "%.2f °C, prospective SD %.2f\n",
         admitted, readyAt, s.c.l6_a / 100.0, s.c.l6_b / 1e4, s.c.l6_c / 1e4, rms,
         std::sqrt((double)s.c.l6_e2 * 256.0 / s.c.l6_w) / 10.0);
  CHECK(admitted >= 190 && readyAt > 0 && readyAt <= 25 && rms <= .3);
  // the reasons: rain, wind, rise, restart (quality), a missing hour, a partial night, a not-clear evening
  struct R { int which; uint8_t want; };
  const R rs[] = {{0, L6R_RAIN}, {1, L6R_WIND}, {2, L6R_RISE}, {3, L6R_QUALITY}, {4, L6R_MISSING}, {5, L6R_MISSING},
                  {6, L6R_NOT_CLEAR}, {7, L6R_EVENING}};
  for (const R& x : rs) {
    learnInit(&s);
    feedNight(s, sep, NightP());
    NightP p;
    switch (x.which) {
      case 0: p.rain = true; break;
      case 1: p.wind = true; break;
      case 2: p.rise = true; break;
      case 3: p.restart = true; break;
      case 4: p.gap = true; break;
      case 5: p.partial = true; break;
      case 6: p.rate = .5f; break;
      default: p.eveWind = true; break;
    }
    const uint32_t w0 = s.c.l6_w;
    [[maybe_unused]] const uint32_t g0 = GV(s.gates[LG_L6_NIGHT]);
    feedNight(s, sep + 86400, p);  // the night opens and closes within its noon-to-noon feed
    CHECK(w0 == 65536u && s.last_night.l6 == x.want && s.c.l6_w == w0 && GT(s.gates[LG_L6_NIGHT] == g0 + 1));
  }
  // a summer-only series (L 7-9 h) leaves c near its prior: the ridge holds what one season cannot identify
  learnInit(&s);
  const uint32_t may = 1779580800u;  // 2026-05-24
  for (int d = 0; d < 90; d++) {
    const uint32_t day = may + 86400u * (uint32_t)d;
    const NightGeo g = nightGeo(day);
    NightP p;
    p.spread = (float)(.5 + 11.5 * lrand());
    p.drop = (float)(2.0 + .5 * p.spread + .3 * g.lq / 10.0 + .8 * lgauss());
    if (p.drop < 1.6f) p.drop = 1.6f;
    feedNight(s, day, p);
  }
  double se2 = 0;
  for (int i = 0; i < 400; i++) {
    const double sp = .5 + 11.5 * lrand(), L = 7.3 + 2.4 * lrand();
    const double truth = 2.0 + .5 * sp + .3 * L, got = s.c.l6_a / 100.0 + s.c.l6_b / 1e4 * sp + s.c.l6_c / 1e4 * L;
    se2 += (got - truth) * (got - truth);
  }
  const double rms2 = std::sqrt(se2 / 400);
  printf("  info: L6 summer only: a %.2f b %.3f c %.3f (prior c 0.200, true 0.300), summer-range RMS %.2f °C\n",
         s.c.l6_a / 100.0, s.c.l6_b / 1e4, s.c.l6_c / 1e4, rms2);
  // the summer nights' lengths vary 7.3 -> 9.7 h, so c moves from its prior toward the truth; the fit is right where
  // the data is
  CHECK(s.c.l6_c >= 2000 && s.c.l6_c <= 3600 && rms2 <= .3);
  // nights of one fixed length cannot identify c: the ridge keeps it at its prior (the intercept takes the level)
  LearnCore k;
  memset(&k, 0, sizeof k);
  k.l6_a = 100, k.l6_b = 4000, k.l6_c = 2000;
  const auto dec = [](int32_t v) { return (int32_t)(((int64_t)v * 64410 + 32768) >> 16); };
  for (int i = 0; i < 60; i++) {
    const int sv = (int)lround(-45 + 115 * lrand()), lv = -35;
    const int y = (int)lround(20 + .5 * (sv + 50) + 3 * 8.5 + 8 * lgauss());
    k.l6_s = dec(k.l6_s) + sv * 256, k.l6_l = dec(k.l6_l) + lv * 256, k.l6_y = dec(k.l6_y) + y * 256;
    k.l6_ss = dec(k.l6_ss) + sv * sv * 256, k.l6_sl = dec(k.l6_sl) + sv * lv * 256, k.l6_ll = dec(k.l6_ll) + lv * lv * 256;
    k.l6_sy = dec(k.l6_sy) + sv * y * 256, k.l6_ly = dec(k.l6_ly) + lv * y * 256;
    k.l6_w = (uint32_t)(((uint64_t)k.l6_w * 64410 + 32768) >> 16) + 65536u;
  }
  l6Solve(k);
  printf("  info: L6 fixed night length: a %.2f b %.3f c %.4f\n", k.l6_a / 100.0, k.l6_b / 1e4, k.l6_c / 1e4);
  CHECK(std::abs(k.l6_c - 2000) <= 10);  // the Q8 decay rounding of the sums leaves a hair of information (0.0004)
}

// after an epoch change L6 cannot read "learned" for the 14 re-learning days (W scaled to 15 nights, the
// re-learning lambda's ceiling is 14.93 nights); the first admitted night after the window brings it back (intended:
// a moved sensor re-proves itself)
static bool l6Ready(const LearnCore& c) { return c.l6_w >= 15u * 65536u && (int64_t)c.l6_e2 * 256 <= 625ll * c.l6_w; }
static void testLearnL6Relearn() {
  static LearnState s;
  learnInit(&s);
  const uint32_t sep = 1788220800u;
  const auto night = [](uint32_t day, uint8_t epoch) {
    const NightGeo g = nightGeo(day);
    NightP p;
    p.spread = (float)(.5 + 11.5 * lrand()), p.te = 12, p.epoch = epoch;
    p.drop = (float)(2.0 + .5 * p.spread + .3 * g.lq / 10.0 + .3 * lgauss());
    return p;
  };
  for (int d = 0; d < 60; d++) feedNight(s, sep + 86400u * (uint32_t)d, night(sep + 86400u * (uint32_t)d, 0));
  CHECK(l6Ready(s.c) && s.c.l6_w > 15u * 65536u);
  // the sums scaled to 5 nights (below the bar of 15): not ready in the re-learning window, ready again once
  // enough new nights are in
  int inWindow = 0, readyInWindow = 0, admitted = 0, readyAt = -1, d = 60;
  for (; d < 120 && readyAt < 0; d++) {
    const uint32_t day = sep + 86400u * (uint32_t)d;
    feedNight(s, day, night(day, 1));
    const uint16_t t14 = lday14(nightGeo(day).end), age = (uint16_t)((s.c.relearn_until - t14) & 0x3FFF);
    if (s.last_night.l6 != L6R_OK) continue;
    admitted++;
    if (age <= 14 && age != 0) inWindow++, readyInWindow += l6Ready(s.c);
    if (l6Ready(s.c)) readyAt = admitted;
  }
  printf("  info: L6 after an epoch change: %d admitted nights in the re-learning window, %d ready; ready again after %d\n",
         inWindow, readyInWindow, readyAt);
  CHECK(s.c.epoch == 1 && inWindow >= 12 && readyInWindow == 0 && readyAt > 14 && readyAt <= 25);
}

// a constant worst-case feed through the real path (spread 30 °C, drop 30 °C: s 250, y 300, the products at
// their maximum 75 000) settles at the analytic margin: s.y stationary ~1.117e9 < 2^31 - 1, positive (no wrap)
static void testLearnL6Worst() {
  static LearnState s;
  learnInit(&s);
  const uint32_t d0 = 1767225600u;
  int64_t most = 0;
  for (int d = 0; d < 2000; d++) {
    NightP p;
    p.te = 40, p.rate = 2, p.spread = 30, p.drop = 30;
    feedNight(s, d0 + 86400u * (uint32_t)d, p);
    const int32_t S[8] = {s.c.l6_s, s.c.l6_l, s.c.l6_y, s.c.l6_ss, s.c.l6_sl, s.c.l6_ll, s.c.l6_sy, s.c.l6_ly};
    for (int32_t v : S) {
      const int64_t m = v < 0 ? -(int64_t)v : v;
      if (m > most) most = m;
    }
  }
  printf("  info: L6 worst case (s 250, y 300, 2000 nights): s.y %ld, s.s %ld, y %ld, largest |sum| %lld, W %.1f, E2 %lu\n",
         (long)s.c.l6_sy, (long)s.c.l6_ss, (long)s.c.l6_y, (long long)most, s.c.l6_w / 65536.0, (unsigned long)s.c.l6_e2);
  CHECK(s.c.l6_sy > 1100000000 && s.c.l6_sy <= 1120000000 && s.c.l6_ss > 900000000 && s.c.l6_y > 0 && most <= 1120000000ll &&
        s.c.l6_w <= 59u * 65536u);
}

static void testLearnL6Arith() {
  // golden vector: fixed sums -> exact quantised coefficients, the same on MSVC (run.ps1, forecast_replay) and GCC
  // (run.sh, CI)
  LearnCore g;
  memset(&g, 0, sizeof g);
  // the sums of the synthetic season (testLearnL6, 200 nights); unrounded 266.1758 / 4595.5824 / 2736.3773 (IEEE double,
  // the same operation order evaluated independently): every value >= 0.17 from a rounding edge
  g.l6_w = 3695206u, g.l6_s = 201808, g.l6_l = 303313, g.l6_y = 1365597;
  g.l6_ss = 17837236, g.l6_sl = 4056818, g.l6_ll = 9412513, g.l6_sy = 26011399, g.l6_ly = 29480368;
  CHECK(!l6Solve(g));
  printf("  info: L6 golden vector: a %d b %d c %d\n", g.l6_a, g.l6_b, g.l6_c);
  CHECK(g.l6_a == 266 && g.l6_b == 4596 && g.l6_c == 2736);
  // 10 years of random nights at the extremes: no sum near the int32 range, every value within its bounds
  static LearnState s;
  learnInit(&s);
  const uint32_t d0 = 1767225600u;  // 2026-01-01
  int64_t most = 0;
  bool in = true;
  for (int d = 0; d < 3650; d++) {
    NightP p;
    p.te = (float)(-20 + 60 * lrand()), p.spread = (float)(30 * lrand()), p.drop = (float)(-5 + 35 * lrand());
    p.rate = (float)(-4 + 12 * lrand()), p.sky = lrand() < .5 ? (float)(.1 + .6 * lrand()) : 0;
    if (p.drop < p.rate) p.drop = p.rate;  // the night keeps falling after the evening
    confidentPm(s, d0 + 86400u * (uint32_t)d);
    feedNight(s, d0 + 86400u * (uint32_t)d, p);
    const int32_t S[8] = {s.c.l6_s, s.c.l6_l, s.c.l6_y, s.c.l6_ss, s.c.l6_sl, s.c.l6_ll, s.c.l6_sy, s.c.l6_ly};
    for (int32_t v : S) {
      const int64_t m = v < 0 ? -(int64_t)v : v;
      if (m > most) most = m;
    }
    L5Src src;
    bool hit;
    const int thr = l5Threshold(s.c, &src, &hit);
    in = in && s.c.l6_a >= -200 && s.c.l6_a <= 600 && s.c.l6_b >= 0 && s.c.l6_b <= 10000 && s.c.l6_c >= 0 &&
         s.c.l6_c <= 6000 && thr >= 50 && thr <= 250 && s.c.l5_ce >= -200 && s.c.l5_ce <= 500 && s.c.l5_oe >= -200 &&
         s.c.l5_oe <= 500 && s.c.l6_w <= 59u * 65536u;
  }
  printf("  info: L6 10 years at the extremes: largest |sum| %lld (limit %d), W %.1f nights, %u nights rejected\n",
         (long long)most, INT32_MAX, s.c.l6_w / 65536.0, (unsigned)GV(s.gates[LG_L6_NIGHT]));
  CHECK(in && most <= 1150000000ll && s.c.l6_w > 0);
}

static void testLearnBlobMigrate() {
  // a version 1 core reads Older -> priors; the full replay of the rings equals an uninterrupted live run, L3 aside
  // (live only); L5 / L6 included
  static LearnState live, ring;
  learnInit(&live);
  learnInit(&ring);
  LearnCore v1 = live.c;
  v1.version = 1, v1.crc = 0, v1.crc = crc32(&v1, sizeof v1);
  CHECK(learnCheckCore(v1, sizeof v1) == BlobCheck::Older);
  const uint32_t d0 = 1791763200u;  // 2026-10-12
  static NightP ps[40];
  for (int d = 0; d < 40; d++) {
    NightP& p = ps[d];
    p.sky = d % 3 ? .65f : .15f, p.te = 12, p.spread = (float)(1 + 8 * lrand()), p.drop = (float)(3 + 6 * lrand());
    p.rate = d % 3 ? 1.8f : .3f;
  }
  for (int d = 0; d < 40; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    LearnCtx lc = lctx(true);
    lc.wind_n = 3, lc.wind[0] = 20, lc.wind[1] = 40, lc.wind[2] = 60;
    if (d % 7 == 0) confidentPm(live, day), confidentPm(ring, day);
    feedNight(live, day, ps[d], lc);
    feedNight(ring, day, ps[d], lctx());
  }
  CHECK(live.c.l5_cn > 0 && live.c.l5_on > 0 && live.c.l6_w > 0);
  LearnCore a = live.c, b = ring.c;
  a.l3_e = b.l3_e = 0, a.l3_n = b.l3_n = 0, a.l3_day = b.l3_day = 0;
  CHECK(memcmp(&a, &b, sizeof a) == 0);
  // catch-up == uninterrupted with L5 / L6 moving: the blob of night 17 + the rings from there
  static LearnState full, part;
  learnInit(&full);
  learnInit(&part);
  LearnCore saved = {};
  for (int d = 0; d < 40; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    if (d % 7 == 0) confidentPm(full, day), confidentPm(part, day);
    feedNight(full, day, ps[d]);
    if (d <= 17) feedNight(part, day, ps[d]);
    if (d == 17) saved = part.c;
  }
  learnSealCore(&saved);
  CHECK(learnCheckCore(saved, sizeof saved) == BlobCheck::Ok);
  learnInit(&part);
  part.c = saved;
  memcpy(part.mask, full.mask, sizeof part.mask);  // the mask blob is kept beside the core
  for (int d = 0; d < 40; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    if (d % 7 == 0 && d > 17) confidentPm(part, day);
    feedNight(part, day, ps[d]);  // the hours up to last_hour are skipped
  }
  full.c.crc = part.c.crc = 0;
  CHECK(memcmp(&full.c, &part.c, sizeof full.c) == 0);
  // resets: location -> L5 + L6 priors; profile -> all; epoch -> L5 n capped at 6, L6 sums scaled to 15 nights
  LearnState& r = full;
  LearnCtx c = lctx();
  const uint32_t next = d0 + 40 * 86400u;
  CHECK(r.c.l5_cn > 6 * 64 && r.c.l6_w > 15u * 65536u);
  const LearnCore before = r.c;
  HourRec ep = lhour(next + 13 * 3600);
  ep.epoch = 1;
  CHECK(learnHour(r, ep, c) & LE_RELEARN);
  CHECK(r.c.l5_cn == 0 && r.c.l5_ce == before.l5_ce && r.c.l6_w == 5u * 65536u && r.c.l6_a == before.l6_a &&
        std::abs((double)r.c.l6_ss / before.l6_ss - 5.0 * 65536 / before.l6_w) < 1e-3);  // n 0, 5 nights
  c.lon = 21;
  HourRec lo = lhour(next + 14 * 3600);
  lo.epoch = 1;
  CHECK((learnHour(r, lo, c) & LE_LOC) && r.c.l5_cn == 0 && r.c.l5_ce == 150 && r.c.l6_w == 0 && r.c.l6_a == 100 &&
        r.c.l6_c == 2000 && r.c.lbl == LBL_UNSET);
  r.c.l5_cn = 500, r.c.l6_w = 999999, r.c.l6_b = 7000;
  c.profile = 0;
  HourRec pr = lhour(next + 15 * 3600);
  pr.epoch = 1;
  CHECK((learnHour(r, pr, c) & LE_PROFILE) && r.c.l5_cn == 0 && r.c.l6_w == 0 && r.c.l6_b == 4000);
}

// every Out::printf of /api/learn stays under 192 chars: the body at its widest values parses as JSON
static void testLearnJsonWidest() {
  static LearnState s;
  learnInit(&s);
  LearnCore& c = s.c;
  c.epoch = 255, c.last_hour = 4294967295u, c.last_day = 4294967295u, c.relearn_until = 1;
  for (auto& x : c.l1) x = LearnCell{65535, 65535, 0x3FFF};
#if FC_DIAG_BENCH
  for (auto& g : s.gates) g = 65535;
#endif
  c.l2_wet_e = c.l2_night_e = 10000, c.l2_wet_n = c.l2_night_n = 65535;
  c.l3_e = 50000, c.l3_n = 65535, c.l4_p50 = c.l4_p90 = 50000, c.l4_n = 65535, c.l7_e = -2147483647, c.l7_n = 65535;
  c.l5_ce = c.l5_oe = -200, c.l5_cn = c.l5_on = 65535;
  c.l6_a = -200, c.l6_b = 10000, c.l6_c = 6000, c.l6_w = 4294967295u, c.l6_e2 = c.l6_ea = 4294967295u;
  c.n_start = 4294967295u, c.n_len = c.n_rhmax = c.n_hours = c.n_ok = c.n_calm = c.n_L = c.n_w5 = 254, c.n_flags = 255;
  c.n_t2 = c.n_td2 = c.n_t0 = c.n_dt3 = c.n_tmin = c.e_dt3 = -32767, c.e_flags = 0;
  c.lbl = LBL_OVERCAST, c.lbl_day = 0x3FFF;
  s.last_night = LearnNight{4294967295u, 254, 254, 254, 254, 255, 254, -32767, -32767, -32767, -32767, -32767, true, 254, 254, 7, -32767};
  s.hit = 255, s.l1_hit = 0xFFFF;
  static LearnView v;
  learnView(s, &v);
  LearnMeta m = {"read_only", "blob_newer", 4294967295u, true, 4294967295u, 4294967295u, 4294967295u, 4294967295u,
                 4294967295u, 52, 20, 4294967295u, 4294967295u, 255, LM_ALL, 4294967295u, 4294967295u};
  std::string js;
  char buf[96];
  Out o(toString, &js, buf, sizeof buf);
  learnJson(o, v, m);
  o.flush();
  JsonReader r(js.data(), js.size());
  const bool parsed = r.skipValue() && !r.failed() && r.atEnd();
  if (!parsed) printf("  /api/learn widest body does not parse:\n%s\n", js.c_str());
#if FC_DIAG_BENCH
  CHECK(parsed && contains(js, "\"free_entries\":4294967295},\"gates\":{") && contains(js, "\"evening\":{\"label\":\"overcast\""));
  // the migration keys at their widest (every learner named) render whole
  CHECK(contains(js, "\"migrated_from\":255,\"backfill\":{") &&
        contains(js, "\"replay\":{\"hours\":4294967295,\"ms\":4294967295,\"learners\":[\"l1\",\"l1b\",\"l2\",\"l3\",\"l4\",\"l5\",\"l6\",\"l7\"]}}"));
#else  // the release body at its widest, no bench key
  CHECK(parsed && contains(js, "\"free_entries\":4294967295},\"l1\":{") && contains(js, "\"evening\":{\"label\":\"overcast\"") &&
        !contains(js, "\"gates\"") && !contains(js, "\"replay\""));
#endif
  printf("  info: /api/learn body at its widest %u B\n", (unsigned)js.size());
}

// ---------------------------------------------------------------- blob migration (versions migrate, never reset)
// a version 1 core: the V1 fields only, sealed
static LearnCoreV1 packV1(const LearnCore& c) {
  LearnCoreV1 v;
  memset(&v, 0, sizeof v);
#define MIG_PACK(f) memcpy(&v.f, &c.f, sizeof v.f);
  LEARN_V1_SHARED(MIG_PACK)
#undef MIG_PACK
  v.magic[0] = 'L', v.magic[1] = 'R', v.version = 1;
  v.crc = 0;
  v.crc = crc32(&v, sizeof v);
  return v;
}
static BlobCheck checkV1(const LearnCoreV1& v) {
  LearnCore c;
  memcpy(&c, &v, sizeof c);
  return learnCheckCore(c, sizeof c);
}
static bool sameL5L6(const LearnCore& a, const LearnCore& b) {
  bool same = true;
#define MIG_EQ(f) same = same && memcmp(&a.f, &b.f, sizeof a.f) == 0;
  LEARN_L5_FIELDS(MIG_EQ)
  LEARN_L6_FIELDS(MIG_EQ)
  return same;
}
static bool sameOld(const LearnCore& a, const LearnCore& b) {  // every learner a V1 core had (state and stamps)
  return memcmp(a.l1, b.l1, sizeof a.l1) == 0 && a.l2_wet_e == b.l2_wet_e && a.l2_wet_n == b.l2_wet_n &&
         a.l2_wet_day == b.l2_wet_day && a.l2_night_e == b.l2_night_e && a.l2_night_n == b.l2_night_n &&
         a.l2_night_day == b.l2_night_day && a.l3_e == b.l3_e && a.l3_n == b.l3_n && a.l3_day == b.l3_day &&
         a.l4_p50 == b.l4_p50 && a.l4_p90 == b.l4_p90 && a.l4_n == b.l4_n && a.l4_day == b.l4_day && a.l7_e == b.l7_e &&
         a.l7_n == b.l7_n && a.l7_flag == b.l7_flag;
#undef MIG_EQ
}
// the 40-night fixture of testLearnBlobMigrate, deterministic (no lrand), hours recorded for the ring
static void migNights(NightP ps[40]) {  // one fixture; steady labels come from confidentPm
  for (int d = 0; d < 40; d++) {
    NightP& p = ps[d];
    p = NightP();
    p.sky = d % 3 ? .65f : .15f, p.te = 12;
    p.spread = 1.0f + (float)(d * 37 % 80) / 10.0f, p.drop = 3.0f + (float)(d * 53 % 60) / 10.0f;
    p.rate = d % 3 ? 1.8f : .3f;
  }
}
static std::vector<HourRec> gMigRing;  // what feedNight's hours would be in the flash ring
static void migRecord(uint32_t d0, int nights, const NightP* ps) {
  gMigRing.clear();
  static LearnState sink;
  learnInit(&sink);
  for (int d = 0; d < nights; d++) feedNight(sink, d0 + 86400u * (uint32_t)d, ps[d], lctx(), LM_ALL, &gMigRing);
}
// one pass as app_learn.cpp runs it: hours <= end, the mask, then the merge
static void migPass(LearnState* pass, LearnState& dst, uint16_t mask, uint32_t end, uint32_t upto) {
  learnPassStart(pass, dst.c, mask, dst.mask);  // the live state's mask, as on the board
  for (const HourRec& h : gMigRing)
    if (h.hour <= upto && h.hour <= end) learnHour(*pass, h, lctx(), mask);
  learnPassMerge(dst.c, pass->c, mask, end);
}

static void testLearnMigLayout() {
  // a hand-built V1 blob (every byte from an LCG) -> Older -> migrated field by field
  LearnCoreV1 v;
  uint32_t x = 12345;
  uint8_t* b = (uint8_t*)&v;
  for (size_t i = 0; i < sizeof v; i++) x = x * 1103515245u + 12345u, b[i] = (uint8_t)(x >> 16);
  v.magic[0] = 'L', v.magic[1] = 'R', v.version = 1;
  if (!v.last_hour) v.last_hour = 1794268800u;
  v.crc = 0;
  v.crc = crc32(&v, sizeof v);
  CHECK(checkV1(v) == BlobCheck::Older);
  LearnCore m;
  uint8_t from = 0;
  uint16_t rep = 0;
  CHECK(learnMigrateCore(&v, sizeof v, &m, &from, &rep) && from == 1 && rep == (LM_SINCE_V1 | LM_SINCE_V2) &&
        m.version == LEARN_VERSION && m.l7_pprev == I16_NONE && m.relearn_hour == 0);
  // L7's flag and count are re-typed in V3: cleared / capped below stage 1, the value kept
#define MIG_FIELD(f) \
  if (strcmp(#f, "l7_flag") && strcmp(#f, "l7_n")) CHECK(memcmp(&m.f, &v.f, sizeof m.f) == 0);
  LEARN_V1_SHARED(MIG_FIELD)
#undef MIG_FIELD
  CHECK(m.l7_flag == 0 && m.l7_n == (v.l7_n < 11 * 64 ? v.l7_n : 11 * 64) && m.l7_e == v.l7_e);
  static LearnState fresh;
  learnInit(&fresh);
  CHECK(sameL5L6(m, fresh.c));
  bool scratch = true;
#define MIG_SC(f) scratch = scratch && memcmp(&m.f, &fresh.c.f, sizeof m.f) == 0;
  LEARN_V2_SCRATCH(MIG_SC)
#undef MIG_SC
  static const uint8_t z[8] = {};
  CHECK(scratch && m.l7_step == 0 && memcmp(m.reserve3, z, sizeof m.reserve3) == 0 && memcmp(m.pad, z, sizeof m.pad) == 0);
  learnSealCore(&m);
  CHECK(learnCheckCore(m, sizeof m) == BlobCheck::Ok);
  // never fed: no pass (the full backfill feeds every learner)
  LearnCoreV1 nf = v;
  nf.last_hour = 0;
  nf.crc = 0;
  nf.crc = crc32(&nf, sizeof nf);
  CHECK(learnMigrateCore(&nf, sizeof nf, &m, &from, &rep) && rep == 0);
  // version 0 (valid CRC): Older but unknown -> not migrated; a flipped CRC -> Bad; v3 -> Newer
  LearnCoreV1 v0 = v;
  v0.version = 0;
  v0.crc = 0;
  v0.crc = crc32(&v0, sizeof v0);
  CHECK(checkV1(v0) == BlobCheck::Older && !learnMigrateCore(&v0, sizeof v0, &m, &from, &rep));
  LearnCoreV1 bad = v;
  bad.crc ^= 1;
  CHECK(checkV1(bad) == BlobCheck::Bad);
  LearnCoreV1 v3 = v;
  v3.version = 4;
  v3.crc = 0;
  v3.crc = crc32(&v3, sizeof v3);
  CHECK(checkV1(v3) == BlobCheck::Newer && !learnMigrateCore(&v3, 255, &m, &from, &rep));
}

static void testLearnMigMask() {
  // a feed with LM_SINCE_V1 moves no old learner (stamps included); one without them moves no L5 / L6 field
  static NightP ps[40];
  migNights(ps);
  const uint32_t d0 = 1791763200u;
  static LearnState a, b;
  learnInit(&a);
  for (int d = 0; d < 20; d++) {
    if (d % 7 == 0) confidentPm(a, d0 + 86400u * (uint32_t)d);
    feedNight(a, d0 + 86400u * (uint32_t)d, ps[d]);
  }
  b = a;
  const LearnCore start = a.c;
  uint8_t mask0[sizeof a.mask];
  memcpy(mask0, a.mask, sizeof mask0);
  for (int d = 20; d < 40; d++) {
    feedNight(a, d0 + 86400u * (uint32_t)d, ps[d], lctx(true), LM_SINCE_V1);
    feedNight(b, d0 + 86400u * (uint32_t)d, ps[d], lctx(true), LM_ALL & ~LM_SINCE_V1);
  }
  CHECK(sameOld(a.c, start) && memcmp(a.mask, mask0, sizeof mask0) == 0 && !sameL5L6(a.c, start));
  CHECK(sameL5L6(b.c, start) && !sameOld(b.c, start));
}

static void testLearnMigRuns() {
  const uint32_t d0 = 1791763200u;
  static NightP ps[40];
  migNights(ps);
  migRecord(d0, 40, ps);
  // run A (every learner, nights 0..39) vs B (the V1 learners only, nights 0..25), migrated, the pass, the catch-up
  // 26..39 -> M == A
  static LearnState A, B, P;
  learnInit(&A);
  learnInit(&B);
  confidentPm(A, d0), confidentPm(B, d0);  // L1 confident before night 0 only: steady labels
  for (int d = 0; d < 40; d++) feedNight(A, d0 + 86400u * (uint32_t)d, ps[d]);
  for (int d = 0; d <= 25; d++) feedNight(B, d0 + 86400u * (uint32_t)d, ps[d], lctx(), LM_ALL & ~LM_SINCE_V1);
  const LearnCoreV1 v1 = packV1(B.c);
  CHECK(checkV1(v1) == BlobCheck::Older);
  static LearnState M;
  learnInit(&M);
  uint8_t from = 0;
  uint16_t rep = 0;
  CHECK(learnMigrateCore(&v1, sizeof v1, &M.c, &from, &rep) && rep == (LM_SINCE_V1 | LM_SINCE_V2));
  rep = LM_SINCE_V1;  // the L5 / L6 part of the V1 pass (the L1 / L2 / L4 re-derive: testLearnV3Migrate)
  const uint32_t end = M.c.last_hour;
  memcpy(M.mask, B.mask, sizeof M.mask);  // the mask blob kept beside the core
  migPass(&P, M, rep, end, end);
  // the pass == a fresh state with the old learners copied in, fed the same hours with the mask alone
  static LearnState F;
  learnInit(&F);
  memcpy(F.c.l1, M.c.l1, sizeof F.c.l1);
  memcpy(F.mask, M.mask, sizeof F.mask);
  F.c.loc_sig = M.c.loc_sig, F.c.profile = M.c.profile, F.c.mac_hash = M.c.mac_hash, F.c.flags = M.c.flags;
  for (const HourRec& h : gMigRing)
    if (h.hour <= end) learnHour(F, h, lctx(), LM_SINCE_V1);
  CHECK(sameL5L6(F.c, M.c) && F.c.l5_cn > 0 && F.c.l6_w > 0);
  for (int d = 26; d < 40; d++) feedNight(M, d0 + 86400u * (uint32_t)d, ps[d]);
  LearnCore a = A.c, m = M.c;
  a.crc = m.crc = 0;
  CHECK(memcmp(&a, &m, sizeof a) == 0);
  // variant: confidentPm every 7 days (the L1 cells move): old learners equal, L5 / L6 differences reported
  static LearnState A2, B2, M2, P2;
  learnInit(&A2);
  learnInit(&B2);
  for (int d = 0; d < 40; d++) {
    if (d % 7 == 0) confidentPm(A2, d0 + 86400u * (uint32_t)d);
    feedNight(A2, d0 + 86400u * (uint32_t)d, ps[d]);
  }
  for (int d = 0; d <= 25; d++) {
    if (d % 7 == 0) confidentPm(B2, d0 + 86400u * (uint32_t)d);
    feedNight(B2, d0 + 86400u * (uint32_t)d, ps[d], lctx(), LM_ALL & ~LM_SINCE_V1);
  }
  const LearnCoreV1 w1 = packV1(B2.c);
  learnInit(&M2);
  learnMigrateCore(&w1, sizeof w1, &M2.c, &from, &rep);
  rep = LM_SINCE_V1;
  memcpy(M2.mask, B2.mask, sizeof M2.mask);
  migPass(&P2, M2, rep, M2.c.last_hour, M2.c.last_hour);
  for (int d = 26; d < 40; d++) {
    if (d % 7 == 0) confidentPm(M2, d0 + 86400u * (uint32_t)d);
    feedNight(M2, d0 + 86400u * (uint32_t)d, ps[d]);
  }
  printf("  info: migration variant (L1 moving): L5 clear %d/%u vs %d/%u, overcast %d/%u vs %d/%u; L6 a/b/c %d/%d/%d vs %d/%d/%d "
         "(migrated vs old history)\n", M2.c.l5_ce, M2.c.l5_cn, A2.c.l5_ce, A2.c.l5_cn, M2.c.l5_oe, M2.c.l5_on, A2.c.l5_oe,
         A2.c.l5_on, M2.c.l6_a, M2.c.l6_b, M2.c.l6_c, A2.c.l6_a, A2.c.l6_b, A2.c.l6_c);
  CHECK(sameOld(M2.c, A2.c) && M2.c.l6_a >= -200 && M2.c.l6_a <= 600 && M2.c.l5_ce >= -200 && M2.c.l5_ce <= 500);
  // a restart mid-pass (half a pass discarded, the V1 blob still in NVS) migrates again: the same result
  static LearnState H, Q;
  learnInit(&H);
  learnMigrateCore(&v1, sizeof v1, &H.c, &from, &rep);
  rep = LM_SINCE_V1;
  memcpy(H.mask, B.mask, sizeof H.mask);
  learnPassStart(&Q, H.c, rep, H.mask);
  for (size_t i = 0; i < gMigRing.size() / 3; i++) learnHour(Q, gMigRing[i], lctx(), rep);  // dropped: no merge, no write
  learnInit(&H);
  learnMigrateCore(&v1, sizeof v1, &H.c, &from, &rep);
  rep = LM_SINCE_V1;
  memcpy(H.mask, B.mask, sizeof H.mask);
  migPass(&Q, H, rep, H.c.last_hour, H.c.last_hour);
  for (int d = 26; d < 40; d++) feedNight(H, d0 + 86400u * (uint32_t)d, ps[d]);
  LearnCore h = H.c;
  h.crc = 0;
  CHECK(memcmp(&h, &a, sizeof h) == 0);
}

// in the pass a confident L1 cell whose day lies after the replayed hour labels clear, not nocell
static void testLearnMigLabel() {
  const uint32_t d0 = 1791763200u;
  static LearnState s, t;
  learnInit(&s);
  learnInit(&t);
  NightP p;
  p.sky = .65f;
  confidentPm(s, d0 + 20 * 86400u), confidentPm(t, d0 + 20 * 86400u);  // the converged cell: 20 days later
  feedNight(s, d0, p, lctx(), LM_SINCE_V1);
  feedNight(t, d0, p, lctx(), LM_ALL);
  CHECK(s.c.lbl == LBL_CLEAR && t.c.lbl == LBL_NOCELL);
}

// migration at E0, at E0 + 2 and mid-night: the scratch carried, that night equals run A; a pass ending before
// `end` marks the evening missing: the night is rejected `evening`, the old learners do not move
static void testLearnMigEdges() {
  const uint32_t d0 = 1791763200u;
  static NightP ps[12];
  for (int d = 0; d < 12; d++) {
    ps[d] = NightP();
    ps[d].sky = .65f, ps[d].te = 12, ps[d].rate = 1.8f, ps[d].spread = 3, ps[d].drop = 5;
  }
  migRecord(d0, 12, ps);
  static LearnState A;
  learnInit(&A);
  confidentPm(A, d0);
  for (const HourRec& h : gMigRing) learnHour(A, h, lctx());
  const NightGeo g = nightGeo(d0 + 8 * 86400u);
  const uint32_t cuts[3] = {g.e0, g.e0 + 2 * 3600, g.start + 5 * 3600};
  int carried = 0;
  for (uint32_t cut : cuts) {
    static LearnState B, M, P;
    learnInit(&B);
    confidentPm(B, d0);
    for (const HourRec& h : gMigRing)
      if (h.hour <= cut) learnHour(B, h, lctx(), LM_ALL & ~LM_SINCE_V1);
    const LearnCoreV1 v1 = packV1(B.c);
    learnInit(&M);
    uint8_t from;
    uint16_t rep;
    learnMigrateCore(&v1, sizeof v1, &M.c, &from, &rep);
    rep = LM_SINCE_V1;
    memcpy(M.mask, B.mask, sizeof M.mask);
    migPass(&P, M, rep, M.c.last_hour, M.c.last_hour);
    for (const HourRec& h : gMigRing)
      if (h.hour > cut) learnHour(M, h, lctx());
    LearnCore a = A.c, m = M.c;
    a.crc = m.crc = 0;
    carried += memcmp(&a, &m, sizeof a) == 0;
  }
  CHECK(carried == 3);
  // the pass withheld the last hour (cut mid-night): EB_MISS, the night rejected `evening`, old learners as run A
  static LearnState B, M, P;
  learnInit(&B);
  confidentPm(B, d0);
  const uint32_t cut = cuts[2];
  for (const HourRec& h : gMigRing)
    if (h.hour <= cut) learnHour(B, h, lctx(), LM_ALL & ~LM_SINCE_V1);
  const LearnCoreV1 v1 = packV1(B.c);
  learnInit(&M);
  uint8_t from;
  uint16_t rep;
  learnMigrateCore(&v1, sizeof v1, &M.c, &from, &rep);
  rep = LM_SINCE_V1;
  memcpy(M.mask, B.mask, sizeof M.mask);
  migPass(&P, M, rep, M.c.last_hour, M.c.last_hour - 3600);
  CHECK(M.c.e_flags == EB_MISS);
  [[maybe_unused]] const uint32_t eve0 = GV(M.gates[LG_L5_EVENING]);
  uint8_t l6 = 255;
  for (const HourRec& h : gMigRing)
    if (h.hour > cut) {
      learnHour(M, h, lctx());
      if (h.hour >= g.end && l6 == 255) l6 = M.last_night.l6;
    }
  CHECK(l6 == L6R_EVENING && GT(M.gates[LG_L5_EVENING] == eve0 + 1) && sameOld(M.c, A.c));
}

// one masked feed through context() (location, profile, console MAC), relearn() (a new epoch) and
// learnDay (L7): the masked-out learners keep every byte; the masked-in ones reset as they would with LM_ALL
static void testLearnMigEvents() {
  const uint32_t d0 = 1791763200u;
  static NightP ps[40];
  migNights(ps);
  static LearnState base;
  learnInit(&base);
  confidentPm(base, d0);
  LearnCtx c = lctx();
  c.mac_hash = 7;
  for (int d = 0; d < 20; d++) feedNight(base, d0 + 86400u * (uint32_t)d, ps[d], c);
  for (int d = 0; d < 70; d++) {  // L7 learned
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(20500 + d));
    dr.hours = 24, dr.p_avg = 10130, dr.quality = 0, dr.epoch = 0;
    learnDay(base, dr, c);
  }
  CHECK(base.c.l7_n > 0 && base.c.l5_cn + base.c.l5_on > 0 && base.c.l4_n > 0);
  base.mask[3] = 0x62;
  for (int which = 0; which < 2; which++) {
    const uint16_t mask = which ? (uint16_t)(LM_ALL & ~LM_SINCE_V1) : LM_SINCE_V1;
    static LearnState s;
    s = base;
    LearnCtx e = c;
    e.lon = 21, e.profile = 2, e.mac_hash = 8;  // a move, another profile, another console
    HourRec h = lhour(d0 + 25 * 86400u);
    h.epoch = 1;  // a new sensor epoch
    const uint8_t ev = learnHour(s, h, e, mask);
    DayRec dr;
    clearDay(&dr, dayNumberToYmd(20600));
    dr.hours = 24, dr.p_avg = 10150, dr.quality = 0, dr.epoch = 1;
    learnDay(s, dr, e, mask);
    const bool events = (ev & LE_LOC) && (ev & LE_PROFILE) && (ev & LE_MAC) && (ev & LE_RELEARN);
    if (!which)  // l5 / l6 only: every old learner and the mask untouched; L5 / L6 at their priors
      CHECK(events && sameOld(s.c, base.c) && s.mask[3] == 0x62 && s.c.l5_cn == 0 && s.c.l6_w == 0);
    else  // the old learners only: L5 / L6 untouched; the old ones reset (then fed this one hour / day), the mask cleared
      CHECK(events && sameL5L6(s.c, base.c) && s.mask[3] == 0 && s.c.l7_n < base.c.l7_n && s.c.l4_n < base.c.l4_n &&
            s.c.l1[2].n == 0);
  }
}

static void testLearnMig() {
  testLearnMigEvents();
  testLearnMigLayout();
  testLearnMigMask();
  testLearnMigRuns();
  testLearnMigLabel();
  testLearnMigEdges();
}

// LM_ALL neutrality -- a deterministic 40-night fixture (no lrand: independent of the test order),
// live (bins, wind) with confident PM cells every 7 days; the final core, mask and gates as one CRC. A feed with an
// explicit LM_ALL equals a feed with no mask argument.
static uint32_t a4Run(bool explicitMask) {
  static LearnState s;
  learnInit(&s);
  const uint32_t d0 = 1791763200u;  // 2026-10-12
  for (int d = 0; d < 40; d++) {
    const uint32_t day = d0 + 86400u * (uint32_t)d;
    NightP p;
    p.sky = d % 3 ? .65f : .15f, p.te = 12, p.spread = 1.0f + (float)(d * 37 % 80) / 10.0f;
    p.drop = 3.0f + (float)(d * 53 % 60) / 10.0f, p.rate = d % 3 ? 1.8f : .3f;
    LearnCtx lc = lctx(true);
    lc.wind_n = 3, lc.wind[0] = 20, lc.wind[1] = 40, lc.wind[2] = (uint8_t)(50 + d % 30);
    if (d % 7 == 0) confidentPm(s, day);
    if (explicitMask) feedNight(s, day, p, lc, LM_ALL);
    else feedNight(s, day, p, lc);
  }
  LearnCore c = s.c;
  c.crc = 0;
  uint32_t h = crc32(&c, sizeof c);
  h = crc32(s.mask, sizeof s.mask, h);
#if FC_DIAG_BENCH
  h = crc32(s.gates, sizeof s.gates, h);
#endif
  return crc32(&s.hours_fed, sizeof s.hours_fed, h);
}
static void testLearnMaskAll() {
  const uint32_t a = a4Run(false), b = a4Run(true);
  printf("  info: LM_ALL neutrality: %08lx / %08lx (golden MSVC: f174f4d3 with gates[] (bench), d05d6347 without (release))\n", (unsigned long)a, (unsigned long)b);
  CHECK(b == a);
#ifdef _MSC_VER
  // The golden is a CRC over the whole core, the mask and gates[] (bench builds: the release has no gates[]), so it moves with any layout change (a new core field
  // such as l7_pprev, updated every hour; a new gate such as LG_MELT) and with any learner rule change (L1's warm-up and
  // ramp, the L2 seeds, the L5 label by the L1b class, confidentPm's sky marks, n back to 0 on a relearn, L7's rules,
  // tolerance and seeding): re-record it with such a change. The guard proper is explicit == implicit LM_ALL (above).
  CHECK(a == (FC_DIAG_BENCH ? 0xf174f4d3u : 0xd05d6347u));
#else
  // the fixture goes through libm trig, which differs between compilers: only the MSVC golden is pinned, g++ prints its own
  printf("  info: LM_ALL golden for g++ not yet recorded: %08lx\n", (unsigned long)a);
#endif
}

// L1's steadiness gate at 1.25 x the clear sky's max/avg. Two steady winter hours (mean 167 / 188 W/m2, max 200 / 225:
// max/avg 1.20 against the model's 1.02-1.09) would be refused at 1.1, so L1 would never start in winter; at 1.25 both
// count in the 15-20 deg AM cell and E moves up. A broken-cloud hour (max/avg 1.6) is still refused. The two hours are
// 09Z / 10Z at 52 N
static void testLearnSteadyWinter() {
  static LearnState s;
  learnInit(&s);
  const uint32_t day = 1794700800u;  // 2026-11-15 00:00Z
  LearnCtx c = lctx();
  c.lon = 17;  // the hours' sun stays AM (solar noon after 10:30Z)
  struct H { uint32_t hr; uint16_t avg, max; };
  const H hs[2] = {{day + 9 * 3600u, 167, 200}, {day + 10 * 3600u, 188, 225}};
  const int ci = 2 * 2 + 0;  // band 15-20 deg, AM
  for (const H& h : hs) {
    double se = 0, sc = 0, mx = 0;
    for (int i = 0; i < LEARN_BINS; i++) {
      double el, az;
      sunPos(h.hr + 150 + 300u * (uint32_t)i, 52, 17, &el, &az);
      se += el, sc += clearSky(el), mx = std::max(mx, clearSky(el));
    }
    const double rc = mx / (sc / LEARN_BINS), rm = (double)h.max / h.avg;
    printf("  info: steady winter hour %02luZ: el %.1f, cs_avg %.0f, cs max/avg %.3f, max/avg %.3f\n",
           (unsigned long)((h.hr - day) / 3600), se / LEARN_BINS, sc / LEARN_BINS, rc, rm);
    CHECK(rm > rc * 1.1 && rm <= rc * 1.25);  // refused at 1.1, admitted at 1.25 (1.30 for an empty cell)
  }
  const int e0 = s.c.l1[ci].e;
  const uint32_t steady0 = GV(s.gates[LG_STEADY]);
  for (const H& h : hs) {
    HourRec r = lhour(h.hr, 2);
    r.solar_avg = h.avg, r.solar_max = h.max;
    learnHour(s, r, c);
  }
  printf("  info: steady winter: cell %d e %d -> %d, n %u, steady gate %lu\n", ci, e0, s.c.l1[ci].e, (unsigned)s.c.l1[ci].n,
         (unsigned long)(GV(s.gates[LG_STEADY]) - steady0));
  CHECK(GT(s.gates[LG_STEADY] == steady0) && s.c.l1[ci].e > e0 && s.c.l1[ci].n > 64 && s.c.l1[ci].n <= 128 && (s.c.l1[ci].day >> 14) == 2);
  // the next day's broken-cloud hour at 09Z: max/avg 1.6 is still unsteady
  HourRec b = lhour(day + 86400u + 9 * 3600u, 2);
  b.solar_avg = 150, b.solar_max = 240;
  const int e1 = s.c.l1[ci].e;
  learnHour(s, b, c);
  CHECK(GT(s.gates[LG_STEADY] == steady0 + 1) && s.c.l1[ci].e == e1);
  // the 08Z hour (k 0.41, under the 10-15 deg band's floor) is still refused, by k_low, even by an empty cell
  {
    const uint32_t h8 = day + 2 * 86400u + 8 * 3600u;
    double sc = 0;
    for (int i = 0; i < LEARN_BINS; i++) {
      double el, az;
      sunPos(h8 + 150 + 300u * (uint32_t)i, 52, 17, &el, &az);
      sc += clearSky(el);
    }
    HourRec e8 = lhour(h8, 2);
    e8.solar_avg = (uint16_t)std::lround(0.41 * sc / LEARN_BINS), e8.solar_max = (uint16_t)(e8.solar_avg + 5);
    [[maybe_unused]] const uint32_t kl = GV(s.gates[LG_K_LOW]);
    learnHour(s, e8, c);
    CHECK(GT(s.gates[LG_K_LOW] == kl + 1));
  }
}

#include "test_learn_v1.inc"

static void testLearn() {
  testLearnMaskAll();  // first
  testLearnLearners();
  testLearnSteadyWinter();
  testLearnMig();
  testLearnSun();
  testLearnBasics();
  testLearnGates();
  testLearnSunnyNovember();
  testLearnQuantiles();
  testLearnResetsAndReplay();
  testLearnWindows();
  testLearnLabels();
  testLearnL5();
  testLearnL6();
  testLearnL6Arith();
  testLearnClampRings();
  testLearnNoLocation();
  testLearnL6Relearn();
  testLearnL6Worst();
  testLearnBlobMigrate();
  testLearnJsonWidest();
}
