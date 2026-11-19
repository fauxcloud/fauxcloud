// test_learn_v1.inc -- the learners' use bars, accuracy at the bar, L7, seeding, restart, the V3 blob and the learned
// words in the outlook, included by test_learn.inc. The generic point 52 N 20 E; synthetic hours.

// ---------------------------------------------------------------- one table of counts
static void testLearnBars() {
  for (int id = 0; id < LBR_COUNT; id++) {
    const LearnBar& b = LEARN_BARS[id];
    const uint16_t lo = learnBarSamples(id, 1), md = learnBarSamples(id, 2), hi = learnBarSamples(id, 3);
    if (b.id != id || b.low != lo || b.med != md || b.high != hi)
      printf("  LEARN_BARS[%d] = %u/%u/%u, the firmware's count says %u/%u/%u\n", id, b.low, b.med, b.high, lo, md, hi);
    CHECK(b.id == id && b.low == lo && b.med == md && b.high == hi);
  }
  // need: at 0 the whole bar, mid-way the rest, at the bar 0 (L4 in 1/16, L6 W in Q16)
  CHECK(learnNeed(LBR_L4, 0, 1) == 265 && learnNeed(LBR_L1, 0, 2) == 12);
  uint32_t n = 0;
  for (int i = 0; i < 100; i++) n = (uint32_t)(((uint64_t)n * 65504) >> 16) + 16;  // 100 L4 hours
  CHECK(learnNeed(LBR_L4, n, 1) == 165);
  CHECK(learnNeed(LBR_L4, 3840, 1) == 0 && learnNeed(LBR_L6, 15u * 65536u, 1) == 0 && learnNeed(LBR_L6, 0, 3) == 78);
  // tolerance: 130 % at n 0, 115 % from the first bar
  CHECK(learnSteadyPct(0) == 130 && learnSteadyPct(160) == 123 && learnSteadyPct(320) == 115 && learnSteadyPct(5000) == 115);
}

// ---------------------------------------------------------------- at its use bar a learner is not prior-bound
static double gauss01() { return lgauss(); }
static double phi01(double z) { return 0.5 * (1 + std::erf(z / std::sqrt(2.0))); }

// L4: hours of wind uniform on [0, u] km/h (iid or AR(1) with a 24-h scale), until the first bar; P50 / P90 there and
// the empirical P90 of the hours fed (a check on a device compares with the ring's own hourly P90)
static void l4Run(double u, bool ar, double* p50, double* p90, double* emp90) {
  static LearnState s;
  learnInit(&s);
  const uint32_t h0 = 1794268800u;
  double z = 0;
  const double ph = std::exp(-1.0 / 24);
  std::vector<double> fed;
  for (int i = 0; i < 3000 && s.c.l4_n < 3840; i++) {
    z = ar ? ph * z + std::sqrt(1 - ph * ph) * gauss01() : gauss01();
    const double w = u * phi01(z);
    HourRec r = lhour(h0 + 3600u * (uint32_t)i);
    r.wind_avg = (uint16_t)std::lround(w * 10), r.gust_max = (uint16_t)(r.wind_avg + 20);
    learnHour(s, r, lctx());
    fed.push_back(r.wind_avg / 10.0);
  }
  std::sort(fed.begin(), fed.end());
  *p50 = s.c.l4_p50 / 1e3, *p90 = s.c.l4_p90 / 1e3, *emp90 = fed[(size_t)(fed.size() * 0.9)];
}
// the median and the worst of |err| over the trials
struct ErrStat {
  std::vector<double> e;
  void add(double x) { e.push_back(std::fabs(x)); }
  double median() { std::sort(e.begin(), e.end()); return e[e.size() / 2]; }
  double worst() { return *std::max_element(e.begin(), e.end()); }
};
static void testLearnNoisyTrials() {
  gLcg = 2311;
  constexpr int TRIALS = 15;
  // L4 P90 at the plausible ends and the prior (truth 3 / 12 / 20), P50 1 / 10: iid; AR(1) (24 h) against the hours'
  // own P90
  const double t90[3] = {3, 12, 20};
  for (double t : t90)
    for (int ar = 0; ar < 2; ar++) {
      ErrStat st;
      for (int k = 0; k < TRIALS; k++) {
        double p50, p90, emp;
        l4Run(t / 0.9, ar, &p50, &p90, &emp);
        st.add(p90 - (ar ? emp : t));
      }
      const double md = st.median(), wo = st.worst();
      printf("  info: L4 P90 truth %.0f%s: |err| at the bar median %.2f, worst %.2f km/h (%d trials)\n", t,
             ar ? " (AR 24 h, vs the hours' P90)" : "", md, wo, TRIALS);
      // iid within 1.0 km/h; under 24-h correlation 265 hours are ~11 independent days, measured up to ~1.1: bound
      // 1.5
      CHECK(md <= (ar ? 1.5 : 1.0));
    }
  const double t50[2] = {1, 10};
  for (double t : t50) {
    ErrStat st;
    for (int k = 0; k < TRIALS; k++) {
      double p50, p90, emp;
      l4Run(2 * t, false, &p50, &p90, &emp);
      st.add(p50 - t);
    }
    const double md = st.median(), wo = st.worst();
    printf("  info: L4 P50 truth %.0f: |err| median %.2f, worst %.2f km/h\n", t, md, wo);
    CHECK(md <= 1.0);  // 0.65 measured on U 0..20 at 10 km/h with the warm-up ramp
  }
  // L2 b: nightly maxima (integer %) with P90 = truth (85 / 93 / 100), seeded from the first night, at the first bar
  const int t2[3] = {85, 93, 100};
  const uint32_t d0 = 1791763200u;
  for (int t : t2) {
    ErrStat st;
    for (int k = 0; k < TRIALS; k++) {
      static LearnState s;
      learnInit(&s);
      for (int d = 0; d < 80 && s.c.l2_night_n < 1280; d++) {
        NightP p;
        p.te = 12;
        const long v = std::lround(t + 0.8 - 8 * lrand());
        p.rhmax = (int)(v > 100 ? 100 : v);
        feedNight(s, d0 + 86400u * (uint32_t)d, p);
      }
      CHECK(s.c.l2_night_n >= 1280);
      st.add(s.c.l2_night_e / 100.0 - t);
    }
    const double md = st.median(), wo = st.worst();
    printf("  info: L2 night truth %d %%: |err| at the bar median %.2f, worst %.2f %%\n", t, md, wo);
    CHECK(md <= 1.5);
  }
  // L1: clear June days with k's P90 = truth (0.45 dim, 1.0 bright, 0.55 the prior; hour-to-hour SD 0.02): the first
  // cell at medium
  const double t1[3] = {.45, 1.0, .55};
  const uint32_t jun = 1781913600u;  // 2026-06-20 00:00Z
  for (double t : t1) {
    ErrStat st;
    for (int k = 0; k < TRIALS; k++) {
      static LearnState s;
      learnInit(&s);
      int at = -1;
      for (int d = 0; d < 40 && at < 0; d++)
        for (int h = 3; h < 19 && at < 0; h++) {
          const uint32_t hr = jun + 86400u * (uint32_t)d + 3600u * (uint32_t)h;
          HourRec r = lhour(hr, 18);
          LearnCtx c = lctx();
          synthSolar(hr, t + .02 * (gauss01() - 1.2816), false, &r, &c);
          learnHour(s, r, c);
          for (int ci = 0; ci < L1_CELLS; ci++)
            if (s.c.l1[ci].n >= 640) at = ci;
        }
      CHECK(at >= 0);
      if (at >= 0) st.add(s.c.l1[at].e / 1e4 - t);
    }
    const double md = st.median(), wo = st.worst();
    printf("  info: L1 truth %.2f: |err| at medium median %.3f, worst %.3f\n", t, md, wo);
    CHECK(md <= 0.03);  // the ramp (x4 -> x2 -> x1 before medium) brings both ends within 0.03
  }
  // L3: live readings with P5 = truth (0.3 / 3 km/h), at the first bar
  const double t3[2][3] = {{.3, .2, 2.2}, {3, 2.6, 10.6}};
  for (const auto& t : t3) {
    ErrStat st;
    for (int k = 0; k < TRIALS; k++) {
      static LearnState s;
      learnInit(&s);
      for (int i = 0; i < 100 && s.c.l3_n < 200 * 64; i++) {
        HourRec r = lhour(1794268800u + 3600u * (uint32_t)i);
        LearnCtx c = lctx(true);
        c.wind_n = 60;
        for (int j = 0; j < 60; j++) c.wind[j] = (uint8_t)std::lround(10 * (t[1] + (t[2] - t[1]) * lrand()));
        learnHour(s, r, c);
      }
      st.add(s.c.l3_e / 1e4 - t[0]);
    }
    const double md = st.median(), wo = st.worst();
    printf("  info: L3 truth %.1f: |err| median %.2f, worst %.2f km/h\n", t[0], md, wo);
    CHECK(md <= 0.2);
  }
}

// ---------------------------------------------------------------- L5's label by the L1b class at mid-hour
static void testLearnL5Label() {
  const uint32_t nov = 1794268800u;
  struct Case { uint8_t cell; float sky; uint8_t want; };
  // sun cell: clear / overcast / mixed; unknown: clear only (darkness proves nothing); shade: none
  const Case cases[] = {{0x62, .65f, LBL_CLEAR}, {0x62, .15f, LBL_OVERCAST}, {0x62, .40f, LBL_MIXED},
                        {0x00, .65f, LBL_CLEAR}, {0x00, .15f, LBL_NONE},     {0x00, .40f, LBL_NONE},
                        {0x60, .65f, LBL_NONE},  {0x60, .15f, LBL_NONE}};
  for (const Case& k : cases) {
    static LearnState s;
    learnInit(&s);
    learnHour(s, lhour(nov + 11 * 3600), lctx());
    confidentPm(s, nov);
    memset(s.mask, k.cell, sizeof s.mask);
    NightP p;
    p.sky = k.sky;
    feedNight(s, nov, p);
    CHECK(s.c.lbl == k.want);
  }
}

// ---------------------------------------------------------------- no L2 a sample from melt tips
static void testLearnL2MeltTips() {
  static LearnState s;
  learnInit(&s);
  const uint32_t d0 = 1794268800u;
  uint32_t hr = d0;
  const auto dry = [&](int n, float t) {
    for (int i = 0; i < n; i++) learnHour(s, lhour(hr += 3600, t), lctx());
  };
  const auto wet = [&](float t) {  // two tipped hours in a row
    for (int i = 0; i < 2; i++) {
      HourRec r = lhour(hr += 3600, t);
      r.rain_sum = 50, r.rh_avg = 93;
      learnHour(s, r, lctx());
    }
  };
  dry(2, 5);
  wet(1.5f);  // at or below 2 °C: melt
  CHECK(GT(s.gates[LG_MELT] >= 1) && s.c.l2_wet_n == 0);
  dry(3, 5);
  HourRec icy = lhour(hr += 3600, 0.5f);  // icy rain (<= 1 °C)
  icy.rain_sum = 20;
  learnHour(s, icy, lctx());
  dry(9, 5);
  [[maybe_unused]] const uint32_t m0 = GV(s.gates[LG_MELT]);
  wet(5.0f);  // 10 hours after icy rain: still melt
  CHECK(GT(s.gates[LG_MELT] > m0) && s.c.l2_wet_n == 0);
  dry(30, 5);
  wet(5.0f);  // a day later: admitted (the second hour; the first follows a dry one), seeded from the sample
  CHECK(s.c.l2_wet_n == 64 && s.c.l2_wet_e == 9300);
}

// ---------------------------------------------------------------- L7
static bool l7HoldEdge();
static void testLearnL7() {
  static LearnState s;
  learnInit(&s);
  // a 60-day step (1000.0 then 1020.0 hPa) -> the weighted mean of the days (half-life 30), not a seed-biased EWMA
  const double lam = std::pow(2.0, -1.0 / 30);
  double num = 0, den = 0;
  for (int i = 0; i < 60; i++) {
    DayRec d;
    clearDay(&d, dayNumberToYmd(20400 + i));
    d.quality = 0, d.hours = 24, d.epoch = 0, d.p_avg = (int16_t)(i < 30 ? 10000 : 10200);
    learnDay(s, d, lctx());
    num = num * lam + d.p_avg, den = den * lam + 1;
  }
  const double e = s.c.l7_e / 65536.0, ref = num / den;
  printf("  info: L7 after a 60-day step: %.2f vs the weighted mean %.2f (0.1 hPa), n_eff %.1f\n", e, ref, s.c.l7_n / 64.0);
  CHECK(std::fabs(e - ref) <= 1.0);
  // the normal: a synthetic table (DJF +10 hPa, JJA -5 everywhere; a lat gradient in one column), a cosine by the day
  static int8_t tab[2 * MSLP_ROWS * MSLP_COLS];
  for (int i = 0; i < MSLP_ROWS * MSLP_COLS; i++) tab[i] = 10, tab[MSLP_ROWS * MSLP_COLS + i] = -5;
  int16_t nv = 0;
  CHECK(learnMslpNormal(52, 20, 15, &nv, tab) && nv == 10230);
  CHECK(learnMslpNormal(52, 20, 198, &nv, tab) && nv == 10080);
  CHECK(learnMslpNormal(52, 20, 106, &nv, tab) && std::abs(nv - 10155) <= 3);  // mid-April: halfway
  CHECK(!learnMslpNormal(71, 20, 15, &nv, tab) && !learnMslpNormal(NAN, 20, 15, &nv, tab));
  tab[6 * MSLP_COLS + 1] = 30, tab[7 * MSLP_COLS + 1] = 10;  // rows 30 / 45 N at 20 E: 52.5 N -> 10 + (30-10) * 0
  CHECK(learnMslpNormal(37.5f, 20, 15, &nv, tab) && nv == 10330);   // halfway between 30 N (30) and 45 N (10): 20
  CHECK(learnMslpNormal(37.5f, 30, 15, &nv, tab) && nv == 10280);   // and halfway to 40 E (10): 15
  CHECK(learnMslpNormal(37.5f, -340, 15, &nv, tab) && nv == 10330);  // lon wraps (-340 = 20 E)
  // the wrap across 0 / 340 E (synthetic: column 340 E = 10, column 0 E = 30 at 45 N)
  static int8_t wrap[2 * MSLP_ROWS * MSLP_COLS];
  wrap[7 * MSLP_COLS + 17] = 10, wrap[7 * MSLP_COLS + 0] = 30;
  CHECK(learnMslpNormal(45, 350, 15, &nv, wrap) && nv == 10330 && learnMslpNormal(45, -10, 15, &nv, wrap) && nv == 10330);
  CHECK(learnMslpNormal(45, 340, 15, &nv, wrap) && nv == 10230 && learnMslpNormal(45, 0, 15, &nv, wrap) && nv == 10430);
  CHECK(learnMslpNormal(45, 359.99f, 15, &nv, wrap) && std::abs(nv - 10430) <= 1);
  // the generated table is built in; a placeholder would fail here
  CHECK(learnMslpBuiltIn());
  // the built-in table at a few points (NCEP/NCAR R1 1991-2020): London Jan / Jul, Reykjavik Jan, Irkutsk Jan
  int16_t wj = 0, wl = 0, rj = 0, ij = 0;
  CHECK(learnMslpNormal(51.5f, -0.1f, 15, &wj) && wj >= 10120 && wj <= 10200);
  CHECK(learnMslpNormal(51.5f, -0.1f, 196, &wl) && wl >= 10120 && wl <= 10180);
  // the table's masked cells (Tibet / Mongolia, Greenland) drop out of the bilinear weights
  // when the rest weigh at least half; the 5-deg bitmap off for these table checks
  static const uint8_t noHigh[(MSLP_HIGH_ROWS * MSLP_HIGH_COLS + 7) / 8] = {};
  learnSetMslpHigh(noHigh);
  const bool rk = learnMslpNormal(64.1f, -21.9f, 15, &rj), ik = learnMslpNormal(52.3f, 104.3f, 15, &ij);
  CHECK(rk && rj >= 9940 && rj <= 10060);  // Reykjavik: its Greenland corner weighs 2.6 %
  CHECK(!ik || (ij >= 10240 && ij <= 10400));
  struct Town { const char* name; float lat, lon; bool has; };
  const Town towns[] = {{"Beijing", 39.9f, 116.4f, true},   {"Hanoi", 21.0f, 105.85f, true},
                        {"Novosibirsk", 55.0f, 82.9f, true}, {"Reykjavik", 64.1f, -21.9f, true},
                        {"Kathmandu", 27.7f, 85.3f, false},  {"Chengdu", 30.66f, 104.07f, false},
                        {"Lhasa", 29.65f, 91.1f, false}};
  for (const Town& tw : towns) {
    const bool got = learnMslpNormal(tw.lat, tw.lon, 15, &nv);
    if (got != tw.has) printf("  N-a: %s %s a normal\n", tw.name, got ? "has" : "has no");
    CHECK(got == tw.has && (!got || (nv >= 9900 && nv <= 10450)));
  }
  CHECK(!learnMslpNormal(45, 90, 15, &rj) && !learnMslpNormal(76 - 6, 320, 15, &rj));  // masked: Tibet, Greenland
  // a corner over high ground (-128) drops out and the others are renormalised while they weigh
  // at least half; at more than half masked: none. Synthetic: 45 N 20 E masked, 60 N 20 E = +20, the rest 0
  static int8_t hi[2 * MSLP_ROWS * MSLP_COLS];
  for (int sn = 0; sn < 2; sn++) hi[sn * MSLP_ROWS * MSLP_COLS + 7 * MSLP_COLS + 1] = MSLP_NONE, hi[sn * MSLP_ROWS * MSLP_COLS + 8 * MSLP_COLS + 1] = 20;
  CHECK(learnMslpNormal(52.5f, 20, 15, &nv, hi) && nv == 10330);  // masked weight exactly 0.5: kept, = the other corner
  CHECK(!learnMslpNormal(52.4f, 20, 15, &nv, hi));                 // masked weight just over 0.5: none
  CHECK(!learnMslpNormal(45, 20, 15, &nv, hi) && learnMslpNormal(60, 20, 15, &nv, hi) && nv == 10330);
  CHECK(learnMslpNormal(45, 0, 15, &nv, hi) && nv == 10130);       // the masked corner at weight 0: kept
  for (int sn = 0; sn < 2; sn++) hi[sn * MSLP_ROWS * MSLP_COLS + 7 * MSLP_COLS + 1] = 0;
  hi[MSLP_ROWS * MSLP_COLS + 7 * MSLP_COLS + 1] = MSLP_NONE;  // the JJA half alone masked: that half decides as well
  CHECK(!learnMslpNormal(45, 20, 15, &nv, hi) && learnMslpNormal(52.5f, 20, 15, &nv, hi));
  // the 5-deg high-ground bitmap silences a station on high ground inside a diluted cell; a
  // synthetic bitmap with Bogota's and Nairobi's boxes set
  static uint8_t hb[(MSLP_HIGH_ROWS * MSLP_HIGH_COLS + 7) / 8];
  const auto setBox = [&](float la, float lo) {
    const int i = (int)std::floor((la + 60) / 5) * MSLP_HIGH_COLS + (int)std::floor((lo < 0 ? lo + 360 : lo) / 5);
    hb[i / 8] |= (uint8_t)(1u << (i % 8));
  };
  setBox(4.6f, -74.1f), setBox(-1.3f, 36.8f);
  learnSetMslpHigh(hb);
  CHECK(learnMslpHighAt(4.6f, -74.1f) && learnMslpHighAt(-1.3f, 36.8f) && !learnMslpHighAt(51.5f, -0.1f) && !learnMslpHighAt(4.6f, -69.9f));
  CHECK(!learnMslpNormal(4.6f, -74.1f, 15, &nv) && !learnMslpNormal(-1.3f, 36.8f, 15, &nv) && learnMslpNormal(51.5f, -0.1f, 15, &nv));
  CHECK(learnMslpNormal(4.6f, -74.1f, 15, &nv, hi));  // a test table (no bitmap applies to it)
  learnSetMslpHigh(nullptr);
  if (learnMslpHighBuiltIn()) {  // the generated bitmap: the high towns silent, the lowland ones kept
    CHECK(learnMslpHighAt(4.6f, -74.1f) && learnMslpHighAt(-1.3f, 36.8f) && learnMslpHighAt(9.0f, 38.7f));  // Bogota, Nairobi, Addis
    CHECK(!learnMslpHighAt(51.5f, -0.1f) && !learnMslpHighAt(37.7f, -97.3f) && !learnMslpHighAt(64.1f, -21.9f));  // London, Wichita, Reykjavik
    printf("  info: high-ground bitmap: Denver %s, Mexico City %s\n", learnMslpHighAt(39.7f, -105.0f) ? "high" : "low",
           learnMslpHighAt(19.4f, -99.1f) ? "high" : "low");
  } else {
    printf("  info: high-ground bitmap not built in yet (regenerate src/mslp_norm.h with tools/mslp_normals.py --orog)\n");
  }
  // the days behind n_eff (the smallest k with neff^k(0) >= n), over 60 = 61
  CHECK(learnL7Days(0) == 0 && learnL7Days(64) == 1 && learnL7Days(12 * 64) == 14 && learnL7Days(30 * 64) == 51 &&
        learnL7Days(44 * 64) == 61);
  printf("  info: MSLP normal: London Jan %.1f / Jul %.1f, Reykjavik Jan %.1f, Irkutsk Jan %.1f hPa\n", wj / 10.0, wl / 10.0,
         rj / 10.0, ij / 10.0);
  // the grid's edges: the last rows (-60 / 75), clamped beyond them up to the +-70 limit, none past it; the 0 / 340 E
  // seam continuous (one 20-deg cell apart)
  int16_t a = 0, b = 0;
  CHECK(learnMslpNormal(-60, 0, 15, &a) && learnMslpNormal(-69.9f, 0, 15, &b) && a == b);  // under -60: row -60
  CHECK(learnMslpNormal(70, 0, 15, &a) && !learnMslpNormal(70.1f, 0, 15, &a) && !learnMslpNormal(-70.1f, 0, 15, &a));
  CHECK(learnMslpNormal(52, 359.99f, 15, &a) && learnMslpNormal(52, 0, 15, &b) && std::abs(a - b) <= 2);
  CHECK(learnMslpNormal(52, -0.01f, 15, &a) && learnMslpNormal(52, 359.99f, 15, &b) && std::abs(a - b) <= 1);
  int along = 0;
  for (int lo = -180; lo < 180; lo += 5)  // no step larger than one table step along 52 N, where both have a normal
    if (learnMslpNormal(52, (float)lo, 15, &a) && learnMslpNormal(52, (float)lo + 5, 15, &b)) {
      along++;
      CHECK(std::abs(a - b) <= 60);
    }
  CHECK(along >= 50);
  // stages, enter / exit against a flat normal (1013.0): each day's value equals the mean, so the mean stays put
  static const int8_t flat[2 * MSLP_ROWS * MSLP_COLS] = {};
  learnSetMslpTable(flat);
  const auto day = [&](int16_t p, LearnCtx c = lctx()) {
    static int k = 0;
    s.c.l7_e = (int32_t)p << 16;
    DayRec d;
    clearDay(&d, dayNumberToYmd(20600 + k++));
    d.quality = 0, d.hours = 24, d.epoch = 0, d.p_avg = p;
    learnDay(s, d, c);
  };
  learnInit(&s);
  s.c.l7_n = 13 * 64;           // stage 1: 20 hPa
  day(10130 + 199);
  CHECK(s.c.l7_flag == 0);
  day(10130 + 200);
  CHECK(s.c.l7_flag == 2);
  day(10130 - 250);
  CHECK(s.c.l7_flag == 1);      // below the normal: "low"
  s.c.l7_flag = 0, s.c.l7_n = 40 * 64;  // stage 2: 12 hPa, exit under 9
  day(10130 + 119);
  CHECK(s.c.l7_flag == 0);
  day(10130 + 120);
  CHECK(s.c.l7_flag == 2);
  day(10130 + 95);
  CHECK(s.c.l7_flag == 2);      // held above 9 hPa
  day(10130 + 89);
  CHECK(s.c.l7_flag == 0);
  LearnCtx off = lctx();
  off.p_off = -2000;            // the bridge's offset -20 hPa takes the +20 out
  s.c.l7_n = 40 * 64;
  day(10130 + 200, off);
  CHECK(s.c.l7_flag == 0);
  LearnCtx nol = lctx();
  nol.lat = NAN;
  day(10130 + 300, nol);
  CHECK(s.c.l7_flag == 0);      // no location: no normal, silent
  learnSetMslpTable(nullptr);
  // the REL-step restart: two consecutive clean hours >= 4 hPa apart (not weather); 3.9 hPa or a gap does not
  learnInit(&s);
  s.c.l7_n = 20 * 64, s.c.l7_e = 10130 << 16;
  const uint32_t h0 = 1794268800u;
  const auto hp = [&](uint32_t hr, int16_t p) {
    HourRec r = lhour(hr);
    r.p_avg = p;
    return learnHour(s, r, lctx());
  };
  hp(h0, 10130);
  CHECK(!(hp(h0 + 3600, 10169) & LE_L7_STEP) && s.c.l7_n == 20 * 64 && !s.c.l7_step);
  CHECK(!(hp(h0 + 3 * 3600, 10300) & LE_L7_STEP) && s.c.l7_n == 20 * 64 && !s.c.l7_step);  // a gap: not consecutive
  // a REL change (30.0 -> 10.0 hPa below): the step marks, the next hour on the new level confirms -> the restart
  CHECK(!(hp(h0 + 4 * 3600, 10100) & LE_L7_STEP) && s.c.l7_n == 20 * 64 && s.c.l7_step == 1);
  CHECK((hp(h0 + 5 * 3600, 10104) & LE_L7_STEP) && s.c.l7_n == 0 && s.c.l7_e == 0 && !s.c.l7_step);
  // a deep low's passage (4.0 hPa an hour, then still falling): no restart, the mark dropped by the further move and
  // never re-opened by it
  const auto fresh = [&] { learnInit(&s); s.c.l7_n = 20 * 64, s.c.l7_e = 10130 << 16; };
  fresh();
  hp(h0, 10000);
  CHECK(!(hp(h0 + 3600, 9960) & LE_L7_STEP) && s.c.l7_step == 1);
  CHECK(!(hp(h0 + 2 * 3600, 9920) & LE_L7_STEP) && !s.c.l7_step && s.c.l7_n == 20 * 64);
  CHECK(!(hp(h0 + 3 * 3600, 9900) & LE_L7_STEP) && !s.c.l7_step && s.c.l7_n == 20 * 64);  // within 2 hPa, but no mark
  fresh();  // a 4-hPa drop, then 3 more: not on the new level
  hp(h0, 10000), hp(h0 + 3600, 9960);
  CHECK(!(hp(h0 + 2 * 3600, 9930) & LE_L7_STEP) && !s.c.l7_step && s.c.l7_n == 20 * 64);
  fresh();  // a step, then a dirty hour (the mark dropped), then a clean hour on the new level: no restart
  hp(h0, 10000), hp(h0 + 3600, 10200);
  {
    HourRec r = lhour(h0 + 2 * 3600);
    r.p_avg = 10201, r.quality |= (uint16_t)(1u << Q_BARO_UNCAL);  // the console's factory pressure: not clean
    CHECK(!(learnHour(s, r, lctx()) & LE_L7_STEP) && !s.c.l7_step);
  }
  CHECK(!(hp(h0 + 3 * 3600, 10201) & LE_L7_STEP) && s.c.l7_n == 20 * 64);
  fresh();  // a step, then a gap: the mark does not cross it
  hp(h0, 10000), hp(h0 + 3600, 10200);
  CHECK(!(hp(h0 + 3 * 3600, 10201) & LE_L7_STEP) && !s.c.l7_step && s.c.l7_n == 20 * 64);
  CHECK(l7HoldEdge());  // exactly 2.0 hPa holds, 2.1 does not
}
// the confirm edge: the third hour 2.0 hPa from the step hour confirms, 2.1 hPa does not
static bool l7HoldEdge() {
  static LearnState s;
  const uint32_t h0 = 1794268800u;
  const auto run = [&](int16_t third) {
    learnInit(&s);
    s.c.l7_n = 20 * 64, s.c.l7_e = 10130 << 16;
    uint8_t ev = 0;
    for (int k = 0; k < 3; k++) {
      HourRec r = lhour(h0 + (uint32_t)k * 3600);
      r.p_avg = k == 0 ? 10000 : k == 1 ? 10200 : third;
      ev = learnHour(s, r, lctx());
    }
    return (ev & LE_L7_STEP) != 0;
  };
  return run(10220) && run(10180) && !run(10221) && !run(10179);
}
// a V3 core written and sealed by an earlier learn.cpp (the step test's state after a REL restart and four hours on the new
// level, L7 seeded), frozen here: the byte l7_step now names (offset 246) is zero, the blob checks Ok as V3 with no
// migration, and every L7 field reads as written
static const char* const V3_FROZEN_B30 =
    "4c5203000100000000cff26a00000000bff5d2c000000000512bf654000000000000ff0000003901000000000f550606080000800080008000804d00"
    "7c15000000007c15000000007c15000000007c15000000007c15000000007c15000000007c15000000007c15000000007c15000000007c1500000000"
    "7c15000000007c15000000001027000000001027000000001027000000000000b80ba02d7900390100007e27c00100000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000006400a00fd0070000960032000000000000000000008000805000000028009506"
    "782700000000000000000000";
static void testLearnV3FrozenStep() {
  uint8_t raw[256];
  CHECK(strlen(V3_FROZEN_B30) == 512);
  for (int i = 0; i < 256; i++) {
    unsigned v = 0;
    sscanf(V3_FROZEN_B30 + 2 * i, "%2x", &v);
    raw[i] = (uint8_t)v;
  }
  LearnCore c;
  memcpy(&c, raw, sizeof c);
  CHECK(learnCheckCore(c, sizeof c) == BlobCheck::Ok && c.version == 3);
  CHECK(raw[246] == 0 && c.l7_step == 0 && c.reserve3[0] == 0);
  CHECK(c.l7_pprev == 10104 && c.l7_n == 7 * 64 && c.l7_e == (int32_t)(10110 << 16) && c.l7_flag == 0);
  // loaded and fed the next hour on the same level: no restart (no mark was stored), the L7 state kept
  static LearnState s;
  learnInit(&s);
  s.c = c;
  HourRec r = lhour(c.last_hour + 3600);
  r.p_avg = 10105;
  CHECK(!(learnHour(s, r, lctx()) & LE_L7_STEP) && s.c.l7_n == 7 * 64 && !s.c.l7_step);
}

// ---------------------------------------------------------------- tolerance by n, neighbour seeding
static void testLearnSeeding() {
  // the 2026-11-15 09Z hour (testLearnSteadyWinter) in the 15-20 deg AM cell (4): pre-set just under medium
  static LearnState s;
  learnInit(&s);
  const uint32_t day = 1794700800u;
  LearnCtx c = lctx();
  c.lon = 17;
  learnHour(s, lhour(day, 2), c);  // adopt the context
  s.c.l1[4] = LearnCell{5800, 639, 0};
  s.c.l1[6] = LearnCell{5500, 10, 0};  // 20-30 AM: n > 0, never overwritten
  s.c.l1[5].e = 5000;                   // 15-20 PM: the other half, never seeded
  HourRec r = lhour(day + 9 * 3600u, 2);
  r.solar_avg = 167, r.solar_max = 200;
  learnHour(s, r, c);
  const int e4 = s.c.l1[4].e;
  CHECK(s.c.l1[4].n >= 640 && s.c.l1[2].e == e4 && s.c.l1[6].e == 5500 && s.c.l1[5].e == 5000 && s.c.l1[0].e == 5500 &&
        s.c.l1[2].n == 0);
  // the sheet's marker: an empty cell with E not the prior, the band of the neighbour with the highest n
  {
    static LearnView v;
    learnView(s, &v);
    LearnMeta m = {"learning", nullptr, 0, true, 0, 0, 0, 0, day + 86400, 52, 17, 0, 0, 0, 0, 0, 0, nullptr};
    std::string js;
    char b[96];
    Out o(toString, &js, b, sizeof b);
    learnJson(o, v, m);
    o.flush();
    CHECK(contains(js, "{\"el\":10,\"pm\":false,\"e\":") && contains(js, "\"from\":15}") &&
          contains(js, "\"state\":\"in_use\",\"from\":null}"));
  }
  // a later move of 0.05 or more re-seeds the empty neighbour; a smaller difference leaves it
  s.c.l1[2].e = (uint16_t)(s.c.l1[4].e - 400);
  HourRec r2 = lhour(day + 86400u + 9 * 3600u, 2);
  r2.solar_avg = 167, r2.solar_max = 200;
  learnHour(s, r2, c);
  CHECK(s.c.l1[2].e == (uint16_t)(e4 - 400));
  s.c.l1[2].e = (uint16_t)(s.c.l1[4].e - 900);
  HourRec r3 = lhour(day + 2 * 86400u + 9 * 3600u, 2);
  r3.solar_avg = 167, r3.solar_max = 200;
  learnHour(s, r3, c);
  CHECK(s.c.l1[2].e == s.c.l1[4].e && s.c.l1[0].e == 5500);  // two bands away: never
  // tolerance: max/avg 1.28 x the model's is taken by an empty cell, refused by a known one
  static LearnState t;
  learnInit(&t);
  learnHour(t, lhour(day, 2), c);
  HourRec w = lhour(day + 9 * 3600u, 2);
  w.solar_avg = 167, w.solar_max = (uint16_t)std::lround(167 * 1.067 * 1.28);
  [[maybe_unused]] const uint32_t st0 = GV(t.gates[LG_STEADY]);
  learnHour(t, w, c);
  CHECK(GT(t.gates[LG_STEADY] == st0) && t.c.l1[4].n > 0);
  t.c.l1[4].n = 320;
  HourRec w2 = lhour(day + 86400u + 9 * 3600u, 2);
  w2.solar_avg = 167, w2.solar_max = (uint16_t)std::lround(167 * 1.067 * 1.28);
  learnHour(t, w2, c);
  CHECK(GT(t.gates[LG_STEADY] == st0 + 1));
}

// ---------------------------------------------------------------- "Restart learning"
static void testLearnRestart() {
  const uint32_t d0 = 1791763200u;
  static NightP ps[40];
  migNights(ps);
  migRecord(d0, 30, ps);
  static LearnState s, r;
  learnInit(&s);
  confidentPm(s, d0);
  const size_t cut = gMigRing.size() * 2 / 3;
  for (size_t i = 0; i < cut; i++) learnHour(s, gMigRing[i], lctx());
  s.c.l3_n = 5000, s.c.l7_n = 900, s.c.l7_e = 10130 << 16;
  s.c.l2_night_e = 9000;  // the night value the restart keeps as its start point
  const LearnCore before = s.c;
  CHECK(before.l4_n > 0 && before.l2_night_n > 0 && before.l5_cn + before.l5_on > 0);
  CHECK(learnRestart(s, 20) == LE_RELEARN);
  bool zero = s.c.l2_wet_n == 0 && s.c.l2_night_n == 0 && s.c.l3_n == 0 && s.c.l4_n == 0 && s.c.l5_cn == 0 &&
              s.c.l5_on == 0 && s.c.l7_n == 0 && s.c.l6_w <= 5u * 65536u;
  for (const LearnCell& x : s.c.l1) zero = zero && x.n == 0;
  CHECK(zero && s.c.l4_p90 == before.l4_p90 && s.c.l2_night_e == before.l2_night_e && s.c.l1[1].e == before.l1[1].e &&
        s.c.relearn_hour == before.last_hour && (s.c.flags & LF_RELEARNED) && s.mask[100] == 0 && s.c.relearn_until);
  {  // every level back to "default" in the JSON
    static LearnView v;
    learnView(s, &v);
    LearnMeta m = {"learning", nullptr, 0, true, 0, 0, 0, 0, s.c.last_hour, 52, 20, 0, 0, 0, 0, 0, 0, nullptr};
    std::string js;
    char b[96];
    Out o(toString, &js, b, sizeof b);
    learnJson(o, v, m);
    o.flush();
    CHECK(!contains(js, "\"state\":\"learned\"") && !contains(js, "\"state\":\"in_use\"") && !contains(js, "\"state\":\"info\"") &&
          contains(js, "\"relearn\":true"));
  }
  for (size_t i = cut; i < gMigRing.size(); i++) learnHour(s, gMigRing[i], lctx());
  // L2 re-seeds: the first night after the restart sets E to that night's maximum (85 % in the fixture)
  CHECK(s.c.l2_night_e == 8500);
  // a replay of the rings from the priors with the restart hour re-applies it there: the same core
  learnInit(&r);
  confidentPm(r, d0);
  r.c.relearn_hour = before.last_hour;
  for (const HourRec& h : gMigRing) learnHour(r, h, lctx());
  LearnCore a = s.c, b = r.c;
  a.l3_n = b.l3_n = 0, a.l7_e = b.l7_e = 0, a.l7_n = b.l7_n = 0;
  CHECK(memcmp(&a, &b, sizeof a) == 0);
}

// ---------------------------------------------------------------- V3 blob, migration from V2 and V1
static LearnCoreV2 packV2(const LearnCore& c) {
  LearnCoreV2 v;
  memset(&v, 0, sizeof v);
#define B23_PACK(f) memcpy(&v.f, &c.f, sizeof v.f);
  LEARN_V2_SHARED(B23_PACK)
#undef B23_PACK
  v.magic[0] = 'L', v.magic[1] = 'R', v.version = 2;
  v.crc = 0;
  v.crc = crc32(&v, sizeof v);
  return v;
}
static bool sameReplayed(const LearnCore& a, const LearnCore& b, uint16_t m) {
  bool same = true;
#define B23_EQ(f) same = same && memcmp(&a.f, &b.f, sizeof a.f) == 0;
  if (m & LM_L1) { LEARN_L1_FIELDS(B23_EQ) }
  if (m & LM_L2) { LEARN_L2_FIELDS(B23_EQ) }
  if (m & LM_L4) { LEARN_L4_FIELDS(B23_EQ) }
  if (m & LM_L5) { LEARN_L5_FIELDS(B23_EQ) }
  if (m & LM_L6) { LEARN_L6_FIELDS(B23_EQ) }
#undef B23_EQ
  return same;
}
// a V2 core written and sealed by the V2 learn.cpp itself (10 synthetic days at 52 N 20 E, 20 day records), frozen here, so a typo in LearnCoreV2 cannot hide behind its own static_asserts
static const char* const V2_FROZEN =
    "4c52020001000000707fff6a07293501bff5d2c067452301448e0bbb00000000"
    "0000ff0000004301001dff6a105b080800001d00f5ff2000f7ff08007c150000"
    "00007c15000000007c15000000007c15000000007c15000000007c1500000000"
    "7c15000000007c15000000007c15000000007c15000000007c15000000007c15"
    "00000000102700000000e3261c0242011027000000000000ec138025aa0d4301"
    "a26489279f040000000000000000000000000000000000000000000000000000"
    "00000000000000000000000000000000000000006400a00fd007000096003200"
    "00000000000000002300f7ff0b00420124059a04000000000000000000000000";
static void testLearnV2Frozen() {
  uint8_t raw[256];
  for (int i = 0; i < 256; i++) {
    unsigned v = 0;
    sscanf(V2_FROZEN + 2 * i, "%2x", &v);
    raw[i] = (uint8_t)v;
  }
  LearnCore chk;
  memcpy(&chk, raw, sizeof chk);
  CHECK(learnCheckCore(chk, sizeof chk) == BlobCheck::Older);
  LearnCore m;
  uint8_t from = 0;
  uint16_t rep = 0;
  CHECK(learnMigrateCore(raw, sizeof raw, &m, &from, &rep) && from == 2 && rep == LM_SINCE_V2 && m.version == 3);
  CHECK(m.last_hour == 1795129200u && m.last_day == 20261127u && m.mac_hash == 0x1234567u && m.profile == 1 && m.epoch == 0);
  CHECK(m.l4_p50 == 5100 && m.l4_p90 == 9600 && m.l4_n == 3498 && m.l2_night_e == 9955 && m.l2_night_n == 540);
  CHECK(m.l5_ce == 150 && m.l5_oe == 50 && m.l6_w == 0 && m.l6_a == 100 && m.l6_b == 4000 && m.l6_c == 2000);
  CHECK(m.l7_e == 663315618 && m.l7_n == 11 * 64 && m.l7_flag == 0);  // n 1183 under the 90-day lambda, capped
  CHECK(m.l7_pprev == I16_NONE && m.relearn_hour == 0 && m.l1[4].e == 5500);
  learnSealCore(&m);
  CHECK(learnCheckCore(m, sizeof m) == BlobCheck::Ok);
}

static void testLearnV3Migrate() {
  testLearnV2Frozen();
  // layout: an LCG-filled V2 blob -> Older -> every V2 field copied, the V3 fields at init
  LearnCoreV2 v;
  uint32_t x = 777;
  uint8_t* p = (uint8_t*)&v;
  for (size_t i = 0; i < sizeof v; i++) x = x * 1103515245u + 12345u, p[i] = (uint8_t)(x >> 16);
  v.magic[0] = 'L', v.magic[1] = 'R', v.version = 2;
  if (!v.last_hour) v.last_hour = 1794268800u;
  v.crc = 0;
  v.crc = crc32(&v, sizeof v);
  LearnCore chk;
  memcpy(&chk, &v, sizeof chk);
  CHECK(learnCheckCore(chk, sizeof chk) == BlobCheck::Older);
  LearnCore m;
  uint8_t from = 0;
  uint16_t rep = 0;
  CHECK(learnMigrateCore(&v, sizeof v, &m, &from, &rep) && from == 2 && rep == LM_SINCE_V2 && m.version == 3);
  // every V2 field bit for bit, but L7's flag and count (their meaning changed in V3)
#define B23_FIELD(f) \
  if (strcmp(#f, "l7_flag") && strcmp(#f, "l7_n")) CHECK(memcmp(&m.f, &v.f, sizeof m.f) == 0);
  LEARN_V2_SHARED(B23_FIELD)
#undef B23_FIELD
  static const uint8_t z[4] = {};
  CHECK(m.l7_pprev == I16_NONE && m.relearn_hour == 0 && m.l7_step == 0 && m.reserve3[0] == 0 && memcmp(m.pad, z, 4) == 0);
  CHECK(m.l7_flag == 0 && m.l7_n == (v.l7_n < 11 * 64 ? v.l7_n : 11 * 64) && m.l7_e == v.l7_e);  // value kept, stage 0
  // the pass: V2 history (every learner) -> V3; L1 / L2 / L4 equal a replay of them alone from their priors, the rest
  // copied; then the catch-up feeds every learner
  const uint32_t d0 = 1791763200u;
  static NightP ps[40];
  migNights(ps);
  migRecord(d0, 40, ps);
  static LearnState A, M, P, F;
  learnInit(&A);
  memset(A.mask, 0x62, sizeof A.mask);
  const size_t cut = gMigRing.size() * 3 / 4;
  for (size_t i = 0; i < cut; i++) learnHour(A, gMigRing[i], lctx());
  for (int ver = 2; ver >= 1; ver--) {
    learnInit(&M);
    memcpy(M.mask, A.mask, sizeof M.mask);
    if (ver == 2) {
      const LearnCoreV2 v2 = packV2(A.c);
      CHECK(learnMigrateCore(&v2, sizeof v2, &M.c, &from, &rep) && from == 2 && rep == LM_SINCE_V2);
    } else {
      const LearnCoreV1 v1 = packV1(A.c);
      CHECK(learnMigrateCore(&v1, sizeof v1, &M.c, &from, &rep) && from == 1 && rep == (LM_SINCE_V1 | LM_SINCE_V2));
    }
    const LearnCore pre = M.c;
    migPass(&P, M, rep, M.c.last_hour, M.c.last_hour);
    learnInit(&F);
    memcpy(F.mask, A.mask, sizeof F.mask);
    F.c.loc_sig = A.c.loc_sig, F.c.profile = A.c.profile, F.c.mac_hash = A.c.mac_hash, F.c.flags = A.c.flags;
    for (const HourRec& h : gMigRing)
      if (h.hour <= M.c.last_hour) learnHour(F, h, lctx(), rep);
    CHECK(sameReplayed(F.c, M.c, rep) && M.c.l4_n > 0 && M.c.l2_night_n > 0);
    // the learners not replayed are the migrated ones, bit for bit
    CHECK(M.c.l3_e == pre.l3_e && M.c.l3_n == pre.l3_n && M.c.l7_e == pre.l7_e && M.c.l7_n == pre.l7_n);
    if (ver == 2) CHECK(sameL5L6(M.c, A.c));
    printf("  info: V%d -> V3 pass: L4 P90 %.2f -> %.2f, L2 night %.2f -> %.2f\n", ver, A.c.l4_p90 / 1e3, M.c.l4_p90 / 1e3,
           A.c.l2_night_e / 100.0, M.c.l2_night_e / 100.0);
  }
  // A was fed under the V3 rules already: its V2 -> V3 re-derive is the identity on L1 / L2 / L4
  learnInit(&M);
  memcpy(M.mask, A.mask, sizeof M.mask);
  const LearnCoreV2 v2 = packV2(A.c);
  learnMigrateCore(&v2, sizeof v2, &M.c, &from, &rep);
  migPass(&P, M, rep, M.c.last_hour, M.c.last_hour);
  CHECK(sameReplayed(M.c, A.c, LM_SINCE_V2));
}

// ---------------------------------------------------------------- graduation
static void testLearnUseOf() {
  static LearnState s;
  learnInit(&s);
  const uint32_t noon = 1782036000u;  // 2026-06-21 10:00Z: el ~ 60 deg at 52 N 20 E
  learnHour(s, lhour(noon), lctx());
  LearnUse u;
  learnUseOf(s, 52, 20, 0, 0, &u);
  CHECK(u.hour == noon && u.bits == 0 && u.sky_cls == 255);  // nothing at its use level: nothing set
  const uint16_t t14 = lday14(noon);
  const int ci = 5 * 2 + 0;  // >= 45 deg AM
  s.c.l1[ci] = LearnCell{7000, 640, t14};
  s.mask[l1bCell(60, 160)] = 0x62;
  s.c.l2_night_e = 9400, s.c.l2_night_n = 1280, s.c.l2_night_day = t14;
  s.c.l5_ce = 180, s.c.l5_oe = 40, s.c.l5_cn = s.c.l5_on = 800, s.c.l5_cd = s.c.l5_od = t14;
  learnUseOf(s, 52, 20, 150, -20, &u);
  CHECK((u.bits & LU_L1) && (u.bits & LU_L2) && (u.bits & LU_L5) && !(u.bits & LU_L6));
  CHECK(u.rh_sat == 940 - 20 && u.cool == 110 && u.sky_ref > 500 && u.sky_ref_max >= u.sky_ref && u.sky_cls <= 2);
  s.c.l5_on = 700;  // the overcast class below medium: L5 not in use
  s.c.gbits |= LB_STUCK;  // RH stuck at its cap: no S
  learnUseOf(s, 52, 20, 0, 0, &u);
  CHECK(!(u.bits & LU_L5) && !(u.bits & LU_L2) && (u.bits & LU_L1));
  // L6 high with an open night (T / Td at sunset + 2 h): the minimum +- SD, rounded outward, calibrated
  s.c.l6_w = 80u * 65536u, s.c.l6_e2 = 100u * 80u * 256u, s.c.l6_day = t14;  // SD 1.0 °C (e2 Q8, W Q16)
  s.c.l6_a = 100, s.c.l6_b = 4000, s.c.l6_c = 2000;
  s.c.n_start = noon + 3600, s.c.n_len = 14, s.c.n_t2 = 30, s.c.n_td2 = -20, s.c.n_L = 140;
  learnUseOf(s, 52, 20, 150, 0, &u);
  const int16_t pr = l6Predict(30, -20, 140, 100, 4000, 2000);
  printf("  info: L6 use: predicted %.1f, range %.0f .. %.0f (SD 1.0, cal +1.5)\n", pr / 10.0, u.min_lo / 10.0, u.min_hi / 10.0);
  CHECK((u.bits & LU_L6) && u.min_lo % 10 == 0 && u.min_hi % 10 == 0 && u.min_lo <= pr + 15 - 10 && u.min_hi >= pr + 15 + 10 &&
        u.min_hi - u.min_lo <= 40 && u.night_end == noon + 3600 + 14 * 3600);
  s.c.l6_e2 = 900u * 80u * 256u;  // SD 3.0 °C: not even "learned"
  learnUseOf(s, 52, 20, 0, 0, &u);
  CHECK(!(u.bits & LU_L6));
}

// the outlook: nothing changes without a learner in use; each learner's rule in use
static std::string olJson(const Outlook& x) {
  std::string js;
  char b[96];
  Out o(toString, &js, b, sizeof b);
  outlookJson(o, x);
  o.flush();
  return js;
}
static std::string olAll(const Outlook& x) {
  char st[300], at[300];
  outlookState(st, sizeof st, x);
  outlookAttributes(at, sizeof at, x);
  char sg[16];
  snprintf(sg, sizeof sg, "%08lx", (unsigned long)outlookSig(x));
  return olJson(x) + "|" + st + "|" + at + "|" + sg;
}
static void testLearnGraduation() {
  // a calm evening at 52 N 20 E (sunset 15:04Z on 11-06): 0.9 °C an hour, steady pressure
  const uint32_t base = (uint32_t)unixFromUtc(2026, 11, 6, 11, 0, 0);
  OutHour h[8];
  for (int i = 0; i < 8; i++) {
    h[i] = OutHour{};
    h[i].hour = base + 3600u * (uint32_t)i, h[i].p = 1020.0, h[i].t = 8.0f - 0.9f * (float)i, h[i].td = NAN;
    h[i].wind = 2, h[i].rain = 0, h[i].solar = NAN, h[i].calm = true, h[i].rh = 90, h[i].solar_max = NAN;
  }
  OutlookIn in = {};
  in.now = base + 7 * 3600 + 600, in.time_ok = in.has_baro = true, in.lat = 52, in.lon = 20, in.h = h, in.n = 7;
  in.open_rain = 0, in.t = h[6].t, in.td = NAN, in.rate = in.rain10 = in.gust10 = in.solar = NAN, in.rh = 90, in.wind10 = NAN;
  const Outlook d0 = outlook(in);
  // byte-identical with no use, a zero use and a use whose learner decides nothing (L2 in use, no fog)
  LearnUse zu = {};
  in.use = &zu;
  const Outlook d1 = outlook(in);
  LearnUse l2 = {};
  l2.bits = LU_L2, l2.rh_sat = 990, l2.hour = h[6].hour;
  in.use = &l2;
  const Outlook d2 = outlook(in);
  CHECK(olAll(d0) == olAll(d1) && olAll(d0) == olAll(d2) && d0.state != OS_NIGHT_CLEAR && contains(olJson(d0), "\"conf\":\"default\""));
  // L5: a learned 0.8 °C an hour makes the same evening clear
  LearnUse l5 = {};
  l5.bits = LU_L5, l5.cool = 80, l5.hour = h[6].hour;
  in.use = &l5;
  const Outlook a = outlook(in);
  const std::string ja = olJson(a);
  CHECK(a.state == OS_NIGHT_CLEAR && a.learned == LU_L5 && contains(ja, "\"conf\":\"learned\"") &&
        contains(ja, "\"learn\":[\"l5\"],\"cool_thr\":0.80}}"));
  // L6: tonight's minimum, on the clear night only, before the night's end, never over a tendency
  LearnUse l56 = l5;
  l56.bits |= LU_L6, l56.min_lo = -90, l56.min_hi = -50, l56.night_end = in.now + 6 * 3600;
  in.use = &l56;
  const Outlook b = outlook(in);
  char st[300], at[300];
  outlookState(st, sizeof st, b);
  outlookAttributes(at, sizeof at, b);
  printf("  info: min_range HA: %s\n", st);
  CHECK(b.next == ON_MIN_RANGE && contains(st, "Minimum around \xE2\x88\x92" "9 to \xE2\x88\x92" "5 \xC2\xB0" "C tonight.") &&
        contains(at, ",\"min\":[-9,-5]}") && contains(olJson(b), "\"min_lo\":-9,\"min_hi\":-5}") && strlen(st) <= 255 &&
        outlookSig(b) != outlookSig(a));
  l56.night_end = in.now;
  CHECK(outlook(in).next != ON_MIN_RANGE);
  l56.night_end = in.now + 6 * 3600;
  OutHour hf[8];
  memcpy(hf, h, sizeof hf);
  for (int i = 0; i < 8; i++) hf[i].p = 1025.0 - 1.0 * i;  // falling 3 hPa in 3 h
  in.h = hf;
  const Outlook c = outlook(in);
  CHECK(c.state == OS_NIGHT_CLEAR && c.next == ON_P_FALLING);
  in.h = h;
  in.use = &l56;
  in.stale = true;
  CHECK(outlook(in).next != ON_MIN_RANGE);
  in.stale = false;
  // L2 fog: the newest hour's RH' >= 98 %, light wind, no tip in 3 h, the live RH' >= 97 %
  LearnUse f = {};
  f.bits = LU_L2, f.rh_sat = 975, f.hour = h[6].hour;
  OutHour hw[8];
  memcpy(hw, h, sizeof hw);
  for (OutHour& x : hw) x.t = 5, x.rh = 96;
  in.h = hw, in.t = 5, in.rh = 95.5f, in.use = &f;
  const Outlook g = outlook(in);
  CHECK(g.warn == OW_FOG_POSSIBLE && g.learned == LU_L2 && contains(olJson(g), "\"learn\":[\"l2\"],\"rh_sat\":97.5,\"rh_rel\":98.5}}"));
  in.rh = 94;  // the live RH' 96.4 %: gone
  CHECK(outlook(in).warn == FC_NONE);
  in.rh = 95.5f;
  hw[5].rain = 0.2f;  // a tip 2 h ago
  CHECK(outlook(in).warn == FC_NONE);
  hw[5].rain = 0;
  hw[6].wind = 6;  // not calm
  CHECK(outlook(in).warn == FC_NONE);
  hw[6].wind = 2;
  in.use = nullptr;
  CHECK(outlook(in).warn == FC_NONE);  // L2 not in use: the default S of 100 % never says fog
  // priority on a clear night (cooling 1.2 °C/h: clear by the fixed rule too): hoar frost > ground frost > fog
  LearnUse f5 = f;
  f5.bits |= LU_L5, f5.cool = 80;
  OutHour hc[8];
  memcpy(hc, h, sizeof hc);
  for (int i = 0; i < 8; i++) hc[i].rh = 96, hc[i].t = 8.0f - 1.2f * (float)i;
  in.h = hc, in.use = &f5, in.t = -1.0f, in.td = -2.5f;
  CHECK(outlook(in).warn == OW_HOAR_FROST);
  in.t = 1.0f, in.td = -5;
  CHECK(outlook(in).warn == OW_GROUND_FROST);  // a safety warning is never displaced by the inference
  in.t = 3.0f;
  const Outlook gf = outlook(in);
  CHECK(gf.state == OS_NIGHT_CLEAR && gf.warn == OW_FOG_POSSIBLE && gf.learned == (LU_L2 | LU_L5));
  f5.bits = LU_L5;
  CHECK(outlook(in).warn == FC_NONE);
  // a learned L5 threshold never moves the frost warnings. 1.60 °C/h learned, the night cools
  // 1.2 °C/h at T 1 °C: not "Clear night." by the learned rule, but "Ground frost possible." exactly as without learners
  LearnUse s16 = {};
  s16.bits = LU_L5, s16.cool = 160, s16.hour = hc[6].hour;
  in.t = 1.0f, in.td = -5, in.use = nullptr;
  const Outlook fixed = outlook(in);
  in.use = &s16;
  const Outlook learned16 = outlook(in);
  CHECK(fixed.warn == OW_GROUND_FROST && learned16.warn == OW_GROUND_FROST && learned16.state != OS_NIGHT_CLEAR &&
        fixed.state == OS_NIGHT_CLEAR);
  // and the other way: 0.80 learned on a night cooling 0.9 °C/h: "Clear night." by the learned rule, no frost warning
  // (the fixed rule sees no clear night: as without learners)
  in.h = h, in.use = &l5, in.t = 1.0f;
  const Outlook l08 = outlook(in);
  in.use = nullptr;
  CHECK(l08.state == OS_NIGHT_CLEAR && l08.warn == FC_NONE && outlook(in).warn == FC_NONE);
  // L1 sky words by day: 2026-06-21 10:00Z (el ~ 60 deg), the newest closed hour
  const uint32_t dh = (uint32_t)unixFromUtc(2026, 6, 21, 10, 0, 0);
  OutHour dy[5];
  for (int i = 0; i < 5; i++) {
    dy[i] = OutHour{};
    dy[i].hour = dh - 3600u * (uint32_t)(4 - i), dy[i].p = 1015, dy[i].t = 20, dy[i].td = NAN, dy[i].wind = 8, dy[i].rain = 0;
    dy[i].solar = 600, dy[i].solar_max = 640, dy[i].rh = 60;
  }
  OutlookIn di = in;
  di.h = dy, di.n = 5, di.now = dh + 3600 + 600, di.t = 20, di.td = NAN, di.rh = 60, di.solar = NAN;
  LearnUse sk = {};
  sk.bits = LU_L1, sk.hour = dh, sk.sky_ref = 650, sk.sky_ref_max = 680, sk.sky_cls = 1;
  di.use = &sk;
  const auto sky = [&](float avg, float mx, uint8_t cls) {
    dy[4].solar = avg, dy[4].solar_max = mx, sk.sky_cls = cls;
    return outlook(di).state;
  };
  CHECK(sky(600, 640, 1) == OS_DAY_SUNNY && sky(600, 640, 0) == OS_DAY_SUNNY && sky(600, 640, 2) == OS_DRY);
  CHECK(sky(600, 800, 1) == OS_DAY_SPELLS || sky(600, 800, 1) == OS_DRY);  // unsteady and k >= 0.85: not "Sunny."
  CHECK(sky(600, 800, 1) != OS_DAY_SUNNY);
  CHECK(sky(400, 620, 1) == OS_DAY_SPELLS && sky(400, 620, 0) == OS_DAY_SPELLS && sky(400, 620, 2) == OS_DRY);
  CHECK(sky(150, 250, 1) == OS_DAY_NOSUN && sky(150, 250, 0) == OS_DRY && sky(150, 250, 2) == OS_DRY);
  CHECK(sky(600, 640, 255) == OS_DRY);  // dome, < 10 deg, no location: no sky word
  sk.hour = dh - 3600;                   // a stale snapshot (not the newest closed hour)
  CHECK(sky(600, 640, 1) == OS_DRY);
  sk.hour = dh;
  di.solar = 200;  // the live solar under half of E x clear sky: "Sunny." ends
  CHECK(sky(600, 640, 1) == OS_DRY);
  di.solar = 400;  // over half: "No sun." ends
  CHECK(sky(150, 250, 1) == OS_DRY);
  di.solar = NAN;
  dy[4].rain = 0.4f;  // the hour before had a tip: recent rain outranks
  CHECK(sky(600, 640, 1) == OS_RECENT_RAIN);
  dy[4].rain = 0;
  sky(600, 640, 1);
  const Outlook sn = outlook(di);
  CHECK(sn.learned == LU_L1 && contains(olJson(sn), "\"state\":\"day_sunny\"") && contains(olJson(sn), "\"learn\":[\"l1\"]"));
  // line 1 <= 28 and the HA state <= 255 for the day codes at their longest
  const uint8_t dayw[3] = {OS_DAY_SUNNY, OS_DAY_SPELLS, OS_DAY_NOSUN};
  for (uint8_t sc : dayw)
    for (int q = 0; q < OQ_COUNT; q++) CHECK(strlen(OUT_STATE_TEXT[sc]) + 1 + strlen(OUT_QUAL_TEXT[q]) <= 28);
  Outlook w = b;
  w.state = OS_DAY_SPELLS, w.qual = OQ_SEVERE_FROST, w.warn = OW_FOG_POSSIBLE, w.min_lo = -99, w.min_hi = -99;
  char ws[300];
  CHECK(outlookState(ws, sizeof ws, w) > 0 && strlen(ws) <= 255);
}

// the baro_check notice from the station's LearnUse; silent with METAR on, under baro_uncalibrated, or once the
// bridge's offset in force brings it under 9 hPa
static void testLearnBaroNotice() {
  PosixTz tz;
  parsePosixTz("UTC0", &tz);
  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("L7N", defaultStationConfig());
  const auto find = [&](Notice* out) {
    const Notices ns = stationNotices(*st);
    for (int i = 0; i < ns.n; i++)
      if (ns.item[i].kind == NK_BARO_CHECK) return *out = ns.item[i], true;
    return false;
  };
  Notice x{};
  CHECK(!find(&x));  // no flag
  st->use.l7_flag = 2, st->use.l7_off = 250, st->use.l7_days = 30, st->use.l7_stage = 1;
  char t[NOTICE_TEXT_MAX];
  // baro_ref's sentence shape, the chip shared with baro_ref
  CHECK(find(&x) && noticeText(t, sizeof t, x, tz, 0) > 0 &&
        strcmp(t, "This station's pressure averaged about 25 hPa above the usual for this place over 30 days. Set REL in "
                  "the console's SET menu to the current sea-level pressure of a nearby official station.") == 0);
  CHECK(!strcmp(NOTICES[NK_BARO_CHECK].chip, NOTICES[NK_BARO_REF].chip) && strcmp(NOTICES[NK_BARO_CHECK].id, NOTICES[NK_BARO_REF].id));
  char at[200];
  Notices one = {};
  one.add(x.kind, 0, x.count);
  CHECK(noticesAttributes(at, sizeof at, one) > 0 && contains(at, "{\"id\":\"baro_check\",\"off_hpa\":25,\"days\":30}"));
  st->use.l7_flag = 1, st->use.l7_off = -130;
  CHECK(find(&x) && noticeText(t, sizeof t, x, tz, 0) > 0 && contains(t, "about 15 hPa below the usual for this place over 30 days"));
  // the days shown up to 60; the worst case (the largest offset the count holds, 60 days) fits NOTICE_TEXT_MAX
  Notice w{NK_BARO_CHECK, 0, baroCheckCount(-145, 400)};
  CHECK(noticeText(t, sizeof t, w, tz, 0) > 0 && contains(t, "about 145 hPa below the usual for this place over 60 days."));
  w.count = baroCheckCount(-0x7FFF, 0xFFFF);
  CHECK(noticeText(t, sizeof t, w, tz, 0) > 0 && strlen(t) < NOTICE_TEXT_MAX);
  // one pressure chip at most, baro_uncalibrated > baro_ref > baro_check
  const auto pressureChips = [&]() {
    const Notices ns = stationNotices(*st);
    int k = 0;
    for (int i = 0; i < ns.n; i++) k += ns.item[i].kind == NK_BARO_CHECK || ns.item[i].kind == NK_BARO_REF || ns.item[i].kind == NK_BARO_UNCAL;
    return k;
  };
  st->last_seen = 1769904000u;
  st->rel.active = true, st->rel.n = 3, st->rel.last_t = st->last_seen, st->rel.since = st->last_seen - 7200;
  st->rel.d[0] = st->rel.d[1] = st->rel.d[2] = 40, st->rel.ref_t = st->last_seen;
  {
    const Notices ns = stationNotices(*st);
    bool ref = false;
    for (int i = 0; i < ns.n; i++) ref = ref || ns.item[i].kind == NK_BARO_REF;
    CHECK(ref && !find(&x) && pressureChips() == 1);  // baro_ref outranks baro_check
  }
  st->baro_uncal = true;
  CHECK(!find(&x) && pressureChips() == 1);  // baro_uncalibrated outranks both
  st->baro_uncal = false;
  relReset(&st->rel);
  CHECK(find(&x) && pressureChips() == 1);  // the reference gone: baro_check speaks again
  st->use.metar = true;
  CHECK(!find(&x));
  st->use.metar = false;
  st->baro_uncal = true;
  CHECK(!find(&x));
  st->baro_uncal = false;
  st->cfg.pressure_hpa_offset = 5.0f;  // -13.0 + 5.0 = -8.0 hPa: under the exit line at once
  CHECK(!find(&x));
  st->cfg.pressure_hpa_offset = 0;
  st->use.l7_flag = 0;
  CHECK(!find(&x));
  // end to end with the built-in table (52 N 20 E, January): 20 days at 1046 hPa -> stage 1, flag high, the snapshot's
  // offset ~ +30 hPa against the normal; the notice with METAR off, none with METAR on
  static LearnState s;
  learnInit(&s);
  for (int i = 0; i < 20; i++) {
    DayRec d;
    clearDay(&d, dayNumberToYmd(20454 + 5 + i));  // 2026-01-06 ..
    d.quality = 0, d.hours = 24, d.epoch = 0, d.p_avg = 10460;
    learnDay(s, d, lctx());
  }
  s.c.last_hour = 1769904000u, s.c.flags = 0;  // a fed state (2026-02-01)
  LearnUse u;
  learnUseOf(s, 52, 20, 0, 0, &u);
  printf("  info: L7 end to end: flag %u, stage %u, off %.1f hPa over %u days\n", u.l7_flag, u.l7_stage, u.l7_off / 10.0,
         u.l7_days);
  CHECK(s.c.l7_flag == 2 && u.l7_flag == 2 && u.l7_stage == 1 && u.l7_off >= 250 && u.l7_off <= 350);
  CHECK(u.l7_days == 20);  // the admitted days, not n_eff (16.x)
  st->use = u;
  CHECK(find(&x) && noticeText(t, sizeof t, x, tz, 0) > 0 && contains(t, "hPa above the usual for this place over 20 days"));
  st->use.metar = true;
  CHECK(!find(&x));
  st->use.metar = false;
  // the JSON says the same (days, stage, flag, off, norm present); re-judged with the bridge's
  // offset in force and silent under baro_uncalibrated, as the notice
  const auto json = [&](int16_t poff, bool uncal) {
    static LearnView v;
    learnView(s, &v);
    LearnMeta m = {"learning", nullptr, 0, true, 0, 0, 0, 0, 1769904000u, 52, 20, 0, 0, 0, 0, 0, 0, nullptr, poff, uncal};
    std::string js;
    char b[96];
    Out o(toString, &js, b, sizeof b);
    learnJson(o, v, m);
    o.flush();
    return js;
  };
  std::string js = json(0, false);
  CHECK(contains(js, "\"days\":20,\"stage\":1,\"flag\":\"high\",\"uncal\":false,\"off\":") && !contains(js, "\"norm\":null"));
  st->cfg.pressure_hpa_offset = -30.0f;
  js = json(-300, false);  // the user fixed it in Settings: the sheet clears at once, as the notice (0.1 hPa)
  CHECK(contains(js, "\"flag\":\"none\",\"uncal\":false,\"off\":0.") && !find(&x));
  st->cfg.pressure_hpa_offset = 0;
  js = json(0, true);
  CHECK(contains(js, "\"flag\":\"none\",\"uncal\":true,\"off\":"));
}

// the learned day codes merged with the T3 overlay. Day words vs T3: day_sunny /
// day_spells beat T3, T3 beats day_nosun, dry and the gauge-blind null; the qualifier on every state x qualifier pair
// (day words keep it, T3 never); the single warning chain with fog_possible below fog_near, above mist_near
static void testLearnOutlookOverlay() {
  // ---- line 1: a June noon at 52 N 20 E, L1 in use (as testLearnGraduation), calm hours ("Calm.")
  const uint32_t dh = (uint32_t)unixFromUtc(2026, 6, 21, 10, 0, 0);
  OutHour dy[5];
  for (int i = 0; i < 5; i++) {
    dy[i] = OutHour{};
    dy[i].hour = dh - 3600u * (uint32_t)(4 - i), dy[i].p = 1015, dy[i].t = 20, dy[i].td = NAN, dy[i].wind = 2, dy[i].rain = 0;
    dy[i].solar = 600, dy[i].solar_max = 640, dy[i].rh = 60, dy[i].calm = true;
  }
  OutlookIn di = {};
  di.now = dh + 3600 + 600, di.time_ok = di.has_baro = true, di.lat = 52, di.lon = 20, di.h = dy, di.n = 5;
  di.open_rain = 0, di.t = 20, di.td = NAN, di.rate = di.rain10 = di.gust10 = di.solar = NAN, di.rh = 60, di.wind10 = NAN;
  LearnUse sk = {};
  sk.bits = LU_L1, sk.hour = dh, sk.sky_ref = 650, sk.sky_ref_max = 680, sk.sky_cls = 1;
  di.use = &sk;
  OutNear nr = {};
  nr.ok = true, nr.km = 18, nr.obs = di.now - 1500;
  memcpy(nr.icao, "KJFK", 5);
  const auto day = [&](float avg, float mx, uint8_t wx) {
    dy[4].solar = avg, dy[4].solar_max = mx;
    nr.wx = wx;
    di.near = wx ? nr : OutNear{};
    return outlook(di);
  };
  static const uint8_t T3W[3] = {NW_SNOW, NW_RAIN, NW_DRIZZLE};
  static const uint8_t T3S[3] = {OS_SNOW_NEAR, OS_RAIN_NEAR, OS_DRIZZLE_NEAR};
  CHECK(day(600, 640, 0).state == OS_DAY_SUNNY && day(400, 620, 0).state == OS_DAY_SPELLS && day(150, 250, 0).state == OS_DAY_NOSUN);
  for (int k = 0; k < 3; k++) {
    const Outlook su = day(600, 640, T3W[k]), sp = day(400, 620, T3W[k]), ns = day(150, 250, T3W[k]);
    CHECK(su.state == OS_DAY_SUNNY && su.qual == OQ_CALM && !su.near.ok && su.learned == LU_L1);  // sun here wins
    CHECK(sp.state == OS_DAY_SPELLS && sp.qual == OQ_CALM && !sp.near.ok && sp.learned == LU_L1);
    // "No sun." says nothing about precipitation: the report replaces it, L1 not used for a code
    CHECK(ns.state == T3S[k] && ns.qual == FC_NONE && ns.near.ok && ns.learned == 0 && contains(olJson(ns), "\"conf\":\"default\"") &&
          !contains(olJson(ns), "\"learn\":"));
    sk.sky_cls = 0;  // the sun cell unknown: "No rain." instead of "No sun.", replaced by the report too
    const Outlook dr = day(150, 250, T3W[k]);
    sk.sky_cls = 1;
    CHECK(dr.state == T3S[k] && dr.qual == FC_NONE && dr.near.ok);
    di.t = 1.5f;  // the gauge-blind null (T <= 2 °C) with "No sun." possible: replaced either way
    const Outlook bl = day(150, 250, T3W[k]);
    di.t = 20;
    CHECK(bl.state == T3S[k] && bl.qual == FC_NONE);
  }
  // the qualifier on every reachable state x qualifier pair of this pass: the day words keep each of the six, a T3
  // state none. Each qualifier set up on its own (the chain picks the first)
  const auto setQ = [&](int q) {
    di.t = 20, dy[4].t = 20, dy[4].wind = 2, di.wind10 = NAN, dy[4].calm = true, dy[4].rh = 60, di.rh = 60;
    if (q == OQ_SEVERE_FROST) di.t = -12;
    else if (q == OQ_FREEZING) di.t = -1;
    else if (q == OQ_HOT) di.t = 31, dy[4].t = 31;
    else if (q == OQ_WINDY) dy[4].wind = 25, di.wind10 = 20, dy[4].calm = false;
    else if (q == OQ_HUMID) dy[4].rh = 95, di.rh = 95, dy[4].calm = false;
  };
  int pairs = 0;
  for (int q = 0; q < OQ_COUNT; q++) {
    setQ(q);
    const Outlook su = day(600, 640, 0), sp = day(400, 620, 0), ns = day(150, 250, 0);
    CHECK(su.state == OS_DAY_SUNNY && su.qual == q && sp.state == OS_DAY_SPELLS && sp.qual == q && ns.state == OS_DAY_NOSUN &&
          ns.qual == q);
    pairs += 3;
    for (int k = 0; k < 3; k++) {
      const Outlook t = day(150, 250, T3W[k]);
      char st[300];
      CHECK(t.state == T3S[k] && t.qual == FC_NONE && outlookState(st, sizeof st, t) > 0 && !contains(st, OUT_QUAL_TEXT[q]));
      pairs++;
    }
  }
  setQ(OQ_CALM);
  CHECK(pairs == OQ_COUNT * 6);
  // the table over every state x qualifier: the flag is the 14-char rule, the day words carry it, the T3 states do not,
  // and every allowed pair fits line 1 (28)
  for (int s = 0; s < OS_COUNT; s++) {
    CHECK(OUT_STATE_QUAL[s] == (strlen(OUT_STATE_TEXT[s]) <= 14));
    for (int q = 0; q < OQ_COUNT; q++) CHECK(!OUT_STATE_QUAL[s] || strlen(OUT_STATE_TEXT[s]) + 1 + strlen(OUT_QUAL_TEXT[q]) <= 28);
  }
  CHECK(OUT_STATE_QUAL[OS_DAY_SUNNY] && OUT_STATE_QUAL[OS_DAY_SPELLS] && OUT_STATE_QUAL[OS_DAY_NOSUN] &&
        !OUT_STATE_QUAL[OS_SNOW_NEAR] && !OUT_STATE_QUAL[OS_RAIN_NEAR] && !OUT_STATE_QUAL[OS_DRIZZLE_NEAR]);

  // ---- the warning chain: icy > thunder_near > gusts > hoar_frost > ground_frost > fog_near > fog_possible > mist_near
  CHECK(outWarnRank(OW_ICY) < outWarnRank(OW_THUNDER_NEAR) && outWarnRank(OW_THUNDER_NEAR) < outWarnRank(OW_GUSTS) &&
        outWarnRank(OW_GUSTS) < outWarnRank(OW_HOAR_FROST) && outWarnRank(OW_HOAR_FROST) < outWarnRank(OW_GROUND_FROST) &&
        outWarnRank(OW_GROUND_FROST) < outWarnRank(OW_FOG_NEAR) && outWarnRank(OW_FOG_NEAR) < outWarnRank(OW_FOG_POSSIBLE) &&
        outWarnRank(OW_FOG_POSSIBLE) < outWarnRank(OW_MIST_NEAR) && outWarnRank(OW_MIST_NEAR) < outWarnRank(FC_NONE));
  for (int w = 0; w < OW_COUNT; w++) CHECK(outWarnRank((uint8_t)w) < 99);  // every warning has its place
  // an evening at 52 N 20 E, L2 in use: fog possible (as testLearnGraduation)
  const uint32_t base = (uint32_t)unixFromUtc(2026, 11, 6, 11, 0, 0);
  OutHour h[8];
  for (int i = 0; i < 8; i++) {
    h[i] = OutHour{};
    h[i].hour = base + 3600u * (uint32_t)i, h[i].p = 1020.0, h[i].t = 5, h[i].td = NAN, h[i].wind = 2, h[i].rain = 0;
    h[i].solar = NAN, h[i].calm = true, h[i].rh = 96, h[i].solar_max = NAN;
  }
  OutlookIn in = {};
  in.now = base + 7 * 3600 + 600, in.time_ok = in.has_baro = true, in.lat = 52, in.lon = 20, in.h = h, in.n = 7;
  in.open_rain = 0, in.t = 5, in.td = NAN, in.rate = in.rain10 = in.gust10 = in.solar = NAN, in.rh = 95.5f, in.wind10 = NAN;
  LearnUse f = {};
  f.bits = LU_L2, f.rh_sat = 975, f.hour = h[6].hour;
  in.use = &f;
  nr.obs = in.now - 1500;
  const auto ev = [&](uint8_t wx) {
    nr.wx = wx;
    in.near = wx ? nr : OutNear{};
    return outlook(in);
  };
  const Outlook g0 = ev(0);
  CHECK(g0.warn == OW_FOG_POSSIBLE && g0.learned == LU_L2 && g0.state == OS_DRY);
  // the report wins (same phenomenon, observed): its values only, L2 not used
  const Outlook fn = ev(NW_FOG);
  CHECK(fn.warn == OW_FOG_NEAR && fn.near.ok && fn.learned == 0 && std::isnan(fn.rh_sat) && std::isnan(fn.rh_rel) &&
        !contains(olJson(fn), "rh_sat"));
  const Outlook mn = ev(NW_MIST);  // "Mist or fog possible." already covers mist
  CHECK(mn.warn == OW_FOG_POSSIBLE && !mn.near.ok && mn.learned == LU_L2 && contains(olJson(mn), "\"rh_sat\":97.5"));
  CHECK(ev(NW_THUNDER).warn == OW_THUNDER_NEAR && std::isnan(ev(NW_THUNDER).rh_sat));
  // a T3 state never silences fog_possible (it was judged on our own state): drizzle nearby + fog possible
  const Outlook dz = ev(NW_DRIZZLE);
  CHECK(dz.state == OS_DRIZZLE_NEAR && dz.warn == OW_FOG_POSSIBLE && dz.learned == LU_L2 && dz.near.ok);
  CHECK(ev(NW_DRIZZLE | NW_FOG).warn == OW_FOG_NEAR && ev(NW_DRIZZLE | NW_MIST).warn == OW_FOG_POSSIBLE);
  // frost (forms here) above both fog codes: a clear night cooling 1.2 °C/h (clear by the fixed rule)
  for (int i = 0; i < 8; i++) h[i].t = 8.0f - 1.2f * (float)i;
  in.t = 1.0f, in.td = -5;
  CHECK(ev(0).warn == OW_GROUND_FROST && ev(NW_FOG).warn == OW_GROUND_FROST && ev(NW_MIST).warn == OW_GROUND_FROST);
  in.t = -1.0f, in.td = -2.5f;
  CHECK(ev(0).warn == OW_HOAR_FROST && ev(NW_FOG).warn == OW_HOAR_FROST);
  CHECK(ev(NW_THUNDER).warn == OW_THUNDER_NEAR);  // lightning above frost
  in.gust10 = 45;
  CHECK(ev(NW_FOG).warn == OW_GUSTS && ev(NW_THUNDER).warn == OW_THUNDER_NEAR);
  in.gust10 = NAN;
  in.t = 3.0f, in.td = -5;  // above frost: fog_possible on the clear night, the report above it, mist below
  CHECK(ev(0).warn == OW_FOG_POSSIBLE && ev(NW_FOG).warn == OW_FOG_NEAR && ev(NW_MIST).warn == OW_FOG_POSSIBLE);
}

static void testLearnLearners() {
  testLearnBaroNotice();
  testLearnBars();
  testLearnNoisyTrials();
  testLearnL5Label();
  testLearnL2MeltTips();
  testLearnL7();
  testLearnV3FrozenStep();
  testLearnSeeding();
  testLearnRestart();
  testLearnV3Migrate();
  testLearnUseOf();
  testLearnGraduation();
  testLearnOutlookOverlay();
}
