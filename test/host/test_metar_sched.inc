// test_metar_sched.inc -- the METAR source on the
// queue (fake clock), the RAM store, the reference reading, /api/metar's JSON, the admin POST.
#include "metar_sched.h"
#include "metar_store.h"

namespace {
struct MSim {
  MetarSched s;
  MetarTickIn in{1000, true, true};
  uint16_t busy = 0;
  bool idle = true;
  long sendDue = -1;
  explicit MSim(uint32_t phase = 0) : s(phase) {}
  // one loop tick with METAR alone; returns the kind started (None = nothing)
  FwdKind tick() {
    NqCand c[NQ_MAX];
    const int n = s.offer(in, c);
    const NqIn q{in.now_ms, busy, 60000, 50000, idle && !s.running(), sendDue};
    const NqPick p = nqPick(q, c, n);
    FwdKind k = FwdKind::None;
    return s.picked(in, c, n, p, &k) ? k : FwdKind::None;
  }
  void adv(uint32_t ms) { in.now_ms += ms; }
  // run the clock until a start (or the limit); returns ms waited
  uint32_t until(FwdKind want, uint32_t limit_ms) {
    uint32_t w = 0;
    while (w <= limit_ms) {
      if (tick() == want) return w;
      adv(1000), w += 1000;
    }
    return UINT32_MAX;
  }
};
MetarObs obs(const char* icao, uint32_t t, int16_t qnh, uint8_t flags = 0) {
  MetarObs o;
  memset(&o, 0, sizeof o);
  memcpy(o.icao, icao, 4);
  o.flags = (uint8_t)(MO_VALID | flags);
  o.obs_unix = t;
  o.qnh_dhpa = qnh;
  o.t_dc = o.td_dc = METAR_I16_UNSET;
  o.wdir = METAR_WDIR_NONE;
  o.wind_dkmh = o.gust_dkmh = o.vis_m = o.ceil_ft = METAR_U16_UNSET;
  o.cover = METAR_COVER_UNSET;
  return o;
}
Airport apt3(const char* icao, uint16_t km) {
  Airport a;
  memset(&a, 0, sizeof a);
  memcpy(a.icao, icao, 4);
  snprintf(a.name, sizeof a.name, "%s field", icao);
  a.lat = 4000, a.lon = -7000, a.km = km;
  return a;
}
std::string metarJsonStr(const MetarCfg& c, const MetarStore& s, bool on, uint32_t now) {
  std::string out;
  char buf[96];
  Out o(toString, &out, buf, sizeof buf);
  metarJson(o, c, s, on, now);
  o.flush();
  return out;
}
}  // namespace

// the widest JSON, a run once inside an open window, airports changed during a fetch
static void testMetarEdges() {
  {  // the admin counters at their widest stay whole and parse; /api/metar at its widest parses
    MetarCounters n;
    n.ok = n.nodata = n.fail = n.yield = n.skip = n.heap = n.limited = n.untrusted = n.paused = n.missed = 4294967295u;
    std::string out = "{\"x\":1";
    char buf[96];
    Out o(toString, &out, buf, sizeof buf);
    metarCountersJson(o, n);
    o.raw("}");
    o.flush();
    CHECK(jsonParses(out) && out.find("\"missed\":4294967295}") != std::string::npos);
    MetarCfg c = {};
    MetarStore s;
    metarStoreClear(&s);
    const uint32_t now = (uint32_t)unixFromUtc(2026, 11, 13, 11, 30, 0);
    c.on = true;
    c.n = METAR_AIRPORTS_MAX;
    for (int i = 0; i < c.n; i++) {
      c.a[i] = apt3(i == 0 ? "KAAA" : i == 1 ? "KBBB" : "KCCC", 65534);
      memset(c.a[i].name, '"', sizeof c.a[i].name - 1);  // every character escaped
      c.a[i].name[sizeof c.a[i].name - 1] = 0;
      MetarObs x = obs(c.a[i].icao, now - 60, 10850, MO_COR | MO_AUTO | MO_CAVOK | MO_LOW_CONF | MO_CB | MO_VIS_GE);
      x.wdir = 359, x.wind_dkmh = x.gust_dkmh = 9999, x.vis_m = 10000, x.ceil_ft = 65000, x.cover = 4, x.wx = 0xFF, x.wx_int = -1;
      x.t_dc = -800, x.td_dc = -800;
      metarStorePut(&s, c, x);
    }
    const std::string j = metarJsonStr(c, s, true, now);
    CHECK(jsonParses(j) && j.find("\"local_values\"") != std::string::npos);
  }
  {  // each airport's night flag from its own position at the report's time (the sheet's sky glyph), not the station's
    MetarCfg c = {};
    MetarStore s;
    metarStoreClear(&s);
    c.on = true;
    c.n = 3;
    c.a[0] = apt3("EGLL", 20), c.a[0].lat = 5148, c.a[0].lon = -46;   // London
    c.a[1] = apt3("KJFK", 65534), c.a[1].lat = 4064, c.a[1].lon = -7378;  // New York
    c.a[2] = apt3("ZZZZ", 65534), c.a[2].lat = 0, c.a[2].lon = 0;     // no position stored
    const uint32_t noon = (uint32_t)unixFromUtc(2026, 11, 16, 12, 0, 0), eve = (uint32_t)unixFromUtc(2026, 11, 16, 20, 0, 0);
    auto put = [&](uint32_t t) {
      metarStoreClear(&s);
      for (int i = 0; i < c.n; i++) metarStorePut(&s, c, obs(c.a[i].icao, t, 10150));
      return metarJsonStr(c, s, true, t + 60);
    };
    auto night = [](const std::string& j, const char* icao) {
      const size_t k = j.find(std::string("\"icao\":\"") + icao + "\"");
      const size_t e = j.find("\"icao\":", k + 1);
      const std::string a = j.substr(k, e == std::string::npos ? std::string::npos : e - k);
      return a.find("\"night\":true") != std::string::npos ? 1 : a.find("\"night\":false") != std::string::npos ? 0 : -1;
    };
    const std::string j1 = put(noon), j2 = put(eve);
    CHECK(jsonParses(j1) && jsonParses(j2));
    // 12:00Z: day in London and New York (07:00 local, after the sunrise); 20:00Z: night in London, day in New York (15:00)
    CHECK(night(j1, "EGLL") == 0 && night(j1, "KJFK") == 0 && night(j2, "EGLL") == 1 && night(j2, "KJFK") == 0);
    CHECK(night(j1, "ZZZZ") == -1 && night(j2, "ZZZZ") == -1);  // no position: no key (the page keeps the station's)
    const std::string j3 = put((uint32_t)unixFromUtc(2026, 11, 17, 2, 0, 0));  // 02:00Z: night in both
    CHECK(night(j3, "EGLL") == 1 && night(j3, "KJFK") == 1);
    // an airport without a report carries no flag (no report time)
    metarStoreClear(&s);
    const std::string j4 = metarJsonStr(c, s, true, noon);
    CHECK(jsonParses(j4) && j4.find("\"night\"") == std::string::npos);
  }
  {  // "Fetch now" while the scheduled fetch runs: queued (not refused), started after the fetch's result
    MSim m;
    CHECK(m.until(FwdKind::Fetch, 100000) == 60000 && m.s.running());
    m.adv(1000);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Accepted && m.s.runPending());
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Busy);  // one waiting at a time
    m.adv(3000);
    CHECK(m.tick() == FwdKind::None);
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.tick() == FwdKind::RunOnce && !m.s.runPending());
    m.s.result(FwdKind::RunOnce, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.s.n.ok == 2 && m.s.takeRunOutcome() == MetarUserOut::None);
    m.adv(10000);  // on an idle line the gap still holds
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::TooSoon);
  }
  {  // a run once that answers inside an open window serves it -- no second fetch in that cycle
    MSim m;
    CHECK(m.until(FwdKind::Fetch, 100000) == 60000);
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    m.adv(900000);  // the next window opens
    m.busy = FB_HTTP;  // a page holds the scheduled fetch (the sheet is open)
    CHECK(m.tick() == FwdKind::None);
    m.adv(70000);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Accepted);
    m.busy = 0;
    CHECK(m.tick() == FwdKind::RunOnce);
    m.adv(2000);
    m.s.result(FwdKind::RunOnce, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.until(FwdKind::Fetch, 200000) == UINT32_MAX);  // the rest of the window: nothing
    CHECK(m.s.nextS(m.in.now_ms) > 600);                   // the next cycle
  }
  {  // new airports saved while a scheduled fetch runs: its result does not put the old grid back
    MSim m;
    CHECK(m.until(FwdKind::Fetch, 100000) == 60000);
    m.adv(1000);
    m.s.airportsChanged();
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    m.adv(1000);
    CHECK(m.until(FwdKind::Fetch, 200000) <= 61000);  // the first due again (60 s), not 15 min
  }
}

// the grid on :05 / :20 / :35 / :50 UTC (not boot-relative), the :50 cycle the TAF one; the JSON with forecasts
static void testMetarGridTaf() {
  {  // real time 10:00:00 UTC: the first due 10:05 (>= 60 s away), then 10:20, 10:35, 10:50 (TAF), 11:05
    MSim m;
    m.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 0, 0);
    CHECK(m.until(FwdKind::Fetch, 400000) == 300000 && !m.s.runTaf());
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    bool taf[4];
    for (int i = 0; i < 4; i++) {
      m.adv(1000);
      CHECK(m.until(FwdKind::Fetch, 900000) == 899000);
      taf[i] = m.s.runTaf();
      m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    }
    CHECK(!taf[0] && !taf[1] && taf[2] && !taf[3]);  // 10:20, 10:35, 10:50 (TAF), 11:05
    MSim late;  // 10:04:30: 10:05 is under 60 s away -> 10:20
    late.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 4, 30);
    CHECK(late.until(FwdKind::Fetch, 1000000) == 930000);
    MSim taf50;  // 10:49:00 -> 10:50 is 60 s away exactly: the first fetch is the TAF one
    taf50.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 49, 0);
    CHECK(taf50.until(FwdKind::Fetch, 100000) == 60000 && taf50.s.runTaf() && taf50.s.tafCycleNext());
    MSim noClock;  // no real time: a boot-relative due, never a TAF cycle
    CHECK(noClock.until(FwdKind::Fetch, 100000) == 60000 && !noClock.s.runTaf());
  }
  {  // a run once inside the TAF cycle's window does not serve it (the TAF still goes)
    MSim m;
    m.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 49, 0);
    m.busy = FB_HTTP;
    m.adv(61000);
    CHECK(m.tick() == FwdKind::None);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Accepted);
    m.busy = 0;
    CHECK(m.tick() == FwdKind::RunOnce);
    m.s.result(FwdKind::RunOnce, MetarOutcome::Ok, m.in.now_ms + 1000, 0);
    m.adv(61000);
    CHECK(m.tick() == FwdKind::Fetch && m.s.runTaf());
  }
  {  // the store: a forecast per chosen airport, an older issue never replaces; remap moves it; /api/metar at its widest
    MetarCfg c = {};
    MetarStore s;
    metarStoreClear(&s);
    const uint32_t now = (uint32_t)unixFromUtc(2026, 11, 14, 12, 30, 0);
    c.on = true;
    c.n = METAR_AIRPORTS_MAX;
    const char* ids[3] = {"KAAA", "KBBB", "KCCC"};
    for (int i = 0; i < 3; i++) {
      c.a[i] = apt3(ids[i], 65534);
      memset(c.a[i].name, '"', sizeof c.a[i].name - 1);
      c.a[i].name[sizeof c.a[i].name - 1] = 0;
      TafObs t = {};
      memcpy(t.icao, ids[i], 5);
      t.status = (uint8_t)TafStatus::Ok;
      t.issue_unix = now - 3600;
      t.valid_to = now + 30 * 3600;
      t.grid_unix = now - now % 3600;
      for (int k = 0; k < TAF_HOURS; k++) t.cell[k] = (uint16_t)(k % 2 ? 0x3FF : 0x2AA);  // every phenomenon, runs broken
      CHECK(metarStorePutTaf(&s, c, t) == i);
      MetarObs x = obs(ids[i], now - 60, 10850, MO_COR | MO_AUTO);
      x.wdir = 359, x.wind_dkmh = x.gust_dkmh = 9999, x.vis_m = 10000, x.ceil_ft = 65000, x.cover = 4, x.wx = 0xFF;
      x.t_dc = -800, x.td_dc = -800;
      metarStorePut(&s, c, x);
    }
    TafObs older = s.taf[0];
    older.issue_unix -= 7200;
    CHECK(metarStorePutTaf(&s, c, older) == -1);
    s.last_taf_t = now - 100, s.last_taf = MetarOutcome::Ok, s.last_taf_any = true;
    const std::string j = metarJsonStr(c, s, true, now);
    CHECK(jsonParses(j) && j.find("\"next12\":[{\"what\":\"thunder\"") != std::string::npos &&
          j.find("\"last_taf\":{\"t\":") != std::string::npos);
    {  // "cut":true only on the windows that run to where the data stops, once per such window
      // "started":true the same way, only on the windows that began before now
      int cuts = 0, starts = 0;
      for (int i = 0; i < 3; i++) {
        TafWindow w[8];
        const int n = tafWindows(s.taf[i], now, w, 8);
        for (int k = 0; k < n; k++) cuts += w[k].cut, starts += w[k].started;
      }
      int seen = 0, seen_s = 0;
      for (size_t p = j.find("\"cut\":true"); p != std::string::npos; p = j.find("\"cut\":true", p + 1)) seen++;
      for (size_t p = j.find("\"started\":true"); p != std::string::npos; p = j.find("\"started\":true", p + 1)) seen_s++;
      CHECK(seen == cuts && j.find("\"cut\":false") == std::string::npos);
      CHECK(seen_s == starts && j.find("\"started\":false") == std::string::npos);
    }
    {  // rain expected all along: one window to the 12-h edge, flagged cut and started (it began before now)
      MetarStore s2 = s;
      for (int k = 0; k < TAF_HOURS; k++) s2.taf[0].cell[k] = TB_E_RA;
      const std::string j2 = metarJsonStr(c, s2, true, now);
      CHECK(jsonParses(j2) && j2.find("{\"what\":\"rain\",\"possible\":false,\"from\":") != std::string::npos &&
            j2.find("\"cut\":true,\"started\":true}") != std::string::npos);
    }
    MetarCfg after = c;
    after.n = 1;
    after.a[0] = c.a[2];
    metarStoreRemap(&s, c, after);
    CHECK(!strcmp(s.taf[0].icao, "KCCC") && s.taf[1].status == 0);
  }
  {  // the TAF cycle's end -- absent markers only after ok / nodata, the time only then, failures in result
    MetarCfg c = {};
    MetarStore s;
    metarStoreClear(&s);
    const uint32_t now = (uint32_t)unixFromUtc(2026, 11, 14, 12, 50, 30), grid = now - now % 3600;
    c.on = true;
    c.n = 2;
    c.a[0] = apt3("KAAA", 10);
    c.a[1] = apt3("KBBB", 20);
    TafObs t = {};
    memcpy(t.icao, "KAAA", 5);
    t.status = (uint8_t)TafStatus::Ok, t.issue_unix = now - 1800, t.valid_to = now + 24 * 3600, t.grid_unix = grid;
    t.cell[0] = TB_E_RA;
    const char* const both[] = {"KAAA", "KBBB"};  // the job's want-list (taken at its start)
    // a failed first cycle: nothing stored, no time, the failure in result; the sheet then says "comes with the next fetch"
    metarStoreTafCycle(&s, c, MetarOutcome::Fail, &t, 1, now, grid, both, 2);
    std::string j = metarJsonStr(c, s, true, now);
    CHECK(s.taf[0].status == 0 && s.taf[1].status == 0 && s.last_taf_t == 0 &&
          j.find("\"last_taf\":{\"t\":null,\"result\":\"fail\"}") != std::string::npos && j.find("\"taf\":null") != std::string::npos);
    // ok with KAAA only: KAAA stored, KBBB absent, the time set
    metarStoreTafCycle(&s, c, MetarOutcome::Ok, &t, 1, now, grid, both, 2);
    CHECK(s.taf[0].status == (uint8_t)TafStatus::Ok && s.taf[1].status == (uint8_t)TafStatus::Absent && s.last_taf_t == now);
    j = metarJsonStr(c, s, true, now);
    CHECK(jsonParses(j) && j.find("\"status\":\"absent\",\"current\":false,\"issued\":null,\"valid_to\":null,\"to\":null,\"next12\":[]") != std::string::npos);
    CHECK(j.find("\"to\":\"2026-11-15T01:00:00Z\"") != std::string::npos);  // KAAA's horizon = grid + 13 h
    // a 429 / yield later: the forecasts and the time stay, the result says what happened
    metarStoreTafCycle(&s, c, MetarOutcome::Limited, nullptr, 0, now + 3600, grid + 3600, both, 2);
    CHECK(s.taf[0].status == (uint8_t)TafStatus::Ok && s.last_taf_t == now && s.last_taf == MetarOutcome::Limited);
    // nodata: every chosen airport absent (the answer was whole and had none of them)
    metarStoreTafCycle(&s, c, MetarOutcome::NoData, nullptr, 0, now + 7200, grid + 7200, both, 2);
    CHECK(s.taf[0].status == (uint8_t)TafStatus::Absent && s.taf[1].status == (uint8_t)TafStatus::Absent &&
          s.last_taf_t == now + 7200);
    // an airport added while the cycle ran was not asked: it keeps what it has (nothing yet), only the asked one
    // missing from the answer is absent
    metarStoreClear(&s);
    c.n = 3;
    c.a[2] = apt3("KCCC", 30);
    metarStoreTafCycle(&s, c, MetarOutcome::Ok, &t, 1, now + 10800, grid + 10800, both, 2);
    CHECK(s.taf[0].status == (uint8_t)TafStatus::Ok && s.taf[1].status == (uint8_t)TafStatus::Absent && s.taf[2].status == 0);
    metarStoreTafCycle(&s, c, MetarOutcome::NoData, nullptr, 0, now + 14400, grid + 14400, both, 2);
    CHECK(s.taf[1].status == (uint8_t)TafStatus::Absent && s.taf[2].status == 0 && s.last_taf_t == now + 14400);
  }
  {  // the TAF cycle's retries stay the TAF cycle; a 429 moves it to the next hour's :50
    MSim m;
    m.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 49, 0);
    CHECK(m.until(FwdKind::Fetch, 100000) == 60000 && m.s.runTaf());
    m.s.result(FwdKind::Fetch, MetarOutcome::Fail, m.in.now_ms, 0);
    CHECK(m.until(FwdKind::Fetch, 100000) == 30000 && m.s.runTaf());  // retry 1 at +30 s: still the TAF
    m.s.result(FwdKind::Fetch, MetarOutcome::Yield, m.in.now_ms, 0);
    CHECK(m.until(FwdKind::Fetch, 200000) == 120000 && m.s.runTaf());  // retry 2 at +120 s: still the TAF
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.until(FwdKind::Fetch, 1000000) > 0 && !m.s.runTaf());  // 11:05: reports
    MSim l;
    l.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 49, 0);
    CHECK(l.until(FwdKind::Fetch, 100000) == 60000 && l.s.runTaf());
    l.s.result(FwdKind::Fetch, MetarOutcome::Limited, l.in.now_ms, 0);  // 429 at 10:50: the next due >= 900 s later
    bool taf[4];
    for (int i = 0; i < 4; i++) {
      CHECK(l.until(FwdKind::Fetch, 1000000) > 0);
      taf[i] = l.s.runTaf();
      l.s.result(FwdKind::Fetch, MetarOutcome::Ok, l.in.now_ms, 0);
      l.adv(1000);
    }
    CHECK(!taf[0] && !taf[1] && !taf[2] && taf[3]);  // 11:05, 11:20, 11:35, 11:50 (TAF)
  }
  {  // a real-clock step after the anchoring moves nothing (the grid runs on the ms clock); a restart
    // (airports changed) anchors again on the clock it then sees -- the slot moves once
    MSim m;
    m.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 0, 0);
    CHECK(m.until(FwdKind::Fetch, 400000) == 300000 && !m.s.runTaf());  // 10:05
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    m.in.now_unix += 7200 + 600;  // the clock jumps 2 h 10 min
    bool taf[3];
    for (int i = 0; i < 3; i++) {
      m.adv(1000);
      CHECK(m.until(FwdKind::Fetch, 900000) == 899000);
      taf[i] = m.s.runTaf();
      m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    }
    CHECK(!taf[0] && !taf[1] && taf[2]);  // still every 4th on the ms grid
    m.s.airportsChanged();
    m.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 12, 49, 0);
    CHECK(m.until(FwdKind::Fetch, 100000) == 60000 && m.s.runTaf());  // re-anchored on the clock now seen: 12:50
  }
  {  // the ms clock wraps (49.7 d) between the anchoring and the TAF cycle: still every 4th
    MSim m;
    m.in.now_ms = 0xFFFFFFFFu - 1500000u;
    m.in.now_unix = (uint32_t)unixFromUtc(2026, 11, 14, 10, 0, 0);
    CHECK(m.until(FwdKind::Fetch, 400000) == 300000 && !m.s.runTaf());  // 10:05
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    bool taf[4];
    for (int i = 0; i < 4; i++) {
      m.adv(1000);
      CHECK(m.until(FwdKind::Fetch, 900000) == 899000);
      taf[i] = m.s.runTaf();
      m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    }
    CHECK(!taf[0] && !taf[1] && taf[2] && !taf[3]);  // 10:20, 10:35 (the wrap is between), 10:50 (TAF), 11:05
  }
}

static void testMetarSched() {
  {  // first due 60 s (+ phase) after it can run, then every 900 s; the full busy table holds it
    MSim m(25000);
    CHECK(m.until(FwdKind::Fetch, 100000) == 85000);  // 60 s + the 25-s phase
    m.s.result(FwdKind::Fetch, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.s.n.ok == 1 && m.s.nextS(m.in.now_ms) == 900);
    m.adv(1000);
    CHECK(m.until(FwdKind::Fetch, 1000000) == 899000);
    m.s.result(FwdKind::Fetch, MetarOutcome::NoData, m.in.now_ms, 0);
    CHECK(m.s.n.nodata == 1);
    MSim b;
    b.busy = FB_HTTP;  // a page request < 3 s ago, every second: never a fetch
    CHECK(b.until(FwdKind::Fetch, 360000) == UINT32_MAX);
    CHECK(b.s.n.skip == 1 && b.s.n.ok == 0);  // the window (300 s from the due) ended: one skip, nothing queued
    b.busy = 0;
    CHECK(b.until(FwdKind::Fetch, 900000) > 500000);  // the next cycle, not at once
    for (uint16_t bit : {FB_UPLOAD, FB_STORE, FB_LEARN, FB_HOUR, FB_MQTT, FB_RESTART, FB_LINK}) {
      MSim x;
      x.busy = bit;
      CHECK(x.until(FwdKind::Fetch, 120000) == UINT32_MAX);
    }
    MSim h;  // the heap holds a whole window: counted as heap, not skip
    NqCand c[NQ_MAX];
    for (int k = 0; k < 400; k++) {
      const int n = h.s.offer(h.in, c);
      const NqIn q{h.in.now_ms, 0, 20000, 15000, true, -1};
      FwdKind kd;
      h.s.picked(h.in, c, n, nqPick(q, c, n), &kd);
      h.adv(1000);
    }
    CHECK(h.s.n.heap == 1 && h.s.n.skip == 0);
  }
  {  // retries on fail / yield: +30 s, +120 s inside the window, then the cycle is missed
    MSim m;
    m.until(FwdKind::Fetch, 100000);
    const uint32_t t0 = m.in.now_ms;
    m.s.result(FwdKind::Fetch, MetarOutcome::Fail, m.in.now_ms, 0);
    CHECK(m.s.nextS(m.in.now_ms) == 30 && strcmp(m.s.stateName(), "retry") == 0);
    CHECK(m.until(FwdKind::Fetch, 40000) == 30000);
    m.s.result(FwdKind::Fetch, MetarOutcome::Yield, m.in.now_ms, 0);
    CHECK(m.until(FwdKind::Fetch, 200000) == 120000);
    m.s.result(FwdKind::Fetch, MetarOutcome::Fail, m.in.now_ms, 0);
    CHECK(m.s.n.fail == 2 && m.s.n.yield == 1 && m.s.n.missed == 1);
    CHECK(m.s.nextS(m.in.now_ms) == (long)((t0 + 900000 - m.in.now_ms + 999) / 1000));  // the grid holds
    // a retry that would cross the window's end: missed at once
    MSim w;
    w.busy = FB_STORE;
    w.until(FwdKind::Fetch, 335000);  // held until 276 s into the window
    w.busy = 0;
    w.until(FwdKind::Fetch, 30000);
    w.s.result(FwdKind::Fetch, MetarOutcome::Fail, w.in.now_ms, 0);
    CHECK(w.s.n.missed == 1 && strcmp(w.s.stateName(), "idle") == 0);
  }
  {  // 429: the cycle skipped, Retry-After honoured (at least one cycle)
    MSim m;
    m.until(FwdKind::Fetch, 100000);
    m.s.result(FwdKind::Fetch, MetarOutcome::Limited, m.in.now_ms, 1800);
    CHECK(m.s.n.limited == 1 && m.s.nextS(m.in.now_ms) >= 1800 && m.s.nextS(m.in.now_ms) <= 1800 + 900);
    MSim s;
    s.until(FwdKind::Fetch, 100000);
    s.s.result(FwdKind::Fetch, MetarOutcome::Limited, s.in.now_ms, 0);
    CHECK(s.s.nextS(s.in.now_ms) >= 900);
  }
  {  // untrusted / too_large pause; run once lifts it; airports changed lifts it; three no_memory pause
    MSim m;
    m.until(FwdKind::Fetch, 100000);
    m.s.result(FwdKind::Fetch, MetarOutcome::Untrusted, m.in.now_ms, 0);
    CHECK(m.s.paused() && m.s.n.untrusted == 1 && m.s.n.paused == 1 && m.s.nextS(m.in.now_ms) == -1);
    CHECK(m.until(FwdKind::Fetch, 2000000) == UINT32_MAX);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Accepted && !m.s.paused());
    CHECK(m.tick() == FwdKind::RunOnce);
    m.s.result(FwdKind::RunOnce, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(!m.s.paused() && m.s.n.ok == 1);
    MSim t;
    t.until(FwdKind::Fetch, 100000);
    t.s.result(FwdKind::Fetch, MetarOutcome::TooLarge, t.in.now_ms, 0);
    CHECK(t.s.paused());
    t.s.airportsChanged();
    CHECK(!t.s.paused() && t.until(FwdKind::Fetch, 100000) == 60000);  // the first due again
    MSim nm;
    for (int k = 0; k < 3; k++) {
      CHECK(nm.until(FwdKind::Fetch, 1000000) != UINT32_MAX);
      nm.s.result(FwdKind::Fetch, MetarOutcome::NoMemory, nm.in.now_ms, 0);
    }
    CHECK(nm.s.paused() && nm.s.n.heap == 3);
  }
  {  // run once and lookup: user priority, <= 30 s wait, 60 s apart, one at a time; off without the switch
    MSim m;
    CHECK(m.s.requestRun(m.in.now_ms, false) == MetarReq::Off);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Accepted);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Busy);
    CHECK(m.tick() == FwdKind::RunOnce && m.s.running());
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Busy);  // its own kind runs
    // a lookup pressed while the run once runs: queued behind it, started when the line is free
    CHECK(m.s.requestLookup(m.in.now_ms, true) == MetarReq::Accepted && m.s.lookupPending());
    CHECK(m.s.requestLookup(m.in.now_ms, true) == MetarReq::Busy);
    m.adv(2000);
    CHECK(m.tick() == FwdKind::None);  // the line is still taken
    m.s.result(FwdKind::RunOnce, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.tick() == FwdKind::Lookup);
    m.s.result(FwdKind::Lookup, MetarOutcome::Ok, m.in.now_ms, 0);
    m.adv(10000);
    CHECK(m.s.requestLookup(m.in.now_ms, true) == MetarReq::TooSoon);  // 60 s after the previous METAR job
    m.adv(50000);
    CHECK(m.s.requestLookup(m.in.now_ms, true) == MetarReq::Accepted);
    m.busy = FB_HTTP;  // a page load: the lookup waits, then gives up after 30 s
    for (int k = 0; k < 32; k++) {
      CHECK(m.tick() == FwdKind::None);
      m.adv(1000);
    }
    CHECK(!m.s.lookupPending() && m.s.takeLookupOutcome() == MetarUserOut::Busy && m.s.takeLookupOutcome() == MetarUserOut::None);
    m.busy = 0;
    m.adv(60000);
    CHECK(m.s.requestLookup(m.in.now_ms, true) == MetarReq::Accepted && m.tick() == FwdKind::Lookup);
    m.s.result(FwdKind::Lookup, MetarOutcome::Ok, m.in.now_ms, 0);
    CHECK(m.s.n.ok == 1);  // a lookup is not a fetch
    m.in.can_run = false;  // the switch went off with a run pending
    m.adv(60000);
    CHECK(m.s.requestRun(m.in.now_ms, true) == MetarReq::Accepted);
    m.tick();
    CHECK(m.s.takeRunOutcome() == MetarUserOut::Off && strcmp(m.s.stateName(), "off") == 0);
    MSim e;  // no airports: no fetch, a lookup still runs
    e.in.have_airports = false;
    CHECK(e.until(FwdKind::Fetch, 200000) == UINT32_MAX && e.s.nextS(e.in.now_ms) == -1);
    CHECK(e.s.requestLookup(e.in.now_ms, true) == MetarReq::Accepted && e.tick() == FwdKind::Lookup);
  }
  {  // on the queue with forwarding: a send outranks a fetch; a fetch yields to a send due within 15 s
    MSim m;
    CHECK(m.tick() == FwdKind::None);  // the first tick schedules the first due (60 s)
    m.adv(60000);
    m.sendDue = 10;
    CHECK(m.tick() == FwdKind::None && m.s.lastWhy() == FW_WHY_SEND_SOON);
    m.sendDue = 16;
    CHECK(m.tick() == FwdKind::Fetch);
    CHECK(strcmp(metarOutcomeName(MetarOutcome::TooLarge), "too_large") == 0 && strcmp(metarReqName(MetarReq::TooSoon), "too_soon") == 0);
  }
}

static void testMetarStore() {
  const uint32_t now = 1800000000u;
  MetarCfg c;
  metarCfgDefault(&c);
  CHECK(metarCfgValid(c) && !c.on && c.n == 0);
  c.n = 3;
  c.a[0] = apt3("KAAA", 30), c.a[1] = apt3("KBBB", 10), c.a[2] = apt3("KCCC", 60);
  CHECK(metarCfgValid(c) && metarCfgFind(c, "KBBB") == 1 && metarCfgFind(c, "KZZZ") < 0);
  {
    MetarCfg bad = c;
    bad.a[2] = apt3("KAAA", 5);
    CHECK(!metarCfgValid(bad));  // twice
    bad = c, bad.ver = 2;
    CHECK(!metarCfgValid(bad));
    bad = c, bad.a[1].lat = 9500;
    CHECK(!metarCfgValid(bad));
  }
  MetarStore s;
  memset(&s, 0, sizeof s);
  metarStoreClear(&s);
  {  // newer replaces, older never, a COR of the same time replaces; not ours -> -1
    CHECK(metarStorePut(&s, c, obs("KAAA", now - 600, 10180)) == 0);
    CHECK(metarStorePut(&s, c, obs("KAAA", now - 1200, 10100)) == -1 && s.obs[0].qnh_dhpa == 10180);
    CHECK(metarStorePut(&s, c, obs("KAAA", now - 600, 10170)) == -1);
    CHECK(metarStorePut(&s, c, obs("KAAA", now - 600, 10170, MO_COR)) == 0 && s.obs[0].qnh_dhpa == 10170);
    CHECK(metarStorePut(&s, c, obs("EDDF", now, 10100)) == -1);
    const uint32_t g = s.gen;
    CHECK(metarStorePut(&s, c, obs("KAAA", now - 300, 10175)) == 0 && s.gen == g + 1);
  }
  {  // stale at 90 min + 1 s; a failed fetch keeps the reading and its age grows
    const MetarObs o = obs("KBBB", now - METAR_STALE_S, 10200);
    CHECK(metarCurrent(o, now) && !metarCurrent(o, now + 1) && !metarCurrent(obs("KBBB", now + 60, 1), now));
  }
  {  // reference: the user's order -- weights 3 / 2 / 1 by position, `local` = the first current airport,
     // the first current QNH alone over the spread (the km play no part)
    metarStoreClear(&s);
    metarStorePut(&s, c, obs("KAAA", now - 600, 10181));  // 30 km
    metarStorePut(&s, c, obs("KBBB", now - 900, 10170));  // 10 km
    metarStorePut(&s, c, obs("KCCC", now - 7200, 10000)); // 60 km, stale: out
    MetarRef r = metarReference(c, s, now);
    // weights 3 (first) and 2 (second): (3 * 10181 + 2 * 10170) / 5 = 10176.6 -> 10177
    CHECK(r.local == 0 && r.qnh_dhpa == 10177 && r.qnh_mask == 3 && !r.inconsistent);
    metarStorePut(&s, c, obs("KAAA", now - 60, 10190));  // 2.0 hPa apart: the first current alone
    r = metarReference(c, s, now);
    CHECK(r.inconsistent && r.qnh_dhpa == 10190 && r.qnh_mask == 1 && r.local == 0);
    MetarStore one = s;
    one.obs[0].qnh_dhpa = METAR_I16_UNSET;  // the first without a QNH: local stays it, QNH from the second
    r = metarReference(c, one, now);
    CHECK(r.local == 0 && r.qnh_dhpa == 10170 && r.qnh_mask == 2 && !r.inconsistent);
    MetarStore late = s;
    late.obs[0].obs_unix = now - METAR_STALE_S - 1;  // the first stale: local and the QNH move to the next current one
    r = metarReference(c, late, now);
    CHECK(r.local == 1 && r.qnh_dhpa == 10170 && r.qnh_mask == 2);
    MetarCfg unk = c;  // a typed code without a location: the same rule (the order, not the km)
    for (int i = 0; i < 3; i++) unk.a[i].km = AIRPORT_KM_UNKNOWN;
    metarStorePut(&s, unk, obs("KAAA", now - 30, 10175, MO_COR));
    r = metarReference(unk, s, now);
    CHECK(r.local == 0 && r.qnh_dhpa == 10173);  // (3 * 10175 + 2 * 10170) / 5 = 10173
    MetarStore three;  // three current within 1.5 hPa, weights 3 / 2 / 1: (3 * 10176 + 2 * 10170 + 10164) / 6 = 10172
    metarStoreClear(&three);
    metarStorePut(&three, c, obs("KAAA", now - 60, 10176));
    metarStorePut(&three, c, obs("KBBB", now - 60, 10170));
    metarStorePut(&three, c, obs("KCCC", now - 60, 10164));
    r = metarReference(c, three, now);
    CHECK(r.local == 0 && r.qnh_dhpa == 10172 && r.qnh_mask == 7 && !r.inconsistent);
    MetarStore none;
    memset(&none, 0, sizeof none);
    r = metarReference(c, none, now);
    CHECK(r.local == -1 && r.qnh_dhpa == METAR_I16_UNSET && r.qnh_mask == 0);
  }
  {  // a v1 blob with long (cut) names -- shortened in RAM at load, idempotent
    MetarCfg old;
    metarCfgDefault(&old);
    old.on = 1, old.n = 3;
    snprintf(old.a[0].icao, sizeof old.a[0].icao, "EGLL");
    snprintf(old.a[0].name, sizeof old.a[0].name, "%s", "London/Heathrow Internat");
    snprintf(old.a[1].icao, sizeof old.a[1].icao, "EGKK");
    snprintf(old.a[1].name, sizeof old.a[1].name, "%s", "London/Gatwick Arpt");
    snprintf(old.a[2].icao, sizeof old.a[2].icao, "ZZZZ");
    snprintf(old.a[2].name, sizeof old.a[2].name, "%s", "Sample Field");
    CHECK(metarCfgValid(old));
    MetarCfg ld = old;
    metarCfgShortNames(&ld);
    CHECK(strcmp(ld.a[0].name, "Heathrow") == 0 && strcmp(ld.a[1].name, "Gatwick") == 0 && strcmp(ld.a[2].name, "Sample Field") == 0);
    CHECK(ld.ver == old.ver && ld.n == 3 && !strcmp(ld.a[0].icao, "EGLL") && metarCfgValid(ld));  // the airports are kept (a blob version migrates, never resets)
    MetarCfg again = ld;
    metarCfgShortNames(&again);
    CHECK(memcmp(&again, &ld, sizeof ld) == 0);
    // a reorder is the same set; a swap of an airport is not
    MetarCfg re = ld;
    re.a[0] = ld.a[2], re.a[2] = ld.a[0];
    CHECK(metarSameSet(ld, re) && metarSameSet(re, ld));
    MetarCfg other = ld;
    snprintf(other.a[1].icao, sizeof other.a[1].icao, "EGSS");
    CHECK(!metarSameSet(ld, other));
    MetarCfg fewer = ld;
    fewer.n = 2;
    CHECK(!metarSameSet(ld, fewer));
  }
  {  // remap when the airports change: kept ones move, the others go
    MetarCfg after = c;
    after.n = 2;
    after.a[0] = c.a[2], after.a[1] = c.a[0];
    metarStoreClear(&s);
    metarStorePut(&s, c, obs("KAAA", now - 60, 10180));
    metarStorePut(&s, c, obs("KBBB", now - 60, 10170));
    metarStoreRemap(&s, c, after);
    CHECK(!(s.obs[0].flags & MO_VALID) && s.obs[1].qnh_dhpa == 10180 && !(s.obs[2].flags & MO_VALID));
  }
  {  // /api/metar JSON
    metarStoreClear(&s);
    MetarObs a = obs("KAAA", now - 600, 10181, MO_AUTO | MO_VIS_GE);
    a.wdir = 320, a.wind_dkmh = 93, a.vis_m = 10000, a.cover = 0, a.t_dc = 60, a.td_dc = -20;
    metarStorePut(&s, c, a);
    MetarObs b = obs("KBBB", now - 900, 10170);
    b.wdir = METAR_WDIR_VRB, b.wind_dkmh = 37, b.gust_dkmh = METAR_U16_UNSET, b.cover = 4, b.ceil_ft = 1500;
    b.wx = MW_PRECIP | MW_FOG, b.wx_int = -1, b.t_dc = -5, b.td_dc = -15, b.vis_m = 4828;
    metarStorePut(&s, c, b);
    s.last_t = now - 600, s.last = MetarOutcome::Ok;
    const std::string j = metarJsonStr(c, s, true, now);
    CHECK(jsonParses(j));
    CHECK(j.find("\"on\":true") != std::string::npos && j.find("\"icao\":\"KAAA\",\"name\":\"KAAA field\",\"cc\":\"US\",\"fp\":3,\"km\":30") != std::string::npos);
    CHECK(j.find("\"age_s\":600,\"stale\":false,\"cur\":true") != std::string::npos && j.find("\"qnh_hpa\":1018.1") != std::string::npos);
    CHECK(j.find("\"wind_dir\":\"vrb\",\"wind_kmh\":3.7,\"gust_kmh\":null") != std::string::npos);
    CHECK(j.find("\"t_c\":-0.5,\"td_c\":-1.5") != std::string::npos && j.find("\"cover\":\"overcast\",\"ceil_ft\":1500") != std::string::npos);
    CHECK(j.find("\"wx\":{\"precip\":true,\"rain\":false,\"drizzle\":false,\"snow\":false,\"freezing\":false,\"thunder\":false,"
                 "\"fog\":true,\"mist\":false,\"int\":-1}") != std::string::npos);
    CHECK(j.find("{\"icao\":\"KCCC\",\"name\":\"KCCC field\",\"cc\":\"US\",\"fp\":3,\"km\":60,\"obs\":null,\"age_s\":null,\"stale\":true,\"cur\":false,\"taf\":null}") != std::string::npos);
    CHECK(j.find("\"reference\":{\"qnh_hpa\":1017.7,\"qnh_from\":[{\"icao\":\"KAAA\",\"km\":30,\"age_s\":600},"
                 "{\"icao\":\"KBBB\",\"km\":10,\"age_s\":900}],\"qnh_inconsistent\":false,\"local\":{\"icao\":\"KAAA\"") !=
          std::string::npos);  // the first in the user's order, not the nearest
    CHECK(j.find("\"local_values\":{\"from\":\"KAAA\",\"vis_m\":10000") != std::string::npos);
    CHECK(j.find("\"last\":{\"t\":") != std::string::npos && j.find("\"result\":\"ok\"}") != std::string::npos);
    CHECK(j.substr(j.find("\"reference\"")).find("wind") == std::string::npos);  // wind never in the reference
    const std::string empty = metarJsonStr(c, MetarStore{}, false, now);
    CHECK(jsonParses(empty) && empty.find("\"reference\":null") != std::string::npos &&
          empty.find("\"last\":{\"t\":null,\"result\":null}") != std::string::npos);
    MetarCfg zero;
    metarCfgDefault(&zero);
    const std::string z = metarJsonStr(zero, s, false, now);
    CHECK(jsonParses(z) && z.find("\"airports\":[],\"reference\":null") != std::string::npos);
  }
  {  // admin POST: codes from the current list or the last lookup; at most three; no duplicates
    MetarLookup look;
    memset(&look, 0, sizeof look);
    look.n = 2;
    look.a[0] = apt3("EGLL", 12), look.a[1] = apt3("EGKK", 40);
    MetarCfg out;
    const char* err = nullptr;
    const char* b1 = "{\"on\":true,\"airports\":[\"KBBB\",\"EGLL\"]}";
    CHECK(metarCfgFromJson(b1, strlen(b1), c, look, &out, &err) && out.on && out.n == 2 &&
          strcmp(out.a[0].icao, "KBBB") == 0 && out.a[1].km == 12 && metarCfgValid(out));
    const char* b2 = "{\"airports\":[\"EDDF\"]}";
    CHECK(!metarCfgFromJson(b2, strlen(b2), c, look, &out, &err) && strcmp(err, "look the airport up first") == 0);
    const char* b3 = "{\"airports\":[\"EGLL\",\"EGLL\"]}";
    CHECK(!metarCfgFromJson(b3, strlen(b3), c, look, &out, &err) && strcmp(err, "an airport twice") == 0);
    const char* b4 = "{\"airports\":[\"KAAA\",\"KBBB\",\"KCCC\",\"EGLL\"]}";
    CHECK(!metarCfgFromJson(b4, strlen(b4), c, look, &out, &err) && strcmp(err, "at most 3 airports") == 0);
    const char* b5 = "{\"on\":false}";
    CHECK(metarCfgFromJson(b5, strlen(b5), c, look, &out, &err) && !out.on && out.n == 3);  // the airports kept
    const char* b6 = "{\"airports\":[\"egll\"]}";
    CHECK(!metarCfgFromJson(b6, strlen(b6), c, look, &out, &err));
    const char* b7 = "{\"airports\":[]}";
    CHECK(metarCfgFromJson(b7, strlen(b7), c, look, &out, &err) && out.n == 0 && metarCfgValid(out));
    CHECK(!metarCfgFromJson("[", 1, c, look, &out, &err) && !metarCfgFromJson("{\"on\":1}", 8, c, look, &out, &err));
    // the user's own display name -- trimmed, cut to 24 B on a UTF-8 boundary, kept by lookups,
    // following its airport through a reorder; "" gives the name back to the lookup's (or none until a lookup has it)
    MetarCfg u;
    const char* n1 = "{\"names\":{\"KBBB\":\"  London Gatwick  \"}}";
    CHECK(metarCfgFromJson(n1, strlen(n1), c, look, &u, &err) && !strcmp(u.a[1].name, "London Gatwick") && metarNameUser(u, 1) &&
          !metarNameUser(u, 0) && metarCfgValid(u));
    MetarCfg r2;
    const char* n2 = "{\"airports\":[\"KBBB\",\"KAAA\"]}";  // the flag moves with KBBB to place 0; KCCC's place clears
    CHECK(metarCfgFromJson(n2, strlen(n2), u, look, &r2, &err) && metarNameUser(r2, 0) && !metarNameUser(r2, 1) &&
          r2.reserved == 1 && !strcmp(r2.a[0].name, "London Gatwick"));
    MetarCfg sn = u;  // the load-time shortening leaves the user's name as typed
    snprintf(sn.a[1].name, sizeof sn.a[1].name, "%s", "Big/Field Intl");
    metarCfgShortNames(&sn);
    CHECK(!strcmp(sn.a[1].name, "Big/Field Intl"));
    MetarLookup lk2 = look;  // a lookup refreshes only the names the user did not type
    lk2.n = 3, lk2.a[2] = apt3("KBBB", 10), lk2.a[0] = apt3("KAAA", 30);
    snprintf(lk2.a[2].name, sizeof lk2.a[2].name, "%s", "Bravo");
    snprintf(lk2.a[0].name, sizeof lk2.a[0].name, "%s", "Alpha");
    MetarCfg rf = u;
    CHECK(metarLookupNames(&rf, lk2) && !strcmp(rf.a[0].name, "Alpha") && !strcmp(rf.a[1].name, "London Gatwick"));
    CHECK(!metarLookupNames(&rf, lk2));  // nothing new the second time
    MetarCfg cl;  // "" clears the flag: the last lookup's name, or none
    const char* n3 = "{\"names\":{\"KBBB\":\"\"}}";
    CHECK(metarCfgFromJson(n3, strlen(n3), u, lk2, &cl, &err) && !metarNameUser(cl, 1) && !strcmp(cl.a[1].name, "Bravo"));
    // without the airport in the lookup the last known name stays, no longer the user's (a lookup replaces it)
    CHECK(metarCfgFromJson(n3, strlen(n3), u, look, &cl, &err) && !metarNameUser(cl, 1) && !strcmp(cl.a[1].name, "London Gatwick") &&
          metarCfgValid(cl));
    CHECK(metarLookupNames(&cl, lk2) && !strcmp(cl.a[1].name, "Bravo"));
    const char* n4 = "{\"names\":{\"KBBB\":\"Zażółć gęślą jaźń lotnisko\"}}";  // 33 B of UTF-8: cut to <= 24 on a boundary
    CHECK(metarCfgFromJson(n4, strlen(n4), c, look, &cl, &err) && strlen(cl.a[1].name) <= 24 &&
          (((unsigned char)cl.a[1].name[strlen(cl.a[1].name) - 1]) & 0xC0) != 0xC0 && metarCfgValid(cl));
    const char* n5 = "{\"names\":{\"KBBB\":\"a\\u0007b\"}}";
    CHECK(!metarCfgFromJson(n5, strlen(n5), c, look, &cl, &err) && strcmp(err, "names: no control characters") == 0);
    const char* n6 = "{\"names\":{\"EDDF\":\"x\"}}";
    CHECK(!metarCfgFromJson(n6, strlen(n6), c, look, &cl, &err) && strcmp(err, "names: not one of your airports") == 0);
    const char* n7 = "{\"names\":{\"KBBB\":\"<b>&\\\"\"}}";  // stored as typed; every face escapes it (JSON out escapes the quote)
    CHECK(metarCfgFromJson(n7, strlen(n7), c, look, &cl, &err) && !strcmp(cl.a[1].name, "<b>&\""));
  }
}
