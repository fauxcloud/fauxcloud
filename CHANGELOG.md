# Changelog

What changed in each fauxcloud release, in plain words. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the versions follow
[Semantic Versioning](https://semver.org/). The release workflow copies the section of the tagged version into the
release notes, so every release needs its own `## [<version>]` section before it is tagged (see `docs/releasing.md`).

## [Unreleased]

## [1.0.0] - 2026-11-20

The first stable release of fauxcloud. The firmware is the same as 1.0.0-rc.1 apart from its version string, so
everything in the [1.0.0-rc.1 notes](https://github.com/fauxcloud/fauxcloud/releases/tag/v1.0.0-rc.1) applies to
it. It was verified on one console: a Vevor 7-in-1, model YT60234, firmware 1.0.1, EU 868 MHz; see
[docs/station-profiles.md](docs/station-profiles.md) for what was and was not checked.

### Changed
- **Browser installer:** the steps to set the board up (replug it, join its setup Wi-Fi, open the setup page, answer
  the four steps, open `fauxcloud.local`) now appear after a flash, next to the QR code; the form then shrinks to one
  "Flash another board" button. The driver help opens when no board is found or a flash fails, and the flashing log
  has a Copy button for bug reports.
- **Website:** an Install link in the header of every page; the docs sidebar names its first group "Start" and shows
  each label once on a phone; the diagram on the front page says "Home Wi-Fi" instead of "LAN".
- **README** opens with the project's header and links to the installer, Get started and the demo.
- **Release notes** link the project's own copy of the Arduino-ESP32 core used for the build,
  https://github.com/fauxcloud/arduino-esp32/tree/3.3.12.

## [1.0.0-rc.1] - 2026-11-19

The first public release candidate of fauxcloud. It was verified on one console: a Vevor 7-in-1, model YT60234,
firmware 1.0.1, EU 868 MHz. Other consoles are not tested yet; see
[docs/station-profiles.md](docs/station-profiles.md) for what was and was not checked on it.

### Added
- **Works without the internet.** The board runs its own Wi-Fi for the console, answers the console's Weather
  Underground upload itself and keeps a record of each console it serves (a station). No account, no app, no router
  changes. The console always gets its answer, even when the broker or the internet is down.
- **Setup from a phone** in four steps (home Wi-Fi, nearest city, an admin password, a password for the console's
  Wi-Fi), a browser installer at fauxcloud.org/install, and a first-install package for esptool.
- **Dashboard** on your own network (`fauxcloud.local`): live wind, rain, temperature, humidity, pressure, UV and sun;
  charts and a table for the newest readings, today, yesterday or any date range; a wind rose; plain-word notices
  ("Offline since 14:05", "Battery low", "Sensor not updating", "Console clock off", "Pressure at factory value");
  metric or imperial, dark or light, English or Polish. It can be pinned to a phone's home screen.
- **Plausibility filter:** impossible readings (wind above 195 km/h, humidity under 3 %, rain counters that go
  backwards) are dropped before they reach the page or Home Assistant.
- **Home Assistant** over MQTT with discovery: one device per console, a bridge device with Wi-Fi signal, restarts,
  firmware and memory, wind averages over 2 and 10 minutes, the strongest gust of the last 10 minutes, rain over the
  last hour and the rain rate, and an unavailable state after four minutes without an upload.
- **History** on the board's flash: hourly records for over three months and daily records for about two years,
  kept through restarts and updates, with CSV export and a settings backup file.
- **Local outlook** under the tiles from your own console's readings (rain now, calm, pressure movement, warnings for
  ice, strong gusts and frost), a Learning sheet that adapts it to your site, and a pressure check against long-term
  normals for your area. It is not a forecast.
- **Online services**, off until you turn them on: forward your readings to Weather Underground, Windy, WOW-BE,
  PWSweather, a Weather Underground-compatible server or your own URL (up to three at once, encrypted by default, a
  "Send one now" test, write-only keys). Weather Underground refuses uploads without a parameter this console never
  sends; fauxcloud adds it. This was found by replaying the console's request against the service, not on the console.
- **Weather Underground forecast** for tomorrow, fetched hourly with your own API key, shown beside the outlook and
  labelled as theirs.
- **Airport weather**, off until you turn it on: official reports (METAR) from up to three airports near you and the
  airport's forecast (TAF) in plain words, shown beside your own readings and never stored or sent on. A notice
  suggests the REL value to set on the console when its pressure stays off.
- **Updates and maintenance:** firmware update from Settings with automatic rollback if the new image does not start,
  a Wi-Fi change that returns to the old network if the new one fails, a boot log and crash data for bug reports, and
  a Console tab with the raw lines the console sent.
- **Security:** only devices on the board's own Wi-Fi can send readings, the admin password itself never travels over
  the network, wrong guesses lock the sign-in for a while, saved keys are write-only, and a stored password never
  follows a changed host, port or network name.

### Known limits
- One console per board, about 50 KB of working memory, plain HTTP for the dashboard (keep it off the internet) and a
  fixed history store; see [docs/boards.md](docs/boards.md#what-the-board-can-hold).
- Not verified on the console yet: the low-battery flag, a real rain event compared with an official station, solar
  readings in full sun and gusts above about 100 km/h (60 mph).
- The model that pairs through the SmartLife / Tuya app (YT60307) and any console that uploads only over HTTPS are not
  supported.
- The update page does not block a downgrade: it checks that a file is a fauxcloud image, not its version.
