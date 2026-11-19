// test/tools/setup_vectors.js -- web/setup.js's subnet check on the success card: a home network that overlaps fauxcloud's
// own Wi-Fi (192.168.77.0/24) under the wider of the two masks, the /24 test on the address alone when no mask comes (an
// older image), and the card's warning in place of the address: a /24 network the address advice, a wider one the
// subnet-mask advice with its network/prefix. Usage: node test/tools/setup_vectors.js
// The vectors use private ranges beyond the documented 192.168.1.x on purpose: the check has to see /16, /17 and /20
// networks, a neighbouring 192.169 range and the 10/8 and 172.16/12 networks (allowed in this file by tools/leakgate-allow.txt).
'use strict';
const fs = require('fs');
const path = require('path');
const { decl } = require('./js_decl');

const src = fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'fauxcloud', 'web', 'setup.js'), 'utf8');
let fail = 0, pass = 0;
function check(name, got, want) {
  if (JSON.stringify(got) === JSON.stringify(want)) pass++;
  else { fail++; console.log('FAIL ' + name + ': ' + JSON.stringify(got) + ' != ' + JSON.stringify(want)); }
}
const pick = names => names.map(n => decl(src, n)).join('\n');
const clash = new Function(pick(['ipNum', 'subnetClash']) + '\nreturn subnetClash;')();
const wideNet = new Function(pick(['ipNum', 'wideNet']) + '\nreturn wideNet;')();

// the old behaviour, kept: a /24 home network on 192.168.77.x clashes, any other /24 does not; no mask -> the /24 test
check('192.168.77.23/24 clashes', clash('192.168.77.23', '255.255.255.0'), true);
check('192.168.1.5/24 does not', clash('192.168.1.5', '255.255.255.0'), false);
check('no mask (an older image): 192.168.77.x clashes', [clash('192.168.77.23', ''), clash('192.168.77.23', undefined)], [true, true]);
check('no mask: 192.168.1.5 does not', clash('192.168.1.5', ''), false);
check('no address: no warning', [clash('', '255.255.0.0'), clash(undefined, undefined)], [false, false]);
// the new: a home network wider than /24 that holds 192.168.77.0
check('192.168.1.5/16 holds 192.168.77.x', clash('192.168.1.5', '255.255.0.0'), true);
check('192.168.200.9/17 holds 192.168.128-255 only: no', clash('192.168.200.9', '255.255.128.0'), false);
check('192.168.70.9/20 holds 192.168.64-79: yes', clash('192.168.70.9', '255.255.240.0'), true);
check('192.168.77.130/25 (narrower than ours): yes', clash('192.168.77.130', '255.255.255.128'), true);
check('10.0.0.5/8 and 172.16.0.5/12: no', [clash('10.0.0.5', '255.0.0.0'), clash('172.16.0.5', '255.240.0.0')], [false, false]);
check('192.168.0.0/16 from a 192.169 neighbour: no', clash('192.169.1.5', '255.255.0.0'), false);
check('a malformed mask counts as none (the /24 test)', [clash('192.168.1.5', '255.255.0'), clash('192.168.77.4', 'x.y.z.w')], [false, true]);
// the card uses it (the address's line is replaced by the warning)
check('success() takes the clash from subnetClash(sta_ip, sta_mask)', /const clash = subnetClash\(j\.sta_ip, j\.sta_mask\)/.test(src), true);
// the network a wide warning names: network/prefix, only wider than /24
check('wideNet: /16 and /20 named, /24 and /25 not', [wideNet('192.168.1.5', '255.255.0.0'), wideNet('192.168.70.9', '255.255.240.0'),
  wideNet('192.168.77.23', '255.255.255.0'), wideNet('192.168.77.130', '255.255.255.128')], ['192.168.0.0/16', '192.168.64.0/20', '', '']);
check('a 0.0.0.0 mask counts as none (the /24 test; no "/0" network)', [clash('192.168.1.5', '0.0.0.0'), clash('192.168.77.4', '0.0.0.0'),
  wideNet('192.168.77.4', '0.0.0.0')], [false, true, '']);
check('wideNet: no mask, a malformed or non-contiguous mask: none', [wideNet('192.168.1.5', ''), wideNet('192.168.1.5', '255.0.255.0'),
  wideNet('192.168.1.5', '255.255.0')], ['', '', '']);
// the success card rendered (setup.js success() with stubs): the address, the /24 warning, the wide warning
const card = { innerHTML: '' };
const success = new Function('$', 'S', 'esc', 'ssidOf', 'apName', 'WU_STEP', 'dots',
  pick(['ipNum', 'subnetClash', 'wideNet', 'success']) + '\nreturn success;')(() => card, { keep: { ap: true, admin: true } },
  s => String(s), () => 'Home', () => 'fauxcloud-0000', '', () => {});
const show = (ip, mask) => { success({ hostname: 'fauxcloud', sta_ssid: 'Home', sta_ip: ip, sta_mask: mask }); return card.innerHTML; };
const WIDE = 'Your home network (192.168.0.0/16) includes 192.168.77.x, the address range of fauxcloud’s own Wi-Fi, so the ' +
  'dashboard will not open from your home network. Set your router’s subnet mask to 255.255.255.0, then restart fauxcloud ' +
  'from Settings (or unplug it for a few seconds): it joins again with a new address.';
const NARROW = 'Your home network also uses 192.168.77.x, the same as fauxcloud’s own Wi-Fi, so the dashboard will not open ' +
  'from your home network. Change your router’s network address (for example to 192.168.1.x), then unplug fauxcloud for a few ' +
  'seconds: it joins again with a new address.';
const h16 = show('192.168.1.5', '255.255.0.0'), h24 = show('192.168.77.23', '255.255.255.0'), hok = show('192.168.1.5', '255.255.255.0');
check('/16 home network: the wide sentence, its network named, no address line', [h16.includes(WIDE), h16.includes(NARROW), h16.includes('class="address"')], [true, false, false]);
check('/24 on 192.168.77.x: the address sentence (unchanged), not the wide one', [h24.includes(NARROW), h24.includes(WIDE), h24.includes('class="address"')], [true, false, false]);
check('no clash: the address, no warning', [hok.includes('class="address"'), /role="alert"/.test(hok)], [true, false]);
check('no mask (an older image) on 192.168.77.x: the address sentence', show('192.168.77.23', '').includes(NARROW), true);

console.log('setup_vectors: ' + pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
