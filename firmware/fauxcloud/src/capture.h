// capture.h -- "Listen to a new console": the pure parts of the listen-only mode. The sketch (app_capture.cpp)
// owns the sockets, the lwIP listeners and the buffers; everything that decides what a record says, what is hidden,
// what counts as the same thing again and in which order the mode starts and stops lives here, host-tested.
//
// While the mode is on, every request a device sends on the bridge's own Wi-Fi becomes a few short records: the DNS
// names it asks (a counted table), its joins / leases / leaves, NTP, every HTTP request on port 80 (the query whole,
// a fixed list of headers, the head of a body), and the first bytes of a connection on eight more TCP ports (a TLS
// ClientHello's facts, an HTTP request, an MQTT CONNECT summary, else hex). Secrets are hidden here, before a record
// is stored; the admin page polls the records and builds the report. Nothing is answered beyond what the run mode
// answers anyway, nothing is stored on flash, nothing leaves the board.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fc {

class Out;

constexpr int CAPTURE_SLOTS = 16;          // records the board keeps; the page keeps the history
constexpr size_t CAPTURE_TEXT = 232;       // a record's text, NUL included (a record is 256 B)
constexpr int CAPTURE_GET_MAX = 8;         // records per GET ("more" when others wait)
constexpr int CAPTURE_DNS_N = 16;          // distinct names; the rest are counted as "other"
constexpr size_t CAPTURE_DNS_NAME = 40;    // a name, NUL included (a longer one ends in "..")
constexpr int CAPTURE_CLIENTS = 8;         // devices on the fauxcloud Wi-Fi
constexpr uint32_t CAPTURE_IDLE_MS = 3600000;  // no GET for this long: nobody is watching, the mode stops
constexpr uint32_t CAPTURE_IDLE_MIN_MS = 60000;  // the shortest a start may ask for (bench)
constexpr size_t CAPTURE_PEEK_MAX = 512;   // the first bytes of a connection on a peek port
constexpr uint32_t CAPTURE_PEEK_MS = 300;  // how long a peek waits for them
constexpr size_t CAPTURE_BODY_HEAD = 128;  // the head of an HTTP body on port 80
constexpr size_t CAPTURE_BODY_HEX = 32;    // a binary body as hex
constexpr size_t CAPTURE_RAW_HEX = 16;     // unknown bytes on a peek port as hex
constexpr uint32_t CAPTURE_DRAIN_MS = 3000;    // a running online job gets this long to stop (the OTA's figure)
constexpr uint32_t CAPTURE_OFFLINE_MS = 1000;  // the retained "offline" gets this long for its acknowledgement
constexpr uint32_t CAPTURE_MQTT_GONE_MS = 2000;  // the MQTT client's stop is awaited this long
constexpr int CAPTURE_PORT_N = 8;
extern const uint16_t CAPTURE_PORTS[CAPTURE_PORT_N];  // 443, 8080, 8443, 8000, 8880, 1883, 8883, 5000

// The start's heap gate (measured after the MQTT client stopped): the buffers' sum + this margin, and the largest
// block at least the ring (the biggest of the four allocations).
constexpr uint32_t CAPTURE_MARGIN8 = 12u * 1024, CAPTURE_LARGEST_MIN = 4608;

// ---------------------------------------------------------------- records
enum class CapKind : uint8_t { Join, Lease, Leave, Ntp, Tcp, Tls, Http, Hdr, Body, Mqtt, Raw, COUNT };
const char* capKindName(CapKind k);  // join lease leave ntp tcp tls http hdr body mqtt raw

struct CaptureRec {
  uint32_t id;               // the seq it was first written with: its identity for the page
  uint32_t seq;              // bumped on every change (a repeat counted): what a GET's `since` compares
  uint32_t group;            // the id of the first record of the unit it was written in
  uint32_t t_first, t_last;  // ms since the start
  uint16_t n;                // times seen
  uint8_t kind;              // CapKind
  uint8_t client;            // the last octet of the device's address on the bridge's Wi-Fi, 0 = unknown
  char text[CAPTURE_TEXT];
};
static_assert(sizeof(CaptureRec) == 256, "a record is 256 B");

// One record to write: its kind and text (a unit is up to CAPTURE_UNIT_MAX of these, written together).
struct CapItem {
  CapKind kind;
  char text[CAPTURE_TEXT];
};
constexpr int CAPTURE_UNIT_MAX = 5;  // an HTTP request: up to three `http` parts, `hdr`, `body`
struct CapUnit {
  int n;
  CapItem it[CAPTURE_UNIT_MAX];
  void add(CapKind k, const char* text);
};

// The ring of the latest records. A unit equal to a resident one (same kinds, texts and client, the same size) is
// counted again: each of its records takes a new seq (so a GET returns it again with its new count); else it is
// appended, evicting the records changed longest ago. `lost` counts records evicted before any GET returned them.
class CaptureRing {
 public:
  void reset();
  void put(const CapUnit& u, uint8_t client, uint32_t t);
  // the same for n records given as arrays (a single record from a task with a small stack: no CapUnit copy)
  void putItems(int n, const CapKind* kinds, const char* const* texts, uint8_t client, uint32_t t);
  void put1(CapKind k, const char* text, uint8_t client, uint32_t t) { putItems(1, &k, &text, client, t); }
  // a GET asked with `since`: what the page holds from now on (lost compares against the highest)
  void served(uint32_t since);
  // the records with seq > since, oldest change first, at most max; *more = others wait
  int snapshot(uint32_t since, CaptureRec* out, int max, bool* more) const;
  uint32_t seq() const { return seq_; }
  uint32_t lost() const { return lost_; }
  uint32_t written() const { return written_; }  // records appended (repeats not counted)
  int count() const;

 private:
  int find(uint32_t id) const;
  CaptureRec r_[CAPTURE_SLOTS];
  uint32_t seq_ = 0, served_ = 0, lost_ = 0, written_ = 0;
};

// DNS names asked, counted per name (the console asks the same few again and again).
struct CapDnsEnt {
  char name[CAPTURE_DNS_NAME];
  uint16_t n;
  uint8_t types;   // bit 0 A, bit 1 AAAA, bit 2 another type
  uint8_t client;  // the first asker
};
struct CaptureDns {
  int n;
  uint32_t other;  // asks for names beyond the table
  uint32_t gen;    // bumped on every change
  CapDnsEnt e[CAPTURE_DNS_N];
  void reset();
  void add(const char* name, uint16_t qtype, uint8_t client);
};

// The devices on the bridge's Wi-Fi during the mode (joined before it or during it). The full MAC is kept here only to
// match a leave or a lease to its join; it never leaves the board (the vendor prefix does, or "random").
struct CapClient {
  uint8_t mac[6];
  uint32_t ip;         // network byte order, 0 = no lease seen
  uint32_t joined_ms;  // since the start; `before` = it was there already
  uint32_t left_ms;    // since the start, valid while `gone`
  uint16_t n;          // records with its address
  bool before, gone, active;  // active: sent something that is not a phone's connectivity probe or DNS
};
struct CaptureClients {
  int n;
  uint32_t over;  // devices that found the list full (none had left)
  CapClient c[CAPTURE_CLIENTS];
  void reset();
  // the AP events (the Wi-Fi event task); a ninth device replaces the one that left longest ago
  void join(const uint8_t mac[6], uint32_t t, bool before = false);
  void lease(uint32_t ip, const uint8_t mac[6], uint32_t t);
  void leave(const uint8_t mac[6], uint32_t t);
  // a record from this address (`active`: not a probe, not DNS)
  void count(uint32_t ip, bool active);
  // the vendor prefix of the first active device ("" = none yet), "random" for a locally administered MAC
  void vendor(char out[9]) const;
};
void capVendor(const uint8_t mac[6], char out[9]);  // "aa:bb:cc" or "random"
inline uint8_t capOctet(uint32_t ip_net) { return (uint8_t)(ip_net >> 24); }  // 192.168.77.23 -> 23 (little-endian)

// ---------------------------------------------------------------- redaction (before a record is stored)
// A key that names a secret or an identity: it contains key / pass / pwd / token / secret / auth / sign / cred, or it is
// id / uid / did / devid / deviceid / device_id / mac / sn / serial / wid / user / imei (ASCII case-insensitive; the
// key is compared %-decoded). Over-hiding is the safe side.
bool capSecretKey(const char* k, size_t n);
// a=b&c=d (a query or a form body): a secret key's value -> ***
size_t capRedactQuery(const char* in, size_t n, char* out, size_t cap);
// a path: the segment after wid / key / id / token / user / pass / station / device / dev / sn / serial / mac -> ***
size_t capRedactPath(const char* in, size_t n, char* out, size_t cap);
// JSON (or its head, cut anywhere): "k": value -> "k":*** for a secret key, the value cut by the end included
size_t capRedactJson(const char* in, size_t n, char* out, size_t cap);
// a request URI: the path and the query, each redacted
size_t capRedactUri(const char* uri, char* out, size_t cap);
// every byte outside printable ASCII -> '?', in place (spaces kept)
void capPrintable(char* s);

// ---------------------------------------------------------------- what a request or a connection becomes
// A header of the port-80 list: its record key and whether its value is hidden.
struct CapHdrName {
  const char* name;  // as the request names it (the httpd looks it up by name)
  const char* key;   // the record's short key
};
constexpr int CAPTURE_HDR_N = 15;
extern const CapHdrName CAPTURE_HDRS[CAPTURE_HDR_N];
// One request on port 80 (the httpd parsed it): method, URI, the values of CAPTURE_HDRS (nullptr = not sent) and
// the body head. peek_port = 0.
struct CapHttpIn {
  const char* method;
  const char* uri;
  const char* hdr[CAPTURE_HDR_N];
  const uint8_t* body;  // the head read, or nullptr
  size_t body_len;      // bytes in `body`
  size_t content_len;   // the announced length (0 = none)
  bool chunked;         // Transfer-Encoding: chunked (not read)
  bool body_timeout;    // a body was announced and nothing arrived
  bool closed_before_reply;
  bool over_limits;     // 414 / 431: the httpd did not parse it
  bool headers_over;    // 431 (else 414)
};
void capHttpUnit(const CapHttpIn& in, CapUnit* u);
// The httpd task's scratch for one port-80 request (the header values, the body head, the records): it lives in the
// block allocated at the start, so a request adds no kilobytes to the httpd task's 8 KB stack.
struct CapHttpScratch {
  char hv[CAPTURE_HDR_N][64];  // a longer value is cut (the httpd says TRUNC)
  uint8_t head[CAPTURE_BODY_HEAD];
  CapUnit u;
};
// Whether a port-80 request is recorded: the bridge runs (not the setup face), the request came on the fauxcloud
// Wi-Fi (the console's side, never the home network) and the mode is on.
inline bool capWants(bool mode_run, bool ap_side, bool on) { return mode_run && ap_side && on; }
// ms since the mode went on; a clock read before the start's stamp (another task, the other core) is 0, never 49 days
inline uint32_t capSince(uint32_t now, uint32_t on_ms) {
  const int32_t d = (int32_t)(now - on_ms);
  return d > 0 ? (uint32_t)d : 0;
}
// The body head as one record's text: "json 412B: {...}", "form 63B: a=b", "text 20B: ...", "hex 32 of 412B: 00 ..".
void capBodyText(const uint8_t* b, size_t n, size_t total, char* out, size_t cap);

// The first bytes of a connection on a peek port: a `tcp` record (":443", ":443 no data", ":443 busy, closed") and
// what the bytes are (`tls`, `http` + `hdr` + `body`, `mqtt`, `raw`).
struct CapPeekIn {
  uint16_t port;
  const uint8_t* d;
  size_t n;
  bool cut;     // more arrived than CAPTURE_PEEK_MAX
  bool busy;    // refused: another peek was being read
};
void capPeekUnit(const CapPeekIn& in, CapUnit* u);
// An MQTT CONNECT summary ("MQTT 3.1.1 flags=user,pass,clean keepalive=60 clientid=12B"): never the client id, the user
// or the password themselves. false = not a CONNECT.
bool capMqttConnect(const uint8_t* d, size_t n, char* out, size_t cap);

// ---------------------------------------------------------------- the start / stop order
enum class CapSt : uint8_t { Off, Drain, Offline, Gate, On };
enum class CapAct : uint8_t {
  FwdAbort,        // the running online job stops at its next step
  PublishOffline,  // the bridge's availability "offline", retained, QoS 1 (a clean disconnect drops the will)
  MqttOff,         // the MQTT client stops (and no start while the mode runs)
  Alloc,           // the buffers (after the heap gate)
  Open,            // the peek listeners
  LogStart,
  Close,           // the listeners and a live peek, under the lwIP core lock
  Free,
  MqttOn,          // the MQTT client may start again (the normal start: "online", the discovery)
  LogStop,
  LogRefused,      // the heap gate said no
};
const char* capActName(CapAct a);
enum class CapPress : uint8_t { Ok, Ota, Restart, On };  // a start press: ok or the 409 code
enum class CapEnd : uint8_t { None, Stop, Idle, Restart, Heap };
const char* capEndName(CapEnd e);  // "" stop idle restart heap
struct CapTickIn {
  uint32_t now_ms;
  bool net_idle;      // no online job posted or running
  bool offline_done;  // the retained "offline" was acknowledged, or there is no broker
  bool mqtt_gone;     // no MQTT client exists
  bool heap_ok;       // the gate, read after the client stopped
  bool restart;       // a planned restart or factory reset
};
struct CapActs {
  int n;
  CapAct a[8];
  void add(CapAct x) {
    if (n < 8) a[n++] = x;
  }
};
// One owner of the order. The loop task executes the actions a tick returns, in order; the press and the poll come
// from the httpd task (the sketch serialises them with the loop under its own lock).
class CaptureMachine {
 public:
  CapPress start(uint32_t now, bool ota, bool restart, uint32_t idle_ms = CAPTURE_IDLE_MS);
  void stop() { stop_ = st_ != CapSt::Off; }
  void poll(uint32_t now) { poll_ms_ = now; }
  CapActs tick(const CapTickIn& in);
  CapActs allocFailed();  // the executor's malloc refused: back off like a failed gate
  CapSt state() const { return st_; }
  bool quiet() const { return st_ != CapSt::Off; }  // uploads skipped, online jobs held, MQTT not started
  bool mqttOff() const { return mqtt_off_; }        // the client must be stopped
  bool on() const { return st_ == CapSt::On; }
  CapEnd ended() const { return end_; }
  uint32_t startedMs() const { return t_start_; }
  uint32_t onMs() const { return t_on_; }
  uint32_t idleMs() const { return idle_ms_; }
  // ms until an unwatched mode stops (0 when not on)
  uint32_t idleLeft(uint32_t now) const;

 private:
  CapActs finish(CapEnd why);
  CapSt st_ = CapSt::Off;
  bool stop_ = false, aborted_ = false, offline_ = false, mqtt_off_ = false, alloc_ = false, open_ = false;
  CapEnd end_ = CapEnd::None;
  uint32_t t_start_ = 0, t_step_ = 0, t_on_ = 0, poll_ms_ = 0, idle_ms_ = CAPTURE_IDLE_MS;
};

// ---------------------------------------------------------------- the GET body
struct CaptureView {
  const char* state;     // off starting on
  const char* ended;     // "" while none
  const char* blocked;   // why a start would be refused now: "" ota restart (the page cannot read a 409's body)
  uint32_t elapsed_s;    // since On (0 before)
  uint32_t idle_left_s;  // until an unwatched mode stops
  uint32_t started_utc;  // 0 = no real clock at the start
  bool clock;            // the bridge has a real clock now
  uint32_t ntp_asked, ntp_answered;
  uint32_t uploads_skipped;
  char vendor[9];
  uint32_t seq, lost;
  bool more;
  const CaptureClients* clients;  // nullptr after the stop
  const CaptureDns* dns;
  const CaptureRec* recs;
  int nrecs;
};
// {"state","ended","blocked","elapsed_s","idle_left_s","started_utc","clock","ntp":{..},"uploads_skipped","vendor","seq","lost",
//  "more","clients":[..],"dns":{"other":n,"names":[..]},"events":[..]} -- texts through the escaping writer
void captureRenderJson(Out& o, const CaptureView& v);

}  // namespace fc
