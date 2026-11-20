# Releasing fauxcloud

How a fauxcloud release is made, what it contains and how to install it. The first part is for the maintainer; the
asset table and the install notes are for everyone.

## The rules in one paragraph

A release is built **only by CI**, from an annotated tag `v<version>` on `main`, by `.github/workflows/release.yml`.
CI creates a **draft**; it never publishes. The maintainer installs the CI-built image from the draft on a real board,
lets it run for a soak (a release candidate: **overnight, at least 10 hours**; the final release: **one hour**), and only then presses Publish. A binary built on someone's own machine is never
attached to a release. The tag must equal `FC_VERSION_BASE` in `firmware/fauxcloud/app.h`, or the workflow stops.

## Version scheme

`FC_VERSION_BASE` in `firmware/fauxcloud/app.h` is the one place the version lives. It ends up in the image marker that
the update page checks, in the version Home Assistant shows, and in the User-Agent sent to the online services, so it is
always a clean [semantic version](https://semver.org/):

| Stage | `FC_VERSION_BASE` | Tag | Released? |
|---|---|---|---|
| Release candidate | `1.0.0-rc.1`, `1.0.0-rc.2`, … | `v1.0.0-rc.1` | yes, as a pre-release |
| Release | `1.0.0` | `v1.0.0` | yes |
| Fix release | `1.0.1` | `v1.0.1` | yes |
| Between releases | `1.0.1-dev.1`, `1.0.1-dev.2`, … | none | **never**: the workflow refuses a `-dev` version |

The diagnostic image is the same version with `+diag` appended (`1.0.0+diag`); it is built from the same tag.

## Making a release (maintainer)

1. **Prepare on a branch, merge by pull request:** set `FC_VERSION_BASE` to the new version, and add the release's
   section to `CHANGELOG.md` with the heading `## [<version>] - <date>` (the release body takes that section; **a tag
   without its section stops the release**). CI must be green on `main` after the merge.
2. **Tag `main`** (annotated), and push the tag:
   ```bash
   git switch main && git pull --ff-only
   git tag -a v1.0.0-rc.1 -m "fauxcloud 1.0.0-rc.1"
   git push origin v1.0.0-rc.1
   ```
3. **CI builds the draft.** The release workflow checks the tag against `FC_VERSION_BASE`, refuses a tag that is not on
   `main`, runs every compile cell and gate of the normal build, assembles the assets, runs the leak gate on every file
   that goes out (with the private deny-list, which a release requires), and creates a draft release with the assets and
   the body from `.github/RELEASE_TEMPLATE.md`. A release candidate is marked as a pre-release.
4. **Check the draft:** the asset list below, `SHA256SUMS`, the body (the source sentence names the tag; the changelog
   section is there).
5. **Soak:** download `fauxcloud-<version>.bin` from the draft and install it on the maintainer's board through the
   admin page (Maintenance → Firmware update). Let it run overnight, **at least 10 hours** for a release candidate and
   **one hour** for the final release (its image differs from the candidate's only by the version string): uploads
   arrive, MQTT and Home Assistant stay up, forwarding and airport weather run, no unexpected restart.
6. **Publish** the draft by hand. Nothing else publishes it. Then run the site workflow on `main` so the browser
   installer lists the new version (see [The browser installer](#the-browser-installer)).
7. **Open the next cycle:** a pull request that sets `FC_VERSION_BASE` to `<next>-dev.1`.

**If the workflow fails or the soak finds a problem:** delete the draft, fix the problem by pull request, then move to the
next candidate (`-rc.2`). A tag that never had a published release may be deleted and re-created
(`git push origin :refs/tags/v1.0.0-rc.1`); a tag of a published release is never moved or reused.

**Dry run:** *Actions → release → Run workflow* on a branch runs every step on that branch without the tag check and
leaves the assets as a run artifact (`release-assets`). Ticking *draft* also creates a draft release called
`dry-run-<run number>`; delete it afterwards (a draft creates no tag until it is published). A development tree has
no `CHANGELOG.md` section for its version, so a dry run puts a stand-in line in the body (with a notice) where a tag
release would stop.

## If a published release is bad

The board goes back to the previous version by itself only when a new image does not start properly (see
[Installing](#installing)). A release that starts but is wrong has no automatic way back; the maintainer:

1. **Says so on the release:** edits its body to name the problem and the version to use instead. The tag stays (a
   tag of a published release is never moved or reused).
2. **Takes it off the installer:** deletes the release's `fauxcloud-<version>-factory.zip` asset (`tools/site_versions.py`
   skips a release without it), then runs the site workflow on `main` as after a Publish.
3. **Ships a fix release** by the steps above.

A user goes back by installing the previous release's `fauxcloud-<version>.bin` on the admin page (Maintenance →
Firmware update), like any update: **the update page does not block a downgrade**; it checks that the file is a
fauxcloud image, not its version. An older version may not use everything a newer one stored: learned values a newer
version wrote stay read-only for it (not overwritten), and a history store in a layout it does not know shows **History
not saved** (not erased). Over USB, the factory zip of the previous release works as a recovery (see below).

## What a release contains

| Asset | What it is | Who needs it |
|---|---|---|
| `fauxcloud-<version>.bin` | The app image. **This is the update file.** | Everyone updating a board: admin page → Maintenance → Firmware update. |
| `fauxcloud-<version>-factory.zip` | The four parts of a first install, each at its own offset: bootloader (`0x1000`), partition table (`0x8000`), boot selector `boot_app0` (`0xE000`), app (`0x10000`), plus `flash_args`, `manifest.json` (for a browser flasher), `parts.json` (for the installer at fauxcloud.org/install/) and `README.txt` with the `esptool` command. | A first install or a recovery over USB. |
| `fauxcloud-<version>-diag.bin` | The same release with extra diagnostics (`+diag`). Installs like the update file. | Only when a bug report asks for it. |
| `fauxcloud-<version>-elf.zip` | The ELF files of both images. | Decoding a crash report against the exact release. |
| `THIRD_PARTY_NOTICES_FIRMWARE.txt` | The licence texts of the platform components inside the image. | Everyone who passes the binary on. |
| `SHA256SUMS` | SHA-256 of every asset above. | Checking a download: `sha256sum -c SHA256SUMS --ignore-missing`. |

Every release body says where the source is: this repository at the release's tag, built with the pinned Arduino-ESP32
core from `firmware/fauxcloud/sketch.yaml`; `tools/build.ps1` or the CI workflow rebuilds the image. The body also
links the core's upstream source at that version and the project's own copy,
https://github.com/fauxcloud/arduino-esp32/tree/3.3.12. The wording lives in `.github/RELEASE_TEMPLATE.md`.

## Installing

**Updating a running board:** use `fauxcloud-<version>.bin` on the admin page (Maintenance → Firmware update). The board
checks that the file is a fauxcloud image, installs it next to the running one, restarts, and goes back to the previous
version by itself if the new one does not start properly. Settings and history stay. An older
`fauxcloud-<version>.bin` installs the same way (see [If a published release is bad](#if-a-published-release-is-bad)).

**First install or recovery over USB:** unzip `fauxcloud-<version>-factory.zip` and write the four parts at their offsets
(the same line is in its `README.txt`; esptool 4.x spells the command `write_flash`):

```bash
esptool --chip esp32 --port PORT write-flash 0x1000 fauxcloud-1.0.0-bootloader.bin 0x8000 fauxcloud-1.0.0-partitions.bin 0xE000 fauxcloud-1.0.0-boot_app0.bin 0x10000 fauxcloud-1.0.0.bin
```

> **Warning: never write a merged image at `0x0` over a working board.** fauxcloud keeps its settings (Wi-Fi, MQTT,
> keys, learned values) in NVS at `0x9000`–`0xE000`, between the partition table and the app. A merged "factory" image
> covers that gap with padding, so writing it erases the settings; the image the Arduino core builds as
> `*.merged.bin` is padded to the whole 4 MB and erases the history too. That is why a release ships the parts
> separately and no merged image at all. The four parts above write nothing in NVS and nothing after the first app slot.
> For a clean start on purpose, run `esptool --chip esp32 --port PORT erase-flash` first. A browser flasher reading the
> zip's `manifest.json`, and the installer on fauxcloud.org, offer to erase the device first: leave that box unticked to
> keep the settings.

## The browser installer

The installer at `fauxcloud.org/install/` reads the release's own files, nothing built for it alone:

- **`parts.json`** inside the factory zip lists the four parts in flash order, each with its file name, offset, size and
  SHA-256 (the facts `README.txt` states). `tools/release_assets.py` writes it with the zip, from the same checked
  layout as `flash_args` and `manifest.json`. The installer reads the zip in the browser and checks, before writing:
  the zip's size and SHA-256 against `versions.json`; that `parts.json` names this version; each part's size and SHA-256;
  each part at its fixed place (bootloader `0x1000`, table `0x8000`, `boot_app0` on otadata, the app on an app
  partition) and none touching NVS or another kept partition of the release's table; and the board's own partition
  table at `0x8000`: a readable one that differs from the release's stops the install unless the erase box is ticked (a
  blank one, a new board, goes ahead). Then it writes the four parts without erasing anything else, and the flash's MD5
  is compared with each part after its write.
- **`site/firmware/versions.json`** is the installer's list of boards and versions. The site workflow
  (`.github/workflows/pages.yml`) writes it from `site/boards.json` and the published releases (`tools/site_versions.py`,
  drafts left out, newest first, pre-releases marked) and copies each listed factory zip onto the site at
  `/firmware/<version>/`, checked against its SHA-256, so the browser downloads it from the site itself. The copy in
  the repository is a placeholder with no version to install.
- **After pressing Publish,** run the site workflow by hand on `main` (*Actions → pages → Run workflow*): only then does
  the installer offer the new version. A run started by the release event itself would run on the tag, which the
  `github-pages` environment does not deploy from.

## How the build is pinned

- The ESP32 core and toolchain are pinned in `firmware/fauxcloud/sketch.yaml`; the compiler flags are the ones in
  `.github/workflows/build.yml` (the same as `tools/build.ps1`).
- The release workflow runs `build.yml` itself, so a release passes exactly the gates of every push: the compile cells,
  the exception-table guard, the app-slot size gate, the RTC layout, the symbol checks, the web asset checks, the leak
  gate, the REUSE lint and the host tests. `reuse lint` is a gate: every file must carry a licence and a copyright line
  (`REUSE.toml`) and every licence in use must have its text in `LICENSES/`; a new licence identifier needs its file
  there before it merges.
- `SOURCE_DATE_EPOCH` is the tag commit's time, so the build date inside the image marker is fixed by the commit. Two
  runs of one tag are expected to produce the same images; the project will say "reproducible" only after it has seen
  two CI runs give the same `SHA256SUMS`.
- `arduino-cli` itself is pinned (1.5.1, the `version:` of `setup-arduino-cli` in `build.yml`, the same as
  `firmware/README.md`).
- Every third-party action in the workflows is pinned by commit; Dependabot proposes updates.

## For the maintainer: the switches

- **`LEAKGATE_DENY`** (repository secret): the private deny-list, base64. Required by a tag release and by a site
  deploy; the normal build runs the public rules without it (pull requests from forks never see it).
- **`PAGES_ENABLED`** (repository variable): `true` turns on `.github/workflows/pages.yml` (site build → leak gate →
  GitHub Pages). Unset, the site workflow is skipped.
- **The release body template** `.github/RELEASE_TEMPLATE.md` may use `{{version}}`, `{{tag}}`, `{{repo_url}}`,
  `{{changelog}}` (the `CHANGELOG.md` section of this version) and `{{sha256sums}}`. Any other `{{…}}` stops the release,
  and so does a body that does not name the tag and `THIRD_PARTY_NOTICES_FIRMWARE.txt`. The placeholders are filled in
  that order, so a changelog section that quotes `{{tag}}` gets it filled, and one that quotes any other `{{…}}` stops
  the release.
