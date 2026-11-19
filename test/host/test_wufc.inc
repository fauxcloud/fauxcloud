// test_wufc.inc -- the WU forecast line. The streaming scanner on the
// recorded, scrubbed response (fixtures/wufc-5day-en.json: key and geocode removed, dates -52 weeks) fed whole, in
// 1-byte and odd splits; the after-15:00 shape (today's slots null); null tomorrow fields; 40+-byte phrases with a
// UTF-8 cut and \u escapes; long keys and nested unknown objects; gzip; a truncated body; the cap; the plausibility
// edges; the store's stale rule and day label (DST, a :30 zone); the widest JSON; the scheduler.

namespace {
std::string wufcFixture() {
  std::string path = __FILE__;
  const size_t cut = path.find_last_of("/\\");
  path = (cut == std::string::npos ? std::string(".") : path.substr(0, cut)) + "/fixtures/wufc-5day-en.json";
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char b[1024];
    size_t k;
    while ((k = fread(b, 1, sizeof b, f)) > 0) s.append(b, k);
    fclose(f);
  }
  return s;
}
std::string repl(std::string s, const std::string& a, const std::string& b) {
  const size_t i = s.find(a);
  if (i != std::string::npos) s.replace(i, a.size(), b);
  return s;
}
WufcParse wufcRun(const std::string& body, uint32_t now, WufcData* d, size_t step = 0, bool cut = false) {
  static WufcSink k;  // the sink lives in the job block on the board; static here (no stack copy)
  k.reset();
  if (!step) k.feed(body.data(), body.size());
  else
    for (size_t i = 0; i < body.size(); i += step) k.feed(body.data() + i, body.size() - i < step ? body.size() - i : step);
  return k.finish(cut, now, d);
}
}  // namespace

static void testWufc() {
  const std::string fx = wufcFixture();
  CHECK(fx.size() > 5000);
  const uint32_t now = 1763359200u - 20u * 3600u;  // the morning before (valid = tomorrow 07:00 local)
  WufcData d;
  // ---- the recorded answer: tomorrow's day slot
  CHECK(wufcRun(fx, now, &d) == WufcParse::Ok);
  CHECK(d.hi == 9 && d.lo == 5 && d.pop == 55 && d.valid_t == 1763359200u && strcmp(d.phrase, "PM Light Rain") == 0);
  // every split: 1-byte feeds, 2, 3, 7, 61 bytes
  bool splits = true;
  for (size_t step : {1, 2, 3, 7, 61}) {
    WufcData e;
    splits = splits && wufcRun(fx, now, &e, step) == WufcParse::Ok && e.hi == 9 && e.pop == 55 && !strcmp(e.phrase, "PM Light Rain");
  }
  CHECK(splits);
  // ---- the after-15:00 shape: today's slots null, tomorrow's unchanged -> "Tomorrow" as before
  std::string late = repl(fx, "\"temperatureMax\":[7,", "\"temperatureMax\":[null,");
  late = repl(late, "\"precipChance\":[3,7,", "\"precipChance\":[null,7,");
  late = repl(late, "\"wxPhraseLong\":[\"Sunny\",", "\"wxPhraseLong\":[null,");
  late = repl(late, "\"cloudCover\":[10,", "\"cloudCover\":[null,");
  CHECK(late != fx && wufcRun(late, now, &d) == WufcParse::Ok && d.hi == 9 && !strcmp(d.phrase, "PM Light Rain"));
  // ---- null / missing tomorrow fields -> nodata; a null precipChance -> the line without it
  CHECK(wufcRun(repl(fx, "\"temperatureMax\":[7,9,", "\"temperatureMax\":[7,null,"), now, &d) == WufcParse::NoData);
  CHECK(wufcRun(repl(fx, "\"Partly Cloudy\",\"PM Light Rain\"", "\"Partly Cloudy\",null"), now, &d) == WufcParse::NoData);
  CHECK(wufcRun(repl(fx, "\"precipChance\":[3,7,55,", "\"precipChance\":[3,7,null,"), now, &d) == WufcParse::Ok && d.pop == WUFC_UNSET);
  // ---- phrases: a 40-B cut never splits a character (raw UTF-8 and \u escapes), control characters dropped
  {
    const std::string longp = "\"Partly Cloudy\",\"Rain and snow showers late, then clearing \\u00e9t\\u00e9 more\"";
    CHECK(wufcRun(repl(fx, "\"Partly Cloudy\",\"PM Light Rain\"", longp), now, &d) == WufcParse::Ok);
    CHECK(strlen(d.phrase) <= 40 && strncmp(d.phrase, "Rain and snow showers late, then clearin", 40) == 0);
    // 39 ASCII bytes + a 2-byte character: the character does not fit, the cut is before it
    const std::string edge = "\"Partly Cloudy\",\"" + std::string(39, 'a') + "\xc3\xa9xyz\"";
    CHECK(wufcRun(repl(fx, "\"Partly Cloudy\",\"PM Light Rain\"", edge), now, &d) == WufcParse::Ok && strlen(d.phrase) == 39);
    const std::string esc = "\"Partly Cloudy\",\"" + std::string(38, 'b') + "\\u00e9\\u00e9\"";
    CHECK(wufcRun(repl(fx, "\"Partly Cloudy\",\"PM Light Rain\"", esc), now, &d) == WufcParse::Ok && strlen(d.phrase) == 40 &&
          (uint8_t)d.phrase[38] == 0xc3);
    CHECK(wufcRun(repl(fx, "\"PM Light Rain\"", "\"Light\\nRain \\\"x\\\" \\ud83d\""), now, &d) == WufcParse::Ok &&
          strcmp(d.phrase, "LightRain \"x\" ?") == 0);
  }
  // ---- keys longer than 24 B, nested unknown objects and arrays, a 256-character narrative
  {
    std::string odd = repl(fx, "{\"calendarDayTemperatureMax\"",
                           "{\"anExtremelyLongKeyNameThatIsLongerThanTheBuffer\":{\"x\":[1,{\"y\":[2,3]},\"z\"],\"temperatureMax\":[1,99]},"
                           "\"calendarDayTemperatureMax\"");
    odd = repl(odd, "\"narrative\":[\"Mainly sunny.", "\"narrative\":[\"" + std::string(256, 'n') + "\",\"Mainly sunny.");
    CHECK(wufcRun(odd, now, &d) == WufcParse::Ok && d.hi == 9);
  }
  // ---- gzip, not JSON, not the shape, a truncated body, over the cap
  CHECK(wufcRun(std::string("\x1f\x8b\x08\x00", 4) + fx, now, &d) == WufcParse::Encoding);
  CHECK(wufcRun("<html>Unauthorized</html>", now, &d) == WufcParse::Bad);
  CHECK(wufcRun("{\"errors\":[{\"error\":{\"code\":\"CDN-0001\"}}],\"success\":false}", now, &d) == WufcParse::Bad);
  CHECK(wufcRun(fx.substr(0, fx.size() - 3), now, &d) == WufcParse::Bad);  // a half answer is never a line
  CHECK(wufcRun(fx, now, &d, 0, true) == WufcParse::TooLarge);
  CHECK(wufcRun(fx + " ", now, &d) == WufcParse::Ok && wufcRun(fx + "x", now, &d) == WufcParse::Bad);
  // ---- plausibility: -60..60, lo <= hi + 2, 0..100 %, valid within now -24 h .. +72 h
  CHECK(wufcRun(repl(fx, "\"temperatureMax\":[7,9,", "\"temperatureMax\":[7,61,"), now, &d) == WufcParse::Bad);
  CHECK(wufcRun(repl(fx, "\"temperatureMin\":[0,5,", "\"temperatureMin\":[0,12,"), now, &d) == WufcParse::Bad);
  CHECK(wufcRun(repl(fx, "\"temperatureMin\":[0,5,", "\"temperatureMin\":[0,11,"), now, &d) == WufcParse::Ok);  // hi + 2
  CHECK(wufcRun(repl(fx, "\"precipChance\":[3,7,55,", "\"precipChance\":[3,7,101,"), now, &d) == WufcParse::Bad);
  CHECK(wufcRun(fx, 1763359200u + 86401u, &d) == WufcParse::Bad && wufcRun(fx, 1763359200u - 3u * 86400u - 1, &d) == WufcParse::Bad);
  CHECK(wufcRun(fx, 1763359200u + 86400u, &d) == WufcParse::Ok);

  // ---- the store: shown <= 2 h, the day label from the bridge's local date
  PosixTz cet, ist;
  CHECK(parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", &cet) && parsePosixTz("IST-5:30", &ist));
  WufcStore s = {};
  CHECK(wufcRun(fx, now, &d) == WufcParse::Ok);
  wufcPut(&s, d, now);
  CHECK(s.gen == 1 && wufcDay(s, now, cet) && !strcmp(wufcDay(s, now, cet), "tomorrow"));
  CHECK(wufcDay(s, now + WUFC_STALE_S, cet) && !wufcDay(s, now + WUFC_STALE_S + 1, cet));  // stale at 2 h + 1 s
  const uint32_t lateNight = 1763359200u - 6u * 3600u;  // 01:00 CET on the valid day: before WU's 03:00 rollover
  s.fetched_t = lateNight - 1800;
  CHECK(!strcmp(wufcDay(s, lateNight, cet), "today"));
  CHECK(!wufcDay(s, 1763359200u + 86400u - 3600, cet) || strcmp(wufcDay(s, 1763359200u + 86400u - 3600, cet), "tomorrow"));
  // a :30 zone: 07:00 CET = 11:30 IST, the same calendar day
  s.fetched_t = now;
  CHECK(!strcmp(wufcDay(s, now, ist), "tomorrow"));
  // DST: the valid day in summer time (2026-03-29 is the change) still reads "tomorrow" the evening before
  {
    WufcStore ds = s;
    ds.d.valid_t = (uint32_t)unixFromUtc(2026, 3, 29, 5, 0, 0);  // 07:00 CEST
    ds.fetched_t = (uint32_t)unixFromUtc(2026, 3, 28, 20, 0, 0);
    CHECK(!strcmp(wufcDay(ds, ds.fetched_t + 600, cet), "tomorrow"));
  }
  wufcDrop(&s);
  CHECK(s.gen == 2 && !wufcDay(s, now, cet));
  // ---- the widest JSON: a 40-B phrase with quotes, -60 / 60, 100 %, 10-digit times
  {
    WufcStore w = {};
    WufcData x = {};
    x.hi = 60, x.lo = -60, x.pop = 100, x.valid_t = 1763359200u;
    memset(x.phrase, '"', 40);
    x.phrase[40] = 0;
    wufcPut(&w, x, now);
    std::string js;
    char b[64];
    {
      Out o(toString, &js, b, sizeof b);
      wufcLiveJson(o, w, now, cet);
      o.flush();
    }
    CHECK(js.find(",\"wufc\":{\"day\":\"tomorrow\",\"phrase\":\"\\\"") == 0 && js.find("\"hi\":60,\"lo\":-60,\"pop\":100,\"t\":\"") != std::string::npos);
    char at[256];
    const int n = wufcAttributes(at, sizeof at, w, now, cet);
    printf("  info: wufc HA attributes widest %d B (limit 255)\n", n);
    CHECK(n > 0 && n < 255 && strstr(at, "\"source\":\"Weather Underground\""));
    w.d.pop = WUFC_UNSET;
    CHECK(wufcAttributes(at, sizeof at, w, now, cet) > 0 && strstr(at, "\"precip_pct\":null"));
    js.clear();
    {
      Out o(toString, &js, b, sizeof b);
      wufcLiveJson(o, w, now + WUFC_STALE_S + 1, cet);  // stale: nothing at all
      o.flush();
    }
    CHECK(js.empty() && wufcAttributes(at, sizeof at, w, now + WUFC_STALE_S + 1, cet) == 2);
  }
  // ---- the HA bridge sensor: not diagnostic, attributes, its icon; the bridge's device, never a station's.
  // Built only with FC_HA_WUFC 1 (default 0)
#if FC_HA_WUFC
  {
    const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc};
    char topic[128], payload[896];
    const EntitySpec e = bridgeSpec(B_WU_TOMORROW);
    CHECK(!strcmp(e.key, "wu_tomorrow") && e.attributes && bridgeDiscoveryTopic(topic, sizeof topic, mac, e.key) > 0 &&
          !strcmp(topic, "homeassistant/sensor/fauxcloud_bridge_246f28aabbcc/wu_tomorrow/config"));
    CHECK(bridgeDiscoveryPayload(payload, sizeof payload, mac, e, "fauxcloud 0.1.0-b24", "http://192.168.1.20/") > 0);
    const std::string p = payload;
    CHECK(contains(p, "\"name\":\"Forecast tomorrow (Weather Underground)\"") && contains(p, "mdi:weather-partly-cloudy") &&
          contains(p, "\"json_attributes_topic\":\"fauxcloud/bridge/246f28aabbcc/wu_tomorrow/attr\"") &&
          !contains(p, "entity_category"));
  }
#else
  CHECK(B_COUNT == B_NOTICES + 1);  // no wu_tomorrow entity in the default image
#endif
  // ---- request, key, cfg
  {
    char p[200];
    CHECK(wufcPath(p, sizeof p, 51.4775f, -0.4614f, "0123456789abcdef0123456789ABCDEF") > 0 &&
          strstr(p, "geocode=51.5,-0.5&units=m&language=en-US&format=json&apiKey=0123456789abcdef0123456789ABCDEF"));  // gitleaks:allow (a fake 32-hex key fixture, not a secret)
    CHECK(wufcKeyOk("0123456789abcdef0123456789ABCDEF") && !wufcKeyOk("0123456789abcdef0123456789abcde") &&
          !wufcKeyOk("0123456789abcdef0123456789abcdeg") && !wufcKeyOk(nullptr));
    WufcCfg c;
    const uint8_t blob[4] = {1, 7, 1, 0};
    CHECK(wufcCfgLoad(blob, 4, &c) && c.on == 1 && c.verified == 1);
    const uint8_t future[4] = {9, 1, 1, 0};
    CHECK(!wufcCfgLoad(future, 4, &c) && c.on == 0 && c.ver == WUFC_CFG_VER);
  }
}

static void testWufcSched() {
  const uint32_t unix0 = (uint32_t)unixFromUtc(2026, 11, 16, 12, 0, 0);  // 12:00:00 UTC
  // ---- the grid: :10 UTC + the boot's phase, the first due >= 120 s after it can run
  {
    WufcSched w(45000);
    NqCand c[NQ_MAX];
    WufcTickIn in{1000, true, true, unix0};
    CHECK(w.offer(in, c) == 0 && w.nextS(1000) == 600 + 45);
    WufcSched late(0);
    WufcTickIn l2{1000, true, true, unix0 + 540};  // 12:09: the 12:10 slot is < 120 s away -> 13:10
    CHECK(late.offer(l2, c) == 0 && late.nextS(1000) == 3660);
    // in the window: one Pull candidate with the full busy table and the METAR heap figure
    in.now_ms = 1000 + 645000;
    CHECK(w.offer(in, c) == 1 && c[0].kind == FwdKind::WuFetch && c[0].prio == NqPrio::Pull && c[0].mask == FB_ALL &&
          c[0].src == NQ_SRC_WUFC && c[0].need8 == 17920u + FWD_TLS_MARGIN8);
    // the window ends without a start: a skip, the next hour
    in.now_ms = 1000 + 645000 + WUFC_WINDOW_MS;
    CHECK(w.offer(in, c) == 0 && w.n.skip == 1 && w.nextS(in.now_ms) == 3600 - WUFC_WINDOW_MS / 1000);
    // off when it cannot run; the first due again when it can
    in.can_run = false;
    CHECK(w.offer(in, c) == 0 && w.nextS(in.now_ms) == -1 && !strcmp(w.stateName(), "off"));
  }
  // ---- outcomes
  auto startPull = [](WufcSched& w, uint32_t& ms) {
    NqCand c[NQ_MAX];
    WufcTickIn in{ms, true, true, 1};
    int n = w.offer(in, c);
    for (int g = 0; g < 400 && !n; g++) ms += 10000, in.now_ms = ms, n = w.offer(in, c);
    NqIn q{ms, 0, 100000, 60000, true, -1};
    const NqPick p = nqPick(q, c, n);
    FwdKind k = FwdKind::None;
    return w.picked(in, c, n, p, &k) && k == FwdKind::WuFetch;
  };
  {
    WufcSched w(0);
    uint32_t ms = 5000;
    CHECK(startPull(w, ms) && w.running() && !strcmp(w.stateName(), "running"));
    w.result(FwdKind::WuFetch, WufcOutcome::Fail, ms, 0);  // one retry 5 min later
    CHECK(!strcmp(w.stateName(), "retry") && w.nextS(ms) == 300);
    uint32_t m2 = ms;
    CHECK(startPull(w, m2) && m2 - ms >= WUFC_RETRY_MS);
    w.result(FwdKind::WuFetch, WufcOutcome::Fail, m2, 0);  // the second fail: missed, the next hour
    CHECK(w.n.fail == 2 && w.n.missed == 1 && !strcmp(w.stateName(), "idle"));
    uint32_t m3 = m2;
    CHECK(startPull(w, m3));
    w.result(FwdKind::WuFetch, WufcOutcome::Limited, m3, 7200);  // 429 + Retry-After 2 h
    CHECK(w.n.limited == 1 && w.nextS(m3) >= 7200);
    // three refused in a row pause until the key changes
    for (int i = 0; i < 3; i++) {
      uint32_t m = m3 + 10000000u * (uint32_t)(i + 1);
      CHECK(startPull(w, m));
      w.result(FwdKind::WuFetch, WufcOutcome::Refused, m, 0);
    }
    CHECK(w.pause() == WufcPause::Key && w.nextS(m3) == -1 && w.n.paused == 1 && !strcmp(wufcPauseName(w.pause()), "key"));
    w.keyChanged();
    CHECK(w.pause() == WufcPause::None);
  }
  {  // encoding / too_large / untrusted pause at once; no_memory after three
    WufcSched w(0);
    uint32_t ms = 5000;
    CHECK(startPull(w, ms));
    w.result(FwdKind::WuFetch, WufcOutcome::Encoding, ms, 0);
    CHECK(w.pause() == WufcPause::Encoding);
    w.keyChanged();
    for (int i = 0; i < 3; i++) {
      uint32_t m = ms + 4000000u * (uint32_t)(i + 1);
      CHECK(startPull(w, m));
      w.result(FwdKind::WuFetch, WufcOutcome::NoMemory, m, 0);
    }
    CHECK(w.pause() == WufcPause::Memory && w.n.heap == 3);
  }
  {  // a save during a pull (the app's order: the epoch bumped under gLock, then keyChanged): the old job's answer is
     // fenced to `off` (finishWufc: same ? out : Off) and pauses nothing; unfenced, the same answer pauses the new key
    for (WufcOutcome r : {WufcOutcome::Untrusted, WufcOutcome::TooLarge, WufcOutcome::Encoding}) {
      WufcSched w(0), u(0);
      uint32_t ms = 5000, mu = 5000;
      CHECK(startPull(w, ms) && startPull(u, mu));
      w.keyChanged(), u.keyChanged();                // the save lands while both pulls run
      w.result(FwdKind::WuFetch, WufcOutcome::Off, ms, 0);  // fenced: the epoch moved
      u.result(FwdKind::WuFetch, r, mu, 0);                 // the late result of a fenced pull
      CHECK(w.pause() == WufcPause::None && !strcmp(w.stateName(), "off") && u.pause() != WufcPause::None);
      // the next offer starts the new key's schedule from its first due
      NqCand c[NQ_MAX];
      WufcTickIn in{ms + 1000, true, true, 1};
      CHECK(w.offer(in, c) == 0 && w.nextS(ms + 1000) > 0);
    }
    // the old behaviour, kept: the same answer with the settings unchanged (same epoch) still pauses
    WufcSched s(0);
    uint32_t m = 5000;
    CHECK(startPull(s, m));
    s.result(FwdKind::WuFetch, WufcOutcome::Untrusted, m, 0);
    CHECK(s.pause() == WufcPause::Untrusted);
  }
  {  // a pull never sent (the key or the location gone at its start): `off` counts a skip, never `bad`, and no streak ends
    WufcSched w(0);
    uint32_t ms = 5000;
    CHECK(!strcmp(wufcOutcomeName(WufcOutcome::Off), "off") && !strcmp(wufcOutcomeName(WufcOutcome::Bad), "bad"));
    for (int i = 0; i < 2; i++) {
      uint32_t m = ms + 10000000u * (uint32_t)(i + 1);
      CHECK(startPull(w, m));
      w.result(FwdKind::WuFetch, WufcOutcome::Refused, m, 0);
    }
    uint32_t m = ms + 30000000u;
    CHECK(startPull(w, m));
    const uint32_t sk = w.n.skip;  // the windows the jumps passed count as skips too
    w.result(FwdKind::WuFetch, WufcOutcome::Off, m, 0);
    CHECK(w.n.bad == 0 && w.n.skip == sk + 1 && w.pause() == WufcPause::None && !w.running() && !strcmp(w.stateName(), "idle") &&
          w.nextS(m) > 0);  // the hour closed: no re-offer in the same window
    m += 10000000u;
    CHECK(startPull(w, m));
    w.result(FwdKind::WuFetch, WufcOutcome::Refused, m, 0);
    CHECK(w.pause() == WufcPause::Key);  // the third refused in a row: the streak survived the `off`
    // the old behaviour, kept: `bad` counts bad and ends the refused streak
    WufcSched b(0);
    uint32_t mb = 5000;
    for (int i = 0; i < 2; i++) {
      uint32_t x = mb + 10000000u * (uint32_t)(i + 1);
      CHECK(startPull(b, x));
      b.result(FwdKind::WuFetch, WufcOutcome::Refused, x, 0);
    }
    uint32_t x = mb + 30000000u;
    CHECK(startPull(b, x));
    const uint32_t skb = b.n.skip;
    b.result(FwdKind::WuFetch, WufcOutcome::Bad, x, 0);
    CHECK(b.n.skip == skb);
    x += 10000000u;
    CHECK(startPull(b, x));
    b.result(FwdKind::WuFetch, WufcOutcome::Refused, x, 0);
    CHECK(b.n.bad == 1 && b.pause() == WufcPause::None);
  }
  // ---- "Fetch now": queued, never refused for a busy line; off / low_memory the only refusals; a second
  // press joins the first; a page request does not hold it (FB_TEST: else the sheet's 2-s poll keeps
  // FB_HTTP set until a 30-s give-up); held by the 60-s gap, an upload and the heap gate; given up as busy after 10 min
  {
    WufcSched w(0);
    CHECK(w.requestRun(1000, false, true) == MetarReq::Off && !w.runPending());
    CHECK(w.requestRun(1000, true, false) == MetarReq::LowMemory && !w.runPending());
    CHECK(strcmp(metarReqName(MetarReq::LowMemory), "low_memory") == 0);
    CHECK(w.requestRun(1000, true, true) == MetarReq::Accepted && w.runPending());
    CHECK(w.requestRun(1100, true, false) == MetarReq::Accepted && w.runPending());  // joins (the heap is the job's gate then)
    CHECK(w.etaS(1100, false, false) == WUFC_ETA_START_S && w.etaS(1100, true, false) == WUFC_ETA_JOB_S + WUFC_ETA_START_S);
    // held by its start gate (an upload, no link, the heap): the job's estimate too, not "about 5 s" for ten minutes
    CHECK(w.etaS(1100, false, true) == WUFC_ETA_JOB_S + WUFC_ETA_START_S);
    {
      NqCand g{};
      nqHeapGate(true, FwdTlsClass::Wufc, g);
      CHECK(!wufcRunHeld(FB_HTTP | FB_STORE | FB_MQTT | FB_HOUR, g.need8, g.largest8));  // not in FB_TEST, the gate met
      CHECK(wufcRunHeld(FB_UPLOAD, 100000, 60000) && wufcRunHeld(FB_LINK, 100000, 60000) && wufcRunHeld(FB_RESTART, 100000, 60000));
      CHECK(wufcRunHeld(0, g.need8 - 1, 60000) && wufcRunHeld(0, 100000, g.largest8 - 1) && !wufcRunHeld(0, 100000, 60000));
    }
    NqCand c[NQ_MAX];
    WufcTickIn in{1200, false, true, 1};  // the switch off: Fetch now still runs (the key check)
    int n = w.offer(in, c);
    CHECK(n == 1 && c[0].kind == FwdKind::WuRun && c[0].prio == NqPrio::User && c[0].mask == FB_TEST);
    NqIn q{1200, FB_HTTP | FB_STORE | FB_MQTT | FB_HOUR, 100000, 60000, true, -1};  // the sheet polling: it starts
    FwdKind k = FwdKind::None;
    CHECK(w.picked(in, c, n, nqPick(q, c, n), &k) && k == FwdKind::WuRun && !w.runPending());
    CHECK(w.requestRun(1300, true, true) == MetarReq::Accepted && !w.runPending());  // pressed while it runs: joins
    w.result(FwdKind::WuRun, WufcOutcome::Ok, 5000, 0);
    // the busy race: pressed again 1 s after the start -- queued (no too_soon), offered only once 60 s have passed
    CHECK(w.n.ok == 1 && w.requestRun(2200, true, true) == MetarReq::Accepted && w.runPending());
    CHECK(w.etaS(2200, false, false) == (WUFC_USER_GAP_MS - 1000 + 999) / 1000 + WUFC_ETA_START_S);
    CHECK(w.etaS(2200, false, true) == (WUFC_USER_GAP_MS - 1000 + 999) / 1000 + WUFC_ETA_START_S);  // the gap is longer
    CHECK(w.etaS(1200 + WUFC_USER_GAP_MS - 5000, false, true) == WUFC_ETA_JOB_S + WUFC_ETA_START_S);  // 5 s of gap left, held
    in.now_ms = 1200 + WUFC_USER_GAP_MS - 1;
    CHECK(w.offer(in, c) == 0 && w.runPending());
    in.now_ms = 1200 + WUFC_USER_GAP_MS;
    n = w.offer(in, c);
    CHECK(n == 1 && c[0].kind == FwdKind::WuRun);
    NqIn held{in.now_ms, FB_UPLOAD, 100000, 60000, true, -1};  // an upload holds it (in FB_TEST)
    CHECK(!w.picked(in, c, n, nqPick(held, c, n), &k));
    NqIn busyLine{in.now_ms, 0, 100000, 60000, false, -1};  // a send running: it waits its turn
    CHECK(!w.picked(in, c, n, nqPick(busyLine, c, n), &k));
    in.now_ms = 1200 + WUFC_USER_GAP_MS + 30001;  // held > 30 s since it was first offered: still queued (no 30-s give-up)
    n = w.offer(in, c);
    CHECK(n == 1 && w.runPending() && w.takeRunOutcome() == MetarUserOut::None);
    in.now_ms = 2200 + WUFC_USER_WAIT_MS + 1;  // 10 min without a start: busy, once
    CHECK(w.offer(in, c) == 0 && !w.runPending() && w.takeRunOutcome() == MetarUserOut::Busy);
    CHECK(w.takeRunOutcome() == MetarUserOut::None);
    // the line going down while queued (no key / location / time): off
    CHECK(w.requestRun(in.now_ms, true, true) == MetarReq::Accepted);
    WufcTickIn gone{in.now_ms + 1, false, false, 0};
    CHECK(w.offer(gone, c) == 0 && !w.runPending() && w.takeRunOutcome() == MetarUserOut::Off);
  }
  // ---- the queue: the pull is the lowest, yields to a send window, never two jobs at once (with METAR's candidates)
  {
    NqCand c[3];
    c[0] = NqCand{NQ_SRC_WUFC, 0, FwdKind::WuFetch, NqPrio::Pull, FB_ALL, 0, 0, 100};
    c[1] = NqCand{NQ_SRC_METAR, 0, FwdKind::Fetch, NqPrio::Fetch, FB_ALL, 0, 0, 200};
    NqIn q{0, 0, 100000, 60000, true, -1};
    NqPick p = nqPick(q, c, 2);
    CHECK(p.idx == 1 && p.why[0] == FW_WHY_TURN);
    q.send_due_s = 10;  // a send window 10 s away: both online reads wait
    p = nqPick(q, c, 1);
    CHECK(p.idx == -1 && p.why[0] == FW_WHY_SEND_SOON);
    q.send_due_s = -1;
    q.netout_idle = false;
    p = nqPick(q, c, 2);
    CHECK(p.idx == -1 && p.why[0] == FW_WHY_NETOUT && p.why[1] == FW_WHY_NETOUT);
    CHECK(!strcmp(wufcOutcomeName(WufcOutcome::TooLarge), "too_large") && !strcmp(fwdTlsClassName(FwdTlsClass::Wufc), "wufc"));
  }
}
