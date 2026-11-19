// crypto.js -- SHA-256 / HMAC-SHA256 / PBKDF2-SHA256 in plain JS for the challenge-response login
// and the password change. WebCrypto's subtle API needs a secure context (HTTPS), which the ESP does not have, so this is
// self-contained. PBKDF2 keeps the HMAC key's inner/outer states and works on 32-bit words: 2 compressions per
// iteration. The C++ host test checks the same vectors. Inlined by tools/gen_web_assets.py.
var FCX = (function () {  // FCX: the crypto namespace (FC is core.js)
  // FIPS 180-4 constants; Int32Array wraps values above 2^31 to the signed 32-bit words the code works on.
  var K = new Int32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);
  var IV = new Int32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
  var W = new Int32Array(64);

  // One compression of W[0..15] into H (in place).
  function compress(H) {
    var i, x, y;
    for (i = 16; i < 64; i++) {
      x = W[i - 15];
      y = W[i - 2];
      W[i] = (W[i - 16] + ((x >>> 7 | x << 25) ^ (x >>> 18 | x << 14) ^ (x >>> 3)) + W[i - 7] +
              ((y >>> 17 | y << 15) ^ (y >>> 19 | y << 13) ^ (y >>> 10))) | 0;
    }
    var a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7], t1, t2;
    for (i = 0; i < 64; i++) {
      t1 = (h + ((e >>> 6 | e << 26) ^ (e >>> 11 | e << 21) ^ (e >>> 25 | e << 7)) + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
      t2 = (((a >>> 2 | a << 30) ^ (a >>> 13 | a << 19) ^ (a >>> 22 | a << 10)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    H[0] = (H[0] + a) | 0; H[1] = (H[1] + b) | 0; H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
    H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0; H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
  }

  // Hashes `bytes` starting from state `start` that already absorbed `prefix` bytes; returns the 8-word digest.
  function hashFrom(start, prefix, bytes) {
    var H = new Int32Array(start), n = bytes.length, total = prefix + n;
    var padLen = ((n + 9 + 63) >> 6) << 6, buf = new Uint8Array(padLen), i, j;
    buf.set(bytes);
    buf[n] = 0x80;
    var bits = total * 8;
    buf[padLen - 4] = bits >>> 24; buf[padLen - 3] = bits >>> 16; buf[padLen - 2] = bits >>> 8; buf[padLen - 1] = bits;
    buf[padLen - 5] = Math.floor(total / 0x20000000);
    for (i = 0; i < padLen; i += 64) {
      for (j = 0; j < 16; j++)
        W[j] = buf[i + 4 * j] << 24 | buf[i + 4 * j + 1] << 16 | buf[i + 4 * j + 2] << 8 | buf[i + 4 * j + 3];
      compress(H);
    }
    return H;
  }

  function wordsToBytes(H) {
    var out = new Uint8Array(32);
    for (var i = 0; i < 8; i++) {
      out[4 * i] = H[i] >>> 24; out[4 * i + 1] = H[i] >>> 16; out[4 * i + 2] = H[i] >>> 8; out[4 * i + 3] = H[i];
    }
    return out;
  }

  function utf8(s) { return typeof s === "string" ? new TextEncoder().encode(s) : s; }

  // Inner / outer states of HMAC with `key`.
  function hmacKey(key) {
    key = utf8(key);
    if (key.length > 64) key = wordsToBytes(hashFrom(IV, 0, key));
    var block = new Uint8Array(64), st = {i: new Int32Array(IV), o: new Int32Array(IV)}, j;
    block.set(key);
    for (j = 0; j < 16; j++)
      W[j] = (block[4 * j] ^ 0x36) << 24 | (block[4 * j + 1] ^ 0x36) << 16 | (block[4 * j + 2] ^ 0x36) << 8 | (block[4 * j + 3] ^ 0x36);
    compress(st.i);
    for (j = 0; j < 16; j++)
      W[j] = (block[4 * j] ^ 0x5c) << 24 | (block[4 * j + 1] ^ 0x5c) << 16 | (block[4 * j + 2] ^ 0x5c) << 8 | (block[4 * j + 3] ^ 0x5c);
    compress(st.o);
    return st;
  }

  function hmac(key, msg) {
    var st = typeof key.i !== "undefined" ? key : hmacKey(key);
    return wordsToBytes(hashFrom(st.o, 64, wordsToBytes(hashFrom(st.i, 64, utf8(msg)))));
  }

  // PBKDF2-HMAC-SHA256 with one 32-byte block (all fauxcloud needs).
  function pbkdf2(password, salt, iters) {
    var st = hmacKey(password), s = utf8(salt), first = new Uint8Array(s.length + 4), j, k;
    first.set(s);
    first[s.length + 3] = 1;
    var u = hmac(st, first), U = new Int32Array(8), T = new Int32Array(8), H = new Int32Array(8);
    for (j = 0; j < 8; j++) T[j] = U[j] = u[4 * j] << 24 | u[4 * j + 1] << 16 | u[4 * j + 2] << 8 | u[4 * j + 3];
    for (k = 1; k < iters; k++) {
      // inner: ipad state + U (32 bytes) + padding, total 96 bytes = 768 bits
      for (j = 0; j < 8; j++) { W[j] = U[j]; H[j] = st.i[j]; }
      W[8] = 0x80000000 | 0; W[9] = W[10] = W[11] = W[12] = W[13] = W[14] = 0; W[15] = 768;
      compress(H);
      // outer: opad state + inner digest
      for (j = 0; j < 8; j++) { W[j] = H[j]; U[j] = st.o[j]; }
      W[8] = 0x80000000 | 0; W[9] = W[10] = W[11] = W[12] = W[13] = W[14] = 0; W[15] = 768;
      compress(U);
      for (j = 0; j < 8; j++) T[j] ^= U[j];
    }
    return wordsToBytes(T);
  }

  function hex(b) {
    var s = "";
    for (var i = 0; i < b.length; i++) s += (b[i] < 16 ? "0" : "") + b[i].toString(16);
    return s;
  }
  function unhex(s) {
    var b = new Uint8Array(s.length / 2);
    for (var i = 0; i < b.length; i++) b[i] = parseInt(s.substr(2 * i, 2), 16);
    return b;
  }
  function rand(n) {
    var b = new Uint8Array(n);
    crypto.getRandomValues(b);  // available without a secure context, unlike crypto.subtle
    return b;
  }
  // proof = HMAC(H, nonce as ASCII hex) with H = PBKDF2(password, salt, iters) -- what the ESP checks.
  function proof(password, saltHex, iters, nonce) { return hex(hmac(pbkdf2(password, unhex(saltHex), iters), nonce)); }
  // A new credential for setup / password change: the password never leaves the browser.
  function credential(password, iters) {
    var salt = rand(16);
    return {salt: hex(salt), h: hex(pbkdf2(password, salt, iters)), iters: iters};
  }
  return {sha256: function (b) { return hex(wordsToBytes(hashFrom(IV, 0, utf8(b)))); }, hmac: hmac, pbkdf2: pbkdf2,
          hex: hex, unhex: unhex, rand: rand, proof: proof, credential: credential};
})();
