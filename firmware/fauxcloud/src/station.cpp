// station.cpp -- see station.h.
#include "station.h"

#include "derive.h"
#include "forecast.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace fc {

const char* srcIfName(SrcIf i) {
  switch (i) {
    case SrcIf::Ap: return "ap";
    case SrcIf::Sta: return "sta";
    case SrcIf::Mixed: return "mixed";
    case SrcIf::Unknown: break;
  }
  return "unknown";
}

SrcIf classifySrc(uint32_t local, uint32_t peer, uint32_t ap_ip) {
  if (!local) return SrcIf::Unknown;
  uint8_t p[4], a[4];  // the first three bytes in memory are the /24 in network order, whatever the host order
  memcpy(p, &peer, 4);
  memcpy(a, &ap_ip, 4);
  const bool localAp = local == ap_ip, peerAp = memcmp(p, a, 3) == 0;
  if (localAp && peerAp) return SrcIf::Ap;
  if (!localAp && !peerAp) return SrcIf::Sta;
  return SrcIf::Mixed;
}

const char* Station::displayName(char* buf, size_t cap) const {
  if (cfg.name[0]) snprintf(buf, cap, "%s", cfg.name);
  else snprintf(buf, cap, "fauxcloud %s", id);
  return buf;
}

KeyCheck checkStationKey(Station& st, const uint8_t key[32]) {
  if (!st.key_bound) {
    memcpy(st.key_hash, key, sizeof st.key_hash);
    st.key_bound = true;
    return KeyCheck::Bound;
  }
  uint8_t diff = 0;  // constant time, out of habit
  for (size_t i = 0; i < sizeof st.key_hash; i++) diff |= (uint8_t)(st.key_hash[i] ^ key[i]);
  return diff ? KeyCheck::Mismatch : KeyCheck::Match;
}

uint8_t staleThreshold(bool set_absent, float frozen_wind, float frozen_gust) {
  return set_absent || frozen_wind > 0 || frozen_gust > 0 ? STALE_SENSOR_N : STALE_CALM_N;
}

bool consoleClockOff(int32_t skew_s, bool dst_active) {
  const int32_t a = skew_s < 0 ? -skew_s : skew_s;
  const int32_t h = skew_s - 3600;
  return a > CLOCK_SKEW_MAX_S && !(dst_active && h >= -CLOCK_DST_HOUR_TOL_S && h <= CLOCK_DST_HOUR_TOL_S);
}

bool applyFingerprint(Station& st, Fingerprint fp) {
  st.fp = fp;
  if (st.stored || st.uploads || fp != Fingerprint::Mismatch || st.cfg.profile == Profile::Generic) return false;
  st.cfg.profile = Profile::Generic;
  return true;
}

uint32_t fnv1a32(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

UploadVerdict uploadVerdict(const Station* st, KeyCheck kc, uint32_t rx_ms, uint32_t qhash, bool dated) {
  if (!st) return UploadVerdict::NoSlot;
  if (rateLimited(*st, rx_ms)) return UploadVerdict::RateLimited;
  if (isDuplicate(*st, rx_ms, qhash, dated)) return UploadVerdict::Duplicate;
  if (kc == KeyCheck::Mismatch) return UploadVerdict::KeyMismatch;
  return UploadVerdict::Accept;
}

LoadOrder loadOrder(const char* owner0, uint32_t newest0, const char* owner1, uint32_t newest1) {
  LoadOrder o = {};
  const bool has0 = owner0 && owner0[0], has1 = owner1 && owner1[0];
  const bool oneFirst = has1 && (!has0 || newest1 > newest0);  // a tie keeps ring 0
  const char* a = oneFirst ? owner1 : (has0 ? owner0 : "");
  const char* b = oneFirst ? (has0 ? owner0 : "") : (has1 ? owner1 : "");
  snprintf(o.first, sizeof o.first, "%s", a);
  snprintf(o.second, sizeof o.second, "%s", b);
  return o;
}

int loadPass(const char* id, const LoadOrder& o) {
  if (id && o.first[0] && strcmp(id, o.first) == 0) return 0;
  if (id && o.second[0] && strcmp(id, o.second) == 0) return 1;
  return 2;
}

bool isGhost(const char* id, Stations& s, const ParkedList& parked) { return !s.find(id) && !parked.has(id); }

Station* admitStation(Stations& s, const ParkedList& parked, const char* id, const StationConfig& initial, int limit) {
  if (parked.has(id)) return s.find(id);  // never a slot with defaults for a stored id (null unless it took one at boot)
  return s.findOrAdd(id, initial, limit);
}

PutAdmit stationPutAdmit(Stations& s, const ParkedList& parked, const char* id, const StationConfig& initial, int limit,
                         bool synced, const CalLog* stored, const char* body, size_t len, Errors* scratch, Station** out) {
  *out = nullptr;
  Station* st = s.find(id);
  if (!st && parked.has(id)) return PutAdmit::Parked;
  if (!st && s.active() >= limit) return PutAdmit::NoSlot;
  const StationConfig& base = st ? st->cfg : initial;
  if (!synced) {
    const CalLog* log = st && st->cal.n ? &st->cal : stored && stored->n ? stored : nullptr;
    const CalEntry cur = log ? log->e[log->n - 1] : calOf(base, 0);
    StationConfig c = base;
    stationFromJson(body, len, &c, scratch);
    if (!calSame(calOf(c, 0), cur)) return PutAdmit::CalRefused;
  }
  if (!st) st = admitStation(s, parked, id, initial, limit);
  if (!st) return PutAdmit::NoSlot;
  *out = st;
  return PutAdmit::Ok;
}

void NoSlotNote::note(const char* sid, uint32_t now_ms) {
  if (strncmp(id, sid ? sid : "", sizeof id) != 0) same = false, from[0] = 0;  // another console: no offer carried over
  snprintf(id, sizeof id, "%s", sid ? sid : "");
  ms = now_ms | 1;
}
void NoSlotNote::offer(bool is_same, const char* loaded_id, const uint8_t key[32]) {
  same = is_same && loaded_id && loaded_id[0];
  if (!same) {
    from[0] = 0;
    return;
  }
  snprintf(from, sizeof from, "%s", loaded_id);
  memcpy(key_hash, key, sizeof key_hash);
}
bool NoSlotNote::offers(const char* loaded_id, const char* new_id) const {
  return same && id[0] && loaded_id && new_id && strncmp(from, loaded_id, sizeof from) == 0 && strncmp(id, new_id, sizeof id) == 0;
}
void NoSlotNote::expire(uint32_t now_ms) {
  if (ms && now_ms - ms >= SHOW_MS) clear();
}

const char* uploadDrop(SrcIf via, bool lan_uploads, bool blocked) {
  if (via == SrcIf::Mixed || (via == SrcIf::Sta && !lan_uploads)) return "lan";
  return blocked ? "blocked" : nullptr;
}

static void rainFromHours(Station& st, uint32_t now);  // below Stations::ingest

AdminGate adminGate(SrcIf via, bool csrf, bool needSession, bool sessionOk) {
  if (via != SrcIf::Sta || !csrf) return AdminGate::CatchAll;
  return needSession && !sessionOk ? AdminGate::Unauth : AdminGate::Pass;
}

bool apCatchAllApplies(SrcIf via, bool setup_mode) { return via == SrcIf::Ap && !setup_mode; }

static bool allDigits(const char* p, size_t n, size_t max) {
  if (n < 1 || n > max) return false;
  for (size_t i = 0; i < n; i++)
    if (p[i] < '0' || p[i] > '9') return false;
  return true;
}
static bool sameNoCase(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}
static bool ipv4Literal(const char* p, size_t n) {
  int parts = 0;
  size_t i = 0;
  while (i <= n) {
    size_t j = i;
    while (j < n && p[j] != '.') j++;
    if (!allDigits(p + i, j - i, 3)) return false;
    int v = 0;
    for (size_t k = i; k < j; k++) v = v * 10 + (p[k] - '0');
    if (v > 255) return false;
    parts++;
    i = j + 1;
  }
  return parts == 4;
}

bool hostAllowed(const char* host, const char* hostname) {
  if (!host || !host[0]) return true;
  const size_t len = strlen(host);
  if (host[0] == '[') {  // an IPv6 literal, "[..]" or "[..]:port"
    const char* e = strchr(host, ']');
    if (!e || e == host + 1) return false;
    for (const char* p = host + 1; p < e; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') || *p == ':' || *p == '.'))
        return false;
    return !e[1] || (e[1] == ':' && allDigits(e + 2, strlen(e + 2), 5));
  }
  size_t n = len;
  const char* colon = strchr(host, ':');
  if (colon) {
    if (!allDigits(colon + 1, strlen(colon + 1), 5)) return false;
    n = (size_t)(colon - host);
  }
  if (n && host[n - 1] == '.') n--;
  if (!n) return false;
  if (ipv4Literal(host, n)) return true;
  const size_t h = hostname ? strlen(hostname) : 0;
  if (!h || n < h || !sameNoCase(host, hostname, h)) return false;
  if (n == h) return true;
  // <hostname> plus mDNS's .local or a private-use suffix routers hand out, which public DNS never delegates (a
  // router domain under a public TLD, such as fritz.box, could be pointed at the board from outside: not listed)
  static const char* const SUFFIX[] = {".local", ".lan", ".home", ".internal", ".home.arpa"};
  for (const char* sfx : SUFFIX)
    if (n - h == strlen(sfx) && sameNoCase(host + h, sfx, n - h)) return true;
  return false;
}

void ParkedList::add(const char* id) {
  if (!id[0] || has(id)) return;
  if (n >= MAX) {
    dropped++;
    return;
  }
  snprintf(ids[n++], STATION_ID_LEN, "%s", id);
}
bool ParkedList::has(const char* id) const {
  for (int i = 0; i < n; i++)
    if (strcmp(ids[i], id) == 0) return true;
  return false;
}
ParkedList parkedListing(const ParkedList& parked, const ParkedList& leftover, Stations& s) {
  ParkedList out = parkedWithoutSlot(parked, s);
  for (int i = 0; i < leftover.n; i++)
    if (!s.find(leftover.ids[i])) out.add(leftover.ids[i]);  // add() skips one already listed
  return out;
}
ParkedList parkedWithoutSlot(const ParkedList& p, Stations& s) {
  ParkedList out = {};
  out.dropped = p.dropped;
  for (int i = 0; i < p.n; i++)
    if (!s.find(p.ids[i])) out.add(p.ids[i]);
  return out;
}
bool ParkedList::remove(const char* id) {
  for (int i = 0; i < n; i++)
    if (strcmp(ids[i], id) == 0) {
      memmove(ids[i], ids[i + 1], (size_t)(n - i - 1) * STATION_ID_LEN);
      n--;
      return true;
    }
  return false;
}

Station* Stations::find(const char* id) {
  for (Station* st : s_)
    if (st && st->used && strcmp(st->id, id) == 0) return st;
  return nullptr;
}

int Stations::reserve(int n) {
  int k = 0;
  for (Station*& slot : s_) {
    if (k >= n) break;
    if (!slot) {
      // calloc: all-zero is the valid empty state of every member (used == false). Never freed.
      slot = (Station*)calloc(1, sizeof(Station));
      if (!slot) break;
    }
    k++;
  }
  return k;
}

Station* Stations::findOrAdd(const char* id, const StationConfig& initial, int limit) {
  if (Station* st = find(id)) return st;
  if (active() >= limit) return nullptr;
  for (Station* st : s_) {  // a reserved slot (never a new allocation)
    if (!st || st->used) continue;
    memset((void*)st, 0, sizeof *st);
    st->used = true;
    st->gen = ++gen_;
    strncpy(st->id, id, sizeof st->id - 1);
    st->cfg = initial;
    return st;
  }
  return nullptr;
}

Station* Stations::first() {
  for (Station* st : s_)
    if (st && st->used) return st;
  return nullptr;
}

int Stations::active() const {
  int n = 0;
  for (const Station* st : s_) n += st && st->used;
  return n;
}

bool Stations::forget(int slot) {
  if (slot < 0 || slot >= MAX_STATIONS || !s_[slot] || !s_[slot]->used) return false;
  memset((void*)s_[slot], 0, sizeof *s_[slot]);  // used = false: the allocation stays for the next station
  return true;
}

int Stations::indexOf(const Station* st) const {
  for (int i = 0; i < MAX_STATIONS; i++)
    if (s_[i] == st) return i;
  return -1;
}

int Stations::allocated() const {
  int n = 0;
  for (const Station* st : s_) n += st != nullptr;
  return n;
}

static void feedDay(Station& st, const PosixTz& tz, bool* day_closed) {
  if (st.hourly.size() == 0) return;
  st.hour_closes++;
  if (st.hourly.lastCloseReplaced()) return;  // the day folds the hours again when it closes: nothing to add
  if (st.daily.addHour(st.hourly.at(st.hourly.size() - 1), st.hourly, tz)) *day_closed = true;
}

static_assert(HIST_KEYS == HIST_CH && HIST_KEY_LEN == (int)CH_KEY_LEN, "the config's hist keys are the channel keys");

// quality inputs the app sets (one bridge, one clock)
static uint32_t gBootUnix = 0;
static bool gClockApprox = false;
void Stations::setBootTime(uint32_t unix_s) { gBootUnix = unix_s; }
void Stations::setClockApprox(bool approx) { gClockApprox = approx; }

// the channel candidates in their order, with the scale the records use (10^decimals)
static int channelOf(const Station& st, const char* key) {
  for (int c = 0; c < HIST_CH; c++)
    if (st.chmap.key[c][0] && strcmp(st.chmap.key[c], key) == 0) return c;
  return -1;
}
static bool addChannel(Station& st, const char* key, float scale) {
  if (channelOf(st, key) >= 0) return false;
  for (int c = 0; c < HIST_CH; c++)
    if (!st.chmap.key[c][0]) {
      snprintf(st.chmap.key[c], sizeof st.chmap.key[c], "%s", key);
      st.chmap.scale[c] = scale;
      return true;
    }
  return false;  // all four taken: the user picks with the `hist` key
}
// the scale of a channel key: a field's FIELDS scale, a promotion's 10^decimals; 0 = not a candidate (yet)
static float channelScale(const Station& st, const char* key) {
  const int f = fieldByKey(key);
  if (f >= 0) return FIELDS[f].scale;
  for (const Promote& p : st.cfg.promote)
    if (p.used && strcmp(p.key, key) == 0) {
      float scale = 1;
      for (int d = 0; d < promoType(p.type).dec && d < 3; d++) scale *= 10;
      return scale;
    }
  return 0;
}

bool Stations::updateChannels(Station& st) {
  if (st.cfg.hist[0][0]) {  // chosen by hand (`hist`): exactly those keys, in that order
    ChMap want = st.chmap;
    memset(want.key, 0, sizeof want.key);
    for (int c = 0; c < HIST_CH && c < HIST_KEYS && st.cfg.hist[c][0]; c++) {
      snprintf(want.key[c], sizeof want.key[c], "%s", st.cfg.hist[c]);
      want.scale[c] = channelScale(st, st.cfg.hist[c]);
      if (want.scale[c] <= 0) want.scale[c] = 1;
    }
    if (memcmp(want.key, st.chmap.key, sizeof want.key) == 0) return false;
    st.chmap = want;
    st.chmap.ver = (uint8_t)(st.chmap.ver % 255 + 1);
    st.ch_dirty = true;
    return true;
  }
  bool changed = false;
  for (int f : {(int)F_INDOOR_TEMP_C, (int)F_INDOOR_HUMIDITY})
    if ((st.seen_fields >> f) & 1u) changed = addChannel(st, FIELDS[f].key, FIELDS[f].scale) || changed;
  for (int i = 0; i < MAX_PROMOTE; i++) {
    const Promote& p = st.cfg.promote[i];
    if (!p.used || !p.key[0] || !((st.seen_promoted >> i) & 1u)) continue;
    float scale = 1;
    for (int d = 0; d < promoType(p.type).dec && d < 3; d++) scale *= 10;
    changed = addChannel(st, p.key, scale) || changed;
  }
  if (changed) {
    st.chmap.ver = (uint8_t)(st.chmap.ver % 255 + 1);  // 1..255; 0 = no map yet
    st.ch_dirty = true;
  }
  return changed;
}

// one accepted reading as the hour aggregator takes it (raw values: calibration applies on read; quality flags;
// channels)
static void hourInput(const Station& st, const Filtered& fo, uint32_t now, uint16_t rej_count, const PosixTz& tz,
                      HourIn* in) {
  memset(in, 0, sizeof *in);
  in->t = now;
  in->present = fo.s.present & ((1u << CORE_FIELDS) - 1);
  for (int f = 0; f < CORE_FIELDS; f++) in->v[f] = fo.raw_v[f];
  const bool w2m = ((st.live.present & st.fresh) >> F_WIND_AVG2M_KMH) & 1u;  // sent or derived with this reading
  in->wind2m = w2m ? st.live.v[F_WIND_AVG2M_KMH] : NAN;
  in->wind_cal = fo.s.has(F_WIND_KMH) ? fo.s.v[F_WIND_KMH] : NAN;
  // rain raw too (one rule for every factor): the filter's deltas and rate are factored, so the factor is
  // taken out again; the calibration log puts it back on read
  const float rf = st.cfg.rain_factor > 0 ? st.cfg.rain_factor : 1.0f;
  in->rain_delta = fo.rain_delta_valid ? fo.rain_delta_mm / rf : NAN;
  in->rain_counter = fo.s.has(F_RAIN_DAILY_MM);
  in->rain_rate = fo.s.has(F_RAIN_RATE_MMH) ? fo.s.v[F_RAIN_RATE_MMH] / rf : NAN;
  uint16_t q = 0;
  if (st.sensor_stale) q |= 1u << Q_STALE;
  if (st.baro_uncal) q |= 1u << Q_BARO_UNCAL;
  if (gClockApprox) q |= 1u << Q_CLOCK_APPROX;
  if (gBootUnix && gBootUnix >= now - now % 3600) q |= 1u << Q_RESTART;
  if (rej_count) q |= 1u << Q_REJECTED;
  if (fo.rain_reset) {
    const int64_t local = (int64_t)now + localOffset(tz, now);
    const uint32_t sod = (uint32_t)(((local % 86400) + 86400) % 86400);
    if (sod > RESET_MIDNIGHT_TOL_S && sod < 86400 - RESET_MIDNIGHT_TOL_S) q |= 1u << Q_CONSOLE_RESET;
  }
  if (st.sensor_absent) q |= 1u << Q_CONSOLE_RESET;
  in->flags = q;
  in->ch_ver = st.chmap.ver;
  for (int c = 0; c < HIST_CH; c++) {
    in->ch[c] = NAN;
    in->ch_scale[c] = st.chmap.scale[c];
    const char* k = st.chmap.key[c];
    if (!k[0]) continue;
    const int f = fieldByKey(k);
    if (f >= 0) {
      if (fo.s.has(f)) in->ch[c] = fo.s.v[f];
      continue;
    }
    for (int i = 0; i < MAX_PROMOTE; i++)
      if (st.cfg.promote[i].used && strcmp(st.cfg.promote[i].key, k) == 0 && ((fo.promoted_mask >> i) & 1u))
        in->ch[c] = fo.promoted[i];
  }
}

// a known key is updated in place; a new one takes an empty slot, else the oldest-seen key that is not promoted
static bool promotedKey(const Station& st, const char* key) {
  for (int i = 0; i < MAX_PROMOTE; i++)
    if (st.cfg.promote[i].used && strcmp(st.cfg.promote[i].param, key) == 0) return true;
  return false;
}
static void rememberKey(Station& st, const Extra& e, uint32_t now) {
  if (!e.key[0]) return;
  KnownKey* slot = nullptr;
  for (KnownKey& k : st.known)
    if (k.t && strcmp(k.key, e.key) == 0) {
      slot = &k;
      break;
    }
  for (KnownKey& k : st.known)
    if (!slot && !k.t) slot = &k;
  if (!slot) {
    for (KnownKey& k : st.known)
      if (!promotedKey(st, k.key) && (!slot || k.t < slot->t)) slot = &k;
  }
  if (!slot) return;  // every slot a promoted key: cannot happen with MAX_PROMOTE 4 < MAX_KNOWN
  snprintf(slot->key, sizeof slot->key, "%s", e.key);
  snprintf(slot->val, sizeof slot->val, "%s", e.val);
  slot->t = now ? now : 1;
}

IngestResult Stations::ingest(Station& st, const Sample& s, uint32_t now, uint32_t now_ms, uint32_t src_ip,
                              SrcIf src_if, bool clock_ok, const PosixTz& tz) {
  IngestResult res = {};
  clock_ok = clock_ok && now >= EPOCH_MIN;  // an uptime-based now never opens a 1970 hour
  static Filtered fo;  // ~0.3 KB, kept off the caller's stack; callers are serialized
  // a rate profile's `rainin` is a rain rate in/h -> rain_rate_mmh (the filter's rain factor applies)
  const StationProfile& prof = profileRules(st.cfg.profile);
  static Sample moved;
  const Sample* in = &s;
  if (prof.rainin_is_rate && (s.has(F_RAIN_MM) || ((s.rejected() >> F_RAIN_MM) & 1u))) {
    moved = s;
    const uint32_t from = 1u << F_RAIN_MM, to = 1u << F_RAIN_RATE_MMH;
    auto mv = [&](uint32_t& m) { m = (m & ~(from | to)) | ((m & from) ? to : 0); };
    moved.v[F_RAIN_RATE_MMH] = s.v[F_RAIN_MM];
    mv(moved.present);
    mv(moved.rejected_parse);
    mv(moved.rejected_bounds);
    in = &moved;
  }
  runFilters(*in, st.cfg, now, &st.fstate, &fo);

  st.last_seen = now;
  st.last_seen_ms = now_ms;
  st.src_ip = src_ip;
  st.src_if = src_if;
  st.uploads++;
  const uint32_t rej = fo.rejectedFields();
  for (int f = 0; f < F_COUNT; f++)
    if ((rej >> f) & 1u) st.rejects[f]++;
  for (int r = 0; r < R_COUNT; r++)
    if (fo.fired(r)) st.rej_rule[r]++;
  st.live_rejected = rej;
  res.rain_glitch = fo.rain_glitch;
  const uint16_t rejCount = (uint16_t)popcount32(rej);

  if (fo.sample_dropped) {
    st.dropped++;
    res.dropped = true;
    if (clock_ok && st.hourly.addRejected(now)) {
      res.hour_closed = true;
      feedDay(st, tz, &res.day_closed);
    }
    return res;  // live, recent and the published values stay at the last accepted sample
  }

  // the stale-sensor rule, on the set as received (before calibration)
  const uint32_t smask = prof.sensor_mask & ((1u << CORE_FIELDS) - 1);
  const uint32_t spres = in->present & smask;
  bool same;
  if (!spres) {
    same = (st.seen_fields & smask) != 0;  // absent counts as unchanged once the station had it
  } else {
    same = spres == st.last_sensor_present;
    for (int f = 0; same && f < CORE_FIELDS; f++)
      if ((spres >> f) & 1u) same = memcmp(&in->v[f], &st.last_sensor[f], sizeof(float)) == 0;
  }
  if (same && st.sensor_init) {
    if (st.same_n < 255) st.same_n++;
    if (st.stale_run < 65535) st.stale_run++;
    if (st.stale_run > st.stale_longest_run) st.stale_longest_run = st.stale_run;
  } else {
    st.stale_run = 0;
    st.same_n = 0;
    st.sensor_stale = false;
    st.stale_since = now;
  }
  st.sensor_init = true;
  if (spres) {
    st.last_sensor_present = spres;
    for (int f = 0; f < CORE_FIELDS; f++)
      if ((spres >> f) & 1u) st.last_sensor[f] = in->v[f];
  }
  const bool windKnown = (st.last_sensor_present >> F_WIND_KMH) & 1u, gustKnown = (st.last_sensor_present >> F_GUST_KMH) & 1u;
  const uint8_t need = staleThreshold(!spres, windKnown ? st.last_sensor[F_WIND_KMH] : 0, gustKnown ? st.last_sensor[F_GUST_KMH] : 0);
  if (st.same_n >= need && !st.sensor_stale) {
    st.sensor_stale = true;
    if (st.stale_on_count < 65535) st.stale_on_count++;
  }
  st.sensor_absent = st.sensor_stale && !spres;
  // while stale the set is not news: not merged (the carry rule keeps the values with their age), not published,
  // not written to recent / hourly / daily; pressure, rain and the rest go on
  if (st.sensor_stale) fo.s.present &= ~smask;
  // the console's uncalibrated pressure, on the raw value (before pressure_hpa_offset)
  if (!std::isnan(prof.baro_factory_hpa) && in->has(F_PRESSURE_HPA)) {
    if (std::fabs(in->v[F_PRESSURE_HPA] - prof.baro_factory_hpa) <= BARO_FACTORY_TOL_HPA) {
      if (st.baro_n < 255) st.baro_n++;
    } else {
      st.baro_n = 0;
    }
  }
  if (std::isnan(prof.baro_factory_hpa)) st.baro_n = 0;
  st.baro_uncal = st.baro_n >= BARO_UNCAL_N;
  if (st.baro_uncal) relReset(&st.rel);  // one notice, never two (baro_uncalibrated outranks baro_ref)
  if (!st.baro_n && fo.s.has(F_PRESSURE_HPA) && now >= EPOCH_MIN) st.good_p = fo.s.v[F_PRESSURE_HPA], st.good_t = now;
  if (st.baro_uncal && !st.good_t) {  // after a reboot, from the hourly records, once (not per second)
    float gp;
    uint32_t gt;
    if (lastGoodFromHours(st, &gp, &gt)) st.good_p = gp, st.good_t = gt;
  }
  // the console's clock, judged only against a real clock of ours
  if (s.has_dateutc && clock_ok) {
    const int64_t d = s.dateutc - (int64_t)now;
    st.clock_skew_s = d > INT32_MAX ? INT32_MAX : d < INT32_MIN ? INT32_MIN : (int32_t)d;
    st.clock_off = consoleClockOff(st.clock_skew_s, tz.has_dst && localOffset(tz, now) != tz.std_off);
  } else if (!clock_ok) {
    st.clock_off = false;
  }

  // merge field by field; a field this upload lacks keeps its value and its time
  const uint32_t carried = st.live.present & ~fo.s.present;
  float keep[F_COUNT];
  for (int f = 0; f < F_COUNT; f++)
    if ((carried >> f) & 1u) keep[f] = st.live.v[f];
  st.live = fo.s;
  for (int f = 0; f < F_COUNT; f++) {
    if ((carried >> f) & 1u) st.live.v[f] = keep[f];
    else if (fo.s.has(f)) st.field_t[f] = now;
  }
  st.live.present |= carried;
  st.fresh = fo.s.present;
  for (int i = 0; i < MAX_PROMOTE; i++)
    if ((fo.promoted_mask >> i) & 1u) {
      st.promoted[i] = fo.promoted[i];
      st.promo_t[i] = now;
    }
  st.promoted_mask |= fo.promoted_mask;
  st.promo_fresh = fo.promoted_mask;
  for (int k = 0; k < fo.s.n_extras; k++) rememberKey(st, fo.s.extras[k], now);
  st.seen_fields |= fo.s.present;
  st.seen_promoted |= fo.promoted_mask;
  if (prof.rainin_is_rate) {
    // the ring's rain_mm slot holds this upload's rain (the reset-aware daily-counter delta, 0.01 mm), so the last
    // hour is a window sum; the live rain_mm is that sum once warm, else absent (HA: unavailable, never 0)
    static Sample ring;
    ring = fo.s;
    ring.present &= ~(1u << F_RAIN_MM);
    if (fo.rain_delta_valid) {
      ring.v[F_RAIN_MM] = fo.rain_delta_mm;
      ring.present |= 1u << F_RAIN_MM;
      if (!st.rain_started) {
        st.rain_started = true;
        st.rain_first_ms = now_ms;
      }
    }
    st.recent.push(ring, now);
    if (s.has(F_RAIN_DAILY_MM)) st.seen_fields |= 1u << F_RAIN_MM;  // one constant entity set
    const WindowSum w = recentSum(st.recent, F_RAIN_MM, now, RAIN_HOUR_S);
    // the 60-reading ring "spans the hour" with a margin (55 min), so a burst at the 7-in-1's 65 s
    // cadence does not flicker the derived hour to unavailable
    st.rain_warm = st.rain_started && now_ms - st.rain_first_ms >= RAIN_HOUR_S * 1000u &&
                   recentSpans(st.recent, now, RAIN_SPAN_MIN_S);
    st.rain_n = (uint16_t)w.n;
    const uint32_t bit = 1u << F_RAIN_MM;
    st.derived = st.rain_warm ? st.derived | bit : st.derived & ~bit;
    st.warming = st.rain_warm ? st.warming & ~bit : st.warming | bit;
    st.derived_n[F_RAIN_MM] = (uint8_t)(w.n > 255 ? 255 : w.n);
    if (st.rain_warm) {
      st.live.v[F_RAIN_MM] = w.sum;
      st.live.present |= 1u << F_RAIN_MM;
      st.field_t[F_RAIN_MM] = now;
      st.fresh |= 1u << F_RAIN_MM;
    } else {
      st.live.present &= ~(1u << F_RAIN_MM);
      st.fresh &= ~(1u << F_RAIN_MM);
    }
  } else {
    st.recent.push(fo.s, now);
    st.derived &= ~(1u << F_RAIN_MM);  // a reported last hour is not derived, nor warming
    st.warming &= ~(1u << F_RAIN_MM);
  }
  // the WU wind fields the console does not send, over the ring by time (this upload included)
  for (const DerivedDef& d : WIND_DERIVED) {
    const uint32_t bit = 1u << d.field;
    if (((in->present | in->rejected()) >> d.field) & 1u) {  // reported: the console's value wins
      st.derived &= ~bit;
      continue;
    }
    const WindowValue w = d.kind == WinKind::Mean         ? recentMean(st.recent, d.source, now, d.window_s)
                          : d.kind == WinKind::VectorMean ? recentVectorMean(st.recent, d.source, now, d.window_s)
                                                          : recentMax(st.recent, d.source, now, d.window_s);
    if (!w.n) {
      st.derived &= ~bit;  // nothing in the window (sensor stale, wind not sent): the last value stays, aged
      continue;
    }
    st.live.v[d.field] = w.v;
    st.live.present |= bit;
    st.field_t[d.field] = now;
    st.fresh |= bit;
    st.seen_fields |= bit;
    st.derived |= bit;
    st.derived_n[d.field] = (uint8_t)(w.n > 255 ? 255 : w.n);
  }
  // the 10-min mean wind, once the station has sent wind; an empty window keeps the last value (aged)
  st.wind10_fresh = false;
  if ((st.seen_fields >> F_WIND_KMH) & 1u) {
    int n = 0;
    const float m = recentFieldMean(st.recent, F_WIND_KMH, now, GUST_MAX_S, &n);
    if (n) st.wind10 = m, st.wind10_n = (uint8_t)(n > 255 ? 255 : n), st.wind10_t = now, st.wind10_have = st.wind10_fresh = true;
  }
  // a channel map change (a new candidate, or `hist`) applies from the next hour boundary: one map per hour
  if (!st.hourly.acc().hour || !clock_ok || st.hourly.acc().hour != now - now % 3600) updateChannels(st);
  if (clock_ok) {
    static HourIn hin;  // ~110 B, off the caller's stack; callers are serialized
    hourInput(st, fo, now, rejCount, tz, &hin);
    if (st.hourly.add(hin)) {
      res.hour_closed = true;
      feedDay(st, tz, &res.day_closed);
    }
  }
  st.rain_hours = false;
  if (prof.rainin_is_rate && clock_ok && !st.rain_warm) rainFromHours(st, now);
  return res;
}

// until the ring is warm, "Rain, last hour" = the open hour (this upload included) plus
// the part of the previous closed hour still inside the last 60 min, from the hour records (calibrated like the
// rows). A weak previous hour (few readings, a gap, a partial or reset rain counter) is left out: the open hour alone.
// No previous hour record, an open hour stamped by an approximate clock or without rain: no value (warming).
static void rainFromHours(Station& st, uint32_t now) {
  HourRec open;
  if (!st.hourly.openRecord(&open) || open.hour != now - now % 3600 || !hasU16(open.rain_sum) ||
      (open.quality & (1u << Q_CLOCK_APPROX)) || !st.hourly.size())
    return;
  HourRec prev = st.hourly.at(st.hourly.size() - 1);
  if (prev.hour != open.hour - 3600 || !hasU16(prev.rain_sum)) return;
  calHour(&open, calFor(st.cal, open.hour));
  calHour(&prev, calFor(st.cal, prev.hour));
  constexpr uint16_t WEAK = (1u << Q_GAP) | (1u << Q_RAIN_PARTIAL) | (1u << Q_CONSOLE_RESET);
  const bool usePrev = prev.n >= PROFILE_HOUR_N_MIN && !(prev.quality & WEAK);
  const uint32_t into = now - open.hour;
  const float mm = open.rain_sum / 100.0f + (usePrev ? prev.rain_sum / 100.0f * (float)(3600 - into) / 3600.0f : 0.0f);
  const uint32_t bit = 1u << F_RAIN_MM, n = (uint32_t)open.n + (usePrev ? prev.n : 0);
  st.rain_hours = true;
  st.live.v[F_RAIN_MM] = mm;
  st.live.present |= bit;
  st.field_t[F_RAIN_MM] = now;
  st.fresh |= bit;
  st.derived |= bit;
  st.warming &= ~bit;
  st.derived_n[F_RAIN_MM] = (uint8_t)(n > 255 ? 255 : n);
  st.rain_n = (uint16_t)n;
}

void Stations::profileChanged(Station& st, uint32_t now) {
  st.profile_t = now ? now : 1;  // the profile_changed notice
  st.rain_started = st.rain_warm = st.rain_hours = false;
  st.rain_first_ms = 0;
  st.rain_n = 0;
  const uint32_t bit = 1u << F_RAIN_MM;
  st.derived &= ~bit;
  st.warming &= ~bit;
  st.derived_n[F_RAIN_MM] = 0;
  st.recent.clearField(F_RAIN_MM);
  st.live.present &= ~(1u << F_RAIN_RATE_MMH);  // a rate under the old meaning; the next upload brings its own
  st.baro_n = 0;  // the factory pressure is the profile's
  st.baro_uncal = false;
  relReset(&st.rel);  // another console behind the id
}

void Stations::repromote(Station& st) {
  st.promoted_mask = 0;
  for (int i = 0; i < MAX_PROMOTE; i++) {
    const Promote& p = st.cfg.promote[i];
    if (!p.used) continue;
    for (const KnownKey& k : st.known) {
      double x;
      if (!k.t || strcmp(k.key, p.param) != 0 || !parseNumber(k.val, &x) || !promotedOk(promoteConvert(p, x))) continue;
      st.promoted[i] = (float)promoteConvert(p, x);
      st.promo_t[i] = k.t;
      st.promoted_mask |= (uint8_t)(1u << i);
      break;
    }
  }
  st.promo_fresh &= st.promoted_mask;
}

bool Stations::forgetKey(Station& st, const char* key) {
  for (KnownKey& k : st.known)
    if (k.t && strcmp(k.key, key) == 0) {
      memset(&k, 0, sizeof k);
      return true;
    }
  return false;
}

int Stations::clockSynced(Station& st, uint32_t offset) {
  if (st.last_seen && st.last_seen < EPOCH_MIN) st.last_seen += offset;
  for (uint32_t& t : st.field_t)
    if (t && t < EPOCH_MIN) t += offset;
  for (uint32_t& t : st.promo_t)
    if (t && t < EPOCH_MIN) t += offset;
  for (KnownKey& k : st.known)
    if (k.t && k.t < EPOCH_MIN) k.t += offset;
  for (int f = 0; f < CORE_FIELDS; f++)
    if (((st.fstate.have >> f) & 1u) && st.fstate.last_t[f] < EPOCH_MIN) st.fstate.last_t[f] += offset;
  if (st.stale_since && st.stale_since < EPOCH_MIN) st.stale_since += offset;
  if (st.profile_t && st.profile_t < EPOCH_MIN) st.profile_t += offset;
  return st.recent.restamp(offset);
}

TickResult Stations::tick(Station& st, uint32_t now, const PosixTz& tz) {
  TickResult res = {};
  if (now < EPOCH_MIN) return res;  // history runs on the real clock only
  if (st.hourly.tick(now)) {
    res.hour_closed = true;
    feedDay(st, tz, &res.day_closed);
  }
  if (st.daily.tick(now, st.hourly, tz)) res.day_closed = true;
  return res;
}

bool Stations::closeHourNow(Station& st, const PosixTz& tz) {
  if (!st.hourly.closeNow()) return false;
  bool dayClosed = false;
  feedDay(st, tz, &dayClosed);
  return true;
}

bool Stations::closeDayNow(Station& st, const PosixTz& tz) {
  closeHourNow(st, tz);
  return st.daily.closeNow(st.hourly, tz);
}

}  // namespace fc
