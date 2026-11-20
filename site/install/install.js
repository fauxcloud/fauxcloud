// install.js -- the browser installer of fauxcloud.org/install/ (Chrome or Edge on a computer: Web Serial). It reads the
// board and version list (/firmware/versions.json), fetches the chosen release's factory zip, reads parts.json inside it
// and writes the four parts at their offsets with esptool-js (/assets/esptool-bundle.js, vendored, loaded only here and
// only once Connect is pressed), WITHOUT erasing: fauxcloud keeps its settings in NVS between the partition table and
// the app. A full erase is the unticked checkbox. Before writing: the zip's size and SHA-256 from the list, each part's
// size and SHA-256 from parts.json, the parts' fixed places and the settings guard on the zip's partition table, and the
// board's own table at 0x8000 (a readable one that differs needs the erase box). After each part, the flash's MD5.
// No cookies, no storage, no request beyond the site's own files.
// The top part is plain functions; makeInstaller() is the connect / install sequencing with the library, the port and
// the page injected; test/tools/install_vectors.js runs both in node. boot() wires the page.

// ---------------------------------------------------------------- the state machine
// state: [the word, its icon in the sprite, its tone]
var STATES = {
  select: ['SELECT BOARD', 'i-chip', 'idle'],
  usb: ['CONNECT USB', 'i-usb', 'idle'],
  connecting: ['CONNECTING', 'i-usb', 'busy'],
  connected: ['CONNECTED', 'i-check', 'ok'],
  flashing: ['FLASHING', 'i-zap', 'busy'],
  done: ['DONE', 'i-check', 'ok'],
  noboard: ['NO BOARD', 'i-usb', 'idle'],
  failed: ['FAILED', 'i-alert', 'bad']
};
// state -> {event: next state}; an event a state does not list leaves it as it is (DONE: only the one button, which
// brings the form back for the next board)
var MOVES = {
  select: { board: 'usb' },
  usb: { board: 'usb', unboard: 'select', connect: 'connecting' },
  noboard: { board: 'usb', unboard: 'select', connect: 'connecting' },
  failed: { board: 'usb', unboard: 'select', connect: 'connecting' },
  connecting: { cancel: 'noboard', linked: 'connected', fail: 'failed', lost: 'failed' },
  connected: { flash: 'flashing', board: 'usb', unboard: 'select', fail: 'failed', lost: 'failed' },
  flashing: { done: 'done', fail: 'failed', lost: 'failed' },
  done: { board: 'usb' }
};
function nextState(state, ev) { var m = MOVES[state]; return (m && m[ev]) || state; }
function stateWord(state, pct) { return state === 'flashing' ? 'FLASHING ' + (pct | 0) + ' %' : STATES[state][0]; }
// what the card shows in a state: the post-flash block ("Next: set it up") and the QR only after a flash (DONE), where
// the form shrinks to its one button (Board, Version and the erase box come back when it is pressed) and the button
// turns quiet beside the block's own; the bar only while flashing; the driver help (folded) lit and opened after NO BOARD
// and FAILED, unless the failure is not the board's (opts.noHelp: the version list did not load)
function view(state, opts) {
  var done = state === 'done';
  return { next: done, qr: done && !!(opts && opts.qr), opts: !done, quiet: done, bar: state === 'flashing',
    help: (state === 'noboard' || state === 'failed') && !(opts && opts.noHelp) };
}

// the whole write as one percentage: part i is `written` of `total` (its own unit, compressed bytes), each part weighs
// its size; 100 only when the last part is through
function percent(sizes, i, written, total) {
  var all = 0, before = 0;
  for (var k = 0; k < sizes.length; k++) { all += sizes[k]; if (k < i) before += sizes[k]; }
  if (!all || i < 0 || i >= sizes.length) return 0;
  var f = total > 0 ? Math.min(1, Math.max(0, written / total)) : 0;
  return Math.min(100, Math.floor(100 * (before + sizes[i] * f) / all));
}

// the sentences the page says for the installer's own refusals (their errors carry the key as a prefix)
var SAY = {
  layout: 'This board has a different flash layout. Tick Erase everything, settings included, and press Connect, then install again.',
  verify: 'The write did not verify. Unplug the board, plug it in again and install again.',
  incomplete: 'The version list is incomplete; nothing was written. Reload the page and try again.',
  refused: 'This release failed a safety check, so nothing was written. Please report it, and install by hand meanwhile.',
  bundle: 'The installer could not load its flashing code. Check the connection, reload the page and press Connect again.',
  chip: 'This board is not an ESP32 (WROOM-32). Pick the right board or use another one.',
  small: 'This board has less than 4 MB of flash; fauxcloud needs 4 MB.'
};
function fail(key, detail) { return new Error(key + ': ' + detail); }
// one plain sentence for an error (the log keeps the library's own words)
function errorSentence(e) {
  var m = String((e && (e.message || e.name)) || e || ''), k = /^([a-z]+): /.exec(m);
  if (k && SAY[k[1]]) return SAY[k[1]];
  if (/MD5/.test(m)) return SAY.verify;
  if (/lost|disconnect|NetworkError|device has been|port is closed|readable/i.test(m))
    return 'The board was disconnected. Unplug it, plug it in again and press Connect.';
  if (/Failed to connect|timed? ?out|No serial data|wrong boot mode|sync/i.test(m))
    return 'The board did not answer. Hold its BOOT button while you press Connect, or try another USB cable.';
  if (/already open|Failed to open|access denied|in use/i.test(m))
    return 'Another program is using the port. Close it (a serial monitor, another tab), then press Connect.';
  if (/download|fetch|HTTP/i.test(m)) return 'The firmware file could not be downloaded. Check the connection and try again.';
  if (/SHA-256|parts\.json|zip/i.test(m)) return 'The firmware file did not check out, so nothing was written. Reload the page and try again.';
  return 'That did not finish. Unplug the board, plug it in again and press Connect. The log below says what went wrong.';
}

// ---------------------------------------------------------------- the zip (stored or deflate, the factory zip's own form)
function zipIndex(b) {
  var dv = new DataView(b.buffer, b.byteOffset, b.byteLength), e = -1, i;
  for (i = b.length - 22; i >= 0 && i >= b.length - 22 - 65535; i--) if (dv.getUint32(i, true) === 0x06054b50) { e = i; break; }
  if (e < 0) throw new Error('not a zip file');
  var n = dv.getUint16(e + 10, true), p = dv.getUint32(e + 16, true), out = {};
  for (i = 0; i < n; i++) {
    if (p + 46 > b.length || dv.getUint32(p, true) !== 0x02014b50) throw new Error('broken zip directory');
    var nl = dv.getUint16(p + 28, true);
    out[new TextDecoder().decode(b.subarray(p + 46, p + 46 + nl))] = {
      method: dv.getUint16(p + 10, true), csize: dv.getUint32(p + 20, true), size: dv.getUint32(p + 24, true),
      at: dv.getUint32(p + 42, true) };
    p += 46 + nl + dv.getUint16(p + 30, true) + dv.getUint16(p + 32, true);
  }
  return out;
}
async function zipRead(b, ent) {
  var dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (ent.at + 30 > b.length || dv.getUint32(ent.at, true) !== 0x04034b50) throw new Error('broken zip entry');
  var start = ent.at + 30 + dv.getUint16(ent.at + 26, true) + dv.getUint16(ent.at + 28, true);
  var raw = b.subarray(start, start + ent.csize), out;
  if (raw.length !== ent.csize) throw new Error('zip entry cut short');
  if (ent.method === 0) out = raw.slice();
  else if (ent.method === 8) out = new Uint8Array(await new Response(new Blob([raw]).stream()
    .pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer());
  else throw new Error('zip method ' + ent.method + ' is not supported');
  if (out.length !== ent.size) throw new Error('zip entry size differs');
  return out;
}

// ---------------------------------------------------------------- parts.json and the settings guard
var ROLES = ['bootloader', 'partitions', 'boot_app0', 'app'];
var FLASH_4MB = 0x400000;
// parts.json text + the zip's entry names (relative to the folder of parts.json) -> {version, parts in flash order}
function parseParts(text, names) {
  var j;
  try { j = JSON.parse(text); } catch (e) { throw new Error('parts.json is not JSON'); }
  if (!j || j.name !== 'fauxcloud' || !Array.isArray(j.parts)) throw new Error('parts.json is not a fauxcloud parts list');
  if (j.parts.length !== ROLES.length) throw new Error('parts.json must list the four parts');
  var parts = j.parts.slice().sort(function (a, b) { return a.offset - b.offset; }), seen = {}, end = 0;
  parts.forEach(function (p) {
    if (ROLES.indexOf(p.role) < 0 || seen[p.role]) throw new Error('parts.json: unexpected part ' + p.role);
    seen[p.role] = 1;
    if (!Number.isInteger(p.offset) || !Number.isInteger(p.size) || p.size <= 0 || p.offset % 0x1000)
      throw new Error('parts.json: bad offset or size for ' + p.role);
    if (p.offset < end) throw new Error('parts.json: ' + p.role + ' overlaps the part before it');
    end = p.offset + p.size;
    if (!/^[0-9a-f]{64}$/.test(p.sha256 || '')) throw new Error('parts.json: no SHA-256 for ' + p.role);
    if (names.indexOf(p.name) < 0) throw new Error('parts.json names ' + p.name + ', which is not in the zip');
  });
  if (end > FLASH_4MB) throw new Error('parts.json: a part ends past 4 MB');
  return { version: j.version, parts: parts };
}
// the partition table (32-byte entries, magic 0x50AA) -> [{label, type, sub, offset, size}]
function partTable(b) {
  var dv = new DataView(b.buffer, b.byteOffset, b.byteLength), out = [];
  for (var i = 0; i + 32 <= b.length && dv.getUint16(i, true) === 0x50AA; i += 32) {
    var lab = b.subarray(i + 12, i + 28), z = lab.indexOf(0);
    out.push({ label: new TextDecoder().decode(z < 0 ? lab : lab.subarray(0, z)), type: b[i + 2], sub: b[i + 3],
      offset: dv.getUint32(i + 4, true), size: dv.getUint32(i + 8, true) });
  }
  return out;
}
// null when every part sits at its fixed place: the bootloader at 0x1000, the table at 0x8000, boot_app0 on the otadata
// partition and the app on an app partition of the table it brings; else the reason
function fixedPlaces(parts, table) {
  var at = {};
  parts.forEach(function (p) { at[p.role] = p.offset; });
  var ota = table.filter(function (t) { return t.type === 1 && t.sub === 0; })[0];
  if (at.bootloader !== 0x1000) return 'the bootloader is not at 0x1000';
  if (at.partitions !== 0x8000) return 'the partition table is not at 0x8000';
  if (!ota || at.boot_app0 !== ota.offset) return 'boot_app0 is not on the otadata partition';
  if (!table.some(function (t) { return t.type === 0 && t.offset === at.app; })) return 'the app is not on an app partition';
  return null;
}
// null when no part writes into a kept partition of the table it brings (NVS above all), else the reason
function keepsSettings(parts, table) {
  var nvs = table.filter(function (t) { return t.type === 1 && t.sub === 2; });
  if (!nvs.length) return 'the partition table has no settings (NVS) partition';
  var app0 = parts.filter(function (p) { return p.role === 'app'; })[0], keep = table.filter(function (t) {
    return !(t.type === 1 && t.sub === 0) && !(t.type === 0 && app0 && t.offset === app0.offset);
  });
  for (var i = 0; i < parts.length; i++)
    for (var k = 0; k < keep.length; k++)
      if (parts[i].offset < keep[k].offset + keep[k].size && keep[k].offset < parts[i].offset + parts[i].size)
        return parts[i].role + ' would overwrite the ' + keep[k].label + ' partition';
  return null;
}

// the board's own table (read from 0x8000 before writing) against the zip's: null = go on; a blank or unreadable table
// (a new board, null here) goes on; a readable one that differs needs the erase box ticked
function layoutVerdict(boardBytes, zipTable, erase) {
  if (erase || !boardBytes) return null;
  var have = partTable(boardBytes);
  if (!have.length) return null;
  var key = function (t) { return [t.label, t.type, t.sub, t.offset, t.size].join(','); };
  return have.map(key).join(';') === zipTable.map(key).join(';') ? null : 'layout';
}
// a versions.json row the installer may write: a url and its SHA-256 (fail closed)
function rowReady(v) { return !!(v && v.url && /^[0-9a-f]{64}$/.test(v.sha256 || '')); }
// the version picked for a board: the newest installable non-pre-release, else the newest installable, else the first
function pickVersion(versions) {
  var i;
  for (i = 0; i < versions.length; i++) if (rowReady(versions[i]) && !versions[i].prerelease) return i;
  for (i = 0; i < versions.length; i++) if (rowReady(versions[i])) return i;
  return 0;
}
function versionLabel(v) {
  return v.version + (v.date ? ' · ' + v.date : '') + (v.prerelease ? ' · pre-release' : '') +
    (v.local ? ' · built on this computer' : '') + (rowReady(v) ? '' : ' · not available yet');
}

// ---------------------------------------------------------------- checksums
function hex(u8) { var s = ''; for (var i = 0; i < u8.length; i++) s += (u8[i] < 16 ? '0' : '') + u8[i].toString(16); return s; }
async function sha256hex(u8) { return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', u8))); }
// MD5 of a Uint8Array as hex: what the library compares with the flash's own MD5 after each part
var MD5_K = [], MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
for (var md5i = 0; md5i < 64; md5i++) MD5_K[md5i] = Math.floor(Math.abs(Math.sin(md5i + 1)) * 4294967296) | 0;
function md5hex(d) {
  var n = d.length, nb = ((n + 8 >> 6) + 1) * 16, w = new Int32Array(nb), i, o;
  for (i = 0; i < n; i++) w[i >> 2] |= d[i] << (i % 4 * 8);
  w[n >> 2] |= 0x80 << (n % 4 * 8);
  w[nb - 2] = n * 8;
  w[nb - 1] = Math.floor(n / 536870912);
  var h = [0x67452301, 0xefcdab89 | 0, 0x98badcfe | 0, 0x10325476];
  for (o = 0; o < nb; o += 16) {
    var A = h[0], B = h[1], C = h[2], D = h[3];
    for (i = 0; i < 64; i++) {
      var r = i >> 4, f, g;
      if (r === 0) { f = (B & C) | (~B & D); g = i; }
      else if (r === 1) { f = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
      else if (r === 2) { f = B ^ C ^ D; g = (3 * i + 5) & 15; }
      else { f = C ^ (B | ~D); g = (7 * i) & 15; }
      var x = (A + f + MD5_K[i] + w[o + g]) | 0, s = MD5_S[r * 4 + (i & 3)];
      A = D; D = C; C = B; B = (B + ((x << s) | (x >>> (32 - s)))) | 0;
    }
    h[0] = (h[0] + A) | 0; h[1] = (h[1] + B) | 0; h[2] = (h[2] + C) | 0; h[3] = (h[3] + D) | 0;
  }
  var out = new Uint8Array(16);
  for (i = 0; i < 16; i++) out[i] = h[i >> 2] >>> (i % 4 * 8) & 255;
  return hex(out);
}

// ---------------------------------------------------------------- the setup Wi-Fi and its QR code
// the firmware names its setup Wi-Fi fauxcloud-setup- + the last two bytes of the Wi-Fi station MAC in upper-case hex
// (app_net.cpp netStartSetup, settings.cpp mac4); the station MAC is the factory MAC esptool-js reads from the eFuses
function setupSsid(mac) {
  if (!/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(mac || '')) return null;
  var p = mac.split(':');
  return 'fauxcloud-setup-' + (p[4] + p[5]).toUpperCase();
}
function wifiQrText(ssid, pass) {
  var esc = function (s) { return s.replace(/([\\;,:"])/g, '\\$1'); };
  return 'WIFI:T:WPA;S:' + esc(ssid) + ';P:' + esc(pass) + ';;';
}

// QR code, byte mode, error correction M, versions 1-10 (a Wi-Fi payload needs 3 or 4)
var QR_M = [null, [10, [1, 16]], [16, [1, 28]], [26, [1, 44]], [18, [2, 32]], [24, [2, 43]], [16, [4, 27]], [18, [4, 31]],
  [22, [2, 38], [2, 39]], [22, [3, 36], [2, 37]], [26, [4, 43], [1, 44]]];
var QR_AL = [null, [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34], [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50]];
var GF_EXP = new Uint8Array(512), GF_LOG = new Uint8Array(256);
(function () { for (var i = 0, x = 1; i < 255; i++) { GF_EXP[i] = x; GF_LOG[x] = i; x <<= 1; if (x & 256) x ^= 0x11d; }
  for (i = 255; i < 512; i++) GF_EXP[i] = GF_EXP[i - 255]; })();
function gfMul(a, b) { return a && b ? GF_EXP[GF_LOG[a] + GF_LOG[b]] : 0; }
function rsEc(data, n) {
  var g = [1], i, j, r = [];
  for (i = 0; i < n; i++) {
    var ng = []; for (j = 0; j <= g.length; j++) ng.push(0);
    for (j = 0; j < g.length; j++) { ng[j] ^= g[j]; ng[j + 1] ^= gfMul(g[j], GF_EXP[i]); }
    g = ng;
  }
  for (i = 0; i < n; i++) r.push(0);
  for (i = 0; i < data.length; i++) {
    var f = data[i] ^ r.shift(); r.push(0);
    for (j = 0; j < n; j++) r[j] ^= gfMul(g[j + 1], f);
  }
  return r;
}
function qrCodewords(bytes, v) {
  var spec = QR_M[v], cap = 0, s, i, j, bits = [], data = [];
  for (s = 1; s < spec.length; s++) cap += spec[s][0] * spec[s][1];
  var put = function (val, len) { for (var k = len - 1; k >= 0; k--) bits.push(val >>> k & 1); };
  put(4, 4); put(bytes.length, v < 10 ? 8 : 16);
  for (i = 0; i < bytes.length; i++) put(bytes[i], 8);
  put(0, Math.min(4, cap * 8 - bits.length));
  while (bits.length % 8) bits.push(0);
  for (i = 0; i < bits.length; i += 8) { var x = 0; for (j = 0; j < 8; j++) x = x << 1 | bits[i + j]; data.push(x); }
  for (var pad = 0xEC; data.length < cap; pad ^= 0xEC ^ 0x11) data.push(pad);
  var blocks = [], ecs = [], at = 0, cw = [], most = 0;
  for (s = 1; s < spec.length; s++) for (j = 0; j < spec[s][0]; j++) {
    var blk = data.slice(at, at + spec[s][1]); at += blk.length; blocks.push(blk); ecs.push(rsEc(blk, spec[0]));
    most = Math.max(most, blk.length);
  }
  for (i = 0; i < most; i++) blocks.forEach(function (b) { if (i < b.length) cw.push(b[i]); });
  for (i = 0; i < spec[0]; i++) ecs.forEach(function (e) { cw.push(e[i]); });
  return cw;
}
var QR_MASKS = [function (x, y) { return (x + y) % 2 === 0; }, function (x, y) { return y % 2 === 0; },
  function (x) { return x % 3 === 0; }, function (x, y) { return (x + y) % 3 === 0; },
  function (x, y) { return (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0; },
  function (x, y) { return x * y % 2 + x * y % 3 === 0; }, function (x, y) { return (x * y % 2 + x * y % 3) % 2 === 0; },
  function (x, y) { return ((x + y) % 2 + x * y % 3) % 2 === 0; }];
// text -> {version, mask, size, rows: [[0|1]]}; `mask` forces one (the vectors compare every mask), else the lowest penalty
function qrEncode(text, mask) {
  var bytes = new TextEncoder().encode(text), v;
  for (v = 1; v <= 10; v++) {
    var cap = 0; for (var s = 1; s < QR_M[v].length; s++) cap += QR_M[v][s][0] * QR_M[v][s][1];
    if (4 + (v < 10 ? 8 : 16) + 8 * bytes.length <= cap * 8) break;
  }
  if (v > 10) throw new Error('text too long for the QR code');
  var cw = qrCodewords(bytes, v);
  if (mask !== undefined) return qrBuild(v, cw, mask);
  var best = null;
  for (var k = 0; k < 8; k++) { var q = qrBuild(v, cw, k); q.penalty = qrPenalty(q.rows); if (!best || q.penalty < best.penalty) best = q; }
  return best;
}
function qrBuild(v, cw, mask) {
  var n = 17 + 4 * v, m = [], fn = [], x, y, i, dx, dy;
  for (y = 0; y < n; y++) { m.push(new Array(n).fill(0)); fn.push(new Array(n).fill(0)); }
  var set = function (cx, cy, dark) { m[cy][cx] = dark ? 1 : 0; fn[cy][cx] = 1; };
  [[0, 0], [n - 7, 0], [0, n - 7]].forEach(function (c) {
    for (dy = -1; dy <= 7; dy++) for (dx = -1; dx <= 7; dx++) {
      x = c[0] + dx; y = c[1] + dy;
      if (x < 0 || y < 0 || x >= n || y >= n) continue;
      var ring = Math.max(Math.abs(dx - 3), Math.abs(dy - 3));
      set(x, y, ring !== 2 && ring !== 4);
    }
  });
  for (i = 0; i < n; i++) { if (!fn[6][i]) set(i, 6, i % 2 === 0); if (!fn[i][6]) set(6, i, i % 2 === 0); }
  var al = QR_AL[v];
  al.forEach(function (ay) { al.forEach(function (ax) {
    if ((ax === 6 && ay === 6) || (ax === 6 && ay === al[al.length - 1]) || (ay === 6 && ax === al[al.length - 1])) return;
    for (dy = -2; dy <= 2; dy++) for (dx = -2; dx <= 2; dx++) set(ax + dx, ay + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
  }); });
  var fmt = function (k) {
    var data = 0 << 3 | k, rem = data;  // level M = 0b00
    for (var j = 0; j < 10; j++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
    var b = (data << 10 | rem) ^ 0x5412, bit = function (j) { return b >>> j & 1; };
    for (j = 0; j <= 5; j++) set(8, j, bit(j));
    set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
    for (j = 9; j < 15; j++) set(14 - j, 8, bit(j));
    for (j = 0; j < 8; j++) set(n - 1 - j, 8, bit(j));
    for (j = 8; j < 15; j++) set(8, n - 15 + j, bit(j));
    set(8, n - 8, 1);
  };
  fmt(0);  // reserve the format areas before the data
  if (v >= 7) {
    var rem = v; for (i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >>> 11) * 0x1F25);
    var vb = v << 12 | rem;
    for (i = 0; i < 18; i++) { var a = n - 11 + i % 3, b2 = Math.floor(i / 3), on = vb >>> i & 1; set(a, b2, on); set(b2, a, on); }
  }
  var bitAt = 0, total = cw.length * 8;
  for (var right = n - 1; right >= 1; right -= 2) {
    if (right === 6) right = 5;
    for (var vert = 0; vert < n; vert++) for (var j = 0; j < 2; j++) {
      x = right - j;
      y = ((right + 1) & 2) === 0 ? n - 1 - vert : vert;
      if (!fn[y][x] && bitAt < total) { m[y][x] = cw[bitAt >>> 3] >>> (7 - (bitAt & 7)) & 1; bitAt++; }
    }
  }
  for (y = 0; y < n; y++) for (x = 0; x < n; x++) if (!fn[y][x] && QR_MASKS[mask](x, y)) m[y][x] ^= 1;
  fmt(mask);
  return { version: v, mask: mask, size: n, rows: m };
}
// the usual penalty rules: runs of five or more, 2x2 blocks, finder-like patterns, the dark share
function qrPenalty(m) {
  var n = m.length, p = 0, x, y, dark = 0;
  var line = function (get) {
    for (var i = 0; i < n; i++) {
      var run = 1, s = '';
      for (var j = 0; j < n; j++) {
        s += get(i, j);
        if (j && get(i, j) === get(i, j - 1)) { run++; if (run === 5) p += 3; else if (run > 5) p++; } else run = 1;
      }
      p += 40 * (s.split('10111010000').length - 1 + s.split('00001011101').length - 1);
    }
  };
  line(function (i, j) { return m[i][j]; });
  line(function (i, j) { return m[j][i]; });
  for (y = 0; y < n - 1; y++) for (x = 0; x < n - 1; x++) {
    var c = m[y][x]; if (c === m[y][x + 1] && c === m[y + 1][x] && c === m[y + 1][x + 1]) p += 3;
  }
  for (y = 0; y < n; y++) for (x = 0; x < n; x++) dark += m[y][x];
  return p + 10 * Math.floor(Math.abs(dark * 20 / (n * n) - 10));
}
function qrSvg(q, label) {
  var d = '';
  for (var y = 0; y < q.size; y++) for (var x = 0; x < q.size; x++) if (q.rows[y][x]) d += 'M' + x + ' ' + y + 'h1v1h-1z';
  var w = q.size + 8;
  return '<svg viewBox="-4 -4 ' + w + ' ' + w + '" role="img" aria-label="' + label + '" shape-rendering="crispEdges">' +
    '<rect x="-4" y="-4" width="' + w + '" height="' + w + '" fill="#fff"/><path fill="#000" d="' + d + '"/></svg>';
}

// ---------------------------------------------------------------- the factory zip, checked before anything is written
// zip bytes + the version the list chose -> {files: [{data, address, part}] in flash order, table}; every refusal throws
async function readFactory(zip, version, log) {
  var idx = zipIndex(zip), pj = Object.keys(idx).filter(function (n) { return /(^|\/)parts\.json$/.test(n); })[0];
  if (!pj) throw new Error('no parts.json in the zip');
  var folder = pj.slice(0, pj.length - 'parts.json'.length);
  var names = Object.keys(idx).filter(function (n) { return n.indexOf(folder) === 0; }).map(function (n) { return n.slice(folder.length); });
  var list = parseParts(new TextDecoder().decode(await zipRead(zip, idx[pj])), names), files = [];
  if (list.version !== version) throw fail('refused', 'the zip is fauxcloud ' + list.version + ', the list says ' + version);
  for (var i = 0; i < list.parts.length; i++) {
    var p = list.parts[i], data = await zipRead(zip, idx[folder + p.name]);
    // the SHA-256 chain (versions.json -> the zip -> parts.json -> each part) is the integrity check; the zip's CRC-32
    // is not read, so these checks must stay
    if (data.length !== p.size || await sha256hex(data) !== p.sha256) throw new Error(p.name + ' does not match its SHA-256');
    log(p.role + ': ' + p.name + ', ' + p.size + ' B at 0x' + p.offset.toString(16));
    files.push({ data: data, address: p.offset, part: p });
  }
  var table = partTable(files.filter(function (f) { return f.part.role === 'partitions'; })[0].data);
  var bad = fixedPlaces(list.parts, table) || keepsSettings(list.parts, table);
  if (bad) throw fail('refused', bad);
  return { files: files, table: table };
}

function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return '&#' + c.charCodeAt(0) + ';'; }); }
function msgOf(e) { return String((e && e.message) || e); }

// ---------------------------------------------------------------- the sequences: connect, install, a lost port
// env: requestPort(), loadLib() (the bundle), fetch(url), version() (the chosen versions.json row), erase() (the box),
// terminal (the library's log), ui: show(state, aux html, opts), aux(html), progress(pct, text), button(label, on),
// lock('idle' | 'linked' | 'busy'), log(text), untick() (the erase box, after every install). Each job holds an epoch;
// when the port goes away, the board changes or Connect is pressed again the epoch moves on, and the old job, once it
// wakes, leaves the page and the new link alone and closes only its own transport. After DONE the one button ("Flash
// another board") brings the form back for the next board (CONNECT USB); it does not open the port chooser.
var BAUD = 460800;
var SETUP_PASS = 'fauxcloud';
var NOT_YET = 'No version of fauxcloud can be installed from here yet: the first release brings it. Until then, ' +
  '<a href="/docs/flash/#by-hand-with-esptool">flash it by hand</a>.';
// esptool's classic hard reset: EN held low for 100 ms through RTS, with DTR (IO0) high, so the board boots its new
// app. esptool-js 0.7.1's after('hard_reset') only releases RTS and never pulls EN low.
async function hardReset(t) {
  await t.setDTR(false);
  await t.setRTS(true);
  await new Promise(function (r) { setTimeout(r, 100); });
  await t.setRTS(false);
}
function makeInstaller(env) {
  var st = 'select', epoch = 0, link = null, port = null, ui = env.ui;
  function go(ev, aux, opts) { st = nextState(st, ev); ui.show(st, aux, opts || {}); }
  function ready() { return rowReady(env.version()); }
  async function close(l) {
    if (!l) return;
    if (link === l) link = null;
    var t = l.transport;
    l.transport = null;
    if (t) try { await t.disconnect(); } catch (e) { /* already closed */ }
  }

  // the board select changed (chosen: a board is picked)
  function board(chosen) {
    epoch++;
    port = null;
    close(link);
    ui.lock('idle');
    if (!chosen) { go('unboard', 'Pick your board to begin.'); ui.button('Connect', false); return; }
    if (!ready()) { go('board', NOT_YET); ui.button('Connect', false); return; }
    go('board', 'Plug the board into this computer with a USB data cable, then press Connect.');
    ui.button('Connect', true);
  }
  // the version select changed
  function version() {
    if (st !== 'connected') { board(true); return; }
    var v = env.version();
    ui.aux(ready() ? 'Ready to install fauxcloud ' + esc(v.version) + '.' : NOT_YET);
    ui.button('Install', ready());
  }

  async function connect() {
    var my = ++epoch;
    port = null;  // the previous board's: its unplug while the chooser is open must not touch this job
    close(link);
    ui.button('Connect', false);
    ui.lock('busy');
    go('connect', 'Choose your board in the list your browser shows.');
    var p;
    try { p = await env.requestPort(); } catch (e) {
      if (my !== epoch) return;
      ui.log('no port chosen: ' + msgOf(e));
      ui.lock('idle');
      go('cancel', 'No board was chosen. If yours was not in the list, check the cable and the driver below, then press Connect.');
      ui.button('Connect', true);
      return;
    }
    if (my !== epoch) return;
    port = p;
    ui.aux('Talking to the board. Some boards need their BOOT button held down now.');
    var l = { transport: null, esp: null, mac: null };
    try {
      var lib;
      try { lib = await env.loadLib(); } catch (e) { throw fail('bundle', msgOf(e)); }
      l.transport = new lib.Transport(p, false);  // no packet tracing
      l.esp = new lib.ESPLoader({ transport: l.transport, baudrate: BAUD, terminal: env.terminal, debugLogging: false });
      var chip = await l.esp.main();
      if (l.esp.chip.CHIP_NAME !== 'ESP32') throw fail('chip', chip);
      var size = await l.esp.detectFlashSize();
      ui.log('flash size: ' + size);
      if (!/^\d+MB$/.test(size || '') || parseInt(size, 10) < 4) throw fail('small', String(size));
      l.mac = await l.esp.chip.readMac(l.esp);
      if (my !== epoch) { await close(l); return; }
      link = l;
      ui.lock('linked');
      go('linked', '<b>' + esc(chip) + '</b>, ' + esc(size) + ' flash. Ready to install fauxcloud ' + esc(env.version().version) + '.');
      ui.button('Install', ready());
    } catch (e) {
      if (my !== epoch) { await close(l); return; }
      ui.log('connect error: ' + msgOf(e));
      ui.lock('idle');
      go('fail', errorSentence(e));
      ui.button('Connect', true);
      await close(l);
    }
  }

  async function install() {
    var l = link;
    if (!l || st !== 'connected') return;
    var my = ++epoch, v = env.version(), erase = env.erase();
    ui.button('Install', false);
    ui.lock('busy');
    go('flash', 'Keep the cable plugged in. This takes about a minute.');
    try {
      if (!rowReady(v)) throw fail('incomplete', 'no url or SHA-256 for ' + (v && v.version));
      ui.log('downloading ' + v.url);
      var r = await env.fetch(v.url);
      if (!r.ok) throw new Error('download failed: HTTP ' + r.status);
      var zip = new Uint8Array(await r.arrayBuffer());
      if (Number.isInteger(v.size) && zip.length !== v.size) throw new Error('the zip is ' + zip.length + ' B, the list says ' + v.size);
      if (await sha256hex(zip) !== v.sha256) throw new Error('the zip does not match its SHA-256');
      var fac = await readFactory(zip, v.version, ui.log);
      if (my !== epoch) { await close(l); return; }
      // the board's own table: a readable one that differs from the release's needs the erase box
      var have = null;
      try { have = await l.esp.readFlash(0x8000, 0xC00); } catch (e) { ui.log('the board\'s partition table did not read (' + msgOf(e) + '): taken as a new board'); }
      if (my !== epoch) { await close(l); return; }
      ui.log(have && partTable(have).length ? 'the board\'s partition table: ' + partTable(have).map(function (t) { return t.label; }).join(', ')
        : 'the board has no partition table yet');
      if (layoutVerdict(have, fac.table, erase)) throw fail('layout', 'the board\'s partition table differs from the release\'s');
      if (erase) { ui.log('erase everything first (the box is ticked)'); ui.aux('Erasing the whole flash first, settings included. A few seconds.'); }
      var sizes = fac.files.map(function (f) { return f.data.length; });
      await l.esp.writeFlash({
        fileArray: fac.files.map(function (f) { return { data: f.data, address: f.address }; }),
        flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', eraseAll: erase, compress: true,
        reportProgress: function (i, written, total) {
          if (my !== epoch) return;
          ui.progress(percent(sizes, i, written, total), written === 0 ? 'Writing ' + fac.files[i].part.role.replace('_', ' ') +
            ' (' + (i + 1) + ' of ' + fac.files.length + '). Keep the cable plugged in.' : null);
        },
        calculateMD5Hash: md5hex
      });
      if (my !== epoch) { await close(l); return; }
      var note = '';
      try { ui.log('restarting the board (EN low for 100 ms)'); await hardReset(l.transport); } catch (e) {
        ui.log('reset error: ' + msgOf(e));
        note = ' It did not restart by itself: press its EN (RESET) button, or unplug it.';
      }
      if (my !== epoch) { await close(l); return; }
      var ssid = setupSsid(l.mac);
      go('done', 'fauxcloud ' + esc(v.version) + ' is on the board' +
        (erase ? '. The board was erased first.' : '. Any settings it had were kept.') + note,
        { ssid: ssid, qr: ssid ? qrSvg(qrEncode(wifiQrText(ssid, SETUP_PASS)), 'QR code: join the Wi-Fi ' + ssid) : '' });
      ui.button('Flash another board', true);
    } catch (e) {
      if (my !== epoch) { await close(l); return; }
      ui.log('install error: ' + msgOf(e));
      go('fail', errorSentence(e));
      ui.button('Connect', true);
    }
    ui.untick();  // every install ends on the safe default: the next board keeps its settings unless the box is ticked again
    ui.lock('idle');
    await close(l);
  }

  // the browser's serial disconnect event for port p
  function lost(p) {
    if (!p || p !== port) return;
    port = null;
    if (st !== 'connecting' && st !== 'connected' && st !== 'flashing') return;
    epoch++;
    close(link);
    ui.log('the port went away');
    ui.lock('idle');
    go('lost', 'The board was disconnected. Unplug it, plug it in again and press Connect.');
    ui.button('Connect', ready());
  }

  return {
    board: board, version: version, connect: connect, install: install, lost: lost,
    press: function () { if (st === 'done') return board(true); return st === 'connected' ? install() : connect(); },
    state: function () { return st; }
  };
}

// ---------------------------------------------------------------- the page
var BUNDLE = '/assets/esptool-bundle.js';

function boot() {
  var $ = function (id) { return document.getElementById(id); };
  var card = $('fl'), gate = $('fl-gate');
  if (!card) return;
  if (!('serial' in navigator)) { gate.hidden = false; card.hidden = true; return; }
  var boardSel = $('fl-board'), verSel = $('fl-ver'), go = $('fl-go'), erase = $('fl-erase'), logEl = $('fl-log');
  var boards = {}, pct = 0;

  function log(s) { logEl.textContent += s + '\n'; logEl.scrollTop = logEl.scrollHeight; }
  var ui = {
    log: log,
    show: function (state, aux, opts) {
      var s = STATES[state], v = view(state, opts);
      if (state !== 'flashing') pct = state === 'done' ? 100 : 0;
      $('fl-word').textContent = stateWord(state, pct);
      $('fl-state').dataset.tone = s[2];
      $('fl-icon').setAttribute('href', '#' + s[1]);
      $('fl-aux').innerHTML = aux;
      $('fl-bar').hidden = !v.bar;
      $('fl-bar').firstChild.style.width = pct + '%';
      $('fl-qr').hidden = !v.qr;
      $('fl-qr-say').hidden = !v.qr;  // "scan the QR code on this page, or" only beside a QR
      if (v.qr) $('fl-qr').innerHTML = opts.qr;
      $('fl-photo').hidden = v.qr || !boardSel.value;
      $('fl-mark').hidden = v.qr || !!boardSel.value;
      if (state === 'done') { $('fl-ssid').textContent = opts.ssid || 'fauxcloud-setup-XXXX'; $('fl-ssid-note').hidden = !!opts.ssid; }
      $('fl-next').hidden = !v.next;
      card.classList.toggle('done', v.next);
      $('fl-opts').hidden = !v.opts;
      go.classList.toggle('primary', !v.quiet);
      go.classList.toggle('quiet', v.quiet);
      $('fl-help').classList.toggle('lit', v.help);
      if (v.help) $('fl-help').open = true;  // opened for NO BOARD and FAILED, never folded by the page
    },
    aux: function (html) { $('fl-aux').innerHTML = html; },
    progress: function (p, text) {
      pct = p;
      $('fl-word').textContent = stateWord('flashing', p);
      $('fl-bar').firstChild.style.width = p + '%';
      if (text) $('fl-aux').textContent = text;
    },
    button: function (label, on) { go.textContent = label; go.disabled = !on; },
    lock: function (phase) {
      boardSel.disabled = phase !== 'idle';
      verSel.disabled = phase === 'busy' || !boardSel.value;
      erase.disabled = phase === 'busy';
    },
    untick: function () { erase.checked = false; }
  };
  function version() { var b = boards[boardSel.value]; return b && b.versions[+verSel.value]; }
  var inst = makeInstaller({
    ui: ui, version: version, erase: function () { return erase.checked; },
    requestPort: function () { return navigator.serial.requestPort(); },
    loadLib: function () { return import(BUNDLE); },
    fetch: function (url) { return fetch(url, { cache: 'no-store' }); },
    terminal: { clean: function () {}, writeLine: log, write: function (d) { logEl.textContent += d; } }
  });

  function fillVersions() {
    var b = boards[boardSel.value];
    verSel.innerHTML = '';
    if (!b) { verSel.add(new Option('Choose a board first', '')); return; }
    b.versions.forEach(function (v, i) { var o = new Option(versionLabel(v), String(i)); o.disabled = !rowReady(v); verSel.add(o); });
    verSel.value = String(pickVersion(b.versions));
  }
  boardSel.onchange = function () {
    fillVersions();
    var b = boards[boardSel.value], img = $('fl-photo');
    if (b) { img.src = b.photo; img.width = b.photo_size[0]; img.height = b.photo_size[1]; img.alt = b.label; }
    inst.board(!!b);
  };
  verSel.onchange = inst.version;
  // the log's Copy button: the whole log as text (for a bug report); says so for a moment, as the docs' code blocks do
  var copy = $('fl-copy');
  copy.onclick = function () {
    if (navigator.clipboard) navigator.clipboard.writeText(logEl.textContent).then(function () {
      copy.textContent = 'Copied'; setTimeout(function () { copy.textContent = 'Copy'; }, 1500); }, function () {});
  };
  go.onclick = inst.press;
  navigator.serial.addEventListener('disconnect', function (e) { inst.lost(e.target); });

  fetch('/firmware/versions.json', { cache: 'no-store' }).then(function (r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).then(function (j) {
    boards = j.boards || {};
    boardSel.innerHTML = '';
    boardSel.add(new Option('Choose your board', ''));
    Object.keys(boards).forEach(function (id) { boardSel.add(new Option(boards[id].label, id)); });
    fillVersions();
    ui.lock('idle');
    ui.show('select', 'Pick your board to begin.', {});
  }).catch(function (e) {
    log('versions.json: ' + msgOf(e));
    ui.show('failed', 'The list of versions did not load. Reload the page, or <a href="/docs/flash/#by-hand-with-esptool">flash it by hand</a>.', { noHelp: true });
  });
}

if (typeof document !== 'undefined') boot();
