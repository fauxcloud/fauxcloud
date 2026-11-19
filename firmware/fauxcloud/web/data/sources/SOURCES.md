# Vendored sources of the location-picker data packs

`tools/gen_tz_flags.py` builds `../tz.json` and `../flags-<region>.json` from these files only (no download at
build time). They were downloaded once, reviewed (licence read; every SVG scanned for script, event
handlers, `href`, `foreignObject`, `url()`, `data:` and `<image>`, with no hits)
and copied here unchanged. Licences: `THIRD_PARTY_LICENSES.md` at the repo
root.

| Folder | Source | Version | Integrity | Licence |
|---|---|---|---|---|
| `tzdata/` | IANA tzdata, https://data.iana.org/time-zones/tzdata-latest.tar.gz | 2026f | archive sha256 `01b4a91cec5167ed48c11e8ab78f2048cead78ce94b0cafcad0ae8c5a3044d3d` (483 310 B, not vendored) | public domain (`tzdata/LICENSE`) |
| `posix_tz_db/` | nayarsystems/posix_tz_db, https://github.com/nayarsystems/posix_tz_db | commit `93447c0ddac304ca6672a5fd905261c7e8905159` | `zones.json` sha256 `7487aa05c5a263d882487a7d739ac31682d84733d1eec6ec274d6d311ab74560` | MIT (`posix_tz_db/LICENSE`; its copyright line is an unfilled template, credit goes to the repository) |
| `circle-flags/` | HatScripts/circle-flags, https://github.com/HatScripts/circle-flags | commit `379588b5da95482d6bbf10bd45644a35b0609ea6` | see below | MIT, (c) 2026 HatScripts (`circle-flags/LICENSE.md`) |

Files used:

- `tzdata/`: `zone.tab` (sha256 `e2e8e31c4e10f44576c79345e76060591dbb3eef6ce14e006afc8a373caa9593`), `iso3166.tab`
  (`837c80785080c8433fd9d4ea87e78f161ac7a40389301c5153d4f90198baeb2a`), `version`, `LICENSE`. `zone.tab` rather than
  `zone1970.tab`, so each country lists its own zones.
- `posix_tz_db/`: `zones.json`, `LICENSE`.
- `circle-flags/flags/`: `<cc>.svg` for the 247 country codes in `zone.tab`, plus `bq-bo.svg` and `sh-hl.svg`.
  Four of them are git symlinks upstream and are stored here as text files naming their target
  (`sj.svg` -> `no.svg`, `um.svg` -> `us.svg`, `bq.svg` -> `bq-bo.svg`, `sh.svg` -> `sh-hl.svg`); the generator
  follows them.

Regenerate and check:

```bash
python -I tools/gen_tz_flags.py
python -I tools/gen_tz_flags.py --check
```
