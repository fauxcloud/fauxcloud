# site/ — the static site

The site (fauxcloud.org) is plain files built by `tools/build_site.py` (stdlib Python plus the asset generator's pinned
node + terser + zopfli) into `site/_build/` (not in git). No analytics, no cookies, no external fonts, scripts or CDNs,
no network requests: every link to a page or file of the site is a root path on the same host.

**Deploys only with the flasher.** The landing speaks of installing from this site, over USB, in the present tense, so the
site goes public only together with `/install/` and its browser installer, never before.

**The landing's live demo** is an `<iframe>` of `/try/#frost-night` (lazy): it loads the whole demo app (the dashboard's
assets and the demo data) on top of the landing, outside the landing budget below. Accepted: the demo section is the live
frame by choice.

| URL | What | Source |
|---|---|---|
| `/` | the landing page | `templates/landing.html` (+ `diagram.html`, `demo.html`: the live `/try/` in a phone frame) |
| `/docs/` | Get started (a copy; its canonical URL is `/docs/get-started/`) | GitHub Pages has no redirects |
| `/docs/all-pages/` | all pages, with their sections (the sidebar's Index) | generated from the docs pages |
| `/docs/<slug>/` | one docs page | a Markdown file: `docs/*.md`, `SECURITY.md`, `CONTRIBUTING.md`, `CHANGELOG.md` (the table `DOCS` in `build_site.py`) |
| `/install/` | the browser installer (below) | `templates/install.html`, `install/install.js`, `assets/esptool-bundle.js` |
| `/firmware/versions.json` | the installer's boards and versions | `firmware/versions.json` (a placeholder until the first release) |
| `/try/` | the click-through demo | the shipped dashboard, see below |
| `/404.html` | the not-found page | `templates/notfound.html` |

Every page shares `templates/base.html` (top bar, footer, the share-card tags) and `templates/sprite.svg` (the mark,
the outlined wordmark, the icons). One stylesheet: `tokens.css` + `site.css`, minified, at `/assets/site-<hash8>.css`;
one script: `site.js` (the theme toggle, the phone menu and docs sidebar, the code copy buttons), mangled by terser,
at `/assets/site-<hash8>.js`.

**Theme:** dark by default on every page, whatever the system says; light is the visitor's choice with the toggle,
kept per browser in `localStorage` under `fc-theme` (the same key as the device). No `prefers-color-scheme` anywhere.

**Tokens never drift:** `tokens.css` is generated from the dashboard's `firmware/fauxcloud/web/app.css` (face `dash`:
its `:root` block and its `:root[data-theme="light"]` block, as written). A build rewrites it when `app.css` changed
(commit the result); `--check` fails when the committed file differs. The site's own additions (the sky = the night set
in dark, the dry-day set in light; `--mono`, `--bezel`) are at the top of `site.css`.

## Writing a docs page

A docs page is a Markdown file in `docs/` that also reads well on GitHub. Start it with:

```
---
title: Get started
order: 10
lede: One sentence that says what the reader has at the end of the page.
---
```

and add it to `DOCS` in `tools/build_site.py` (slug, source, sidebar group). The renderer is `tools/site_md.py`, a
small stdlib subset: `##` / `###` / `####` headings (h2s are listed on "All pages" and in the sidebar under the current page; a
page with at most 3 h2s and at least 5 h3s lists its h3s there too, `H3_SIDEBAR`; `####` groups items inside a sub-section), paragraphs, `**bold**`, `*italic*`, `` `code` ``,
links, bullet and numbered lists (one nesting level), steps (a numbered item with a second paragraph starting
`**You should see**`), callouts (`> [!NOTE]` or `> [!TIP]`: the quiet note with an info icon, for an aside such as a
pointer, a tip or a reassurance; `> [!WARNING]`: the amber box, for a risk; first bold line = the title, else the kind
names it), fenced code with a language and `title="…"`, pipe tables (every row has the header's cell count, or the build
fails; on a phone, a table of three or more columns shows each row as a card, its cells under their column names, with
ARIA table roles), `---`, a button: a paragraph that is only `[**Label**](target)`,
a photo: a paragraph that is only `![alt](../site/assets/<name>-<width>.jpg)` at the page's top level, not in a list
item or a callout (one variant of `tools/site_images.py`; GitHub shows that file, the site every width of it, beside
the text from 600 px; photos one after another make one row, in their order; a title in quotes, `![alt](path "Caption")`, becomes its caption; with script, a click opens the
largest variant in a popout), and a labelled entry: a paragraph that
starts `**Cause.**`, `**Fix.**` or `**fauxcloud shows this as**` renders as a `<dl>` with the label in its own column (a
label alone takes the list after it and the plain paragraphs after that list). Raw HTML is escaped; an image inside a
line and HTML blocks are refused.

Links: another docs file by its relative path (`troubleshooting.md#time-is-one-hour-off`) becomes its site page; any
other file of the repository becomes its GitHub page; `https://fauxcloud.org/…` becomes a root path. A link to a
missing file, a private path (the notes, scratch and capture folders, `secrets*`) or out of the repository fails the
build. Every behaviour sentence comes from the code or `docs/api.md`, and no page makes a claim the code does not back.

## Checks (`--check`, what CI runs)

- `/try/`: the shells and assets are the firmware's bytes (below).
- `tokens.css` matches `app.css`.
- Every internal `href` / `src` / `srcset` resolves to a built file, every `#anchor` to an id on its page; no link to a
  shop; every external link goes to a host in `HOSTS` (`build_site.py`: a new host is a deliberate change there).
  `--links` prints the external links.
- Every page carries one share card (`og:*`, `twitter:card`), the non-affiliation line, and none of: internal paths,
  decision numbers, internal ids, the words "owner", "supervisor", "brief", Polish text.
- Budgets: the landing's HTML + CSS + JS **≤ 30 KB gzipped** (images not counted), and the same for `/install/` (its
  page, the stylesheet, `site.js` and `install.js`; the vendored bundle apart); the images one page loads at most (per
  `<img>` / `<picture>`, its largest candidate) **≤ 450 KB**. `build_site.py` prints them after every build.
- The installer: `install.js` and the bundle on `/install/` only; `firmware/versions.json` valid and in step with
  `boards.json` (labels, photo and its pixel size), every same-origin url built and matching its SHA-256; the bundle's
  bytes equal the SHA-256 in `SOURCES.md`.

## Images

The photos on the site are variants of private originals, made once on a dev machine by `tools/site_images.py`
(PIL): every metadata block dropped, two widths per use (1x and 2x), JPEG quality 80 (79 for the variants in
`QUALITY_FOR`), committed under `site/assets/`. The case renders (`case-*.jpg`) are the case designer's, used with
permission (`COPYRIGHT.md`).
Photos are never cropped by CSS: each `<img>` carries the file's width and height.

## Screenshots of the admin face

The four `shot-listen-*` pictures on Report a console come from the laptop mock of the admin face
(`test/tools/capture_mock.py`, fictional data only: the AP `fauxcloud-1A2B`, `example.net` hosts, the documentation MAC
prefix `00:00:5e`), taken at phone size by a headless browser:

1. Start the mock (`python -I test/tools/capture_mock.py --node <node>`) and set its scenario `shot`
   (`http://localhost:8399/__scn?s=shot`): a release version, a fixed start time, every pause line, the whole session
   played by the first poll. Stop a running capture first (`POST /api/admin/capture/stop`).
2. Start Edge or Chrome headless with `--remote-debugging-port=9333`, then for each of `home`, `wifi`, `start`, `listening`:
   `node tools/site_shot.js 9333 "http://localhost:8399/admin?shot=<name>" shot-listen-<name>.png` (375 × 812 at 2x,
   dark; `?shot=` opens the sheets by itself). Name the `listening` one `shot-listen-report.png`.
3. `python tools/site_images.py <that folder> --only shot-listen-home.png shot-listen-wifi.png shot-listen-start.png
   shot-listen-report.png`.

## The share card

`site/assets/social.png`, 1280 × 640, < 1 MB: the mark + wordmark, the hero line, the tagline and `fauxcloud.org`
over an illustration (the station, the board in a dashed cloud, the home) on navy. Its source is a private HTML
file, rendered by headless Edge at exactly 1280 × 640; the PNG carries no metadata chunk (IHDR, IDAT, IEND only). Every page links it in `og:image`; the same file is the
repository's social preview (Settings → Social preview).

## `/try/`: the real dashboard on demo data

`/try/` is the dashboard and the admin face of the current commit, byte for byte, running on the synthetic year in
`demo/fixtures/` instead of a board:

- The build imports `tools/gen_web_assets.py` and writes the same pages and assets the firmware serves: every asset at
  its hashed path `/a/<hash8>/<name>` (uncompressed, for a static host), and `/try/index.html` + `/try/admin.html` =
  the firmware's `index.html` and `admin.html` with two added blocks: one line, `<script src="/try/demo.js"></script>`,
  before the first page script, and the share-card tags right before `</head>`; and one link: the dashboard's Settings
  button points at `/try/admin.html` instead of the board's `/admin` (`TRY_LINK`), so a crawler that follows the raw href
  finds a page. The check changes it back before it compares the bytes. Nothing else of the dashboard is copied or
  edited for the demo, so every fix reaches it at the next build.
- `site/try/` holds the extras: `demo.js` (the shim and the demo bar), `demo.css` (the bar, on the faces' colour
  tokens) and `demo_text.py` (the bar's words, pinned). `demo/replay.js` and the fixtures are copied next to them
  (`/try/replay.js`, `/try/data/`).
- The shim wraps `fetch` (and `XMLHttpRequest`, for the firmware upload) before the page scripts run. Every `/api/*`
  request is answered from `replay.js` with the board's headers (`X-Fauxcloud-Face`, an `ETag` and a real 304 where the
  board sends them); everything else, the hashed assets included, goes to the network. The admin face opens signed in.
  Every write is refused the way the board refuses one, with `demo: not saved` as the reason, and the bar's
  "Nothing is saved." lights up. How the demo time maps onto the viewer's clock: `demo/README.md`, "Time".
- The bar: an example day (four seasons, day or night), the scenarios of `demo/fixtures/index.json`, a live line (the
  next upload) and one note. The choice is kept in the address (`/try/#rain`) and for the tab's session, across the two
  faces.

## The installer (`/install/`)

One card for Chrome or Edge on a computer (Web Serial; elsewhere the page says so and points to esptool): a big state
word (SELECT BOARD → CONNECT USB → CONNECTING → CONNECTED → FLASHING n % → DONE, or NO BOARD / FAILED with one sentence
of what to do), the **Board** and **Version** selects, one button, the driver help, a collapsed flashing log. The page's
own script is `install/install.js` (minified like `site.js`, at `/assets/install-<hash8>.js`); its top half is plain
functions that `test/tools/install_vectors.js` runs in node.

- **What it checks before writing:** the zip's size and SHA-256 against `versions.json` (a row without them is never
  installed); `parts.json` (written by `tools/release_assets.py`) names the chosen version; each part's size and
  SHA-256; each part at its fixed place (bootloader `0x1000`, table `0x8000`, `boot_app0` on otadata, the app on an app
  partition) and none touching NVS or another kept partition of the zip's table; the board's own table at `0x8000`: a
  readable one that differs from the zip's stops with "This board has a different flash layout. Tick Erase everything,
  settings included, and press Connect, then install again."; a blank or unreadable one (a new board) goes ahead.
- **What it writes:** the four parts at their offsets with esptool-js, **without erasing**: fauxcloud keeps its
  settings in NVS between the table and the app. The checkbox "Erase everything, settings included: for a clean start
  only" is unticked by default. After each part the library compares the flash's MD5 with the part's; a mismatch says
  "The write did not verify. Unplug the board, plug it in again and install again." (an erase would not help it)
- **Sequencing:** `makeInstaller()` runs connect and install with the library, the port and the page injected; each job
  holds an epoch, so after an unplug and a quick reconnect the old job never touches the page or the new connection
  (`install_vectors.js` drives it with a fake library).
- **After the flash:** the setup Wi-Fi name `fauxcloud-setup-XXXX`, where XXXX is the last two bytes of the board's
  factory MAC in upper-case hex (what the firmware uses; esptool-js reads the same MAC at connect), and a QR code
  that joins it (`WIFI:T:WPA;S:…;P:fauxcloud;;`, drawn by the page itself), then Get started.
- **esptool-js** is vendored as `assets/esptool-bundle.js` (version, source, SHA-256 and how it was taken:
  `SOURCES.md`; licences: `THIRD_PARTY_NOTICES_SITE.txt`, served at `/third-party-notices.txt`, linked from every page's footer, and next to the installer at `/install/third-party-notices.txt`). `install.js`
  imports it when Connect is pressed; no other page loads it. `.gitattributes` keeps its bytes from line conversion.
- **Boards:** `boards.json`, human-curated (label, chip, flash size, photo with its pixel size, the release asset name).

### The installer's firmware files (versions.json, CORS)

`tools/site_versions.py` writes `firmware/versions.json` from `boards.json` and the GitHub Releases API (drafts left
out, newest first, pre-releases marked, each zip with its SHA-256 and date). The site workflow runs it before the build
and with `--mirror`: each listed zip is copied onto the site at `/firmware/<version>/` (after the site's leak gate; the
release's gate scanned the parts already) and listed there, so the browser never fetches a release asset from
another origin. The order: versions → build → leak gate → the zips join → `build_site.py --check --firmware-extra DIR
--served site/_build` (the uploaded folder equals the checked build, and every row matches its served zip: present,
size, SHA-256) → deploy. Whether GitHub's release downloads allow a cross-origin `fetch` is not relied on; it is checked once at
the first public release (without `--mirror` the list points at the release assets, which works only if they do). The
committed `versions.json` is a placeholder with no installable version.

### A rehearsal before any release (`--local-firmware`)

```
python -I tools/build_site.py --local-firmware      # takes firmware/fauxcloud/build/default/
python -m http.server -d site/_build 8000 --bind 127.0.0.1   # then Chrome or Edge at http://127.0.0.1:8000/install/
```

`--local-firmware [DIR]` makes a factory zip of this computer's firmware build with the release's own code and checks
(`release_assets.factory_from_build`: the image marker's version, the table equals `partitions.csv`, the layout keeps
NVS) and lists it first, "built on this computer", in the build's `versions.json`. It lands in `site/_build/firmware/local/`
only: never in a tracked file, never in the export, never in a deploy (the site workflow does not pass it). Use
`127.0.0.1` or `localhost`: Web Serial and the SHA-256 checks need a secure context.

The build must be **without `secrets.h`**: a build with the developer's seed is refused (its `sketch/` copy holds
`secrets.h`; `release_assets.py assemble` refuses it the same way), because a freshly installed board would join the
developer's Wi-Fi and skip setup. Copy the sketch folder without `secrets.h` under another name (e.g. `fc0/`, with
`fc0.ino`), build that with the same profile and pass its build folder: `<name>.ino.*` parts are read as the release's.

## Build, check and look

```
cd tools; npm ci --ignore-scripts; cd ..          # once: the generator's pinned tools (firmware/README.md)
python -I tools/build_site.py                     # site/_build/ (and site/tokens.css when app.css changed)
python -I tools/build_site.py --check             # what CI runs: the demo's bytes, tokens, links, words, budgets
python -m http.server -d site/_build 8000         # then open http://localhost:8000/
```

The pages link root-absolute paths (`/a/…`, `/try/…`, `/docs/…`), so the build works served at a domain root only,
not under a project path such as `/<repo>/`. `--out DIR` builds elsewhere: an absent or empty folder, or an earlier
build (each build writes a `.fauxcloud-site-build` marker; a folder without it is refused, never emptied).

Tests: `test/tools/test_build_site.py` (the demo's bytes, the pages, share cards, links, words, tokens, budgets, the
Markdown subset, the installer's checks and `--local-firmware`), `test/tools/test_site_text.py`,
`test/tools/test_site_versions.py`, `test/tools/test_release_assets.py` (`parts.json`), `node test/tools/demo_vectors.js`,
`node test/tools/install_vectors.js`.
