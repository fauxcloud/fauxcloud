// test/tools/install_vectors.js -- the browser installer's plain functions (site/install/install.js) in node: every state
// transition and the FAILED paths, the state word, the percentage of a four-part write, the error sentences, the zip
// reader against a factory zip written by tools/release_assets.py itself (Python's zipfile, deflate), parts.json and its
// refusals, the partition table and the settings (NVS) guard, MD5 against node's crypto, the setup Wi-Fi name from the
// MAC, the Wi-Fi QR payload, and the QR encoder against reference matrices of the qrcode package for all eight masks
// (install_qr_fixture.json, test/tools/gen_install_qr.py).
// Usage: node test/tools/install_vectors.js   (python on PATH for the factory-zip case)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');
const zlib = require('zlib');
const { spawnSync } = require('child_process');

const ROOT = path.join(__dirname, '..', '..');
vm.runInThisContext(fs.readFileSync(path.join(ROOT, 'site', 'install', 'install.js'), 'utf8'), { filename: 'install.js' });
/* global STATES, MOVES, nextState, stateWord, percent, errorSentence, zipIndex, zipRead, parseParts, partTable,
   keepsSettings, md5hex, sha256hex, setupSsid, wifiQrText, qrEncode, qrPenalty, qrSvg, ROLES, SAY, fixedPlaces,
   layoutVerdict, rowReady, pickVersion, versionLabel, readFactory, makeInstaller, esc */

let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
function throws(name, fn, words) {
  try { fn(); fail++; console.log('FAIL ' + name + ': no error'); }
  catch (e) { check(name, words.test(e.message), true); }
}
async function rejects(name, p, words) {
  try { await p; fail++; console.log('FAIL ' + name + ': no error'); }
  catch (e) { check(name, words.test(e.message), true); }
}

(async () => {
  // ---- 1. the state machine: every listed move, and nothing else moves
  const EVENTS = ['board', 'unboard', 'connect', 'cancel', 'linked', 'flash', 'done', 'fail', 'lost'];
  const WANT = {
    select: { board: 'usb' },
    usb: { board: 'usb', unboard: 'select', connect: 'connecting' },
    noboard: { board: 'usb', unboard: 'select', connect: 'connecting' },
    failed: { board: 'usb', unboard: 'select', connect: 'connecting' },
    connecting: { cancel: 'noboard', linked: 'connected', fail: 'failed', lost: 'failed' },
    connected: { flash: 'flashing', board: 'usb', unboard: 'select', fail: 'failed', lost: 'failed' },
    flashing: { done: 'done', fail: 'failed', lost: 'failed' },
    done: { board: 'usb', unboard: 'select', connect: 'connecting' }
  };
  check('states', Object.keys(STATES).sort(), Object.keys(WANT).sort());
  for (const s of Object.keys(WANT)) for (const ev of EVENTS) check('move ' + s + ' --' + ev, nextState(s, ev), WANT[s][ev] || s);
  // the happy path and the three ways to FAILED / NO BOARD
  const walk = evs => evs.reduce((s, ev) => nextState(s, ev), 'select');
  check('happy path', walk(['board', 'connect', 'linked', 'flash', 'done']), 'done');
  check('port dialog cancelled', walk(['board', 'connect', 'cancel']), 'noboard');
  check('no answer at connect', walk(['board', 'connect', 'fail']), 'failed');
  check('unplugged mid-flash', walk(['board', 'connect', 'linked', 'flash', 'lost']), 'failed');
  check('write error', walk(['board', 'connect', 'linked', 'flash', 'fail']), 'failed');
  check('again after FAILED', walk(['board', 'connect', 'fail', 'connect', 'linked']), 'connected');
  check('flash cannot start without a link', walk(['board', 'flash']), 'usb');
  check('done ignores a late lost', walk(['board', 'connect', 'linked', 'flash', 'done', 'lost']), 'done');
  check('words', ['select', 'usb', 'connecting', 'connected', 'done', 'noboard', 'failed'].map(s => stateWord(s, 0)),
    ['SELECT BOARD', 'CONNECT USB', 'CONNECTING', 'CONNECTED', 'DONE', 'NO BOARD', 'FAILED']);
  check('flashing word', [stateWord('flashing', 0), stateWord('flashing', 42.7), stateWord('flashing', 100)],
    ['FLASHING 0 %', 'FLASHING 42 %', 'FLASHING 100 %']);
  check('icons in the sprite', Object.values(STATES).every(s =>
    fs.readFileSync(path.join(ROOT, 'site', 'templates', 'sprite.svg'), 'utf8').includes('<symbol id="' + s[1] + '"')), true);

  // ---- 2. the percentage of the whole write
  const sizes = [24992, 3072, 8192, 1751664];
  check('start', percent(sizes, 0, 0, 100), 0);
  check('end', percent(sizes, 3, 900, 900), 100);
  check('bootloader done', percent(sizes, 0, 50, 50), 1);
  check('app half', percent(sizes, 3, 450, 900), Math.floor(100 * (24992 + 3072 + 8192 + 875832) / 1787920));
  check('clamped', [percent(sizes, 3, 2000, 900), percent(sizes, 3, -5, 900), percent(sizes, 9, 1, 1), percent([], 0, 1, 1)],
    [100, 2, 0, 0]);
  let last = 0, mono = true, early100 = false;
  sizes.forEach((sz, i) => { const total = Math.ceil(sz / 3); for (let w = 0; w <= total; w += 1024) {
    const p = percent(sizes, i, Math.min(w, total), total); if (p < last) mono = false; last = p;
    if (p === 100 && !(i === 3 && w >= total)) early100 = true; } });
  check('monotonic', mono, true);
  check('100 only at the very end', early100, false);

  // ---- 3. one plain sentence per error
  check('lost', /disconnected/.test(errorSentence(new Error('The device has been lost.'))), true);
  check('no answer', /BOOT button/.test(errorSentence(new Error('Failed to connect with the device'))), true);
  check('busy port', /Another program/.test(errorSentence(new Error('Failed to execute open: Failed to open serial port.'))), true);
  check('download', /downloaded/.test(errorSentence(new Error('download failed: HTTP 404'))), true);
  check('md5 after the write', errorSentence(new Error('MD5 of file does not match data in flash!')),
    'The write did not verify. Unplug the board, plug it in again and install again.');
  check('sha before the write', /nothing was written/.test(errorSentence(new Error('the zip does not match its SHA-256'))), true);
  check('layout', errorSentence(new Error('layout: differs')),
    'This board has a different flash layout. Tick Erase everything, settings included, and press Connect, then install again.');
  check('incomplete', errorSentence(new Error('incomplete: no sha')), 'The version list is incomplete; nothing was written. Reload the page and try again.');
  check('bundle', /flashing code/.test(errorSentence(new Error('bundle: Failed to fetch dynamically imported module'))), true);
  check('refused', /safety check/.test(errorSentence(new Error('refused: app would overwrite the nvs partition'))), true);
  check('chip / small', [/not an ESP32/.test(errorSentence(new Error('chip: ESP32-S3'))), /4 MB/.test(errorSentence(new Error('small: 2MB')))], [true, true]);
  check('other', /log below/.test(errorSentence(new Error('something else'))), true);

  // ---- 4. the zip reader on a factory zip written by release_assets.py (deflate entries, a folder inside)
  const tmp = fs.mkdtempSync(path.join(require('os').tmpdir(), 'fc-install-'));
  const py = `
import sys, json
sys.path.insert(0, ${JSON.stringify(path.join(ROOT, 'tools'))})
import release_assets as R
files = {"bootloader": b"\\xe9" + bytes(range(256)) * 90, "partitions": open(sys.argv[2], "rb").read(),
         "boot_app0": b"\\xff" * 0x2000, "app": b"\\xe9" + bytes((i * 7) & 255 for i in range(70000))}
ent = R.factory_set("1.0.0", "--flash-mode dio", {"bootloader": 0x1000, "partitions": 0x8000, "boot_app0": 0xE000, "app": 0x10000}, files)[0]
R.write_zip(sys.argv[1], ent)
`;
  const tbl = Buffer.alloc(0xC00, 0xff);
  const rows = [['nvs', 1, 2, 0x9000, 0x5000], ['otadata', 1, 0, 0xE000, 0x2000], ['app0', 0, 0x10, 0x10000, 0x1D0000],
    ['app1', 0, 0x11, 0x1E0000, 0x1D0000], ['coredump', 1, 3, 0x3B0000, 0xA000], ['littlefs', 1, 0x83, 0x3BA000, 0x46000]];
  rows.forEach((r, i) => { const o = i * 32; tbl.writeUInt16LE(0x50AA, o); tbl[o + 2] = r[1]; tbl[o + 3] = r[2];
    tbl.writeUInt32LE(r[3], o + 4); tbl.writeUInt32LE(r[4], o + 8); tbl.fill(0, o + 12, o + 32); tbl.write(r[0], o + 12, 'ascii'); });
  fs.writeFileSync(path.join(tmp, 'table.bin'), tbl);
  const zp = path.join(tmp, 'f.zip');
  const r = spawnSync('python', ['-I', '-c', py, zp, path.join(tmp, 'table.bin')], { encoding: 'utf8' });
  check('python wrote the zip', r.status, 0);
  if (r.status) console.log(r.stderr);
  const zip = new Uint8Array(fs.readFileSync(zp));
  const idx = zipIndex(zip);
  const F = 'fauxcloud-1.0.0-factory/';
  check('zip names', Object.keys(idx).sort(), ['README.txt', 'manifest.json', 'parts.json', 'flash_args', 'fauxcloud-1.0.0-bootloader.bin',
    'fauxcloud-1.0.0-partitions.bin', 'fauxcloud-1.0.0-boot_app0.bin', 'fauxcloud-1.0.0.bin'].map(n => F + n).sort());
  check('deflate entries', Object.values(idx).every(e => e.method === 8), true);
  const pjText = new TextDecoder().decode(await zipRead(zip, idx[F + 'parts.json']));
  const names = Object.keys(idx).map(n => n.slice(F.length));
  const list = parseParts(pjText, names);
  check('parts in flash order', list.parts.map(p => [p.role, p.offset]),
    [['bootloader', 0x1000], ['partitions', 0x8000], ['boot_app0', 0xE000], ['app', 0x10000]]);
  check('version', list.version, '1.0.0');
  for (const p of list.parts) {
    const data = await zipRead(zip, idx[F + p.name]);
    check('size ' + p.role, data.length, p.size);
    check('sha256 ' + p.role, await sha256hex(data), p.sha256);
    check('bytes ' + p.role, crypto.createHash('sha256').update(data).digest('hex'), p.sha256);
  }
  // a stored entry, and the refusals of a broken zip
  const stored = mkzip([['a.txt', Buffer.from('hello'), 0], ['b.bin', Buffer.from('x'.repeat(5000)), 8]]);
  const si = zipIndex(stored);
  check('stored entry', new TextDecoder().decode(await zipRead(stored, si['a.txt'])), 'hello');
  check('deflate entry (node writer)', (await zipRead(stored, si['b.bin'])).length, 5000);
  throws('not a zip', () => zipIndex(new Uint8Array(100)), /not a zip/);
  await rejects('size differs', zipRead(stored, Object.assign({}, si['b.bin'], { size: 4999 })), /size differs/);
  await rejects('method', zipRead(stored, Object.assign({}, si['a.txt'], { method: 12 })), /not supported/);
  await rejects('cut short', zipRead(stored.subarray(0, 60), si['b.bin']), /cut short|broken/);

  // ---- 5. parts.json refusals
  const pj = JSON.parse(pjText);
  const bad = (name, edit, words) => { const j = JSON.parse(pjText); edit(j); throws('parts.json ' + name, () => parseParts(JSON.stringify(j), names), words); };
  throws('parts.json not JSON', () => parseParts('{', names), /not JSON/);
  bad('another project', j => { j.name = 'other'; }, /not a fauxcloud/);
  bad('three parts', j => { j.parts.pop(); }, /four parts/);
  bad('a role twice', j => { j.parts[1].role = 'bootloader'; }, /unexpected part/);
  bad('an unknown role', j => { j.parts[2].role = 'nvs'; }, /unexpected part/);
  bad('misaligned', j => { j.parts[3].offset = 0x10010; }, /bad offset/);
  bad('overlap', j => { j.parts[0].size = 0x8000; }, /overlaps/);
  bad('no sha', j => { delete j.parts[3].sha256; }, /SHA-256/);
  bad('not in the zip', j => { j.parts[3].name = 'merged.bin'; }, /not in the zip/);
  bad('past 4 MB', j => { j.parts[3].size = 0x400000; }, /past 4 MB/);
  check('order of the input does not matter', parseParts(JSON.stringify(Object.assign({}, pj, { parts: pj.parts.slice().reverse() })), names).parts.map(p => p.role), ROLES);

  // ---- 6. the partition table and the settings guard
  const table = partTable(new Uint8Array(tbl));
  check('table', table.map(t => [t.label, t.type, t.sub, t.offset, t.size]), rows);
  const P = (role, offset, size) => ({ role, offset, size });
  const four = [P('bootloader', 0x1000, 24992), P('partitions', 0x8000, 0xC00), P('boot_app0', 0xE000, 0x2000), P('app', 0x10000, 1751664)];
  check('the four parts keep the settings', keepsSettings(four, table), null);
  check('a merged image at 0x0', /overwrite the nvs/.test(keepsSettings([P('app', 0, 0x400000)], table)), true);
  check('a table part into NVS', /overwrite the nvs/.test(keepsSettings([P('partitions', 0x8000, 0x1001)], table)), true);
  check('an app into app1', /overwrite the app1/.test(keepsSettings([P('app', 0x10000, 0x1D0001)], table)), true);
  check('no NVS in the table', /no settings/.test(keepsSettings(four, table.filter(t => t.label !== 'nvs'))), true);
  check('an app straddling otadata and app0', /overwrite the app0/.test(keepsSettings([P('boot_app0', 0xE000, 0x2000), P('app', 0xF000, 0x2000)], table) || ''), true);
  // the roles' fixed places, against the table the zip carries
  check('fixed places', fixedPlaces(four, table), null);
  check('bootloader elsewhere', fixedPlaces([P('bootloader', 0x2000, 1)].concat(four.slice(1)), table), 'the bootloader is not at 0x1000');
  check('table elsewhere', fixedPlaces([four[0], P('partitions', 0x9000, 1), four[2], four[3]], table), 'the partition table is not at 0x8000');
  check('boot_app0 off otadata', fixedPlaces([four[0], four[1], P('boot_app0', 0xF000, 1), four[3]], table), 'boot_app0 is not on the otadata partition');
  check('app on no app partition', fixedPlaces([four[0], four[1], four[2], P('app', 0x3BA000, 1)], table), 'the app is not on an app partition');
  check('app on app1 is a place', fixedPlaces([four[0], four[1], four[2], P('app', 0x1E0000, 1)], table), null);
  // the board's own table against the release's
  const blank = new Uint8Array(0xC00).fill(0xff);
  const other = Buffer.from(tbl); other.writeUInt32LE(0x6000, 8);  // the nvs size changed
  check('layout same', layoutVerdict(new Uint8Array(tbl), table, false), null);
  check('layout blank board', layoutVerdict(blank, table, false), null);
  check('layout unreadable board', layoutVerdict(null, table, false), null);
  check('layout differs', layoutVerdict(new Uint8Array(other), table, false), 'layout');
  check('layout differs, erase ticked', layoutVerdict(new Uint8Array(other), table, true), null);
  // the version list: a row is installable only with a url and its SHA-256 (fail closed); the pick and the label
  const S = 'a'.repeat(64);
  check('row ready', [rowReady({ url: '/x', sha256: S }), rowReady({ url: '/x', sha256: null }), rowReady({ url: '/x' }),
    rowReady({ url: null, sha256: S }), rowReady({ url: '/x', sha256: 'A'.repeat(64) }), rowReady(null)], [true, false, false, false, false, false]);
  const V = (version, o) => Object.assign({ version, url: '/f/' + version, sha256: S, date: '2027-01-01', prerelease: false }, o || {});
  check('pick: newest non-pre-release', pickVersion([V('1.1.0-rc.1', { prerelease: true }), V('1.0.1'), V('1.0.0')]), 1);
  check('pick: a pre-release when nothing else', pickVersion([V('1.1.0-rc.1', { prerelease: true }), V('1.0.0', { sha256: null })]), 0);
  check('pick: a row without sha is never picked', pickVersion([V('1.0.1', { sha256: null }), V('1.0.0')]), 1);
  check('pick: the placeholder', pickVersion([{ version: 'from the first release', url: null, sha256: null }]), 0);
  check('labels', [versionLabel(V('1.0.0')), versionLabel(V('1.1.0-rc.1', { prerelease: true })),
    versionLabel({ version: '0.1.0-b34', url: '/l', sha256: S, date: null, local: true }),
    versionLabel({ version: 'from the first release', url: null, sha256: null, date: null }), versionLabel(V('1.0.2', { sha256: null }))],
    ['1.0.0 · 2027-01-01', '1.1.0-rc.1 · 2027-01-01 · pre-release', '0.1.0-b34 · built on this computer',
      'from the first release · not available yet', '1.0.2 · 2027-01-01 · not available yet']);
  check('esc', esc('<b a="1">&'), '&#60;b a=&#34;1&#34;&#62;&#38;');

  // ---- 7. MD5
  for (const n of [0, 1, 3, 55, 56, 57, 63, 64, 65, 1000, 100003]) {
    const d = crypto.randomBytes(n);
    check('md5 ' + n, md5hex(new Uint8Array(d)), crypto.createHash('md5').update(d).digest('hex'));
  }
  check('md5 abc', md5hex(new TextEncoder().encode('abc')), '900150983cd24fb0d6963f7d28e17f72');

  // ---- 8. the setup Wi-Fi and the payload
  // synthetic, locally administered addresses, joined from parts (no real-looking MAC on one line of this file)
  const mac = (...p) => p.join(':');
  check('ssid', setupSsid(mac('02', '00', '00', '3f', '0a', '9b')), 'fauxcloud-setup-0A9B');
  check('ssid upper input', setupSsid(mac('02', '00', '00', '3F', 'FF', '01')), 'fauxcloud-setup-FF01');
  check('no mac', [setupSsid(''), setupSsid(null), setupSsid(mac('02', '00', '00', '3f', '0a')), setupSsid(mac('zz', '00', '00', '3f', '0a', '9b'))],
    [null, null, null, null]);
  check('payload', wifiQrText('fauxcloud-setup-0A9B', 'fauxcloud'), 'WIFI:T:WPA;S:fauxcloud-setup-0A9B;P:fauxcloud;;');
  check('payload escapes', wifiQrText('a;b,c:d"e\\f', 'p;w'), 'WIFI:T:WPA;S:a\\;b\\,c\\:d\\"e\\\\f;P:p\\;w;;');
  // the firmware's own derivation, read from the source: the last two MAC bytes, upper-case hex, after the prefix
  const fw = fs.readFileSync(path.join(ROOT, 'firmware', 'fauxcloud', 'src', 'settings.cpp'), 'utf8');
  check('firmware mac4 is the last two bytes, upper hex', /snprintf\(out, 5, "%02X%02X", mac\[4\], mac\[5\]\)/.test(fw), true);
  check('firmware setup prefix', /"fauxcloud-setup-%s", gMac4/.test(fs.readFileSync(path.join(ROOT, 'firmware', 'fauxcloud', 'app_net.cpp'), 'utf8')), true);
  check('firmware setup password', /SETUP_AP_PASS = "fauxcloud"/.test(fs.readFileSync(path.join(ROOT, 'firmware', 'fauxcloud', 'src', 'settings.h'), 'utf8')), true);
  check('firmware MAC is the station one', /esp_read_mac\(gMac, ESP_MAC_WIFI_STA\)/.test(fs.readFileSync(path.join(ROOT, 'firmware', 'fauxcloud', 'fauxcloud.ino'), 'utf8')), true);

  // ---- 9. the QR encoder against the reference, every mask
  const ref = JSON.parse(fs.readFileSync(path.join(__dirname, 'install_qr_fixture.json'), 'utf8')).codes;
  for (const c of ref) {
    const pens = [];
    for (let k = 0; k < 8; k++) {
      const q = qrEncode(c.text, k);
      check('qr ' + c.text.slice(0, 24) + ' v' + c.version + ' mask ' + k, q.rows.map(r => r.join('')), c.masks[k]);
      check('qr version ' + c.version, q.version, c.version);
      pens.push(qrPenalty(q.rows));
    }
    const auto = qrEncode(c.text);
    check('qr auto mask is the lowest penalty', auto.penalty, Math.min.apply(null, pens));
    check('qr auto is a reference matrix', auto.rows.map(r => r.join('')), c.masks[auto.mask]);
  }
  throws('qr too long', () => qrEncode('x'.repeat(300)), /too long/);
  const q = qrEncode(wifiQrText('fauxcloud-setup-0A9B', 'fauxcloud'));
  const svg = qrSvg(q, 'QR code: join the Wi-Fi fauxcloud-setup-0A9B');
  check('svg modules', (svg.match(/h1v1h-1z/g) || []).length, q.rows.reduce((a, r) => a + r.reduce((x, y) => x + y, 0), 0));
  check('svg quiet zone', svg.includes('viewBox="-4 -4 ' + (q.size + 8) + ' ' + (q.size + 8) + '"'), true);

  // ---- 10. the factory zip as the installer reads it, and the sequences with a fake library, port and page
  const zipSha = crypto.createHash('sha256').update(zip).digest('hex');
  const fac = await readFactory(zip, '1.0.0', () => {});
  check('readFactory', fac.files.map(f => f.address), [0x1000, 0x8000, 0xE000, 0x10000]);
  await rejects('readFactory: another version', readFactory(zip, '1.0.1', () => {}), /^refused: the zip is fauxcloud 1.0.0/);
  // a broken central directory and a corrupt deflate stream
  const cut = zip.slice(); const eocd = Buffer.from(cut).lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  new DataView(cut.buffer).setUint32(eocd + 16, cut.length - 30, true);
  throws('broken directory', () => zipIndex(cut), /broken zip directory/);
  const broken = zip.slice(); const e0 = zipIndex(zip)[F + 'fauxcloud-1.0.0.bin'];
  const st0 = e0.at + 30 + new DataView(broken.buffer).getUint16(e0.at + 26, true);
  broken.fill(0xff, st0, st0 + 40);
  await rejects('corrupt deflate', zipRead(broken, e0), /(?:)/);

  const row = { version: '1.0.0', url: '/firmware/1.0.0/f.zip', sha256: zipSha, date: null, prerelease: false };
  const tick = () => new Promise(r => setImmediate(r));
  function rig(o) {
    o = o || {};
    const R = { states: [], said: [], aux: [], buttons: [], locks: [], logs: [], writes: [], closed: [], sig: [], unticked: 0, erase: !!o.erase, row: o.row || row };
    let tid = 0;
    class Transport {
      constructor(p, tracing) { this.id = ++tid; this.port = p; R.tracing = tracing; }
      async disconnect() { R.closed.push(this.id); }
      async setDTR(v) { R.sig.push('DTR' + (+v)); }
      async setRTS(v) { if (o.afterFails) throw new Error('no RTS'); R.sig.push('RTS' + (+v)); if (!v && R.sig.includes('RTS1')) R.reset = true; }
    }
    class ESPLoader {
      constructor(a) { this.t = a.transport; this.chip = { CHIP_NAME: o.chip || 'ESP32', readMac: async () => '02:00:00:3f:0a:9b' }; R.baud = a.baudrate; }
      async main() { if (o.mainFails) throw new Error(o.mainFails); return o.chip ? o.chip + '-X' : 'ESP32-D0WD-V3 (revision v3.1)'; }
      async detectFlashSize() { return o.size || '4MB'; }
      async readFlash(addr, size) { R.read = [addr, size]; if (o.readFails) throw new Error('read failed'); return o.board || new Uint8Array(tbl); }
      async writeFlash(opt) {
        R.writes.push({ files: opt.fileArray.map(f => [f.address, f.data.length]), eraseAll: opt.eraseAll, md5: typeof opt.calculateMD5Hash,
          md5app: opt.calculateMD5Hash(opt.fileArray[3].data) });
        opt.reportProgress(0, 0, 10); opt.reportProgress(3, 10, 10);
        if (o.hold && R.writes.length === 1) await o.hold;
        if (o.writeFails && (!o.firstOnly || R.writes.length === 1)) throw new Error(o.writeFails);
      }
      async after() { R.libReset = true; }  // esptool-js 0.7.1's reset never pulls EN low: never called
    }
    const ports = o.ports || [{ n: 1 }];
    let pi = 0;
    const inst = makeInstaller({
      ui: {
        show: (s, a) => { R.states.push(s); R.said.push(a); R.aux.push(a); }, aux: a => R.aux.push(a), progress: (p, t) => { R.pct = p; if (t) R.aux.push(t); },
        button: (l, on) => R.buttons.push([l, on]), lock: ph => R.locks.push(ph), log: t => R.logs.push(t),
        untick: () => { R.unticked++; R.erase = false; }
      },
      version: () => R.row, erase: () => R.erase,
      requestPort: async () => {
        if (o.cancel) throw new Error('No port selected by the user.');
        if (o.choose) return o.choose(pi++);
        return ports[Math.min(pi++, ports.length - 1)];
      },
      loadLib: async () => { if (o.noBundle) throw new Error('Failed to fetch dynamically imported module'); return { Transport, ESPLoader }; },
      fetch: async () => ({ ok: true, arrayBuffer: async () => zip.buffer.slice(zip.byteOffset, zip.byteOffset + zip.byteLength) }),
      terminal: {}
    });
    R.inst = inst;
    R.last = () => R.states[R.states.length - 1];
    R.lastAux = () => R.aux[R.aux.length - 1];
    R.btn = () => R.buttons[R.buttons.length - 1];
    return R;
  }

  let R = rig({ cancel: true });
  R.inst.board(true);
  check('flow: board -> CONNECT USB', [R.last(), R.btn()], ['usb', ['Connect', true]]);
  await R.inst.press();
  check('flow: cancelled port -> NO BOARD', [R.last(), R.btn(), /no port chosen/.test(R.logs.join())], ['noboard', ['Connect', true], true]);

  R = rig();
  R.inst.board(true);
  await R.inst.press();
  check('flow: connected', [R.last(), R.btn(), R.tracing, R.baud, R.locks[R.locks.length - 1]], ['connected', ['Install', true], false, 460800, 'linked']);
  await R.inst.press();
  check('flow: done', [R.last(), R.reset, R.pct, R.writes.length], ['done', true, 100, 1]);
  check('flow: the reset pulls EN low and lets it go, IO0 high', [R.sig, !!R.libReset], [['DTR0', 'RTS1', 'RTS0'], false]);
  check('flow: the erase box is unticked after DONE', R.unticked, 1);
  check('flow: four parts at their offsets, no erase, MD5 hook', [R.writes[0].files.map(f => f[0]), R.writes[0].eraseAll, R.writes[0].md5],
    [[0x1000, 0x8000, 0xE000, 0x10000], false, 'function']);
  check('flow: the board table was read first', R.read, [0x8000, 0xC00]);
  check('flow: the transport closed after', R.closed, [1]);
  check('flow: done says the settings are kept', /settings, if it had any, are kept/.test(R.said[R.states.lastIndexOf('done')]), true);

  R = rig({ writeFails: 'MD5 of file does not match data in flash!' });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: MD5 after the write', [R.last(), R.lastAux()], ['failed', 'The write did not verify. Unplug the board, plug it in again and install again.']);
  check('flow: the erase box is unticked after FAILED too', R.unticked, 1);

  const other2 = Buffer.from(tbl); other2.writeUInt32LE(0x6000, 8);
  R = rig({ board: new Uint8Array(other2) });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: another layout is refused, nothing written', [R.last(), R.lastAux(), R.writes.length],
    ['failed', 'This board has a different flash layout. Tick Erase everything, settings included, and press Connect, then install again.', 0]);
  R = rig({ board: new Uint8Array(other2), erase: true });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: another layout with the erase box', [R.last(), R.writes[0].eraseAll, /freshly erased/.test(R.said[R.states.lastIndexOf('done')])], ['done', true, true]);
  check('flow: after an erase install the box is unticked, the next one keeps settings', [R.unticked, R.erase], [1, false]);
  R = rig({ board: blank });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: a blank board goes on', R.last(), 'done');
  R = rig({ readFails: true });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: an unreadable table goes on', [R.last(), /taken as a new board/.test(R.logs.join())], ['done', true]);

  R = rig({ row: Object.assign({}, row, { sha256: null }) });
  R.inst.board(true);
  check('flow: a row without sha cannot connect', R.btn(), ['Connect', false]);
  R.row = row; await R.inst.press(); R.row = Object.assign({}, row, { sha256: null }); R.inst.version();
  check('flow: the version changed to one without sha', R.btn(), ['Install', false]);
  await R.inst.install();
  check('flow: a row without sha is refused, nothing written', [R.last(), R.lastAux(), R.writes.length],
    ['failed', 'The version list is incomplete; nothing was written. Reload the page and try again.', 0]);

  R = rig({ row: Object.assign({}, row, { version: '1.0.1' }) });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: zip and list disagree on the version', [R.last(), /safety check/.test(R.lastAux()), R.writes.length], ['failed', true, 0]);

  R = rig({ afterFails: true });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('flow: a failed reset is still DONE, with the button sentence', [R.last(), /EN \(RESET\) button/.test(R.said[R.states.lastIndexOf('done')])], ['done', true]);

  R = rig({ noBundle: true });
  R.inst.board(true); await R.inst.press();
  check('flow: the bundle did not load', [R.last(), /flashing code/.test(R.lastAux())], ['failed', true]);
  R = rig({ chip: 'ESP32-S3' });
  R.inst.board(true); await R.inst.press();
  check('flow: wrong chip', [R.last(), /not an ESP32/.test(R.lastAux()), R.closed.length], ['failed', true, 1]);
  R = rig({ size: '2MB' });
  R.inst.board(true); await R.inst.press();
  check('flow: 2 MB flash', [R.last(), /4 MB/.test(R.lastAux())], ['failed', true]);
  R = rig({ mainFails: 'Failed to connect with the device' });
  R.inst.board(true); await R.inst.press();
  check('flow: no answer', [R.last(), /BOOT button/.test(R.lastAux())], ['failed', true]);

  // unplug mid-flash, Connect again at once, then the old write fails late: it must not touch the new link
  let release;
  const hold = new Promise(r => { release = r; });
  const p1 = { n: 1 }, p2 = { n: 2 };
  R = rig({ hold, writeFails: 'The device has been lost.', firstOnly: true, ports: [p1, p2] });
  R.inst.board(true); await R.inst.press();
  const job = R.inst.press();
  // wait by the clock, not by a tick count: on a slow runner the checks before the first write take longer
  for (const t0 = Date.now(); !R.writes.length && Date.now() - t0 < 20000;) await tick();
  check('race: flashing, the write under way', [R.last(), R.writes.length], ['flashing', 1]);
  R.inst.lost({ n: 9 });
  check('race: another port going away is ignored', R.last(), 'flashing');
  R.inst.lost(p1);
  check('race: unplugged -> FAILED, Connect on', [R.last(), R.btn(), R.closed], ['failed', ['Connect', true], [1]]);
  await R.inst.press();
  check('race: reconnected', [R.last(), R.btn()], ['connected', ['Install', true]]);
  const before = R.states.length;
  release(); await job; await tick();
  check('race: the old job left the page alone', [R.states.length, R.last(), R.btn()], [before, 'connected', ['Install', true]]);
  check('race: the new transport stays open', R.closed, [1]);
  await R.inst.press();
  check('race: the new link installs', [R.last(), R.closed], ['done', [1, 2]]);
  // a second Connect while connecting: the button is off, and a stale connect closes only its own transport
  R = rig({ ports: [p1, p2] });
  R.inst.board(true);
  const c1 = R.inst.connect();
  check('double connect: the button is off while connecting', R.btn(), ['Connect', false]);
  const c2 = R.inst.connect(); await c1; await c2;
  check('double connect: one link; the stale job opened nothing', [R.last(), R.closed], ['connected', []]);
  // after DONE, Connect another board; the first board is unplugged while the chooser is open: nothing changes
  let pickB;
  const pA = { n: 'A' }, pB = { n: 'B' };
  R = rig({ choose: i => i === 0 ? Promise.resolve(pA) : new Promise(r => { pickB = r; }) });
  R.inst.board(true); await R.inst.press(); await R.inst.press();
  check('next board: the first one done', R.last(), 'done');
  const next = R.inst.press();
  await tick();
  check('next board: the chooser open', R.last(), 'connecting');
  R.inst.lost(pA);
  check('next board: the first board unplugged changes nothing', R.last(), 'connecting');
  pickB(pB); await next;
  check('next board: the picked port links', [R.last(), R.btn()], ['connected', ['Install', true]]);
  // install() outside CONNECTED leaves a running connect alone
  R = rig({ choose: () => new Promise(r => { pickB = r; }) });
  R.inst.board(true);
  const c3 = R.inst.connect(); await tick();
  await R.inst.install();
  check('install outside CONNECTED: no state change', R.last(), 'connecting');
  pickB(pB); await c3;
  check('install outside CONNECTED: the connect still links', R.last(), 'connected');
  // the board changed while connected
  R.inst.board(false);
  check('board cleared while connected: its link closed', [R.last(), R.closed, R.btn()], ['select', [1], ['Connect', false]]);

  fs.rmSync(tmp, { recursive: true, force: true });
  console.log('install_vectors: ' + pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})().catch(e => { console.log('FAIL (exception) ' + (e && e.stack || e)); process.exit(1); });

// a minimal zip writer for the stored-entry case (method 0 or 8), central directory and end record
function mkzip(entries) {
  const locals = [], central = [];
  let off = 0;
  for (const [name, data, method] of entries) {
    const body = method === 8 ? zlib.deflateRawSync(data) : data, nb = Buffer.from(name);
    const crc = zlib.crc32 ? zlib.crc32(data) : 0;
    const lh = Buffer.alloc(30); lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(method, 8);
    lh.writeUInt32LE(crc >>> 0, 14); lh.writeUInt32LE(body.length, 18); lh.writeUInt32LE(data.length, 22); lh.writeUInt16LE(nb.length, 26);
    const ch = Buffer.alloc(46); ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(method, 10);
    ch.writeUInt32LE(crc >>> 0, 16); ch.writeUInt32LE(body.length, 20); ch.writeUInt32LE(data.length, 24); ch.writeUInt16LE(nb.length, 28);
    ch.writeUInt32LE(off, 42);
    locals.push(lh, nb, body); central.push(ch, nb);
    off += 30 + nb.length + body.length;
  }
  const cd = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(off, 16);
  return new Uint8Array(Buffer.concat(locals.concat([cd, end])));
}
