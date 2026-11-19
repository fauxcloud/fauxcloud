// test_airport.inc -- integer distance, centi-degree parsing, the lookup
// box, one stationinfo object, the streaming JSON sink with nearest-5. Public airports.
#include "airport.h"

namespace {
double havKm(double lat1, double lon1, double lat2, double lon2) {
  const double r = 3.14159265358979323846 / 180.0;
  const double a = std::sin((lat2 - lat1) * r / 2), b = std::sin((lon2 - lon1) * r / 2);
  const double h = a * a + std::cos(lat1 * r) * std::cos(lat2 * r) * b * b;
  return 2 * 6371.0 * std::asin(std::sqrt(h));
}
std::string apt(const char* icao, const char* site, const char* lat, const char* lon, const char* types) {
  return std::string("{\"id\":\"x\",\"icaoId\":\"") + icao + "\",\"iataId\":null,\"site\":\"" + site + "\",\"lat\":" + lat +
         ",\"lon\":" + lon + ",\"elev\":4,\"state\":\"NY\",\"country\":\"US\",\"priority\":1,\"siteType\":[" + types + "]}";
}
}  // namespace

static void testAirport() {
  {  // the display name (AWC site forms), idempotent
    struct { const char* in; const char* out; } V[] = {
        {"London/Heathrow Intl", "Heathrow"}, {"London/Gatwick Arpt", "Gatwick"}, {"Sample Field", "Sample Field"},
        {"London Heathrow Arpt", "London Heathrow"}, {"New York/JF Kennedy Intl", "JF Kennedy"}, {"Intl Arpt", "Intl Arpt"},
        {"City/Intl", "City/Intl"}, {"Gander Intl Airport", "Gander"}, {"A/B/C International", "C"}, {"", ""},
        {"  Spaced   Name  Intl ", "Spaced Name"}, {"CafÃ© Arpt", "CafÃ©"}};
    // an older blob stored the site text cut at 24 B; a cut noise word at the end goes too (a whole 24-B name keeps its words)
    {
      char s[64] = "London/Heathrow Internat";
      airportShortName(s);
      CHECK(strcmp(s, "Heathrow") == 0);
      snprintf(s, sizeof s, "%s", "New York/JF Kennedy Intl");  // 24 B and whole
      airportShortName(s);
      CHECK(strcmp(s, "JF Kennedy") == 0);
      snprintf(s, sizeof s, "%s", "Gatwick Field Airp Lon In");  // 25 B: not a stored cut name, nothing but whole noise words go
      airportShortName(s);
      CHECK(strcmp(s, "Gatwick Field Airp Lon In") == 0);
      snprintf(s, sizeof s, "%s", "London/Gatwick Field Arp");  // 24 B: the cut "Arpt"
      airportShortName(s);
      CHECK(strcmp(s, "Gatwick Field") == 0);
    }
    for (auto& v : V) {
      char s[64];
      snprintf(s, sizeof s, "%s", v.in);
      airportShortName(s);
      CHECK(strcmp(s, v.out) == 0);
      airportShortName(s);
      CHECK(strcmp(s, v.out) == 0);  // idempotent
    }
  }
  {  // the country from the ICAO prefix (three letters before two before one) and its flag pack
    uint8_t p = 0;
    char b[3];
    auto airportCountry = [&b](const char* icao, uint8_t* pk) -> const char* { fc::airportCountry(icao, b, pk); return b; };
    CHECK(!strcmp(airportCountry("EGLL", &p), "GB") && p == 0);
    CHECK(!strcmp(airportCountry("KJFK", &p), "US") && p == 3);
    CHECK(!strcmp(airportCountry("UMKK", &p), "RU") && p == 0);  // UMK (Kaliningrad) before UM (Belarus)
    CHECK(!strcmp(airportCountry("UMMS", &p), "BY") && p == 0);
    CHECK(!strcmp(airportCountry("YSSY", &p), "AU") && p == 4);
    // one airport per prefix family
    const char* V[][2] = {{"EHAM", "NL"}, {"LFPG", "FR"}, {"ZBAA", "CN"}, {"ZKPY", "KP"}, {"LYPG", "ME"}, {"LYBE", "RS"},
                          {"WBSB", "BN"}, {"EKVG", "FO"}};
    for (auto& v : V) CHECK(!strcmp(airportCountry(v[0], nullptr), v[1]));
    CHECK(!strcmp(airportCountry("QQQQ", &p), "") && p == AIRPORT_PACK_NONE);
    CHECK(!strcmp(airportCountry("", nullptr), ""));
  }
  {  // distance vs haversine: 1 000 pairs under 300 km, <= 1 % or 1 km
    uint32_t seed = 12345;
    auto rnd = [&](int32_t lo, int32_t hi) {
      seed = seed * 1103515245u + 12345u;
      return lo + (int32_t)((seed >> 8) % (uint32_t)(hi - lo + 1));
    };
    int checked = 0, bad = 0;
    double worst = 0;
    while (checked < 1000) {
      const int32_t la = rnd(-8500, 8500), lo = rnd(-18000, 18000);
      const int32_t lb = la + rnd(-270, 270), lob = lo + rnd(-800, 800);
      if (lb < -9000 || lb > 9000) continue;
      const int32_t lobw = lob > 18000 ? lob - 36000 : lob < -18000 ? lob + 36000 : lob;
      const double h = havKm(la / 100.0, lo / 100.0, lb / 100.0, lobw / 100.0);
      if (h >= 300) continue;
      const double e = std::fabs((double)geoKm(la, lo, lb, lobw) - h);
      const double allowed = std::max(1.0, h * 0.01);
      if (e > allowed) bad++;
      if (e / allowed > worst) worst = e / allowed;
      checked++;
    }
    CHECK(bad == 0);
    printf("  info: geoKm vs haversine, 1000 pairs < 300 km: worst %.2f of the allowance\n", worst);
    CHECK(geoKm(0, 17990, 0, -17990) == 22 && geoKm(0, -17990, 0, 17990) == 22);  // across the antimeridian
    CHECK(geoKm(4078, -7397, 4078, -7397) == 0);
  }
  {  // centi-degrees from JSON text, no float
    int32_t v = 0;
    CHECK(centiFromText("40.6398", &v) && v == 4064);
    CHECK(centiFromText("-73.7789", &v) && v == -7378);
    CHECK(centiFromText("51.4706", &v) && v == 5147);
    CHECK(centiFromText("-0.4619", &v) && v == -46);
    CHECK(centiFromText("40.995", &v) && v == 4100 && centiFromText("-40.995", &v) && v == -4100);
    CHECK(centiFromText("12", &v) && v == 1200 && centiFromText("0.004", &v) && v == 0);
    CHECK(!centiFromText("1e3", &v) && !centiFromText("", &v) && !centiFromText("-", &v) && !centiFromText("4.", &v) &&
          !centiFromText("abc", &v) && !centiFromText("1.2.3", &v));
  }
  {  // the lookup box: +-1.1 deg lat, +-1.1/cos(lat) lon capped at +-3 deg, clipped to the globe; centred on
     // the 0.1-deg grid point nearest the location and 0.1 deg wider each way
    int32_t b[4];
    airportBox(0, 0, b);
    CHECK(b[0] == -120 && b[1] == -120 && b[2] == 120 && b[3] == 120);
    airportBox(6000, 1000, b);
    CHECK(b[1] == 1000 - 230 && b[3] == 1000 + 230);
    airportBox(8900, 0, b);
    CHECK(b[1] == -310 && b[3] == 310 && b[2] == 9000);
    airportBox(-4000, -17950, b);
    CHECK(b[1] == -18000 && b[3] > -17950);
    // a location 0.04 deg from a grid point and one 0.05 deg away: the box is the grid point's, never the location's
    airportBox(4064, -7378, b);  // KJFK's position: centre 40.60, -73.80
    CHECK(b[0] == 4060 - 120 && b[2] == 4060 + 120 && (b[1] + b[3]) / 2 == -7380);
    int32_t b2[4];
    airportBox(4056, -7384, b2);  // another location in the same cell: the very same request
    CHECK(memcmp(b, b2, sizeof b) == 0);
    airportBox(-3395, 15117, b);  // centre -34.00 (half away from zero), 151.20
    CHECK(b[0] == -3400 - 120 && (b[1] + b[3]) / 2 == 15120);
  }
  {  // one object
    Airport a;
    std::string o = apt("KJFK", "New York/JF Kennedy Intl", "40.6398", "-73.7789", "\"METAR\",\"TAF\"");
    CHECK(airportFromJson(o.data(), o.size(), &a) && strcmp(a.icao, "KJFK") == 0 && a.lat == 4064 && a.lon == -7378);
    CHECK(strcmp(a.name, "JF Kennedy") == 0 && a.km == AIRPORT_KM_UNKNOWN);  // the short name
    o = apt("EGLL", "London/Heathrow International Airport", "51.4706", "-0.4619", "\"METAR\"");
    CHECK(airportFromJson(o.data(), o.size(), &a) && strcmp(a.name, "Heathrow") == 0 && a.lon == -46);
    // shortened before the 24-B cut: the useful part of a long site text survives
    o = apt("KMSP", "Minneapolis-St Paul/Wold-Chamberlain Intl", "44.88", "-93.22", "\"METAR\"");
    CHECK(airportFromJson(o.data(), o.size(), &a) && strcmp(a.name, "Wold-Chamberlain") == 0);
    o = apt("KJFK", "Caf\\u00e9 \\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9", "1", "2", "\"METAR\"");
    CHECK(airportFromJson(o.data(), o.size(), &a) && strlen(a.name) <= 24 && (strlen(a.name) % 2) == 0);  // no split é
    o = apt("KJFK", "x", "40.6", "-73.7", "\"TAF\"");
    CHECK(!airportFromJson(o.data(), o.size(), &a));  // not a METAR site
    o = apt("JFK", "x", "40.6", "-73.7", "\"METAR\"");
    CHECK(!airportFromJson(o.data(), o.size(), &a));  // not a 4-character ICAO
    o = apt("KJFK", "x", "null", "-73.7", "\"METAR\"");
    CHECK(!airportFromJson(o.data(), o.size(), &a));  // no position
    o = apt("KJFK", "x", "95.0", "-73.7", "\"METAR\"");
    CHECK(!airportFromJson(o.data(), o.size(), &a));
    CHECK(!airportFromJson("{\"icaoId\":", 11, &a) && !airportFromJson("[]", 2, &a));
  }
  {  // the stream: nearest 5 from a city centre, any split; strings with braces, a dropped long object, a rejected one
    std::string body = "[" + apt("KISP", "Islip/Long Island Mac Arthur", "40.7952", "-73.1002", "\"METAR\"") + "," +
                       apt("KJFK", "New York/JF Kennedy Intl", "40.6398", "-73.7789", "\"METAR\",\"TAF\"") + "," +
                       apt("KLGA", "New York/La Guardia", "40.7772", "-73.8726", "\"METAR\",\"TAF\"") + "," +
                       apt("KEWR", "Newark Liberty Intl", "40.6925", "-74.1687", "\"METAR\",\"TAF\"") + "," +
                       apt("KZZZ", "A \\\"{quoted}\\\" ]name[", "40.80", "-73.95", "\"TAF\"") + "," +
                       apt("KTEB", "Teterboro", "40.8501", "-74.0608", "\"METAR\"") + "," +
                       apt("KHPN", "White Plains", "41.0670", "-73.7076", "\"METAR\",\"TAF\"") + "," +
                       apt("KFRG", "Farmingdale", "40.7288", "-73.4134", "\"METAR\"") + "," +
                       "{\"icaoId\":\"KLNG\",\"site\":\"" + std::string(400, 'L') + "\",\"lat\":40.78,\"lon\":-73.97," +
                       "\"siteType\":[\"METAR\"]}]";
    const int32_t refLat = 4078, refLon = -7397;  // a city centre (Manhattan), public
    for (size_t step : {(size_t)1, (size_t)3, (size_t)7, (size_t)64, body.size()}) {
      AirportJsonSink k(true, refLat, refLon);
      for (size_t i = 0; i < body.size(); i += step) k.feed(body.data() + i, std::min(step, body.size() - i));
      CHECK(k.seen() == 9 && k.dropped() == 1 && k.rejected() == 1 && k.count() == 5);
      if (k.count() != 5) continue;
      const char* order[5] = {"KLGA", "KTEB", "KEWR", "KJFK", "KHPN"};  // 8, 10, 19, 21, 33 km; KFRG 47, KISP 74 out
      for (int i = 0; i < 5; i++) {
        CHECK(strcmp(k.at(i).icao, order[i]) == 0);
        if (i) CHECK(k.at(i - 1).km <= k.at(i).km);
        const double h = havKm(refLat / 100.0, refLon / 100.0, k.at(i).lat / 100.0, k.at(i).lon / 100.0);
        CHECK(std::fabs(k.at(i).km - h) <= 1.0);
      }
    }
    AirportJsonSink none(false, 0, 0);  // "Add by code" without a location: in order, no km
    none.feed(body.data(), body.size());
    CHECK(none.count() == 5 && strcmp(none.at(0).icao, "KISP") == 0 && none.at(0).km == AIRPORT_KM_UNKNOWN);
    AirportJsonSink err(true, 0, 0);  // an error object, not an array: nothing read
    const std::string e = "{\"error\":\"bad bbox\",\"data\":[{\"icaoId\":\"KJFK\"}]}";
    err.feed(e.data(), e.size());
    CHECK(err.seen() == 0 && err.count() == 0);
    AirportJsonSink empty(true, 0, 0);
    empty.feed("[]", 2);
    CHECK(empty.seen() == 0 && empty.count() == 0);
  }
}
