# demo/ — data for the `/try/` demo

The site's `/try/` page runs the real dashboard and admin assets of a release, with a small shim in front of the network.
This folder holds the **data** and the **derivation** the shim needs. The shim itself (a `fetch` wrapper that answers
`/api/*` through `replay.js`, the clock and the demo bar) is `site/try/demo.js`; `tools/build_site.py` puts it next to
the release's own pages (`site/README.md`).

- `fixtures/`: a synthetic two-year history for a station named "Garden" in the centre of Warsaw (52.23 N, 21.01 E, a
  generic city-centre point, not an address), `CET-1CEST,M3.5.0,M10.5.0/3` (`Europe/Warsaw`), with the two real nearby
  airports by their public codes and names: `EPMO` Warsaw Modlin (main, 33 km) and `EPWA` Warsaw Chopin (10 km). The
  airports' reports are **derived from the synthetic station series**, not real METARs. Station id `DEMO1`, placeholder
  addresses, `<set>` for every secret.
- `replay.js`: a pure function library (no DOM, no network, no clock) that turns the fixtures and a time `t0` into every
  per-time body the pages read. The shim calls `answer(path, t0, switches)`.

The weather is **simulated**, not copied from any record. It is seeded and has no downloaded input. It is tuned to
Warsaw's climate: about 8.4 °C a year, January about −3 °C, July about 19 °C, about 640 mm of rain, 120 frost days.

## Regenerate

```
python -I tools/demo_data.py          # the static fixtures (about 7 s; the same bytes on every run)
node demo/replay.js --write           # snap/: the example days' and scenarios' live snapshots and frames
python -I -m unittest discover -s test/tools -p test_demo_data.py
node test/tools/demo_vectors.js
```

## What happens in the year

The simulation runs in 10-minute steps: pressure waves, fronts with rain bands and gusts, the S2 tide, cloud tied to
pressure, annual and daily temperature cycles, dew point from a spread, wind from WSW veering with the pressure trend,
summer showers and thunderstorms, dry snow that melts into the unheated gauge later, clear-sky sun by solar elevation, and a
roof to the south-west and a tree to the east that shade the sensor at low sun. Hour records come from the steps and
day records from the hours, as the firmware builds them.

Scripted on top:
- a cold spell (17–27 Jan 2026) whose front brings wet snow at about 0 °C on the morning of 21 Jan (2.8 mm/h that melts in
  the gauge as it falls, so the station's rain rate, the airport's snow and the sky's flakes agree), then a clear frosty night;
- the console's REL mis-set by +22 hPa after a battery change (30 Jan – 5 Apr): the pressure check (L7) flags it from
  late March;
- a clear high with frosty mornings (14–16 Apr);
- a heat wave (16–23 Aug) ending in an evening thunderstorm on 22 Aug;
- an autumn front (8 Oct);
- a clear frosty night in a cold snap and snow (9–12 Nov);
- a three-hour power cut at the bridge (21 Oct), one rejected reading (3 Sep), and the first 30 hours of the two-year
  ring at the console's factory pressure.

## Files: static, cut at t0, or derived

| File | Kind | Use |
|---|---|---|
| `index.json` | static | the station, zone, airports, the replayable range (`series.first_t0` .. `series.last_t0`), the learner snapshots, `example_days`, `scenarios`, the static route map |
| `history/series.json` | **cut at t0** | the hourly series, columnar (one array per field, `from` = the first hour, one hour apart): `n` (0 = a lost hour), `q` (quality bits, firmware order), temperature lo/avg/hi, dew point, humidity lo/avg/hi, pressure, wind, gust, direction, steadiness, calm readings, rain, rate max, UV max, solar avg/max; plus `cloud` (%), `pr` (precipitation mm/h, snow included) and `ts` (thunder) for the airports and the forecasts. It covers a year + 8 days before `last_t0` and the day after it (the TAF's and WU's future; never shown as history) |
| `history/daily.json` | **cut at t0** | `/api/daily` rows, 736 local days up to the day before `last_t0` |
| `learn/<YYYYMMDDTHHMMZ>.json` | **newest at or before t0** | `/api/learn`: the learners run over the history to that time (the first of every month, plus one just before every named time) |
| `static/bridge.json`, `static/station.json` | static | `/api/bridge`, `/api/station` |
| `static/diag.json` | static | `/api/live`'s `diag` (the counters follow the uploads) |
| `admin/*.json` | static | the admin GETs (`index.json` `static` maps the routes), each object `"demo": true`; `replay.js` fills the parts that follow the time (the WU card, the airport fetch, the Learning tile, the console lines) |
| `snap/<key>/live.json`, `frames.json` | derived | `replay.js --write` at each example day (`<season>-day`, `<season>-night`) and scenario: the `/api/live` body at its t0, and 120 upload frames (65 s apart, columnar) around it; ready-made, the same as `replay.js` computes |

## Deriving a body at any t0 (`replay.js`, the reference)

- **Hour row** of hour `i`: the series' columns in `web_render.cpp`'s `hourRow` shape (`q` names from the bits, triples
  with their `null` slots, `rain_daily_mm` `null`).
- **Day row**: `history/daily.json`. The open day aggregates that day's hour rows plus the open hour, by history.cpp's
  rules (`dayRow`).
- **Cut**: `/api/hourly` answers hours before t0's hour, by default the newest 48, or `from` / `to`; its `open` is
  t0's hour with `n` and rain counted to t0. `/api/daily` answers days before t0's local date, by default the newest
  30, or `days=N`, `all=1`, `from` / `to`; its `open` is the open day.
- **Uploads** fall every 65 s from the series' start (none in a lost hour). `last_seen` = the newest upload at or
  before t0 (minus `late`); `age_s` = t0 − `last_seen`.
- **The 1-minute frame rule** (`frame(t)`). Each value is the hourly mean interpolated linearly between hour middles,
  plus a bounded, stateless value noise, kept inside the hour record's limits:
  - temperature ±0.35 °C (10-min) and ±0.12 °C (2-min), clamped to the hour's lo/hi;
  - humidity ±1.5 % (15-min), clamped to the hour's lo/hi; dew point from both (Magnus);
  - pressure ±0.12 hPa;
  - wind ×(1 ± 0.45 at 1 min, ± 0.2 at 5 min), at most the hour's gust; gust 1.25–1.6 × wind + up to 1.5 km/h,
    between the wind and the hour's gust;
  - direction: the circular interpolation ± 12° (30-min) ± 9° (1-min);
  - solar ×(1 ± 0.2), at most the hour's max, 0 at night; UV 0.9 × the interpolation, at most the hour's max;
  - rain: the hour's tips (rain ÷ 0.225 mm) at hashed seconds within the hour. The daily counter counts them since
    local midnight; the rate is the last 10 minutes' tips × 6, at most the hour's rate max.

  The hash is `u32(k, n)` in `replay.js` (32-bit integer mixing; no state, no seed to carry).
- **Recent ring**: the frames at the last 60 uploads; `rain_mm` is the counter's rise since the reading before.
- **Live**: the newest upload's frame. Derived fields: last-hour rain, the 2-min and 10-min wind means and the rain
  rate; no indoor values (the YT60234 console never uploads them). Also the learners' `use`, the outlook, WU, the notices
  and the diag counters.
- **Airports** report every 30 min. Each report is the station series at the report time plus the airport's offsets in
  `index.json` (temperature, pressure; QNH whole hPa, without the REL error), wind ×1.2 in whole knots, cover from
  `cloud`, weather from `pr` / `ts` / temperature (snow at ≤ 0.5 °C) / humidity (mist, fog). `cur` is a report
  ≤ 75 min old.
- **TAF**: the phenomena of the next 12 series hours (thunder `ts`; snow or rain `pr` > 0.3; fog humidity ≥ 97 % in
  light wind; gusts ≥ 40 km/h). An hour with a neighbour is likely; an hour alone or next to a likely run is possible.
  Runs merge into windows; a window that reaches the 12-h horizon is `cut`.
- **WU line**: tomorrow's series day (today's from 00:00 to 03:00 local): the phrase, hi / lo and the chance of rain by
  fixed thresholds; fetched at :10.
- **Outlook**: the rules of `docs/api.md` §3.2.3 on the hour rows before the last upload, its reading, the main airport
  (T3: EPMO, ≤ 75 min, ≤ 60 km) and the snapshot's `use` (L1 day words, L2 fog, L5 cooling, L6 minimum).
- **Notices**: `baro_ref` while airport weather is on and the last three reports differ by ≥ 2 hPa the same way (the REL
  error); `baro_check` while it is off and L7 flags (the `pressure-off` scenario).
- **Switches** (`scenarios[...].switches`): `metar: false` gives `/api/metar` `{"on": false, ...}`, no `metar` in the
  status, no T3, `use.metar` false. `wufc: false` gives no `wufc`. `late: s` means the console stopped `s` seconds
  before t0 (offline at ≥ 240 s).

## Time

Every time value is absolute UTC from the generation run (`last_t0` = 2026-11-16 11:15 UTC, the default "today"). The
demo picks any t0 between `series.first_t0` and `series.last_t0`: an example day's day or night time, a scenario's
t0, or anything in between. The shim (`site/try/demo.js`) shows it as the viewer's **now**: it maps the chosen t0
onto the wall clock and leaves the page's `Date` alone:
1. d = the wall clock at the choice − t0, rounded to whole hours (hour records stay on whole hours in every zone); the
   demo runs from t0' = wall clock − d, within 30 minutes of t0, at the wall clock's pace. k = the viewer's local date
   − the station's local date at t0', in whole days.
2. Every time value in an answer moves into page time:
   - ISO strings `YYYY-MM-DDTHH:MM:SSZ` and hour keys `YYYY-MM-DDTHH:MMZ`: + d;
   - day keys `YYYY-MM-DD`: + k days;
   - unix seconds (recent `rows[i][0]`, learn `last_hour`, `written`, `use.hour`, `use.night_end`, `night.last.start`,
     console `items[].t`): + d;
   - unix days (learn `l1.cells[].last`, `evening.label_day`): + k; learn `last_day` (`YYYYMMDD`): + k days;
   - the REL hint's `HH:MM` (`baro_ref`): rewritten in the viewer's zone.

   Durations never move. A history query in page time (`/api/hourly` and `/api/daily` `from` / `to`) moves back first.
3. So the newest upload is "now" for the page's own clock: ages, "today", the 24-hour charts and the TAF's "tomorrow"
   agree with `Date`. The pages poll as on a board; `replay.answer(path, t0' + elapsed, sw)` gives a new upload every
   65 s, `age_s` counting, the wind dial moving. The sky follows the station's light (day or night as the example),
   while clock times read in the viewer's zone.

## Example days and scenarios

| Key | t0 (UTC) | The strip (outlook.js) |
|---|---|---|
| `winter-day` / `-night` | 2026-01-21 09:30 / 21:00 | "Sleet or snow. Freezing." + "Icy surfaces possible." (wet snow at about 0 °C: the gauge's rate, flakes in the sky, EPMO reports snow) / "Clear night. Freezing." + icy |
| `spring-day` / `-night` | 2026-04-15 11:30 / 20:30 | "Sunny." / "Clear night. Calm.", pressure falling, ground frost |
| `summer-day` / `-night` | 2026-08-22 12:40 / 20:30 | "Sunny. Hot." / "Clear night." + "Minimum around … tonight." after the storm |
| `autumn-day` / `-night` | 2026-10-08 06:20 / 20:30 | "No rain.", pressure falling, the TAF's rain on page 2 / "Light rain.", pressure rising |
| `today` (default) | 2026-11-16 11:15 | "Sunny spells.", pressure falling; WU tomorrow "Rain" |
| `sunny` | 2026-08-22 12:40 | "Sunny. Hot." |
| `rain` | 2026-10-08 14:27 | "Moderate rain. Windy." + gusts |
| `snow` | 2026-01-21 08:30 | "Sleet or snow. Freezing." + "Icy surfaces possible.": wet snow at about 0 °C, flakes in the sky, EPMO reports snow (the station's own `sleet_snow` outranks the airport's report in the strip) |
| `thunderstorm` | 2026-08-22 17:40 | "Heavy rain." + "Thunder reported nearby." |
| `frost-night` | 2026-11-09 22:00 | "Clear night. Freezing." + "Minimum around 0 to 4 °C tonight." + ground frost (0 °C; the night then falls below the range, about −3 °C by 02:00) |
| `pressure-off` | 2026-03-31 11:00, sources off | `baro_check` "about 15 hPa above the usual … over 60 days" on the pressure tile; L7 flags it in the Learning sheet |
| `offline` | 2026-11-16 11:15, `late` 1500 | the last upload 25 min old: no outlook card, stale tiles |
| `no-sources` | 2026-11-16 11:15, sources off | no plane tab, no page 2 |

The learners look like a station that has run for a year, because the two-year history feeds them:
- L1: twelve sun-height cells, the low ones from winter days, 45° from summer, the 5° afternoon cell mostly behind
  the roof.
- L1b: the sky map with the roof and tree shade.
- L2: from wet hours (clear nights here top out below 92 %).
- L3: the console's 1-mph step. L4: the hourly winds.
- L5: clear and cloudy evenings.
- L6: in use from April, its relation fitted on the clear calm nights.
- L7: silent except during the REL error.
- The Learning tile reads "4 of 4 in use".

## Not covered

No `sensor_stale` / `sensor_absent` episode, no console clock skew, no promoted extra sensor, no second station, no
TAF `nil` / `cnl`, no `console_reset` or `rain_partial` hours.
