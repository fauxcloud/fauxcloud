// test/tools/crypto_vectors.js -- runs firmware/fauxcloud/web/crypto.js (the browser's half of the challenge-response login) under
// node against the vectors the C++ host test checks (test/host/test_fauxcloud.cpp, testPbkdf2): if the two sides
// ever disagree, nobody can sign in. Usage: node test/tools/crypto_vectors.js
'use strict';
const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'crypto.js'), 'utf8');
const FCX = new Function(src + '\nreturn FCX;')();

let fail = 0, pass = 0;
function check(name, got, want) {
  if (got === want) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + got + ' != ' + want); }
}

const salt16 = FCX.unhex('000102030405060708090a0b0c0d0e0f');
const pw = 'correct horse battery staple ż';  // U+017C in UTF-8: the non-ASCII byte path
check('RFC 7914 PBKDF2 (passwd, salt, 1)', FCX.hex(FCX.pbkdf2('passwd', 'salt', 1)),
      '55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc');
check('PBKDF2 10000 (the device vector)', FCX.hex(FCX.pbkdf2(pw, salt16, 10000)),
      '460b333275bd03de2073208552f336d871c8cf4ea173a9b5e61c0ef17e98998f');
check('PBKDF2 with a key longer than the block', FCX.hex(FCX.pbkdf2('x'.repeat(100), 'salt', 2)),
      'd43a18cd77bafc1a4b0c6025dbbf29c7e6d67acce6ad02a736d4a3003b6a3c26');
check('HMAC-SHA256 (key, fox)', FCX.hex(FCX.hmac('key', 'The quick brown fox jumps over the lazy dog')),
      'f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8');
check('login proof', FCX.proof(pw, '000102030405060708090a0b0c0d0e0f', 10000, '00112233445566778899aabbccddeeff'),
      '6358e538940f03d3d087a6ab8dceb417a0e3a26746d6bd589bd457ba703926e5');
console.log((fail ? 'FAILED' : 'OK') + ': ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
