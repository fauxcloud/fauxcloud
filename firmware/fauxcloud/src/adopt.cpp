// adopt.cpp -- see adopt.h
#include "adopt.h"

#include <cstdio>
#include <cstring>

namespace fc {

const char* const ADOPT_MOVE[ADOPT_PREFIXES] = {"st", "ch", "cl", "lr", "lm"};

uint32_t adoptMacHash(const uint8_t mac[6]) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
  return h ? h : 1;
}

bool adoptCandidate(const AdoptIn& in) {
  if (in.blocked || in.parked || in.max_stations != 1 || !in.loaded || !in.loaded_stored) return false;
  if (!in.up_has_mac || !in.up_mac) return false;  // no MAC on the upload: never an offer (the plain "no slot" note)
  if (in.st_has_mac && in.st_mac) return memcmp(in.up_mac, in.st_mac, 6) == 0;
  return in.persisted_mac_hash && adoptMacHash(in.up_mac) == in.persisted_mac_hash;
}

static bool idOk(const char* id) {
  const void* end = memchr(id, 0, STATION_ID_LEN);
  return end && id[0];
}
bool adoptJournalValid(const AdoptJournal& j) { return idOk(j.from) && idOk(j.to) && strcmp(j.from, j.to) != 0; }

bool adoptJournalDecode(const uint8_t* b, size_t n, AdoptJournal* j) {
  memset(j, 0, sizeof *j);
  if (!b || (n != ADOPT_JOURNAL_V1_SIZE && n != sizeof *j)) return false;
  memcpy(j, b, n);  // version 1: the first 64 B, ver and boots zero
  if (n == ADOPT_JOURNAL_V1_SIZE) j->ver = ADOPT_JOURNAL_VER, j->boots = 0;  // the migration
  if (j->ver != ADOPT_JOURNAL_VER || !adoptJournalValid(*j)) {
    memset(j, 0, sizeof *j);
    return false;
  }
  return true;
}

int adoptGiveUpText(char* out, size_t cap, const char* from, const char* to, bool keys_from, bool keys_to, bool rings_from,
                    bool rings_to, uint32_t targets) {
  if (!out || !cap) return 0;
  char where[2][32];  // "settings on <15 chars>"
  const auto on = [](char* w, size_t n, const char* what, bool a, bool b, const char* ia, const char* ib) {
    if (a && b) snprintf(w, n, "%s on both", what);  // both ids: short, so the widest line fits the 200-B log line
    else if (a || b) snprintf(w, n, "%s on %s", what, a ? ia : ib);
    else snprintf(w, n, "no %s", what);
  };
  on(where[0], sizeof where[0], "settings", keys_from, keys_to, from, to);
  on(where[1], sizeof where[1], "rings", rings_from, rings_to, from, to);
  char slots[48] = "";
  size_t k = 0;
  for (int i = 0; i < 32 && k + 4 < sizeof slots; i++)
    if (targets >> i & 1u) k += (size_t)snprintf(slots + k, sizeof slots - k, " %d", i + 1);
  int n = targets ? snprintf(out, cap, "given up after %u boots: %s, %s, target slot(s)%s on %s", (unsigned)ADOPT_MAX_BOOTS,
                             where[0], where[1], slots, from)
                  : snprintf(out, cap, "given up after %u boots: %s, %s, no target on %s", (unsigned)ADOPT_MAX_BOOTS, where[0],
                             where[1], from);
  return n < 0 ? 0 : (size_t)n < cap ? n : (int)cap - 1;
}

AdoptBoot adoptJournalBoot(AdoptKv& kv, AdoptJournal* j) {
  uint8_t raw[sizeof(AdoptJournal)];
  const long n = kv.type(ADOPT_JOURNAL_KEY) == AdoptKv::Type::Blob ? kv.get(ADOPT_JOURNAL_KEY, raw, sizeof raw) : -1;
  const bool ok = n > 0 && adoptJournalDecode(raw, (size_t)n, j);
  memset(raw, 0, sizeof raw);  // the key hash
  if (!ok) return AdoptBoot::None;
  if (j->boots >= ADOPT_MAX_BOOTS) {
    kv.del(ADOPT_JOURNAL_KEY);
    return AdoptBoot::GiveUp;
  }
  j->boots++;
  j->ver = ADOPT_JOURNAL_VER;
  j->pad[0] = j->pad[1] = 0;
  return kv.put(ADOPT_JOURNAL_KEY, AdoptKv::Type::Blob, (const uint8_t*)j, sizeof *j) ? AdoptBoot::Run : AdoptBoot::Skip;
}

void adoptKey(const char pp[2], const char* id, char out[16]) {
  if (strlen(id) <= 12) {
    snprintf(out, 16, "%c%c:%.12s", pp[0], pp[1], id);
    return;
  }
  uint32_t h = 2166136261u;
  for (const char* p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  snprintf(out, 16, "%c%c#%08lx", pp[0], pp[1], (unsigned long)h);
}

// the config text "<id>\n<json>": the first line becomes the new id; anything else is copied as it is
static long renameConfig(uint8_t* buf, long n, size_t cap, const char* to) {
  const uint8_t* nl = (const uint8_t*)memchr(buf, '\n', (size_t)n);
  if (!nl) return n;
  const size_t head = (size_t)(nl - buf), tl = strlen(to);
  const long out = (long)(tl + ((size_t)n - head));
  if ((size_t)out > cap) return -1;
  memmove(buf + tl, buf + head, (size_t)n - head);
  memcpy(buf, to, tl);
  return out;
}

static const char* const ALL[] = {"st", "sk", "ch", "cl", "lr", "lm"};  // every per-station prefix (the bound key too)

bool adoptKeysExist(AdoptKv& kv, const char* id) {
  char k[16];
  for (const char* pp : ALL) {
    adoptKey(pp, id, k);
    if (kv.type(k) != AdoptKv::Type::None) return true;
  }
  return false;
}

AdoptBlock adoptPrecheck(AdoptKv& kv, const char* from, const char* to) {
  char ko[16], kn[16];
  for (const char* pp : ALL) {
    adoptKey(pp, from, ko);
    adoptKey(pp, to, kn);
    if (strcmp(ko, kn) == 0) return AdoptBlock::KeyCollision;
    if (kv.type(kn) != AdoptKv::Type::None) return AdoptBlock::ToExists;
  }
  return AdoptBlock::None;
}

uint32_t adoptNeedEntries(AdoptKv& kv, const char* from) {
  uint32_t need = 0;
  char k[16];
  for (int i = 0; i < ADOPT_PREFIXES; i++) {
    adoptKey(ADOPT_MOVE[i], from, k);
    const long n = kv.size(k);
    if (n >= 0) need += (uint32_t)((n + 1 + 31) / 32) + 2;
  }
  need += 3 + ADOPT_MARGIN_ENTRIES;  // the new "sk" blob (32 B) and the margin
  return need < ADOPT_MIN_FREE_ENTRIES ? ADOPT_MIN_FREE_ENTRIES : need;
}

bool adoptNvs(AdoptKv& kv, const AdoptJournal& j, uint8_t* buf, size_t cap) {
  if (!adoptJournalValid(j)) return false;
  char ko[16], kn[16];
  for (int i = 0; i < ADOPT_PREFIXES; i++) {  // copy where the old key exists and the new one does not
    adoptKey(ADOPT_MOVE[i], j.from, ko);
    adoptKey(ADOPT_MOVE[i], j.to, kn);
    const AdoptKv::Type t = kv.type(ko);
    if (t == AdoptKv::Type::None || kv.type(kn) != AdoptKv::Type::None) continue;
    long n = kv.get(ko, buf, cap);
    if (n < 0) return false;
    if (i == 0 && t == AdoptKv::Type::Str) n = renameConfig(buf, n, cap, j.to);
    if (n < 0 || !kv.put(kn, t, buf, (size_t)n)) return false;
  }
  static const uint8_t zero[32] = {};
  adoptKey("sk", j.to, kn);
  if (memcmp(j.key, zero, sizeof zero) != 0 && kv.type(kn) == AdoptKv::Type::None &&
      !kv.put(kn, AdoptKv::Type::Blob, j.key, sizeof j.key))
    return false;
  for (int i = 0; i < ADOPT_PREFIXES; i++) {  // the copies are all in place: the old keys go
    adoptKey(ADOPT_MOVE[i], j.from, ko);
    if (kv.type(ko) != AdoptKv::Type::None) kv.del(ko);
  }
  adoptKey("sk", j.from, ko);
  if (kv.type(ko) != AdoptKv::Type::None) kv.del(ko);
  return true;
}

}  // namespace fc
