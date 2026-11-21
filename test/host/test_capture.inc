// test_capture.inc -- src/capture.cpp and tlsHello (src/tls_sni.cpp): "Listen to a new console". The record ring and
// what counts as the same thing again, the DNS table, the devices on the Wi-Fi, the redaction, what a request or a
// peek becomes, the start / stop order, the GET body. Included by test_fauxcloud.cpp after test_fwd.inc (TlsServer).

namespace {

CapUnit unit1(CapKind k, const char* t) {
  CapUnit u = {};
  u.add(k, t);
  return u;
}
CapUnit unit2(CapKind k1, const char* t1, CapKind k2, const char* t2) {
  CapUnit u = {};
  u.add(k1, t1);
  u.add(k2, t2);
  return u;
}
std::string redQ(const char* s) {
  char out[400];
  capRedactQuery(s, strlen(s), out, sizeof out);
  return out;
}
std::string redJ(const char* s) {
  char out[400];
  capRedactJson(s, strlen(s), out, sizeof out);
  return out;
}
std::string redP(const char* s) {
  char out[400];
  capRedactPath(s, strlen(s), out, sizeof out);
  return out;
}
// a ClientHello with the given suites, versions (empty = no supported_versions), groups (empty = none), SNI and
// optional padding, record version 0x0301 as browsers send it
std::vector<uint8_t> helloWith(std::vector<uint16_t> suites, std::vector<uint16_t> versions, std::vector<uint16_t> groups,
                               const char* sni, size_t pad = 0, uint16_t legacy = 0x0303) {
  std::vector<uint8_t> ext;
  auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); };
  if (sni) {
    const uint16_t n = (uint16_t)strlen(sni);
    u16(ext, 0); u16(ext, n + 5u); u16(ext, n + 3u); ext.push_back(0); u16(ext, n);
    ext.insert(ext.end(), sni, sni + n);
  }
  if (!groups.empty()) {
    u16(ext, 10); u16(ext, 2u + 2u * (uint32_t)groups.size()); u16(ext, 2u * (uint32_t)groups.size());
    for (uint16_t g : groups) u16(ext, g);
  }
  u16(ext, 13); u16(ext, 4); u16(ext, 2); u16(ext, 0x0403);  // signature_algorithms: ecdsa_secp256r1_sha256
  if (!versions.empty()) {
    u16(ext, 43); u16(ext, 1u + 2u * (uint32_t)versions.size()); ext.push_back((uint8_t)(2 * versions.size()));
    for (uint16_t v : versions) u16(ext, v);
  }
  if (pad) {  // the padding extension (21), as browsers fill a hello to a round size
    u16(ext, 21); u16(ext, (uint32_t)pad);
    ext.insert(ext.end(), pad, 0);
  }
  std::vector<uint8_t> hs;
  u16(hs, legacy);
  hs.insert(hs.end(), 32, 0xab);
  hs.push_back(0);  // no session id
  u16(hs, 2u * (uint32_t)suites.size());
  for (uint16_t s : suites) u16(hs, s);
  hs.push_back(1); hs.push_back(0);  // one compression method
  u16(hs, (uint32_t)ext.size());
  hs.insert(hs.end(), ext.begin(), ext.end());
  std::vector<uint8_t> rec = {0x16, 0x03, 0x01};
  u16(rec, (uint32_t)hs.size() + 4u);
  rec.push_back(0x01); rec.push_back(0); u16(rec, (uint32_t)hs.size());
  rec.insert(rec.end(), hs.begin(), hs.end());
  return rec;
}
std::string helloText(const std::vector<uint8_t>& h, size_t n = 0) {
  TlsHello x;
  if (!tlsHello(h.data(), n ? n : h.size(), &x)) return "";
  char t[CAPTURE_TEXT];
  tlsHelloText(x, t, sizeof t);
  return t;
}
std::string peek(uint16_t port, const std::vector<uint8_t>& d, CapUnit* u, bool cut = false) {
  CapPeekIn in = {port, d.data(), d.size(), cut, false};
  capPeekUnit(in, u);
  std::string all;
  for (int i = 0; i < u->n; i++) all += std::string(capKindName(u->it[i].kind)) + "|" + u->it[i].text + "\n";
  return all;
}
bool hasMac6(const std::string& s) {  // six colon-separated hex pairs anywhere (a full MAC)
  for (size_t i = 0; i + 17 <= s.size(); i++) {
    bool ok = true;
    for (int k = 0; k < 17 && ok; k++) ok = (k % 3 == 2) ? s[i + k] == ':' : isxdigit((unsigned char)s[i + k]) != 0;
    if (ok) return true;
  }
  return false;
}
std::string acts(const CapActs& a) {
  std::string s;
  for (int i = 0; i < a.n; i++) s += std::string(i ? "," : "") + capActName(a.a[i]);
  return s;
}

}  // namespace

static void testCaptureRing() {
  static CaptureRing r;
  r.reset();
  // the same request three times between two polls: one record, counted 3, one id, a new seq each time
  const CapUnit a = unit1(CapKind::Http, "GET /x?ID=***");
  for (int i = 0; i < 3; i++) r.put(a, 23, 1000u * (uint32_t)i);
  CHECK(r.count() == 1 && r.seq() == 3 && r.written() == 1);
  CaptureRec out[CAPTURE_GET_MAX];
  bool more = true;
  CHECK(r.snapshot(0, out, CAPTURE_GET_MAX, &more) == 1 && !more && out[0].id == 1 && out[0].seq == 3 && out[0].n == 3 &&
        out[0].t_first == 0 && out[0].t_last == 2000 && out[0].client == 23);
  r.served(3);  // the page has seq 3; the same request again comes back with its new count
  r.put(a, 23, 5000);
  CHECK(r.snapshot(3, out, CAPTURE_GET_MAX, &more) == 1 && out[0].id == 1 && out[0].n == 4 && out[0].seq == 4);
  // dedupe against every resident record, not only the newest
  r.put(unit1(CapKind::Tcp, ":443"), 23, 6000);
  r.put(a, 23, 7000);
  CHECK(r.count() == 2);
  // another client sending the same is another record
  r.put(a, 24, 7000);
  CHECK(r.count() == 3);
  // a unit: the same pair again counts both; another header makes a new pair; the line alone is not that unit
  r.reset();
  const CapUnit req = unit2(CapKind::Http, "GET /u", CapKind::Hdr, "host=a");
  r.put(req, 23, 0);
  r.put(unit1(CapKind::Tcp, ":443"), 23, 1);
  r.put(req, 23, 2);
  CHECK(r.count() == 3 && r.snapshot(0, out, CAPTURE_GET_MAX, &more) == 3);
  for (int i = 0; i < 3; i++)
    if (out[i].kind != (uint8_t)CapKind::Tcp) CHECK(out[i].n == 2 && out[i].group == 1);
  r.put(unit2(CapKind::Http, "GET /u", CapKind::Hdr, "host=b"), 23, 3);
  CHECK(r.count() == 5);
  r.put(unit1(CapKind::Http, "GET /u"), 23, 4);  // the resident units are longer
  CHECK(r.count() == 6);
  // 17 distinct records into 16 slots: the oldest goes; lost only when no GET returned it
  r.reset();
  char t[16];
  for (int i = 0; i < 17; i++) {
    snprintf(t, sizeof t, "r%d", i);
    r.put(unit1(CapKind::Raw, t), 1, (uint32_t)i);
  }
  CHECK(r.count() == CAPTURE_SLOTS && r.lost() == 1);
  r.reset();
  for (int i = 0; i < 16; i++) {
    snprintf(t, sizeof t, "r%d", i);
    r.put(unit1(CapKind::Raw, t), 1, (uint32_t)i);
  }
  r.served(16);
  r.put(unit1(CapKind::Raw, "r16"), 1, 16);
  CHECK(r.lost() == 0);
  r.served(999);  // a since past the newest is held at the newest (17)
  for (int i = 17; i < 34; i++) {  // 17 more: the 17th evicts r17, which no GET returned
    snprintf(t, sizeof t, "r%d", i);
    r.put(unit1(CapKind::Raw, t), 1, (uint32_t)i);
  }
  CHECK(r.lost() == 1);
  // at most 8 per GET, `more`, the next GET the rest
  r.reset();
  for (int i = 0; i < 12; i++) {
    snprintf(t, sizeof t, "e%d", i);
    r.put(unit1(CapKind::Raw, t), 1, (uint32_t)i);
  }
  CHECK(r.snapshot(0, out, CAPTURE_GET_MAX, &more) == 8 && more && out[0].seq == 1 && out[7].seq == 8);
  CHECK(r.snapshot(out[7].seq, out, CAPTURE_GET_MAX, &more) == 4 && !more && out[3].seq == 12);
}

static void testCaptureDnsClients() {
  static CaptureDns d;
  d.reset();
  char nm[32];
  for (int i = 0; i < 17; i++) {
    snprintf(nm, sizeof nm, "host%d.example.net", i);
    d.add(nm, 1, 23);
  }
  CHECK(d.n == CAPTURE_DNS_N && d.other == 1);
  d.add("HOST0.example.net", 28, 23);  // case-folded, AAAA added
  CHECK(d.e[0].n == 2 && d.e[0].types == 3);
  d.reset();
  std::string longName(100, 'a');
  d.add(longName.c_str(), 1, 23);
  CHECK(strlen(d.e[0].name) == CAPTURE_DNS_NAME - 1 && std::string(d.e[0].name).substr(CAPTURE_DNS_NAME - 3) == "..");

  static CaptureClients c;
  c.reset();
  const uint8_t m1[6] = {0x02, 0x00, 0x00, 0x11, 0x22, 0x33}, m2[6] = {0xa4, 0xcf, 0x12, 0x01, 0x02, 0x03};
  const uint32_t ip23 = 0x174da8c0u, ip24 = 0x184da8c0u;  // 192.168.77.23 / .24 in network order
  CHECK(capOctet(ip23) == 23);
  c.join(m1, 1000);
  c.lease(ip24, m1, 1100);
  c.join(m2, 3000);
  c.lease(ip23, m2, 3100);
  c.count(ip23, true);
  c.count(ip24, false);
  c.leave(m1, 9000);
  CHECK(c.n == 2 && c.c[0].gone && c.c[0].left_ms == 9000 && !c.c[1].gone && c.c[1].n == 1 && c.c[1].active);
  c.join(m1, 12000);  // back again: its first join stays (the join / leave records say the rest)
  CHECK(c.n == 2 && !c.c[0].gone && c.c[0].joined_ms == 1000);
  c.leave(m1, 9000);
  char v[9];
  c.vendor(v);
  CHECK(std::string(v) == "a4:cf:12");  // the first device that sent something real, not the phone
  capVendor(m1, v);
  CHECK(std::string(v) == "random");
  // a device that was there before the start; a ninth device takes the place of the one that left longest ago
  for (int i = 0; i < 6; i++) {
    const uint8_t m[6] = {0x12, 0, 0, 0, 0, (uint8_t)i};  // randomised addresses (phones)
    c.join(m, 0, true);
  }
  CHECK(c.n == CAPTURE_CLIENTS && c.c[2].before);
  const uint8_t m9[6] = {0x10, 0, 0, 0, 0, 9};
  c.join(m9, 10000);
  CHECK(c.n == CAPTURE_CLIENTS && !memcmp(c.c[0].mac, m9, 6) && c.over == 0);
  const uint8_t m10[6] = {0x10, 0, 0, 0, 0, 10};
  c.join(m10, 11000);  // nobody left any more
  CHECK(c.over == 1);
  // a lease that moves an address clears it from the old device
  c.lease(ip23, m9, 12000);
  CHECK(c.c[0].ip == ip23 && c.c[1].ip == 0);
  // the GET body never holds a full MAC
  CaptureView w = {};
  w.state = "on";
  w.ended = "";
  w.clients = &c;
  w.dns = &d;
  memcpy(w.vendor, "a4:cf:12", 9);
  std::string j;
  char buf[256];
  Out o(toString, &j, buf, sizeof buf);
  captureRenderJson(o, w);
  o.flush();
  CHECK(jsonParses(j) && !hasMac6(j) && contains(j, "\"ip\":\"192.168.77.23\"") && contains(j, "\"vendor\":\"random\"") &&
        contains(j, "\"joined_s\":null") && contains(j, "\"clients_over\":1"));
}

static void testCaptureRedact() {
  CHECK(redQ("ID=CAPT1&PASSWORD=abc&dateutc=now&tempf=51.2") == "ID=***&PASSWORD=***&dateutc=now&tempf=51.2");
  CHECK(redQ("user=x&passwd=y&apikey=z&token=t&mac=aa:bb&temp=1") == "user=***&passwd=***&apikey=***&token=***&mac=***&temp=1");
  CHECK(redQ("PASS%57ORD=x&a=1") == "PASS%57ORD=***&a=1");  // the key is compared %-decoded
  CHECK(redQ("stationtype=WS&PASSKEY=ab12&humidity=60&dateutc=now&solarRadiation=3") ==
        "stationtype=WS&PASSKEY=***&humidity=60&dateutc=now&solarRadiation=3");
  CHECK(redQ("a&b=&ID=") == "a&b=&ID=***");
  CHECK(redJ("{\"stationKey\":\"k\",\"client_secret\":\"s\",\"access_token\":\"t\",\"device_id\":\"d\",\"uid\":1,\"t\":3.5}") ==
        "{\"stationKey\":***,\"client_secret\":***,\"access_token\":***,\"device_id\":***,\"uid\":***,\"t\":3.5}");
  CHECK(redJ("{\"token\":\"abc") == "{\"token\":***");  // the value cut by the end
  CHECK(redJ("{\"tok") == "{\"tok");                    // the key cut: nothing to hide
  CHECK(redJ("{\"auth\":{\"user\":\"a\",\"pass\":\"b\"},\"t\":1}") == "{\"auth\":***,\"t\":1}");
  CHECK(redJ("[{\"id\": \"x\" , \"v\":\"a\\\"b\"}]") == "[{\"id\":*** , \"v\":\"a\\\"b\"}]");
  CHECK(redP("/v01/set/wid/abc/key/def/temp/1") == "/v01/set/wid/***/key/***/temp/1");
  CHECK(redP("/station/ABC123/data") == "/station/***/data");
  CHECK(redP("/device/x/sn/y") == "/device/***/sn/***");
  CHECK(redP("/api/v1/up") == "/api/v1/up");
  CHECK(capSecretKey("PASSWORD", 8) && capSecretKey("ID", 2) && !capSecretKey("humidity", 8) && !capSecretKey("dateutc", 7) &&
        !capSecretKey("windspeedmph", 12) && !capSecretKey("softwaretype", 12) && !capSecretKey("user-agent", 10));
  // identity keys with a prefix and the usual whole names; never a bare ending "id" (humid, valid)
  CHECK(capSecretKey("station_id", 10) && capSecretKey("stationId", 9) && capSecretKey("siteid", 6) && capSecretKey("sid", 3) &&
        capSecretKey("client_id", 9) && capSecretKey("clientId", 8) && capSecretKey("dev_id", 6) && capSecretKey("X-Device-Id", 11) &&
        !capSecretKey("humid", 5) && !capSecretKey("valid", 5) && !capSecretKey("_id", 3));
  CHECK(redQ("station_id=S1&siteid=9&humid=60") == "station_id=***&siteid=***&humid=60");
  // a path segment that looks like a key: 16+ letters / digits / - _, both letters and digits
  CHECK(redP("/pws/update/0a1b2c3d4e5f6a7b8c9d") == "/pws/update/***");
  CHECK(redP("/v1/3fa85f64-5717-4562-b3fc-2c963f66afa6/obs") == "/v1/***/obs");
  CHECK(redP("/api/v1/observations-latest") == "/api/v1/observations-latest");  // letters only: kept
  CHECK(redP("/weatherstation/updateweatherstation.php") == "/weatherstation/updateweatherstation.php");
  char out[58];
  capRedactUri("/weatherstation/updateweatherstation.php?ID=K1&PASSWORD=x", out, sizeof out);
  CHECK(std::string(out) == "/weatherstation/updateweatherstation.php?ID=***&PASSWORD=");  // cut after the redaction
}

static void testCaptureHttp() {
  CapHttpIn in = {};
  in.method = "GET";
  // a long query arrives whole across two records, the secret hidden before the cut
  std::string q = "/weatherstation/updateweatherstation.php?ID=K1&PASSWORD=SECRETVALUE&dateutc=now";
  while (q.size() < 430) q += "&field" + std::to_string(q.size()) + "=12.5";
  in.uri = q.c_str();
  CapUnit u = {};
  capHttpUnit(in, &u);
  CHECK(u.n == 2 && u.it[0].kind == CapKind::Http && u.it[1].kind == CapKind::Http);
  const std::string p0 = u.it[0].text, p1 = u.it[1].text;
  CHECK(p0.size() == CAPTURE_TEXT - 1 && p0.substr(p0.size() - 3) == "..." && p1.substr(0, 3) == "...");
  char red[700];
  capRedactUri(q.c_str(), red, sizeof red);
  CHECK(p0.substr(0, p0.size() - 3) + p1.substr(3) == "GET " + std::string(red));
  CHECK(!contains(p0 + p1, "SECRETVALUE"));
  // the fixed header list, in its order, secrets hidden, values with a space quoted
  in.uri = "/u";
  in.hdr[3] = "12";  // Content-Length
  in.hdr[0] = "rtupdate.wunderground.com";
  in.hdr[1] = "Mozilla/5.0 (X11)";
  in.hdr[11] = "Bearer abc";
  in.hdr[12] = "sid=1";
  in.closed_before_reply = true;
  capHttpUnit(in, &u);
  CHECK(u.n == 2 && std::string(u.it[1].text) ==
                        "host=rtupdate.wunderground.com ua=\"Mozilla/5.0 (X11)\" cl=12 auth=*** cookie=*** closed-before-reply");
  // a body: its head redacted; chunked not read; none in time
  const char* body = "{\"token\":\"t\",\"t\":1}";
  in.method = "POST";
  in.body = (const uint8_t*)body;
  in.body_len = strlen(body);
  in.content_len = 412;
  capHttpUnit(in, &u);
  CHECK(u.n == 3 && u.it[2].kind == CapKind::Body && std::string(u.it[2].text) == "json 412B: {\"token\":***,\"t\":1}");
  in.body_len = 0;
  capHttpUnit(in, &u);
  CHECK(u.n == 3 && std::string(u.it[2].text) == "412B announced, none arrived in 3 s");
  in.chunked = true;
  capHttpUnit(in, &u);
  CHECK(std::string(u.it[2].text) == "chunked, not read");
  CapHttpIn g = {};
  g.method = "GET";
  g.uri = "/generate_204";
  capHttpUnit(g, &u);
  CHECK(u.n == 1);  // no header of the list, no body: the line only
  CapHttpIn ol = {};
  ol.over_limits = true;
  capHttpUnit(ol, &u);
  CHECK(u.n == 1 && contains(u.it[0].text, "URI 512 B"));
  // a binary body as hex, a form body redacted
  const uint8_t bin[40] = {0x00, 0x01, 0xff};
  char t[CAPTURE_TEXT];
  capBodyText(bin, sizeof bin, 40, t, sizeof t);
  CHECK(std::string(t).substr(0, 18) == "hex 32 of 40B: 00 " && std::string(t).size() == 14 + 32 * 3);
  const char* form = "mac=aa:bb&temp=1";
  capBodyText((const uint8_t*)form, strlen(form), 0, t, sizeof t);
  CHECK(std::string(t) == "form 16B: mac=***&temp=1");
  // UTF-8 in a body head is text: redacted, the high bytes shown as '?', never dumped as hex
  const char* utf = "{\"passkey\":\"0123456789abcdef\",\"name\":\"Z\xc3\xbc" "rich\"}";
  capBodyText((const uint8_t*)utf, strlen(utf), 0, t, sizeof t);
  CHECK(std::string(t) == "json " + std::to_string(strlen(utf)) + "B: {\"passkey\":***,\"name\":\"Z??rich\"}");
  CHECK(!contains(t, "0123456789abcdef"));
  const uint8_t del[] = {'a', '=', '1', 0x7f};  // DEL is a control byte: binary
  capBodyText(del, sizeof del, 0, t, sizeof t);
  CHECK(std::string(t) == "hex 4B: 61 3d 31 7f");
}

static void testCaptureTls() {
  // the suite families and their verdict against the vendored BearSSL server set
  const std::vector<uint16_t> MBED = {0xc02c, 0xc02b, 0xc030, 0xc02f, 0xcca9, 0xcca8, 0xc024, 0xc023, 0x009d, 0x009c, 0x00ff};
  std::string s = helloText(helloWith(MBED, {}, {0x0017, 0x001d}, "api.example.net"));
  CHECK(s == "verdict=ecdhe-ecdsa-gcm v=1.2 sni=api.example.net suites=c02c,c02b,c030,c02f,cca9,cca8,c024,c023,009d,009c "
             "groups=p256,x25519 sigalg=yes mfl=- alpn=-");
  CHECK(contains(helloText(helloWith({0xc02f, 0xc030}, {}, {0x0017}, "a")), "verdict=ecdhe-rsa-gcm"));
  CHECK(contains(helloText(helloWith({0xc013, 0xc014}, {}, {0x0017}, "a")), "verdict=cbc-only"));
  CHECK(contains(helloText(helloWith({0x002f, 0x0035}, {}, {}, "a")), "verdict=rsa-kx-only"));
  CHECK(contains(helloText(helloWith({0x1301, 0x1302}, {0x0304}, {0x001d}, "a")), "verdict=tls13-only v=1.3"));
  CHECK(contains(helloText(helloWith({0xc02b}, {}, {0x0018}, "a")), "verdict=no-common-curve"));
  CHECK(contains(helloText(helloWith({0xc02b}, {}, {}, "a", 0, 0x0301)), "verdict=tls-old v=1.0"));
  // an OpenSSL-like hello offering 1.3 and 1.2 with GREASE: both versions shown, GREASE ignored, 1.2 usable
  const std::string o = helloText(helloWith({0x2a2a, 0x1301, 0x1302, 0xc02b, 0xc02f}, {0x3a3a, 0x0304, 0x0303}, {0x4a4a, 0x001d}, "x.example"));
  CHECK(contains(o, "verdict=ecdhe-ecdsa-gcm v=1.3,1.2 sni=x.example suites=1301,1302,c02b,c02f groups=x25519"));
  // a 600-B hello cut at 512: the suites whole, the extensions partial
  std::vector<uint16_t> many;
  for (uint16_t i = 0; i < 20; i++) many.push_back((uint16_t)(0xc001 + i));
  many.push_back(0xc02b);
  const std::vector<uint8_t> big = helloWith(many, {}, {0x0017}, nullptr, 520);
  CHECK(big.size() > 600);
  const std::string c = helloText(big, 512);
  CHECK(contains(c, "verdict=ecdhe-ecdsa-gcm") && contains(c, " +9") && contains(c, " cut") && contains(c, "alpn=?"));
  // every truncation is safe; not a hello
  for (size_t k = 0; k < big.size(); k++) helloText(big, k);
  TlsHello x;
  const uint8_t http[] = "GET / HTTP/1.1\r\n\r\n";
  CHECK(!tlsHello(http, sizeof http - 1, &x));
  // the project's own BearSSL client (the in-process server's first bytes): the verdict a 1.2 server would need
  {
    TlsServer srv;
    struct {
      br_x509_trust_anchor ta;
    } ra{pki().taA};
    tlsRun(srv, &ra.ta, 1);
    const std::string b = helloText(srv.hello);
    CHECK(contains(b, "verdict=ecdhe-ecdsa-gcm v=1.2 sni=test.example") && contains(b, "c02b"));
  }
  // the record text never runs past its buffer
  char small[20];
  TlsHello h;
  const std::vector<uint8_t> m = helloWith(MBED, {0x0304, 0x0303}, {0x0017, 0x001d, 0x0018, 0x0019}, "a-very-long-name.example.net");
  CHECK(tlsHello(m.data(), m.size(), &h) && tlsHelloText(h, small, sizeof small) == sizeof small - 1);
}

static void testCapturePeek() {
  CapUnit u = {};
  CHECK(peek(443, clientHello("api.example.net", true), &u) ==
        "tcp|:443\ntls|:443 verdict=tls13-only v=1.2 sni=api.example.net suites=1301,1302 groups=- sigalg=no mfl=- alpn=-\n");
  // an MQTT CONNECT: a summary, never the client id or the credentials
  std::vector<uint8_t> mq = {0x10, 0x26, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0xC2, 0x00, 0x3C, 0x00, 0x0C};
  const char* rest = "abcdefghijklUSERNAMEpw";
  mq.insert(mq.end(), rest, rest + strlen(rest));
  const std::string m = peek(1883, mq, &u);
  CHECK(m == "tcp|:1883\nmqtt|:1883 MQTT 3.1.1 flags=user,pass,clean keepalive=60 clientid=12B\n" && !contains(m, "abcdef"));
  // unknown bytes: 16 hex pairs; nothing; a busy refusal
  std::vector<uint8_t> raw(20);
  for (size_t i = 0; i < raw.size(); i++) raw[i] = (uint8_t)(0x80 + i);
  CHECK(peek(8080, raw, &u) == "tcp|:8080\nraw|:8080 20B: 80 81 82 83 84 85 86 87 88 89 8a 8b 8c 8d 8e 8f\n");
  CHECK(peek(8080, {}, &u) == "tcp|:8080 no data\n");
  CapPeekIn busy = {8443, nullptr, 0, false, true};
  capPeekUnit(busy, &u);
  CHECK(u.n == 1 && std::string(u.it[0].text) == ":8443 busy, closed");
  // HTTP on another port: the line, every header (secrets hidden), the body's head
  const std::string req = "POST /api/v2/upload?key=ABC&t=1 HTTP/1.1\r\nHost: api.example.net\r\nUser-Agent: ESP32HTTPClient\r\n"
                          "Authorization: Basic dXNlcjpwYXNz\r\nContent-Type: application/json\r\nContent-Length: 30\r\n\r\n"
                          "{\"passkey\":\"P\",\"temp\":21.5}";
  const std::string h = peek(8080, std::vector<uint8_t>(req.begin(), req.end()), &u);
  CHECK(h == "tcp|:8080\nhttp|:8080 POST /api/v2/upload?key=***&t=1\nhdr|host=api.example.net user-agent=ESP32HTTPClient "
             "authorization=*** content-type=application/json content-length=30\nbody|json 30B: {\"passkey\":***,\"temp\":21.5}\n");
  CHECK(!contains(h, "dXNlcjpwYXNz") && !contains(h, "ABC"));
  // twelve long headers: cut at a whole pair, " ..." at the end
  std::string many = "GET / HTTP/1.1\r\n";
  for (int i = 0; i < 12; i++) many += "X-Header-" + std::to_string(i) + ": " + std::string(20, 'v') + "\r\n";
  many += "\r\n";
  peek(80, std::vector<uint8_t>(many.begin(), many.end()), &u);
  const std::string hd = u.it[2].text;
  CHECK(u.it[2].kind == CapKind::Hdr && hd.substr(hd.size() - 4) == " ..." && hd.size() <= CAPTURE_TEXT - 1 &&
        hd[hd.size() - 5] == 'v');
}

static void testCaptureMachine() {
  CaptureMachine m;
  CapTickIn in = {};
  const uint32_t t0 = 0xFFFF0000u;  // across the millis() wrap
  CHECK(m.start(t0, false, false) == CapPress::Ok && m.state() == CapSt::Drain && m.quiet() && !m.mqttOff());
  CHECK(m.start(t0, false, false) == CapPress::On);
  in.now_ms = t0;
  CHECK(acts(m.tick(in)) == "fwd_abort");
  in.now_ms = t0 + 2999;
  CHECK(acts(m.tick(in)) == "");
  in.now_ms = t0 + 3000;  // the job did not stop in 3 s: go on anyway
  CHECK(acts(m.tick(in)) == "publish_offline" && m.state() == CapSt::Offline);
  in.now_ms += 10;
  in.offline_done = true;
  CHECK(acts(m.tick(in)) == "mqtt_off" && m.mqttOff() && m.state() == CapSt::Gate);
  in.now_ms += 10;  // the client is still there: wait (up to 2 s)
  CHECK(acts(m.tick(in)) == "" && m.state() == CapSt::Gate);
  in.mqtt_gone = true;
  in.heap_ok = true;
  CHECK(acts(m.tick(in)) == "alloc,open,log_start" && m.on());
  const uint32_t on = in.now_ms;
  // a poll keeps it on; an hour without one stops it
  in.now_ms = on + CAPTURE_IDLE_MS - 1;
  m.poll(in.now_ms);
  CHECK(acts(m.tick(in)) == "" && m.idleLeft(in.now_ms) == CAPTURE_IDLE_MS);
  in.now_ms += CAPTURE_IDLE_MS - 1;
  CHECK(acts(m.tick(in)) == "" && m.idleLeft(in.now_ms) == 1);
  in.now_ms += 1;
  CHECK(acts(m.tick(in)) == "close,free,mqtt_on,log_stop" && m.ended() == CapEnd::Idle && !m.quiet());
  // the press: an OTA, a planned restart
  CHECK(m.start(0, true, false) == CapPress::Ota && m.start(0, false, true) == CapPress::Restart && !m.quiet());
  // Stop at every step: only what was done is undone
  CHECK(m.start(100, false, false) == CapPress::Ok);
  m.stop();
  in = {};
  in.now_ms = 100;
  CHECK(acts(m.tick(in)) == "log_stop" && m.ended() == CapEnd::Stop);
  m.start(100, false, false);
  in.net_idle = true;
  CHECK(acts(m.tick(in)) == "fwd_abort,publish_offline");
  in.offline_done = true;
  CHECK(acts(m.tick(in)) == "mqtt_off");
  m.stop();
  CHECK(acts(m.tick(in)) == "mqtt_on,log_stop");
  // a stop between the retained "offline" and the client's stop: MQTT is told to come back (so "online" again)
  m.start(100, false, false);
  in.offline_done = false;
  CHECK(acts(m.tick(in)) == "fwd_abort,publish_offline" && m.state() == CapSt::Offline && !m.mqttOff());
  m.stop();
  CHECK(acts(m.tick(in)) == "mqtt_on,log_stop" && !m.quiet() && m.ended() == CapEnd::Stop);
  in.offline_done = true;
  // a restart planned while starting; a heap gate that says no; an allocation that fails
  m.start(100, false, false);
  in.restart = true;
  CHECK(acts(m.tick(in)) == "log_stop" && m.ended() == CapEnd::Restart);
  in.restart = false;
  m.start(100, false, false);
  m.tick(in);
  m.tick(in);
  in.mqtt_gone = true;
  in.heap_ok = false;
  CHECK(acts(m.tick(in)) == "mqtt_on,log_refused" && m.ended() == CapEnd::Heap);
  m.start(100, false, false);
  m.tick(in);
  m.tick(in);
  in.heap_ok = true;
  CHECK(acts(m.tick(in)) == "alloc,open,log_start");
  CHECK(acts(m.allocFailed()) == "mqtt_on,log_refused" && !m.quiet());
  // the client never goes: the gate is read after 2 s anyway
  m.start(0, false, false);
  in = {};
  in.net_idle = in.offline_done = in.heap_ok = true;
  m.tick(in);
  m.tick(in);
  CHECK(acts(m.tick(in)) == "");
  in.now_ms = CAPTURE_MQTT_GONE_MS;
  CHECK(acts(m.tick(in)) == "alloc,open,log_start");
  // the bench's shorter idle limit is clamped
  m.stop();
  m.tick(in);
  m.start(0, false, false, 1000);
  CHECK(m.idleMs() == CAPTURE_IDLE_MIN_MS);
  // a poll stamped after the tick read its clock (a GET on the other core): no idle stop (bench 2026-11-21: it stopped
  // a 2-minute capture as "idle")
  {
    CaptureMachine q;
    CapTickIn qi = {};
    qi.net_idle = qi.offline_done = qi.mqtt_gone = qi.heap_ok = true;
    q.start(1000, false, false);
    q.tick(qi);
    q.tick(qi);
    CHECK(acts(q.tick(qi)) == "alloc,open,log_start" && q.on());
    qi.now_ms = 200000;
    q.poll(200005);  // the GET's millis() is 5 ms after the tick's
    CHECK(acts(q.tick(qi)) == "" && q.on() && q.idleLeft(200000) == CAPTURE_IDLE_MS);
    qi.now_ms = 200005 + CAPTURE_IDLE_MS;
    CHECK(acts(q.tick(qi)) == "close,free,mqtt_on,log_stop" && q.ended() == CapEnd::Idle);
  }
  // a record's time: a clock read before the start's stamp (another task) is 0, not 49 days; across the wrap it counts on
  CHECK(capSince(1000, 1005) == 0 && capSince(1005, 1000) == 5 && capSince(4u, 0xFFFFFFF0u) == 20 && capSince(7, 7) == 0);
  // which port-80 requests are recorded: the bridge runs, the request came on the fauxcloud Wi-Fi, the mode is on
  CHECK(capWants(true, true, true) && !capWants(false, true, true) && !capWants(true, false, true) && !capWants(true, true, false));
}

static void testCaptureJsonWidest() {
  // eight records of 231 quote characters (each doubles under escaping), the widest counters: the body parses
  static CaptureRing r;
  r.reset();
  std::string q(CAPTURE_TEXT - 1, '"');
  for (int i = 0; i < 8; i++) {
    q[0] = (char)('a' + i);
    r.put(unit1(CapKind::Hdr, q.c_str()), 255, 0xFFFFFFF0u);
  }
  CaptureRec recs[CAPTURE_GET_MAX];
  bool more = false;
  CaptureView v = {};
  v.state = "starting";
  v.ended = "restart";
  v.elapsed_s = v.idle_left_s = v.started_utc = v.ntp_asked = v.ntp_answered = v.uploads_skipped = UINT32_MAX;
  v.seq = v.lost = UINT32_MAX;
  v.recs = recs;
  v.nrecs = r.snapshot(0, recs, CAPTURE_GET_MAX, &more);
  static CaptureDns d;
  d.reset();
  for (int i = 0; i < CAPTURE_DNS_N; i++) d.add((std::string(60, '"') + std::to_string(i)).c_str(), 28, 255);
  v.dns = &d;
  std::string j;
  char buf[256];
  Out o(toString, &j, buf, sizeof buf);
  captureRenderJson(o, v);
  o.flush();
  CHECK(v.nrecs == 8 && jsonParses(j) && contains(j, "\"started_utc\":4294967295"));
  // after the stop: no clients, no names, no events
  CaptureView off = {};
  off.state = "off";
  off.ended = "stop";
  std::string k;
  Out o2(toString, &k, buf, sizeof buf);
  captureRenderJson(o2, off);
  o2.flush();
  CHECK(jsonParses(k) && contains(k, "\"clients\":[]") && contains(k, "\"names\":[]") && contains(k, "\"events\":[]") &&
        contains(k, "\"started_utc\":null") && contains(k, "\"vendor\":null"));
}

// the LED while listening: a steady light, not the attention pulse of a stopped MQTT client; a firmware upload wins
static void testCaptureLed() {
  LedInputs in = {};
  in.sta_up = true;
  in.uptime_ms = 60000;
  CHECK(ledState(in) == LedState::Ok);
  in.capture = true;
  in.attention = true;  // the stopped client
  CHECK(ledState(in) == LedState::Listening);
  CHECK(ledOn(LedState::Listening, 0, true) && ledOn(LedState::Listening, 2999, true) && ledOn(LedState::Listening, 5999, false));
  in.ota = true;
  CHECK(ledState(in) == LedState::Setup);
}

static void testCapture() {
  testCaptureLed();
  testCaptureRing();
  testCaptureDnsClients();
  testCaptureRedact();
  testCaptureHttp();
  testCaptureTls();
  testCapturePeek();
  testCaptureMachine();
  testCaptureJsonWidest();
}
