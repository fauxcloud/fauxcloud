# fauxcloud: copyright and licences

Copyright (C) 2026 the fauxcloud authors (see `AUTHORS`).

fauxcloud is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version,
with the additional permission below. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
License (`LICENSE`) for more details.

## Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the Espressif ESP-IDF and
Arduino-ESP32 libraries (or a modified version of those libraries), including the parts distributed by Espressif in
object form only, containing parts covered by the terms of their own licences, the licensors of this Program grant you
additional permission to convey the resulting work. Corresponding Source for a non-source form of such a combination
shall not include the source code for the parts of those libraries that are distributed by Espressif in object form
only (today, for example, `libnet80211`, `libpp`, `libcore`, `libespnow`, `libsmartconfig`, `libphy`, `librtc`,
`libcoexist` and `libbtdm_app`; the list is not limiting).

You may remove this additional permission from your copy of the Program or of a work based on it (GPL version 3
section 7).

## Documentation

The documentation (`docs/`) and the text of the project's website are licensed under the Creative Commons
Attribution-ShareAlike 4.0 International licence (https://creativecommons.org/licenses/by-sa/4.0/). Configuration
snippets in the documentation (for example Home Assistant YAML) are dedicated to the public domain under CC0 1.0
(https://creativecommons.org/publicdomain/zero/1.0/), so you can paste them into your own setup freely.

The renders of the printed case on the website (`site/assets/case-*.jpg`) are by KRCreations, the case's designer,
used with permission (`LICENSES/LicenseRef-Used-With-Permission.txt`).

## Third-party parts and source

- `THIRD_PARTY_LICENSES.md`: the components of the firmware image, its web pages and its data (BearSSL, Lucide, the
  time-zone and flag packs, the NOAA PSL derived table, the CA certificates), with their notices and a table of the
  firmware platform. The device serves this file as `licenses.txt`.
- `THIRD_PARTY_NOTICES_FIRMWARE.txt`: the full licence texts of the firmware platform linked into the image (the
  Arduino-ESP32 core, ESP-IDF and its components, newlib, the GCC runtime). It is attached to every release.
- Source: this repository at the release tag is the Corresponding Source of the firmware image. It is built with the
  Arduino-ESP32 core 3.3.12 (ESP-IDF v5.5.5, toolchain xtensa-esp-elf 14.2.0_20260121) from
  `firmware/fauxcloud/sketch.yaml`. The core's source at that version: https://github.com/espressif/arduino-esp32/tree/3.3.12
- Rebuilding against a modified core (LGPL 2.1): install your core in place of 3.3.12 and build with `tools/build.ps1`
  (`firmware/README.md`); this repository's source and `sketch.yaml` are all that is needed besides the core.

## Names

fauxcloud is an independent open-source project, not affiliated with or endorsed by Weather Underground / The Weather
Company, Weathercloud, Windy, WOW-BE (wow.meteo.be) or the Met Office's WOW, PWSweather, Home Assistant, NOAA /
aviationweather.gov, Vevor or SucceBuy. Names are used only to say what fauxcloud works with.
