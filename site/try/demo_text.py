"""The demo bar's words, word for word (site/try/demo.js: the W, EX and SC tables). A wording change edits both in the
same commit; test/tools/test_site_text.py compares them both ways."""

TEXTS = {
    "W": [
        "Demo controls",
        "Example day",
        "Time of day",
        "Day",
        "Night",
        "Scenarios",
        "Live · next upload in {n} s",
        "Live · the console has stopped uploading",
        "Loading the demo data…",
        "The demo data did not load. Reload the page to try again.",
        "Demo data: a synthetic year for a station in central Warsaw. Nothing is saved.",
        "demo: not saved",
    ],
    # the example days of demo/fixtures/index.json by id
    "EX": ["Winter day", "Spring day", "Summer day", "Autumn day"],
    # the scenarios of demo/fixtures/index.json by id
    "SC": ["Today", "Sunny", "Rain", "Snow", "Thunderstorm", "Frost night", "Pressure off", "Offline", "No sources"],
}
