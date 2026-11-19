# Polish glossary and voice (fauxcloud web faces)

The rules and terms the Polish pack (`pl.json`) follows.

The maintainer's choice wins over any line here, and every correction made on `/try/` comes back into this file.

## B.1 Voice
1. **Plain and short**, sentence case (only the first word and names capitalised: never Title Case), no exclamation marks.
2. **Second person singular, implicit:** "Ustaw REL w menu SET konsoli", "Sprawdź baterie". **No "proszę"**, no "Państwo",
   no impersonal "należy", no diminutives ("chwilkę", "danych troszkę").
3. **Names and data stay as they are:** fauxcloud (lower case), station names, airport names, ICAO codes, METAR, TAF, REL,
   SET, BARO, AP, NTP, MQTT, Home Assistant, Weather Underground, Windy, Wi-Fi; **units unchanged** (°C, km/h, hPa, mm, %,
   W/m², UV, h, min, s) with a space before them; numbers are formatted by the code (`12,5`), never typed into the pack.
4. Punctuation: Polish quotes „…”, an en dash with spaces in prose (" – "), none in a time span (`14:00–15:00`), the ellipsis
   character `…`.
5. **Length:** labels (tiles, chips, buttons, segments, table heads) at most ≈ +30 % of the English; where it is tight use the
   short forms in use: "Maks. poryw dziś" for "Peak gust today", "Średnia (1 h)" for "Average, last hour".

## B.2 Rules of form
1. **A Polish sentence is a whole template**: one key, one sentence; never build Polish from two keys, never reuse a
   fragment of one template in another even when the words repeat.
2. **Plurals:** every count template has `one`, `few`, `many`, `other` ("1 wiersz", "2 wiersze" / "22 wiersze", "5 wierszy"
   / "12 wierszy", "1,5 wiersza"): `one` = 1 only; `few` = ends in 2–4 except 12–14; `many` = 0, 5–21, and ends in 0–1 or
   5–9 above 21; `other` = fractions.
3. **Keep every `{slot}` and every tag** of the English (`<b>…</b>`, `<a>…</a>`); the order may change.
4. **A slot that carries a noun** (`{what}`, `{station}`, `{measure}`) stays in the nominative and nothing else agrees with
   it: phrase around it ("{what}: prawdopodobnie {span}", "{what}: możliwe {span}", "Stacja {station}: offline"). Where no
   natural phrasing exists, **list the key** (the English would need one template per noun: a later change to the English).
5. **One English word, several roles** gets the form of its role (a legend, a value and a phenomenon differ): a chart legend "Porywy", a value
   "Poryw"; a tile "Opady", a reported phenomenon "Deszcz"; a button "Wyczyść", the sky "Bezchmurnie".
6. **State words agree with their noun** (`Off` of a service: "wyłączona"; of MQTT: "wyłączony"): translate per key, never
   one shared word.

## B.3 Terms (as the project uses them)
| English | Polish | note |
|---|---|---|
| Wind / Gust (value) / Gust (legend) | Wiatr / Poryw / Porywy | IMGW "poryw", never "podmuch" |
| Peak gust today / strongest gust | Maks. poryw dziś / najsilniejszy poryw | |
| Average, last hour / last 10 min | Średnia (1 h) / Średnia (10 min) | short forms |
| Calm (wind) / Calm. (outlook) | cisza / Bezwietrznie. | Beaufort 0 = "cisza" |
| Temperature / Dew point / feels like | Temperatura / Punkt rosy / odczuwalna | |
| Humidity / Indoor / Indoor humidity | Wilgotność / W domu / Wilgotność w domu | |
| Pressure / sea level / absolute | Ciśnienie / poziom morza / na poziomie stacji | |
| Pressure steady / rising / rising fast | Ciśnienie stałe. / Ciśnienie rośnie. / Ciśnienie szybko rośnie. | |
| Pressure falling: less settled weather possible. | Ciśnienie spada: możliwa zmiana pogody. | |
| Pressure falling fast: unsettled spell possible. | Ciśnienie szybko spada: możliwe załamanie pogody. | |
| Rain today / Rain, last hour / Rain per hour, day, week | Opad dziś / Opad (1 h) / Opad na godzinę, dzień, tydzień | a tile and a sum: "opad" |
| No rain. / Light / Moderate / Heavy rain. | Bez opadów. / Słaby / Umiarkowany / Silny deszcz. | IMGW intensity words |
| Sleet or snow. / Recent rain. | Deszcz ze śniegiem lub śnieg. / Niedawno padało. | |
| Sunny. / Sunny spells. / No sun. | Słonecznie. / Przejaśnienia. / Bez słońca. | "Pochmurno." for No sun is an open question |
| UV and sun / UV index / Sun | UV i słońce / Indeks UV / Słońce | |
| Mist or fog possible. | Możliwe zamglenia lub mgła. | |
| Icy surfaces possible. | Możliwe oblodzenie nawierzchni. | IMGW "oblodzenie" |
| Hoar frost likely on cold surfaces. | Prawdopodobny szron na zimnych powierzchniach. | |
| Ground frost possible. / Severe frost. / Freezing. | Możliwy przymrozek przy gruncie. / Silny mróz. / Mróz. | |
| Hot. / Windy. / Humid. | Upał. / Wietrznie. / Parno. | "Parno" in warmth |
| Gusts to {gust}. | Porywy do {gust}. | |
| Minimum around {range} tonight. | W nocy minimum około {range}. | |
| Outlook (the strip) / Forecast (WU, TAF) | Prognoza lokalna / Prognoza | keep the two apart |
| Thunder(storm) / Snow / Rain / Drizzle / Fog / Mist | Burza / Śnieg / Deszcz / Mżawka / Mgła / Zamglenie | METAR/TAF words |
| Freezing rain / Strong gusts | Marznący deszcz / Silne porywy | |
| likely / possible | prawdopodobnie / możliwe | B.2 rule 4 |
| Clear / No low clouds / Partly cloudy / Mostly cloudy / Overcast | Bezchmurnie / Brak niskich chmur / Częściowe zachmurzenie / Duże zachmurzenie / Całkowite zachmurzenie | a headline on its own: "Częściowe" alone reads as a fragment |
| few / scattered / broken clouds | małe / umiarkowane / duże zachmurzenie | the aviation terms (nieliczne, rozproszone, poprzerywane) only if the maintainer chooses them |
| Very low / Low / Mid-level / High clouds | Bardzo niskie / Niskie / Średnie / Wysokie chmury | |
| base unknown / no ceiling | podstawa nieznana / brak pułapu chmur | |
| Visibility / variable (wind) | Widzialność / zmienny | |
| Now / Today / Yesterday / Tomorrow | Teraz / Dziś / Wczoraj / Jutro | |
| throughout / until {to} / from {from} | cały dzień / do {to} / od {from} | "cały dzień" is the maintainer's choice: a deliberate simplification (the forecast refreshes anyway; "przez cały czas" is not natural Polish) |
| Next 12 h / Forecast, next 12 h: | Następne 12 h / Prognoza na 12 h: | |
| {pop} % chance of precipitation | prawdopodobieństwo opadów {pop} % | |
| Range / Average / lowest / highest / total / change | Zakres / Średnia / min. / maks. / suma / zmiana | "min."/"maks." avoid gender agreement |
| mostly from | przeważnie z | |
| Export CSV / rows exported | Eksportuj CSV / wiersz, wiersze, wierszy | plurals B.2 |
| Online / Offline / Updated {ago} | Online / Offline / Zaktualizowano {ago} | `{ago}` from `Intl` ("5 min temu") |
| Waiting for the first upload / Battery low | Czekam na pierwsze dane / Słaba bateria | |
| Loading… / Could not load … | Wczytywanie… / Nie udało się wczytać … | |
| Bridge health | Stan fauxcloud | |
| Beaufort 0–12 | cisza, powiew, słaby wiatr, łagodny wiatr, umiarkowany wiatr, dość silny wiatr, silny wiatr, bardzo silny wiatr, wicher, silny wicher, sztorm, gwałtowny sztorm, huragan | the Polish Beaufort names |
| compass N, NE, E … | unchanged letters | Polish abbreviations are a possible later change |

## B.4 Not translated
Station and airport names, ICAO codes, CSV headers, the `/tools` page, log lines, the WU forecast phrase (`{phrase}`: Weather
Underground sends it in English; asking WU for Polish is a firmware change, not made yet).


## B.5 The admin face (Settings): terms
Actions are the imperative of the second person singular, like the rest ("Zapisz", not "Zapisanie"); state words agree with
their noun (B.2 rule 6). The firmware's own error lines and the setup page stay English.

| English | Polish | note |
|---|---|---|
| Settings (the face) / Dashboard | Ustawienia / Pulpit | |
| Save / Cancel / Close / Done / Back | Zapisz / Anuluj / Zamknij / Gotowe / Wstecz | |
| Edit / Remove / Add / Keep / Forget / Block / Unblock | Edytuj / Usuń / Dodaj / Zachowaj / Zapomnij / Zablokuj / Odblokuj | "Forget a station": Zapomnij stację |
| Sign in / Sign out / Password / Admin password | Zaloguj / Wyloguj / Hasło / Hasło administratora | |
| Station / console / bridge (fauxcloud itself) | stacja / konsola / fauxcloud | "bridge" in UI text means fauxcloud: write "fauxcloud" |
| Upload (the console's) / uploads | wysyłka danych / wysyłki | "the console uploads" = konsola wysyła dane |
| General / Filters / Extra sensors / Calibration / Console | Ogólne / Filtry / Dodatkowe czujniki / Kalibracja / Konsola | the station sheet's tabs |
| Publishing / Online services / Airport weather / Learning / Maintenance | Publikowanie / Usługi online / Pogoda z lotnisk / Uczenie / Konserwacja | sheet titles |
| Home Wi-Fi / home network / fauxcloud's own Wi-Fi | domowe Wi-Fi / LAN / Wi-Fi fauxcloud | the maintainer's choice: "home network" = LAN |
| Location and time / time zone | Lokalizacja i czas / strefa czasowa | |
| Restart / factory reset / firmware update | Uruchom ponownie / Przywróć ustawienia fabryczne / aktualizacja oprogramowania | a button is an imperative |
| Boot log / bug report / detailed log | dziennik uruchomienia / zgłoszenie błędu / szczegółowy dziennik | |
| Reading / readings / rejected | odczyt / odczyty / odrzucony | counts: B.2 rule 2 |
| Minimum / Maximum / Max change per minute | Minimum / Maksimum / Maks. zmiana na minutę | |
| Hidden / Off / On | Ukryty / Wyłączone / Włączone | agree with the noun per key (B.2 rule 6) |
| Key (WU station key, API key) | klucz | "Station Key" stays a WU name inside a sentence |
| sensor (Home Assistant) / entity / device | czujnik / encja / urządzenie | Home Assistant's own Polish terms |
| from (the wind's direction: the sheet's Now row, its table header, its row label) | kierunek / Kierunek | the maintainer's choice; "od" stays for a date range's start ("Od" in the range picker) |
| the light on fauxcloud (the board's LED) | LED | the maintainer's choice: "LED", never "lampka" or "dioda" |
| dashboard pages open in browsers ("close the ones you do not use") | sesje | the maintainer's choice: "Zamknij nieużywane sesje", not "strony pulpitu" |

## B.6 Polish style (worked on admin-station … admin-sheets)
The edits made there are the reference; every rule below names one of them. The English stays as it is: these rules are
about the Polish only, and they win over a literal reading of the English.

1. **Terser than the English, not literal.** Say it once and drop what the screen already says: "Surowe dane z konsoli.
   Hasła ukryte. Kopię można bezpiecznie udostępnić." (`admin-station.con_h`); "Ustawienia i historia zostaną usunięte
   nieodwracalnie." (`admin-fwd.sure_s`); "Informacyjnie." (`admin-learn.info`).
2. **No place qualifiers.** Drop "here", "on this board", "in this fauxcloud", "this sensor": "Pogodne noce sięgają…" not
   "Pogodne noce tutaj sięgają…" (`admin-learn.l2n`, `l4`, `l5`, `l7hi`); "za mało pamięci na szyfrowaną wysyłkę." not
   "… na tej płytce." (`admin-fwd.paused_mem`); "Jej zapisane ustawienia zostaną usunięte" not "… w tym fauxcloud …"
   (`admin-station.fq_set_h`). The subject "fauxcloud" goes wherever the sentence reads fine without it (`admin-sheets.x_upd_h`,
   `x_nomem`).
3. **Technical terms stay technical:** sieć domowa → **LAN** (`admin-station.pub_lan`, `admin-fwd.lan_q`); adres internetowy →
   **adres IP** (`admin-metar.on_s2`); płytka → **urządzenie** where a device must be named (`admin-station.pub_why`); dane o
   awarii → **dane diagnostyczne** (`admin-sheets.x_cd_gone`, `x_cd_done`); najniższa → **min** (`admin-station.hd_mem`);
   32 znaki szesnastkowe → **32 hex** (`admin-fwd.e_key_format`).
4. **Numbers as digits, "about" as "~":** "do 3", "Maksymalnie 3 usługi" (`admin-metar.yours`, `admin-fwd.full`);
   "(~ 11 km)" for "(about 11 km)" in parentheses (`admin-fwd.fc_info`, `fc_e3`); "zaokrągloną do 10 km" (`admin-metar.on_s2`).
5. **Shorter verbs and states:** "ostatnio pobrano {t}" not "ostatnie pobranie {t}" (`admin-fwd.fc_off_last`);
   "Sprawdzanie…" not "Testowanie…" (`admin-sheets.m_testing`); "Zapisz" not "Loguj" (`admin-sheets.x_logv`); "Najpierw
   pobierz" not "Najpierw pobierz teraz" (`admin-fwd.fc_check`); "najpierw włącz" not "najpierw to włącz" (`admin-metar.r_off`);
   "Ponowne uruchamianie…" not "Uruchamianie ponownie…" (`admin-sheets.x_rebooting`); "Przywróć ustawienia fabryczne" (a
   button is an imperative, `admin-sheets.x_reset`).
6. **Rephrase instead of a calque:** "Nierealna wartość" not "Niemożliwa wartość" (`admin-station.rl_bounds`); "Jeżeli się nie
   zgadza, …" not "Jeśli to wygląda źle, …" (`admin-station.p_wrong`); "Czujnik odczytał słońce" not "słońce dotarło tam do
   czujnika" (`admin-learn.legend`); "Brak poważnych błędów w ciągu {n} dni." not "Nie znaleziono dużego błędu…"
   (`admin-learn.l7ok`); "Zostaje przy obecnej wersji." not "Działająca wersja pozostaje." (`admin-sheets.x_not_inst`).
7. **Drop the second half of a sentence when it only repeats the obvious:** "Najpierw ustaw lokalizację (Lokalizacja i czas),
   aby znaleźć lotniska w pobliżu." without "Wpisany kod działa i tak." (`admin-metar.loc`); `admin-fwd.h_windy` without the
   optional profile sentence.
8. **Consistency:** one form per idea across keys ("ostatnio pobrano" vs "ostatnio pobrane": both are in the pack today,
   an open check); "Na razie" (two words).
