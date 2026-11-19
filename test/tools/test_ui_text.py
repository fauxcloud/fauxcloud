"""The UI sentences are in their files word for word, and the retired phrases are gone from the page sources
(firmware/fauxcloud/web) and the firmware sources (firmware/fauxcloud/src; web_assets.h is generated and compressed, so
it is skipped). A wording change updates these lists in the same commit.

Usage: python -I -m unittest discover -s test/tools
"""
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "..", "firmware", "fauxcloud")


def read(rel):
    with open(os.path.join(FW, rel), encoding="utf-8") as f:
        return f.read()


# file -> sentences that must appear exactly ("file#x": another list for the same file)
NEW = {
    "src/notices.cpp": [
        "Outdoor sensor not updating since %s. A calm night can do this; a long gap means its battery or radio.",
        "Outdoor sensor missing since %s: check its batteries. A short battery-out of the console helps it find the sensor.",
        "The console's pressure is at its factory value; set REL once in its SET menu.",
        "Another key arrived for this station (%lu uploads ignored). Re-paired the console? Accept it in Settings.",
        "The console's clock is off by %s; switch NTP off and on in its SET menu.",
        "Time is estimated since the restart. Sign in to the admin page once on your phone, or connect fauxcloud to the internet.",
        "Waiting for the real time. Sign in to the admin page once on your phone, or connect fauxcloud to the internet.",
        "Memory is low; readings still arrive. If this stays, restart fauxcloud in Settings.",
        "History not saved (%lu writes failed); readings still arrive. If this stays, restart fauxcloud in Settings.",
        # the same two sentences for a count of one (whole templates)
        "Another key arrived for this station (1 upload ignored). Re-paired the console? Accept it in Settings.",
        "History not saved (1 write failed); readings still arrive. If this stays, restart fauxcloud in Settings.",
        "fauxcloud restarted itself to free memory; nothing to do.",
        "History not saved: this firmware needs one USB flash to prepare its storage (see the README).",
        "The time is behind the saved history, so history is paused until the time is confirmed (at most 24 h).",
        '"Time approximate"', '"Waiting for time"',
    ],
    "web/dash.js": [
        "The console sends nothing on batteries or during a power cut; readings resume on their own.",
        "No answer from fauxcloud",
    ],
    # the Details in Settings, under the status row (health.js)
    "web/admin-station.js#2": ["Other data from the console", "Wind, 10-min average", "Computed by fauxcloud"],
    "web/adminface.js#3": ["Hide details", "Bridge health"],
    "web/dash.js#2": ["Offline. Last upload "],
    "web/health.js#2": ["'Offline since {w}'"],  # one template for the dashboard and the admin tile
    "web/setup.js": ["hold BARO for 3 seconds until AP shows", "hashed on this phone; only the hash was sent.",
                     # the home network uses fauxcloud's own Wi-Fi subnet
                     "Your home network also uses 192.168.77.x, the same as fauxcloud’s own Wi-Fi, so the dashboard will "
                     "not open from your home network. Change your router’s network address (for example to 192.168.1.x), "
                     "then unplug fauxcloud for a few seconds: it joins again with a new address.",
                     # a home network wider than /24 that holds 192.168.77.x: the mask advice
                     "Your home network (${esc(net)}) includes 192.168.77.x, the address range of fauxcloud’s own Wi-Fi, so "
                     "the dashboard will not open from your home network. Set your router’s subnet mask to 255.255.255.0, "
                     "then restart fauxcloud from Settings (or unplug it for a few seconds): it joins again with a new address."],
    # one whole sentence per resolution, the range's own words
    "web/dash-sheet.js": ["'Rain per 6 hours'", "'Rain per week'", "'Some readings rejected in those 6 hours'",
                          "'No readings in this range.'", "'Could not load this range. Pick it again to retry.'",
                          "Speed shades need hourly data: pick 7 days or less.", "rose_calm_h: 'Calm: {n} h'", "rose_calm_r: 'Calm: {n}'", "'Calm: none'",
                          "one: '{n} row exported'", "other: '{n} rows exported'", "dates: 'Date range'",
                          # the Now row (current values, the unit on the number) and the range row under the chart
                          "now_h: 'Now'", "'reading'", "'gust'", "'gust, last 10 min'", "'from'", "'direction'", "'dew point'",
                          "'feels like'", "'absolute'", "'today'", "'last hour'", "'this week'", "'this month'", "'this year'", "'last 7 days'",
                          "'humidity'", "'light'", "yday: 'Yesterday', today: 'Today'",
                          "'average'", "'lowest / highest'", "'dew point average'", "'strongest gust'", "'mostly from'",
                          "'change'", "'total'", "'highest'", "'of sun'", "'wettest day, {d}'", "'wettest hour, {d} {t}'",
                          "'wettest hour, {t}'", "rs_aria: '{head} summary'",
                          "Far above the row’s average: a sensor glitch or a filter gap."],
    "web/index.html": ['aria-label="Date range"', 'aria-label="Admin" title="Admin"'],
    # the language button: what the page speaks and what a tap switches to, one whole template
    "web/lang.js": ["a: 'Language: {name}. Switch to {next}'"],
    "web/adminface.js": ["BARO on the console for 3 seconds until AP shows",
                         # the adopt offer under the Stations tiles
                         "Console <b>{to}</b> uploads from the same device as <b>{from}</b>. Did its ID change? {btn}",
                         "Keep the history under {to}",
                         # the fourth section, the memory note per station cap
                         "Home Assistant and online services",
                         "fauxcloud keeps working, but open pages may fail. Close dashboard pages you are not using.",
                         "Forget stations you no longer use, or close dashboard pages you are not using.",
                         ],
    # Maintenance lives in admin-sheets.js (a sheet loaded on demand)
    "web/admin-sheets.js": ["Download the boot log", "fauxcloud-bootlog.txt",
                            "x_bug: 'Bug report'", "The boot log shows station ids and addresses; check it before you post it.", "Choose a file",
                            "Log every upload and page request", "Only when asked for a bug report: the log then covers only the last few minutes."],
    # page 2 "Next" of the outlook card
    "web/dash-wufc.js": ["a: '{what} likely {span}'", "b: '{what} possible {span}'",  # page 2 with the sheet's span
                         "d: '{day}: {phrase} · {hi}° / {lo}°'", "'Weather Underground forecast'",
                         "'{name} airport forecast (TAF)'", "'Next: {what}'", "'airport, 12 h'", "'Strong gusts'",
                         # the i sheet's Forecast rows (grouped windows) and the one source line per source combination
                         "l1: '{what} likely'", "p1: '{what} possible'", "{phrase}, {hi}° / {lo}°, {pop} % chance of precipitation",
                         "k: '{phrase}, {hi}° / {lo}°'",
                         "Airport windows from the {name} ({icao}) forecast. Not this station’s readings.",
                         "Tomorrow from Weather Underground, fetched {t}. Not this station’s readings.",
                         "Today from Weather Underground, fetched {t}. Not this station’s readings.",
                         "Airport windows from the {name} ({icao}) forecast; tomorrow from Weather Underground, fetched {t}. Not this station’s readings.",
                         "Airport windows from the {name} ({icao}) forecast; today from Weather Underground, fetched {t}. Not this station’s readings.",
                         # WU's AM / PM in words
                         "'Morning'", "'Afternoon'"],
    # the Now header, one Forecast section, the sun caveat as a temperature block's note
    "web/outlook-why.js#2": ["Now · this station’s readings, {at}", "fc: 'Forecast'", "sun: 'In direct sun with light wind the sensor can read high.'",
                               "hot: 'At or above {lim}.'", "head: 'From this station’s own readings, as of {at}. No forecast model.'"],
    # the card and its sheet "Outlook"; the admin tile and HA keep "Local outlook"
    "web/dash.js#3": ["FCO.svg('info') + WD.ol", "ol: 'Outlook'"],
    "web/adminface.js#4": ["weather: 'Weather description', local_ol: 'Local outlook'"],
    # the tile's trend word is the strip's
    "web/tendency.js": ["rising_fast: 'rising fast'", "falling_fast: 'falling fast'"],
    # the Publishing confirm view's consequence lines
    "web/admin-station.js#3": ["'Its saved settings on this bridge are deleted; the console keeps uploading.'",
                                   "'Its hourly and daily history on this bridge is deleted; the console keeps uploading.'"],
    # page 2 shows a possible window too; a window cut at the 12-h horizon has no end; possible on both sides of a
    # likely run
    "web/dash-wufc.js#2": ["fr: 'from {from}'", "fr_tm: 'tomorrow from {from}'", "sp_tm: 'tomorrow {from}–{to}'", "sp_to_tm: '{from}–{to} tomorrow'"],
    # the Publishing confirm view's buttons; whole plural templates
    "web/admin-station.js#4": ["'Forget its history?'", "'Forget its settings?'", "keep: 'Keep'", "forget: 'Forget'",
                                 "'Key changed: 1 upload ignored since restart.'", "other: 'Key changed: {n} uploads ignored since restart.'"],
    # the L7 sentences end as the notice does; the 60-day wording
    "web/admin-learn.js#2": ["The pressure here averaged about {d} above what is usual here. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.",
                               "The pressure here averaged about {d} below what is usual here. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.",
                               "No large error found over {n} days."],
    # the Online services sheet -- header, status vocabulary, the key-problem line, the forecast card
    "web/admin-fwd.js#2": ["Sends this station’s readings to the services below. The time, airport weather and the forecast work without it.",
                             "On: one station only, no uploads over your home network (LAN mode).", "Off · last sent {t}",
                             "Paused · last sent {t}", "Key missing: Edit to add it", "Key refused: Edit to check it", "{list} are set up above.",
                             "Weather Underground forecast", "Show WU’s forecast on the dashboard", "Off · last fetch {t}",
                             "Not fetched yet · the first fetch is at about {t}", "Fetched {t} · {day}: {phrase}, {hi}° / {lo}°",
                             "Paused · last fetch {t}", "Fetch now first: it checks the key.",
                             "On the dashboard: tomorrow, from WU once an hour · location {lat}, {lon} (about 11 km)",
                             "Needs this station’s location (Settings, Location).",
                             "WU gives API keys to stations that upload to it. Your console can upload to WU itself, or turn Weather Underground on above.",
                             "Shows tomorrow’s weather from Weather Underground on the outlook card’s second page, marked WU. fauxcloud asks WU once an hour.",
                             "The API key is on wunderground.com → API keys (free for stations that upload to WU). It is not the Station Key.",
                             "WU gets this station’s location with each request, rounded to {lat}, {lon} (about 11 km).", "Save first, then fetch one.",
                             # a fetch paused for memory; the last fetch's result
                             "Paused: not enough memory for the fetch. Save to try again.", "Last fetch {t} · {r}",
                             "Key format: 32 hex characters (the API key, not the Station Key)",
                             # Fetch now queued; a failed attempt is "Tried", never "Fetched"
                             "Queued · runs in about {n} s", "Tried {t} · {r}", "fauxcloud is low on memory, try later",
                             "fauxcloud stayed busy, try again later", "Needs the key, the location and the time"],
    "src/wufc.cpp": ['\\"source\\":\\"Weather Underground\\"'],
    "web/admin-fwd.js": ["Send one now first: it checks the id and key.", "See your station on {svc}", "{svc} on",
                         "Save first, then send one.", "Up to three services: remove one to add another.",
                         "On Windy’s station page you can add your name, a website and a logo; none of it is needed."],
    "web/admin-station.js": ["Set north from the vane", "Reading {d}°: set the offset to {o}°?",
                             # the adopt confirmation (one template per message, so it can be translated whole)
                             "History, settings, learning and forwarding move to {to}, ", "Keep {from}", "Move to {to}",
                             "Same console, new ID",
                             "Turn the vane so it points north and hold it there until two readings agree (up to 3 minutes).",
                             "No two matching readings yet. Is the vane held still and the console uploading?", "Try again","Only for advanced setups: lets devices on your home network send readings."],
    "web/outlook-why.js": ["Fixed rules; uses what this station learned where a part is in use.",
                           "Read-only. The description uses only this station’s readings.",
                           # the source sentence and the admin's footer when a phrase is sourced
                           "From the {name} ({icao}) report, {age} old, {d} away. A reading from this station comes first.",
                           "Read-only. Phrases marked “reported nearby” come from the main airport’s report; the rest from this station’s readings.",
                           # the pressure notices' short sheet + the Airport weather sheet's comparison
                           "Your station reads {p}, within {thr} of nearby airports.",
                           "Your station reads {p}; nearby airports say {ref}. To match, set REL in the console’s SET menu to about {target}.",
                           "Your station reads {p}, more than {thr} away from nearby airports. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.",
                           "The difference has held since {since}.",
                           "The console’s pressure is at its factory value (after a unit change or a power cut). Set REL in its SET menu to about {target} (the last good reading, {age} ago).",
                           "The console’s pressure is at its factory value (after a unit change or a power cut). Set REL in its SET menu to the current sea-level pressure of a nearby official station.",
                           "Nearby airports now: {ref}."],
    # baro_ref, the chip shared with baro_check, and its two endings
    "src/notices.cpp#2": ['"Pressure looks off"', '"Pressure at factory value"',
                        "This station's pressure reads about %s %s nearby airports since %s. Set REL in the console's SET menu to about %s.",
                        "This station's pressure reads about %s %s nearby airports since %s. Set REL in the console's SET menu to the current sea-level pressure of a nearby official station."],
    "web/dash.js#4": ["tr_in: 'in {n} h, {w}'"],  # the trend's words: the tile (with the unit) and the sheet's Now row (without)
    "web/dash.js#5": ["a_warn: 'Warning: {chip}'"],
    # the strip phrases (the firmware's HA text is kept equal by test_table_sync.py)
    "web/outlook.js": ["'Snow reported nearby.'", "'Rain reported nearby.'", "'Drizzle reported nearby.'",
                       "'Thunder reported nearby.'", "'Fog reported nearby.'", "'Mist reported nearby.'"],
    "web/dash-wufc.js#3": ["{pop} % chance of precipitation", "{hi}° / {lo}°"],  # the WU pieces, one wording
    "web/outlook-why.js#3": [  # one sentence per learner in use; the learners' codes in the info sheet
                           "Uses what this station learned about its humidity sensor.",
                           "Uses what this station learned about how fast its clear evenings cool.",
                           "Humidity at {rel} of what this sensor shows in saturated air, light wind, no rain in the last 3 hours."],
    # the Learning sheet's states, L3's console step, L7, the restart
    "web/admin-learn.js": ["'Smallest wind reported'", "The console reports wind in steps; its smallest reading above 0 is about {v}.",
                           "For information; the outlook does not use it.", "Measured {v}", "Default {v} (not yet learned)",
                           "Standard {v} (not yet learned)", "Still learning: about {k} more clear-sky hours.",
                           "It counts hours when the light reaches the clear-sky level; bright haze does not count.",
                           "One bar at {a} clear-sky hours at one sun height, two at {b}, three at {c}.",
                           "No large error found over {n} days.",
                           "Moved or replaced the sensor? Learning restarts from what it knows; the outlook uses the standard rules until each part is learned again.",
                           "Restart learning for {name}? Every part goes back to the standard rules until it is learned again."],
    "web/outlook.js#2": ["day_sunny: 'Sunny.'", "day_spells: 'Sunny spells.'", "day_nosun: 'No sun.'",
                       "min_range: 'Minimum around {range} tonight.'", "fog_possible: 'Mist or fog possible.'"],
    # baro_check in baro_ref's one-chip sentence family
    "src/notices.cpp#3": ['"Pressure looks off"',
                          "This station's pressure averaged about %s %s the usual for this place over %s days. Set REL in the console's SET menu to the current sea-level pressure of a nearby official station."],
    # the Learning tile's status
    "web/adminface.js#5": ["'Paused: set the location'", "learn_n: '{n} of 4 in use'", "'Each part feeds the outlook once learned'"],
    "src/ha_discovery.cpp": ['"Wind speed, 10-min average"', "default_entity_id"],
    # the airport weather sheet -- what leaves the board, and the lookup outcomes
    "web/admin-metar.js": ["Reports every 15 min and forecasts every hour, from aviationweather.gov (US weather service).",
                           "It sees your internet address and the airport codes; “Find airports near you” also sends your area, rounded to about 10 km.",
                           "Add an airport", "Main airport, {d}", "Last report {age}",
                           "No airport with reports near your location. Add one by its code.",
                           "No airport {id} in the aviation weather list.", "{id} is already in your airports.",
                           "More airports than fauxcloud could read: these are the nearest of the ones it read.",
                           "The airports differ by more than 1.5 hPa: the first one in your list is used.",
                           # the list's order is the priority
                           "'Main airport'", "{p} · wind {w}", "Move {id} up", "Move {id} down",
                           # the forecast line, the outlook's words
                           # one template per certainty, the phenomena as words; the last fetch as Online services says it
                           "taf_e: '{what} likely {from}–{to}.'", "taf_p: '{what} possible {from}–{to}.'",
                           # a window cut at the board's horizon: the dashboard's "from {from}" words, no end time
                           "taf_e_o: '{what} likely from {from}.'", "taf_p_o: '{what} possible from {from}.'",
                           "tc_thunder: 'Thunder', tc_snow: 'Snow', tc_rain: 'Rain', tc_fog: 'Fog', tc_gusts: 'Strong gusts'",
                           "last: 'Fetched {t} · {r}'", "tried: 'Tried {t} · {r}'",
                           "Nothing significant forecast.", "No forecast for this airport.",
                           # the badge template
                           "Forecast comes with the next hourly fetch.", "{d} · forecast",
                           # the user's own name for an airport
                           "About 24 characters, fewer with accents. Leave it empty for the name from aviationweather.gov.", "Name for {id}", "Rename {id}"],
    # the dashboard's airport view
    "web/dash-airport.js": ["title_m: 'Airport weather · {main}'", "head: 'Main airport. Not this station’s readings.'",
                            "also: '<span>Also {n}:</span><span>{v}</span>'", "others: 'Others'", "span_o: 'from {from}'", "span_otm: 'tomorrow from {from}'",
                            "n: '{w} at {main}.'", "No current report from {main}.", "s_drizzle: 'Drizzle'", "s_nocloud: 'No low clouds'",  # one template
                            "taf_e: '{what} likely {span}.'", "taf_p: '{what} possible {span}.'", "{from}–{to} tomorrow",
                            "wg: 'gust <b>{g}</b> {u}'", "wv_gust: '<b>variable</b>, gust <b>{g}</b> {u}'",  # under the dial
                            "tomorrow {from}–{to}", "Airport weather is off. Turn it on in Settings, Airport weather.",
                            "Could not load the airport weather. It tries again in a minute.",
                            "<b>{n}</b> (main) {km} · {age}",
                            "Very low clouds", "Low clouds", "'Sky'", "Mid-level clouds", "High clouds", "{c}, base {v} {u}", "Scattered clouds", "No low clouds", "no ceiling", "Few clouds", "Broken clouds", "'Overcast'", "'Clear'",
                            "Reports from aviationweather.gov, for reference: not this station’s readings, not stored.",
                            # the all-airports sheet
                            "'For reference: not this station’s readings.'",
                            "Reports and forecasts from aviationweather.gov, not stored. The order is yours (Settings, Airport weather).",
                            "{n} airports, in your order", "'1 airport'", "'Next 12 h'", "Until {to}", "'No current report'",
                            "{t}: {v}. Open all airports", "'All airports'"],
    # a TAF window that began before now (the board's started flag): no start time
    "web/dash-airport.js#2": ["span_on_o: 'throughout'", "span_on: 'until {to}'", "span_on_tm: 'until {to} tomorrow'"],
    "web/admin-metar.js#2": ["taf_e_n: '{what} likely throughout.'", "taf_p_n: '{what} possible throughout.'",
                                 "taf_e_u: '{what} likely until {to}.'", "taf_p_u: '{what} possible until {to}.'"],
    "web/dash-wufc.js#4": ["sp_on_o: 'throughout'", "sp_on: 'until {to}'", "sp_on_tm: 'until {to} tomorrow'"],
    # the touching-window sentences as sentence templates over whole time phrases (the span words above + 'now'), the day
    # word on the first phrase on the next local day
    "web/dash-wufc.js#5": ["sp: '{from}–{to}'", "nw: 'now'",
                               "pl: '{what} possible {s1}, likely {s2}'", "plp: '{what} possible {s1}, likely {s2}, possible {s3}'",
                               "plpo: '{what} possible {s1}, likely {s2}, then possible'", "lp: '{what} likely {s2}, possible {s3}'",
                               "lpo: '{what} likely {s2}, then possible'"],
    # three later strings: (1) the gauge's precipitation at <= 1.0 °C as shown
    # (skyFx draws flakes at the same value): one state, no intensity word; its HA text and its info-sheet block
    "web/outlook.js#3": ["sleet_snow: 'Sleet or snow.'", "sleet_snow: 'snowflake'"],
    "src/forecast.cpp#2": ['"sleet_snow"', '"Sleet or snow."'],
    "web/outlook-why.js#4": ["sleet_since: 'Wet since {since}. At or below {lim} the gauge’s precipitation may be sleet or snow.'",
                               "sleet: 'At or below {lim} the gauge’s precipitation may be sleet or snow.'"],
    # (2) broken or overcast with the base not reported (///) is a ceiling of unknown height ("no ceiling" stays for
    # few / scattered); the "Also" row's template with the cover word
    "web/dash-airport.js#3": ["base_unk: 'base unknown'", "oc_unk: '{c}, base unknown'"],
    # (3) the chart legend's band (a user read "Lowest to highest" as a sort order)
    "web/dash-sheet.js#2": ["lg_range: 'Range'"],
}

# retired phrases: (label, regex) -- none may appear in web/ or src/
RETIRED = [
    ("the chart legend's 'Lowest to highest' ('Range'; a user read it as a sort order)", r"Lowest to highest"),
    ("the sheet's footnote about a highest value far above the average", r"A highest value far above the average"),
    ("the sheet's unit-carrying Now labels (the unit rides the number)", r"\+ ' (now|gust now|reading|gust|strongest gust|in this range|lowest / highest|dew point|feels like|absolute|today|last hour|this week|this month|this year|last 7 days|highest)'"),
    ("the sheet's sub-labels that said 'now' under the heading Now ('reading', or the bare name)", r"(?:\), |: )'now', (?:k|'uv'|'solar_wm2')\]|'light, now'"),
    ("the sheet's 'gust now' sub-label under the heading Now ('gust'; the raw value is the 'reading')", r"'gust now'"),
    ("the sheet's unit-carrying Now literals", r"'(% now|UV now|W/m² now|% in this range|% humidity)'"),
    ("'average gust' (an hour's gust is its maximum; the gust sheet shows the strongest only)", r"'average gust'"),
    ("the footnote's two endings", r"Such rows are marked ▲ in the table|Open the table to see them marked"),
    ("the airport view's '{main}, your main airport' head (the title + 'Main airport.' say it)", r"your main airport"),
    # replaced wordings
    ("the tendency's 'quickly' ('fast', as the strip)", r"(rising|falling) quickly"),
    ("the i sheet's old sections", r"Next 12 h · \{name\} airport|The airport’s forecast, not this station’s reading|\{dw\} · Weather Underground"),
    ("the loose sun line (a block note)", r"may read high in direct sun"),
    ("the airport list's colon", r"Fetched \{t\}: \{r\}"),
    ("'more than 60 days'", r"over more than 60 days"),
    ("the L7 sheet's old ending", r"; check REL in the console"),
    ("the airport Humidity tile", r"t_hum|function rh\("),
    ("a run-time plural fragment for ignored uploads", r"upload\$\{x\.km"),
    ("the plane mark on page 2 (the source tag)", r"src pl"),
    ("the Airport weather sheet's '120 km' figure", r"within 120 km"),
    ("the internal word 'slot' in the turned-away chip", r"no free slot"),
    ("the Weather description footer's promise of an airport source (the airport source has shipped)", r"airport weather source comes in a later version"),
    ("the main row's long note ('Main airport' / 'Main airport, {d}' in the hint line)", r"shown first on the dashboard"),
    ("the switch name 'Detailed boot log' (it read as dead)", r"Detailed boot log"),
    ("the 'Detailed: on/off' line beside the download (one Bug report group; the switch shows it)", r"Detailed: (on|off)"),
    ("'No cloud detected' in the airport view ('No low clouds', the sensor sees only up to ~3.7 km)", r"'[Nn]o cloud detected"),  # the UI string (a JS literal), not the METAR comment in src/metar.h
    ("the airport forecast's gusts with a number (one family, 'Strong gusts')", r"Gusts to \{gust\} (likely|possible)"),
    ("BARO ... 5 seconds (the console takes 3 s; BOOT 5 s is the factory reset and stays)", r"BARO[^\n]{0,80}5 seconds"),
    ("No NTP", r"No NTP"),
    ("example.invalid placeholder links", r"example\.invalid"),
    ("trial 11-05", r"trial 11-05"),
    ("learning is not built yet", r"learning is not built yet"),
    # the learners are in use; L3 measures the console's step, not the cups' start; L7 never says "right"
    ("learning that changes nothing yet", r"changes nothing yet"),
    ("the cups' start speed", r"cups start turning"),
    ("L7's 'looks right'", r"pressure setting looks right"),
    # one pressure sentence family; the Learning tile is a status, not a promise
    ("baro_check's old 'usual here ... check REL' sentence", r"the usual here for %s days"),
    ("the Learning tile's 'In the background'", r"'Learning', 'In the background'"),
    ("re-cased chips", r"chip\.toLowerCase"),
    ("CSS-only Details word", r'content: "Details"'),
    ("hash timing on the setup page", r"hashed on this phone in"),
    ("a slice of a UI sentence", r"OFF_WHY\.slice"),
    # the "90 days" segment is the calendar; the day rose has a calm count; no glued "per" fragment
    ("the 90 days segment", r">90 days</button>"),
    ("no calm count on the day rose", r"no calm count"),
    ("Rain per + a fragment", r"Rain per \$\{"),
    # the touching-window sentences with bare clock times (no day word across midnight) -> sentence + time-phrase templates
    ("the touching-window sentences with bare times", r"\{what\} (possible (from \{from\}|now), likely|likely (\{from2\}–\{to\}|until \{to\}), (possible until|then))"),
    ("the started forms' own keys (one sentence per order, 'now' is a time phrase)", r"\b(pln|plon|plpn|plpon|lpn|lpon): '"),
    ("the strip's bare chance on the WU line (the i sheet says it as a sentence)", r"\{lo\}° · \{pop\} %"),
]


def sources():
    for sub in ("web", "src"):
        for name in sorted(os.listdir(os.path.join(FW, sub))):
            if name == "web_assets.h" or not name.endswith((".js", ".css", ".html", ".cpp", ".h")):
                continue
            yield sub + "/" + name


class UiText(unittest.TestCase):
    def test_new_sentences_verbatim(self):
        for rel, items in NEW.items():
            text = read(rel.split("#")[0])  # "file#2": a second list for the same file
            for s in items:
                self.assertIn(s, text, "%s: missing %r" % (rel, s))

    def test_retired_phrases_absent(self):
        for rel in sources():
            text = read(rel)
            for label, rx in RETIRED:
                self.assertIsNone(re.search(rx, text), "%s: retired phrase (%s)" % (rel, label))

    def test_the_boot_button_keeps_its_5_seconds(self):
        # the factory reset is a 5-s hold of BOOT by design (HOLD_MS 5000): the retired rule targets BARO only
        self.assertIn("Hold the BOOT button for 5 seconds", read("web/adminface.js"))


if __name__ == "__main__":
    unittest.main()
