// test_taf.inc -- the TAF parser and its next-12-h windows. Real shapes from
// a raw AWC answer (public data, distant airports); synthetic ones use ZZZZ. Cells are one hour (taf.h), so
// a window is whole hours clipped to [now, now + 12 h) and to the TAF's validity.
#include "taf.h"

namespace {
const char* const kTafWant[] = {"KJFK", "UUEE", "LFPG", "EGLL", "YSSY", "KDEN", "ZZZZ"};

std::string hhmm(uint32_t t) {
  char b[8];
  snprintf(b, sizeof b, "%02u:%02u", (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
  return b;
}
// the windows as text: "rain p 21:00-00:00; gusts e 21:00-03:00"
std::string wins(const TafObs& t, uint32_t now) {
  TafWindow w[16];
  const int n = tafWindows(t, now, w, 16);
  std::string s;
  for (int i = 0; i < n; i++)
    s += (i ? "; " : "") + std::string(tafWhatName(w[i].what)) + (w[i].possible ? " p " : " e ") + hhmm(w[i].from) + "-" +
         hhmm(w[i].to);
  return s;
}
// a body through the line sink (any split) into the parser; finish = a clean end
struct TafRun {
  TafParser p;
  explicit TafRun(uint32_t now) : p(kTafWant, 7, now) {}
  void feed(const std::string& body, size_t step = 0, bool clean = true) {
    MetarLineSink k(TafParser::lineFn, &p);
    if (!step) step = body.size() ? body.size() : 1;
    for (size_t i = 0; i < body.size(); i += step) k.feed(body.data() + i, std::min(step, body.size() - i));
    if (clean) {
      k.finish();
      p.finish();
    } else {
      k.drop();
    }
  }
  const TafObs* get(const char* icao) const {
    for (int i = 0; i < p.count(); i++)
      if (!strcmp(p.at(i).icao, icao)) return &p.at(i);
    return nullptr;
  }
};
std::string winsOf(const std::string& body, const char* icao, uint32_t now) {
  TafRun r(now);
  r.feed(body);
  const TafObs* t = r.get(icao);
  return t ? wins(*t, now) : std::string("<none>");
}
uint32_t utc(int y, int mo, int d, int h, int mi) { return (uint32_t)unixFromUtc(y, mo, d, h, mi, 0); }
}  // namespace

static void testTaf() {
  const uint32_t n13 = utc(2026, 11, 13, 21, 0);
  // AWC's raw layout: one group per line, two-space indent, a trailing space, PROBnn alone before its TEMPO
  const std::string kjfk = "METAR KJFK 132051Z 35016KT 10SM -RA SCT013 BKN047 OVC070 05/03 A2966 RMK AO2 SLP043\n"
                           "TAF KJFK 132035Z 1321/1424 35014G20KT P6SM VCSH BKN020 BKN040 \n"
                           "  FM140000 34011KT P6SM SCT018 BKN040 \n"
                           "  TEMPO 1400/1404 BKN018 \n"
                           "  FM141400 31015G25KT P6SM BKN050";
  const std::string uuee = "TAF UUEE 132016Z 1321/1421 15008G13MPS 6000 BKN010 TX05/1321Z TN02/1406Z \n"
                           "  TEMPO 1321/1324 15011G17MPS 3000 -SHRA BKN005 BKN016CB \n"
                           "  BECMG 1401/1403 29006MPS \n"
                           "  BECMG 1407/1409 24008G15MPS BKN016";
  const std::string lfpg = "TAF LFPG 131700Z 1318/1424 02005KT CAVOK TX09/1414Z TN02/1406Z \n"
                           "  BECMG 1410/1412 10010KT \n"
                           "  PROB40 \n"
                           "  TEMPO 1422/1424 4000 -RA BKN013";
  // a 20-kt gust (37 km/h), VCSH, a TEMPO with clouds only: nothing; the 25-kt FM is beyond the 12 h
  CHECK(winsOf(kjfk, "KJFK", n13) == "");
  // rain possible (TEMPO -SHRA); gusts expected (13 m/s) until the first BECMG ends without a gust; possible in the
  // second BECMG's transition (15 m/s); the TEMPO 17-m/s gust lies under the expected one
  CHECK(winsOf(uuee, "UUEE", n13) == "rain p 21:00-00:00; gusts e 21:00-03:00; gusts p 07:00-09:00");
  // PROB40 on its own line before TEMPO; the window ends with the validity
  CHECK(winsOf(lfpg, "LFPG", utc(2026, 11, 14, 15, 0)) == "rain p 22:00-00:00");
  // BR at 4 000 m is no fog; a TEMPO with a visibility only: nothing
  CHECK(winsOf("TAF EGLL 131700Z 1318/1424 22010KT 9999 SCT030\n  TEMPO 1400/1410 6000\n  PROB30 1402/1408 4000 BR\n"
               "  BECMG 1410/1412 15010KT", "EGLL", n13) == "");
  // an amended TAF; PROB30 TEMPO with TS, rain and a 45-kt gust (thunder first, then rain, then gusts)
  CHECK(winsOf("TAF AMD YSSY 132200Z 1322/1424 27010KT 9999 SCT030\n  TEMPO 1402/1412 29015G25KT 3000 SHRA BKN020\n"
               "  PROB30\n  TEMPO 1404/1412 VRB30G45KT 2000 TSRA SCT012 SCT070CB", "YSSY", utc(2026, 11, 13, 22, 0)) ==
        "thunder p 04:00-10:00; rain p 02:00-10:00; gusts p 02:00-10:00");
  const uint32_t n14 = utc(2026, 11, 14, 12, 30);
  // FM replaces everything: rain until 17:00, nothing after (the 03:00 thunder is past the edge)
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA BKN015\n  FM141700 27008KT 9999 SCT030\n"
               "  FM150300 18012G42KT 3000 +TSRA BKN010CB", "ZZZZ", n14) == "rain e 12:30-17:00");
  // a BECMG without weather keeps the rain; NSW ends it at its period's end
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA BKN015\n  BECMG 1414/1415 30012KT\n"
               "  BECMG 1418/1419 30012KT 9999 NSW SCT030", "ZZZZ", n14) == "rain e 12:30-19:00");
  // 11 m/s is no gust, 12 m/s is (43 km/h); the window clipped at the 12-h edge
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27008G11MPS 9999 SCT030\n  TEMPO 1500/1503 27010G12MPS", "ZZZZ", n14) ==
        "gusts p 00:00-00:30");
  // snow expected all along; freezing rain possible
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27008KT 9999 SN OVC010\n  TEMPO 1500/1503 FZRA", "ZZZZ", n14) ==
        "snow e 12:30-00:30; rain p 00:00-00:30");
  // a 9-h TAF, a TEMPO to hour 24: clipped to the validity
  CHECK(winsOf("TAF ZZZZ 140600Z 1406/1415 27010KT 9999 SCT030\n  TEMPO 1412/1424 4000 -RA BKN015", "ZZZZ",
               utc(2026, 11, 14, 7, 0)) == "rain p 12:00-15:00");
  // INTER as TEMPO
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 SCT030\n  INTER 1414/1418 3000 TSRA", "ZZZZ", n14) ==
        "thunder p 14:00-18:00; rain p 14:00-18:00");
  // QNH...INS, an icing group, wind shear and the remarks change nothing
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 SCT030 QNH2992INS 610805 WS020/20055KT\n"
               "  TEMPO 1414/1418 -RA RMK NXT FCST BY 141800Z", "ZZZZ", n14) == "rain p 14:00-18:00");
  // the month end (Oct -> Nov) and the year end (Dec -> Jan)
  const std::string den = "TAF KDEN 311651Z 3118/0124 34006KT P6SM SCT220\n  FM010100 VRB06KT P6SM BKN220\n"
                          "  PROB30 0102/0106 -SHRA BKN060\n  FM011700 30010KT P6SM";
  CHECK(winsOf(den, "KDEN", utc(2026, 10, 31, 21, 0)) == "rain p 02:00-06:00");
  CHECK(winsOf(den, "KDEN", utc(2026, 12, 31, 21, 0)) == "rain p 02:00-06:00");
  {  // NIL and CNL: a status, no windows, not current
    TafRun r(n13);
    r.feed("TAF LFPG 131700Z NIL\nTAF AMD EGLL 131800Z 1318/1424 CNL");
    CHECK(r.get("LFPG") && r.get("LFPG")->status == (uint8_t)TafStatus::Nil && !tafCurrent(*r.get("LFPG"), n13));
    CHECK(r.get("EGLL") && r.get("EGLL")->status == (uint8_t)TafStatus::Cnl && wins(*r.get("EGLL"), n13) == "");
  }
  // fog: FG or BR below 1 000 m, expected and possible
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 00000KT 0300 FG VV001\n  FM141500 27005KT 9999 SCT030\n"
               "  TEMPO 1418/1421 0800 BR", "ZZZZ", n14) == "fog e 12:30-15:00; fog p 18:00-21:00");
  {  // METAR and TAF lines for several ids in a non-request order; each block to its id; METAR lines counted apart
    TafRun r(n13);
    r.feed("METAR UUEE 132100Z 16006MPS 9000 -SHRA SCT010 05/04 Q0992 NOSIG\n" + uuee + "\nMETAR KJFK 132051Z 35016KT 10SM\n" +
           kjfk.substr(kjfk.find("TAF")) + "\n" + "METAR EGLL 132050Z AUTO 25010KT 9999 NCD 09/06 Q1015\n" + lfpg);
    CHECK(r.p.count() == 3 && r.get("UUEE") && r.get("KJFK") && r.get("LFPG") && !r.get("EGLL"));
    CHECK(r.p.other() == 3 && r.p.garbage() == 0 && r.p.unwanted() == 0);
    CHECK(wins(*r.get("UUEE"), n13) == "rain p 21:00-00:00; gusts e 21:00-03:00; gusts p 07:00-09:00");
    // every split gives the same grids
    const std::string body = uuee + "\n" + lfpg;
    int same = 0, total = 0;
    for (size_t step : {(size_t)1, (size_t)3, (size_t)7, (size_t)64}) {
      TafRun s(n13);
      s.feed(body, step);
      total++;
      same += s.p.count() == 2 && s.get("UUEE") && !memcmp(s.get("UUEE")->cell, r.get("UUEE")->cell, sizeof r.get("UUEE")->cell);
    }
    CHECK(same == total);
    // a body cut inside the third block's header: a block is committed only once the next one starts (or the body ends
    // cleanly) -- the first stands, the second (whose last lines may be the cut ones) and the third are never committed
    TafRun c(n13);
    c.feed(uuee + "\n" + lfpg + "\nTAF EGLL 131700Z 1318/1424 22010KT 99", 0, false);
    CHECK(c.p.count() == 1 && c.get("UUEE") && !c.get("LFPG") && !c.get("EGLL"));
  }
  {  // an issue time 40 h old, 12 min ahead: rejected; an airport not asked for: unwanted
    TafRun r(n13);
    r.feed("TAF ZZZZ 120400Z 1206/1312 27010KT 9999 -RA\nTAF ZZZZ 132112Z 1321/1424 27010KT 9999 -RA\n"
           "TAF EDDF 132000Z 1321/1424 27010KT 9999 -RA");
    CHECK(r.p.count() == 0 && r.p.garbage() == 2 && r.p.unwanted() == 1);
  }
  {  // the stale rule: past the validity or past the grid, nothing current
    TafRun r(n13);
    r.feed(lfpg);
    const TafObs& t = *r.get("LFPG");
    CHECK(tafCurrent(t, n13) && !tafCurrent(t, utc(2026, 11, 15, 0, 0)) && !tafCurrent(t, n13 + 13 * 3600));
    CHECK(TAF_HOURS == 13 && sizeof(TafObs) <= 48);  // the per-airport model stays small (x3 in RAM)
  }
  // ---- edge cases ----
  // a BECMG whose weather token has no bits of ours (HZ) still restates the weather: the rain ends at its end
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 4000 -RA BKN015\n  BECMG 1416/1418 6000 HZ", "ZZZZ", n14) ==
        "rain e 12:30-18:00");
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 4000 -RA BKN015\n  BECMG 1416/1418 6000 VCSH", "ZZZZ", n14) ==
        "rain e 12:30-18:00");
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 4000 -RA BKN015\n  BECMG 1416/1418 6000", "ZZZZ", n14) ==
        "rain e 12:30-00:30");  // no weather token: the rain goes on
  {  // a header with no validity is no forecast: it never replaces the good one before it
    TafRun r(n14);
    r.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA\nTAF AMD ZZZZ 141230Z\n");
    CHECK(r.p.count() == 1 && r.get("ZZZZ")->issue_unix == utc(2026, 11, 14, 12, 0) && r.p.garbage() == 1);
    CHECK(wins(*r.get("ZZZZ"), n14) == "rain e 12:30-00:30");
    TafRun only(n14);
    only.feed("TAF ZZZZ 141200Z");
    CHECK(only.p.count() == 0 && only.p.garbage() == 1);
  }
  {  // a line over the sink's 191 B is never parsed: its block is dropped; after RMK it changes nothing
    std::string longLine = "  TEMPO 1414/1418 -RA";
    while (longLine.size() < 260) longLine += " SCT030";
    TafRun r(n14);
    r.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 SCT030\n" + longLine + "\n  FM141600 27010KT 9999 -SN\n");
    CHECK(r.p.count() == 0 && r.p.garbage() == 1);
    TafRun rmk(n14);
    rmk.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA RMK\n" + longLine + "\n");
    CHECK(rmk.p.count() == 1 && wins(*rmk.get("ZZZZ"), n14) == "rain e 12:30-00:30");
    TafRun other(n14);  // a cut METAR line ends the open block like an uncut one
    other.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA\nMETAR ZZZZ" + longLine + "\n");
    CHECK(other.p.count() == 1 && other.p.other() == 1);
  }
  {  // day 1-31, hour 0-24 (24 only at :00), minute 0-59; anything else is no time
    CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 SCT030\n  TEMPO 1425/1428 -RA\n  FM142460 30010KT 9999 -SN\n"
                 "  BECMG 1499/1416 TSRA", "ZZZZ", n14) == "");
    CHECK(winsOf("TAF ZZZZ 141200Z 1412/1424 27010KT 9999 -RA", "ZZZZ", n14) == "rain e 12:30-00:00");
    TafRun r(n14);
    r.feed("TAF ZZZZ 141200Z 3212/3312 27010KT 9999 -RA");
    CHECK(r.p.count() == 0 && r.p.garbage() == 1);
  }
  {  // shown for 3 h from the fetch hour (one missed hourly cycle tolerated); the horizon = min(grid + 13 h, valid_to)
    TafRun r(n14);
    r.feed("TAF ZZZZ 141200Z 1412/1518 27010KT 9999 -RA");
    const TafObs& t = *r.get("ZZZZ");
    const uint32_t g = utc(2026, 11, 14, 12, 0);
    CHECK(t.grid_unix == g && tafCurrent(t, g + 3 * 3600 - 1) && !tafCurrent(t, g + 3 * 3600));
    CHECK(tafHorizon(t) == g + 13 * 3600);
    TafRun s(n14);
    s.feed("TAF ZZZZ 141200Z 1412/1418 27010KT 9999 -RA");
    CHECK(tafHorizon(*s.get("ZZZZ")) == utc(2026, 11, 14, 18, 0) && !tafCurrent(*s.get("ZZZZ"), utc(2026, 11, 14, 18, 0)));
    CHECK(!tafCurrent(t, g - 3600));  // a clock stepped back an hour before the fetch hour: not shown
    // cut: a window that runs to where the data stops has no forecast end -- at now + 12 h (off the hour), and at the
    // grid's end when the forecast is shown late (a whole hour that would otherwise read as a real end)
    TafWindow w[4];
    CHECK(tafWindows(t, n14, w, 4) == 1 && w[0].to == n14 + 12 * 3600 && w[0].cut);
    CHECK(tafWindows(t, g + 2 * 3600 + 900, w, 4) == 1 && w[0].to == g + 13 * 3600 && w[0].cut);
    CHECK(tafWindows(*s.get("ZZZZ"), n14, w, 4) == 1 && w[0].to == utc(2026, 11, 14, 18, 0) && !w[0].cut);  // the validity's end
    TafRun f(n14);
    f.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA BKN015\n  FM141700 27008KT 9999 SCT030");
    CHECK(tafWindows(*f.get("ZZZZ"), n14, w, 4) == 1 && w[0].to == utc(2026, 11, 14, 17, 0) && !w[0].cut);  // FM ends it
  }
  {  // started: the run's first hour began at or before now, so `from` is now (no forecast start) -- the page says
     // "throughout" (also cut) or "until {to}"; a run that starts later is not started
    TafRun r(n14);
    r.feed("TAF ZZZZ 141200Z 1412/1518 27010KT 9999 -RA");
    const TafObs& t = *r.get("ZZZZ");
    TafWindow w[4];
    CHECK(tafWindows(t, n14, w, 4) == 1 && w[0].from == n14 && w[0].started && w[0].cut);  // began before now, past the horizon
    CHECK(tafWindows(t, utc(2026, 11, 14, 13, 0), w, 4) == 1 && w[0].from == utc(2026, 11, 14, 13, 0) && w[0].started);  // on the hour
    TafRun e(n14);
    e.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 -RA BKN015\n  FM141500 27008KT 9999 SCT030");
    CHECK(tafWindows(*e.get("ZZZZ"), n14, w, 4) == 1 && w[0].from == n14 && w[0].to == utc(2026, 11, 14, 15, 0) && w[0].started &&
          !w[0].cut);  // began before now, ends inside the horizon
    TafRun l(n14);
    l.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 SCT030\n  FM141500 27008KT 9999 -RA BKN015");
    CHECK(tafWindows(*l.get("ZZZZ"), n14, w, 4) == 1 && w[0].from == utc(2026, 11, 14, 15, 0) && !w[0].started && w[0].cut);  // starts in 2.5 h
    // two runs of one phenomenon, only the first began before now -- the flag is per run
    TafRun two(n14);
    two.feed("TAF ZZZZ 141200Z 1412/1512 27010KT 9999 SCT030\n  TEMPO 1412/1414 -RA\n  FM141600 27008KT 9999 -RA BKN015");
    CHECK(tafWindows(*two.get("ZZZZ"), n14, w, 4) == 2 && w[0].what == TafWhat::Rain && w[0].possible && w[0].from == n14 &&
          w[0].started && w[1].what == TafWhat::Rain && !w[1].possible && w[1].from == utc(2026, 11, 14, 16, 0) && !w[1].started);
  }
  {  // two blocks for one airport -- the later issue wins in either order
    const std::string a = "TAF ZZZZ 141100Z 1412/1512 27010KT 9999 -RA", b = "TAF AMD ZZZZ 141200Z 1412/1512 27010KT 9999 -SN";
    CHECK(winsOf(a + "\n" + b, "ZZZZ", n14) == "snow e 12:30-00:30");
    CHECK(winsOf(b + "\n" + a, "ZZZZ", n14) == "snow e 12:30-00:30");
  }
  // a gust in km/h (40 km/h is the bar), fog from a visibility in statute miles (1/2 SM = 804 m; 1 1/2 SM is not fog)
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27030G45KMH 9999 SCT030", "ZZZZ", n14) == "gusts e 12:30-00:30");
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27020G35KMH 9999 SCT030", "ZZZZ", n14) == "");
  CHECK(winsOf("TAF ZZZZ 141200Z 1412/1512 27005KT 1/2SM FG VV002\n  FM141600 27005KT 1 1/2SM BR", "ZZZZ", n14) ==
        "fog e 12:30-16:00");
  {  // at most 8 windows, in priority order -- thunder first, gusts the first to go
    TafObs t = {};
    memcpy(t.icao, "ZZZZ", 5);
    t.status = (uint8_t)TafStatus::Ok, t.issue_unix = n14 - 1800, t.valid_to = n14 + 30 * 3600, t.grid_unix = n14 - n14 % 3600;
    for (int k = 0; k < TAF_HOURS; k++) t.cell[k] = (uint16_t)(k % 2 ? 0x3FF : 0x2AA);
    TafWindow w[8];
    CHECK(tafWindows(t, n14, w, 8) == 8 && w[0].what == TafWhat::Thunder && w[7].what != TafWhat::Gusts);
  }
}
