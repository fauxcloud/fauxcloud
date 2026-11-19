// test_metar.inc -- the METAR line parser and the line splitter. Real lines: AWC format=raw
// (public data, distant airports); synthetic lines use ZZZZ and say so.
#include "metar.h"
#include "metar_store.h"  // METAR_AIRPORTS_MAX

namespace {
const char* const kWant[] = {"KJFK", "EGLL", "LFPG", "ZZZZ"};
MetarResult mp(const char* s, MetarObs& o, uint32_t now) { return metarParse(s, strlen(s), kWant, 4, now, o); }

struct LineSeen {
  std::vector<std::string> lines;
  int trunc = 0;
  static void fn(void* c, const char* l, size_t n, bool t) {
    LineSeen* s = (LineSeen*)c;
    s->lines.emplace_back(l, n);
    s->trunc += t;
  }
};
}  // namespace

static void testMetar() {
  const uint32_t now = (uint32_t)unixFromUtc(2026, 11, 13, 11, 30, 0);
  MetarObs o;
  {  // KJFK, US units, RMK ignored
    const MetarResult r = mp("METAR KJFK 131051Z 02018G25KT 3SM -RA BR FEW009 BKN015 OVC020 07/06 A2948 RMK AO2 VIS 2 W SLP981 "
                             "P0003 T00720061 $", o, now);
    CHECK(r.why == MetarWhy::Ok && r.field_bad == 0 && (o.flags & MO_VALID) && strcmp(o.icao, "KJFK") == 0);
    CHECK(o.obs_unix == (uint32_t)unixFromUtc(2026, 11, 13, 10, 51, 0) && o.got_unix == now);
    CHECK(o.wdir == 20 && o.wind_dkmh == 333 && o.gust_dkmh == 463);
    CHECK(o.vis_m == 4828 && !(o.flags & MO_VIS_GE));
    CHECK(o.wx == (MW_PRECIP | MW_RAIN | MW_MIST) && o.wx_int == -1);  // -RA BR = rain and mist
    CHECK(o.cover == 4 && o.ceil_ft == 1500 && !(o.flags & MO_CB));
    CHECK(o.t_dc == 70 && o.td_dc == 60 && o.qnh_dhpa == 9983);
  }
  {  // EGLL: COR AUTO, variable sector, NCD from an automatic station
    const MetarResult r = mp("METAR EGLL 131120Z COR AUTO 32005KT 280V360 9999 NCD 06/M02 Q1018 NOSIG", o, now);
    CHECK(r.why == MetarWhy::Ok && r.field_bad == 0);
    CHECK((o.flags & MO_COR) && (o.flags & MO_AUTO) && (o.flags & MO_LOW_CONF) && (o.flags & MO_VIS_GE));
    CHECK(o.wdir == 320 && o.wind_dkmh == 93 && o.gust_dkmh == METAR_U16_UNSET);
    CHECK(o.vis_m == 10000 && o.cover == 0 && o.ceil_ft == METAR_U16_UNSET && o.wx == 0);
    CHECK(o.t_dc == 60 && o.td_dc == -20 && o.qnh_dhpa == 10180);
  }
  {  // LFPG: FEW only, no ceiling
    const MetarResult r = mp("METAR LFPG 131100Z 03010KT 360V090 9999 FEW041 09/05 Q1016 NOSIG", o, now);
    CHECK(r.why == MetarWhy::Ok && o.cover == 1 && o.ceil_ft == METAR_U16_UNSET && o.wind_dkmh == 185 && o.wdir == 30);
    CHECK(o.t_dc == 90 && o.td_dc == 50 && o.qnh_dhpa == 10160 && !(o.flags & MO_LOW_CONF));
  }
  // synthetic (ZZZZ)
  {
    const MetarResult r = mp("METAR ZZZZ 131100Z VRB02KT CAVOK M05/M08 Q1031", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wdir == METAR_WDIR_VRB && o.wind_dkmh == 37);
    CHECK((o.flags & MO_CAVOK) && (o.flags & MO_VIS_GE) && o.vis_m == 10000 && o.cover == 0);
    CHECK(o.t_dc == -50 && o.td_dc == -80 && o.qnh_dhpa == 10310);
  }
  {
    const MetarResult r = mp("SPECI ZZZZ 131100Z 00000KT 0200 FG VV/// M01/M01 Q1020", o, now);
    CHECK(r.why == MetarWhy::Ok && (o.flags & MO_SPECI) && o.wdir == METAR_WDIR_NONE && o.wind_dkmh == 0);
    CHECK(o.vis_m == 200 && o.wx == MW_FOG && o.cover == 4 && o.ceil_ft == METAR_U16_UNSET);
    CHECK(o.t_dc == -10 && o.td_dc == -10 && o.qnh_dhpa == 10200);
  }
  {
    const MetarResult r = mp("METAR ZZZZ 131100Z 18005MPS 1 1/2SM +TSRA BKN008CB 15/14 A2992", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wdir == 180 && o.wind_dkmh == 180 && o.vis_m == 2414);
    CHECK(o.wx == (MW_THUNDER | MW_PRECIP | MW_RAIN) && o.wx_int == 1 && o.cover == 3 && o.ceil_ft == 800 && (o.flags & MO_CB));
    CHECK(o.t_dc == 150 && o.td_dc == 140 && o.qnh_dhpa == 10132);
  }
  {
    CHECK(mp("METAR ZZZZ 131100Z NIL=", o, now).why == MetarWhy::Nil && !(o.flags & MO_VALID));
    // the tgftp form without a prefix, every group reported missing
    const MetarResult r = mp("ZZZZ 131100Z AUTO /////KT //// ////// ////// Q////", o, now);
    CHECK(r.why == MetarWhy::Ok && r.field_bad == 0 && o.wdir == METAR_WDIR_NONE && o.wind_dkmh == METAR_U16_UNSET);
    CHECK(o.vis_m == METAR_U16_UNSET && o.cover == METAR_COVER_UNSET && o.qnh_dhpa == METAR_I16_UNSET && o.t_dc == METAR_I16_UNSET);
  }
  {  // more units and forms
    CHECK(mp("ZZZZ 131100Z 27015KMH M1/4SM SN 00/M01 Q1001", o, now).why == MetarWhy::Ok);
    CHECK(o.wind_dkmh == 150 && o.vis_m == 402 && o.wx == (MW_PRECIP | MW_SNOW) && o.wx_int == 0);
    CHECK(mp("ZZZZ 131100Z 27015KT P6SM FZDZ VCSH RERA SCT030TCU 01/M00 A3001", o, now).why == MetarWhy::Ok);
    CHECK(o.vis_m == 9656 && (o.flags & MO_VIS_GE) && o.wx == (MW_FREEZING | MW_PRECIP | MW_DRIZZLE) && o.cover == 2);
    CHECK(o.td_dc == 0 && o.qnh_dhpa == 10163);
    CHECK(mp("ZZZZ 131100Z 27015KT 4000 2000SW R27L/P1500 BR 10/09 Q1012", o, now).why == MetarWhy::Ok && o.vis_m == 4000);
    CHECK(mp("ZZZZ 131100Z 27015KT 9999NDV 10/09 Q1012", o, now).why == MetarWhy::Ok && o.vis_m == 10000);
  }
  {  // out-of-bound fields: the field unset, counted; the line kept
    MetarResult r = mp("ZZZZ 131100Z 27015G10KT 9999 FEW020 10/12 Q0500", o, now);
    CHECK(r.why == MetarWhy::Ok && r.field_bad == 3 && o.wind_dkmh == 278 && o.gust_dkmh == METAR_U16_UNSET);
    CHECK(o.t_dc == 100 && o.td_dc == METAR_I16_UNSET && o.qnh_dhpa == METAR_I16_UNSET);
    r = mp("ZZZZ 131100Z 400250KT 9999 99/M99 A1000", o, now);
    CHECK(r.why == MetarWhy::Ok && r.field_bad == 4 && o.wind_dkmh == METAR_U16_UNSET && o.wdir == METAR_WDIR_NONE);
    r = mp("ZZZZ 131100Z 270250KT 9999 10/11 Q1012", o, now);  // 463 km/h; Td one above T is allowed
    CHECK(r.field_bad == 1 && o.wind_dkmh == METAR_U16_UNSET && o.td_dc == 110);
  }
  {  // rejected lines
    CHECK(mp("<html>", o, now).why == MetarWhy::Garbage);
    CHECK(mp("ZZZZ 999999Z 27015KT", o, now).why == MetarWhy::Time);
    CHECK(mp("ZZZZ 131200Z 27015KT", o, now).why == MetarWhy::Time);  // 30 min in the future (and last month: too old)
    CHECK(mp("ZZZZ 131145Z 27015KT", o, now).why == MetarWhy::Time);  // 15 min in the future
    CHECK(mp("ZZZZ 131139Z 27015KT", o, now).why == MetarWhy::Ok);    // 9 min ahead: a clock skew, kept
    CHECK(mp("ZZZZ 121100Z 27015KT", o, now).why == MetarWhy::Time);  // older than 24 h
    CHECK(mp("ZZZZ 121140Z 27015KT", o, now).why == MetarWhy::Ok);
    CHECK(mp("EDDF 131100Z 27015KT", o, now).why == MetarWhy::Unwanted);
    CHECK(mp("METAR", o, now).why == MetarWhy::Garbage);
    CHECK(mp("KJFK", o, now).why == MetarWhy::Garbage);
    CHECK(mp("", o, now).why == MetarWhy::Empty && mp("   \r", o, now).why == MetarWhy::Empty);
    CHECK(strcmp(metarWhyName(MetarWhy::Unwanted), "unwanted") == 0);
  }
  {  // the month: a report from the 31st seen on the 1st belongs to the previous month; day 31 in a 30-day month
    const uint32_t first = (uint32_t)unixFromUtc(2026, 11, 1, 0, 20, 0);
    CHECK(mp("ZZZZ 312350Z 27015KT", o, first).why == MetarWhy::Ok && o.obs_unix == (uint32_t)unixFromUtc(2026, 10, 31, 23, 50, 0));
    const uint32_t jan = (uint32_t)unixFromUtc(2027, 1, 1, 0, 10, 0);
    CHECK(mp("ZZZZ 312355Z 27015KT", o, jan).why == MetarWhy::Ok && o.obs_unix == (uint32_t)unixFromUtc(2026, 12, 31, 23, 55, 0));
    const uint32_t dec1 = (uint32_t)unixFromUtc(2026, 12, 1, 0, 10, 0);
    CHECK(mp("ZZZZ 312355Z 27015KT", o, dec1).why == MetarWhy::Time);  // November has no 31st
    CHECK(mp("ZZZZ 302355Z 27015KT", o, dec1).why == MetarWhy::Ok);
    CHECK(mp("ZZZZ 131100Z 27015KT", o, 0).why == MetarWhy::Time);  // no real time
  }
  // ---- the line splitter: every split point gives the same lines and the same parse
  {
    const std::string body = "METAR EGLL 131120Z COR AUTO 32005KT 280V360 9999 NCD 06/M02 Q1018 NOSIG\r\n\r\n"
                             "METAR KJFK 131051Z 02018G25KT 3SM -RA BR FEW009 BKN015 OVC020 07/06 A2948 RMK AO2\r\n"
                             "METAR LFPG 131100Z 03010KT 360V090 9999 FEW041 09/05 Q1016 NOSIG";  // no trailing newline
    for (size_t step : {(size_t)1, (size_t)3, (size_t)7, body.size()}) {
      LineSeen s;
      MetarLineSink k(LineSeen::fn, &s);
      for (size_t i = 0; i < body.size(); i += step) k.feed(body.data() + i, std::min(step, body.size() - i));
      k.finish();
      CHECK(s.lines.size() == 3 && s.trunc == 0 && k.trunc() == 0);
      if (s.lines.size() == 3) {
        CHECK(s.lines[0].back() == 'G' && s.lines[2].back() == 'G');  // CR dropped
        CHECK(mp(s.lines[1].c_str(), o, now).why == MetarWhy::Ok && o.qnh_dhpa == 9983);
      }
    }
    int same = 0;
    for (size_t cut = 0; cut <= body.size(); cut++) {
      LineSeen s;
      MetarLineSink k(LineSeen::fn, &s);
      k.feed(body.data(), cut);
      k.feed(body.data() + cut, body.size() - cut);
      k.finish();
      int ok = 0;
      for (const std::string& l : s.lines) ok += mp(l.c_str(), o, now).why == MetarWhy::Ok;
      same += s.lines.size() == 3 && ok == 3;
    }
    CHECK(same == (int)body.size() + 1);
  }
  {  // a 400-B line with no newline: the first 191 B kept, counted
    std::string longLine = "METAR ZZZZ 131100Z 27015KT 9999 FEW020 10/09 Q1012 RMK ";
    while (longLine.size() < 400) longLine += "AO2 ";
    LineSeen s;
    MetarLineSink k(LineSeen::fn, &s);
    k.feed(longLine.data(), longLine.size());
    k.finish();
    CHECK(s.lines.size() == 1 && s.trunc == 1 && k.trunc() == 1);
    if (s.lines.size() == 1) {
      CHECK(s.lines[0].size() == MetarLineSink::LINE_CAP - 1);
      CHECK(mp(s.lines[0].c_str(), o, now).why == MetarWhy::Ok && o.qnh_dhpa == 10120);
    }
    k.finish();  // nothing pending: no second line
    CHECK(s.lines.size() == 1);
  }
  // ---- the stream mode of FwdResp feeding the line sink
  const std::string lines = "METAR EGLL 131120Z COR AUTO 32005KT 280V360 9999 NCD 06/M02 Q1018 NOSIG\n"
                            "METAR KJFK 131051Z 02018G25KT 3SM -RA BR FEW009 BKN015 OVC020 07/06 A2948 RMK AO2\n"
                            "METAR LFPG 131100Z 03010KT 360V090 9999 FEW041 09/05 Q1016 NOSIG\n";
  auto run = [&](const std::string& wire, size_t step, size_t cap, FwdResp& r, LineSeen& seen) {
    MetarLineSink k(LineSeen::fn, &seen);
    r.begin(FwdType::Template, "");
    r.stream(MetarLineSink::sinkFn, &k, cap);
    bool done = false;
    for (size_t i = 0; i < wire.size() && !done; i += step) done = r.feed(wire.data() + i, std::min(step, wire.size() - i));
    if (!done) r.end();
    k.finish();
  };
  {  // Content-Length, any split: three lines, three reports, nothing kept in the 256-B body
    const std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(lines.size()) +
                             "\r\n\r\n" + lines;
    for (size_t step = 1; step <= wire.size(); step += (step < 8 ? 1 : 97)) {
      FwdResp r;
      LineSeen seen;
      run(wire, step, 2048, r, seen);
      int ok = 0;
      for (const std::string& l : seen.lines) ok += mp(l.c_str(), o, now).why == MetarWhy::Ok;
      CHECK(r.done() && r.status() == 200 && r.result() == FwdResult::Ok && seen.lines.size() == 3 && ok == 3);
      CHECK(r.streamed() == lines.size() && !r.cut() && r.bodyLen() == 0);
    }
  }
  {  // chunked, the chunks cutting the lines
    std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (size_t i = 0; i < lines.size(); i += 50) {
      const std::string part = lines.substr(i, 50);
      char hex[16];
      snprintf(hex, sizeof hex, "%x\r\n", (unsigned)part.size());
      wire += hex + part + "\r\n";
    }
    wire += "0\r\n\r\n";
    for (size_t step : {(size_t)1, (size_t)3, (size_t)7, wire.size()}) {
      FwdResp r;
      LineSeen seen;
      run(wire, step, 2048, r, seen);
      CHECK(r.done() && r.result() == FwdResult::Ok && seen.lines.size() == 3 && r.streamed() == lines.size());
      if (seen.lines.size() == 3) CHECK(seen.lines[1] + "\n" == lines.substr(lines.find("METAR KJFK"), seen.lines[1].size() + 1));
    }
  }
  {  // no Content-Length, closed by the server; 204 (no data); the cap; a short body; 429
    FwdResp r;
    LineSeen seen;
    run("HTTP/1.1 200 OK\r\n\r\n" + lines, 7, 2048, r, seen);
    CHECK(r.result() == FwdResult::Ok && seen.lines.size() == 3);
    FwdResp z;
    LineSeen none;
    run("HTTP/1.1 204 No Content\r\n\r\n", 5, 2048, z, none);
    CHECK(z.done() && z.status() == 204 && none.lines.empty() && z.streamed() == 0);
    {  // a 204 is done at the end of its headers, with no Content-Length and the connection left open
      FwdResp z2;
      LineSeen n2;
      MetarLineSink k2(LineSeen::fn, &n2);
      z2.begin(FwdType::Template, "");
      z2.stream(MetarLineSink::sinkFn, &k2, 2048);
      const std::string w = "HTTP/1.1 204 No Content\r\nServer: x\r\n\r\n";
      CHECK(z2.feed(w.data(), w.size()) && z2.result() == FwdResult::Ok && n2.lines.empty());
    }
    FwdResp c;
    LineSeen part;
    run("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(lines.size()) + "\r\n\r\n" + lines, 11, 100, c, part);
    CHECK(c.done() && c.cut() && c.streamed() == 100 && part.lines.size() == 2);  // one whole line + the cut rest
    if (part.lines.size() == 2) CHECK(mp(part.lines[0].c_str(), o, now).why == MetarWhy::Ok);
    {  // the glue drops the part line after a cut (never a half report)
      FwdResp c2;
      LineSeen p2;
      MetarLineSink k2(LineSeen::fn, &p2);
      c2.begin(FwdType::Template, "");
      c2.stream(MetarLineSink::sinkFn, &k2, 100);
      const std::string w = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(lines.size()) + "\r\n\r\n" + lines;
      c2.feed(w.data(), w.size());
      if (c2.cut()) k2.drop();
      else k2.finish();
      CHECK(c2.cut() && p2.lines.size() == 1 && k2.trunc() == 1);
      k2.drop();  // nothing pending: not counted again
      CHECK(k2.trunc() == 1);
    }
    FwdResp s;
    LineSeen shortSeen;
    run("HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n" + lines.substr(0, 80), 9, 2048, s, shortSeen);
    CHECK(s.result() == FwdResult::Fail);  // closed before Content-Length: a short read, retried
    FwdResp e;
    LineSeen errSeen;
    run("HTTP/1.1 429 Too Many\r\nRetry-After: 900\r\nContent-Length: 2\r\n\r\nno", 4, 2048, e, errSeen);
    CHECK(e.result() == FwdResult::Limited && e.retryAfter() == 900);
    FwdResp b4;  // AWC's 400 for an unknown parameter, a short text body
    LineSeen b4s;
    run("HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: 35\r\n\r\nBad request: unknown parameter xyz", 6,
        2048, b4, b4s);
    CHECK(b4.done() && b4.status() == 400 && b4.result() != FwdResult::Ok);
  }
  // ---- body ends, chunked caps, fractions
  {  // a body that dies inside a line -- the part line is dropped, never parsed (cover would read "few", no ceiling)
    CHECK(metarCleanEnd(true, true, false, false) && !metarCleanEnd(false, true, false, false) &&
          !metarCleanEnd(true, false, false, false) && !metarCleanEnd(true, true, true, false) && !metarCleanEnd(true, true, false, true));
    const std::string part = "METAR ZZZZ 131151Z 02018KT 3SM BR FEW009 BKN0";
    FwdResp r;
    LineSeen seen;
    MetarLineSink k(LineSeen::fn, &seen);
    r.begin(FwdType::Template, "");
    r.stream(MetarLineSink::sinkFn, &k, 2048);
    const std::string w = "HTTP/1.1 200 OK\r\nContent-Length: 120\r\n\r\n" + part;  // the connection dies here
    r.feed(w.data(), w.size());
    r.end();
    CHECK(r.done() && r.result() == FwdResult::Fail);
    if (metarCleanEnd(true, r.done(), r.cut(), r.result() == FwdResult::Fail)) k.finish();
    else k.drop();
    CHECK(seen.lines.empty() && k.trunc() == 1);
  }
  {  // a chunked body exactly at the cap is whole (its last line kept); one byte more is cut
    auto chunked = [](const std::string& body) {
      char hex[16];
      snprintf(hex, sizeof hex, "%x\r\n", (unsigned)body.size());
      return std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") + hex + body + "\r\n0\r\n\r\n";
    };
    const std::string body = "METAR ZZZZ 131100Z 27015KT 9999 FEW020 10/09 Q1012\nMETAR LFPG 131100Z 03010KT 9999 FEW041 09/05 Q1016";
    for (int extra : {0, 1}) {
      FwdResp r;
      LineSeen seen;
      MetarLineSink k(LineSeen::fn, &seen);
      r.begin(FwdType::Template, "");
      r.stream(MetarLineSink::sinkFn, &k, body.size());
      const std::string w = chunked(body + std::string(extra, 'X'));
      bool done = r.feed(w.data(), w.size());
      if (!done) r.end();
      if (metarCleanEnd(true, r.done(), r.cut(), r.result() == FwdResult::Fail)) k.finish();
      else k.drop();
      CHECK(r.done() && r.cut() == (extra == 1) && seen.lines.size() == (extra ? 1u : 2u));
    }
  }
  {  // a wrap case reads as what it says (26 95/99 SM >= 10 km, 64-bit), an improper fraction is counted
    MetarResult r = mp("ZZZZ 131100Z 27015KT 26 95/99SM FEW020 10/09 Q1012", o, now);
    CHECK(r.why == MetarWhy::Ok && o.vis_m == 10000 && (o.flags & MO_VIS_GE) && r.field_bad == 0 && o.qnh_dhpa == 10120);
    r = mp("ZZZZ 131100Z 27015KT 26 99/95SM FEW020 10/09 Q1012", o, now);
    CHECK(r.why == MetarWhy::Ok && o.vis_m == METAR_U16_UNSET && r.field_bad == 1 && o.qnh_dhpa == 10120);
    r = mp("ZZZZ 131100Z 27015KT 2 3/4SM FEW020 10/09 Q1012", o, now);  // a proper one: 2.75 SM = 4 426 m
    CHECK(r.why == MetarWhy::Ok && o.vis_m == 4426 && r.field_bad == 0);
  }
  // ---- real-world variants, synthetic ZZZZ with real shapes
  {
    MetarResult r = mp("METAR ZZZZ 131100Z CCA 27015KT 9999 FEW020 10/09 Q1012", o, now);  // Canadian correction
    CHECK(r.why == MetarWhy::Ok && (o.flags & MO_COR));
    r = mp("METAR COR ZZZZ 131100Z 27015KT 9999 FEW020 10/09 Q1012", o, now);  // COR before the ICAO
    CHECK(r.why == MetarWhy::Ok && (o.flags & MO_COR) && o.qnh_dhpa == 10120);
    CHECK(mp("METAR ZZZZ 131100Z RTD 27015KT 9999 10/09 Q1012", o, now).why == MetarWhy::Ok && (o.flags & MO_COR));
    CHECK(mp("METAR ZZZZ 131100Z CC1 27015KT 9999 10/09 Q1012", o, now).why == MetarWhy::Ok && !(o.flags & MO_COR));
    r = mp("ZZZZ 131100Z AUTO 27015KT 10SM CLR 10/09 A2992", o, now);
    CHECK(r.why == MetarWhy::Ok && o.cover == 0 && (o.flags & MO_LOW_CONF) && o.vis_m == 10000 && (o.flags & MO_VIS_GE) &&
          o.qnh_dhpa == 10132);
    r = mp("METAR ZZZZ 131100Z 27015KT 10SM CLR 10/09 A2992 RMK AO2", o, now);  // CLR alone is automatic
    CHECK(r.why == MetarWhy::Ok && (o.flags & MO_LOW_CONF) && !(o.flags & MO_AUTO));
    CHECK(mp("METAR ZZZZ 131100Z 27015KT 9999 SKC 10/09 Q1012", o, now).why == MetarWhy::Ok && o.cover == 0 &&
          !(o.flags & MO_LOW_CONF));  // SKC is an observer's "sky clear"
    r = mp("ZZZZ 131100Z E18010KT 10SM FEW030 10/09 A3022", o, now);  // a national wind form, counted
    CHECK(r.why == MetarWhy::Ok && o.wind_dkmh == METAR_U16_UNSET && o.wdir == METAR_WDIR_NONE && o.qnh_dhpa == 10234 &&
          r.field_bad == 1);
    r = mp("ZZZZ 131100Z 3006KT 9999 SCT030 29/26 Q1007", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wind_dkmh == METAR_U16_UNSET && r.field_bad == 1 && o.qnh_dhpa == 10070);
    r = mp("ZZZZ 131100Z 33007G12KTKT 7SM FEW060 20/06 A2982", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wind_dkmh == METAR_U16_UNSET && r.field_bad == 1 && o.vis_m == 10000 && (o.flags & MO_VIS_GE));
    r = mp("ZZZZ 131100Z 08021KT 14KM FEW029/// 28/21 Q1017", o, now);  // whole km
    CHECK(r.why == MetarWhy::Ok && o.wind_dkmh == 389 && o.vis_m == 10000 && (o.flags & MO_VIS_GE) && o.cover == 1);
    CHECK(mp("ZZZZ 131100Z 08021KT 8KM 28/21 Q1017", o, now).why == MetarWhy::Ok && o.vis_m == 8000 && !(o.flags & MO_VIS_GE));
    r = mp("ZZZZ 131100Z 34012G21KT 100SM FEW020 M07/M13", o, now);
    CHECK(r.why == MetarWhy::Ok && o.vis_m == 10000 && (o.flags & MO_VIS_GE) && r.field_bad == 0 && o.t_dc == -70);
    r = mp("ZZZZ 131100Z AUTO 17010KT 10SM M 17/M01 A2955", o, now);  // a lone M
    CHECK(r.why == MetarWhy::Ok && o.t_dc == 170 && o.td_dc == -10);
    r = mp("ZZZZ 131100Z AUTO 20010KT 10SM MM 16/03 A2988", o, now);
    CHECK(r.why == MetarWhy::Ok && o.t_dc == 160 && o.td_dc == 30);
    r = mp("ZZZZ 131100Z AUTO 29013KT //// // ///////// 09/04 Q1016 RE// W13/H24", o, now);
    CHECK(r.why == MetarWhy::Ok && o.vis_m == METAR_U16_UNSET && o.cover == METAR_COVER_UNSET && o.wx == 0 &&
          o.qnh_dhpa == 10160 && o.t_dc == 90 && o.td_dc == 40 && r.field_bad == 0);
    r = mp("ZZZZ 131100Z 27015KT 5SM +VCTSRA OVC060 11/09 A3000", o, now);  // in the vicinity, not here
    CHECK(r.why == MetarWhy::Ok && o.wx == 0 && o.cover == 4 && o.ceil_ft == 6000);
    r = mp("ZZZZ 131100Z 21010KT CAVOK 21/14 Q1014 R24/510260 WS R25 R/SNOCLO", o, now);
    CHECK(r.why == MetarWhy::Ok && (o.flags & MO_CAVOK) && o.qnh_dhpa == 10140 && o.t_dc == 210 && r.field_bad == 0);
    r = mp("ZZZZ 131100Z 06004KT OAVOK 06/05 Q1025 R34/CLRD70 NOSIG", o, now);  // a typo stays unread
    CHECK(r.why == MetarWhy::Ok && o.vis_m == METAR_U16_UNSET && o.qnh_dhpa == 10250);
    r = mp("ZZZZ 131100Z 00000Z 7000 NSC 25/14 Q1016", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wind_dkmh == METAR_U16_UNSET && o.vis_m == 7000 && o.cover == 0 && !(o.flags & MO_LOW_CONF));
    r = mp("ZZZZ 131100Z 27015KT 6000 BR DZ 10/09 Q1012", o, now);  // mist and drizzle
    CHECK(r.why == MetarWhy::Ok && o.wx == (MW_MIST | MW_DRIZZLE | MW_PRECIP));
    r = mp("ZZZZ 131100Z 27015KT 0400 FG 10/09 Q1012", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wx == MW_FOG);
    r = mp("ZZZZ 131100Z 27015KT 2000 -SHRASN 01/00 Q1012", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wx == (MW_PRECIP | MW_RAIN | MW_SNOW) && o.wx_int == -1);
    CHECK(mp("ZZZZ 131100Z 17005KT CAVOK 18/12 A3008 Q1018", o, now).why == MetarWhy::Ok && o.qnh_dhpa == 10186);
    CHECK(mp("ZZZZ 131100Z 17005KT CAVOK 18/12 Q1018 A3008", o, now).why == MetarWhy::Ok && o.qnh_dhpa == 10180);
    r = mp("SPECI ZZZZ 131100Z AUTO VRB04KT 10SM BKN018 OVC036 07/ A2969", o, now);
    CHECK(r.why == MetarWhy::Ok && o.t_dc == 70 && o.td_dc == METAR_I16_UNSET && o.cover == 4 && o.ceil_ft == 1800);
    CHECK(mp("ZZZZ 131100Z AUTO 09007KT //// // ////// 10/08 Q1012", o, now).why == MetarWhy::Ok && o.vis_m == METAR_U16_UNSET);
    CHECK(mp("ZZZZ 131100Z AUTO 09007KT 15SM 10/08 Q1012", o, now).why == MetarWhy::Ok && o.vis_m == 10000 && (o.flags & MO_VIS_GE));
    CHECK(mp("ZZZZ 131100Z 27015KT 9999 M00/M01 Q1012", o, now).why == MetarWhy::Ok && o.t_dc == 0 && o.td_dc == -10);
    r = mp("ZZZZ 131100Z VRB05G15KT 9999 10/09 Q1012", o, now);
    CHECK(r.why == MetarWhy::Ok && o.wdir == METAR_WDIR_VRB && o.wind_dkmh == 93 && o.gust_dkmh == 278);
    CHECK(mp("METAR ZZZZ NIL", o, now).why == MetarWhy::Garbage);  // NIL without a time stays garbage
    r = mp("ZZZZ 131100Z 27015KT 9999 BECMG 27020KT 10/09 Q1012", o, now);  // nothing after a trend is read
    CHECK(r.why == MetarWhy::Ok && o.qnh_dhpa == METAR_I16_UNSET && o.t_dc == METAR_I16_UNSET && o.wind_dkmh == 278);
    r = mp("ZZZZ 131100Z 26004MPS 9999 SCT010C BKN045 ///003/// 23/21 A2998", o, now);
    CHECK(r.why == MetarWhy::Ok && o.cover == 3 && o.ceil_ft == 4500 && o.wind_dkmh == 144);
    r = mp("METAR ZZZZ 131100Z AUTO 34005KT 11/10 RMK AO1 SLP897 T01130098", o, now);  // no pressure in the body
    CHECK(r.why == MetarWhy::Ok && o.qnh_dhpa == METAR_I16_UNSET && o.t_dc == 110 && o.vis_m == METAR_U16_UNSET &&
          o.cover == METAR_COVER_UNSET);
    // a known limit (the hours=2 fetch cures it): a month-old line whose day and time pass reads as today's
    CHECK(mp("ZZZZ 131100Z 27015KT 9999 10/09 Q1012", o, now).why == MetarWhy::Ok &&
          o.obs_unix == (uint32_t)unixFromUtc(2026, 11, 13, 11, 0, 0));
  }
  {  // an hours=2 answer, newest first by time across the ids, a SPECI and a correction; the newest per airport kept
    const char* const want3[] = {"KJFK", "EGLL", "LFPG"};
    const char* const wire[] = {
        "METAR EGLL 131120Z AUTO 32005KT 9999 NCD 06/M02 Q1018 NOSIG",
        "SPECI KJFK 131108Z 02016KT 3SM -RA BR BKN014 07/06 A2949",
        "METAR LFPG 131100Z 03010KT 9999 FEW041 09/05 Q1016 NOSIG",
        "METAR LFPG 131100Z COR 03010KT 9999 FEW041 09/05 Q1017 NOSIG",
        "METAR EGLL 131050Z AUTO 32006KT 9999 NCD 06/M02 Q1017 NOSIG",
        "METAR KJFK 131051Z 02018G25KT 3SM -RA BR FEW009 BKN015 OVC020 07/06 A2948",
        "METAR LFPG 131030Z 03009KT 9999 FEW040 09/05 Q1016 NOSIG",
        "METAR EGLL 131020Z AUTO 32006KT 9999 NCD 06/M02 Q1017 NOSIG",
        "METAR KJFK 130951Z 02017KT 3SM BR OVC020 07/06 A2947",
    };
    MetarObs got[METAR_AIRPORTS_MAX];
    uint8_t ngot = 0;
    for (const char* l : wire) {
      MetarObs x;
      if (metarParse(l, strlen(l), want3, 3, now, x).why == MetarWhy::Ok) CHECK(metarKeep(got, ngot, METAR_AIRPORTS_MAX, x));
    }
    CHECK(ngot == 3);
    for (int i = 0; i < ngot; i++) {
      if (!strcmp(got[i].icao, "EGLL")) CHECK(got[i].obs_unix == (uint32_t)unixFromUtc(2026, 11, 13, 11, 20, 0) && got[i].qnh_dhpa == 10180);
      if (!strcmp(got[i].icao, "KJFK")) CHECK(got[i].obs_unix == (uint32_t)unixFromUtc(2026, 11, 13, 11, 8, 0) && (got[i].flags & MO_SPECI));
      if (!strcmp(got[i].icao, "LFPG")) CHECK(got[i].qnh_dhpa == 10170 && (got[i].flags & MO_COR));  // the correction replaced it
    }
    MetarObs extra;
    CHECK(mp("METAR ZZZZ 131100Z 27015KT 9999 10/09 Q1012", extra, now).why == MetarWhy::Ok && !metarKeep(got, ngot, 3, extra) && ngot == 3);
  }
}
