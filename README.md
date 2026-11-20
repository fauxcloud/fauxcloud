<h1 align="center"><a href="https://fauxcloud.org"><img src=".github/fauxcloud-header.svg" alt="fauxcloud" width="280"></a></h1>

<p align="center"><a href="https://fauxcloud.org/install/">Install in the browser</a> · <a href="https://fauxcloud.org/docs/get-started/">Get started</a> · <a href="https://fauxcloud.org/try/">Try the demo</a></p>

**Your weather station's cloud, in your own home.**
Keep your Wi-Fi weather station working, without the internet, an app or router tricks.

fauxcloud is a local weather bridge for Wi-Fi weather stations. It is free, open-source firmware for a plain ESP32
board (about €5, no soldering).

The board runs a small Wi-Fi network just for the console (your weather station's display unit). It plays the part of
the website the console wants to upload to (Weather Underground). So the console keeps sending its readings to a board
on your shelf instead of the cloud. fauxcloud keeps a record of each console it serves and calls it a station.

You read the weather on a page on your own network, which you can pin to your phone's home screen. If you use Home
Assistant, the readings also appear there as ready-made entities. fauxcloud keeps a history on the board and tells you
when something stops reporting.

Nothing leaves your house by default. If you want, and the board has internet through your home Wi-Fi, you can turn
on online services (send your readings to Weather Underground, Windy and others) and airport weather.

fauxcloud works with consoles that upload to Weather Underground; it is not made by or affiliated with any of the
brands named here (see [the last section](#names)).

```
outdoor sensor ──radio──▶ weather console ──Wi-Fi──▶ fauxcloud (ESP32: its own Wi-Fi, answers the upload)
                                                         └──home Wi-Fi──▶ dashboard · Home Assistant (MQTT)
                                                                          · online services and airport weather (off until you turn them on)
```

**Try it without a board:** a demo of the dashboard and **Settings** with example data is at
[fauxcloud.org/try](https://fauxcloud.org/try/).

## Who it is for

- **"My station stopped making sense."** The temperature reads nonsense, the outdoor values froze, or the pressure
  jumped. fauxcloud cannot repair hardware, but it names the part that went quiet ("Sensor not updating", "Offline
  since 14:05", "Pressure at factory value", "Console clock off") so you know where to look.
- **"My console never got online."** Weather Underground now refuses uploads without a parameter this console does not
  send, so its own upload may not arrive. This was found by replaying the console's request against the service, not
  on the console itself. With fauxcloud the console uploads to your own board. If you still want a public page,
  fauxcloud can forward your readings with the missing parameter added.
- **"I use Home Assistant."** fauxcloud publishes wind, gusts, rain, temperature, humidity, pressure, UV and solar
  over MQTT (a small message system Home Assistant uses) with discovery, so they appear as entities by themselves.
  Nothing calls home, and a quiet station shows as unavailable instead of a frozen value.

## Will it work with my console?

**Verified on one unit: Vevor 7-in-1, model YT60234, firmware 1.0.1, EU 868 MHz.**

**If your console's setup page has a Weather Underground box, it may work (not tested yet; a console report tells us).
If it pairs only through an app, it will not.**

- **Likely, not yet tested:** the same console sold as SucceBuy, and other consoles with the same setup page (a `WWS-`
  network, a page at `http://192.168.1.1` with a Weather Underground box).
- **Not supported:** the model that pairs through the SmartLife / Tuya app (YT60307), and any console that uploads
  only over HTTPS.

"Verified on one unit" means exactly that: one console, one firmware version. What was checked on it, what was not,
and the console's known quirks are in [docs/station-profiles.md](docs/station-profiles.md). A low-battery flag, a real
rain event compared with an official station, solar readings in full sun and gusts above about 100 km/h (60 mph) have
**not been seen on the verified console yet**.

Have a different console? A short [console report](https://github.com/fauxcloud/fauxcloud/issues/new?template=console_report.yml)
is the way a model gets onto the list.

## What you need

- **An ESP32 board with the ESP32-WROOM-32 module and 4 MB flash** (a common "ESP32 DevKit" board), and a USB power
  supply for it. No extra parts.
- **The weather console, on its power adapter.** On batteries the console switches its Wi-Fi off and uploads nothing.
- **A phone** for the setup, and a home Wi-Fi on **2.4 GHz** (the board has no 5 GHz radio).
- **A computer with a USB cable, once,** for the first install.
- Optional: an MQTT broker (the server that passes MQTT messages) and **Home Assistant 2025.10 or later**; internet on
  your home Wi-Fi for online services, airport weather and the Weather Underground forecast.

## Get started

The full steps, with what you should see after each one, are in [docs/get-started.md](docs/get-started.md).

1. **Install fauxcloud on the board.** From Chrome or Edge on a computer, the installer at
   [fauxcloud.org/install](https://fauxcloud.org/install/) writes it over USB. Or download the latest release from the
   [releases page](https://github.com/fauxcloud/fauxcloud/releases) and write it to the board with
   [esptool](https://docs.espressif.com/projects/esptool/) (a free command-line tool that writes the board). The
   `-factory.zip` file holds the parts and a `README.txt` with the command; see [docs/flash.md](docs/flash.md).
   Updates later go through **Settings**, without a cable (see [Updates](#updates)).
2. **Set up fauxcloud from your phone.** Power the board and join the Wi-Fi **`fauxcloud-setup-XXXX`** with the
   password **`fauxcloud`**. Answer the four steps: your home Wi-Fi, the nearest city, an admin password for
   **Settings**, and a password for **`fauxcloud-XXXX`**, the Wi-Fi your console will use.
3. **Pair the console.** With the console on its adapter, hold **BARO** for more than 3 seconds. Join the console's own
   network (`WWS-` plus six characters) and open **`http://192.168.1.1`**. Choose **`fauxcloud-XXXX`**, tick
   **Weather Underground** with a station ID and a key of your choice, and apply. Then open
   **`http://fauxcloud.local`** from your home network, or the board's IP address (for example `192.168.1.50`) that setup
   showed. The tiles appear with the first upload.

For Home Assistant, open **Settings** → **MQTT** and enter your broker; see
[docs/home-assistant.md](docs/home-assistant.md).

**New router or Wi-Fi password?** Hold the board's **BOOT** button for 1 to 3 seconds: fauxcloud restarts into setup
and keeps your stations, settings and history. Holding it for 5 seconds **erases everything**; see
[the BOOT button](docs/boards.md#the-boot-button).

## What you get

**Works without the internet**
- Your console uploads to fauxcloud instead of the cloud: no account, no app, no changes on your router.
- fauxcloud's own Wi-Fi is just for the console, so the console never reaches your home network or the internet.
- The console always gets its answer, even when your broker or your internet is down.
- Only devices on fauxcloud's own Wi-Fi can send readings, and the first console to use a station ID keeps it.
- Advanced: the console can stay on your home network if your router points the Weather Underground name at
  fauxcloud (a DNS override). One console per network, and not while online services are on.

**Dashboard**
- A page on your own network (`fauxcloud.local`) with live wind, rain, temperature, humidity, pressure, UV and sun.
  Home network only, plain HTTP, no way in from outside. Pin it to your phone's home screen from the browser menu.
- Tap a tile for charts and a table: the newest readings, today, yesterday or any date range, with a summary of the
  range. A wind rose and Beaufort bands in the wind sheet.
- Plain-word notices: "Offline since 14:05", "Battery low", "Sensor not updating", "Console clock off", "Pressure at
  factory value". It names the part that went quiet; it cannot fix the hardware.
- Drops impossible readings (wind above 195 km/h, humidity under 3 %, rain counters that go backwards) before they reach
  the page or Home Assistant.
- Name each console, hide measurements, add calibration offsets.
- Metric or imperial, dark or light, English or Polish (setup is in English).

**Home Assistant**
- MQTT discovery: one device per console with entities for wind, gust, direction, rain, temperature, humidity, dew
  point, pressure, UV and solar; a bridge device with Wi-Fi signal, restarts, firmware and memory.
- Wind averages over 2 and 10 minutes and the strongest gust of the last 10 minutes, worked out by fauxcloud, for
  awning rules that ignore single spikes. Rain over the last hour and the rain rate.
- A quiet station shows as unavailable after four minutes without an upload. "Notices", "Outdoor sensor link" and a
  battery-low sensor for automations; trigger on the notice id, not on its text.
- Plain TCP to your own broker with a user and password; no TLS to the broker.

**History**
- Hourly records for over three months and daily records for about two years, on the board's flash, through restarts
  and updates. When it is full, the oldest records go first.
- Export the range on screen as a CSV file. Save one console's settings to a file and load them again (settings only).
- A changed station ID on the same console: fauxcloud recognises the console and offers to carry its history over.

**Local outlook**
- A short outlook under the tiles from your own console's readings: rain now, calm, how the pressure is moving, and
  warnings for ice, strong gusts and frost. Not a forecast: it reaches a few hours ahead at most.
- It learns your site (sunny spells, mist, frost, how fast the night cools) once it has enough days, and the Learning
  sheet shows its progress. Also in Home Assistant as one text entity.
- Checks your console's pressure against long-term normals for your area, with no internet.

**Online services** (off until you turn them on; needs internet)
- Send your readings to Weather Underground, Windy, WOW-BE or PWSweather, to any Weather Underground-compatible server,
  or to your own URL with placeholders. Up to three services at once. Your key travels to that service, and it shows
  your data on its public page.
- Weather Underground refuses uploads without a parameter the console never sends; fauxcloud adds it. This was tested
  by replaying the console's own request against the service, not on the console.
- Encrypted sending (HTTPS) per service, on by default for a new service.
- Every 5 minutes, or the service's own minimum plus 10 seconds. One send at a time; the console is never delayed.
- A "Send one now" test and the last result in plain words. Keys are write-only: fauxcloud never shows a saved key
  again.
- Weather Underground's own forecast for tomorrow, fetched once an hour with your own Weather Underground API key and
  shown beside the outlook, labelled as Weather Underground's.

**Airport weather** (off until you turn it on; needs internet and your location or an airport code)
- The latest official reports (METAR) from up to three airports near you, every 15 minutes, beside your own readings,
  and the airport's forecast (TAF) for the next 12 hours in plain words. Data from aviationweather.gov: shown, never
  stored, never sent on.
- Finding airports sends the service a rough area of about 10 km, never your exact position.
- The airports' pressure beside yours. When your console's pressure stays off, a notice suggests the value to set as
  REL on the console (its relative pressure setting, set to sea-level pressure).

**Updates and maintenance**
- Update the firmware from **Settings**; fauxcloud goes back to the old version by itself if the new one does not start.
- Change your home Wi-Fi from **Settings**: if the new network does not work, it returns to the old one.
- The admin password itself never travels over the network, and wrong guesses lock the sign-in for a while. What the
  browser derives from it is enough to sign in and crosses the network in plain HTTP at setup (on fauxcloud's setup
  Wi-Fi) and on a password change (on your LAN): choose a strong password and do both on a network you trust
  ([API reference](docs/api.md#4-authentication)).
- A boot log and crash data to download for a bug report, and a Console tab with the raw lines your console sent.

## Online services, airport weather and the forecast

All three are in **Settings**.

- **Online services:** add a service, enter its station ID and key, keep "Encrypted (HTTPS)" on unless the service
  needs plain HTTP, and press "Send one now" to check. The sheet says "Accepted", "Key refused" or "Server not reachable".
  More in [docs/online-services.md](docs/online-services.md).
- **Airport weather:** use your location to find airports within reach, or type airport codes. The first airport in
  your list leads on the dashboard and counts most in the pressure reference. More in
  [docs/airport-weather.md](docs/airport-weather.md).
- **Forecast:** under Online services → Forecast, enter your own Weather Underground API key and switch it on. It is
  not sent to Home Assistant.

## Hardware limits

One console per board, about 50 KB of working memory, no encrypted dashboard (keep the pages off the internet) and a
fixed history store: see [What the board can hold](docs/boards.md#what-the-board-can-hold).

## Updates

Download `fauxcloud-<version>.bin` from the [releases page](https://github.com/fauxcloud/fauxcloud/releases) (not
the `-factory.zip`: that one is for a first install over USB). Open **Settings → Maintenance →
Firmware update** and choose the file. fauxcloud checks that it is a fauxcloud image, installs it and restarts; your
stations, settings and history stay. If the new version does not start properly, it goes back to the old one by
itself and says so. There is no automatic update. To go back by hand, install an older `.bin` the same way: the
update page checks that the file is a fauxcloud image, not its version.

## FAQ

**Light rain or drizzle shows nothing.** The rain gauge counts about 0.2 mm per tip of its bucket, so drizzle and the
start of a shower may not show. On the verified console the first tip of the day does not reach the upload at all
(the console rounds it down), so "Rain today" starts at the second tip.

**Snow does not count as rain.** Snow does not tip the bucket. It counts when it melts. Without a heated gauge the
outlook cannot see snow either, so it does not say "No rain" when it is cold.

**"Offline since …" after a power cut.** On batteries the console switches its Wi-Fi off, so nothing arrives until its
adapter has power again. Readings resume by themselves; a gap in the history stays a gap.

**The pressure jumped to 1013.** The console resets its pressure calibration (REL) to the factory value when you change
a display unit, step past item 15 of its SET menu, or take its batteries out. fauxcloud shows "Pressure at factory
value". Set REL again in the console's SET menu, using a nearby official station or airport.

**Which browsers work?** Chrome or Edge 88 and later, and Safari 14.5 and later on iPhone and iPad.

**I live in a half-hour time zone.** Hourly records start on the whole hour of UTC. In zones such as UTC+5:30, +5:45 or
+9:30 each hour and each day in the history is shifted by 30 or 45 minutes from your local clock.

**The board in its case is dark and opens no Wi-Fi.** Check the case before the firmware: a button stem that presses
the board's EN (reset) button holds it in reset (LED dark, no Wi-Fi), and one that presses BOOT while it powers up
keeps it in download mode. Leave clearance over both buttons.

**What does "Low memory" mean?** The free memory dipped below its safe level in the last three minutes. Readings
still arrive. If it stays, restart fauxcloud from **Settings**. If it comes back often, a bug report with the boot log helps.

**"History not saved: this firmware needs one USB flash to prepare its storage."** Readings still arrive, but the
history is kept in memory only until the next restart. The fix is one USB flash, which starts the history again and
keeps your settings: see [If the history is not saved](docs/flash.md#if-the-history-is-not-saved).

**What does the LED mean?** A short flash every 6 seconds means all is good; three short blips, then a pause, mean
something needs a look. The full list is in [The LED](docs/boards.md#the-led).

**Who may send readings?** Consoles on fauxcloud's own Wi-Fi. A console that reaches fauxcloud through your router
(with a DNS override, see [What you get](#what-you-get)) needs **Settings → Publishing → "Accept uploads from my home
network"**, which is greyed out while online services are on (the board has no memory for both). Each console is tied
to the key it sends first; uploads with another key wait until you accept the new key.

**After a console reset,** type the same station ID and key into the console as before and fauxcloud carries on. With a
different ID, **Settings** offers to keep the history under the new one.

**Is it an app?** No. It is a page on your own network that you can pin to your phone's home screen, plus Home
Assistant. Nothing to install from a store, no account.

**Does it need the internet?** No, for the console's uploads. One exception: a console that lost its clock waits for the
real time before it uploads, and fauxcloud gets that from the internet, your router's time server, or your phone when
you sign in to **Settings**.

More: the HTTP routes, MQTT topics and fields are in [docs/api.md](docs/api.md).

## Report a problem

Use the [bug report form](https://github.com/fauxcloud/fauxcloud/issues/new?template=bug_report.yml); it asks for the
diagnostics block from your board. For a console that is not on the list, use the
[console report](https://github.com/fauxcloud/fauxcloud/issues/new?template=console_report.yml). Never post a
password, a key or the crash file (it contains your passwords).

Contributions are welcome within the rules in [CONTRIBUTING.md](CONTRIBUTING.md); how decisions are made is in
[GOVERNANCE.md](GOVERNANCE.md), and everyone follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Source and build

Source: this repository at tag `v<version>`. Built with the Arduino-ESP32 core 3.3.12 (ESP-IDF v5.5.5 `b774170`,
lib-builder `6671d0b`, toolchain xtensa-esp-elf 14.2.0_20260121) from `firmware/fauxcloud/sketch.yaml`;
`tools/build.ps1` or the CI workflow rebuilds the same image. The core's source at that version:
https://github.com/espressif/arduino-esp32/tree/3.3.12 (the project keeps a copy at
https://github.com/fauxcloud/arduino-esp32/tree/3.3.12).

- The toolchain is pinned in the sketch's `sketch.yaml` profile and installed by `arduino-cli` into a folder of the
  checkout. `tools\build.ps1 fauxcloud` builds the release image; the CI workflow (`.github/workflows/build.yml`)
  runs the same build on Linux with every check. Setup, flashing and the web-page generator:
  [firmware/README.md](firmware/README.md).
- Build flags: `FWD_TLS` (1, the default and the release: HTTPS sending and airport weather; 0 builds a plain-HTTP
  fallback without airport weather, checked by CI, not released) and `FC_DIAG_BENCH` (1 adds developer diagnostics;
  that image's version ends in `+diag`). `FC_MAX_STATIONS=2` is the two-console build for boards with PSRAM, compiled
  by CI and not measured on a board.

## Licence

Copyright (C) 2026 the fauxcloud authors ([AUTHORS](AUTHORS)). fauxcloud is free software under the
**GPL-3.0-or-later** ([LICENSE](LICENSE)), with an additional permission for the Espressif SDK libraries that come in
object form only ([COPYRIGHT.md](COPYRIGHT.md)). The documentation in `docs/` is under CC BY-SA 4.0
([docs/LICENSE](docs/LICENSE)).

Free software under GPL-3.0, provided as is, without warranty of any kind; you flash and run it at your own risk.

Third-party notices: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) (the device serves it as `licenses.txt`,
linked from every page) and [THIRD_PARTY_NOTICES_FIRMWARE.txt](THIRD_PARTY_NOTICES_FIRMWARE.txt) (the full licence
texts of the firmware platform, also attached to every release).

## How it is made

fauxcloud is developed by one maintainer with AI assistance (Claude Code). Every change is reviewed, covered by tests
and verified on real hardware before release; the maintainer is responsible for all of it.

If the project stops being maintained, it will be archived; forks are welcome.

## Security

Please report a vulnerability privately, never in a public issue: see [SECURITY.md](SECURITY.md).

## Names

fauxcloud is an independent open-source project, not affiliated with or endorsed by any company or service named
here. All product and service names are trademarks of their owners and are used only to describe compatibility.
