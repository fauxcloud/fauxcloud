// tools/site_shot.js -- one phone-sized screenshot (375 x 812 CSS px at 2x, dark) of a page, over the DevTools protocol
// of a Chromium-based browser started headless with --remote-debugging-port. Used for the docs pictures of the admin
// face (site/README.md, "Screenshots of the admin face"); Node 22 or later (fetch and WebSocket built in).
//
//   node tools/site_shot.js <port> <url> <out.png> [wait ms, default 9000]
'use strict';
const fs = require('fs');
const [port, url, out, waitMs = '9000'] = process.argv.slice(2);
if (!port || !url || !out) {
  console.error('usage: node tools/site_shot.js <port> <url> <out.png> [wait ms]');
  process.exit(2);
}
(async () => {
  const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  const page = list.find(t => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0;
  const pend = new Map();
  ws.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
  };
  await new Promise(r => { ws.onopen = r; });
  const cmd = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  // a phone: the window of a desktop headless browser has a minimum width, so the page is emulated, not resized
  await cmd('Emulation.setDeviceMetricsOverride', { width: 375, height: 812, deviceScaleFactor: 2, mobile: true });
  await cmd('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] });
  await cmd('Page.enable');
  await cmd('Page.navigate', { url });
  await new Promise(r => setTimeout(r, +waitMs));
  const shot = await cmd('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(out, Buffer.from(shot.result.data, 'base64'));
  ws.close();
  process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
