// test_adopt.inc -- "same console, new ID". The offer's test, the
// turned-away note, the NVS move (byte-equal under the new keys, the config's id line rewritten, the old keys gone, the
// key rebound), its replay after a cut at every step, the ring headers and the snapshot entry renamed. Ids OLD1 / NEW1.
#include "adopt.h"

namespace {
// A fake NVS: typed values, and an optional failure after `budget` puts (a power cut between two writes)
struct MapKv : AdoptKv {
  std::map<std::string, std::pair<Type, std::string>> m;
  int budget = -1;  // puts left before a failure (-1 = never fails)
  Type type(const char* k) override {
    auto it = m.find(k);
    return it == m.end() ? Type::None : it->second.first;
  }
  long size(const char* k) override {
    auto it = m.find(k);
    return it == m.end() ? -1 : (long)it->second.second.size();
  }
  long get(const char* k, uint8_t* buf, size_t cap) override {
    auto it = m.find(k);
    if (it == m.end() || it->second.second.size() > cap) return -1;
    memcpy(buf, it->second.second.data(), it->second.second.size());
    return (long)it->second.second.size();
  }
  bool put(const char* k, Type t, const uint8_t* d, size_t n) override {
    if (budget == 0) return false;
    if (budget > 0) budget--;
    m[k] = {t, std::string((const char*)d, n)};
    return true;
  }
  void del(const char* k) override { m.erase(k); }
};
AdoptJournal journal(const char* from, const char* to, uint8_t keyByte) {
  AdoptJournal j = {};
  snprintf(j.from, sizeof j.from, "%s", from);
  snprintf(j.to, sizeof j.to, "%s", to);
  memset(j.key, keyByte, sizeof j.key);
  return j;
}
// OLD1's entries as the device writes them (the config text starts with the id line)
void seedOld(MapKv& kv, const char* id) {
  char k[16];
  const std::string cfg = std::string(id) + "\n{\"name\":\"Garden\",\"publish\":true}";
  adoptKey("st", id, k), kv.m[k] = {AdoptKv::Type::Str, cfg};
  adoptKey("sk", id, k), kv.m[k] = {AdoptKv::Type::Blob, std::string(32, '\x11')};
  adoptKey("ch", id, k), kv.m[k] = {AdoptKv::Type::Blob, std::string(40, '\x22')};
  adoptKey("cl", id, k), kv.m[k] = {AdoptKv::Type::Blob, std::string(32, '\x33')};
  adoptKey("lr", id, k), kv.m[k] = {AdoptKv::Type::Blob, std::string(300, '\x44')};
  adoptKey("lm", id, k), kv.m[k] = {AdoptKv::Type::Blob, std::string(961, '\x55')};
  kv.m["fw0"] = {AdoptKv::Type::Str, "{\"station\":\"other\"}"};  // not a per-station key: never touched
}
}  // namespace

static void testAdopt() {
  // ---- the offer (candidate matrix)
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6}, other[6] = {1, 2, 3, 4, 5, 7};
  AdoptIn in = {};
  in.max_stations = 1, in.loaded = in.loaded_stored = true, in.up_has_mac = true, in.up_mac = mac;
  in.st_has_mac = true, in.st_mac = mac;
  CHECK(adoptCandidate(in));                                      // the same MAC in RAM
  in.st_mac = other;
  CHECK(!adoptCandidate(in));                                     // another device
  in.st_has_mac = false, in.st_mac = nullptr, in.persisted_mac_hash = adoptMacHash(mac);
  CHECK(adoptCandidate(in));                                      // after a bridge reboot: the learner core's mac_hash
  in.persisted_mac_hash = adoptMacHash(other);
  CHECK(!adoptCandidate(in));
  in.persisted_mac_hash = 0;
  CHECK(!adoptCandidate(in));                                     // no MAC known for the loaded station: no offer
  in.persisted_mac_hash = adoptMacHash(mac), in.up_has_mac = false;
  CHECK(!adoptCandidate(in));                                     // no MAC on the upload: no offer
  in.up_has_mac = true;
  AdoptIn x = in;
  x.blocked = true;
  CHECK(!adoptCandidate(x));
  x = in, x.parked = true;
  CHECK(!adoptCandidate(x));
  x = in, x.max_stations = 2;
  CHECK(!adoptCandidate(x));                                      // a profile with a free slot: not this case
  x = in, x.loaded_stored = false;
  CHECK(!adoptCandidate(x));
  CHECK(adoptCandidate(in));
  CHECK(adoptMacHash(mac) != 0);
  {  // the same rule as app_learn's macHash (FNV-1a over the 6 bytes, 0 -> 1)
    uint32_t h = 2166136261u;
    for (uint8_t b : mac) h = (h ^ b) * 16777619u;
    CHECK(adoptMacHash(mac) == (h ? h : 1));
  }

  // ---- the turned-away note carries the offer for its id only
  NoSlotNote n = {};
  const uint8_t key[32] = {9};
  n.note("NEW1", 1000);
  n.offer(true, "OLD1", key);
  CHECK(n.same && strcmp(n.from, "OLD1") == 0 && n.key_hash[0] == 9);
  n.note("NEW1", 2000);
  CHECK(n.same);                                                  // the same console again: the offer stays
  n.note("NEW2", 3000);
  CHECK(!n.same && n.from[0] == 0);                               // another id: the offer is gone
  n.offer(false, "OLD1", key);
  CHECK(!n.same);
  n.note("NEW1", 4000), n.offer(true, "OLD1", key), n.expire(4001 + NoSlotNote::SHOW_MS);  // ms is stored | 1
  CHECK(!n.same && n.id[0] == 0);                                 // expired with the note

  // ---- the journal and the keys
  CHECK(adoptJournalValid(journal("OLD1", "NEW1", 7)) && !adoptJournalValid(journal("OLD1", "OLD1", 7)) &&
        !adoptJournalValid(journal("", "NEW1", 7)));
  char k[16];
  adoptKey("lr", "OLD1", k);
  CHECK(strcmp(k, "lr:OLD1") == 0);
  adoptKey("st", "ABCDEFGHIJKLM", k);  // 13 chars: the hashed form, as app_core's nvsKey
  CHECK(k[2] == '#' && strlen(k) == 11);

  // ---- the move: byte-equal blobs under the new keys, the config's id line, the key rebound, the old keys gone
  static uint8_t buf[4096];
  MapKv kv;
  seedOld(kv, "OLD1");
  const auto before = kv.m;
  CHECK(adoptNvs(kv, journal("OLD1", "NEW1", 0x77), buf, sizeof buf));
  bool sameBlobs = true;
  for (const char* p : {"ch", "cl", "lr", "lm"}) {
    char ko[16], kn[16];
    adoptKey(p, "OLD1", ko), adoptKey(p, "NEW1", kn);
    sameBlobs = sameBlobs && kv.m.count(kn) && kv.m[kn] == before.at(ko) && !kv.m.count(ko);
  }
  CHECK(sameBlobs);
  CHECK(kv.m["st:NEW1"].first == AdoptKv::Type::Str && kv.m["st:NEW1"].second == "NEW1\n{\"name\":\"Garden\",\"publish\":true}");
  CHECK(!kv.m.count("st:OLD1") && !kv.m.count("sk:OLD1") && kv.m["sk:NEW1"].second == std::string(32, '\x77'));
  CHECK(kv.m["fw0"].second == "{\"station\":\"other\"}");
  {  // done twice (the journal found again after a cut just before its removal): the same state
    const auto once = kv.m;
    CHECK(adoptNvs(kv, journal("OLD1", "NEW1", 0x77), buf, sizeof buf) && kv.m == once);
  }
  {  // no key noted: no "sk" for the new id (its next upload binds), the old one still goes
    MapKv kz;
    seedOld(kz, "OLD1");
    CHECK(adoptNvs(kz, journal("OLD1", "NEW1", 0), buf, sizeof buf) && !kz.m.count("sk:NEW1") && !kz.m.count("sk:OLD1"));
  }
  {  // a long id: the hashed key forms on both sides
    MapKv kl;
    seedOld(kl, "OLD1");
    CHECK(adoptNvs(kl, journal("OLD1", "NEWLONGID1234", 0x77), buf, sizeof buf));
    char kn[16];
    adoptKey("st", "NEWLONGID1234", kn);
    CHECK(kl.m.count(kn) && kl.m[kn].second.rfind("NEWLONGID1234\n", 0) == 0 && !kl.m.count("st:OLD1"));
  }
  {  // a cut after every number of writes, then the replay: always the one final state
    MapKv ref;
    seedOld(ref, "OLD1");
    CHECK(adoptNvs(ref, journal("OLD1", "NEW1", 0x77), buf, sizeof buf));
    bool all = true;
    for (int cut = 0; cut <= 6; cut++) {
      MapKv c;
      seedOld(c, "OLD1");
      c.budget = cut;
      const bool first = adoptNvs(c, journal("OLD1", "NEW1", 0x77), buf, sizeof buf);
      all = all && (first == (cut >= 6));
      if (!first) all = all && c.m.count("st:OLD1") == 1;  // a failed pass erases nothing
      c.budget = -1;
      all = all && adoptNvs(c, journal("OLD1", "NEW1", 0x77), buf, sizeof buf) && c.m == ref.m;
    }
    CHECK(all);
  }
  {  // a buffer too small for the config: refused, nothing erased
    MapKv s;
    seedOld(s, "OLD1");
    CHECK(!adoptNvs(s, journal("OLD1", "NEW1", 0x77), buf, 16) && s.m.count("st:OLD1"));
  }

  // ---- the ring headers and the snapshot entry renamed; the records untouched
  static RamFs fs;
  fs.files.clear();
  CHECK(buildLayout(fs));
  CHECK(ringHeaderWrite(fs, hourSpec(0), ringFingerprint(), "OLD1") && ringHeaderWrite(fs, daySpec(0), ringFingerprint(), "OLD1"));
  static FlashRing h;
  h.init(&fs, hourSpec(0));
  for (int i = 0; i < 100; i++) {
    HourRec r = hourRec(1700000000u + (uint32_t)i * 3600, 1, 1, 0);
    h.append(&r);
  }
  char seg[RING_PATH_MAX];
  ringSegPath(seg, hourSpec(0), 0);
  const std::string seg0 = fs.files[seg];
  CHECK(ringRenameOwner(fs, ringFingerprint(), 0, "OLD1", "NEW1") == 2);
  RingHeader rh;
  CHECK(ringHeaderRead(fs, hourSpec(0), ringFingerprint(), &rh) && strcmp(rh.id, "NEW1") == 0);
  CHECK(ringHeaderRead(fs, daySpec(0), ringFingerprint(), &rh) && strcmp(rh.id, "NEW1") == 0);
  CHECK(ringRenameOwner(fs, ringFingerprint(), 0, "OLD1", "NEW1") == 0);  // the replay: nothing left to rename
  CHECK(fs.files[seg] == seg0);                                             // no record re-keyed or copied
  static FlashRing again;
  again.init(&fs, hourSpec(0));
  CHECK(again.scan() == 100);
  static SnapEntry se[RING_SLOTS], scratch[RING_SLOTS];
  memset((void*)se, 0, sizeof se);
  snprintf(se[0].id, sizeof se[0].id, "OLD1");
  se[0].last_seen = 123;
  CHECK(snapWrite(fs, se, 1));
  CHECK(snapRename(fs, "OLD1", "NEW1", scratch) == 1);
  static SnapEntry got;
  CHECK(snapFind(fs, "NEW1", &got) && got.last_seen == 123 && !snapFind(fs, "OLD1", &got));
  CHECK(snapRename(fs, "OLD1", "NEW1", scratch) == 0);  // the replay

  // ---- the route's precondition -- nothing of NEW1 yet, no key-form collision
  {
    MapKv p;
    seedOld(p, "OLD1");
    CHECK(adoptPrecheck(p, "OLD1", "NEW1") == AdoptBlock::None);
    for (const char* pp : {"st", "sk", "ch", "cl", "lr", "lm"}) {  // any stale NEW1 key refuses (no silent skip-then-erase)
      MapKv q = p;
      char kn[16];
      adoptKey(pp, "NEW1", kn);
      q.m[kn] = {AdoptKv::Type::Blob, "x"};
      CHECK(adoptPrecheck(q, "OLD1", "NEW1") == AdoptBlock::ToExists);
    }
    CHECK(adoptPrecheck(p, "OLD1", "OLD1") == AdoptBlock::KeyCollision);  // the same key forms (the branch a long-id FNV clash takes)
  }
  // ---- the refusal's way out: leftover NEW1 keys without a config (learners only here) are found by adoptKeysExist (the
  // Forget route accepts that id) and the refused id goes to the parked list (RAM: the admin lists it with Forget); once
  // Forget's six prefixes are erased, the precheck passes. Nothing is erased by the refusal itself.
  {
    MapKv p;
    seedOld(p, "OLD1");
    char kn[16];
    adoptKey("lr", "NEW1", kn);
    p.m[kn] = {AdoptKv::Type::Blob, std::string(300, '\x66')};
    const auto kv0 = p.m;
    CHECK(adoptKeysExist(p, "NEW1") && adoptKeysExist(p, "OLD1") && !adoptKeysExist(p, "GONE1"));
    CHECK(adoptPrecheck(p, "OLD1", "NEW1") == AdoptBlock::ToExists && p.m == kv0);  // refused, nothing touched
    ParkedList pk = {}, left = {};
    static Stations none;
    none = Stations();
    none.reserve(MAX_STATIONS);
    pk.add("PARK1");   // a stored config parked at boot (the old listing)
    left.add("NEW1");  // what adoptWriteJournal does on ToExists for an id with no slot: the leftover list, not the parked one
    const ParkedList shown = parkedListing(pk, left, none);
    CHECK(shown.n == 2 && shown.has("PARK1") && shown.has("NEW1") && parkedListing(pk, ParkedList{}, none).n == 1);
    // the leftover id is not parked: a Forget elsewhere plans no restart for it, and its console gets a free slot
    ParkedList noPark = {};
    CHECK(!forgetNeedsRestart(noPark) && forgetNeedsRestart(pk));
    CHECK(admitStation(none, noPark, "NEW1", defaultStationConfig(), MAX_STATIONS) != nullptr);
    CHECK(admitStation(none, pk, "PARK1", defaultStationConfig(), MAX_STATIONS) == nullptr);  // the parked rule, unchanged
    CHECK(parkedListing(pk, left, none).n == 1 && parkedListing(pk, left, none).has("PARK1"));  // NEW1 took its slot: not listed
    for (const char* pp : {"st", "sk", "ch", "cl", "lr", "lm"}) adoptKey(pp, "NEW1", kn), p.del(kn);  // Forget's erase set
    CHECK(!adoptKeysExist(p, "NEW1") && adoptPrecheck(p, "OLD1", "NEW1") == AdoptBlock::None);
    // a long id (the hashed key form) is found the same way
    const char* LONG = "LONGSTATION01";
    adoptKey("lm", LONG, kn);
    p.m[kn] = {AdoptKv::Type::Blob, "x"};
    CHECK(kn[2] == '#' && adoptKeysExist(p, LONG));
  }
  // ---- the room the move needs, from the old keys' real lengths
  {
    MapKv p;
    seedOld(p, "OLD1");  // st 41 + ch 40 + cl 32 + lr 300 + lm 961 B
    const uint32_t want = (2 + 2) + (2 + 2) + (2 + 2) + (10 + 2) + (31 + 2) + 3 + ADOPT_MARGIN_ENTRIES;
    CHECK(adoptNeedEntries(p, "OLD1") == want && want == 76);
    p.m["st:OLD1"].second = std::string("OLD1\n") + std::string(3795, 'x');  // the largest config text
    CHECK(adoptNeedEntries(p, "OLD1") == want - 4 + (119 + 2));
    MapKv none;
    CHECK(adoptNeedEntries(none, "OLD1") == ADOPT_MIN_FREE_ENTRIES);  // the floor
  }
  // ---- the route's pair check after the note's hour
  {
    NoSlotNote q = {};
    const uint8_t kq[32] = {3};
    q.note("NEW1", 5000);
    q.offer(true, "OLD1", kq);
    CHECK(q.offers("OLD1", "NEW1") && !q.offers("OLD1", "NEW2") && !q.offers("OLD2", "NEW1"));
    q.expire(5000 + NoSlotNote::SHOW_MS - 2);
    CHECK(q.offers("OLD1", "NEW1"));
    q.expire(5001 + NoSlotNote::SHOW_MS);
    CHECK(!q.offers("OLD1", "NEW1"));  // expired: "adopt: the offer has expired"
  }
  // ---- the forwarding rename, and the whole move replayed after a cut between any two steps
  {
    FwdTarget t[FWD_SLOTS] = {};
    t[0].used = true, snprintf(t[0].station, sizeof t[0].station, "OLD1");
    t[1].used = true, snprintf(t[1].station, sizeof t[1].station, "OTHER");
    t[2].used = false, snprintf(t[2].station, sizeof t[2].station, "OLD1");  // an unused slot is not touched
    uint32_t mask = 0;
    CHECK(fwdRenameStation(t, FWD_SLOTS, "OLD1", "NEW1", &mask) == 1 && mask == 1u && strcmp(t[0].station, "NEW1") == 0 &&
          strcmp(t[1].station, "OTHER") == 0 && strcmp(t[2].station, "OLD1") == 0);
    CHECK(fwdRenameStation(t, FWD_SLOTS, "OLD1", "NEW1", &mask) == 0 && mask == 0);  // the replay
  }
  {
    struct World {
      MapKv kv;
      RamFs fs;
      FwdTarget t[FWD_SLOTS];
    };
    static SnapEntry scratch2[RING_SLOTS];
    auto seedWorld = [](World& w) {
      seedOld(w.kv, "OLD1");
      w.fs.files.clear();
      buildLayout(w.fs);
      ringHeaderWrite(w.fs, hourSpec(0), ringFingerprint(), "OLD1");
      ringHeaderWrite(w.fs, daySpec(0), ringFingerprint(), "OLD1");
      static SnapEntry e1[RING_SLOTS];
      memset((void*)e1, 0, sizeof e1);
      snprintf(e1[0].id, sizeof e1[0].id, "OLD1");
      snapWrite(w.fs, e1, 1);
      memset(w.t, 0, sizeof w.t);
      w.t[0].used = true, snprintf(w.t[0].station, sizeof w.t[0].station, "OLD1");
    };
    const AdoptJournal j = journal("OLD1", "NEW1", 0x66);
    auto step = [&](World& w, int k) {  // the boot's order: NVS, headers, snapshot, forwarding
      if (k == 0) return adoptNvs(w.kv, j, buf, sizeof buf);
      if (k == 1) return ringRenameOwner(w.fs, ringFingerprint(), 0, j.from, j.to) >= 0;
      if (k == 2) return snapRename(w.fs, j.from, j.to, scratch2) >= 0;
      uint32_t m;
      fwdRenameStation(w.t, FWD_SLOTS, j.from, j.to, &m);
      return true;
    };
    static World ref, w;
    seedWorld(ref);
    for (int st = 0; st < 4; st++) step(ref, st);
    bool same = true;
    for (int cut = 0; cut <= 4; cut++) {  // `cut` steps done, then the power goes; the next boot runs them all again
      seedWorld(w);
      for (int st = 0; st < cut; st++) step(w, st);
      for (int st = 0; st < 4; st++) same = same && step(w, st);
      same = same && w.kv.m == ref.kv.m && w.fs.files == ref.fs.files && strcmp(w.t[0].station, "NEW1") == 0;
    }
    CHECK(same);
    RingHeader rh2;
    CHECK(ringHeaderRead(ref.fs, hourSpec(0), ringFingerprint(), &rh2) && strcmp(rh2.id, "NEW1") == 0 &&
          ref.kv.m["sk:NEW1"].second == std::string(32, '\x66'));
  }

  // ---- the journal's layout: version 1 (64 B, as older images wrote it) decodes and migrates; version 2 counts
  // the boots that started the move, written before it runs; after three such boots the fourth gives the journal up
  {
    // version 1 bytes as the old struct wrote them (from "OLD1", to "NEW1", the key hash 32 x 0x66; no padding)
    static const char* const V1 =
        "4f4c4431000000000000000000000000" "4e455731000000000000000000000000"
        "6666666666666666666666666666666666666666666666666666666666666666";
    uint8_t v1[ADOPT_JOURNAL_V1_SIZE];
    CHECK(strlen(V1) == 2 * ADOPT_JOURNAL_V1_SIZE);
    for (size_t i = 0; i < ADOPT_JOURNAL_V1_SIZE; i++) {
      unsigned hx = 0;
      sscanf(V1 + 2 * i, "%2x", &hx);
      v1[i] = (uint8_t)hx;
    }
    AdoptJournal d;
    CHECK(adoptJournalDecode(v1, sizeof v1, &d) && !strcmp(d.from, "OLD1") && !strcmp(d.to, "NEW1") && d.key[31] == 0x66 &&
          d.ver == ADOPT_JOURNAL_VER && d.boots == 0);
    // the old behaviour kept: a version 1 journal still runs the whole move (here: through the boot count)
    MapKv jk;
    seedOld(jk, "OLD1");
    jk.m[ADOPT_JOURNAL_KEY] = {AdoptKv::Type::Blob, std::string((const char*)v1, sizeof v1)};
    AdoptJournal j;
    CHECK(adoptJournalBoot(jk, &j) == AdoptBoot::Run && j.boots == 1 && !strcmp(j.to, "NEW1"));
    CHECK(jk.m[ADOPT_JOURNAL_KEY].second.size() == sizeof(AdoptJournal) && (uint8_t)jk.m[ADOPT_JOURNAL_KEY].second[64] == 2 &&
          (uint8_t)jk.m[ADOPT_JOURNAL_KEY].second[65] == 1 && jk.m[ADOPT_JOURNAL_KEY].second.compare(0, 64, std::string((const char*)v1, 64)) == 0);
    CHECK(adoptNvs(jk, j, buf, sizeof buf) && jk.m.count("st:NEW1") && !jk.m.count("st:OLD1") &&
          jk.m["sk:NEW1"].second == std::string(32, '\x66'));
    // a move that never finishes (the forwarding step failing every boot) and NEW1 forgotten meanwhile (its keys erased):
    // boots 2 and 3 replay and rebind sk:NEW1, the fourth gives the journal up, so that stops
    for (const char* pp : {"st", "sk", "ch", "cl", "lr", "lm"}) adoptKey(pp, "NEW1", k), jk.del(k);
    CHECK(adoptJournalBoot(jk, &j) == AdoptBoot::Run && j.boots == 2 && adoptNvs(jk, j, buf, sizeof buf) && jk.m.count("sk:NEW1"));
    jk.del("sk:NEW1");
    CHECK(adoptJournalBoot(jk, &j) == AdoptBoot::Run && j.boots == 3 && adoptNvs(jk, j, buf, sizeof buf) && jk.m.count("sk:NEW1"));
    jk.del("sk:NEW1");
    const auto moved = jk.m;
    CHECK(adoptJournalBoot(jk, &j) == AdoptBoot::GiveUp && !jk.m.count(ADOPT_JOURNAL_KEY) && !strcmp(j.from, "OLD1"));
    auto rest = moved;
    rest.erase(ADOPT_JOURNAL_KEY);
    CHECK(jk.m == rest && !jk.m.count("sk:NEW1") && adoptJournalBoot(jk, &j) == AdoptBoot::None);  // no more rebinding
    // the give-up line names where things were left (the store step failing on every boot: settings moved, rings not)
    {
      char t[160];
      adoptGiveUpText(t, sizeof t, "OLD1", "NEW1", false, true, true, false, 0x5);
      CHECK(!strcmp(t, "given up after 3 boots: settings on NEW1, rings on OLD1, target slot(s) 1 3 on OLD1"));
      adoptGiveUpText(t, sizeof t, "OLD1", "NEW1", true, true, false, true, 0);
      CHECK(!strcmp(t, "given up after 3 boots: settings on both, rings on NEW1, no target on OLD1"));
      adoptGiveUpText(t, sizeof t, "OLD1", "NEW1", false, false, false, false, 0);
      CHECK(!strcmp(t, "given up after 3 boots: no settings, no rings, no target on OLD1"));
      const char* L1 = "ABCDEFGHIJKLMNO", *L2 = "PQRSTUVWXYZABCD";  // the widest ids (15) and every slot
      const int tl = adoptGiveUpText(t, sizeof t, L1, L2, true, true, true, true, 0x7);
      CHECK(tl == (int)strlen(t) && tl + 41 + 24 < 200 && strstr(t, "target slot(s) 1 2 3 on ABCDEFGHIJKLMNO"));  // + "adopt A -> B " (41) + "<ms> [ADMIN] " fits a 200-B log line
      char small[16];
      CHECK(adoptGiveUpText(small, sizeof small, "OLD1", "NEW1", true, true, true, true, 1) == 15 && strlen(small) == 15);
    }
    // a count that cannot be written: the move does not run this boot, the journal stays as it was
    MapKv f;
    seedOld(f, "OLD1");
    AdoptJournal w = journal("OLD1", "NEW1", 0x55);
    w.ver = ADOPT_JOURNAL_VER;
    f.m[ADOPT_JOURNAL_KEY] = {AdoptKv::Type::Blob, std::string((const char*)&w, sizeof w)};
    const auto f0 = f.m;
    f.budget = 0;
    CHECK(adoptJournalBoot(f, &j) == AdoptBoot::Skip && f.m == f0);
    // anything else is no journal: another length, a later version, invalid ids
    AdoptJournal xv = w;
    xv.ver = 3;
    CHECK(!adoptJournalDecode((const uint8_t*)&xv, sizeof xv, &d) && !adoptJournalDecode(v1, 63, &d) &&
          adoptJournalDecode((const uint8_t*)&w, sizeof w, &d) && d.boots == 0);
    MapKv g;
    g.m[ADOPT_JOURNAL_KEY] = {AdoptKv::Type::Blob, std::string((const char*)&xv, sizeof xv)};  // version 3, 68 B
    CHECK(adoptJournalBoot(g, &j) == AdoptBoot::None && g.m.count(ADOPT_JOURNAL_KEY));  // a later version: left alone
    AdoptJournal bad = journal("OLD1", "OLD1", 1);
    bad.ver = ADOPT_JOURNAL_VER;
    CHECK(!adoptJournalDecode((const uint8_t*)&bad, sizeof bad, &d));
  }

  // ---- the admin's station GET names the offer (admin only; never on a public route)
  {
    KeyState ks = {};
    snprintf(ks.adopt, sizeof ks.adopt, "NEW1");
    std::string js;
    char ob[128];
    Out o(toString, &js, ob, sizeof ob);
    renderStationConfig(o, "OLD1", true, defaultStationConfig(), nullptr, &ks);
    o.flush();
    CHECK(js.find("\"adopt\":\"NEW1\"") != std::string::npos && jsonParses(js));
    KeyState none = {};
    js.clear();
    Out o2(toString, &js, ob, sizeof ob);
    renderStationConfig(o2, "OLD1", true, defaultStationConfig(), nullptr, &none);
    o2.flush();
    CHECK(js.find("adopt") == std::string::npos);
  }
}
