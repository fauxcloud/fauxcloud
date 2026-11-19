<!-- Thank you. Please read CONTRIBUTING.md first: code changes need an issue labelled `accepted`. -->

Fixes #<!-- the accepted issue -->

## What changes

<!-- One or two sentences: what it does and why. -->

## Tested

- Board: <!-- e.g. ESP32-WROOM-32 DevKit, 4 MB -->
- Console and its firmware: <!-- e.g. Vevor 7-in-1 YT60234, 1.0.1; or "not touched by this change" -->
- What you did on the hardware and what you saw:

- [ ] Host tests (`sh test/host/run.sh` or `test/host/run.ps1`)
- [ ] `python -I -m unittest discover -s test/tools` and the `node test/tools/*_vectors.js` that cover the change
- [ ] `python -I tools/gen_web_assets.py --check` (the regenerated `src/web_assets.h` is committed if a page changed)
- [ ] The firmware builds (`tools\build.ps1 fauxcloud` or CI); a change in memory or flash use is measured above
- [ ] A test that fails without this change is added (for a fix)

## Before you send it

- [ ] Every commit is signed off (`git commit -s`, Developer Certificate of Origin)
- [ ] The leak gate passes (`python -I tools/leakgate.py --manifest tools/publish-manifest.txt --strict-tags`); new files are listed in `tools/publish-manifest.txt`
- [ ] No passwords, keys, real station IDs, home-network addresses, MAC addresses or locations in code, tests or text
- [ ] AI assistance: [ ] none, or [ ] used, and I understand every line and tested it on hardware
