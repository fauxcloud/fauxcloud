// adopt.h -- "same console, new ID". A console reset can change the station ID; on the one-station profile the new
// ID is turned away (no room for a second station). When it uploads from the same device
// (the same MAC) as the loaded station, the admin offers to adopt it as the same station: history, settings, learners
// and forwarding move to the new ID and the bound upload key rebinds to the one the admin saw. Plain C++, host-tested.
//
// How it lands: the admin's confirmation writes a journal to NVS and plans a restart; the
// next boot, before any station loads, moves every per-station NVS entry (copy where the old key exists and the new
// does not, then erase the old ones), renames the ring headers and the snapshot entry, renames the forwarding targets,
// then removes the journal. Every step is idempotent: a power cut at any point is finished by the next boot. HA drops
// the old id's device by itself (a ghost: announced, no longer a station) and the new id announces on its first upload.
#pragma once
#include <cstddef>
#include <cstdint>

#include "wu_parse.h"  // STATION_ID_LEN

namespace fc {

// The console MAC's FNV-1a (0 -> 1), the learner core's mac_hash (app_learn macHash): one rule, two callers
uint32_t adoptMacHash(const uint8_t mac[6]);

// The offer's test, for an upload turned away for want of a slot (uploadVerdict NoSlot)
struct AdoptIn {
  bool blocked, parked;          // the turned-away id is on the block list / parked: no offer
  int max_stations;              // the profile's (one station: the case adopt serves)
  bool loaded;                   // the slot holds a station ...
  bool loaded_stored;            // ... stored in NVS (a pending RAM-only one has nothing to keep)
  bool up_has_mac;               // the upload's MAC is known (DHCP lease on the AP, ARP in the home subnet)
  const uint8_t* up_mac;
  bool st_has_mac;               // the loaded station's MAC seen since this boot (RAM) ...
  const uint8_t* st_mac;
  uint32_t persisted_mac_hash;   // ... else its learner core's persisted mac_hash (0 = unknown)
};
bool adoptCandidate(const AdoptIn& in);

// The journal, NVS key "adopt" (one blob, versioned by its length and `ver`): the ids and the key hash noted from the
// turned-away upload, and how many boots started the move. Version 1 (64 B: from, to, key) is what the first images
// wrote; version 2 (68 B) keeps those fields at the same offsets and appends ver + boots. A version 1 blob reads as
// version 2 with boots 0 (the migration: rewritten as version 2 at the next count); an image that reads version 1 only
// (a rollback) ignores a 68-B blob, so the move resumes when a version 2 image boots again.
struct AdoptJournal {
  char from[STATION_ID_LEN];
  char to[STATION_ID_LEN];
  uint8_t key[32];  // the key binding's HMAC of the new console's key; all zero = none (the next upload binds)
  uint8_t ver;      // ADOPT_JOURNAL_VER
  uint8_t boots;    // the boots that started the move, counted (and written) before it runs
  uint8_t pad[2];
};
constexpr const char* ADOPT_JOURNAL_KEY = "adopt";
constexpr size_t ADOPT_JOURNAL_V1_SIZE = 64;
constexpr uint8_t ADOPT_JOURNAL_VER = 2;
constexpr uint8_t ADOPT_MAX_BOOTS = 3;  // a move that has not finished after three boots is given up at the fourth
static_assert(sizeof(AdoptJournal) == 68 && offsetof(AdoptJournal, ver) == ADOPT_JOURNAL_V1_SIZE,
              "the journal's version 1 fields keep their offsets; ver and boots are appended");
bool adoptJournalValid(const AdoptJournal& j);  // both ids set, terminated, different
// The stored bytes (n = the blob's length) as version 2: a 64-B version 1 blob (ver 2, boots 0) or a 68-B blob with
// ver 2; false for anything else (another length, a later version, invalid ids). Nothing is written.
bool adoptJournalDecode(const uint8_t* b, size_t n, AdoptJournal* j);
// One call per boot, before the move: reads the journal through `kv`, then
//   None   -- no valid journal: nothing to do;
//   GiveUp -- it has started ADOPT_MAX_BOOTS moves already: it is deleted (the caller logs what is left);
//   Skip   -- the count could not be written: the move does not run this boot (no uncounted replay), the journal stays;
//   Run    -- boots + 1 written back as version 2 (a cut during the move counts too): run the move with *j.
//   (Skip on every boot -- a count that can never be written, NVS full -- means the move never runs and never gives up:
//   safe, no uncounted replay; one ADMIN line per boot says so)
enum class AdoptBoot : uint8_t { None, Run, Skip, GiveUp };
class AdoptKv;
AdoptBoot adoptJournalBoot(AdoptKv& kv, AdoptJournal* j);
// The give-up line's tail: where a move that never finished left things, so the owner can finish it by hand, e.g.
// "given up after 3 boots: settings on NEW1, rings on OLD1, target slot(s) 1 3 on OLD1" ("on both" when both ids hold some). keys_* = any per-station key of that id
// (adoptKeysExist), rings_* = a history ring owned by it, targets = the forwarding slots (bit i = slot i) that still name `from`.
// Returns the length (cut to cap).
int adoptGiveUpText(char* out, size_t cap, const char* from, const char* to, bool keys_from, bool keys_to, bool rings_from,
                    bool rings_to, uint32_t targets);

// The per-station NVS keys follow app_core's nvsKey rule: "<pp>:<id>" for ids <= 12 chars, "<pp>#<fnv32 hex>" beyond
void adoptKey(const char pp[2], const char* id, char out[16]);
// the prefixes that move: config (a string whose first line is the id), channel maps, calibration log, learner core and
// mask; the bound key ("sk") is not copied but written from the journal (the rebind), the old one erased
constexpr int ADOPT_PREFIXES = 5;
extern const char* const ADOPT_MOVE[ADOPT_PREFIXES];  // "st" "ch" "cl" "lr" "lm"

// The NVS the move runs on (the device wraps Preferences; the host tests a map)
class AdoptKv {
 public:
  enum class Type : uint8_t { None, Str, Blob };
  virtual ~AdoptKv() = default;
  virtual Type type(const char* k) = 0;
  virtual long get(const char* k, uint8_t* buf, size_t cap) = 0;  // bytes read (a string without its NUL), -1 = failed
  virtual long size(const char* k) = 0;  // the value's length (a string without its NUL), -1 = absent
  virtual bool put(const char* k, Type t, const uint8_t* d, size_t n) = 0;
  virtual void del(const char* k) = 0;
};
// Step 1 of the move, idempotent: copy (the config's first line rewritten to the new id) where the old key exists and
// the new one does not, write the new "sk" from the journal (when it has a key), then erase every old key. `buf` holds
// one value (the config text is the largest: <= 3 800 B). False = a read or a write failed (the journal stays; the next
// boot tries again).
bool adoptNvs(AdoptKv& kv, const AdoptJournal& j, uint8_t* buf, size_t cap);
// the route's precondition, checked before the journal is written. Copy-if-absent is exact only
// when nothing of `to` exists yet (nothing writes `to` keys between the journal and the boot: it has no slot) and the
// two ids' key forms differ (a long-id FNV collision would erase what it copied).
enum class AdoptBlock : uint8_t { None, ToExists, KeyCollision };
AdoptBlock adoptPrecheck(AdoptKv& kv, const char* from, const char* to);
// Any per-station key of `id` stored (st sk ch cl lr lm: what Forget erases). A ToExists refusal lists such an id with the
// parked ones, so leftover keys of the new id (learners, maps or a key without a config) can be forgotten.
bool adoptKeysExist(AdoptKv& kv, const char* id);
// the NVS entries the move needs while the copies coexist with the old keys: per present old key
// ceil((len + 1) / 32) data entries + 2 (index + header; strings count their NUL), + 3 for the new "sk" blob, + a
// margin of 16; never below the floor of 40. A typical station needs ~70-100, a 3.8-KB config pushes it to ~193.
constexpr uint32_t ADOPT_MIN_FREE_ENTRIES = 40, ADOPT_MARGIN_ENTRIES = 16;
uint32_t adoptNeedEntries(AdoptKv& kv, const char* from);

}  // namespace fc
