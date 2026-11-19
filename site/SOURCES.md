# site/ — vendored third-party files

Every third-party file the site serves, pinned: version, where it came from, its SHA-256, and how it was taken. A test
(`test/tools/test_build_site.py`) recomputes the SHA-256 of each file below and fails when it differs from this table.
The licence texts are in `site/THIRD_PARTY_NOTICES_SITE.txt`, served on every page's footer link as
`/third-party-notices.txt` and next to the installer as `/install/third-party-notices.txt`; `REUSE.toml` names the licences.

| File | Component | Version | Licence | SHA-256 |
|---|---|---|---|---|
| `site/assets/esptool-bundle.js` | esptool-js (Espressif Systems), with pako 2.1.0 and atob-lite 2.0.0 inside | 0.7.1 | Apache-2.0; pako MIT AND Zlib; atob-lite MIT | `d6a4b0b7c9b747a7370cfa976164a962f56332d5faa7c3b45cdb95a2a8b1d456` |

## esptool-js

- **Upstream:** https://github.com/espressif/esptool-js (release 0.7.1, published to npm 2026-11-02).
- **Taken from:** the npm package `esptool-js@0.7.1`, https://registry.npmjs.org/esptool-js/-/esptool-js-0.7.1.tgz
  (tarball SHA-256 `2fec38259eaeb65aacfac87806625c2da6d2296da59c6dfaee7debba9f63f6b9`, npm integrity
  `sha512-vw1isT5ROHgpRmMG3y6B81dceeZ7O1WhF1x05qFbAlH8N0N/pOQqODzJLA+36lCbOXCw06iA4CWctyrQQ7VeSA==`). The file is the
  package's own `bundle.js`, copied unmodified: upstream builds it with `npm run build` (`tsc && rollup --config`), an
  ES module that carries pako and atob-lite.
- **How:**

  ```
  npm pack esptool-js@0.7.1
  tar -xzf esptool-js-0.7.1.tgz package/bundle.js
  cp package/bundle.js site/assets/esptool-bundle.js
  sha256sum site/assets/esptool-bundle.js     # must equal the table above
  ```
- **Used by:** `site/install/install.js` only, which imports it (`import('/assets/esptool-bundle.js')`) when Connect is
  pressed on `/install/`; no other page loads it. The installer uses `ESPLoader` and `Transport`: `main()`,
  `detectFlashSize()`, `chip.readMac()`, `writeFlash()` (four parts at their offsets, `eraseAll` only when the visitor
  ticks the box, the MD5 check after each part) and `after('hard_reset')`.
- **Updating:** take a release at least two weeks old, repeat the steps, update the version and both SHA-256 lines here
  and in `THIRD_PARTY_NOTICES_SITE.txt`, then rehearse a full install on a spare board before it ships.
