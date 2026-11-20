## What is new

{{changelog}}

## Will it work with my console?

Verified on one unit: Vevor 7-in-1, model YT60234, firmware 1.0.1, EU 868 MHz. If your console's setup page has a
Weather Underground box, it may work (not tested yet; a console report tells us). If it pairs only through an app, it
will not. Details, and what is not verified yet:
[docs/station-profiles.md]({{repo_url}}/blob/{{tag}}/docs/station-profiles.md).

## Files

| File | What it is for |
|---|---|
| `fauxcloud-{{version}}.bin` | **The update.** Upload it in Settings → Maintenance → Firmware update. Keeps your settings and history. |
| `fauxcloud-{{version}}-factory.zip` | **A first install or a recovery over USB:** the bootloader, the partition table, `boot_app0` and the app as separate files with their addresses, and a `README.txt` with the `esptool` line. Not for an update. |
| `fauxcloud-{{version}}-diag.bin` | The same firmware with developer diagnostics (its version ends in `+diag`). Only when a bug report asks for it. |
| `fauxcloud-{{version}}-elf.zip` | The debug symbols of this exact build, to read a crash report. Not needed to run fauxcloud. |
| `SHA256SUMS` | The SHA-256 checksum of every file above. |
| `THIRD_PARTY_NOTICES_FIRMWARE.txt` | The full licence texts of the firmware platform linked into the image. |

```
{{sha256sums}}
```

## Update from an older version

1. Download `fauxcloud-{{version}}.bin` (not the `-factory.zip`). Check it against `SHA256SUMS` if you like.
2. On your home network, open Settings (`http://fauxcloud.local/admin`) → **Maintenance** → **Firmware update** and
   choose the file.
3. fauxcloud checks that it is a fauxcloud image, installs it and restarts, in about a minute. Your stations, settings
   and history stay.
4. If the new version does not start properly, fauxcloud goes back to the version it ran before by itself, and the
   Maintenance sheet says so.

## First install

Unpack `fauxcloud-{{version}}-factory.zip` and write the files to the board over USB with `esptool`, as its
`README.txt` and [docs/releasing.md]({{repo_url}}/blob/{{tag}}/docs/releasing.md) describe. Then follow "Get started"
in the [README]({{repo_url}}/blob/{{tag}}/README.md#get-started).

## Source

Source: this repository at tag `{{tag}}`. Built with the Arduino-ESP32 core 3.3.12 (ESP-IDF v5.5.5 `b774170`,
lib-builder `6671d0b`, toolchain xtensa-esp-elf 14.2.0_20260121) from `firmware/fauxcloud/sketch.yaml`;
`tools/build.ps1` or the CI workflow rebuilds the same image. The core's source at that version:
https://github.com/espressif/arduino-esp32/tree/3.3.12 (the project keeps a copy at
https://github.com/fauxcloud/arduino-esp32/tree/3.3.12).

fauxcloud is free software under GPL-3.0-or-later, with an additional permission for the Espressif SDK libraries that
come in object form only (`COPYRIGHT.md`). Provided as is, without warranty of any kind; you flash and run it at your
own risk.

fauxcloud is an independent open-source project, not affiliated with or endorsed by Weather Underground / The Weather
Company, Weathercloud, Windy, WOW-BE (wow.meteo.be) or the Met Office's WOW, PWSweather, Home Assistant, NOAA /
aviationweather.gov, Vevor or SucceBuy. Names are used only to say what fauxcloud works with.
