// tools/terser_run.js -- called by gen_web_assets.py: JavaScript on stdin -> terser, mangle only:
// compress off, comments off, top-level names kept (classic scripts share globals across files). Exact terser version
// from tools/package-lock.json (npm ci --ignore-scripts); the output is byte-identical on every machine.
'use strict';
const { minify } = require('terser');
const v = require('terser/package.json').version;
if (v !== '5.36.0') {  // the pin (tools/package.json): another version would change every hash without saying why
  process.stderr.write('terser ' + v + ' loaded, 5.36.0 pinned: run `npm ci --ignore-scripts` in tools/\n');
  process.exit(1);
}
let src = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', d => { src += d; });
process.stdin.on('end', async () => {
  try {
    const r = await minify(src, { compress: false, mangle: true, format: { comments: false } });
    process.stdout.write(r.code);
  } catch (e) {
    process.stderr.write(String(e && e.message || e) + '\n');
    process.exit(1);
  }
});
