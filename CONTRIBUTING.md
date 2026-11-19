# Contributing to fauxcloud

Thank you for helping. fauxcloud is a small project with one maintainer, so please follow the rules below. They save the
maintainer's time for reviews and keep the firmware working on real boards. How decisions are made is in [GOVERNANCE.md](GOVERNANCE.md);
everyone follows the [Code of Conduct](CODE_OF_CONDUCT.md).

Issues and pull requests are read in batches, roughly once a week. No response time is promised. An issue labelled
`needs-info` gets a reminder after 30 days without an answer and closes 7 days later; a comment keeps it open.

## What helps most

| Contribution | How |
|---|---|
| **A report for a console that is not on the list** | The [console report](https://github.com/fauxcloud/fauxcloud/issues/new?template=console_report.yml) form. A model is called supported only with a report from a real console: the raw lines from the station's Console tab in Settings and photos of the label and the setup page. |
| **A bug report** | The [bug report](https://github.com/fauxcloud/fauxcloud/issues/new?template=bug_report.yml) form, with the diagnostics block it asks for. |
| **A fix** | Open an issue first (or use the bug report). When the maintainer labels it `accepted`, send a pull request that fixes it and adds a test that fails without the fix. |
| **A new feature** | Please do not open a feature pull request without discussing it first. Feature pull requests without an `accepted` issue are closed; forks are always welcome. |
| **Documentation** | Corrections to the README and `docs/` are welcome as small pull requests. |
| **A security problem** | Never in public: see [SECURITY.md](SECURITY.md). |

Ideas and questions belong in GitHub Discussions; the issue forms are for bug reports and console reports.

The dashboard and Settings come in English and Polish; setup is in English only. The site and the documentation are
English only. More languages come later, with a translator's guide, so please do not start a translation yet.

## Pull requests

1. **An `accepted` issue first.** Link it in the pull request ("Fixes #…").
2. **Small and focused.** One change per pull request; no unrelated reformatting.
3. **Tested.** Run the checks below. A change to the firmware is also tested on a real ESP32 board, and, if it touches
   the console side, with a real console. Say which board, console and console firmware in the pull request.
4. **Bench test before the merge.** The maintainer builds your branch and runs it on a separate bench board. Fixes
   found there are added as separate commits on top of yours.
5. **Squash merge.** Your commits are combined into one, with a message written at review time. You stay the author;
   GitHub adds the maintainer as co-author when the maintainer's fixes are part of it.

### Sign your commits off (DCO)

fauxcloud uses the [Developer Certificate of Origin](https://developercertificate.org/) instead of a contributor
licence agreement. Add this line to every commit message, with the e-mail address of the commit's author:

```
Signed-off-by: Your Name <you@example.com>
```

The line says you wrote the change, or have the right to submit it,
under the project's licence (GPL-3.0-or-later; the documentation under CC BY-SA 4.0). `git commit -s` adds the line;
`git rebase --signoff main` adds it to commits you already made. A check on every pull request refuses a commit without
it.

### AI-assisted contributions

Using AI tools is fine if you understand every line you send and you tested it on hardware. Say in the pull request that
you used AI tools; its template has a box for it. Untested generated pull requests are closed without discussion. The same rule applies to the maintainer,
whose own work is made with AI assistance (see the README).

## The checks

The automatic checks on GitHub (CI, `.github/workflows/build.yml`) run all of them on every push and pull request. Locally, from the
repository root:

```bash
# once: the pinned web tools (terser through npm, zopfli into tools/.pylib)
cd tools && npm ci --ignore-scripts && cd ..
python -m pip install --require-hashes --only-binary :all: --target tools/.pylib -r tools/requirements.txt

# the logic in firmware/fauxcloud/src, compiled for the host with g++ (Windows: test/host/run.ps1)
sh test/host/run.sh

# the web pages: generated header up to date, the generator's and the tools' tests, the page vectors
python -I tools/gen_web_assets.py --check
python -I -m unittest discover -s test/tools
node test/tools/core_vectors.js        # and every other test/tools/*_vectors.js

# the firmware build (Windows; the workflow shows the same arduino-cli command for Linux)
tools\build.ps1 fauxcloud
```

The toolchain is pinned in `firmware/fauxcloud/sketch.yaml`; setup, flashing and the web-page generator are described in
[firmware/README.md](firmware/README.md). If you change anything in `firmware/fauxcloud/web/`, run the generator
(`python -I tools/gen_web_assets.py`) and commit the regenerated `src/web_assets.h` with your change. Every
user-visible text is pinned word for word in `test/tools/test_ui_text.py`: a changed sentence changes that test too.

### The leak gate

CI also runs `tools/leakgate.py`. It keeps personal and private data out of the repository: home-network addresses
other than the documented ones, MAC addresses, coordinates, user folder paths, secrets, internal reference numbers, and
binary files outside the places that allow them. Run it before you push:

```bash
python -I tools/leakgate.py --manifest tools/publish-manifest.txt --strict-tags
```

A new file must be listed in `tools/publish-manifest.txt`. Test data uses made-up values (the documentation ranges
`192.168.1.x` and `192.168.77.x`, MAC addresses starting with `02`), never your own station.

## Style

- English everywhere: code, comments, commit messages, documentation.
- Plain words in the pages: short sentences, one action per message, no jargon.
- A comment says why, not what happened in the past; history belongs in git.
- Keep the firmware small: the pages have a fixed size budget and the board has about 50 KB of free memory. A change
  that costs memory or flash says how much, measured.

## Credit

Contributors are added to [AUTHORS](AUTHORS) with their first merged change, and release notes name everyone whose
work is in the release. A console report is credited with your GitHub handle in the station profiles.
