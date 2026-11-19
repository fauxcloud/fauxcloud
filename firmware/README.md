# firmware/ — build, flash, monitor

Toolchain = **Arduino-ESP32 core 3.x via `arduino-cli`, pinned per sketch in `sketch.yaml`**.
The profile is the "venv": whatever is installed globally is irrelevant; CI compiles the same profile.
All arduino-cli data lives in the project-local `.arduino/` (gitignored), so nothing leaks into or out
of the machine's Arduino IDE installation.

## One-time setup (Windows)

```powershell
winget install ArduinoSA.CLI --version 1.5.1
```

arduino-cli itself is pinned at **1.5.1**, the version CI installs (`version:` of `setup-arduino-cli` in
`.github/workflows/build.yml`); bump both together. `arduino-cli version` shows what is installed.

Then from the repo root:

```powershell
tools\build.ps1 discovery
```

The first run downloads the pinned ESP32 platform into `.arduino/` (several hundred MB). The wrapper
passes `--config-file arduino-cli.yaml` so the project-local directories are used.

Pinned core version: **esp32:esp32 3.3.12** (newest in the index on 2026-11-01; see `sketch.yaml`). To see what
exists: `arduino-cli --config-file arduino-cli.yaml core search esp32 --all`. Bump the pin deliberately, in its
own commit, and re-run the AP-isolation test afterwards (the laptop test below).

Flash budget (core 3.3.12): `discovery` 923 KB (70 % of the default 1.25 MB app slot); an early MQTT prototype 1 075 KB
(**82 %** default, 54 % with `PartitionScheme=min_spiffs`). min_spiffs gives 2 × 1.875 MB app but only
**128 KB** SPIFFS/LittleFS (`tools/partitions/min_spiffs.csv` in the core), too small for the planned daily
history files. fauxcloud ships its own `partitions.csv` in the sketch folder, **frozen for all of 1.x**:
2 × 1.81 MB app (0x1D0000) + 40 KB coredump + 280 KB LittleFS (70 blocks, one history ring: 131 days hourly,
736 days daily), NVS unchanged at 0x9000 / 20 KB. `tools/check_partitions.py` (run by `build.ps1` and CI) checks it and
the PSRAM profile's own `partitions-psram.csv` (2 × 1.75 MB app, 32 KB coredump, 416 KB / 104 blocks,
two rings) and refuses any edit of `partitions.csv` (`partitions.csv.sha256`): a new table is a new board profile.

**The one cable step from an older table (once; NVS kept):** export the CSVs you want to keep first (the history
restarts), then

```powershell
.arduino\data\internal\esp32_esptool_py_5.3.1_99ef3036ba521408\esptool.exe --port COMx erase-region 0x1E0000 0x220000
tools\build.ps1 fauxcloud -Port COMx -Upload
```

(esptool 5.x from the pinned core: `.arduino/data/internal/esp32_esptool_py_*/esptool.exe`; a bare `esptool` is not on
PATH.)

The erase covers the new app1, the coredump and LittleFS (0x1E0000 to the end of flash) and nothing in front: the
bootloader, the table area, **NVS** (settings, keys, learners, forwarding, airports) and otadata stay. The first boot
formats LittleFS once (`STORE LittleFS formatted … of 280 KB`); then one OTA puts an image into
app1 so rollback is armed. Without the cable step an OTA keeps the old table: the firmware keeps history in RAM only
and shows the `store_layout` notice (it never formats); an older image OTA'd onto the new table does the same. Never
flash a merged factory image over a working board (it pads 0x9000–0xE000 and so erases NVS; the core's own
`*.merged.bin` is padded to the whole 4 MB and erases LittleFS too).

**Build through `tools\build.ps1` (or the CI command in `.github/workflows/build.yml`), not a bare `arduino-cli compile`.**
A `sketch.yaml` profile cannot carry compiler flags, and the firmware is built with `-fno-exceptions` for every C++
unit we compile (core, core libraries, sketch): the core's own flags file adds `-fexceptions` after
`compiler.cpp.extra_flags`, so both pass the flag through `--build-property build.extra_flags=…` (the core's value of that
property plus `-fno-exceptions`; mirror it on a core bump). It saves ~26.6 KB. A guard
after every build reads the ELF and fails when `.eh_frame` exceeds 8 192 B (≈ 4.6 KB is left from the prebuilt ESP-IDF
archives; ≈ 23 KB means the flag was lost) or a `.gcc_except_table` section exists. No code of ours uses `try`/`throw`.

`tools\build.ps1` builds into `firmware/<sketch>/build/<profile>/` — one directory per profile. Never point
two profiles at one build directory: mixed artifacts produced a non-bootable flash once.

**Keep the checkout path short:** about 50 characters at most (`C:\src\fauxcloud` is well within it). The deepest
compiler header path used (`…\esp32\psram\no-rtti\bits\c++config.h` under `.arduino`) is ≈ 200 characters longer than
the checkout's own path; past ≈ 60 characters it exceeds Windows' 260-character MAX_PATH, and every compile fails with
`bits/c++config.h: No such file or directory`. Windows' long-path setting is not needed and not changed by the project.

**A git worktree in a deeper folder** can share the main working tree's `.arduino` through a junction. For a worktree
under `.claude\worktrees\` (the folder `tools\build.ps1` recognises) whose `.arduino` is such a junction, the script
points arduino-cli at the junction's target through `ARDUINO_DIRECTORIES_DATA` / `_DOWNLOADS` / `_USER`, so the
header paths stay as short as the main working tree's, and prints one line saying so. A value already set by the
caller wins.

**Windows execution policy:** a default PowerShell (`Restricted`) refuses local scripts. Either run them as
`powershell -ExecutionPolicy Bypass -File tools\build.ps1 discovery`, or once per user
`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` (your choice; the project does not change it for you).

## Web pages (`web/` → `src/web_assets.h`)

The pages are baked into the firmware by `python -I tools/gen_web_assets.py` (`--check` in CI, `--sizes` for the
budgets). It runs every JavaScript bundle through **terser 5.36.0** (mangle only), pinned in `tools/package.json` +
`tools/package-lock.json`. Once per checkout:

```powershell
cd tools; npm ci --ignore-scripts; cd ..
python -m pip install --require-hashes --only-binary :all: --target tools/.pylib -r tools/requirements.txt
```

The second line installs **zopfli 0.2.3.post1** (hash-pinned in `tools/requirements.txt`) beside the generator in
`tools/.pylib` (gitignored; `python -I` sees no user site-packages). The gzip bytes come from zopfli (15 iterations,
mtime 0): about 7.8 KB smaller than gzip -9 on the pages; a missing module is a hard error, never a gzip fallback.

The generator uses `node` from `PATH`, or the binary named by `FAUXCLOUD_NODE` (any node 20+ works), and stops with an
error when neither is there: a silent fallback would change every asset hash. Its output is byte-identical on every
machine. Budgets (gzip): initial page ≤ 34.0 KB dashboard (its words and the language code), ≤ 30 KB admin, ≤ 20 KB
setup; a first visit in Polish (the initial page plus its initial packs) ≤ 38.6 KB dashboard, ≤ 34.3 KB admin; every lazy
file ≤ 13 KB. The web files' share of the image is capped as code ≤ `WEB_CODE_MAX` (149 307 B: scripts, stylesheets, page
shells) plus assets ≤ `WEB_ASSET_MAX` (52 000 B: data packs, images, texts) plus the language packs ≤ `WEB_LANG_MAX`
(29 429 B), `WEB_FLASH_MAX` = their sum. A feature raises a cap by its measured cost; the image gate (95 % of app0) is
the hard limit. The budgets a language pack edit can move (code, the packs, the dashboard's initial, each first visit)
carry 64 B of hash-noise margin over their measured figure: the pack's new hash in the manifest moves the bundle's gzip
by up to about 41 B. The dashboard's initial carries the eleven lower-case measure names of the tiles' "Open … history"
labels as their own words: code never lower-cases a translated text, so they cost their bytes (+82 B). The home-screen icon `web/touch-icon.png` is a committed render of the mark:
`python tools/gen_touch_icon.py` (needs pillow + resvg_py; `--check` compares).
`tools/coldload.py <board> --station <ID> --upload-id <ID>` is the two-device cold-load check behind them. It needs
"Accept uploads from my home network" on, and its upload id becomes a real station (plus an HA device when auto-publish
is on): forget it in the admin afterwards.

The site's `/try/` demo is built from these same pages: `tools/build_site.py` imports the generator and emits its shells
and assets unchanged (one script tag added per shell); `python -I tools/build_site.py --check` runs in CI (`site/README.md`).

The page logic has Node vectors (no browser, no board), all run in CI: `crypto_vectors.js`, `tendency_vectors.js`,
`outlook_vectors.js`, `learn_vectors.js`, `format_vectors.js` and `core_vectors.js` (`core.js` requests — a lazy
script answering 404 after an OTA is an error, a JSON 404 is data), e.g. `node test/tools/core_vectors.js`.
Also: `range_vectors.js` (the history sheet's date ranges), `text_vectors.js` and `contrast_check.js` (WCAG
contrast of the caption inks per theme and face). Laptop preview of the pages from your checkout with a board's live
data, read-only: `python -I test/tools/preview_proxy.py --board <board IPv4>` then open `http://localhost:8377`.

## Flash + monitor

```powershell
tools\build.ps1 discovery -Port COM5 -Upload -Monitor
```

## Sketches

| Sketch | Purpose |
|---|---|
| `discovery/` | A listen-first logger: AP + wildcard DNS + NTP responder + raw HTTP logger + TCP/443 probe. Logs what the console does. |
| `fauxcloud/` | **The product firmware.** AP + STA, DNS + NTP responders on the AP side, `esp_http_server` on both interfaces, MQTT with Home Assistant discovery, custom `partitions.csv`. Logic in `src/` (host tests in `test/host/`). |

## Serial capture

`arduino-cli monitor` may refuse the port ("No monitor available for the port protocol serial" was seen on
Windows). Use the project's capture tool instead; it writes `capture-<timestamp>[-tag].log` into the repository's `captures` folder (gitignored):

```powershell
tools\capture.ps1 -Port COM5 -Reset -Tag boot
```

`-Seconds N` stops after N seconds, `-Reset` pulses EN via RTS so the log starts at boot.

## Laptop test of `discovery`

Automated: start `tools\capture.ps1 -Port COM5 -Tag laptop` in one terminal, join Wi-Fi `fauxcloud-setup`
(password in the sketch), then in a second terminal run `tools\laptop-test.ps1 -LanTarget <a LAN host IP>`.
Results land in `laptop-test-<timestamp>.log` in the same `captures` folder; every line must say PASS / BLOCKED. Manual equivalents:

```powershell
ipconfig /all
```

```powershell
nslookup rtupdate.wunderground.com 192.168.77.1   # discovery sketch: 192.168.4.1
```

```powershell
curl.exe "http://192.168.4.1/weatherstation/updateweatherstation.php?ID=TEST&PASSWORD=TEST&dateutc=2026-11-01+12:00:00&baromin=29.92&tempf=50.0&humidity=60&dewptf=37.0&rainin=0.00&dailyrainin=0.12&winddir=180&windspeedmph=3.0&windgustmph=5.0&UV=1&solarRadiation=120.0"
```

```powershell
w32tm /stripchart /computer:192.168.4.1 /samples:1
```

```powershell
curl.exe -k --max-time 3 https://192.168.4.1/
```

Expected serial lines: `[AP] client joined`, `[AP] DHCP lease given`, `[DNS] rtupdate.wunderground.com type=1`,
`[HTTP] GET /weatherstation/... -> WU-UPLOAD reply "success"` with every `arg`, `[NTP] request ... answered`
(only with `secrets.h`), `[TCP443] connection attempt`.

## Bench: setup end-to-end

Run before any image that touches `app_http.cpp`, `web/setup*`, `web/picker.js` or `web/core.js` (no other test covers the whole
setup walk). **A spare board only**, never your everyday board: it starts from a
factory reset (admin → Maintenance).

1. The board shows the setup Wi-Fi `fauxcloud-setup-XXXX`. Join it with the laptop and run
   `tools\laptop-test.ps1 -Setup`: steps S1–S3 must say PASS (the page names `/a/<hash>/tz.json`; the zone list answers
   `200`, `application/json`, ≥ 300 zones; `/api/setup/state` is JSON).
2. Join the same Wi-Fi with a phone; the setup page opens (captive portal) or answers at `http://192.168.77.1/`.
3. Step 1: pick the home Wi-Fi and enter its password. Step 2: the place list loads, the phone's own zone is offered,
   pick a place; set an admin password. Apply.
4. The bridge restarts in run mode and joins the home Wi-Fi; the admin page answers on the home network at the address
   the last setup screen showed. Sign in once with the new password.

## Forwarding: variants, roots and the fake service

- **Two variants.**
  - **The release (`FWD_TLS` 1, the default in `src/fwd.h`)** builds the **BearSSL** client in (`src/tlsc.{h,cpp}` over the
    vendored `src/bearssl/`, the only outbound TLS path). It gives HTTPS per target (a new target starts on HTTPS,
    `FWD_HTTPS_DEFAULT` 1; plain HTTP stays a per-target choice) and airport weather (METAR + TAF, `/api/metar`, the
    dashboard's airport view): `tools\build.ps1 fauxcloud`. CI checks the ELF with `nm`: BearSSL never allocates, our
    objects use no core TLS client, the anchors and the TAF parser are in.
  - **The HTTP-only fallback (`-Define FWD_TLS=0`, its own build dir)** sends **plain HTTP only** and links no TLS client
    and no airport weather (CI: no `br_` symbol, no anchors, no METAR / TAF code). It is a CI cell, not a download.
  - Both images carry the app0-slot gate (fail above the slot, warning at 95 %).
  - The host tests run both (`test/host/run.ps1`, `run.sh`; BearSSL compiled as C with every OS entropy / time source off,
    as on xtensa).
- **Station count per profile.** `src/profile.h` sets `FC_MAX_STATIONS` from the board: **1 on the WROOM-32** (measured: one
  9 152-B station slot next to the web pages and HTTPS forwarding), 2 on the PSRAM profile (`BOARD_HAS_PSRAM`, a placeholder until a PSRAM
  board is measured). `-DFC_MAX_STATIONS=2` on a plain WROOM build is refused at compile time by a `static_assert`; the CI cell for two
  stations builds on the PSRAM fqbn (`PSRAM=enabled`). The host tests run three variants by themselves (`run.ps1` / `run.sh`: default = the TLS release, `FWD_TLS=0`, `FWD_TLS=1 FC_MAX_STATIONS=2 FC_DIAG_BENCH=1`).
- **Bench diagnostics and the diag image.** `FC_DIAG_BENCH=1` (default 0, `src/diag_bench.h`, included by
  `src/fwd.h`) builds the R&D diagnostics in: `diag.tasks`, the send low-waters, the forwarding tick in parts, the METAR
  page-idle statistics, `ota_heap`, `http_busy_ms_max`, the learner gates and replay figures, the `last.diag` stack / curve
  / record extras (the key list: `docs/api.md` §3.2.1 "Bench build"). The release neither computes nor renders them (the
  keys are absent), and CI checks both WROOM images (the TLS release and the HTTP-only fallback) with `tools/check_diag_bench.py` (ELF symbols, the `.bin`'s key
  strings, the snapshot reference; a missing file fails; a self-test runs the release check on the diag image and expects
  it to fail). **The diag image is published next to every release** as `fauxcloud-<ver>-diag.bin` (`FWD_TLS=1,
  FC_DIAG_BENCH=1`, the WROOM-32 option, one station; CI artifact `fauxcloud-diag-bin`). Its version says `+diag`
  everywhere (the admin page, `fw_version`, Home Assistant, the services' User-Agent). For a bug report that needs more
  than the release's keys: install it through the admin page's Maintenance → Firmware update, reproduce, download the
  boot log and the JSON, then install the release `.bin` back the same way (rollback armed both ways). Local build:
  `tools\build.ps1 fauxcloud -Define FWD_TLS=1,FC_DIAG_BENCH=1`. The host suite runs it in the st2 variant.
- **BearSSL (MIT), vendored and pinned:** `python -I tools/vendor_bearssl.py --check` (CI) checks every file against
  `src/bearssl/VENDORED.txt` (commit, per-file SHA-256). **Bump:** `python -I tools/vendor_bearssl.py --commit <40-hex>`
  (clones upstream into a temp dir, copies the device list into `src/bearssl/` and the host-only list into
  `test/host/bearssl/`), review the upstream diff of the vendored files only, run the host tests
  (they run our client against a local TLS server), then one OTA, a test send to each HTTPS service and a Fetch now of the airports and the WU
  forecast from the admin page. Upstream files are never patched.
- **TLS test PKI:** generated by the host test at run time (an RSA-2048 test root, a P-256 test.example certificate);
  no private key is ever committed.
- **Trust anchors** (`src/fwd_roots.{h,c}`, generated as BearSSL `br_x509_trust_anchor` structs, `FWD_TLS=1` only): six roots + two cross-signed anchors pinned by SHA-256 in `tools/gen_fwd_roots.py`, source
  `tools/fwd_roots.pem`; `python -I tools/gen_fwd_roots.py --list` shows each NotAfter, `--check` runs in CI.
  **Maintenance list:** GlobalSign Root CA (Windy) ends **2028-01-28** — put the successor's fingerprint in `ROOTS`,
  `--import` it, regenerate. The others end 2035–2040.
- **Fake service for the board's failure tests**: `python -I tools/fwd_fake_endpoint.py --port 8089` on the laptop,
  then a "WU-compatible server" (plain HTTP) at `<laptop>:8089` with a path such as `/stall15/x` or `/wu-badkey/x`;
  `--list` shows the cases. It logs the request line with the key as `***`.

## Learners: offline checks

`test/host/forecast_replay.cpp` (built by `test/host/run.ps1` / `run.sh` into `test/host/build/`) replays the outlook on
saved board data, nothing installed on the board:

- `forecast_replay --what-if hourly.json bridge.json learn.json [--all]` -- before an OTA that lets learners graduate:
  per closed hour the outlook codes with the fixed rules against the codes with the learners in use (from the board's
  `/api/learn`: the parts whose `state` is `in_use`; `--all` = every learner's current estimate), the changed hours and a
  count per code. Inputs: `/api/hourly?station=<ID>&from=<oldest>` (the calibrated rows), `/api/bridge`, `/api/learn?station=<ID>`.
  The clear-sky learner's dome, geo and indoor gates are not modelled (the per-cell state does not carry them).
- `forecast_replay hourly.json live.json bridge.json [learn.json]` -- the board's codes at one moment (MATCH / DIFF);
  with `/api/learn` its `use` block feeds the learners in use, so `conf` is compared too.
- `forecast_replay --learn hourly_raw.json daily_raw.json bridge.json` -- the learners from priors, as the board's
  `/api/learn` (see the file's header).

`tools/mslp_normals.py` generates `src/mslp_norm.h` (the long-term sea-level pressure behind the "Pressure looks off" notice) once from two NOAA PSL files; see its docstring.

## Crash data

After a crash, admin -> Maintenance offers the crash data (the coredump partition). It asks for your admin password
and clears the crash data on fauxcloud once it was sent. **The file contains your passwords.** Never share it — paste the decoded backtrace from `tools\decode-coredump.ps1`. A factory
reset erases it too.

## Optional `secrets.h` (gitignored)

```c
#define STA_SSID "your-home-wifi"
#define STA_PASS "your-password"
```

Gives the discovery sketch a real clock through your home Wi-Fi so its NTP responder can answer the console.

## The leak gate

`tools/leakgate.py` (stdlib Python) checks what the repository publishes: every tracked file must be classified in
`tools/publish-manifest.txt` (`ship` or `review`; the format is at the top of that file; a tracked file the manifest
classes `private` is refused, a public tree holds none), and the shipped files must carry no private paths, decision
or block references, private-range addresses other than the documented ones, MAC addresses, coordinates,
home-directory paths, Polish text outside the translation data, unlisted binaries or image metadata. A private
deny-list adds your own patterns (home network, station id, names); it is matched against the text, every byte of a
binary and every path. The gate prints `file:line: class` and never the matched text (a matching path is printed as
`<denied-name #n>`). Known false positives go to `tools/leakgate-allow.txt`.

```
python -I tools/leakgate.py --manifest tools/publish-manifest.txt --deny <your deny-list>
python -I tools/leakgate.py --tree site/_build --bin <image>.bin            # a built site; a firmware image
python -I tools/leakgate.py --install-hook --deny <your deny-list>          # the pre-push hook
python -I -m unittest discover -s test/tools -p test_leakgate.py
```

The pre-push hook checks, for every pushed branch or tag, the tip's whole tree, every file that any pushed commit
adds or changes (so a line added and removed again inside one push is still caught), every pushed commit message and
an annotated tag's text; it reads committed bytes, never a checkout. The hook and CI run with `--strict-tags`, so a
block or review tag fails as well. CI runs the gate as the `leakgate` job, and a failure fails the run (the
deny-list from the `LEAKGATE_DENY` secret, base64, when the repository has one); gitleaks runs too when it is on PATH
(`--no-gitleaks` skips it). `--source-is-private` is only for a source tree that holds private files on purpose (the
input of an export), never for CI or the hook.
