// test_t3.inc -- the main airport's
// report on our strip ("reported nearby"). Admission (nearFromStore), mapping, local first, the one warning chain, no
// qualifier under a T3 state, the stale gate, the JSON / HA keys and the ETag word. Airports KJFK / EGLL / ZZZZ only.

namespace {
// a configured module with one or two airports at a generic point 52.00 N 20.00 E (the bridge's location in the tests)
MetarCfg t3Cfg(int n) {
  MetarCfg c;
  metarCfgDefault(&c);
  c.on = 1;
  c.n = (uint8_t)n;
  const char* ids[2] = {"EGLL", "KJFK"};
  for (int i = 0; i < n && i < 2; i++) {
    memset(&c.a[i], 0, sizeof c.a[i]);
    memcpy(c.a[i].icao, ids[i], 5);
    snprintf(c.a[i].name, sizeof c.a[i].name, "%s", i ? "Kennedy" : "Heathrow");
    c.a[i].lat = 5200 + (int16_t)(i * 10), c.a[i].lon = 2030;  // ~20 km east of the location
    c.a[i].km = 20;
  }
  return c;
}
MetarObs t3Obs(const char* icao, uint32_t at, uint8_t wx, uint8_t flags = 0) {
  MetarObs o;
  memset(&o, 0, sizeof o);
  memcpy(o.icao, icao, 4);
  o.flags = (uint8_t)(MO_VALID | flags);
  o.obs_unix = at;
  o.qnh_dhpa = 10132;
  o.wx = wx;
  return o;
}
}  // namespace

static void testT3() {
  const uint32_t now = (uint32_t)unixFromUtc(2026, 11, 16, 12, 0, 0);
  MetarCfg c = t3Cfg(2);
  MetarStore s;
  metarStoreClear(&s);
  // ---- admission: age 0 / 75 / 76 min, a future report, no location, module off, #2 current while #1 stale
  s.obs[0] = t3Obs("EGLL", now, MW_PRECIP | MW_DRIZZLE);
  CHECK(nearFromStore(c, s, 52, 20, now).ok && nearFromStore(c, s, 52, 20, now).wx == NW_DRIZZLE);
  s.obs[0].obs_unix = now - METAR_CURRENT_S;
  CHECK(nearFromStore(c, s, 52, 20, now).ok && METAR_CURRENT_S == 4500);
  s.obs[0].obs_unix = now - METAR_CURRENT_S - 60;
  CHECK(!nearFromStore(c, s, 52, 20, now).ok);
  s.obs[0].obs_unix = now + 60;  // a report from the future (a clock step): not admitted
  CHECK(!nearFromStore(c, s, 52, 20, now).ok);
  s.obs[0].obs_unix = now - 600;
  CHECK(!nearFromStore(c, s, NAN, NAN, now).ok);  // no location: "nearby" cannot be checked
  MetarCfg off = c;
  off.on = 0;
  CHECK(!nearFromStore(off, s, 52, 20, now).ok);
  s.obs[1] = t3Obs("KJFK", now - 300, MW_PRECIP | MW_RAIN);
  s.obs[0].obs_unix = now - METAR_STALE_S;  // #1 too old, #2 current: silence, never #2
  CHECK(!nearFromStore(c, s, 52, 20, now).ok);
  s.obs[0].obs_unix = now - 600;
  // distance by geoKm from the stored lat/lon, never the pick-time km: 59 / 60 / 61 km
  {
    int okKm = -1, failKm = -1;
    for (int dlon = 0; dlon < 200; dlon++) {
      MetarCfg d = c;
      d.a[0].lon = (int16_t)(2000 + dlon);
      d.a[0].km = 1;  // stale pick-time km: ignored
      const uint32_t km = geoKm(d.a[0].lat, d.a[0].lon, 5200, 2000);
      const bool ok = nearFromStore(d, s, 52, 20, now).ok;
      if (km <= METAR_NEAR_KM && ok) okKm = (int)km;
      if (km > METAR_NEAR_KM && !ok && failKm < 0) failKm = (int)km;
      CHECK(ok == (km <= METAR_NEAR_KM));
    }
    CHECK(okKm >= 59 && okKm <= 60 && failKm >= 61 && failKm <= 62);
  }
  CHECK(metarEligible(c, 52, 20) && !metarEligible(c, NAN, NAN) && !metarEligible(off, 52, 20));
  {
    MetarCfg far = c;
    far.a[0].lon = far.a[1].lon = 2300;  // ~200 km
    CHECK(!metarEligible(far, 52, 20));
  }
  // ---- mapping: RASN -> snow, -DZ / FZDZ -> drizzle, SHRA / FZRA / TSRA -> rain, PL / GR / IC alone -> none,
  // BCFG / FZFG -> fog (MW_FOG), BR -> mist, BR -DZ -> drizzle + mist, TS -> thunder
  const struct {
    uint8_t mw, nw;
  } map[] = {
      {MW_PRECIP | MW_RAIN | MW_SNOW, NW_SNOW},           {MW_PRECIP | MW_DRIZZLE, NW_DRIZZLE},
      {MW_PRECIP | MW_DRIZZLE | MW_FREEZING, NW_DRIZZLE}, {MW_PRECIP | MW_RAIN, NW_RAIN},
      {MW_PRECIP | MW_RAIN | MW_FREEZING, NW_RAIN},       {MW_PRECIP | MW_RAIN | MW_THUNDER, NW_RAIN | NW_THUNDER},
      {MW_PRECIP, 0},                                     {MW_FOG, NW_FOG},
      {MW_FOG | MW_FREEZING, NW_FOG},                     {MW_MIST, NW_MIST},
      {MW_MIST | MW_PRECIP | MW_DRIZZLE, NW_DRIZZLE | NW_MIST}, {MW_FOG | MW_MIST, NW_FOG},
      {MW_PRECIP | MW_RAIN | MW_DRIZZLE, NW_RAIN},        {MW_THUNDER, NW_THUNDER},
  };
  bool mapOk = true;
  for (const auto& m : map) {
    s.obs[0].wx = m.mw;
    const OutNear n = nearFromStore(c, s, 52, 20, now);
    mapOk = mapOk && n.ok && n.wx == m.nw;
  }
  CHECK(mapOk);
  s.obs[0].flags |= MO_AUTO;
  CHECK(nearFromStore(c, s, 52, 20, now).autom && strcmp(nearFromStore(c, s, 52, 20, now).icao, "EGLL") == 0);

  // ---- the overlay on our outlook
  OutHour h[6];
  for (int i = 0; i < 6; i++) h[i] = oh(now - 6 * 3600u + (uint32_t)i * 3600u, 8, 1013.0);  // a quiet day, 8 °C
  OutNear nr = {};
  nr.ok = true, nr.km = 18, nr.obs = now - 1500;
  memcpy(nr.icao, "EGLL", 5);
  snprintf(nr.name, sizeof nr.name, "Heathrow");
  const auto run = [&](uint8_t wx, float t, float rate, float gust, bool stale = false) {
    OutlookIn in = oin(h, 6, now);
    in.t = t, in.rate = rate, in.gust10 = gust, in.stale = stale;
    nr.wx = wx;
    in.near = nr;
    return outlook(in);
  };
  // dry replaced; the gauge-blind null (T <= 2) replaced; snow > rain > drizzle
  CHECK(run(NW_DRIZZLE, 8, NAN, NAN).state == OS_DRIZZLE_NEAR && run(NW_RAIN, 8, NAN, NAN).state == OS_RAIN_NEAR);
  CHECK(run(NW_SNOW, 1.5f, NAN, NAN).state == OS_SNOW_NEAR);
  CHECK(run(0, 8, NAN, NAN).state == OS_DRY && !run(0, 8, NAN, NAN).near.ok);  // a report without these groups: silence
  // local first: our gauge's rain keeps the slot
  {
    const Outlook a = run(NW_SNOW, 8, 1.0f, NAN);
    CHECK(a.state == OS_RAIN_LIGHT && !a.near.ok);
  }
  // our gauge at <= 1.0 °C reads sleet_snow and keeps the slot over the airport's snow, as rain_* does
  {
    const Outlook a = run(NW_SNOW, 1.0f, 1.0f, NAN), b = run(NW_SNOW, 1.1f, 1.0f, NAN), d = run(0, 0.5f, 5.0f, NAN);
    CHECK(a.state == OS_SLEET_SNOW && !a.near.ok && b.state == OS_RAIN_LIGHT && !b.near.ok);
    CHECK(d.state == OS_SLEET_SNOW && d.qual == FC_NONE);  // the airport's snow is not required
  }
  // no qualifier under a T3 state; a local state keeps it ("Freezing." at -1 °C with the gauge-blind null)
  {
    const Outlook a = run(NW_DRIZZLE, -1.0f, NAN, NAN);
    CHECK(a.state == OS_DRIZZLE_NEAR && a.qual == FC_NONE);
    const Outlook b = run(0, -1.0f, NAN, NAN);
    CHECK(b.state == FC_NONE && b.qual == OQ_FREEZING);
  }
  for (int st = 0; st < OS_COUNT; st++) CHECK(OUT_STATE_QUAL[st] == (strlen(OUT_STATE_TEXT[st]) <= 14));
  // the warning chain, each adjacent pair: icy > thunder_near > gusts > hoar_frost > ground_frost > fog_near > mist_near
  {
    CHECK(run(NW_THUNDER | NW_RAIN, 0.5f, NAN, NAN).warn == OW_THUNDER_NEAR);  // icy needs rain here: none -> thunder
    CHECK(run(NW_THUNDER, 0.5f, 1.0f, NAN).warn == OW_ICY);                     // icy (our rain at <= 1 °C) beats thunder
    const Outlook g = run(NW_THUNDER, 8, NAN, 45);
    CHECK(g.warn == OW_THUNDER_NEAR && std::isnan(g.gust));                      // thunder beats gusts (the gust dropped)
    const Outlook f = run(NW_FOG, 8, NAN, 45);
    CHECK(f.warn == OW_GUSTS && f.gust > 44);                                    // gusts beat fog_near
    CHECK(run(NW_FOG, 8, NAN, NAN).warn == OW_FOG_NEAR && run(NW_MIST, 8, NAN, NAN).warn == OW_MIST_NEAR);
    CHECK(outWarnRank(OW_ICY) < outWarnRank(OW_THUNDER_NEAR) && outWarnRank(OW_THUNDER_NEAR) < outWarnRank(OW_GUSTS) &&
          outWarnRank(OW_GUSTS) < outWarnRank(OW_HOAR_FROST) && outWarnRank(OW_HOAR_FROST) < outWarnRank(OW_GROUND_FROST) &&
          outWarnRank(OW_GROUND_FROST) < outWarnRank(OW_FOG_NEAR) && outWarnRank(OW_FOG_NEAR) < outWarnRank(OW_MIST_NEAR) &&
          outWarnRank(OW_MIST_NEAR) < outWarnRank(FC_NONE));
  }
  // the stale gate nulls state and warn, T3 too; the other gates leave no T3
  {
    const Outlook a = run(NW_DRIZZLE | NW_FOG, 8, NAN, NAN, true);
    CHECK(a.gate == OG_STALE && a.state == FC_NONE && a.warn == FC_NONE && !a.near.ok);
    OutlookIn in = oin(h, 6, now);
    in.near = nr, in.near.wx = NW_DRIZZLE, in.time_ok = false;
    CHECK(outlook(in).gate == OG_TIME && outlook(in).state == FC_NONE);
    in.time_ok = true, in.has_baro = false;
    for (int i = 0; i < 6; i++) h[i].p = NAN;
    CHECK(!outlook(in).present);
    for (int i = 0; i < 6; i++) h[i].p = 1013.0;
  }
  // ---- JSON, HA state and attributes, the signature, the ETag word
  {
    const Outlook a = run(NW_DRIZZLE | NW_THUNDER, 8, NAN, NAN);
    std::string js;
    char ob[128];
    Out o(toString, &js, ob, sizeof ob);
    outlookJson(o, a);
    o.flush();
    CHECK(jsonParses(js) && js.find("\"state\":\"wx_drizzle_near\"") != std::string::npos &&
          js.find("\"warn\":\"thunder_near\"") != std::string::npos &&
          js.find("\"near\":{\"icao\":\"EGLL\",\"name\":\"Heathrow\",\"km\":18,\"age_s\":1500,\"auto\":false}") != std::string::npos);
    char st[256];
    CHECK(outlookState(st, sizeof st, a) > 0 && strstr(st, "Drizzle reported nearby.") && strstr(st, "Thunder reported nearby."));
    char at[256];
    CHECK(outlookAttributes(at, sizeof at, a) > 0 && strstr(at, "\"near\":\"EGLL\"") && jsonParses(at));
    const Outlook b = run(NW_DRIZZLE | NW_THUNDER, 8, NAN, NAN);
    OutNear other = nr;
    memcpy(other.icao, "KJFK", 5);
    OutlookIn in = oin(h, 6, now);
    in.near = other, in.near.wx = NW_DRIZZLE | NW_THUNDER;
    CHECK(outlookSig(a) == outlookSig(b) && outlookSig(outlook(in)) != outlookSig(a));  // the source moves the signature
    const Outlook q = run(0, 8, NAN, NAN);
    std::string jq;
    Out o2(toString, &jq, ob, sizeof ob);
    outlookJson(o2, q);
    o2.flush();
    CHECK(jq.find("\"near\":null") != std::string::npos);
    CHECK(outlookAttributes(at, sizeof at, q) > 0 && strstr(at, "\"near\":null"));
    // the longest HA state and the widest attributes fit
    OutlookIn w = oin(h, 6, now);
    w.near = nr, w.near.wx = NW_DRIZZLE | NW_THUNDER;
    Outlook x = outlook(w);
    x.next = ON_P_FALLING_FAST;
    CHECK(outlookState(st, sizeof st, x) > 0 && strlen(st) < 255);
    x.state = OS_DRIZZLE_NEAR, x.qual = OQ_SEVERE_FROST, x.gate = OG_STALE, x.has_tend = true, x.tend.code = TC_FALLING;
    x.tend.fast = true, x.tend.d = -12.3f, x.as_of = now;
    CHECK(outlookAttributes(at, sizeof at, x) > 0 && strlen(at) < 256);
    // the ETag word: none -> 0; the source or the codes move it (module off <-> on changes /api/live's ETag)
    OutNear z = {};
    CHECK(outlookNearWord(nullptr) == 0 && outlookNearWord(&z) == 0);
    OutNear m1 = nr, m2 = nr;
    m1.wx = NW_DRIZZLE, m2.wx = NW_RAIN;
    CHECK(outlookNearWord(&m1) != 0 && outlookNearWord(&m1) != outlookNearWord(&m2));
  }
  // ---- the shared vectors (test/tools/t3_vectors.json, also tools/t3_score.py's): a report's weather groups through the
  // firmware's own parser and admission
  {
    std::string path = __FILE__;
    const size_t cut = path.find_last_of("/\\");
    path = (cut == std::string::npos ? std::string(".") : path.substr(0, cut)) + "/../tools/t3_vectors.json";
    FILE* f = fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    std::string js;
    if (f) {
      char b[512];
      size_t k;
      while ((k = fread(b, 1, sizeof b, f)) > 0) js.append(b, k);
      fclose(f);
    }
    JsonReader r(js.data(), js.size());
    char k[16], wx[32], w[16];
    int nvec = 0, nok = 0;
    static const char* const NAMES[6] = {"snow", "rain", "drizzle", "thunder", "fog", "mist"};
    static const uint8_t BITS[6] = {NW_SNOW, NW_RAIN, NW_DRIZZLE, NW_THUNDER, NW_FOG, NW_MIST};
    r.beginObject();
    while (r.nextKey(k, sizeof k)) {
      if (strcmp(k, "map")) { r.skipValue(); continue; }
      r.beginArray();
      while (r.nextElement()) {
        uint8_t want = 0;
        wx[0] = 0;
        r.beginObject();
        while (r.nextKey(k, sizeof k)) {
          if (!strcmp(k, "wx")) r.readString(wx, sizeof wx);
          else if (!strcmp(k, "near")) {
            r.beginArray();
            while (r.nextElement())
              if (r.readString(w, sizeof w))
                for (int i = 0; i < 6; i++) if (!strcmp(w, NAMES[i])) want |= BITS[i];
          } else r.skipValue();
        }
        char line[128];
        snprintf(line, sizeof line, "METAR EGLL 161150Z 25010KT 9999 %s OVC008 08/07 Q1013", wx);
        const char* id[1] = {"EGLL"};
        MetarObs o;
        memset(&o, 0, sizeof o);
        const bool parsed = metarParse(line, strlen(line), id, 1, now, o).why == MetarWhy::Ok;
        s.obs[0] = o;
        const OutNear n = nearFromStore(c, s, 52, 20, now);
        nvec++;
        if (parsed && n.ok && n.wx == want) nok++;
        else printf("  t3 vector %s: parsed %d ok %d wx 0x%02x want 0x%02x\n", wx, parsed, n.ok, n.wx, want);
      }
    }
    CHECK(nvec >= 20 && nok == nvec);
  }
}
