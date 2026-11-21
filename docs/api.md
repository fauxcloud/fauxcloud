# fauxcloud API reference

fauxcloud is ESP32 firmware that stands in for Weather Underground for a Wi-Fi weather console. It answers the
console's upload, keeps a short history and publishes to MQTT with Home Assistant discovery. This page describes
every HTTP route, the MQTT topics and the data fields, written from the code of the release it ships with (checked
against firmware 1.0.0). **The code is the source of truth.**

fauxcloud is an independent open-source project, not affiliated with or endorsed by Weather Underground / The Weather
Company, Weathercloud, Windy, WOW-BE (wow.meteo.be) or the Met Office's WOW, PWSweather, Home Assistant, NOAA /
aviationweather.gov, Vevor or SucceBuy. Names are used only to say what fauxcloud works with.

Examples are sanitised: the station id is `EXAMPLE1`, keys and passwords are `********`, the bridge MAC is shown as
`<mac12>` / `<mac4>` (examples use `02000000a1b2` / `A1B2`), and home-network addresses are `192.168.1.x`.

---

## 1. Overview

### 1.1 Two modes, two sides

fauxcloud runs one HTTP server on port 80. Which routes exist depends on the **mode**; who may use them depends on
the **side** (the "face") a request arrives on.

| Mode | When | What is served |
|---|---|---|
| **Run** | normal life | console upload, dashboard, read-only JSON, static assets, `/admin` + `/api/admin/*` (the AP side gets only the upload, `/tools` and `/`, see below) |
| **Setup** | first boot, after a factory reset, after "setup again", after a short BOOT press | the setup page, `/api/setup/*` and the picker's data assets (`/a/*`); the page and `/api/setup/*` answer on the AP side only |

| Side (face) | Who | Address |
|---|---|---|
| **AP** | the console, and any phone joined to fauxcloud's own Wi-Fi (`fauxcloud-<mac4>` in run mode) | `http://192.168.77.1/` (fauxcloud's DNS answers every name with this address) |
| **LAN** (STA) | the home network | `http://fauxcloud.local/` (mDNS, the hostname is configurable) or the address the router gave it |

**How the face is decided.** For every request fauxcloud looks at both the local address and the peer address
of the TCP connection:

| Local address is 192.168.77.1 | Peer is in 192.168.77.0/24 | Face |
|---|---|---|
| yes | yes | `ap` |
| no | no | `lan` (`sta` in some JSON fields) |
| one of them only | | `mixed` |

**What the AP side serves in run mode.** Only the WU upload path, `/tools`, `/` (which is the
`/tools` plain status page: no JavaScript, not the dashboard shell) and the OS connectivity probes (§3.7). Every other public path on the AP side
(`/api/*`, `/a/*`, `/admin`) answers the catch-all `200 success`, exactly like an unknown path. The LAN side serves
everything as before.

A `mixed` request is treated as neither side: every page and admin call answers like an unknown path, and an upload
is answered and dropped (counted as a LAN drop).

**The `Host` header on the LAN side (run mode).** A page or API request from the home network must name the bridge:
its `Host` is an IP address (IPv4, or IPv6 in brackets), the hostname, or the hostname followed by `.local` or by one of
the private-use suffixes routers hand out: `.lan`, `.home`, `.internal`, `.home.arpa` (any case, an optional trailing
dot and `:port`; a `Host` over 71 bytes is refused), or it has no `Host` at all. Any other name answers like an unknown path: `fauxcloud.example.com`, and
also a router domain under a public top-level domain such as `fauxcloud.fritz.box` (use the address or `.local`). This stops a web page that points its own name at the bridge's address (DNS rebinding) from
reading the open LAN routes. The upload path, setup mode and the AP side do not check `Host` (the console sends the WU
host name).

mDNS (`<hostname>.local`) is announced on the LAN side only. DNS (port 53) and NTP (port 123) answer on the AP side
only.

### 1.2 Transport

- Plain **HTTP/1.1, no TLS**, on both sides. The admin login never sends the password (see §4).
- One request is handled at a time. Keep requests small and sequential. The console's upload is never shed or paced, but it waits for the request already
  running (a stalled client can hold the server for a few seconds).
- Request bodies are limited to **3072 bytes**, except the firmware upload (`POST /api/admin/ota`: a raw image up to
  the size of the app slot, `Content-Length` required). A larger body (up to 16 KB) is read, discarded and answered
  with a JSON error, then the connection is closed. A larger or stalled body is cut at once.
- API query strings are read through a 128-byte buffer; keep them short.

### 1.3 The `X-Fauxcloud-Face` response header

`/`, `/tools`, `/api/live` and `/api/bridge/status` send `X-Fauxcloud-Face: ap` or `X-Fauxcloud-Face: lan` (the AP side only ever gets it from `/` and `/tools`; the API routes answer the catch-all there). Other routes do not send it.

### 1.4 JSON conventions

| Topic | Rule |
|---|---|
| Units | Always metric, fixed per field (§6): °C, %, hPa, km/h, degrees, mm, mm/h, W/m², km. The dashboard converts for display; the API never does. |
| Decimals | Each field has fixed decimals (§6). History values are stored as fixed point, so they come back rounded to that step. |
| Timestamps | ISO 8601 UTC with `Z` (`"2026-11-05T10:15:00Z"`). Exceptions: `/api/recent` rows use unix seconds; hourly keys are `"YYYY-MM-DDTHH:MMZ"` (start of the UTC hour); daily keys are `"YYYY-MM-DD"` (the **local** date in the configured time zone). |
| Before the clock is set | `now`, `last_seen` and recent `t` carry seconds since boot (small numbers, 1970 dates). Check `time_synced`. Readings are re-stamped once real time arrives. |
| `null` | A value that is known to be missing (e.g. a history slot without a reading, an unset `lat`). |
| Absent key | Not applicable or not known (e.g. a field the console never sent, `stale_since` while the sensor is fine). |
| Errors in JSON | Usually HTTP 200 with `{"error":"..."}` or `{"ok":false,"errors":["key: reason", ...]}`. See §1.6. |

### 1.5 Caching

| What | Cache-Control | ETag | 304 |
|---|---|---|---|
| Pages `/`, `/admin`, setup `/` + `/setup` | `no-cache` | `"<build hash>"` of the page | yes, on `If-None-Match` |
| Assets `/a/<hash8>/<name>` | `public, max-age=31536000, immutable` | none (the hash in the path changes with the content) | no |
| `/api/live?station=` | `no-store` | yes | yes |
| `/api/bridge/status` | `no-store` | yes | yes |
| `/api/recent`, `/api/hourly`, `/api/daily` | `no-store` | yes | yes |
| `/api/metar` | `no-store` | yes | yes (§3.2.5) |
| `/api/live` (all stations), `/api/station`, `/api/bridge`, `/api/learn`, `/tools`, all admin and setup JSON | `no-store` | none | no |

ETags are opaque strings; treat them as such. The ETags of the JSON endpoints change on every reboot, so one from
before a restart never matches. A page's ETag is its build hash and changes only with a new firmware. The history ETags also follow the station's config revision, so hiding or showing a field changes them at once. `If-None-Match` accepts a list, `*` and weak tags (`W/"..."`). A 304 has no body. Polling with
`If-None-Match` is the intended way to poll: most polls of `/api/live?station=` between two uploads cost a 304.

Assets and the page shells are always served `Content-Encoding: gzip` (straight from flash), whatever the request's
`Accept-Encoding`.

### 1.6 Status codes

fauxcloud answers almost everything with **200**, because a console that sees anything else retries.

| Code | When |
|---|---|
| 200 | Normal answers, JSON errors, and **every unknown path or method**: `text/plain` body `success` (or `200` when the `Host` header contains `weathercloud`). A denied admin call (except the 401 case below), an admin call from the AP side, a public `/api/*` or `/a/*` or `/admin` request from the AP side and an over-long URI (> 512 B) or header get exactly this answer too (the run-mode AP's OS connectivity probes excepted, §3.7). |
| 204 | OS connectivity probes `/generate_204`, `/gen_204` (run mode, AP side, GET). |
| 302 | Setup mode, AP side: any unknown path or method → `Location: http://fauxcloud.setup/`. |
| 202 | `POST /api/admin/forwarding/test`, `…/metar/run`, `…/metar/lookup`, `…/wufc/run`: the job was accepted, poll the matching GET. |
| 304 | `If-None-Match` matched (§1.5). |
| 401 | An `/api/admin/*` call from the home network (LAN face) that carries `X-Fauxcloud: 1` but no valid session: `{"error":"auth"}`, `Cache-Control: no-store`, connection closed (a request body is not read). Without the header, or from the AP side, the catch-all `200 success` as before. |
| 404 | `/a/<hash8>/<name>` with an unknown hash or name (`text/plain` body `not found`); and a public endpoint that names a station that does not exist, or (no `station` given) when no station is registered yet (`{"error":"no such station"}`, see below). The only real 404s. |
| 400 | `POST /api/admin/metar` and `POST /api/admin/wufc` with a refused body (`{"errors":["…"]}`) and `POST /api/admin/metar/lookup` with a bad `ids` (§3.3.8). |
| 409 | `POST /api/admin/ota` while another firmware upload runs or while the bridge listens to a new console; `POST /api/admin/capture/start` (§3.3.9); refusals of the forwarding test, the airport-weather run and lookup and the forecast run (§3.3.7, §3.3.8). |
| 503 | During a firmware upload: page and asset loads (`/` on the LAN side, `/admin`, `/a/*`; `text/plain`, `Retry-After: 60`) and history endpoints (`{"error":"updating"}`); a history request that finds no memory for its copy (`{"error":"low memory, try again"}`); `POST /api/admin/wufc/run` on an idle line below its memory gate (`{"error":"low_memory"}`); `GET /tools` with no memory for its copy (empty body, `Connection: close`). Load shedding: **any** page or API request (the console upload excepted) is answered `503` with an empty body and `Connection: close` at once when the largest byte-addressable block is under 8 KB, or after waiting up to 200 ms when the free byte heap stays under 24 KB (`http_shed`, `http_paced` in `diag`). |

`?station=` naming a station that does not exist gives **404** with `{"error":"no such station"}` on the public
endpoints (`/api/live?station=`, `/api/recent`, `/api/hourly`, `/api/daily`, `/api/station`, `/api/learn`). The
admin API keeps its own 200 answers.

### 1.7 Rate limits and busy answers

| Limit | Value | Answer |
|---|---|---|
| Console uploads, same station | one per 4 s | `success`, sample dropped, counted (`uploads_rate_limited`) |
| Admin challenges, per client address | one per second, one outstanding nonce per address, at most 4 addresses | `{"error":"rate","retry_ms":1000}` |
| Wi-Fi scan rescans (`again=1`) | one per 15 s | the cached result with `"throttled":true` |
| Firmware upload | one at a time | 409 |
| During a firmware upload | new page and asset loads and history refused (503); open pages' `/api/live` and `/api/bridge/status` polls still work; console uploads are answered but not processed | |

---

## 2. Station upload (the console's side)

### 2.1 Weather Underground path

`GET /weatherstation/updateweatherstation.php?<WU PWS parameters>` — run mode, AP side (LAN side only with
`lan_uploads` on).

The console resolves `rtupdate.wunderground.com` (or any WU host) through fauxcloud's DNS and gets
192.168.77.1. fauxcloud answers **first**, in one TCP segment, then reads the query:

```
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 7
Connection: close

success
```

The answer is always `success`, whether the sample is used or not. fauxcloud never sends `INVALIDPASSWORDID`.

**Parameters.** Keys are matched case-insensitively (`dateutc` included).

| Parameter | Handling |
|---|---|
| `ID` | Station id. Any other character becomes `_`; cut to 15 characters (`id_sanitized` tells). Redacted as `***` in the admin console view, like `PASSWORD`. Missing or empty → `ST1`. |
| `PASSWORD` | The station key. Never stored or logged; only a keyed hash is kept (trust rules below). Redacted as `***` in the admin console view. |
| `dateutc` | `YYYY-MM-DD HH:MM:SS` (lenient: 1-2 digit fields, any separators) or `now`; a value that does not parse (year 2000-2099, month 1-12, day 1-31, hour ≤ 23, minute ≤ 59, second ≤ 60) counts as not sent, like `now`. Informational only: fauxcloud stamps readings with its own clock. It also feeds the "console clock" notice and picks the dedupe window (§2.2). |
| `action`, `softwaretype`, `realtime`, `rtfreq` | Protocol flags, ignored. |
| Measurement parameters | See §6.1. Each value is parsed on its own; a bad value drops only that field (rule `parse` or `bounds`). An empty value (`UV=`) means "not sent". The last occurrence of a repeated key wins. |
| Anything else | Kept as an "extra" (up to 4 per upload, key ≤ 15 chars, value ≤ 15 chars), remembered as a known key per station (up to 8), promotable to a sensor in admin. |

The URI limit is 512 bytes; a longer upload is answered `success` and lost (`uploads_too_long`). Only `GET` is
accepted on this path; any other method gets the catch-all `success`. While the bridge listens to a new console (§3.3.9)
every upload, from the fauxcloud Wi-Fi or the home network, is answered `success` and neither stored nor turned into a
station (`uploads_skipped` in that section's GET).

### 2.2 Trust rules

An upload goes through these checks in order. Every refusal is still answered `success` and counted.

| # | Check | Counter (`diag`) |
|---|---|---|
| 1 | Side: AP side accepted; LAN side only with bridge setting `lan_uploads: true`; `mixed` never | `uploads_dropped_lan` |
| 2 | Block list: the id is on the admin's block list (≤ 8 ids) | `uploads_dropped_blocked` |
| 3 | Upload queue full (8 deep) | `queue_drops` |
| 4 | No free station slot (`max_stations` reached; the build's station count is the ceiling: 1 on the WROOM-32 build) | `uploads_no_slot` (also an id that is stored but has no slot this boot) |
| 5 | Rate: < 4 s after this station's last accepted upload | `uploads_rate_limited` |
| 6 | Dedupe: the identical query string again within 120 s (30 s when `dateutc` is `now`, missing or not a valid date) | `uploads_duplicate` |
| 7 | Key: the first accepted upload binds the station to its `PASSWORD` (trust on first use); another key later is dropped until the owner accepts it in admin | `uploads_dropped_key`, per station `key_mismatch` |

A new station's first accepted upload also sets its **profile** from a fingerprint of the console (User-Agent,
parameter order, MAC vendor). A clear mismatch with the 7-in-1 picks `generic`. Stations stored earlier keep theirs.

### 2.3 Weathercloud

fauxcloud does **not** parse Weathercloud uploads. A request to any other path whose `Host` header contains
`weathercloud` (e.g. `api.weathercloud.net/v01/set/...`) gets `200 OK` with body `200`, the success body
Weathercloud uses. The data still arrives through the WU upload, which the same console sends. A console with
Weathercloud ticked can repeat its WU upload within a cycle; the dedupe rule above drops the copies.

### 2.4 DNS and NTP on the AP side

- **DNS:** every A query gets 192.168.77.1, in both modes. Other record types get an empty answer.
- **NTP** (run mode only): answered only while fauxcloud has real time (SNTP, the router's DHCP NTP server, or a
  browser seed). A clock that is held behind the stored history (more than 1 h) is not served either. Listening to a
  new console (§3.3.9) changes neither answer; it counts the NTP asks and answers.

---

## 3. HTTP routes

Auth column: **none** = public; **CSRF** = header `X-Fauxcloud: 1` required; **session** = valid `fcs` cookie
presented from the IP it was issued to; **proof** = a fresh password proof in the body (§4). Every admin route also
requires the LAN face. A request that fails the LAN, `Host` or CSRF check gets the catch-all `200 success` (it looks
like an unknown path); a LAN call with the CSRF header whose session is missing or invalid gets `401 {"error":"auth"}`.
A wrong or locked-out proof answers `200 {"ok":false,"error":"wrong password"}` or
`{"ok":false,"error":"locked","retry_s":N}` (§4).

### 3.1 Route table


Run mode (every route below is in the default TLS build; the HTTP-only build `FWD_TLS=0` has no `/api/metar`, no airport-weather and no forecast routes; `/api/learn/relearn` lies outside the `/api/admin/` prefix. On the AP side only the upload, `/tools` and `/` are served, everything else answers `success`, except the OS connectivity-probe paths (§1.6, §3.7)):

| Method | Path | Face | Auth | Purpose |
|---|---|---|---|---|
| GET | `/weatherstation/updateweatherstation.php` | AP (LAN with `lan_uploads`) | none | console upload (§2) |
| GET | `/` | both | none | LAN: dashboard page; AP: the `/tools` page |
| GET | `/tools` | both | none | plain HTML status page (no JavaScript) |
| GET | `/api/live` | LAN (AP: `success`) | none | live values, one or all stations |
| GET | `/api/bridge/status` | LAN (AP: `success`) | none | bridge health and station list |
| GET | `/api/recent` | LAN (AP: `success`) | none | last 60 readings |
| GET | `/api/hourly` | LAN (AP: `success`) | none | last 48 hours, or a range from flash (`from`, `to`) |
| GET | `/api/daily` | LAN (AP: `success`) | none | last 30 days, or more from flash |
| GET | `/api/learn` | LAN (AP: `success`) | none | learner state, LAN, not part of the public contract (§3.2.4) |
| GET | `/api/station` | LAN (AP: `success`) | none | a station's config (read-only) |
| GET | `/api/bridge` | LAN (AP: `success`) | none | bridge config (read-only) |
| GET | `/api/metar` | LAN (AP: `success`) | none | airport weather (not in the `FWD_TLS=0` build, §3.2.5) |
| GET | `/a/<hash8>/<name>` | LAN (AP: `success`) | none | static assets |
| GET | `/admin` | LAN | none (the page signs in) | admin page |
| GET | `/api/admin/challenge` | LAN | CSRF | login nonce |
| POST | `/api/admin/login` | LAN | CSRF + proof | sign in |
| POST | `/api/admin/logout` | LAN | CSRF + session | sign out |
| GET | `/api/admin/sessions` | LAN | CSRF + session | list sessions |
| POST | `/api/admin/sessions/revoke` | LAN | CSRF + session | end one session |
| POST | `/api/admin/sessions/revoke-all` | LAN | CSRF + session | end all sessions |
| GET | `/api/admin/settings` | LAN | CSRF + session | device settings |
| PUT | `/api/admin/settings` | LAN | CSRF + session | change device settings |
| PUT | `/api/admin/password` | LAN | CSRF + session + proof | change the admin password |
| POST | `/api/admin/ap-pass/reveal` | LAN | CSRF + session + proof | read the fauxcloud Wi-Fi password |
| GET | `/api/admin/station` | LAN | CSRF + session | station config + key state |
| PUT | `/api/admin/station` | LAN | CSRF + session | change station config |
| GET | `/api/admin/rings` | LAN | CSRF + session | history rings on flash |
| POST | `/api/admin/station/forget` | LAN | CSRF + session | forget a station (or a flash-only history id) |
| POST | `/api/admin/station/adopt` | LAN | CSRF + session | keep a station's history under the new id of the same console |
| POST | `/api/admin/station/accept-key` | LAN | CSRF + session | accept a new console key |
| POST | `/api/admin/station/forget-key` | LAN | CSRF + session | forget a known extra key |
| POST | `/api/learn/relearn` | LAN | CSRF + session | "Restart learning" for a station (§3.2.4); gated like an admin call, though outside `/api/admin/` |
| POST | `/api/admin/station/block` | LAN | CSRF + session | block an id |
| POST | `/api/admin/station/unblock` | LAN | CSRF + session | unblock an id |
| GET | `/api/admin/station/blocked` | LAN | CSRF + session | block list |
| GET | `/api/admin/station/console` | LAN | CSRF + session | recent raw upload lines (redacted) |
| POST | `/api/admin/ota` | LAN | CSRF + session | firmware upload |
| GET | `/api/admin/capture` | LAN | CSRF + session | listening to a new console: state and records (§3.3.9) |
| POST | `/api/admin/capture/start` | LAN | CSRF + session | start listening (§3.3.9) |
| POST | `/api/admin/capture/stop` | LAN | CSRF + session | stop listening (§3.3.9) |
| POST | `/api/admin/coredump` | LAN | CSRF + session + proof | download crash data |
| DELETE | `/api/admin/coredump` | LAN | CSRF + session | clear crash data |
| GET | `/api/admin/bootlog` | LAN | CSRF + session | the boot log, `text/plain` (§3.3.6) |
| GET | `/api/admin/bridge` | LAN | CSRF + session | bridge config |
| PUT | `/api/admin/bridge` | LAN | CSRF + session | change bridge config |
| GET | `/api/admin/wifi/scan` | LAN | CSRF + session | Wi-Fi scan |
| POST | `/api/admin/mqtt/test` | LAN | CSRF + session | start an MQTT connection test |
| GET | `/api/admin/mqtt/test` | LAN | CSRF + session | MQTT test result |
| POST | `/api/admin/reboot` | LAN | CSRF + session | restart |
| POST | `/api/admin/factory-reset` | LAN | CSRF + session | wipe everything |
| POST | `/api/admin/setup-again` | LAN | CSRF + session | restart into setup mode, keeping data |
| GET | `/api/admin/forwarding` | LAN | CSRF + session | forwarding targets, state and counters (§3.3.7) |
| PUT | `/api/admin/forwarding?slot=<0-2>` | LAN | CSRF + session | add, change or remove a target |
| POST | `/api/admin/forwarding/test?slot=<0-2>` | LAN | CSRF + session | send one reading now (202, poll GET) |
| GET | `/api/admin/forwarding/test` | LAN | CSRF + session | the test's state and answer |
| POST | `/api/admin/forwarding/enable` | LAN | CSRF + session | switch forwarding on (one station, LAN uploads off) |
| GET | `/api/admin/metar` | LAN | CSRF + session | airport weather settings, state and lookup (§3.3.8; not in the `FWD_TLS=0` build) |
| POST | `/api/admin/metar` | LAN | CSRF + session | switch airport weather on / off, choose airports, name them |
| POST | `/api/admin/metar/run` | LAN | CSRF + session | fetch the reports now (202, poll GET) |
| POST | `/api/admin/metar/lookup` | LAN | CSRF + session | look up airports (`?near=1` or `?ids=`; 202, poll GET) |
| POST | `/api/admin/wufc` | LAN | CSRF + session | the WU forecast's switch and API key (not in `FWD_TLS=0`) |
| POST | `/api/admin/wufc/run` | LAN | CSRF + session | "Fetch now": one forecast pull, the key check (202 queued with an estimate, poll the forwarding GET) |

Test builds only (`FAUXCLOUD_TEST`, never in a release image): `POST /api/admin/debug/hang`,
`POST /api/admin/debug/seed-days`, `POST /api/admin/debug/roll` (`station=<id>&what=hour|day`: closes the open hour, or hour + day, now).

Setup mode (the pages and `/api/setup/*` answer on the AP side only and the LAN side gets the catch-all, except `/a/<hash8>/<name>`, which is served on both sides):

| Method | Path | Purpose |
|---|---|---|
| GET | `/` and `/setup` | setup page |
| GET | `/api/setup/scan` | Wi-Fi scan |
| GET | `/api/setup/state` | join state and current values |
| POST | `/api/setup/apply` | save and join the home Wi-Fi |
| POST | `/api/setup/time` | seed the clock from the phone |
| GET | `/a/<hash8>/<name>` | static assets (the location picker's data) |

### 3.2 Public read API (run mode, LAN face; the AP face gets the catch-all `success` for all of it except `/tools`)

`/api/recent`, `/api/hourly`, `/api/daily`, `/api/station` and `/api/learn` answer 404 `{"error":"no such station"}` for an unknown station id (or no station yet). The history routes answer 503 during a firmware upload and when they find no memory for their copy (§1.6).

#### `GET /api/live?station=<id>` — one station

The dashboard's main poll. ETag + 304. `station` must name an existing station (else 404 `{"error":"no such station"}`).

Top level: `now` (ISO UTC), `time_synced` (bool), `time_src` (`none | persisted | browser | dhcp | sntp`), `page`
(8 hex digits: the version of the dashboard shell this firmware serves, the same value as the shell's ETag and its
`<html data-page>`; an open dashboard whose own version differs reloads itself once, so a home-screen app picks up an
OTA); on the LAN face also `uptime_s` and `diag` (§3.2.1); then `station`.

`station` object (keys in the order sent):

| Key | Type | Meaning |
|---|---|---|
| `id` | string | station id |
| `name` | string | configured name, else `"fauxcloud <id>"` |
| `configured` | bool | the station has any config override |
| `stored` | bool | its config is saved in flash (survives a reboot) |
| `publish` | bool | published to MQTT |
| `profile` | string | `yt60234` or `generic` |
| `hidden` | [string] | field keys hidden by the owner (left out of `values` and `age`) |
| `derived` | {key: n} | present only when some values are computed by fauxcloud; n = readings behind each |
| `sources` | {key: string} | present only while a derived value is estimated another way: `{"rain_mm":"hours"}` = from the hour records after a restart (§6.2) |
| `sensor_stale` | bool | the outdoor sensor is not updating |
| `baro_uncal` | bool | the console reports its factory pressure (not calibrated); set after 60 uploads in a row at the factory value |
| `stale_since` | ISO | present while `sensor_stale` and the time is known |
| `notices` | [{id, chip, text, …}] with the notice's own parameters (`since`, `n`, `skew_s`, `d_hpa`, …; §6.4) | present only when there are any (§6.4) |
| `outlook` | object | the local outlook (§3.2.2); omitted until the station has sent a pressure (no barometer, no outlook) |
| `wufc` | object | LAN face, `FWD_TLS` builds: the WU forecast line while it is shown -- `{"day":"tomorrow"\|"today","phrase":"Light Rain","hi":9,"lo":4,"pop":70\|null,"t":"<ISO fetched>"}`; absent with the switch off, never fetched, stale (> 2 h) or for a day other than today / tomorrow. `day` from the valid day against the bridge's local date ("today" between 00:00 and WU's 03:00 rollover). `phrase` is WU's own words (≤ 40 B, as received); °C and % as integers. Its own source: never in `outlook`, the live ETag covers it |
| `warming` | [string] | present only while a derived value waits for enough history |
| `online` | bool | an upload arrived within the station's `stale_s` (default 240 s) |
| `last_seen` | ISO | last upload (fauxcloud's clock) |
| `age_s` | int | seconds since `last_seen` |
| `src_ip`, `src_if` | string | LAN face only: the console's address and side (`ap`, `sta`; `unknown` for a restored station before its first upload) |
| `uploads` | int | uploads processed since boot (an upload the filter rules rejected, in part or whole, still counts; uploads dropped before the filters are only in the `diag` counters) |
| `restored` | bool | the values came back from flash after a restart; no upload yet |
| `dateutc` | ISO or null | the console's own timestamp from the last upload |
| `values` | {key: number or null} | every field's last accepted value (merged across uploads); `wind_avg10m_kmh` reads `null` once its newest reading is more than 600 s before the last upload, and a value that cannot be formatted is `null` too |
| `age` | {key: int} | seconds **before the last upload** that each value arrived (0 = in the last upload) |
| `last_rejected` | [string] | fields the filters rejected in the last upload |
| `rejects_since_boot` | {key: int} | per-field rejections since boot (only fields with > 0) |
| `rej` | {rule: int} | per filter rule: uploads in which it fired, since boot (all 7 rules, §6.3) |
| `dropped` | int | whole samples dropped (rule `profile_humidity`) |
| `promoted` | {key: {...}} | owner-promoted extras: `v` (number or null), `name`, `type`, `unit`, `group`, `icon`, `span`, `max`, `dec`, `age` |
| `extras` | {param: string} | unknown parameters of the last upload, raw |
| `known` | [{k, v, age}] | every unknown parameter seen (up to 8), last raw value |
| `extras_dropped` | int | unknown parameters beyond 4 in the last upload |
| `id_sanitized` | bool | the raw `ID` had to be cleaned |

Example (LAN face, `diag` shortened):

```json
{"now":"2026-11-05T10:16:05Z","time_synced":true,"time_src":"sntp","page":"1a2b3c4d","uptime_s":86400,
 "diag":{"ap_clients":1,"sta_up":true,"sta_rssi":-61,"mqtt_up":true,"fw_version":"fauxcloud 1.0.0","...":"..."},
 "station":{"id":"EXAMPLE1","name":"Garden","configured":true,"stored":true,"publish":true,"profile":"yt60234",
  "hidden":[],"derived":{"rain_mm":56,"wind_avg2m_kmh":2,"wind_avg10m_kmh":10},"sensor_stale":false,"baro_uncal":false,
  "online":true,"last_seen":"2026-11-05T10:15:00Z","age_s":65,"src_ip":"192.168.1.23","src_if":"sta",
  "uploads":1312,"restored":false,"dateutc":"2026-11-05T10:14:58Z",
  "values":{"temp_c":8.4,"dewpt_c":5.1,"humidity":80,"pressure_hpa":1009.8,"wind_kmh":7.2,"gust_kmh":11.3,
   "wind_dir":245,"rain_mm":0.30,"rain_daily_mm":1.2,"uv":0.4,"solar_wm2":96,"indoor_temp_c":21.5,
   "indoor_humidity":48,"wind_avg2m_kmh":6.8,"rain_rate_mmh":0.0,"wind_avg10m_kmh":6.1},
  "age":{"temp_c":0,"dewpt_c":0,"humidity":0,"pressure_hpa":0,"wind_kmh":0,"gust_kmh":0,"wind_dir":0,
   "rain_mm":0,"rain_daily_mm":0,"uv":0,"solar_wm2":0,"indoor_temp_c":0,"indoor_humidity":0,
   "wind_avg2m_kmh":0,"rain_rate_mmh":0,"wind_avg10m_kmh":0},
  "last_rejected":[],"rejects_since_boot":{"wind_kmh":2},
  "rej":{"parse":0,"bounds":0,"profile_wind":2,"profile_humidity":0,"limits":0,"jump":0,"rain_glitch":0},
  "dropped":0,"promoted":{},"extras":{},"known":[],"extras_dropped":0,"id_sanitized":false}}
```


#### `GET /api/live` — all stations

No ETag. Same head (`now`, `time_synced`, `time_src`, `page`, LAN: `uptime_s`, `diag`), then `"stations":[...]` with one
station object each (same shape as above). With no station yet: `"stations":[]`; the dashboard uses this to show
the bridge details before the first upload.

#### 3.2.1 `diag` (LAN face only, internal)

Bridge diagnostics, for troubleshooting. **Not a stable interface** (§7). The **release** keys, in the
order they are sent:
`ap_clients`, `sta_up`, `sta_rssi`, `mqtt_up`, `heap_free`, `heap_min`, `heap_largest`, `uploads`, `queue_drops`,
`other_hits`, `uploads_dropped_lan`, `uploads_dropped_key`, `uploads_dropped_blocked`, `uploads_too_long`,
`uploads_no_slot`, `uploads_rate_limited`, `uploads_duplicate`, `stale_longest_run`, `stale_on_count`,
`store_fail`, `store_drop_old`, `store_cut`, `store_fail_append`, `store_future_dropped`, `clock_held`, `clock_held_n`,
`clock_ref`, `clock_cut_n`, `store_future_kept`, `store_retry`, `http_stream_abort`, `mqtt_init_fail`, `snap_from`,
`hist_back` (array of unsigned counts, one element per station slot of the build: 1 element on the WROOM-32 build, 2 on
the PSRAM profile; late readings that joined the open hour or were kept out), `dns_answered`, `ntp_answered`,
`ntp_unanswered`, `mqtt_published`, `mqtt_failed`, `fwd_ok`, `fwd_fail`, `fwd_attempt_fail`, `fwd_skip`, `fwd_drop`,
`heap_low`, `heap_min_10m`, `http_requests`, `http_304`, `capture` (true while the bridge listens to a new console,
§3.3.9: starting or on),
`heap_min8_t`, `heap_min8_ctx`, `heap_min8_socks` (the second in which `heap_min` was reached — uptime seconds,
then the names of what was busy in that second, in this order: `http` (a page request ended or was being handled),
`accept` (httpd accepted a socket: a client connecting), `fwd` (a forwarding send), `mqtt` (a publish), `store` (history
writes due), `learn` (a learner pass), `ota` (a firmware write), `upload` (a console upload), `capture` (listening to a
new console); then the open httpd sockets
at that moment. `null`, `[]`, `null` until the first sample; an empty list means none of these ran),
`http_aborted`, `http_purged`, `http_shed`, `http_paced` (page answers that first waited, up to 200 ms, for earlier
answers to be acknowledged while the byte heap was below 24 KB; one that still found it low is shed and counted in
`http_shed` too), `sta_ip`, `fw_version` (e.g. `"fauxcloud 1.0.0"`; a diagnostic image says `"fauxcloud 1.0.0+diag"`),
`hostname`, `stations_allocated`, `stations_used`, `mode` (`run`),
`admin_lock` (`{"active":bool,"until_s":<seconds remaining>,"failures":int}`; `until_s` is a duration, not a timestamp),
`store_ms_max` (-1 = no flash store), `build` (`"<version> <date> <time>"`), `page` (the admin shell's version, as
`page` above is the dashboard's: the admin page reloads itself once when it differs from its own), `ota_state` (`stable | pending | rolled_back`),
`ota_slot`, `last_reset`, `reset_raw`, `restarts`, `sta_reinits`, `mqtt_reconnects`, `last_crash` (`null` or
`{"reason":"…","task":"…","pc":"0x400d1234"}`, all strings).
The heap figures (`heap_free`, `heap_min`, `heap_largest`, `heap_min_10m`, `heap_low`) are the
byte-addressable heap (`MALLOC_CAP_8BIT`). `heap_low` is `true` while the minimum free heap of the last 3 minutes is
below 18 KB (the same flag drives the `heap_low` notice and the LED's attention pattern); `heap_min_10m` is the minimum of
the last 10 minutes. The R&D keys below are not in the release (absent, not `null`).

**Bench build (`FC_DIAG_BENCH=1`, the published `fauxcloud-<ver>-diag.bin`; see `firmware/README.md`).** It adds, absent
from the release in every body:
- `/api/live` `diag`: `fwd_heap_min`, `fwd_largest_min`, `fwd_send_ms_max` (the forwarding sends' byte-heap low-water, its
  largest block and the longest send, after `fwd_drop`); `ota_heap` (`null` or `{"start8","min8","largest_min8"}`: the byte
  heap the last firmware write had, from RTC memory, shown on the boot after the write; after `http_304`); `tasks`
  (`{"loop","httpd","mqtt","udp53_123","ota","netout"}`, each the bytes of stack its task has never used since it
  started, from one FreeRTOS snapshot a minute; a task that did not exist at that snapshot is absent); `http_busy_ms_max`
  (the longest request handler in the last 10 minutes).
- the forwarding GET (§3.3.7): `heap{send_min,largest_min,send_ms_max,tick_lock_us_max,tick_own_us_max}` next to the
  release's `heap{tick_us_max}`, `tick_parts{slow,worst_job,worst_t,max{…9},worst{…9},start_max{…5},start_worst{…5}}`, and
  per target `last.diag{stack_free,curve,rec_in_max}`, and `wufc.diag{rec_in_max,rec_app_max,min8,ms}` in the forecast block.
- the METAR admin GET (§3.3.8): `http_idle_hist`, `last{http_idle_ms,http_during}`, `last.diag{stack_free,curve,rec_in_max}`.
- `/api/learn` (§3.2.4): `backfill.ms`, `backfill.replay{hours,ms,learners}`, `gates{…20}`.

#### 3.2.2 The `outlook` block (`station.outlook`)

A short local outlook computed from this station's own readings (plus, with the airport module on, what the main
airport reports nearby, §3.2.3 "Nearby report"), as **codes and parameters** (no sentences; the
dashboard words them, MQTT carries an English sentence, §5.2). It sits in the station object after `notices` and
before `warming`. All values are metric. `t` is the station's **last upload** (not the current time), and the block
changes only with an upload, an hour close, a calibration-log change, the bridge's location, the clock becoming
real or the main airport's admitted report changing its phenomena or source; the live ETag (§1.5) covers each of
these (and the REL hint's run, §6.4 `baro_ref`) and the learners' snapshot (a learner tick between uploads), so a 304 does not hide a changed outlook or notice.

Key order as written:

| Key | Type | Meaning |
|---|---|---|
| `v` | int | block version, `1` |
| `t` | ISO or null | the station's last upload (the instant the outlook is computed for) |
| `as_of` | ISO or null | end of the newest closed hour used (the data basis); `null` before any closed hour |
| `gate` | string or null | why the outlook is held back (below); `null` = not gated |
| `state` | string or null | what it is like now |
| `next` | string or null | what comes next, or why there is no line |
| `warn` | string or null | at most one warning |
| `qual` | string or null | line 1's qualifier, said after `state` (a temperature band or calm); `null` = none |
| `conf` | string | `"default"`; `"learned"` when a learner in use was used for a code (`in.learn` names it; "used": the fixed rule may give the same code) |
| `tend` | object or null | the 3-h pressure tendency behind `next` (below) |
| `left_h` | int | with `next` = `collecting`: hours of history still missing; else `0` |
| `p` | object | parameters for the sentences: `gust` (km/h, 1 dec, with `warn` = `gusts`, else `null`), `since` (ISO or null: with a rain `state`, the start of the newest run of wet hours, else `null`); only with `next` = `min_range`: `min_lo`, `min_hi` (whole °C, tonight's minimum range) |
| `in` | object | the inputs behind the lines (the page's "why" sheet), below |

`tend`: `{"d":hPa,"raw":hPa,"tide":bool,"code":"rising|steady|falling","fast":bool}`. `d` = the 3-h change after
removing the semidiurnal pressure tide, 1 decimal; `raw` = before the tide, 2 decimals; `tide` = false when the bridge
has no location (no correction); `fast` = the change is a quick one. `null` when no tendency can be given.

`in`: `night` (bool or null = unknown), `night_src` (`sun` = from the bridge location, `solar` = from the solar
sensor, `none`), `t`, `td` (°C, live, 1 dec), `rate` (mm/h, 1 dec: the live `rain_rate_mmh`, else the daily counter's
rise over the last 10 min times 6), `gust10` (km/h, 1 dec: the 10-min gust, else the last gust), `wind_h` (km/h, 1
dec: mean wind of the newest closed hour), `calm` (bool: that hour was calm), `dt_h` (°C, 1 dec: its mean temperature
minus the hour before), `rain12` (mm, 2 dec: rain in the last 12 h incl. the open hour), `rh` (%, 0 dec: live
humidity), `wind10` (km/h, 1 dec: the mean wind of the last 10 min), `cool_at` (ISO or null: the start of the
clear-night cooling hour), `near` (sent last: the source of a shown nearby-report code, else `null`:
`{"icao":"EGLL","name":"Heathrow","km":18,"age_s":1500,"auto":false}` -- `name` at most 24 bytes, `km` from the bridge's
location, `age_s` the report's age at `t`, `auto` an automatic report). A number is `null` when unknown. `rh` and `wind10`
feed the `humid` / `windy` qualifiers (§3.2.3).
Only when a learner in use was used for a code (absent otherwise), after `near`: `learn` (array of `l1`, `l2`, `l5`, `l6`), `cool_thr` (only when L5's value was used; °C/h, 2 dec: L5's learned
clear-night cooling), `rh_sat`, `rh_rel` (only with `fog_possible`; %, 1 dec: L2's humidity at saturation and the newest hour's humidity
relative to it).

Code lists (English, append-only):

| Key | Codes |
|---|---|
| `state` | `dry`, `night_clear`, `rain_light`, `rain_moderate`, `rain_heavy`, `recent_rain`; nearby report: `wx_snow_near`, `wx_rain_near`, `wx_drizzle_near`; `day_sunny`, `day_spells`, `day_nosun` (L1 in use); `sleet_snow` (a rain state at or below `SLEET_T`) |
| `next` | `p_steady`, `p_rising`, `p_rising_fast`, `p_falling`, `p_falling_fast`, `cooling_fast`, `collecting`, `p_paused`; `min_range` (L6 in use) |
| `warn` | `icy`, `gusts`, `hoar_frost`, `ground_frost`; nearby report: `thunder_near`, `fog_near`, `mist_near`; `fog_possible` (L2 in use) |
| `qual` | `severe_frost`, `freezing`, `hot`, `windy`, `humid`, `calm` (all six emitted; never with a nearby-report `state`) |
| `gate` | `time`, `stale` |
| `conf` | `default`, `learned` |

**Absent and gated.**

| Case | What the client sees |
|---|---|
| No barometer seen (the station never sent a pressure, in this run or in the last saved reading) | the whole `outlook` key is **omitted** |
| `gate: "time"` | there is no valid clock: only a persisted time (the last one before a restart) does not count, SNTP, DHCP NTP or a browser seed do. `state`, `next`, `warn`, `qual`, `tend` are `null`, `left_h` 0. |
| `gate: "stale"` | the outdoor sensor is not updating (`sensor_stale`): `state`, `warn` and `qual` are `null`, `p.gust`, `p.since` and `in.near` are `null`. `next` is kept: the console's barometer still reports. |

**Compatibility.** Codes are append-only: a later stage may add codes (and inputs) without a shape change. A client
must treat an **unknown code as absent** (show nothing for that line), and ignore unknown keys.

Example (a quiet day, LAN face, the rest of the station object left out):

```json
"outlook":{"v":1,"t":"2026-11-05T10:15:00Z","as_of":"2026-11-05T10:00:00Z","gate":null,"state":"dry","next":"p_steady",
 "warn":null,"qual":null,"conf":"default","tend":{"d":0.4,"raw":0.52,"tide":true,"code":"steady","fast":false},"left_h":0,
 "p":{"gust":null,"since":null},
 "in":{"night":false,"night_src":"sun","t":8.4,"td":5.1,"rate":0.0,"gust10":11.3,"wind_h":6.1,"calm":false,
  "dt_h":0.3,"rain12":0.00,"rh":78,"wind10":5.2,"cool_at":null,"near":null}}
```

#### 3.2.3 Outlook rules

**Qualifiers** (one slot on line 1, first that fires: `severe_frost` > `freezing` > `hot` > `windy` > `humid` >
`calm`). A band enters on the newest closed hour's mean and holds on the live value with a margin (no memory in the
rule): `hot` live >= 30 °C and the hour's mean >= 30; `windy` the hour's mean wind >= 20 km/h and the last 10 min's mean
>= 16; `humid` by day only, never with a rain state or `recent_rain`, the hour's mean humidity >= 90 % and live >= 88 %.

**Nearby report: the main airport's report.** With the airport module on (`FWD_TLS` build), the main airport (the
first in the list, never a fall-back to the second) is **admitted** when its report is 0 .. 75 min old at `t`,
the bridge has a location and the airport is at most 60 km from it (by the stored coordinates). Its
groups map to: snow (`SN`, `SG`, `BLSN`, `RASN`) > rain (`RA`, `SHRA`, `TSRA`, `FZRA`, `RADZ`) > drizzle (`DZ`, `FZDZ`)
for `state`; thunder (`TS`), fog (`FG`, `MIFG`, `BCFG`, `FZFG`) > mist (`BR`) for `warn`; `PL`, `GR`, `IC`, `VC..` and
`RE..` alone map to nothing. **Local first:** a nearby-report state replaces only `dry`, the gauge-blind `null` and
`day_nosun` (our rain and `sleet_snow`, `recent_rain`, `night_clear` and `day_sunny` / `day_spells` keep the slot); a
nearby-report warning takes the one warning line by rank: `icy` > `thunder_near` > `gusts` > `hoar_frost` > `ground_frost` >
`fog_near` > `fog_possible` > `mist_near` (a `fog_near` that displaces `fog_possible` leaves no `rh_sat` / `rh_rel`).
A nearby-report state carries no qualifier. The stale gate
clears them like every other line. The airport's forecast (TAF) never enters these rules.

Pure function over the **calibrated hourly rows** (the same rows as `/api/hourly`, calibration log applied; the newest
18 closed hours) and the live values of the last upload. No amounts, nothing is stored;
a learner in use (`/api/learn`'s `use`) may move a threshold or add its code (`day_*`, `min_range`, `fog_possible`). The thresholds are named defaults; a later learner replaces a
default, it never edits one.

| Constant | Value | Used for |
|---|---|---|
| `TEND_THR_HPA` | 1.6 hPa / 3 h | a rise or fall (below: steady) |
| `TEND_FAST_HPA` | 3.6 hPa / 3 h | "quickly" (only when not steady) |
| `TEND_FRESH_S` | 7200 s | freshness: the newest pressure hour must start at most 2 h before the upload |
| `TEND_HOURS` | 4 | a 3-h window = 4 consecutive hourly means |
| `RAIN_MODERATE` | 2.5 mm/h | `rain_light` below, `rain_moderate` from here |
| `RAIN_HEAVY` | 7.6 mm/h | `rain_heavy` above |
| `SOLAR_NIGHT_WM2` | 5 W/m² | dark by the solar sensor (when there is no location) |
| `COOL_DT` | -1.0 °C / h | a clear-sky cooling hour |
| `WARM_DT` | +0.5 °C / h | a later rise ends "clear night" (silence, never "cloud") |
| `CLEAR_WIND_KMH` | 5 km/h | light wind, for the cooling and the frost rules |
| `ICY_T` | 1.0 °C | icy: live temperature at or below |
| `ICY_RAIN12_MM` | 0.2 mm | icy: rain in the last 12 h at least (one tip of the verified unit's bucket is about 0.225 mm), or it is raining now |
| `GUST_WARN_KMH` | 30 km/h | gusts: the 10-min gust (else the last gust) at or above |
| `HOAR_SPREAD` | 2.0 °C | hoar frost: `t - 2 <= td` and `td` at or below 0 °C (grass about 2 °C below the air) |
| `FROST_T` | 2.0 °C | ground frost: air temperature at or below (grass about 0 °C) |
| `WINDOW_H` | 18 h | closed hours read |
| `FREEZING_T` | 0.0 °C | `qual` = `freezing`: live temperature (as shown, 0.1 °C) at or below |
| `SEVERE_FROST_T` | -10.0 °C | `qual` = `severe_frost`: at or below |
| `HOT_T` | 30.0 °C | `qual` = `hot`: at or above |
| `WINDY_KMH` | 20 km/h | `qual` = `windy`: the newest closed hour's mean wind at or above |
| `WINDY_LIVE_KMH` | 16 km/h | `windy`: and the last 10 min's mean wind at or above |
| `HUMID_RH` | 90 % | `qual` = `humid`: the newest closed hour's mean humidity at or above (by day, never with a rain state) |
| `HUMID_LIVE_RH` | 88 % | `humid`: and the live humidity at or above |
| `FOG_ENTER_PCT` | 98 % | `fog_possible` (L2 in use): the newest closed hour's humidity as a share of the learned saturation, at or above |
| `FOG_STAY_PCT` | 97 % | `fog_possible`: and the live value, at or above |
| `FOG_DRY_H` | 3 h | `fog_possible`: no rain tip in the last 3 closed hours |
| `SKY_SUN_K` | 0.85 | `day_sunny` (L1 in use): the newest closed hour's solar against the learned clear-sky level, at or above (steady) |
| `SKY_SPELLS_K` | 0.30 | `day_spells` above, `day_nosun` at or below |
| `SKY_NOSUN_KMAX` | 0.50 | `day_nosun`: the hour's maximum against the clear-sky level below |
| `SKY_EXIT_K` | 0.50 | live solar against the clear-sky level: under it `day_sunny` ends, over it `day_nosun` ends |
| `SKY_STEADY_PCT` | 115 % | `day_sunny`: the hour's maximum over its average at most this share of the clear sky's |
| `BLIND_T` | 2.0 °C (= `FROST_T`) | no `dry` state at or below (the unheated gauge may not see snow or sleet) |
| `SLEET_T` | 1.0 °C | a rain state reads `sleet_snow` at or below (live temperature as shown, 0.1 °C; no hysteresis, like the frost bands: the dashboard's sky draws snow at the same value) |

**`state`** (first that applies): rain (the live rate above 0 → `rain_light|moderate|heavy` by the rate; at or
below `SLEET_T` → `sleet_snow` "Sleet or snow." for every rate, `since` as for rain; it keeps the slot over a nearby-report
`wx_snow_near` as rain does, and counts as a rain state for `humid` and the frost warnings' clear-night gate) >
`recent_rain` (no rain falling, but the open hour or the closed hour right before it measured rain; stored rows only,
so it holds across a restart) > clear night (it is night, an earlier hour since sunset cooled by `COOL_DT` with wind
below `CLEAR_WIND_KMH`, and no rise of `WARM_DT`, no rain since) > `dry`, which is said only above `BLIND_T` (or when there is no live temperature); at or
below it the state is `null`. Night comes from the sun at the bridge's location, else from the solar sensor, else it is
unknown and the night rule is off.

**`next`** (first that applies): `collecting` (fewer than 4 consecutive pressure hours: `left_h` = hours missing) >
`p_paused` (any of those 4 hours is flagged pressure-not-calibrated or approximate clock) > the
tendency (`p_rising`, `p_falling`, with `_fast` from `TEND_FAST_HPA`) > `cooling_fast` (steady pressure, night, the
newest hour cooled by `COOL_DT`) > `p_steady`. With no fresh tendency (an end hour missing, or the newest hour older
than 2 h) there is **no** `next` line (`null`).

The tendency is the dashboard's (the same test vectors): newest closed hourly mean against the one
3 h before, minus the semidiurnal tide of `1.16 cos³(lat)` hPa at mid-hour when the bridge has a location, rounded
to 0.1 hPa half away from zero; at ±1.6 hPa rising or falling, from 3.6 hPa fast.

**`warn`** (one at most, by priority, the single chain with the nearby-report codes): `icy` > `thunder_near` > `gusts` >
`hoar_frost` > `ground_frost` > `fog_near` > `fog_possible` (an inference never displaces a safety warning) >
`mist_near`. The two
frost warnings need a clear night with light wind in the newest closed hour; always by the fixed `COOL_DT`
rule, whatever L5 learned (a learned value moves `night_clear` and `cooling_fast` only).

**The learners in use** (§3.2.4 `use`; each only at its use level, else the fixed rule): L5 replaces `COOL_DT` in
the clear-night and `cooling_fast` tests; L2: `fog_possible` "Mist or fog possible." when the newest closed hour's
humidity is at least 98 % of the learned saturation value with wind under 5 km/h, no rain state and no tip in the
last 3 hours, held while the live value is at least 97 %; L1: `day_sunny` "Sunny.", `day_spells` "Sunny spells.",
`day_nosun` "No sun." by day from the newest closed hour against the learned clear-sky level (ranked after rain and
recent rain); L6: `min_range` "Minimum around {range} tonight." on a clear night before the night's end, replacing only `p_steady` or `cooling_fast`, never a pressure tendency (HA: "Minimum around −9 to −5 °C tonight."). With no learner in use the fixed rules alone decide.

**`qual`** (one, first that applies): `severe_frost` (live air temperature, calibrated and rounded to 0.1 °C as
shown, at or below -10.0) > `freezing` (at or below 0.0) > `hot` (at or above 30.0) > `windy` > `humid` (all six are emitted, §3.2.3)
> `calm` (the newest closed hour was calm); else `null`. Not a warning; `null` when stale or gated. Line 1 = the state
sentence + the qualifier sentence: every state that can carry a qualifier is at most 14 characters (the nearby-report states are longer and never carry one) and every qualifier at most 13, so line 1 fits
28 (a host test checks every pair). The sensor can read high in direct sun with light wind (the page says so for `hot`).

#### 3.2.4 `GET /api/learn[?station=<id>]` — learner state (LAN, not part of the public contract)

The self-calibrating learners' state for one station (default: the first), read by the admin "Learning" sheet. **Not part
of the public contract** (§7): keys and units may change in any build. A learner graduates at its use level (L1 medium per sun
height, L2 low, L5 medium in both classes, L6 high; L3, L4 information only; L7 a notice) and then feeds the outlook
(`in.learn`); below it the fixed rules decide. `Cache-Control: no-store`, no ETag; a few KB. Keys are only added (`v` stays 1). An unknown id
(or no station yet) answers 404 `{"error":"no such station"}`, and so does a station whose learner state is not loaded yet.

| Key | Meaning |
|---|---|
| `v` | 1 |
| `state` / `why` | `learning`, `backfill`, `paused` or `read_only` (a newer image wrote the blobs); `why` null or a reason code, the first that applies: `blob_newer` (`read_only`), `no_store` (no flash store), `no_location` |
| `epoch`, `relearn` | sensor epoch; `true` for 14 days after an epoch change or a "Restart learning" (learning rate × 4) |
| `need`, `state` | in the learner blocks (`l1` per cell, `l2` `wet` / `night` and its own `state`, `l3`, `l4`, `l5` per class and its own `state`, `l7`): `need` = real samples still missing to the next confidence level (0 at high); `state` = `default` (below the first bar), `learned`, `in_use` (the outlook reads it), `info` (L3, L4, L7 past their first bar) |
| `relearn_hour`, `bars` | (after `l6`) the hour of the last "Restart learning" (0 = none; a replay re-applies it there; only the last press is kept, so a replay over two presses re-applies the second only); `bars` = the one table of real samples to one / two / three bars: `l1`, `l2_wet`, `l2_night`, `l3`, `l4`, `l5` (per class), `l6` (nights), `l7` (days: stage 1, stage 2) |
| `use` | the station's snapshot of what the outlook may read (calibrated): `hour`, `l1`/`l2`/`l5`/`l6` (bool, in use), `sky_ref`, `sky_ref_max` (W/m²), `sky_cls` (0 unknown, 1 sun, 2 shade, 255 none), `rh_sat` (%), `cool` (°C/h), `min_lo`, `min_hi` (°C), `night_end` (unix), `metar` (METAR eligible = airport weather on, the location set and at least one chosen airport within 60 km; then `baro_ref` owns pressure and L7's `baro_check` and the sheet's flag sentence are silent) |
| `last_hour`, `last_day` | the newest hour / day fed: `last_hour` unix seconds (hour start), `last_day` `YYYYMMDD` (e.g. `20261111`), 0 = none |
| `migrated_from` | the blob version the core was migrated from at this boot's load (`0` = none) |
| `backfill` | `{"hours":n,"days":n,"done":bool}` (`ms` and `replay{}` only in the bench build, §3.2.1): the catch-up from the flash ring at boot; `days` = days fed, `ms` = its wall time including the loop's other work (RAM only; still counting while the walk runs, 0 without a flash store). On the first boot with no stored learner state it is the full-ring walk, later the catch-up since the last write. |
| `written`, `writes`, `nvs` | the last blob write (unix), writes since boot, `nvs_get_stats` (`used`, `free_entries`) |
| `gates` | **bench build only**: per-gate rejection counts since boot: `quality, readings, physics, frozen, indoor, geo, dome, iced, stuck, steady, k_range, k_low, cap, location, baro, scratch, l5_label, l5_evening, l6_night, melt` (`scratch`: a sky-cell visit dropped because the day's list was full; `l5_label` an evening that could not be classed, `l5_evening` an evening window with a bad hour, `l6_night` a night not admitted to L6; `melt`: a wet hour at or below 2 °C or within 24 h of icy rain, kept out of L2's wet estimate) |
| `l1` | clear-sky envelope: `def`/`lo`/`hi` and 12 `cells` (`el` 5/10/15/20/30/45°, `pm`) with `e` (the site's clear-sun fraction of the clear-sky model), `n` (effective samples), `conf` (`default, low, medium, high`), `last` (day), `hit`, `need`, `state`, `from` (an empty cell seeded by its neighbour one band up or down: that band's `el`, else null) |
| `l1b` | horizon mask: class counts `sun`, `shade`, `unknown` (949 cells), `today`, `clear_today`, `mask` = base64 of 2 bits per cell (0 unknown, 1 sun, 2 shade), 4 cells per byte |
| `l2` | humidity at saturation: `wet` and `night` estimates (`e` %RH, `n`, `conf`, `need`, `state`; `wet.hit`), `s` = the value in use, `src` (`night` (≥ 92 %, at its first bar) first, then `wet`, else `prior`), `stuck`, `state` |
| `l3` | the smallest wind the console reports: `e` km/h (P5 of the light-wind readings: the console's step, not the cups' start), `n`, `conf`, `hit`, `need`, `state` |
| `l4` | the site's usual wind: `p50`, `p90` km/h of the hourly mean wind, `n` (hours), `conf`, `hit` |
| `l5` | clear-evening cooling rate: `clear` and `overcast` = `{e,n,conf}` (`e` °C/h cooling, 2 dec, positive = cooling; `n` effective evenings), `thr` (°C/h, the threshold in use), `src` (`default, learned, inseparable`), `hit` (at a bound, as below) |
| `l6` | frost relation: `a`, `b`, `c` (the fitted terms, 2/3/3 dec), `n` (effective nights), `sd`, `mae` (°C, 1 dec; `null` below 3 nights), `conf`, `learned` (bool), `hit` |
| `l7` | pressure check: `e` = the running mean of the console's daily mean pressure (raw, half-life 30 days), hPa (null before the first day), `n` (effective days), `days` (the admitted days behind `n`; 61 = over 60), `stage` (0; 1 from 14 days: flags ≥ 20 hPa; 2 from 51 days: ≥ 12 hPa; a flag holds until under 9 hPa), `flag` (`none, low, high`; re-judged as the `baro_check` notice: with the station's pressure offset in force, held while ≥ 9 hPa, `none` under `baro_uncalibrated`), `uncal` (the console's pressure at its factory value), `off` (hPa: `e` plus the station's offset in force minus the normal, null without one), `norm` (the built-in long-term sea-level pressure normal here, 15 days back; null without a table, a location, beyond 70°, or over high ground: a cell of mean surface height over 1 000 m), `need`, `state` |
| `hit` | the learner's last step was clamped at a bound of its range ("at a bound"); never true before its first sample. RAM only: false again after a restart until the next clamped step |
| `gate_state` | `indoor`, `frozen_run`, `geo_days`, `dome`, `iced` |
| `night` | `open` / `last`: the night summary in progress and the last closed one (each null before the first); `last_admitted` (bool, present with `last`: the night counted for the learners). Both carry the keys (`start`, `len_h`, `hours`, `ok`, `calm`, `rain`, `hoar`, `restart`, `rh_max`, `t2`, `td2`, `t0`, `dt3`, `tmin`) and `l_h` (night length, h, 1 dec), `w5` (calm-ish hours) and `rise` (bool: a rise of 1.5 °C or more before sunrise). `open` adds `e_dt3` (the evening's 3-hour change, °C, or null) and `e_ok` (bool, the evening window had no bad hour); `last` adds `l6` (why the night did or did not feed L6: `ok, rain, wind, rise, quality, not_clear, evening, missing`) and `pred` (°C, the minimum predicted before the night, or null) |
| `evening` | `{"label":"clear\|overcast\|mixed\|nocell\|none"\|null,"label_day":<unix day>\|null}`: the class of the last evening and its day |

The state lives in flash per station (a 256-B core blob and a 961-B mask blob), written every 6 h, before a
planned restart, after a reset event and at the end of the backfill; both are removed by Forget and by a factory reset.
Each blob is judged on its own: a mask learned at the bridge's current place is kept even when the core is
missing or older, and a blob longer than this firmware's (a newer image's) makes the station read-only instead of being
overwritten.

#### `GET /api/bridge/status`

Small (≤ ~300 B), polled every 60 s by the dashboard. ETag + 304; the ETag changes whenever any value below
changes.

| Key | Meaning |
|---|---|
| `mode` | `run` |
| `sta` | 1 = joined to the home Wi-Fi |
| `mqtt` | -1 no broker configured, 0 down, 1 connected |
| `time` | 1 = real time (SNTP, DHCP NTP or browser seed) that is not held; 0 while the clock is more than 1 h behind the stored history |
| `lock` | 1 = admin login locked account-wide |
| `heap_low` | 1 = minimum free heap in the last 3 minutes below 18 KB of byte-addressable heap |
| `time_src` | `none | persisted | browser | dhcp | sntp` |
| `notices` | bridge notices (§6.4, same item shape as `station.notices`), present only when there are any |
| `ota` | present (1) while a firmware upload is written |
| `metar` | present (1) while airport weather is on with an airport chosen (the dashboard's plane tab); never in a build without TLS |
| `store_fail`, `too_long`, `no_slot` | counts since boot, present only when > 0 |
| `no_slot_id` | the id of the last console turned away for lack of a station slot; present only within an hour of its last try, and gone when a Forget frees a slot (RAM only) |
| `ap` | fauxcloud's run-mode Wi-Fi name, present only while there is no station (for the pairing card) |
| `ids` | station ids, slot order |
| `st` | one character per id: `o` online, `s` stale, `x` publishing off, `u` unconfigured |

```json
{"mode":"run","sta":1,"mqtt":1,"time":1,"lock":0,"heap_low":0,"time_src":"sntp","ids":["EXAMPLE1"],"st":"o"}
```

#### `GET /api/recent?station=<id>`

The last 60 readings of the 11 core fields (§6.1), oldest first. `station` optional (default: the first station).
ETag + 304.

```json
{"station":"EXAMPLE1",
 "fields":["t","temp_c","dewpt_c","humidity","pressure_hpa","wind_kmh","gust_kmh","wind_dir","rain_mm","rain_daily_mm","uv","solar_wm2"],
 "rows":[[1762337635,8.3,5.1,80,1009.8,6.8,10.4,240,0.00,1.2,0.4,94],
         [1762337700,8.4,5.1,80,1009.8,7.2,11.3,245,0.00,1.2,0.4,96]]}
```

`t` = unix seconds (seconds since boot before the clock is set). `null` = no value in that reading. For a
`yt60234` station, `rain_mm` in a row is the rain that fell **since the previous reading** (a delta of the daily
counter). Hidden fields are **omitted** (the column and its values), as in `/api/live`; the hourly and daily rows
leave out a hidden field's keys too.

**Calibration on read.** Hourly and daily rows (`/api/hourly`, `/api/daily`, from RAM or flash, and so the
dashboard's CSV export) are stored **raw**; the station's calibration log is applied when a row is rendered:

| Field | Applied |
|---|---|
| `temp_c` min/avg/max | + `temp_c_offset` |
| `humidity` | + `humidity_offset`, clamped 0..100 |
| `pressure_hpa` | + `pressure_hpa_offset` |
| `wind_dir` | + `wind_dir_offset`, wrapped (`null` stays `null`) |
| `wind_kmh`, `gust_kmh` | × `wind_factor` |
| `rain_mm`, `rain_daily_mm`, `rain_rate_max` | × `rain_factor` |
| dew point, UV, solar, extra channels | untouched |

The entry in force at the row's hour applies (daily: at the day's start, 00:00 of the date as UTC), so changing an
offset does not rewrite older rows. The daily `rose` is stored with the raw direction and rotated by whole 45°
sectors on read (granularity ≤ 22.5°: an offset under 22.5° does not move the rose). The log lives in flash (≤ 8 entries; the oldest drops and its rows then read with the next entry). It starts at the first boot
with the current calibration, gets an entry when a station PUT changes an offset or a factor, and is removed by
Forget and by a factory reset. The history ETags change when the log changes. Live values and MQTT were always
calibrated at upload and are unchanged.

#### `GET /api/hourly?station=<id>`

The open hour and up to 48 closed hours (oldest first). ETag + 304. `from=<unix s>` and `to=<unix s>` (either one, hour
starts in `[from, to)`) read the station's flash ring instead (up to about 131 days of hours; 88 days on the PSRAM profile's second ring); `open` is then `null`
unless the open hour falls inside the range. Without a flash store the range is ignored and the RAM view answers.

```json
{"station":"EXAMPLE1",
 "open":{"h":"2026-11-05T10:00Z","n":15,"q":[],"epoch":0,"rej_fields":0,"temp_c":[8.1,8.3,8.5],"dewpt_c":[null,5.0,null],
  "humidity":[79,80,81],"pressure_hpa":[null,1009.8,null],"wind_kmh":[null,6.1,null],"gust_kmh":14.8,
  "wind_dir":238,"wind_steady":0.62,"calm_n":0,"rain_mm":0.00,"rain_daily_mm":null,"rain_rate_max":0.0,
  "uv":[null,null,0.4],"solar_wm2":[null,81,96]},
 "hours":[{"h":"2026-11-05T09:00Z","n":56,"rej_fields":1,"...":"..."}]}
```

Triples are always `[min, avg, max]`, with `null` for a slot the record does not keep (a key whose three slots are all
missing is `null`). Hourly rows keep: `temp_c` and
`humidity` all three; `dewpt_c`, `pressure_hpa` and `wind_kmh` the **average only** (`[null,avg,null]`); `uv` the
**maximum only** (`[null,null,max]`); `solar_wm2` average and maximum (`[null,avg,max]`). Single numbers: `gust_kmh`
(hour maximum), `wind_dir` (speed-weighted vector-mean direction, degrees; `null` = none), `rain_mm` (mm fallen in the
hour, the sum of the daily counter's deltas). **`rain_daily_mm` is always `null` in an hour row**: read `rain_mm` for
the hour's rain. Further keys: `n` = readings (at most 255 in an hour); `q` = quality flags, a list of names (`stale`, `baro_uncal`,
`clock_approx`, `restart`, `gap`, `console_reset`, `rejected`, `rain_partial`; the names `baro_boundary`,
`baro_bridge` and, in a day, `epoch_split` are reserved and never set), `[]` = a clean hour; `epoch` = reserved for a
sensor-replacement marker, always `0` in this firmware; `rej_fields` = 1 if a filter rejected at least one field in the hour
(0 or 1, not a count); `wind_steady` = steadiness of the direction, 0-1 (1 = one steady direction; `null` with
`wind_dir`); `calm_n` = readings with the wind and its 2-minute mean both under 1.0 km/h (hidden with the wind); `rain_rate_max` = the hour's peak rain rate,
mm/h. `h` = start of the hour, UTC. Hidden fields are omitted (see above). Channel keys: `indoor_temp_c`, `indoor_humidity` and the promoted sensors that took a free history channel (4 in all) appear as extra keys, `[null,avg,null]` in an hour row and `[min,avg,max]` of the hourly averages in a day row.

`raw=1` (hourly and daily, not recent): the rows as stored, without the calibration log. The ETag gains a `-r` suffix, so a raw and a calibrated body never share a tag. Internal: not part of
the public contract (§7).

#### `GET /api/daily?station=<id>[&days=N][&all=1]`

Closed local days and the open day. Same record shape as hourly, with `d` (`"YYYY-MM-DD"`, local date) instead of
`h`, and these differences:
- `dewpt_c` and `pressure_hpa` carry min, avg and max (`[min,avg,max]`; for these two the min and max are taken over the hours' averages); `wind_kmh` stays `[null,avg,null]`, `uv`
  `[null,null,max]`, `solar_wm2` `[null,avg,max]`.
- `rain_mm` and `rain_daily_mm` both hold the day's rain total (mm); `rain_rate_max` is the day's peak rate.
- `rose`: eight integers = hours with the wind from N, NE, E, SE, S, SW, W, NW.
- Counts: `hours` = hourly records in the day, `clean_hours` = of those, the hours with none of the flags `stale`
  ... `gap` (`q` bits 0-6), `calm_hours` = hours with a calm majority, or with wind readings but no direction; hours with no wind data are not counted (hidden with the wind),
  `rain_hours` = hours with rain (hidden with the rain).
- `q`, `epoch` and `wind_steady` as in an hour row; there is no `calm_n`.

| Query | Source |
|---|---|
| none | the RAM ring: up to 30 days |
| `days=N`, N ≤ 30 | the newest N days of the RAM ring |
| `days=N`, N > 30 | the newest N records of the station's flash ring (up to 736 days; the WROOM-32 keeps one ring; the PSRAM profile's second ring 368; N is capped at 736) |
| `all=1` | every closed day on flash (up to 736 days; 368 on the PSRAM profile's second ring); the open day is in `open`; used for the CSV export |
| `from=<unix s>`, `to=<unix s>` | the days on flash whose local midnight lies in `[from, to)`; `open` is `null` unless the open day falls inside |
| `raw=1` | combines with any of the above: rows without the calibration log (see hourly) |

Without a flash store, `from` / `to`, `all=1` and `days` above 30 are ignored and the RAM ring (up to 30 days) answers.
ETag + 304 in every form; the ETag of a flash view does not encode `from`, `to`, `days` or `all`, so key a cached copy by its full URL. The dashboard's CSV export of hour and day records follows the record: it writes only the
min / avg / max columns the record keeps (see the hourly and daily shapes), and a `wind direction (compass)` column beside
`wind_dir` (16 points; a day whose `wind_dir` is `null` takes the 8-point name of its `rose`'s leading sector, a rose tie
the earlier sector, and the degrees stay empty).

```json
{"station":"EXAMPLE1","open":{"d":"2026-11-05","n":610,"rej_fields":0,"temp_c":[4.2,7.0,9.1],"...":"...","rose":[0,0,1,0,2,5,2,0]},
 "days":[{"d":"2026-11-04","n":1310,"rej_fields":3,"temp_c":[3.8,6.5,10.2],"...":"...","rain_mm":1.40,"rain_daily_mm":1.4,"rose":[1,0,0,2,6,9,4,2]}]}
```

#### `GET /api/station?station=<id>`

Read-only config view: `{"station","configured","stored","effective":{...},"overrides":{...}}`. `effective` is the
full config, `overrides` only what differs from the station profile's defaults. Shape in §3.3.4. No key state (admin only).

#### `GET /api/bridge`

Read-only bridge config: `{"effective":{...},"overrides":{...}}` with `tz`, `tz_name`, `lat`, `lon`,
`auto_publish_new_stations`, `lan_uploads`, `max_stations`, `max_stations_cap`, `mqtt_interval_last_seen_s`, `forwarding`,
`log_verbose` (§3.3.5). The dashboard
reads `lat`/`lon` for its day/night sky.

```json
{"effective":{"tz":"CET-1CEST,M3.5.0,M10.5.0/3","tz_name":"Europe/Warsaw","lat":52.2300,"lon":21.0100,
  "auto_publish_new_stations":true,"lan_uploads":false,"max_stations":1,"max_stations_cap":1,"mqtt_interval_last_seen_s":300,
  "forwarding":false,"log_verbose":false},
 "overrides":{"tz":"CET-1CEST,M3.5.0,M10.5.0/3","tz_name":"Europe/Warsaw","lat":52.2300,"lon":21.0100}}
```

#### `GET /tools`

A server-rendered HTML page (auto-refresh 30 s): version, clock, each station's values, rejections, two sparklines
and the hourly table. On the LAN face it also shows a warning while the admin login is locked. For humans; not an API.

#### 3.2.5 `GET /api/metar` — airport weather (LAN face; not in the HTTP-only build `FWD_TLS=0`; reference data, not the station's)

Up to three airports the admin chose, with their last METAR from aviationweather.gov (RAM only: empty after a restart until
the first fetch) and the one reference reading. It reaches MQTT only as words: the
outlook's nearby-report codes with the HA `near` attribute (§3.2.3) and the `baro_ref` notice (§6.4); never as values, and never history,
the rings, the learners, forwarding or a sensor entity.
A build without TLS has no route: the path answers the catch-all `200 success` (§1.6), as do the airport admin routes (§3.3.8). ETag: the store's change counter, a signature of the saved settings (on, the
airports, their order and names) and a 15-minute clock bucket, so a poll gets a `304` while nothing changed and the ages / `stale`
flags never lag more than 15 minutes. The ETag does not follow the console's pressure, so `reference.station` can be up to 15 minutes old on a `304`.

```
{"on":bool,
 "airports":[{"icao":"KJFK","name":"New York/JF Kennedy Intl","cc":"US","fp":3,"km":12|null,   // cc, fp: absent when unknown
              "obs":"<ISO>"|null,"age_s":n|null,"stale":bool,"cur":bool,  // stale: no report, older than 90 min or dated in the future; cur: at most 75 min old
              "cor":bool,"auto":bool,"night":bool?,"qnh_hpa":1018.2|null,       // night: absent without a position
              "wind_dir":320|"vrb"|null,"wind_kmh":9.3|null,"gust_kmh":27.8|null,
              "vis_m":10000|null,"vis_ge":bool,"cavok":bool,
              "cover":"none|few|scattered|broken|overcast"|null,"ceil_ft":1500|null,"cb":bool,"low_conf":bool,
              "wx":{"precip","rain","drizzle","snow","freezing","thunder","fog","mist":bool,"int":-1|0|1},
              "t_c":7.0|null,"td_c":6.0|null,
              "taf":null|{"status":"ok|nil|cnl|absent","current":bool,"issued":"<ISO>"|null,"valid_to":"<ISO>"|null,
                          "to":"<ISO>"|null,"next12":[{"what":"thunder|snow|rain|fog|gusts","possible":bool,"from":"<ISO>","to":"<ISO>","cut":true?,"started":true?}]}}],
 "reference":null|{"qnh_hpa":1018.1|null,"qnh_from":[{"icao","km","age_s"}],"qnh_inconsistent":bool,
                   "local":{"icao","km","age_s"},"local_values":{"from":"KJFK", ...the visibility, cloud, weather and temperature keys as above}},
 "last":{"t":"<ISO>"|null,"result":"ok|nodata|fail|yield|limited|untrusted|too_large|no_memory|reject"|null},  // the last fetch
 "last_taf":{"t":"<ISO>"|null,"result":"<outcome>"|null}}  // t = the last ok / nodata TAF cycle, result = the last one
```

The `airports` come in the user's order (the admin list, ↑ / ↓; a new one goes in by distance): the first is the main
airport. `name` is the display name: aviationweather.gov's `site` text after its last `/`, without the words `Intl`, `Arpt`,
`Airport`, `International` (e.g. "London/Heathrow Intl" -> "Heathrow"), at most 24 bytes (a name cleared before a lookup found it reads as the ICAO code). `cc` is the airport's country (ISO 3166
alpha-2) from its ICAO prefix (three letters, else two, else one; one table for both pages), `fp` the
flag pack that holds its flag (`flags-<fp>.json`, the picker's region of that country); both absent for a prefix the table does
not know (the page shows no flag). `km` is the distance from the bridge's location when the airport was picked (`null` if no location was set then); it is not recomputed. An airport with no report yet carries only `icao`,
`name`, `cc`, `fp`, `km`, `obs: null`, `age_s: null`, `stale: true`, `cur: false` and its `taf`. `night`: the sun is down at the
airport's own stored position at the report's time (the dashboard's sunrise rule), so the airport sheet and
the airport view draw a clear sky as the moon or the sun for that airport, not for the station; absent for an airport with
no stored position or no report (the page then uses the station's day / night). `low_conf`: the cloud group was `NCD` / `NSC` from an automatic
station, or `CLR` (no cloud *detected* below 12 000 ft). `wx`: `fog` is `FG`, `mist` `BR`, `drizzle` `DZ`,
`rain` `RA` (also in showers and thunderstorms), `precip` any precipitation; `int` the strongest intensity (-1 light, 0 moderate or no precipitation, 1 heavy). `reference`:
the pressure is the mean of the current airports' QNH (`qnh_from`) weighted 3 / 2 / 1 by their place in the user's order
(one rule, also without a location); when they differ by more than 1.5 hPa, `qnh_inconsistent` is true and
the first current one alone is used; `local` is the first current airport in the user's order, the source of
`local_values` (never wind). `null` when no airport is current. Each airport carries `cur` (its report is at most
75 min old: the green dot; 75-90 min = shown but amber); in `reference` an airport counts while its report is at most 90 min old (`stale` false). `reference` carries `station`:
`{"p_hpa":1015.9,"qnh_hpa":1013.2,"diff_hpa":2.7,"target_hpa":1013.2|null,"active":bool}` -- the first online station's
pressure as shown, the QNH it is compared with, its difference from it, the REL value to type into the console (QNH minus
the station's pressure offset; `null` unless the newest report behind the QNH is `cur`, at most 75 min old) and whether the `baro_ref` notice
shows (§6.4). `reference.near_qnh_hpa` is that QNH on its own -- the current airports within 60 km only (the REL
basis), consistent; it can differ from `reference.qnh_hpa`, which weighs every current airport, by up to the 1.5-hPa spread;
`null` without a location, with airport weather off, or without such airports. `station` (and its comparison) uses `near_qnh_hpa`; it is `null` without
an online station pressure (or at the factory value) or without a `near_qnh_hpa`.

`taf`: the airport's forecast (TAF) from aviationweather.gov, fetched once an hour (the grid's :50 UTC cycle; the
:05 / :20 / :35 cycles fetch the reports). `null` until an hourly TAF cycle that answered (`ok` / `nodata`) asked for this airport
(the sheet: "Forecast comes with the next hourly fetch."); `status` `absent` = that answer had no TAF for it, `nil` / `cnl`
= no forecast issued / cancelled (`issued` and `valid_to` are `null` when the answer gave none; `to` is `null` unless `status` is `ok`). `current`:
`ok`, inside the validity, and fetched in the last 3 hours (the cycle is hourly; one missed cycle is tolerated) -- false
otherwise (the sheet then says "No forecast for this airport."). `to`: the forecast's horizon, the validity's end or 13 h
from the fetch hour, whichever is earlier (the 13 one-hour cells from the fetch hour cover the next 12 h; the airport view says
"Forecast to {to}:" when under 11 h are left). A failed, yielded (`yield`) or limited (`429`) cycle changes no forecast and
not `last_taf.t`; its outcome is in `last_taf.result`. `next12`: up to 8 windows of the next 12 hours, in the order thunder, snow,
rain, fog, gusts, then by time; whole hours clipped to now and to the validity; `possible` false = the forecast's main line
(the base, `FM`, a `BECMG` after its period), true = `TEMPO`, `PROB30/40`, `INTER` or a `BECMG` transition, shown only where
the same phenomenon is not also expected. `cut` (present only when true): the window runs to where the data stops (now + 12 h,
or the 13-h grid's end when the forecast is shown after a missed hourly cycle), so its `to` is no forecast end; the faces say
"from {from}" without an end time (a `started` one: "throughout"). `started` (present only when true): the window began before now, so its `from` is
now, no forecast start; the faces say "throughout" (also `cut`) or "until {to}". Significant groups only: rain (`RA DZ UP GR GS IC`, also showers and freezing
rain), snow (`SN SG PL`), thunder (`TS`), fog (`FG` or `BR` with the group's visibility under 1 000 m), gusts (a `G` of at
least 22 kt / 12 m/s / 40 km/h). Empty = nothing significant. Not kept: temperatures, clouds, wind direction, the visibility
as a number, PROB30 vs 40. RAM only, like the reports.

### 3.3 Admin API (run mode, LAN face only)

Every call needs `X-Fauxcloud: 1`; all but `challenge` and `login` need the session cookie. Request bodies are JSON
(`Content-Type` is not checked). Responses are `application/json`, `no-store`, no ETag (except the two downloads, the boot log and the crash data). For the PUT documents (settings, station, bridge) and the setup apply, a body that is not a JSON object is reported as `$: not a JSON object`; a syntax error in the middle as `$: malformed JSON, the rest of the document was ignored` (the keys before it still apply). The other POSTs answer with their own errors.

#### 3.3.1 Login and sessions

| Route | Body | Answer |
|---|---|---|
| `GET /api/admin/challenge` | — | `{"nonce":"<32 hex>","salt":"<32 hex>","iters":10000}` or `{"error":"rate","retry_ms":1000}` |
| `POST /api/admin/login` | `{"nonce","proof","t"}` | `{"ok":true,"applied":bool,"time_src":"..."}` + `Set-Cookie`; or `{"ok":false,"error":"wrong password"}` / `{"ok":false,"error":"locked","retry_s":N}` |
| `POST /api/admin/logout` | — | `{"ok":true}`, cookie cleared |
| `GET /api/admin/sessions` | — | `[{"id","ip","ua","created_s","last_s","current"}]` (`*_s` = seconds ago) |
| `POST /api/admin/sessions/revoke` | `{"id":"<8 hex>"}` | `{"ok":true}` or `{"ok":false,"errors":["id: no such session"]}` |
| `POST /api/admin/sessions/revoke-all` | — | `{"ok":true}`, every session ended, own cookie cleared |

`t` (optional) is the browser's `Date.now()` in ms; it seeds fauxcloud's clock after a valid proof, only while no
better time source has worked (`applied` says whether it did).

```json
[{"id":"3f9a12c0","ip":"192.168.1.10","ua":"Mozilla/5.0 (X11; Linux x86_64) ...","created_s":5400,"last_s":12,"current":true}]
```

#### 3.3.2 Device settings

`GET /api/admin/settings` →

```json
{"settings":{"sta_ssid":"HomeWiFi","sta_pass":"<set>","mqtt_host":"192.168.1.5","mqtt_port":1883,
  "mqtt_user":"fauxcloud","mqtt_pass":"<set>","ap_pass":"<set>","admin_password":"<set>",
  "hostname":"fauxcloud","setup_done":true},
 "wifi_change":"","ap_ssid":"fauxcloud-A1B2","sessions":1,"errors":[],"notes":[]}
```

Secrets are write-only: they show as `"<set>"` (or `""` when empty), never as values.

`PUT /api/admin/settings` — partial document; only the keys sent change. Answer: the same shape as GET, with
`errors` and `notes` filled.

| Key | Rule |
|---|---|
| `sta_ssid` | 1-32 bytes, no control characters |
| `sta_pass` | `""`/absent/`"<set>"` = unchanged; `null` = open network; else 8-63 bytes, no control characters. With a new `sta_ssid`, absent / `""` / `"<set>"` means an open network: the stored password is never tried on another network |
| `mqtt_host` | up to 63 bytes, no control characters; `""` = MQTT off |
| `mqtt_port` | 1-65535, default 1883 |
| `mqtt_user` | up to 63 bytes, no control characters; `null` or `""` clears |
| `mqtt_pass` | up to 63 bytes, no control characters; `""`/`"<set>"` = unchanged; `null` clears. A changed `mqtt_host` or `mqtt_port` without a new `mqtt_pass` in the same body clears the stored password: it is never sent to another broker |
| `ap_pass` | 8-63 bytes, no control characters; cannot be cleared. The consoles must be re-paired (a note says so). |
| `hostname` | `[a-z0-9-]`, 1-32, not starting or ending with `-` |

A bad key is reported in `errors` (`"key: reason"`, for example `key: invalid` for a bad string, `key: not a string`, `key: cannot be cleared` for a `null` on `sta_ssid`, `mqtt_host`, `ap_pass` or `hostname`, `mqtt_port: not a number` or `must be 1..65535`, and `nvs: not saved; applied until reboot`); the rest still applies. `admin_password` and `setup_done` are shown by GET but read-only: sending them gives `unknown or read-only key, ignored`. MQTT, AP and hostname changes are
saved and applied at once. A Wi-Fi change is **tried for up to 60 s** and stored only if it works; otherwise the
old network comes back. Poll `GET /api/admin/settings` and read `wifi_change`: `""`, `busy`, `ok`, or
`failed:<code>` with the code `auth` (wrong password), `no_ap` (network not found), `dhcp` (joined, no address) or `timeout`. `busy` starts about 1.5 s after the PUT answers, so wait for it before trusting an older `ok` or `failed`. The admin password is not a settings key (see `/api/admin/password`).

#### 3.3.3 Password and secrets

| Route | Body | Answer |
|---|---|---|
| `PUT /api/admin/password` | `{"nonce","proof","salt":"<32 hex>","h":"<64 hex>","iters":10000..200000}` | `{"ok":true,"errors":[]}`; every other session is ended. A malformed new credential (checked before the proof, so no nonce is used): `{"ok":false,"errors":["salt / h / iters: the new credential is malformed"]}`; flash write failed: `{"ok":false,"errors":["nvs: not saved"]}` (the new password then holds only until a restart) |
| `POST /api/admin/ap-pass/reveal` | `{"nonce","proof"}` | `{"ok":true,"ap_pass":"********"}` |
| `POST /api/admin/coredump` | `{"nonce","proof"}` | `application/octet-stream` attachment `fauxcloud-coredump.bin`, then the crash data is erased; or `{"error":"no crash data"}` |
| `DELETE /api/admin/coredump` | — | `{"ok":true}`, or `{"ok":false,"errors":["crash data: not cleared"]}` |

The proof is made with the **current** password (§4). A failed proof answers like a failed login and counts toward
the lockouts. The AP password is the only secret that can be read back. A crash dump is a RAM image that contains
the device's passwords: never share it; share the decoded backtrace.

#### 3.3.4 Stations

`GET /api/admin/station?station=<id>` (default: the first station):

```json
{"station":"EXAMPLE1","configured":true,"stored":true,"key":"bound","key_mismatch":0,
 "src_mac":"02:00:00:00:00:01","fp":"match","cal_n":2,"learn_use":2,
 "effective":{"name":"Garden","profile":"yt60234","publish":true,"stale_s":240,"limits":{},"jumps":{},
  "calib":{"temp_c_offset":0,"humidity_offset":0,"pressure_hpa_offset":-2.5,"wind_dir_offset":0,
   "rain_factor":1,"wind_factor":1},"disabled":[],"hist":[],"promote":{}},
 "overrides":{"name":"Garden","calib":{"pressure_hpa_offset":-2.5}}}
```

`key` = `bound | none` (never the key or its hash); `src_mac` = the console's MAC or `null`; `configured` = at least one override is set; `stored` = the settings are saved in flash; `key_mismatch` = uploads dropped for another key since the last restart; an unknown station gives `{"error":"no such station"}` (HTTP 200); `fp` = fingerprint of
the last upload: `match | unknown | mismatch` or `""`; `learn_use` = the number of learners in use (0–4); `cal_n` = entries in the calibration log (§3.2; ≤ 8; 0 = not loaded yet). `adopt` (only while it applies) = the id of a console turned away for want of a
slot that uploads from the **same device** as this station (its MAC, or the learner core's stored MAC hash after a
restart) within the last hour: its ID changed after a console reset. The admin offers to keep the history under it.

`POST /api/admin/station/adopt` — body `{"from":"OLD1","to":"NEW1"}`: the offer must still stand for exactly this
pair. Answer `{"ok":true,"restart":true}`: a journal is written to NVS and fauxcloud restarts (planned restart `adopt`, 3
s); the boot moves OLD1's settings, channel maps, calibration log and learners to NEW1, binds the key of the upload the
owner saw, renames the history rings' owner and the snapshot entry (the records carry no id), and renames the
forwarding targets; HA retracts OLD1 and NEW1 announces on its first upload. A power cut during the move is finished by
the next boot. The move is journalled in flash. Every boot that starts the move counts itself in the journal first; a move that three boots did not
finish (a step failing every boot) is given up at the fourth: the journal is deleted, what was moved stays, and one
`ADMIN` line in the boot log says where things were left, e.g. `adopt OLD1 -> NEW1 given up after 3 boots: settings on
NEW1, rings on OLD1, target slot(s) 1 3 on OLD1` (slots are counted from 1 in this line; settings / rings "on both" or "no …"; "no target on OLD1"), for the
owner to finish by hand. After `"adopt: the new id already has saved settings; forget it first"` the new id is listed
under Settings, "Saved settings without a slot", with Forget (a RAM-only entry until the next restart: not a parked
station, so it changes no admission or restart rule; no adopt offer for it until it is forgotten), and Forget also
accepts any id with stored settings keys.
Refusals: `{"ok":false,"errors":["adopt: the offer has expired"]}`, `"adopt: from and to are station
ids"`, `"adopt: the new id already has saved settings; forget it first"`, `"adopt: the two ids share a settings key"`,
`"adopt: not enough room in settings"` (fewer free NVS entries than the move needs: the old keys' sizes + a margin, at
least 40), `"adopt: fauxcloud is still checking its new firmware; try in a minute"` (the image is not confirmed yet),
`"adopt: the settings could not be written"`. Offered only for a console turned away for want of a station slot (in practice the one-station WROOM-32 build).

`PUT /api/admin/station?station=<id>` — partial overrides document; a station not seen yet is created (within
`max_stations`). `null` resets a key to its default. Answer: the config view with `errors` (no key state). A missing or malformed `station` query answers `{"errors":["station: ?station=ID with [A-Za-z0-9_-]"]}`. A change of a calibration offset or factor before the clock is set (the calibration log is stamped with the time) answers `{"errors":["calibration: set the clock first (offsets and factors are logged with the time)"]}` (200, like the other refusals here) and applies no setting and takes no slot (the refusal comes before an id not seen yet is admitted). With no free slot: `station: max_stations reached, no free slot or no memory`. For an id whose settings are parked (saved in NVS, no slot): `station: parked with its saved settings; it gets a place after a restart` (a parked id gets a slot only at boot).

| Key | Value |
|---|---|
| `name` | string, ≤ 47 bytes UTF-8 (the HA device name); a longer name is cut and reported |
| `profile` | `yt60234` (default) or `generic` |
| `publish` | bool |
| `stale_s` | 60-86400: no upload for this long → offline |
| `limits` | `{"<core key>_min": n, "<core key>_max": n}` — values outside are rejected (rule `limits`) |
| `jumps` | `{"<core key>_per_min": n}` — larger change per minute is rejected (rule `jump`); 0 = off |
| `calib` | `temp_c_offset` ±20, `humidity_offset` ±50, `pressure_hpa_offset` ±100, `wind_dir_offset` ±360, `rain_factor` 0.1-10, `wind_factor` 0.1-10 |
| `disabled` | [field keys]: "hidden": still recorded, not in `/api/live` values, not sent to MQTT |
| `hist` | up to 4 keys `[a-z0-9_]` (a key longer than 23 characters is cut to 23) of promoted sensors to keep in hourly and daily history; `[]` / `null` = automatic. Always present in `effective` |
| `promote` | `{"<upload param>": {"name","type","as","unit","conv","scale","device_class","state_class"}}`, ≤ 4; the object replaces the whole list. `type` ∈ `temperature, soil_temperature, humidity, soil_moisture, leaf_wetness, pressure, wind_speed, rain, pm25, pm10, co2, uv, illuminance, solar, battery, voltage, other`; `as` = MQTT/HA key `[a-z0-9_]{1,23}` (derived from the name when absent); `conv` ∈ `none, FtoC, InHgToHpa, MphToKmh, InToMm, scale`. |

Defaults: `name` empty, `profile` `yt60234`, `publish` true (a station created from now on starts with the bridge's `auto_publish_new_stations`), `stale_s` 240, no limits or jumps, offsets 0, factors 1. A promoted parameter's name is at most 15 characters and its `name` at most 31 bytes; `type` defaults to `other`; `as` cannot be a built-in field key, `last_seen`, `availability` or start with `rej_`. At most 8 errors are listed, then `... N more`.

| Route | Query | Answer |
|---|---|---|
| `GET /api/admin/rings` | — | `{"rings":[{"id":"ST1","hours":3065,"days":741,"slot":true}],"parked":["ST3"]}` — the history rings on flash, by their header owner. `slot:false` = a station that exists on flash only (no RAM slot, e.g. a console that never came back after a reboot). `parked` = station settings saved in NVS that got no slot at boot (stored on a build with a higher station count, or a second console that found the one WROOM-32 slot taken: the slot count is the build's `max_stations_cap`), plus the new id of a refused adopt whose settings were left behind (a RAM-only list, gone at the next restart); ≤ 4 listed; `POST /api/admin/station/forget` accepts those ids too and removes their NVS entries. |
| `POST /api/admin/station/forget` | `station=<id>` (or body `{"station":"<id>"}`) | `{"ok":true}`: HA entities retracted, config, history and learner data deleted, slot freed; an unknown station answers `{"ok":false,"errors":["station: no such station"]}`. A console that keeps uploading comes back. When a slot is freed while a station is parked (see `parked` under `/api/admin/rings`), a restart follows in 3 s (planned restart `forget_parked`) so the boot loader gives that station its slot with its saved settings and key. Also accepts a flash-only id from `/api/admin/rings`: the ring is freed by its header id, the station's NVS entries (config, key, channel maps, calibration log, learner data) are removed and the HA retraction is queued for an announced id. The admin Publishing sheet lists these under "History without a station" with a Forget button. |
| `POST /api/admin/station/accept-key` | `station=<id>` | `{"ok":true}`: the bound key is released; the next upload binds (an unknown station: `{"ok":false,"errors":["station: no such station"]}`) |
| `POST /api/admin/station/forget-key` | `station=<id>&key=<param>` | `{"ok":true}` or `{"ok":false,"errors":["key: not known for this station"]}`; missing or bad parameters: `{"ok":false,"errors":["station, key: ?station=ID&key=KEY"]}` |
| `POST /api/learn/relearn` | `station=<id>` (empty body) | `{"ok":true}` (the loop applies it within a second, after a running backfill); `{"ok":false,"errors":["station: ?station=ID"]}` (no or a malformed id); `{"ok":false,"errors":["station: no learning state"]}` (no state, or a newer image's: read-only) |
| `POST /api/admin/station/block` | `station=<id>` | `{"ok":true}`: the id is listed (≤ 8) and forgotten if it has a slot; a 9th id is refused with `{"ok":false,"errors":["block: list not saved"]}` and the list is unchanged (unblock one first); a missing or malformed id: `{"ok":false,"errors":["station: ?station=ID with [A-Za-z0-9_-]"]}` |
| `POST /api/admin/station/unblock` | `station=<id>` | `{"ok":true}`; a bad id as for block |
| `GET /api/admin/station/blocked` | — | `{"blocked":["<id>",...]}` |
| `GET /api/admin/station/console` | `station=<id>&since=<seq>` | `{"first":n,"last":n,"items":[{"seq":n,"t":unix,"line":"GET /weatherstation/updateweatherstation.php?ID=***&PASSWORD=***&tempf=47.1&..."}]}` — the last raw upload lines (6 shared by all stations, ≤ 255 chars each), `ID` and `PASSWORD` redacted; an absent `station` = the first station; an unknown id, or no station yet: `{"error":"no such station"}` (200) |

#### 3.3.5 Bridge

`GET /api/admin/bridge` → `{"effective","overrides"}` (as `/api/bridge`). `PUT /api/admin/bridge` — partial
document, answer adds `errors`:

| Key | Value |
|---|---|
| `tz` | POSIX TZ rule (validated, up to 63 characters), default `CET-1CEST,M3.5.0,M10.5.0/3`; drives the local day boundaries |
| `tz_name` | IANA name, up to 47 characters, default empty, for the UI only (`[A-Za-z0-9/_+-]`) |
| `lat`, `lon` | location for the dashboard's sky, or `null`; `lat` -90..90, `lon` -180..180 (else `out of range`) |
| `auto_publish_new_stations` | bool, default true: a new station is published and saved on its first upload |
| `lan_uploads` | bool, default false: accept console uploads from the home network |
| `max_stations` | 1 up to `max_stations_cap`, default = the cap. The cap is the build profile's station count: **1 on the WROOM-32 build** (the value is fixed; any other number → error `fixed at 1 on this board`), 2 on the PSRAM profile (untested on hardware; any other value → error `must be 1..2`). Cannot go below the stations in use (error `below the number of stations in use: forget one first`). A stored value above this build's cap is refused at load and the default stays (logged as `stored max_stations above this board's <cap>: <n> used`). |
| `max_stations_cap` | read-only: the build profile's station count (1 on the WROOM-32 build, 2 on the PSRAM profile); not stored. Part of the effective view only; a PUT that contains it is accepted and the value is ignored. |
| `mqtt_interval_last_seen_s` | 60-3600, default 300 |
| `forwarding` | bool, default false: forwarding on. Switching it on through this document sets `max_stations` 1 and `lan_uploads` off; while it is on, `lan_uploads` or `max_stations` above 1 are refused (`off while forwarding is on (not enough memory)`), and on a build that serves more than one station (the PSRAM profile) with two stations in use it answers `forwarding: keep one station first (choose which)` (use `POST /api/admin/forwarding/enable`, §3.3.7, which asks which one to keep; at one station the choice cannot arise). |
| `log_verbose` | bool, default false: the boot log also keeps the chatter lines (below); applied at once, no restart |

An unknown key is reported as `"<key>: unknown key, ignored"` in `errors`; a bad value (`mqtt_interval_last_seen_s` outside 60-3600, a malformed `tz`, …) is reported as `"<key>: <reason>"` and the rest of the document still applies.

#### 3.3.6 Network, MQTT test, device actions

| Route | Body / query | Answer |
|---|---|---|
| `GET /api/admin/wifi/scan` | `again=1` to rescan | `{"state":"running","networks":[]}` or `{"state":"done","age_s":n,["throttled":true,]"networks":[{"ssid","rssi","open"}]}` (≤ 20, strongest first; `throttled` means a rescan was asked within 15 s of the last one, so the cached list is returned; hidden networks are not listed) |
| `POST /api/admin/mqtt/test` | optional `{"mqtt_host","mqtt_port","mqtt_user","mqtt_pass"}`, read like a settings PUT on a copy (missing or `"<set>"` = the stored value; a changed host or port without `mqtt_pass` is tested without a password); nothing is saved | `{"state":"running","detail":""}` |
| `GET /api/admin/mqtt/test` | — | `{"state":"idle|running|ok|failed","detail":"..."}` (8 s limit) |
| `POST /api/admin/reboot` | — | `{"ok":true}`, restart after ~1.5 s |
| `POST /api/admin/factory-reset` | — | `{"ok":true}`, then everything is wiped (settings, stations, history, crash data) and the device starts in setup mode |
| `POST /api/admin/setup-again` | — | `{"ok":true}`, restart into setup mode; stations and history are kept (`{"ok":false,"errors":["nvs: not saved"]}` if the flag could not be saved; the restart still happens) |
| `GET /api/admin/bootlog` | — | `text/plain; charset=utf-8`, `Cache-Control: no-store`, chunked (below) |

**Boot log.** Every kept log line since the last reset, held in RAM (RTC slow memory, 2 + 2 KB; a restart starts
it empty, after copying its tail, below): a write-once **head** and a rolling **tail** (the latest lines). The head
holds only the **boot facts** (about 20 lines: the banner, the mode line with the PBKDF2 self-test, the image
and reset reason, crash data in flash, one NVS + CFG summary, the HEAP slot reservation, the clock at boot, the history
mount and the station's restore, mDNS, DNS/NTP up, forwarding targets and the net-out task, airport weather, the learner
load, the home Wi-Fi join, the first MQTT connect and the announce, the boot's `BEAT`, the first upload), from reset until
120 s of uptime or until one does not fit; every other line goes to the tail only. A line is `<uptime ms> [<TAG>] <text>`, at most 200 B
(a longer one ends in ` [cut]`). The home Wi-Fi name (and a pending new one) reads `<home Wi-Fi>` and the bridge's MAC
reads `<mac>`; the serial log keeps them. Lines logged while the log is being sent are counted, not stored, and one
`[LOG] N line(s) not kept while the log was sent` line follows.

Levels: errors and events (boot, Wi-Fi / MQTT state changes, OTA, clock, store, notices, a forwarding
result other than delivered, a request answered 4xx / 5xx or broken off except a 503 of the load shedding, the first
upload, the first three other requests, the boot's `BEAT`, and each DNS name's / NTP client's first line since the
restart or since the 32-name table dropped it) are always kept. A repeating problem line (`HEAP tight` every 10 s,
`HTTP low memory` while pages are answered 503) is an event for the first line of a spell and once per 10 min, chatter
between. The console's AP lines print every change of direction (joined <-> left) and limit only a repeat
of the same direction the same way; the DHCP lease line prints once after a printed "joined". The setup page and its
probes in setup mode are limited the same way, each on its own limiter, and so are the `FILT` lines, one per kind (a
whole sample dropped, a rain counter glitch). The chatter -- every further upload, page request (`REQ`), captive-portal
probe, delivered forwarding send, repeated DNS / NTP line, the minute's `BEAT`, and `HIST hour closed`, `STORE
hourly ring rotated` and `STORE day written` -- is kept only with `log_verbose` on (Maintenance → Bug report → **Log every upload and page request**); with it off the tail reaches much further back than with it on. The serial log follows the same switch: with it off, the chatter lines are not printed there either.

Redaction is for the two values above only: an SSID of 1-2 characters is not replaced (it would match all over the
text), and the station's MAC, client MACs, station ids, IP addresses and the AP name `fauxcloud-XXXX` (the MAC's last
two bytes, broadcast on air anyway) stay. The file is owner data: check it before posting it. Body:

```
fauxcloud 1.0.0 boot log, uptime 5234 s, restart #12, reset reason ota
<head lines>
--- latest lines ---                      (or: --- N line(s) not kept here --- when tail lines were lost)
<tail lines the head does not hold, oldest first>
--- previous boot (reset: <reason>, restart #<n>) ---   (absent after a power-on or when nothing was kept)
<the previous boot's tail lines, oldest first>
```

**Previous boot.** At boot, before the ring is reset, the previous boot's tail (its lines oldest first, the head
copies included) is copied into a 2-KB block of RTC memory after the boot log, unless the reset was a power-on or an unknown one (RTC
content undefined) or the ring does not read as whole lines. So the lines before a crash, a watchdog reset, an OTA or a
planned restart survive one restart; `<reason>` is this boot's reset reason (what ended the previous one) and `<n>` the
previous boot's restart number -- the number that boot recorded in an RTC word once it had read its NVS counter, so
it stays right when the counter's write failed or NVS was wiped since; the NVS counter's previous value only when that
boot recorded none (an older image, the first boot after the update).

The admin page's Maintenance → Bug report → **Download the boot log** fetches it (with the CSRF header) as
`fauxcloud-bootlog.txt`. It is a diagnostic for a bug report: the text may show station ids, IP addresses, DNS names
asked on the AP and client MACs, as `/tools` does.

#### 3.3.7 Forwarding

Up to 3 targets (`slot` 0–2), each sending one station's reading to an online weather service over HTTPS or plain HTTP,
per target (the release is the TLS build, `"tls": true`, and a new target starts on HTTPS, `"https_default": true`;
the HTTP-only build `FWD_TLS=0` answers `"tls": false` and sends plain HTTP only). Interval fixed by fauxcloud: every 5 min, or the
service's own minimum + 10 s (Windy, PWSweather 310 s).

| Route | Body / query | Answer |
|---|---|---|
| `GET /api/admin/forwarding` | — | the object below |
| `PUT /api/admin/forwarding?slot=n` | a partial target (keys below); no `slot`, or an unused one = a new target in the first free slot; `{"remove":true}` removes it | the object below, with `"errors":["<key>: <reason>"]` when something was refused (always `200`: read `errors`; at most 8 are listed). A new target is saved whole or not at all; a change applies what passed. `slot: three services already: remove one first` when all 3 are used. Body ≤ 3 072 B |
| `POST /api/admin/forwarding/test?slot=n` | — | `202 {"state":"running"}`; `202 {"state":"done"}` with `result: "too_soon"` (inside the service's minimum since the last send); `409 {"error":"busy"}` (a test is waiting or running, or this target is sending now; another target's send does not refuse it: the test waits for the line, up to 20 s) or `409 {"error":"off"}` (forwarding off, or no target in that slot) |
| `GET /api/admin/forwarding/test` | — | `{"state":"idle|running|done","slot":n}`; when done also `result`, `status` (HTTP, −1 = none), `ms`, `target` (the request path and query that was sent, without the host; the `{key}` placeholder shows as `***` — a key written literally into a `template` URL is shown as written), `body` (the service's answer: only for `wu_compat` and `template` targets and only when the result is not `ok`; at most 80 characters, non-printable bytes as spaces, the id and key masked `***`; empty for the presets), `why`?, `diag`. `result` is a send result (as `last.result`) or `too_soon`, `nodata` (no fresh reading to send), `off` (forwarding off, or the target removed meanwhile) or `busy` (the line stayed taken for 20 s); a test also updates the target's `last` and `last_ok` |
| `POST /api/admin/forwarding/enable` | `{"keep":"<station id>"}` (needed only with two stations) | `{"ok":true}`; `{"ok":true,"restart":true}` when the other station was forgotten (a restart follows in 3 s to free its memory); `{"ok":true,"errors":["nvs: not saved; applied until reboot"]}`; `{"ok":false,"errors":[...]}` (also `forwarding: not switched on`; with two stations and no valid `keep`: `keep: choose the station to keep` or `keep: no such station`). Sets `max_stations` 1 and `lan_uploads` off. The other station is forgotten as by Forget (config and history) and its targets move to the kept one |

PUT keys: `type` (`wu`, `windy`, `wowbe`, `pwsweather`, `wu_compat`, `template`; a new target needs it), `station`
(default: the first station), `host` + `path` (`wu_compat` only; host `[A-Za-z0-9.-]` ≤ 64 plus `:port`, path ≤ 128),
`url` (`template` only, ≤ 384), `success` (the answer text that means accepted, ≤ 16 bytes, no quote or backslash; non-preset types only; empty = any 2xx), `id` (printable,
≤ 64 bytes, no quote or backslash), `key` (≤ 96; `"<set>"` or `""` = unchanged, `null` = clear; a changed type, host, path or url clears
it), `https` (omitted on a new target = `https_default`; omitted on a type change = the target keeps its value;
a `template` takes its URL's scheme), `enabled` (true/false). `min_s` / `max_s` are fixed: `null`, `0` or the fixed value pass, anything else is an
error. The key is stored apart and never sent back.

The presets fix the host, path, query and answer rule: `wu` is `rtupdate.wunderground.com`, `pwsweather` `pwsupdate.pwsweather.com`, `windy` `stations.windy.com`, `wowbe` `wow.meteo.be`; the key goes out as `PASSWORD` and the id as `ID` (`id` for Windy). `wu_compat` sends the WU query to your `host` and `path` (default path `/weatherstation/updateweatherstation.php`; `host` is required). `template` needs `url`: `http(s)://host[:port]/path?query` (`https://` only in the TLS build), no `user:password@`, no `#fragment`, no spaces, control or non-ASCII characters (use `%XX`; in the query it must be valid UTF-8), no quote or backslash, at most 384 bytes, with `{name}` placeholders. The names: `id`, `key`, `softwaretype`; the time `dateutc`, `dateutc_iso`, `dateutc_compact`, `epoch`, `date_local`, `time_local`; the WU names `tempf`, `dewptf`, `humidity`, `baromin`, `windspeedmph`, `windgustmph`, `winddir`, `windspdmph_avg2m`, `winddir_avg2m`, `windgustmph_10m`, `rainin`, `dailyrainin`, `rainratein`, `UV`, `solarradiation`; the period's `windmean_mph`, `gustmax_mph`, `wind_mean_ms`, `gust_max_ms`, `wind_mean_kn`, `gust_max_kn`; the metric names `temp_c`, `dewpt_c`, `pressure_hpa`, `wind_ms`, `gust_ms`, `wind_avg2m_ms`, `wind_avg10m_ms`, `gust_10m_ms`, `wind_kmh`, `gust_kmh`, `rain_1h_mm`, `rain_daily_mm`, `rain_rate_mmh`. An unknown name is refused at save (`url: unknown variable {name}`). In the path a missing value ends the send as `reject` (`why` `no_value`, not retried); in the query a pair whose value is missing is left out. An `https` that contradicts the URL's scheme is refused (`https: the URL's scheme decides`).

**The WU forecast** (TLS build only; the HTTP-only build has neither the block nor the two routes). The forwarding GET also carries
`"wufc":{"on","key":bool,"verified","loc":{"lat","lon"}|null,"state":"off|idle|retry|running|paused","pause":"none|key|encoding|too_large|untrusted|memory","next_t":"<ISO>"|null,"last":{"t","result","status"}|null,"run":{"state":"idle|waiting|running|done","result"|null,"eta_s"(waiting),"t"(done)},"n":{"ok","nodata","fail","limited","refused","skip","heap","missed","bad","paused"},"tomorrow":{"day","phrase","hi","lo","pop","t"}|null}`
(the key as a bool only; `loc` = the geocode as sent, 1 decimal ≈ 11 km; `result` = `ok nodata fail yield limited refused
encoding too_large untrusted no_memory bad`; `run.result` also `off`: nothing was sent, the key, the location or the time
was gone at the start; `n.skip` = a window that ended without a start, or a pull that was never sent, or one whose answer
belongs to settings saved while it ran (that answer stores, verifies and pauses nothing; `last` still shows it) -- neither counts as `bad` nor ends a refused / no-memory streak). `POST /api/admin/wufc {"on":bool,"key":"<32 hex>"|""|null}`: an empty or
missing key = unchanged, `null` clears it; a new or cleared key clears `verified` (and the switch); `"on":true` needs a
verified key (400 `{"errors":["not_verified"]}`; a malformed key: 400 `key_format`; a body that is not an object, a wrong type for `on` or `key`, or a syntax error: 400 `not a JSON object`; a body over 3 072 B or cut: 400 `body: missing or larger than 3072 bytes`); the answer is `{"ok":true}`, or with `"errors":["nvs: not saved; applied until reboot"]`; any accepted save lifts a pause; any change of the key or the switch restarts the schedule, lifts a pause and (off, a key
change) takes the line off `/api/live` at once. The key is stored apart, write-only (never in a GET, a log, MQTT or an export).
`POST /api/admin/wufc/run`: "Fetch now", allowed with the switch off (it is the key check). Never refused for a busy
line -- queued (one pending flag; a press while one is queued or running joins it, and an accepted press lifts a pause) and answered at once with 202
`{"ok":true,"queued":true,"eta_s":n}` (an estimate: the rest of the 60-s gap since the previous WU job, at least 20 s while
the net-out line is taken, + 5 s); it waits only for an upload, a restart, the link (Send one now's busy bits, not a page
request) and the heap gate, and is given up as `busy` after 10 min; it does not yield to a page while it runs. Refused only
with 409 `{"error":"off"}` (no key, no location or no real time) or 503 `{"error":"low_memory"}` (the line is idle and the 8-bit heap is below the
WU job's start gate at the press). The result arrives in `wufc.run` of the forwarding GET (`eta_s` while waiting, `t` = when
it ended once done; `busy` / `off` without a job when it never started); an `ok` or `nodata` answer sets `verified`, written to NVS by the next forwarding GET. The
pull: hourly at :10 UTC + a per-boot phase of 0-120 s, `GET https://api.weather.com/v3/wx/forecast/daily/5day?geocode=<lat>,<lon>&units=m&language=en-US&format=json&apiKey=<key>`
with `Accept-Encoding: identity` (anchor GlobalSign Root CA - R3), the lowest net-out priority; 401 / 403 three times in a
row pause the pulls until the next accepted save or Fetch now; a gzip body, a body over 8 KB or an untrusted certificate pause until the next accepted save or Fetch now; a 406 answer pauses as `encoding`.

**Switching on needs an accepted send.** `verified` is read-only, stored with the target:
- **Set** when a "Send one now" asked after the last change ends `ok` or `dup`. The loop marks it in RAM; the next
  `GET /api/admin/forwarding` or `…/test` writes it to flash, so a restart before that poll loses it.
- **Cleared** by a change of `type`, `id`, `key` (a new value or `null`; the same key typed again is no change),
  `host`, `path`, `url` or `success`. A target that was on
  goes off with it (logged, no error; the same applies when one PUT changes such a field and asks `enabled: true`: the target just stays off).
- **Refused:** `enabled: true` on an unverified target answers the error
  `enabled: Send one now first: it checks the id and key.`; the rest of the PUT applies and the target stays off.
- **Migration:** a target stored before the flag counts as verified if it was on (a running service keeps running) and
  as unverified if it was off.
- A `verified` key in a PUT has no effect.

GET object:

    {"enabled":bool,"tls":bool,"https_default":bool,"stations":["<id>",...],
     "targets":[{"slot":0,"type":"windy","station":"<id>",["host","path" | "url",] ["success",] "id":"...",
       "key":"<set>"|"","min_s":310,"max_s":430,"floor_s":300,"https":false,"enabled":true,
       "verified":true,"paused":null|"memory"|"key","state":"off|idle|due|running|retry","next_s":n,
       "last":{"t":"<ISO>"|null,"status":200,"result":"ok|dup|limited|refused|reject|fail|tls_untrusted|no_memory","ms":n,
               "why"?:"...","diag"?:{...}},
       "last_ok":"<ISO>"|null,
       "n":{"ok","dup","skip","nodata","reject","refused","limited","drop","heap","fail","unconfirmed"},
       "causes":{"dns","connect","read","5xx","429","other"},
       "skip_why":null|"upload|http|store|learn|hour|mqtt|restart|link|capture|heap|data|netout|turn"}],
     "heap":{"tick_us_max"},   (the release; the bench build adds `wufc.diag` and, to `heap`, send_min, largest_min, send_ms_max,
                               tick_lock_us_max, tick_own_us_max and "tick_parts", §3.2.1)
     "task":{"up":bool,"stack_free":n},
     "errors"?:[...]}

Before a target's first send, `last.result` is `""` and `last.status` is -1. At the top, `enabled` is the master switch (`forwarding` in the bridge settings); a target also has its own `enabled`. `floor_s` is the service's own minimum interval (60 s; 300 s for Windy and PWSweather); `min_s` is the send interval; `max_s` ends the send window (`min_s` + 120 s, at most twice `min_s`). `next_s` = seconds to the next send window (0 = now), −1 when off or paused.

`n` counts per target, since the restart: `ok` a reading delivered, `dup` the service already had it, `skip` a send
window that passed while fauxcloud was busy, `nodata` a window with no fresh reading to send, `reject` the service
refused the data (`tls_untrusted` included), `refused` the key or id was refused (3 in a row pause the target:
`paused: "key"`), `limited` rate-limited, `fail` a **failed attempt** (each retry counts; `causes` splits them by
cause), `drop` a **reading given up** after its retries (or when a retry could not start before the next window),
`heap` a window skipped, or a send that could not allocate its TLS buffers (`no_memory`; 3 in a row: `paused: "memory"`; a send aborted midway for lack of memory is a `fail` counted under `other`),
`unconfirmed`: a reading whose request went out whole but whose answer was lost, not sent again because a retry
would come inside the service's minimum interval (the service most likely has it; counted in neither `fail` nor `drop`,
its `causes` entry is `read`).
"Send one now" (the test) is counted in neither `n` nor `causes`; an accepted test lifts a pause. `causes` counts
failed, out-of-memory and rate-limited attempts by cause (`429` = any rate limit, `other` includes `no_memory` and a `reject` with `why` `too_large`), each
0–65535 (it stops there). `skip_why` is why the open window has not started yet (`null` when nothing holds it; it is
cleared when the window is counted as skipped). `diag`, `heap` and `task` are internal (may change any release). `diag` is a send's trace (the same object in `last`, in the test's answer and in the airport `last`): `pre8` / `largest8_pre` = the free and largest block of the 8-bit heap just before the connect, `min8` / `largest8_min` = the lowest of each during the send (bytes); `ms_dns`, `ms_tcp`, `ms_total` = milliseconds; `sock_err` = the resolver or socket error code of a failed lookup or connect (0 when none); in the TLS build also `stage` (`none`, `alloc`, `handshake`, `data`, `done`), `ms_hs` (the TLS handshake, ms), `br_err`, `br_err_name` (the TLS library's error), `version` and `suite` (the negotiated TLS version and cipher suite). A GET of the forwarding object or of the test is not read-only: it saves a pending `verified` flag; the forwarding GET also saves a pending WU key verification.

#### 3.3.8 Airport weather (not in the HTTP-only build `FWD_TLS=0`)

Same session + CSRF rules as the other admin routes. The module is off by default; turning it on, a fetch, a run once
and a lookup send requests to aviationweather.gov (the sheet says what leaves: the airport codes and, for "near me", a
rough area — the box is centred on a 0.1° grid point, about 10 km, never on the location).

- `GET /api/admin/metar` →
  `{"available":true,"on":bool,"location":bool,"time":bool,"airports":[{"icao","name","own"?,"cc"?,"km"|null}],
  "state":"off|idle|retry|paused|running","next_s":n,"wait_why":null|"send_soon"|"<busy reason>",
  "n":{"ok","nodata","fail","yield","skip","heap","limited","untrusted","paused","missed"},
  "run":{"state":"idle|waiting|running|done","result":null|"<outcome>"},
  "lookup":{"state":"idle|waiting|running|done","near":bool,"result":null|"ok|none|partial|no_location|<outcome>",
  "list":[{"icao","name","cc"?,"km"|null,"taf":bool}]},"last":null|{"t","kind":"fetch|taf|run_once|lookup","result","status","why",
  "lines","garbage","unwanted","nil","old","field_bad","trunc","dropped", ...send diag}}`.
  Outcomes: `ok nodata fail yield limited untrusted too_large no_memory reject`. `partial`: the lookup answer was
  longer than fauxcloud reads (32 KB), the list is the nearest of what was read. `run.result` and `lookup.result` are an outcome, or `off` / `busy` when the job never started (switched off meanwhile; the line stayed taken for 30 s). `next_s` = seconds to the next fetch, −1 when off or paused. `n` and `last` are R&D (may change any
  release); `http_idle_hist`, `last.http_idle_ms` / `http_during` and the send diag's `stack_free` / `curve` /
  `rec_in_max` come only from the bench build (§3.2.1).
- `POST /api/admin/metar` `{"on":bool,"airports":["KJFK", ...]}` (either key may be absent): at most three, 4 characters
  (the first `A-Z`, then `A-Z 0-9`), no duplicates, each from the current list or the last lookup (its name, position and distance come from
  there). → `{"ok":true}`, or `{"ok":true,"errors":["nvs: not saved; applied until reboot"]}`, or 400
  `{"errors":["..."]}` (a body over 3 072 B or cut: 400 `{"errors":["body: missing or larger than 3072 bytes"]}`). A change of the set of airports restarts the schedule (the first fetch is the next :05 / :20 / :35 / :50 UTC at least 60 s away); a pure reorder keeps it. A code that is neither in the current list nor in the last lookup answers `look the airport up first`.
- `"names":{"KJFK":"text"}` sets a chosen airport's display name. The rules:
  - The text is trimmed and cut to 24 bytes on a UTF-8 boundary. Control characters answer 400
    `names: no control characters`; a code that is not one of the chosen airports answers 400
    `names: not one of your airports`.
  - A typed name is the user's: a lookup never replaces it, and it follows its airport through a reorder. It is saved with the airport.
  - `""` gives the name back to aviationweather.gov: the last lookup's short name when that lookup has the airport, else
    the last known name stays (no longer the user's) until a lookup replaces it.
  - A lookup refreshes the names the user did not type. The loop changes them in RAM; the next `GET /api/admin/metar`
    saves them.
  - Names are JSON-escaped in every answer.
  - `GET /api/admin/metar` marks a typed name with `"own":true` on its airport.
- `cc`: the country for the row's flag, as in `/api/metar` (absent when the prefix is unknown).
- The schedule: with real time the 15-min cycles sit on :05 / :20 / :35 / :50 UTC (the first one at least 60 s after
  the module can run); the :50 cycle fetches the TAFs (`kind: "taf"` in `last`), the others the reports. `taf` in the lookup
  list: the airport issues forecasts (its `siteType`); it is not stored with the chosen airports.
- `POST /api/admin/metar/run` → `202 {"state":"waiting"}` (one fetch of the reports now, user priority; it lifts a pause; inside an
  open 5-min send window of the 15-min cycle it serves that window, never the TAF cycle), or 409 `{"error":"too_soon|busy|off"}` (`too_soon`: < 60 s after the previous
  METAR request, on an idle line; `busy`: a run once already waits or runs). Pressed while another METAR job runs (the scheduled
  fetch, the TAF cycle, a lookup), it waits for that job and starts after it (given up as `busy` after 30 s).
- `POST /api/admin/metar/lookup?near=1` (the nearest five airports with reports within about 120 km of the configured
  location) or `?ids=XXXX` (one code, anywhere) → `202 {"state":"waiting"}` (`ids` also takes lower case); 400 `{"error":"ids: 4 characters A-Z 0-9"}`;
  409 `{"error":"no_location|too_soon|busy|off"}`, the same waiting rule as the run once (`off`: the module is off or there is no real time yet; for the run also no chosen airport). The result arrives in `lookup` of the GET.
- A page request aborts a scheduled fetch (or the WU pull) before its answer's status line (`yield`, retried); the run once and
  the lookup never yield to a page.

#### 3.3.9 Listen to a new console

For a console that does not upload to fauxcloud: the bridge listens on its own Wi-Fi and records what the devices there
send, so the report can go into a Console report. While it runs, the bridge is quiet like during a firmware upload:
before the MQTT client stops, the bridge's availability topic gets a retained `offline` (QoS 1), so Home Assistant shows
the entities unavailable; online jobs (forwarding, airport weather, the forecast) are held (busy word `capture`; a running
job stops with the reason `capture`); every console upload, from the fauxcloud Wi-Fi or the home network, is answered
`success` and neither stored nor turned into a station; the history's hour close and the learners go on. The AP, DNS and
NTP (§2.4, unchanged) and the LAN face (dashboard, admin) keep working. Nothing is stored on flash; a restart ends it.
It ends on Stop, a planned restart or factory reset, or 60 minutes in which no `GET /api/admin/capture` came (nobody
watching). When it ends, MQTT starts again (`online`, the discovery; also when it is stopped right after the `offline`,
before the client stopped). A capture and a firmware upload exclude each other; the admin's MQTT connection test is
refused while it runs (`fauxcloud is listening to a new console`).

What is recorded, only from the fauxcloud Wi-Fi (the AP side), each request becoming 1-5 short records:
- **DNS**: each name asked, counted (up to 16 names; more are counted as `other`).
- **Devices**: every join, DHCP lease and leave on the fauxcloud Wi-Fi; the list carries the address, the maker part of
  the MAC (its first three bytes; `random` for a locally administered address) and the times. The whole MAC never leaves
  the board.
- **NTP**: asks and answers counted; the first ask is a record.
- **Port 80**: every request, also unknown paths and methods: the method and the whole URI (in up to three `http`
  records), the headers of a fixed list (`Host`, `User-Agent`, `Content-Type`, `Content-Length`, `Accept`,
  `Accept-Encoding`, `Connection`, `Transfer-Encoding`, `Expect`, `Cache-Control`, `X-Requested-With`; `Authorization`,
  `Cookie`, `X-Api-Key`, `X-Auth-Token` as `***`), the first 128 bytes of a body (one receive, up to 3 s; a chunked body
  is not read), and `closed-before-reply` when the device closed before the answer. The answers do not change.
- **Ports 443, 8080, 8443, 8000, 8880, 1883, 8883, 5000** (listening only during a capture): the first bytes of a
  connection, read for 300 ms (at most 512 B), then the connection is closed; nothing is answered. A TLS ClientHello
  becomes its facts (`verdict` against the TLS 1.2 suites fauxcloud's TLS library can serve: `ecdhe-ecdsa-gcm`, `chacha`,
  `ecdhe-rsa-gcm`, `no-common-curve`, `cbc-only`, `rsa-kx-only`, `tls13-only`, `tls-old`, `unknown`; versions, the server
  name, the first 12 suites, the groups, `sigalg`, `mfl`, `alpn`, ` cut` when the bytes ended inside it); an HTTP request
  its line, every header (secret ones `***`) and the body's head (in the report under its `tcp` record's group); an MQTT CONNECT a summary (version, flags, keep-alive,
  the client id's length; never the id, user or password); anything else 16 bytes as hex. One connection is read at a
  time; another one meanwhile is closed and recorded as `busy, closed`.
- **Hidden before a record is stored**: the value of every query, form or JSON key that contains `key`, `pass`, `pwd`,
  `token`, `secret`, `auth`, `sign` or `cred`, or is `id`, `uid`, `did`, `devid`, `deviceid`, `device_id`, `mac`, `sn`,
  `serial`, `wid`, `user`, `username`, `login`, `imei`, `sid`, `siteid`, `stationid` or `clientid`, or ends in `_id` /
  `-id` (case-insensitive, %-decoded); the path segment after `wid`, `key`, `id`, `token`, `user`, `pass`, `station`,
  `device`, `dev`, `sn`, `serial` or `mac`, and any path segment of 16 or more letters, digits, `-` and `_` with both
  letters and digits (a key in the path). A body head with control bytes is shown as hex (32 B); other bytes above
  ASCII (UTF-8) count as text: redacted like any text, then shown as `?`.

| Route | Body / query | Answer |
|---|---|---|
| `GET /api/admin/capture` | `?since=<seq>` (0 or absent: from the start) | the object below; every GET keeps an unwatched capture from stopping |
| `POST /api/admin/capture/start` | none, or `{"idle_s":N}` (the no-poll limit for a bench, 60-3600 s, default 3600) | `{"ok":true}` (the start then runs: the GET says `starting`, `on`, or `off` with `ended` `heap`); 409 `{"error":"ota"}` during a firmware upload, `{"error":"restart"}` with a restart or factory reset planned, `{"error":"on"}` when one runs |
| `POST /api/admin/capture/stop` | none | `{"ok":true}` (also when none runs) |

The GET object: `state` (`off`, `starting`, `on`), `ended` (after a stop: `stop`, `idle`, `restart`, `heap`; else ""),
`blocked` (why a start would be refused now: `ota`, `restart` or ""), `elapsed_s` (listening time; after the stop, the
time it listened), `idle_left_s`, `started_utc` (unix seconds, null without a real clock at the start), `clock` (the bridge
has a real clock now), `ntp` (`asked`, `answered`), `uploads_skipped`, `vendor` (the maker prefix of the first device that
sent something other than a phone's connectivity probe or DNS, or null), `seq` (the newest change), `lost` (records that
left the board before any GET returned them), `more` (more records wait: ask again with `since` = the last `seq` returned),
`clients` (`ip` or null, `vendor`, `joined_s` (null: there before the start), `left_s` (null while there), `n` records,
`active`), `clients_over`, `dns` (`other`, `names`: `name`, `n`, `types` `A`/`AAAA`/`other`, `client` = the asker's last
address byte), `events` (at most 8 per answer, oldest change first: `id` (stable: a repeat keeps it), `seq`, `group` (the
`id` of the first record of its request), `kind` (`join`, `lease`, `leave`, `ntp`, `tcp`, `tls`, `http`, `hdr`, `body`,
`mqtt`, `raw`), `client` (the device's last address byte, 0 = unknown), `n` (times seen: a repeat counts here and returns
with a new `seq`), `t_first_s`, `t_last_s` (seconds since the start), `text`). After the stop the lists are empty and the
counters stay. A start press clears the last run's counters, so a GET while `starting` answers `seq` 0 and `lost` 0.

### 3.4 Firmware upload (OTA)

`POST /api/admin/ota` — LAN face, CSRF + session. Body = the raw `.bin` image (`Content-Type:
application/octet-stream`), `Content-Length` required.

- Refused at once (200 + JSON, connection closed) when memory is low (`{"error":"low memory — restart fauxcloud
  first"}`), when the length is 0 or larger than the app slot (`{"error":"the file must be a firmware image no larger
  than the app slot"}`), while a forwarding send cannot stop in time (`{"error":"busy sending to an online service,
  try again in a few seconds"}`), or when the server cannot hand the connection to the update task (`{"error":"busy, try again"}`). The app slot is 1 900 544 bytes on the WROOM-32 build (1 835 008 on the PSRAM profile). 409 `{"error":"an update is already running"}` when one runs; 409 `{"error":"capture"}` (connection closed) while the bridge listens to a new console (§3.3.9).
- The image is streamed into the inactive slot. It must start with the ESP32 image magic and carry the fauxcloud
  image marker; otherwise `{"error":"not an ESP32 firmware image"}` / `{"error":"not a fauxcloud image"}`. Other
  errors: `low memory`, `update did not start: ...`, `upload stalled` (~6 s without data), `connection lost`,
  `flash write failed: ...`, `image rejected: ...`.
- Success: `{"ok":true,"bytes":1665040,"version":"1.0.0 Nov 16 2026 20:41:07"}` (the image's version and
  build time; a bench image says `1.0.0+diag`), then a restart into the new image. The new image
  confirms itself once the home Wi-Fi has an address, one page or API answer was served and 60 s of uptime passed without a watchdog reset (`ota_state` in `diag`: `pending`, then `stable`). There is no timer: if it restarts before that (a crash, a watchdog reset, a power cut), the previous image comes back (`rolled_back`).
- While it runs: MQTT is stopped, console uploads are answered but not processed, new page loads get 503.

### 3.5 Setup API (setup mode, AP face only)

| Route | Body / query | Answer |
|---|---|---|
| `GET /api/setup/scan` | `again=1` to rescan | as `/api/admin/wifi/scan` |
| `GET /api/setup/state` | — | `{"state","hostname","ap_ssid","sta_ssid","keep_ap","keep_admin","tz","tz_name","sta_ip","sta_mask"}` |
| `POST /api/setup/apply` | see below | `{"errors":[]}` and the join starts; or `{"errors":["key: reason",...]}` and nothing is applied |
| `POST /api/setup/time` | `{"t":<ms>}` | `{"applied":bool,"time_src":"..."}` — the phone's clock, used only while no better source worked |

`state`: `idle`, `busy` (scanning), `joining`, `joined`, or `failed:<auth|no_ap|dhcp|timeout>`. `keep_ap` /
`keep_admin` say that a value is already stored (setup again): those fields may then be left out to keep them.
`ap_ssid` is the run-mode network the console will be paired with. `sta_ip` / `sta_mask` (the mask) are the home
network's address and subnet mask once joined (`""` before): the success card warns when that network overlaps
fauxcloud's own 192.168.77.0/24, compared under the wider of the two masks (a 192.168.0.0/16 home network holds it): a /24
network gets the advice to change its address, a wider one (named as network/prefix) the advice to set the subnet mask to
255.255.255.0. The API carries the mask only; the sentences are the page's.

`/api/setup/apply` body:

| Key | Rule |
|---|---|
| `sta_ssid` | required, 1-32 bytes |
| `sta_pass` | required; `""` for an open network, else 8-63 |
| `ap_pass` | required on first setup, 8-63; absent = keep the stored one |
| `admin_salt`, `admin_h`, `admin_iters` | required on first setup: salt (32 hex), H = PBKDF2-SHA256(password, salt, iters) (64 hex), iters 10000-200000 (default 10000). The password itself is never accepted (`admin_password` is refused). |
| `tz` | required, POSIX TZ rule |
| `tz_name`, `lat`, `lon`, `hostname` | optional (`tz_name` up to 47 characters; `lat` -90..90, `lon` -180..180) |

A second apply while `joining`/`joined` is refused (`"state: already joining, wait for the result"`). Any error, an unknown key or a non-string value included, refuses the whole apply: nothing is applied. In setup again, a new `sta_ssid` clears the stored MQTT password (it is never sent to another network). After a
successful join the device restarts into run mode, about 6 s after `joined`; `sta_ip` in the state shows the new address.

### 3.6 Static pages and assets

| Path | Notes |
|---|---|
| `/` (run) | LAN: dashboard shell. AP: the `/tools` plain status page (no JavaScript) |
| `/admin` (run) | admin shell, LAN face only (AP: catch-all `success`) |
| `/`, `/setup` (setup) | setup shell |
| `/a/<hash8>/<name>` | `app-dash.css`, `app-admin.css`, `sky.css`, `sky.js`, `status.js`, `outlook.js`, `outlook-why.js`, `dash-sheet.js`, `dash-lg.js`, `dash-airport.js`, `dash-wufc.js`, `admin.js`, `admin-station.js`, `admin-sheets.js`, `admin-fwd.js`, `admin-metar.js`, `admin-learn.js`, `favicon.svg`, `touch-icon.png`, `moon.svg`, `stars.svg`, `rain.svg`, `licenses.txt`, `tz.json`, `flags-0.json` … `flags-9.json`, and the Polish language packs, one per script: `status.pl.json`, `outlook.pl.json`, `outlook-why.pl.json`, `dash-sheet.pl.json`, `dash-airport.pl.json`, `dash-wufc.pl.json`, `admin.pl.json`, `admin-station.pl.json`, `admin-sheets.pl.json`, `admin-fwd.pl.json`, `admin-metar.pl.json`, `admin-learn.pl.json` (46 files). The hash changes with the content; take the current path from the page, never hard-code it. A page picks its packs from the manifest in its main script (`lang.js`): the language stored in this browser by the language button, else the first of the browser's languages that is English or has packs, else English (no pack); the packs of the page's first scripts load before its first API call, a lazy script's pack before the script. |

### 3.7 Captive-portal behaviour

| Mode, side | Request | Answer |
|---|---|---|
| Run, AP, GET | `/hotspot-detect.html`, `/library/test/success.html` | 200 `text/html` Apple "Success" page |
| Run, AP, GET | `/generate_204`, `/gen_204` | 204 |
| Run, AP, GET | `/connecttest.txt` | 200 `Microsoft Connect Test` |
| Run, AP, GET | `/ncsi.txt` | 200 `Microsoft NCSI` |
| Run, AP, GET | `/success.txt` | 200 `success` + newline |
| Run, AP, GET | `/canonical.html` | 200 Firefox redirect page |
| Run, any | anything else unknown | 200 `success` (`200` for a Weathercloud host) |
| Setup, AP | any unknown path or method (all the probes above included; a URI over 512 B or oversized headers get the 200 `success`) | 302 to `http://fauxcloud.setup/` |
| Setup, LAN | anything except `/a/*` | 200 `success` |

So in run mode a phone on fauxcloud's Wi-Fi is not nagged by a captive sheet, and in setup mode the OS opens the
setup page. Android validates over HTTPS only, so it may not show a sheet: type `http://fauxcloud.setup` (with the
scheme). In setup mode port 443 accepts a connection, logs the requested server name and closes it.

---

## 4. Authentication

The admin face exists on the LAN side only. The password never reaches the device, at setup or at login.

**Stored credential.** `salt` (16 bytes), `iters`, and `H = PBKDF2-SHA256(password, salt, iters)` (32 bytes). The
setup page and the password change compute H in the browser and send only salt, iters and H.

**Login (challenge-response).**

1. `GET /api/admin/challenge` with `X-Fauxcloud: 1` → `nonce` (32 hex, single use, valid 60 s, bound to the
   client's IP), `salt`, `iters`. Without a stored credential a random salt is returned, so nothing leaks.
2. The browser computes `H` and `proof = hex(HMAC-SHA256(key = H, message = the nonce's 32 ASCII hex characters))`.
3. `POST /api/admin/login` `{"nonce","proof"}` (+ optional `t`, the browser's clock in ms; it sets the device clock only while the device has no real time, and the answer says `applied`). The device compares in constant time.

The same proof (with a fresh nonce) is required to change the password, reveal the AP password and download crash
data.

**Session.** A successful login sets
`fcs=<32 hex>; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000`. The token is 128 random bits.

| Property | Value |
|---|---|
| Lifetime | 30 days idle, sliding (each use refreshes it) |
| Binding | the client IP it was issued to; another IP = not signed in |
| Limit | 4 sessions; a 5th login ends the least recently used |
| Storage | RAM only: a restart ends every session |
| CSRF | `X-Fauxcloud: 1` on every admin call (a cross-site form cannot set it) plus `SameSite=Strict` |
| Ended by | logout, revoke, revoke-all, a password change (other sessions), restart |

An expired or missing session on a LAN call with `X-Fauxcloud: 1` gets `401 {"error":"auth"}`; the admin page
then shows its sign-in. Without the header (or from the AP side) the call gets the catch-all `200 success` text, not
JSON: denied admin calls look like an unknown path. This is not hiding: the `/admin` page itself is public on the LAN. A client should treat a 401 or a non-JSON answer from
`/api/admin/*` as "signed out". A `Cookie` header longer than 255 bytes is not read (the call counts as signed out).

**Lockout** (RAM only; a power cycle clears it on purpose: physical access is trust).

| Scope | Trigger | Effect |
|---|---|---|
| Per IP | 5 failed proofs within 60 s | that IP refused for 60 s: `{"ok":false,"error":"locked","retry_s":N}` |
| Account | 10 failed proofs from any IPs within 10 min | every proof fails for 5 min, doubling per further burst (5, 10, 20, 40, 60 min cap); the answer looks like a wrong password; shown as `lock` in `/api/bridge/status` and `admin_lock` in `diag` |

A success clears both. A proof with an unknown or expired nonce is refused but **not** counted, so a stray request
cannot lock the owner out.

**What it protects against:** reading the password itself off the network (it never crosses it), CSRF from other
sites, online guessing (the lockout), anyone on the AP side, a web page that rebinds its own name to the bridge (§1.1).
**Not** against an active man-in-the-middle on the LAN (no TLS) or a compromised browser. **What H is:** H is a sign-in
equivalent, not just a fingerprint of the password: whoever has H can sign in, and can try password guesses against it
offline. H crosses the network in clear twice: at setup (on fauxcloud's own Wi-Fi, whose setup password is public) and
on a password change (on the LAN). A login sends only a one-time proof, but someone who records it can also try
guesses offline. The session cookie travels in clear too (bound to the browser's IP). So use a strong password, and do
the setup and a password change only on a network you trust.

---

## 5. MQTT

Configured in admin (`mqtt_host` empty = MQTT off, dashboard only). Plain TCP, QoS 0 for everything except the LWT
(QoS 1). Client id `fauxcloud-<last 3 MAC bytes hex>`, keepalive 30 s. fauxcloud **subscribes to nothing**: there
are no MQTT commands.

`<mac12>` = the bridge's 12 lower-case hex MAC digits; `<mac4>` = the last two bytes, upper case, as in the Wi-Fi
name `fauxcloud-<mac4>`; `<id>` = the station id; `<key>` = a field key (§6.1), `last_seen`, `sensor_stale`,
`notices`, `outlook`, `wind_avg10m_kmh`, `rej_<rule>` or a promotion's `as` key.

### 5.1 Topics

| Topic | Payload | Retained | When |
|---|---|---|---|
| `fauxcloud/bridge/<mac12>/status` | `online` / `offline` (LWT) | yes | on connect / on disconnect |
| `fauxcloud/bridge/<mac12>/<key>` | bridge diagnostic value (§5.3) | no | on connect: `sta_ip`, `firmware`, `last_reset`, `restarts`, `last_restart` (valid clock; else once at the first sync), `time_src` (also on change); `rssi` (only while Wi-Fi is up), `ap_clients` and `heap_free` on connect and every 5 min |
| `fauxcloud/bridge/<mac12>/notices` | notice sentences or `OK` | no | on change and every 5 min |
| `fauxcloud/bridge/<mac12>/notices/attr` | `{"items":[{"id","n"?}]}` | no | with the above |
| `fauxcloud/<mac4>/<id>/availability` | `online` / `offline`; empty = station gone | yes | after discovery (its current state), on stale (no upload for `stale_s`), on return; `offline` after a restart until the station's first upload, and once for publish off |
| `fauxcloud/<mac4>/<id>/<field key>` | value as text, field decimals (`8.4`, `245`, `0.30`) | **no** | every accepted upload (not one the profile dropped whole), only the fields that upload carried; the first state ~2 s after discovery |
| `fauxcloud/<mac4>/<id>/<promotion key>` | value, type decimals | no | when the upload carried it |
| `fauxcloud/<mac4>/<id>/sensor_stale` | `1` (not updating) / `0` | no | with every published state |
| `fauxcloud/<mac4>/<id>/last_seen` | ISO UTC | no | first upload, back online, then at most every `mqtt_interval_last_seen_s` (default 300 s, 60 to 3600); only with a valid clock |
| `fauxcloud/<mac4>/<id>/rej_<rule>` | count since boot | no | on change |
| `fauxcloud/<mac4>/<id>/notices` | sentences joined by a space, or `OK`; ≤ 255 bytes (the chips, `Sensor not updating. Key changed.`, when the sentences pass it) | no | on change and every `mqtt_interval_last_seen_s` |
| `fauxcloud/<mac4>/<id>/notices/attr` | `{"items":[{"id":"sensor_stale","since":"<ISO>"}]}` (params as §6.4) | no | with the above |
| `fauxcloud/<mac4>/<id>/outlook` | the outlook sentences joined by a space (§5.2), ≤ 255 bytes | no | when the outlook codes or the tendency change, and every `mqtt_interval_last_seen_s`; computed when an upload or an hour close happened, or the interval is up |
| `fauxcloud/<mac4>/<id>/outlook/attr` | `{"v":1,"state","next","warn","qual","conf","gate","tend","tend_fast","d3","as_of","near"}`, plus `"min":[lo,hi]` only with `next` = `min_range` (codes, §5.2; `near` = the ICAO behind a shown nearby-report code, else null) | no | with the above |
| `fauxcloud/<mac4>/<id>/rain_mm/availability` | `online` / `offline` | yes | profiles whose `rainin` is a rate: `online` from the first upload after a boot when the hour records give the estimate (`source: hours`, §6.2), the ring value taking over one hour after the first rain reading; `offline` only while neither exists (no previous hour record, the open or the previous hour without a rain value, an approximate clock, no clock) |
| `fauxcloud/<mac4>/<id>/rain_mm/attr` | `{"source":"derived","n":56}`; `{"source":"hours","n":N}` while estimated from the hour records after a boot (`n` = the readings used in the window) | yes | on change (rate profiles) |
| `fauxcloud/<mac4>/<id>/pressure_hpa/attr` | `{"calibrated":true}` | yes | on change (profiles with a factory pressure) |
| `fauxcloud/<mac4>/<id>/{wind_avg2m_kmh,wind_dir_avg2m,gust_10m_kmh}/attr` | `{"source":"derived","n":2,"window_s":120}` (600 for the gust) | yes | while fauxcloud computes the field; cleared when the console sends it |
| `fauxcloud/<mac4>/<id>/wind_avg10m_kmh` | the 10-min mean wind, 1 decimal (§6.2) | no | every upload that has wind in the last 10 min |
| `fauxcloud/<mac4>/<id>/wind_avg10m_kmh/attr` | `{"source":"derived","n":10,"window_s":600}` | yes | on change; cleared with the station |

Once per bridge, after an update from an older image, the retained `fauxcloud/<id>/availability` topics of the earlier layout are cleared (an empty retained message). Hidden fields are never published. A forgotten station gets empty retained messages on its availability, its
discovery topics and its retained attribute topics. Measurements are not retained on purpose: a rejected value is
simply not updated, and old values cannot outlive a broker restart.

### 5.2 Home Assistant discovery

Prefix `homeassistant`, retained, re-sent on every connect, after a station's settings change and after the LAN address changes (paced). No device appears before the station's first
upload.

| Item | Value |
|---|---|
| Station entity topic | `homeassistant/<component>/fauxcloud_<mac12>_<id>/<key>/config`, component `sensor`, or `binary_sensor` for `battery_low` and `sensor_stale` |
| `unique_id` | `fauxcloud_<mac12>_<id>_<key>` |
| `default_entity_id` | `<component>.fauxcloud_<id slug>_<key slug>`, e.g. `sensor.fauxcloud_st1_temp_c`, `binary_sensor.fauxcloud_st1_sensor_stale`; bridge `sensor.fauxcloud_bridge_<mac4 in lower case>_<key>`, e.g. `sensor.fauxcloud_bridge_a1b2_rssi`. Slug: lowercase, every run of characters outside `a-z0-9` becomes one `_`, no `_` at either end; an empty slug leaves the key out. HA uses it only when it **creates** the entity: an entity it already knows by its `unique_id` keeps its id (rename it in HA if wanted). Needs Home Assistant 2025.10 or later. |
| Device | identifiers `fauxcloud_<mac12>_<id>`, name = station name (default `fauxcloud <id>`), manufacturer `fauxcloud`, model `WU-protocol weather station`, `sw_version` = fw version, `via_device` `fauxcloud_bridge_<mac12>`, `configuration_url` `http://<LAN IP>/` (left out while there is no LAN address) |
| Availability | list: bridge status + station availability (+ `rain_mm/availability` for a derived rain), `availability_mode: all` |
| Attributes | `json_attributes_topic` = `<state topic>/attr` for `notices`, `outlook`, derived rain, pressure (factory-value profiles), the derived wind fields and `wind_avg10m_kmh` |
| Binary sensors | `payload_on: "1"`, `payload_off: "0"` (the `battery_low` state is `0` when the value printed with no decimals is `0`, else `1`: 0.4 publishes `0`); `battery_low` device class `battery`, `sensor_stale` ("Outdoor sensor link") device class `problem` |
| Bridge entity topic | `homeassistant/sensor/fauxcloud_bridge_<mac12>/<key>/config`, `unique_id` `fauxcloud_bridge_<mac12>_<key>`, device `fauxcloud bridge <MAC4>` (upper case; identifiers `fauxcloud_bridge_<mac12>`), model `fauxcloud bridge (ESP32)`, `sw_version`, `configuration_url` (as for a station), no `via_device`; `availability_topic` = bridge status |

Announced per station: the 11 core fields always; extended fields once the station has sent them; `last_seen`;
`sensor_stale`; `notices`; `outlook`; `wind_avg10m_kmh` ("Wind speed, 10-min average") once the station has sent
`wind_kmh`; the 7 `rej_<rule>` counters (diagnostic); configured promotions. A hidden field's entity is removed and comes
back when un-hidden (hiding `wind_kmh` removes `wind_avg10m_kmh` too). Minimum Home Assistant 2025.10 (the
`default_entity_id` key; how older versions treat it is not verified).

**Local outlook (`outlook`).** A text `sensor` named "Local outlook", icon `mdi:text-box-outline` (neutral, no sky claim), no device
class, no state class. It is part of the constant entity set (announced with the station's first discovery, whatever
the fields), has `json_attributes_topic` = `.../outlook/attr`, and is retracted with the station (empty retained
discovery payload on Forget). Not retained, like the measurements.

State: the English sentences of the gate, `state`, `qual`, `next` and `warn` (§3.2.2) in that order, joined by a space,
at most 255 bytes (the longest combination is 113 B): for example `No rain. Pressure steady.`, `No rain. Calm.
Pressure rising.` or `Freezing. Pressure steady.` Under the time gate the sentence is `Outlook waits for a valid
clock.`; under the stale gate it starts `Outdoor sensor not updating.` (then the pressure line). The pressure line's
status codes (`collecting`, `p_paused`) are left out when a gate, state or qualifier sentence came before them (so the state is never blank; a warning may still follow); the
codes stay in the attributes. The dashboard's strip never shows them (only its ⓘ explanation does). `gusts` reads `Gusts to 45 km/h.`
(the gust rounded to whole km/h), `collecting` reads `Pressure outlook in 3 h.` Nothing is published
without a barometer (no outlook).

Attributes (codes only; `null` where the API block has `null`): `v` (1), `state`, `next`, `warn`, `qual`, `conf`
(`"default"`; `"learned"`), `gate`, `tend` (`rising|steady|falling` or null), `tend_fast` (bool), `d3` (the 3-h change in hPa,
1 decimal, or null), `as_of` (ISO UTC or null), `near` (the ICAO code of the airport behind a shown nearby-report sentence, else null); only with `next` = `min_range`: `min` (`[lo, hi]`, whole °C). Same code lists and the same "unknown code = absent" rule as the API.

Publish rule: the outlook is recomputed in the MQTT loop when the station's upload counter or its closed-hour
counter changed, or `mqtt_interval_last_seen_s` has passed. It is published when its signature (the codes, `left_h`,
the tendency rounded to 0.1 hPa, the gust, the nearby airport and the minimum range) differs from the last one sent, and in any case once per
`mqtt_interval_last_seen_s`. A failed publish is tried again the next second. Only stations with `publish` on and a
completed announcement are considered.

Example (`temp_c`):

```json
{"name":"Temperature","unit_of_measurement":"°C","device_class":"temperature","state_class":"measurement",
 "suggested_display_precision":1,"unique_id":"fauxcloud_02000000a1b2_EXAMPLE1_temp_c",
 "default_entity_id":"sensor.fauxcloud_example1_temp_c","state_topic":"fauxcloud/A1B2/EXAMPLE1/temp_c",
 "availability":[{"topic":"fauxcloud/bridge/02000000a1b2/status"},{"topic":"fauxcloud/A1B2/EXAMPLE1/availability"}],
 "availability_mode":"all",
 "device":{"identifiers":["fauxcloud_02000000a1b2_EXAMPLE1"],"name":"Garden","manufacturer":"fauxcloud",
  "model":"WU-protocol weather station","sw_version":"fauxcloud 1.0.0",
  "via_device":"fauxcloud_bridge_02000000a1b2","configuration_url":"http://192.168.1.20/"}}
```

### 5.3 Bridge diagnostic entities

All are in the HA "diagnostic" category, except `wu_tomorrow`.

| Key | Name | Value | Unit | Device class | State class | Sent |
|---|---|---|---|---|---|---|
| `sta_ip` | IP address | LAN address | | | | connect |
| `rssi` | Wi-Fi signal | dBm | `dBm` | `signal_strength` | `measurement` | connect, every 5 min; only while Wi-Fi is up |
| `ap_clients` | AP clients | count | | | `measurement` | connect, every 5 min |
| `heap_free` | Free heap | bytes, byte-addressable | `B` | `data_size` | `measurement` | connect, every 5 min |
| `last_restart` | Last restart | ISO UTC (once the clock is valid) | | `timestamp` | | connect, or the first clock sync |
| `firmware` | Firmware | e.g. `fauxcloud 1.0.0` | | | | connect |
| `last_reset` | Last reset reason | text | | | | connect |
| `restarts` | Restarts | count | | | `total_increasing` | connect |
| `time_src` | Time source | `none | persisted | browser | dhcp | sntp` | | | | connect, on change |
| `notices` | Notices | sentences or `OK`; attributes `{"items":[{"id","n"?}]}` (§6.4) | | | | connect, on change, every 5 min |
| `wu_tomorrow` | Forecast tomorrow (Weather Underground) | **not in 1.0** (not in the release build). Not diagnostic, discovered only while the forecast switch is on (retracted when off): WU's phrase, `None` (unknown) while nothing is shown; attributes `{"day":"tomorrow"\|"today","t_max_c","t_min_c","precip_pct"\|null,"fetched":"<ISO>","source":"Weather Underground"}`; not retained, on change and every 5 min |

---

## 6. Field reference

### 6.1 Station fields

"Core" fields are kept in the recent ring and in the hourly and daily history. The Aggregation column says what the
history keeps in an hour row (a day row folds the hours): `min/avg/max` all three, `avg` the average only, `max` the
maximum only, `vector mean` the speed-weighted direction, `rain fallen` the sum of the counter's increases.
`rain_daily_mm` is not stored on its own: the rows' `rain_mm` is the rain of the hour or day. The other fields are
live only (`/api/live`, MQTT), with these exceptions: `indoor_temp_c`, `indoor_humidity` and every promoted sensor the console has sent are kept as extra columns while a free channel is left (4 in all; an hour row keeps the average only, a day row min / avg / max of the hourly averages); `rain_rate_mmh` feeds the rows' `rain_rate_max`;
and the station's `hist` setting replaces the automatic choice with up to four keys.

| Key | Upload parameter | Unit | Conversion | Dec | Aggregation | Core | HA device class |
|---|---|---|---|---|---|---|---|
| `temp_c` | `tempf` | °C | °F → °C | 1 | min/avg/max | yes | temperature |
| `dewpt_c` | `dewptf` | °C | °F → °C | 1 | avg (day: min/avg/max of the hours' averages) | yes | temperature |
| `humidity` | `humidity` | % | — | 0 | min/avg/max | yes | humidity |
| `pressure_hpa` | `baromin` | hPa | inHg → hPa | 1 | avg (day: min/avg/max of the hours' averages) | yes | atmospheric_pressure |
| `wind_kmh` | `windspeedmph` | km/h | mph → km/h | 1 | avg | yes | wind_speed |
| `gust_kmh` | `windgustmph` | km/h | mph → km/h | 1 | max | yes | wind_speed |
| `wind_dir` | `winddir` | ° | wrapped 0-359 | 0 | vector mean | yes | wind_direction |
| `rain_mm` | `rainin` (`generic` profile) / derived (`yt60234`) | mm | in → mm | 2 | rain fallen | yes | precipitation |
| `rain_daily_mm` | `dailyrainin` | mm | in → mm | 1 | none (see above) | yes | precipitation (total_increasing) |
| `uv` | `UV` | index | — | 1 | max | yes | — |
| `solar_wm2` | `solarRadiation` | W/m² | — | 0 | avg/max | yes | irradiance |
| `indoor_temp_c` | `indoortempf` | °C | °F → °C | 1 | | no | temperature |
| `indoor_humidity` | `indoorhumidity` | % | — | 0 | | no | humidity |
| `windchill_c` | `windchillf` | °C | °F → °C | 1 | | no | temperature |
| `heatindex_c` | `heatindexf` | °C | °F → °C | 1 | | no | temperature |
| `pressure_abs_hpa` | `absbaromin` | hPa | inHg → hPa | 1 | | no | atmospheric_pressure |
| `rain_weekly_mm` | `weeklyrainin` | mm | in → mm | 1 | | no | precipitation |
| `rain_monthly_mm` | `monthlyrainin` | mm | in → mm | 1 | | no | precipitation |
| `rain_yearly_mm` | `yearlyrainin` | mm | in → mm | 1 | | no | precipitation |
| `battery_low` | `lowbatt` | 0/1 (the console's value is kept, 0-100) | — | 0 | | no | battery (binary) |
| `gust_dir` | `windgustdir` | ° | — | 0 | | no | wind_direction |
| `wind_avg2m_kmh` | `windspdmph_avg2m` or derived | km/h | mph → km/h | 1 | | no | wind_speed |
| `wind_dir_avg2m` | `winddir_avg2m` or derived | ° | — | 0 | | no | wind_direction |
| `gust_10m_kmh` | `windgustmph_10m` or derived | km/h | mph → km/h | 1 | | no | wind_speed |
| `gust_dir_10m` | `windgustdir_10m` | ° | — | 0 | | no | wind_direction |
| `visibility_km` | `visibility` | km | nmi → km | 1 | | no | distance |
| `rain_rate_mmh` | `rainratein`; `rainin` under `yt60234` | mm/h | in → mm | 1 | | no | precipitation_intensity |
| `wind_avg10m_kmh` | none: derived only (§6.2) | km/h | — | 1 | | no | wind_speed |

Sanity bounds (rule `bounds`), checked after conversion: temperatures -60 to 80 °C, humidity 0-100 %, pressure
300-1100 hPa, wind and gust 0-500 km/h, rain today and last hour 0-2000 mm, rain week, month and year 0-30000 mm, rain
rate 0-1000 mm/h (`rainin` read as a rate under `yt60234`: 0-2000), UV 0-30, solar 0-2000 W/m², visibility 0-200 km, `lowbatt` 0-100 (only 0 and 1 mean anything: any other value reaches Home Assistant as unknown), directions -360 to 720 (`wind_dir`
is then wrapped to 0-359; the other direction fields are kept as sent). Non-finite values are always rejected.

**Profiles.** `yt60234` ("Vevor/Youtong 7-in-1", the default) and `generic` ("Generic WU"). A new station whose console fingerprint clearly mismatches the 7-in-1 starts as `generic` (§2.2).

| | `yt60234` | `generic` |
|---|---|---|
| `rainin` means | rain **rate** in/h → `rain_rate_mmh` | past-hour rain → `rain_mm` |
| `rain_mm` | derived (below) | as sent |
| Wind/gust above 195 km/h | wind, gust, direction dropped (`profile_wind`) | — |
| Humidity below 3 % | whole sample dropped (`profile_humidity`) | — |
| Daily rain counter | a fall to more than 0.5 mm is a glitch (`rain_glitch`); a fall to 0.5 mm or less is the console's reset, at any hour | any fall is taken as a reset |
| Factory pressure | 1013.2 hPa (29.92 inHg) → `baro_uncal` after 60 uploads in a row at that value | none |

The `yt60234` column rests on one verified unit (`docs/station-profiles.md`): `rainin` as a rate and the factory pressure were checked on that unit, the rain rate and the daily total by counting bucket tips, not yet in real rain. The `generic` profile has not been tested on a console.

### 6.2 Derived fields

Computed by fauxcloud when the console does not send them. Listed in `/api/live` `derived` (with reading counts) or
`warming`, and tagged in MQTT attributes `{"source":"derived",...}`.

| Key | How |
|---|---|
| `rain_mm` (`yt60234`) | rain fallen in the last 60 min = sum of the daily-counter increases in the recent ring (reset-aware). Until the ring spans an hour after a boot: an **estimate from the hour records** — the open hour (this upload included) plus the previous closed hour's share still inside the last 60 min (`prev × (3600 − seconds into the hour) / 3600`), calibrated like `/api/hourly`; tagged `sources.rain_mm = "hours"`, MQTT attribute `{"source":"hours","n":N}` (n = the readings of the hours used). The previous hour is left out (open hour alone) when it has fewer than 42 readings or carries `gap`, `rain_partial` or `console_reset`. An unplanned reset can lose the snapshot's tail (≤ 10 min) of the open hour. Without a record of the directly preceding clock hour (for example after a restart longer than an hour), with the open or the previous hour lacking a rain value, with an approximate clock or with no clock it is unavailable (`warming`, HA availability `offline`). Once the ring spans the hour the ring value takes over (`source` back to `derived`). |
| `wind_avg2m_kmh` | mean of `wind_kmh` over the last 2 min |
| `wind_dir_avg2m` | speed-weighted vector mean of `wind_dir` over the last 2 min, leaving out readings under 1.0 km/h. With only calm readings in the window there is no new value and the last one stays (aged), as for any derived field with nothing in its window. |
| `gust_10m_kmh` | max of `gust_kmh` over the last 10 min |
| `wind_avg10m_kmh` | mean of `wind_kmh` over the readings in the last 10 min, both ends included (`[now − 600 s, now]`: the same number forwarding sends as the wind). Always derived (no upload parameter; not in the history rings). In `/api/live` `values`, `age` (seconds before the last upload, like every field) and `derived` (reading count) once the station has sent wind; absent while `wind_kmh` is hidden and after a restart until the next upload. With no wind reading in the window the last value stays with its age; once that age passes 600 s the value reads `null`. MQTT sends it only from an upload whose window held wind. |

### 6.3 Filter rules

`rej` and `rej_<rule>` count the uploads in which each rule fired.

| Rule | Fires when |
|---|---|
| `parse` | a value is not a clean number |
| `bounds` | a value is outside the sanity bounds |
| `profile_wind` | profile wind limit (wind fields dropped) |
| `profile_humidity` | profile humidity floor (whole sample dropped) |
| `limits` | a core field is outside the station's configured min/max (compared before calibration) |
| `jump` | a core field changed faster than the station's configured per-minute limit, scaled by the time since its last accepted value (`wind_dir` by the shorter arc) |
| `rain_glitch` | the daily rain counter fell to more than 0.5 mm (`yt60234` only; a fall to 0.5 mm or less is the reset) or rose by more than 100 mm in one upload; after 3 such uploads in a row the counter is taken as the new base |

**Stale sensor.** The outdoor sensor set (`temp_c`, `humidity`, `dewpt_c`, `wind_dir`, `wind_kmh`, `gust_kmh`,
`uv`, `solar_wm2`) repeating the previous upload's values 10 times in a row (110 times, about 2 h, when wind and gust are both 0, i.e. calm air; an absent set counts as repeated)
sets `sensor_stale`; the first change clears it. While it is on, the outdoor set is not published or recorded and its values keep their age; pressure and rain go on.

### 6.4 Notices

Plain-English remarks, the same in `/api/live` (`station.notices`), `/api/bridge/status` (`notices`) and the HA
"Notices" sensors. The `id` is stable; `chip` and `text` are UI wording and may change. At most 5 at a time per station or
bridge (never more than one pressure notice). The order is fixed: for a station sensor, pressure, key, console clock, profile; for the bridge time (or `clock_behind`, which replaces the time notices), memory, history writes, restart, storage layout. `HH:MM` is local time, with the day when it is not today's ("yesterday 14:05", "16 Nov 14:05"; HH:MM alone
while the clock is not synced; `--:--` for a time before the clock was set). The HA copy picks up a new day with its periodic re-send.

Each item carries its code and parameters: `{"id", "since"?, "n"?, "skew_s"?}` (`baro_ref`,
`baro_uncalibrated` and `baro_check` add theirs, below). `since` is ISO UTC (omitted when
unknown); `n` is a count (`key_mismatch`, `store_fail`); `skew_s` is the console's clock difference in seconds,
rounded as the text shows it (`console_clock`). The HA attributes (`.../notices/attr`) carry exactly these
parameters. The API item keeps `chip` and `text` after them as an English convenience.

| id | Where | chip | text |
|---|---|---|---|
| `sensor_stale` | station | Sensor not updating | Outdoor sensor not updating since HH:MM. A calm night can do this; a long gap means its battery or radio. |
| `sensor_absent` | station | Sensor missing | Outdoor sensor missing since HH:MM: check its batteries. A short battery-out of the console helps it find the sensor. |
| `baro_uncalibrated` | station | Pressure at factory value | The console's pressure is at its factory value; set REL once in its SET menu. |
| `key_mismatch` | station | Key changed | Another key arrived for this station (N uploads ignored). Re-paired the console? Accept it in Settings. (N = 1: "1 upload ignored") |
| `console_clock` | station | Console clock off | The console's clock is off by N min/h/days; switch NTP off and on in its SET menu. |
| `profile_changed` | station | Profile changed | Station profile changed: HH:MM. (`since` = when; shown for 24 h, counted at the station's uploads; RAM only) |
| `time_approximate` | bridge | Time approximate | Time is estimated since the restart. Sign in to the admin page once on your phone, or connect fauxcloud to the internet. |
| `time_none` | bridge | Waiting for time | Waiting for the real time. Sign in to the admin page once on your phone, or connect fauxcloud to the internet. |
| `heap_low` | bridge | Low memory | Memory is low; readings still arrive. If this stays, restart fauxcloud in Settings. |
| `store_fail` | bridge | History not saved | History not saved (N writes failed); readings still arrive. If this stays, restart fauxcloud in Settings. (N = 1: "1 write failed") |
| `heap_frag_restart` | bridge | Restarted | fauxcloud restarted itself to free memory; nothing to do. (For 24 h after such a restart; `last_reset` = `heap_frag`.) |
| `store_layout` | bridge | History not saved | History not saved: this firmware needs one USB flash to prepare its storage (see the README). (The partition table is not this firmware's or LittleFS holds another build's ring layout; nothing is formatted, history is RAM only.) |
| `clock_behind` | bridge | Clock behind | The time is behind the saved history, so history is paused until the time is confirmed (at most 24 h). |
| `baro_ref` | station | Pressure looks off | This station's pressure reads about D hPa above/below nearby airports since HH:MM. Set REL in the console's SET menu to about T hPa. (Without a current reference report the ending is "…to the current sea-level pressure of a nearby official station.") |
| `baro_check` | station | Pressure looks off | This station's pressure averaged about N hPa above (below) the usual for this place over N days. Set REL in the console's SET menu to the current sea-level pressure of a nearby official station. (from the learner's pressure check, `l7` in `/api/learn`; the days shown up to 60. Parameters `off_hpa` (signed, rounded to 5) and `days` (admitted, not capped); re-judged with the station's pressure offset in force; silent while METAR is eligible (on, location set, an airport within 60 km), under `baro_uncalibrated` or `baro_ref` (one pressure notice at most), without a location or a built-in normal) |

`baro_ref` (the REL hint): with the airport module on, the station's pressure (the recent readings' mean over the
report's time +- 5 min, as shown) is compared with the nearby airports' QNH once per new reference report, in the airport
job (never per upload). Admitted only with a consistent QNH from the current airports within 60 km (a farther
airport is left out, it does not refuse the sample), a report at most 60 min old, a station not at the factory value and
with a steady own 3-h tendency (no front in between; a stale outdoor sensor does not block it, the pressure is the
console's). It appears after
3 admitted reports in a row differ by at least 2.0 hPa with the same sign, and clears at the first under 1.5 hPa, after
3 h without an admitted report, with the module off or under `baro_uncalibrated` (never both). RAM only. Parameters:
`d_hpa` (the median difference of the last 3, signed), `qnh_hpa` (the airports' QNH of the newest admitted
report, the number the sheet quotes), `target_hpa` (QNH minus the station's pressure offset, `null` while
the reference report is older than 75 min), `ref_t` (that report's time), `since` (the run's first report).
`baro_uncalibrated` gains `last_hpa`, `target_hpa` and `last_t`: the last pressure not at the factory value (after a
restart the newest such closed hour) and the REL value for it, only when at most 3 h old.

Every text is at most 127 bytes at its longest parameters, except the two pressure ones: `baro_ref` at most 188 bytes and `baro_check` at most 191. The HA "Notices" state joins the texts with a space (each is a sentence); when they pass 255 bytes it joins the chips instead, each once (`Waiting for time. Low memory. History not saved.`), and only those are cut with `…`.

`console_clock` appears when the console's `dateutc` differs from fauxcloud's clock by more than 10 min. While summer time is in force in the bridge's zone, a console that is 1 h ahead (within 5 min) is tolerated: some consoles are reported to send local summer time.

```json
"notices":[{"id":"sensor_stale","since":"2026-11-05T02:40:00Z","chip":"Sensor not updating","text":"Outdoor sensor not updating since 03:40. A calm night can do this; ..."},
           {"id":"console_clock","skew_s":-4020,"chip":"Console clock off","text":"The console's clock is off by 67 min; switch NTP off and on in its SET menu."}]
```

---

## 7. Stability and versioning

fauxcloud is pre-1.0. There is no API version field and no compatibility promise yet; the firmware version is the
only version.

| Part | Status |
|---|---|
| Upload path, its `success` answer, DNS/NTP behaviour | Stable: dictated by the consoles |
| `/api/live?station=` station object (documented keys), `/api/bridge/status`, `/api/recent`, `/api/hourly`, `/api/daily` | Intended for integrators. Keys may be **added**; existing keys are not expected to change meaning. Ignore unknown keys. |
| `station.outlook` (§3.2.2) and the `outlook` MQTT sensor | Shape and codes are **append-only**; treat an unknown code as absent. Thresholds (§3.2.3) are defaults and may be tuned. |
| Field keys (§6.1), rule names, notice ids, profile ids | Stable: field keys and rule names are MQTT topic leaves; notice ids and profile ids are values integrations match on |
| Entity ids in HA | Discovery sends `default_entity_id` = `<component>.fauxcloud_<id>_<key>` (slugged, §5.2), so a new install gets these ids on HA 2025.10+. Entities created before `default_entity_id` was sent keep the ids HA gave them (from the names); `unique_id`s are unchanged |
| MQTT topics, retain policy, discovery `unique_id`s | Stable |
| `diag`, `/tools`, `/api/station`, `/api/bridge`, `/api/learn`, `uptime_s` | Internal: may change any release |
| `/api/metar` (§3.2.5) and the `wufc` block of `/api/live` (§3.2) | Reference data from the online services, LAN face only, absent in the HTTP-only build: keys are only added; their details (wording, which phenomena are reported, thresholds) may change in a minor release |
| `/api/admin/*`, `/api/setup/*` | Internal to fauxcloud's own pages; usable, but may change any release (forwarding's `n` / `causes` / `diag` / `heap` included) |
| Notice `chip`/`text`, entity names, the `known`/`extras` details | UI wording: may change |
| Asset paths `/a/<hash8>/...` | Change when the file's content changes (the hash is of its compressed bytes); never hard-code them |

**Version strings.** The release version (e.g. `1.0.0`; a pre-release build carries a suffix after the number) with `+diag` appended in a bench build (e.g. `1.0.0+diag`). Shown as `fauxcloud 1.0.0`
in `diag.fw_version`, the HA bridge `firmware` entity and every discovery `sw_version`; `diag.build` is
`"<version> <date> <time>"` of the running image; the OTA answer returns the same string for the uploaded image.
