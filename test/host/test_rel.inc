// test_rel.inc -- the REL hint.
// The pure run (enter on the 3rd same-sign report, a sign flip resets, hysteresis, exit under 1.5, the 3-h silence, the
// median, the target), the reference side (inconsistent / far / old / no location / module off), the station side
// (tendency, factory value, the ring window), the notice (precedence under baro_uncalibrated, both endings, params),
// the last good value, /api/metar's reference.station and cur, and the module-off consistency. Airports EGLL / KJFK only.

namespace {
// one upload every 65 s from t0 for `mins` minutes; baromin as given (inHg), or ramped from p0 to p1 over the span
void relFeed(Station* st, uint32_t t0, int mins, float p0, float p1, const PosixTz& tz) {
  const int n = mins * 60 / 65;
  for (int i = 0; i < n; i++) {
    char q[96];
    const float p = p0 + (p1 - p0) * (float)i / (float)(n > 1 ? n - 1 : 1);
    snprintf(q, sizeof q, "ID=RL1&baromin=%.2f&tempf=%.1f&humidity=80&windspeedmph=%d", p, 50.0 + (i % 7) * 0.1, 2 + i % 5);
    Stations::ingest(*st, sampleFrom(q), t0 + 65u * (uint32_t)i, 1000 + 65000u * (uint32_t)i, 0, SrcIf::Ap, true, tz);
  }
}
}  // namespace

static void testRelCheck() {
  const uint32_t t = (uint32_t)unixFromUtc(2026, 11, 16, 12, 0, 0);
  // ---- the pure run
  RelState r{};
  relStep(&r, 25, 10132, t);
  relStep(&r, 24, 10132, t + 1800);
  CHECK(!r.active && r.n == 2 && r.since == t && !relShown(r, t + 1800));
  relStep(&r, 24, 10132, t + 1800);  // the same report again: not a step
  CHECK(r.n == 2);
  relStep(&r, 26, 10133, t + 3600);  // the 3rd same-sign report enters
  CHECK(r.active && r.n == 3 && r.since == t && relMedian(r) == 25 && relShown(r, t + 3700));
  relStep(&r, 17, 10133, t + 5400);  // 1.5 .. 2.0 holds an active run
  CHECK(r.active && relShown(r, t + 5400));
  relStep(&r, -25, 10133, t + 7200);  // a sign flip resets (and starts a new run)
  CHECK(!r.active && r.n == 1 && r.sign == -1 && r.since == t + 7200);
  relStep(&r, -17, 10133, t + 9000);  // an inactive run is broken by a value in the band
  CHECK(!r.active && r.n == 0);
  for (int i = 0; i < 3; i++) relStep(&r, -30, 10133, t + 10800 + 1800u * (uint32_t)i);
  CHECK(r.active && relMedian(r) == -30);
  relStep(&r, 10, 10133, t + 16200);  // the first report under 1.5 clears
  CHECK(!r.active && r.n == 0 && !relShown(r, t + 16200));
  // the median of the last 3 (the run's values only), and of 2
  RelState m{};
  relStep(&m, 20, 10130, t), relStep(&m, 40, 10130, t + 1), relStep(&m, 30, 10130, t + 2);
  CHECK(relMedian(m) == 30);
  // the 3-h silence: relShown says no at once, relExpire clears; a report at the edge still counts
  CHECK(relShown(m, t + 2 + REL_SILENCE_S) && !relShown(m, t + 3 + REL_SILENCE_S));
  relExpire(&m, t + 2 + REL_SILENCE_S);
  CHECK(m.active);
  relExpire(&m, t + 3 + REL_SILENCE_S);
  CHECK(!m.active && m.n == 0 && m.seen_t == t + 2);  // seen_t kept: the old report is not stepped again
  // the target: QNH - the bridge offset, only while the report is current (75 min), rounded to 0.1
  RelState g{};
  relStep(&g, 30, 10132, t);
  CHECK(relTarget(g, 1.5f, t + 600) == 10117 && relTarget(g, -0.04f, t) == 10132 && relTarget(g, 0, t + METAR_CURRENT_S) == 10132);
  CHECK(relTarget(g, 0, t + METAR_CURRENT_S + 1) == REL_NONE && relTarget(g, 0, t - 1) == REL_NONE);
  CHECK(relSig(g) == 0);  // inactive: no ETag word
  CHECK(REL_HINT_DHPA == 20 && REL_EXIT_DHPA == 15 && REL_HINT_N == 3 && REL_SAMPLE_S == 3600);

  // ---- the reference side
  MetarCfg c = t3Cfg(2);
  MetarStore s;
  metarStoreClear(&s);
  s.obs[0] = t3Obs("EGLL", t - 600, 0);
  s.obs[1] = t3Obs("KJFK", t - 300, 0);
  int16_t q = 0;
  uint32_t rt = 0;
  CHECK(relReference(c, s, 52, 20, t, REL_SAMPLE_S, &q, &rt) && q == 10132 && rt == t - 300);
  s.obs[1].qnh_dhpa = 10150;  // 1.8 hPa apart: inconsistent -> not admitted
  CHECK(!relReference(c, s, 52, 20, t, REL_SAMPLE_S, &q, &rt));
  s.obs[1].qnh_dhpa = 10132;
  CHECK(!relReference(c, s, NAN, NAN, t, REL_SAMPLE_S, &q, &rt));  // no location
  {
    MetarCfg off = c;
    off.on = 0;
    CHECK(!relReference(off, s, 52, 20, t, REL_SAMPLE_S, &q, &rt));
    MetarCfg far = c;
    far.a[1].lon = 2300;  // #2 is ~200 km away: the reference is the near one alone, admitted
    s.obs[1].qnh_dhpa = 10140;
    CHECK(relReference(far, s, 52, 20, t, REL_SAMPLE_S, &q, &rt) && q == 10132 && rt == t - 600);
    CHECK(metarEligible(far, 52, 20));  // the same case metarEligible calls eligible: baro_ref may speak
    s.obs[1].qnh_dhpa = 10132;
    far.a[0].lon = 2300;  // both far: out
    CHECK(!relReference(far, s, 52, 20, t, REL_SAMPLE_S, &q, &rt));
    s.obs[1].obs_unix = t - REL_SAMPLE_S - 1;  // the near #2 too old for a sample: #1 alone
    CHECK(relReference(c, s, 52, 20, t, REL_SAMPLE_S, &q, &rt) && rt == t - 600);
    s.obs[1].obs_unix = t - 300;
  }
  s.obs[0].obs_unix = s.obs[1].obs_unix = t - REL_SAMPLE_S - 1;  // the newest > 60 min: out (internal age)
  CHECK(!relReference(c, s, 52, 20, t, REL_SAMPLE_S, &q, &rt) && relReference(c, s, 52, 20, t, METAR_STALE_S, &q, &rt));

  // ---- the station side and the notice
  PosixTz tz;
  CHECK(parsePosixTz("UTC0", &tz));
  static Stations all;
  all.reserve(MAX_STATIONS);
  Station* st = all.findOrAdd("RL1", defaultStationConfig());
  const uint32_t t0 = t - 5 * 3600;
  relFeed(st, t0, 300, 30.00f, 30.00f, tz);  // 5 h steady at 1015.9 hPa
  const uint32_t ref = st->last_seen - 240;
  float p = 0;
  CHECK(relStationValue(*st, 52, 20, ref, &p) && std::fabs(p - 1015.9f) < 0.06f);
  CHECK(!relStationValue(*st, 52, 20, ref - 7200, &p));  // the ring has nothing at the report's time
  // the job sees a report 14-15 min old (the :05/:20/:35/:50 grid) -- newer uploads are skipped, not a
  // stop; a report 55 min old is still inside the ring, 70 min old is past its edge
  CHECK(relStationValue(*st, 52, 20, st->last_seen - 900, &p) && std::fabs(p - 1015.9f) < 0.06f);
  CHECK(relStationValue(*st, 52, 20, st->last_seen - 3300, &p));
  CHECK(!relStationValue(*st, 52, 20, st->last_seen - 4200, &p));
  relStationValue(*st, 52, 20, ref, &p);
  CHECK(st->good_t == st->last_seen && std::fabs(st->good_p - 1015.9f) < 0.06f);
  // three admitted reports -> baro_ref with the median, since, the target
  for (int i = 0; i < 3; i++) relStep(&st->rel, (int16_t)lroundf(p * 10 - 10132), 10132, ref - 1800u * (uint32_t)(2 - i));
  Notices ns = stationNotices(*st);
  CHECK(ns.n == 1 && ns.item[0].kind == NK_BARO_REF && ns.item[0].a == 27 && ns.item[0].b == 10132 &&
        ns.item[0].since == ref - 3600 && ns.item[0].count == ref);
  char txt[NOTICE_TEXT_MAX];
  CHECK(noticeText(txt, sizeof txt, ns.item[0], tz, 0) > 0);
  char want[NOTICE_TEXT_MAX];
  {
    const uint32_t sod = (ref - 3600) % 86400;
    snprintf(want, sizeof want, "This station's pressure reads about 2.7 hPa above nearby airports since %02u:%02u. Set REL in the console's SET menu to about 1013.2 hPa.",
             (unsigned)(sod / 3600), (unsigned)(sod % 3600 / 60));
  }
  CHECK(strcmp(txt, want) == 0);
  CHECK(strcmp(NOTICES[NK_BARO_REF].chip, "Pressure looks off") == 0 && strcmp(NOTICES[NK_BARO_REF].id, "baro_ref") == 0);
  char at[256];
  CHECK(noticesAttributes(at, sizeof at, ns) > 0 && strstr(at, "{\"id\":\"baro_ref\",\"since\":\"") &&
        strstr(at, ",\"d_hpa\":2.7,\"qnh_hpa\":1013.2,\"target_hpa\":1013.2,\"ref_t\":\""));  // qnh_hpa
  CHECK(ns.item[0].q == 10132);
  // the offset moves the number to type, not d (the station as shown already carries it)
  st->cfg.pressure_hpa_offset = 0.5f;
  CHECK(stationNotices(*st).item[0].b == 10127);
  st->cfg.pressure_hpa_offset = 0;
  // the reference grows old (> 75 min at the last upload): no number, the rel_off_old ending
  {
    static Station old;  // static: a Station is too big for the stack
    old = *st;
    old.last_seen = ref + METAR_CURRENT_S + 60;
    const Notices o2 = stationNotices(old);
    CHECK(o2.n == 1 && o2.item[0].b == REL_NONE && noticeText(txt, sizeof txt, o2.item[0], tz, 0) > 0 &&
          strstr(txt, "to the current sea-level pressure of a nearby official station.") && !strstr(txt, "to about"));
    CHECK(noticesAttributes(at, sizeof at, o2) > 0 && strstr(at, "\"target_hpa\":null"));
    old.last_seen = st->rel.last_t + REL_SILENCE_S + 1;  // silence: gone
    CHECK(stationNotices(old).n == 0);
  }
  // the ETag word moves with the run
  const uint32_t e1 = outlookEtagIn(*st, 52, 20, true);
  CHECK(relSig(st->rel) != 0);
  {
    static Station b;
    b = *st;
    relReset(&b.rel);
    CHECK(outlookEtagIn(b, 52, 20, true) != e1);
  }
  // a fast tendency (a front between us and the airport): the station side says no
  {
    static Stations two;  // its own set: a one-station profile has a single slot
    two.reserve(MAX_STATIONS);
    Station* fs = two.findOrAdd("RL2", defaultStationConfig());
    CHECK(fs != nullptr);
    if (!fs) return;
    relFeed(fs, t0, 300, 29.80f, 30.20f, tz);  // ~13.5 hPa in 5 h
    CHECK(!relStationValue(*fs, 52, 20, fs->last_seen - 240, &p));
  }
  // ---- baro_uncalibrated outranks baro_ref (one notice, never two), with the last good value <= 3 h
  relFeed(st, st->last_seen + 65, 70, 29.92f, 29.92f, tz);  // the factory value for > BARO_UNCAL_N uploads
  CHECK(st->baro_uncal && !st->rel.active);
  ns = stationNotices(*st);
  CHECK(ns.n == 1 && ns.item[0].kind == NK_BARO_UNCAL && std::abs(ns.item[0].a - 10159) <= 1 && ns.item[0].b == ns.item[0].a);
  CHECK(noticesAttributes(at, sizeof at, ns) > 0 && strstr(at, "\"last_hpa\":1015.9") && strstr(at, "\"last_t\":\""));
  CHECK(noticeText(txt, sizeof txt, ns.item[0], tz, 0) > 0 && strcmp(txt, "The console's pressure is at its factory value; set REL once in its SET menu.") == 0);
  relStep(&st->rel, 30, 10132, st->last_seen + 1);  // even a stepped run stays silent under the factory value
  relStep(&st->rel, 30, 10132, st->last_seen + 2);
  relStep(&st->rel, 30, 10132, st->last_seen + 3);
  CHECK(stationNotices(*st).n == 1 && stationNotices(*st).item[0].kind == NK_BARO_UNCAL);
  relReset(&st->rel);
  {
    static Station late;  // the last good value older than 3 h: no number
    late = *st;
    late.last_seen = late.good_t + REL_LAST_GOOD_S + 1;
    const Notices o3 = stationNotices(late);
    CHECK(o3.n == 1 && o3.item[0].a == REL_NONE && noticesAttributes(at, sizeof at, o3) > 0 && !strstr(at, "last_hpa"));
    // after a reboot (good_t 0): the newest closed hour without the factory mark and not at the factory value
    static Station rb;
    rb = *st;
    rb.good_t = 0;
    float gp = 0;
    uint32_t gt = 0;
    CHECK(!lastGoodPressure(rb, &gp, &gt));  // the per-second path never walks the hours
    CHECK(lastGoodFromHours(rb, &gp, &gt) && std::fabs(gp - 1015.9f) < 0.06f && gt % 3600 == 0 && gt <= rb.last_seen);
  }

  // ---- /api/metar: cur per airport, reference.station, module off
  {
    MetarStore ms;
    metarStoreClear(&ms);
    const uint32_t now = t;
    ms.obs[0] = t3Obs("EGLL", now - 600, 0);
    ms.obs[1] = t3Obs("KJFK", now - METAR_CURRENT_S - 60, 0);  // shown (amber), not current
    MetarStationView sv{true, 1015.9f, 0.5f, true, 52, 20};
    std::string js;
    char buf[64];
    {
      Out o(toString, &js, buf, sizeof buf);
      metarJson(o, c, ms, true, now, &sv);
      o.flush();
    }
    CHECK(js.find("\"icao\":\"EGLL\"") != std::string::npos && js.find("\"stale\":false,\"cur\":true") != std::string::npos);
    CHECK(js.find("\"stale\":false,\"cur\":false") != std::string::npos);
    CHECK(js.find("\"near_qnh_hpa\":1013.2,\"station\":{\"p_hpa\":1015.9,\"qnh_hpa\":1013.2,\"diff_hpa\":2.7,\"target_hpa\":1012.7,\"active\":true}") != std::string::npos);
    // one near + one far airport -- the card's qnh_hpa weighs both, near_qnh_hpa and the comparison the
    // near one only
    {
      MetarCfg nf = c;
      nf.a[1].lon = 2300;
      MetarStore m2 = ms;
      m2.obs[1] = t3Obs("KJFK", now - 300, 0);
      m2.obs[1].qnh_dhpa = 10142;
      std::string j2;
      {
        Out o(toString, &j2, buf, sizeof buf);
        metarJson(o, nf, m2, true, now, &sv);
        o.flush();
      }
      CHECK(j2.find("\"reference\":{\"qnh_hpa\":1013.6") != std::string::npos);  // (3 x 1013.2 + 2 x 1014.2) / 5
      CHECK(j2.find("\"near_qnh_hpa\":1013.2,\"station\":{\"p_hpa\":1015.9,\"qnh_hpa\":1013.2,\"diff_hpa\":2.7") != std::string::npos);
      std::string j3;
      {
        Out o(toString, &j3, buf, sizeof buf);
        const MetarStationView nn{false, 0, 0, false, 52, 20};  // a location, no station reading
        metarJson(o, nf, m2, true, now, &nn);
        o.flush();
      }
      CHECK(j3.find("\"near_qnh_hpa\":1013.2,\"station\":null") != std::string::npos);  // no station: the near QNH still there
    }
    MetarStationView none{};
    js.clear();
    {
      Out o(toString, &js, buf, sizeof buf);
      metarJson(o, c, ms, true, now, &none);
      o.flush();
    }
    CHECK(js.find("\"station\":null") != std::string::npos);
    // module off: the reference has no station line, and the outlook's near word is the null word at the same moment
    MetarCfg off = c;
    off.on = 0;
    js.clear();
    {
      Out o(toString, &js, buf, sizeof buf);
      metarJson(o, off, ms, false, now, &sv);
      o.flush();
    }
    CHECK(js.find("\"on\":false") != std::string::npos && js.find("\"p_hpa\"") == std::string::npos);
    const OutNear nOff = nearFromStore(off, ms, 52, 20, now);
    CHECK(!nOff.ok && outlookNearWord(&nOff) == 0);
  }
}
