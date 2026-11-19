# Third-party licences

fauxcloud, Copyright (C) 2026 the fauxcloud authors, is free software under the GNU General Public License version 3 or
(at your option) any later version, with an additional permission for the Espressif SDK libraries (`LICENSE`,
`COPYRIGHT.md`). It is an independent project, not affiliated with or endorsed by Weather Underground / The Weather
Company, Weathercloud, Windy, WOW-BE (wow.meteo.be) or the Met Office's WOW, PWSweather, Home Assistant, NOAA /
aviationweather.gov, Vevor or SucceBuy. Names are used only to say what fauxcloud works with.

The firmware image, its web pages and the data inside them contain or derive from the following third-party material,
each under its own licence. The device serves this very file as `licenses.txt` (plain text; every page links it as
"Open source · Licenses"), so it is written to be read as text. The full licence texts of the firmware platform (the
table at the end) are in `THIRD_PARTY_NOTICES_FIRMWARE.txt` in the source repository and on every release page.

| Component | Where | Licence |
|---|---|---|
| IANA tzdata 2026f (`zone.tab`, `iso3166.tab`) | `tz.json` data pack | public domain |
| nayarsystems/posix_tz_db (`zones.json`) | `tz.json` data pack (POSIX rules) | MIT |
| HatScripts/circle-flags | `flags-<region>.json` data packs | MIT |
| Lucide icons (some derived from Feather) | web pages (icons) | ISC; Feather: MIT |
| liquid-glass-js / LiquidGlassSkill (refraction field formulas) | web pages | MIT |
| BearSSL (TLS client: encrypted forwarding, airport weather, the forecast) | firmware (the release; not in the HTTP-only `FWD_TLS=0` build) | MIT |
| NOAA PSL NCEP/NCAR Reanalysis 1 (long-term monthly sea-level pressure 1991-2020, surface height) | firmware: the pressure normals table and the high-ground bitmap, derived data | public domain |
| Public CA root certificates (nine: subject name and public key only) | firmware: the trust anchors of encrypted sending | no licence text |
| The firmware platform: Arduino-ESP32 core 3.3.12, ESP-IDF v5.5.5, newlib, the GCC runtime | firmware (linked into every image) | table at the end |
| Moon, stars and rain pictures, the cloud mark (favicon, touch icon) | web pages | own work, GPL-3.0-or-later with the rest |
| Demo data of the `/try/` page | the project's site, not the firmware | synthetic, no third-party source |
| terser, zopfli | build tools only; nothing of them is in the image | BSD-2-Clause, Apache-2.0 |

Vendored source files and their checksums: `firmware/fauxcloud/web/data/sources/SOURCES.md`. Each served data pack
also names its sources in a leading key (`"l"` in `tz.json`, `"_l"` in the flag packs).

The wordmark in the dashboard bar is drawn from the outlines of Nunito SemiBold (SIL Open Font License 1.1, the Nunito
Project Authors, https://github.com/googlefonts/nunito); no font file is included.

## IANA tzdata

Public domain. From the tzdata `LICENSE` file: "Unless specified below, all files in the tz code and data
(including this LICENSE file) are in the public domain." https://www.iana.org/time-zones

## nayarsystems/posix_tz_db

https://github.com/nayarsystems/posix_tz_db (commit `93447c0d`). The upstream LICENSE keeps the template line
"Copyright (c) [year] [fullname]"; the copyright belongs to the posix_tz_db contributors.

MIT License

Copyright (c) the posix_tz_db contributors (nayarsystems/posix_tz_db)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## HatScripts/circle-flags

https://github.com/HatScripts/circle-flags (commit `379588b5`). The SVGs are minified for serving (fixed size
removed, numbers rounded).

MIT License

Copyright (c) 2026 HatScripts

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Lucide icons

https://lucide.dev (commit `a630cec`). The icons `arrow-right`, `chevrons-up`, `chevrons-down`, `clock`, `info`,
`moon` and `circle-alert` (`alert-circle`) are derived from the Feather project, so its MIT notice follows the ISC one.

ISC License

Copyright (c) 2026 Lucide Icons and Contributors

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby
granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

Feather (for the icons named above):

Copyright (c) 2013-present Cole Bemis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Liquid-glass refraction field

The edge-refraction field formulas in the dashboard page (liquid-glass-js commit `78cb6cc`, LiquidGlassSkill commit
`a524ea8`).

Copyright (c) 2025 Armagan Amcalar — liquid-glass-js, https://github.com/dashersw/liquid-glass-js
Copyright (c) 2026 Aref Azizian — LiquidGlassSkill, https://github.com/stormaref/LiquidGlassSkill

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## BearSSL

https://www.bearssl.org/ (upstream git `https://www.bearssl.org/git/BearSSL`, commit `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`),
vendored unmodified by `tools/vendor_bearssl.py` (file list and checksums: `firmware/fauxcloud/src/bearssl/VENDORED.txt`).
Linked into the release image (encrypted forwarding, airport weather); not into the HTTP-only fallback built with `FWD_TLS=0`.

Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## NOAA PSL NCEP/NCAR Reanalysis 1

The pressure normals behind the pressure check ("Pressure looks off" against what is usual for the place) are derived
from NCEP/NCAR Reanalysis 1 data (long-term monthly mean sea-level pressure 1991-2020, `slp.mon.ltm.1991-2020.nc`, and
surface height, `hgt.sfc.nc`) provided by the NOAA PSL, Boulder, Colorado, USA, from their website at
https://psl.noaa.gov. `tools/mslp_normals.py` keeps a coarse table (two seasons, 15 degrees of latitude by 20 of
longitude, one byte per cell) and a 5-degree bitmap of ground over 1 000 m; nothing else of the files is in the image.
The data are in the public domain; NOAA does not endorse fauxcloud, and the table is not official NOAA material.

## Public CA root certificates

Encrypted connections (HTTPS to online weather services, airport weather and the forecast) are checked against nine
public certificate-authority certificates, unmodified, of which only the subject name and the public key reach the
image: seven roots (Starfield Services Root Certificate Authority - G2, GlobalSign Root CA, GlobalSign Root CA - R3,
USERTrust RSA Certification Authority, Hellenic Academic and Research Institutions RootCA 2015, ISRG Root X1, DigiCert
Global Root G2) and two cross-signed certificates (Amazon Root CA 1, GTS Root R1). They are published by their
authorities for this use; no licence text accompanies them. Source: `tools/fwd_roots.pem`, each pinned by its SHA-256
fingerprint; `tools/gen_fwd_roots.py` turns them into `firmware/fauxcloud/src/fwd_roots.c`.

## The firmware platform

Every image links parts of the Arduino-ESP32 core 3.3.12 and of the ESP-IDF v5.5.5 inside it, built with the
xtensa-esp-elf 14.2.0_20260121 toolchain. Their notices and full licence texts: `THIRD_PARTY_NOTICES_FIRMWARE.txt`
(source repository and release page). The core's source at this version: https://github.com/espressif/arduino-esp32/tree/3.3.12

| Component | Licence (SPDX) |
|---|---|
| Arduino-ESP32 core 3.3.12 | LGPL-2.1-or-later (Arduino files), Apache-2.0 (Espressif files) |
| ESP-IDF v5.5.5 components (system, Wi-Fi and network glue, HTTP server, NVS, TLS glue, bootloader) | Apache-2.0 |
| Espressif Wi-Fi, PHY, coexistence and Bluetooth controller libraries (object code; contain FreeBSD net80211, BSD) | Apache-2.0 |
| ESP-MQTT; mDNS 1.13.1, network provisioning 1.0.2, diagnostics 1.3.4 | Apache-2.0 |
| Mbed TLS (used under Apache-2.0 of its Apache-2.0 OR GPL-2.0-or-later) | Apache-2.0 |
| lwIP | BSD-3-Clause |
| wpa_supplicant | BSD-3-Clause |
| FreeRTOS kernel | MIT |
| TLSF allocator | BSD-3-Clause |
| littlefs; esp_littlefs 1.22.3 | BSD-3-Clause; MIT |
| http_parser; cJSON | MIT; MIT |
| Xtensa HAL | MIT |
| newlib 4.3.0 (C and math library) | BSD-style (several notices) |
| GCC 14.2.0 runtime (libgcc, libstdc++) | GPL-3.0-or-later WITH GCC-exception-3.1 |

## Own work and synthetic data

The moon, stars and rain pictures behind the dashboard's sky (`web/moon.svg`, `web/stars.svg`, `web/rain.svg`), the
cloud mark (favicon and touch icon) are drawn for this project and are GPL-3.0-or-later like the code. The demo page's
data (`demo/`) is simulated: seeded, tuned to a city climate, with no downloaded input; the airports' codes and names
in it are public identifiers, and their reports are derived from the simulated station, not copied from real ones.

## Build tools

`tools/gen_web_assets.py` minifies the web pages with terser 5.36.0 (BSD-2-Clause; pinned in `tools/package.json`)
and compresses them with zopfli 0.2.3.post1 (Apache-2.0; pinned with hashes in `tools/requirements.txt`). They run on
the developer's machine only. Their output is the project's own code, minified and compressed, not a work derived from
the tools, so their licences do not follow it into the image. The same holds for `tools/gen_touch_icon.py`, a one-off
render of the home-screen icon (the PNG is committed; CI never runs it): it uses Pillow 12.2.0 (MIT-CMU) and resvg-py
0.2.6 (MIT, a binding of the resvg renderer), which are not part of the project's toolchain.
