'use strict';
// dash-lg.js -- the status face's edge refraction (Chromium only, progressive; loaded on
// first render through FC.lazy() by dash.js, elsewhere the CSS blur stays). Port of the liquid-glass-js field
// via LiquidGlassSkill; notices in licenses.txt. Same global scope as status.js (lgOK, lgAll, fillRow).
const LG = { ei: .015, ri: .028, ed: .5, rd: 1.7, cb: .06, rp: .26, blur: 4 }, lgCache = new Map();
let lgDefs = null, lgN = 0;
function lgMap(w, h, rad, pw, ph) {
  const ss = Math.max(.25, Math.min(2, 1400 / Math.max(w, h))), bw = Math.max(1, Math.round(w * ss)), bh = Math.max(1, Math.round(h * ss));
  const R = rad * ss, md = Math.min(w, h), n = bw * bh, dx = new Float32Array(n), dy = new Float32Array(n);
  let mx = 0;
  for (let y = 0; y < bh; y++) for (let x = 0; x < bw; x++) {
    const cx = (x + .5) / bw, cy = (y + .5) / bh, tx = Math.abs(x + .5 - bw / 2) - (bw / 2 - R), ty = Math.abs(y + .5 - bh / 2) - (bh / 2 - R);
    const d = Math.max(-(Math.hypot(Math.max(tx, 0), Math.max(ty, 0)) + Math.min(Math.max(tx, ty), 0) - R), 0) / ss;
    const rim = Math.exp(-d * LG.rd), tot = Math.exp(-d * LG.ed) * LG.ei + rim * LG.ri;
    let nx = cx - .5, ny = cy - .5; const l = Math.hypot(nx, ny); if (l) { nx /= l; ny /= l; }
    const co = Math.exp(-Math.max(Math.min(cx, 1 - cx), Math.min(cy, 1 - cy)) * md * .3) * LG.cb, rp = Math.sin(d / md * 25) * LG.rp * rim, i = y * bw + x;
    dx[i] = (nx * (tot + co) - ny * rp) * pw; dy[i] = (ny * (tot + co) + nx * rp) * ph;
    mx = Math.max(mx, Math.abs(dx[i]), Math.abs(dy[i]));
  }
  const s = Math.max(mx * 2, 1e-4), bias = s * (128 / 255 - .5), cvs = document.createElement('canvas');
  cvs.width = bw; cvs.height = bh;
  const ctx = cvs.getContext('2d'), im = ctx.createImageData(bw, bh), D = im.data, cl = v => Math.max(0, Math.min(255, Math.round(v)));
  for (let i = 0; i < n; i++) { D[i * 4] = cl(255 * (.5 + (dx[i] - bias) / s)); D[i * 4 + 1] = cl(255 * (.5 + (dy[i] - bias) / s)); D[i * 4 + 2] = 128; D[i * 4 + 3] = 255; }
  ctx.putImageData(im, 0, 0);
  return { url: cvs.toDataURL(), s };
}
function lgApply(el) {
  const w = Math.round(el.offsetWidth), h = Math.round(el.offsetHeight);
  if (w < 2 || h < 2) return;
  const rad = Math.min(parseFloat(getComputedStyle(el).borderTopLeftRadius) || 0, Math.min(w, h) / 2), key = [w, h, rad, innerWidth, innerHeight].join('|');
  if (el.dataset.lg === key) return;
  el.dataset.lg = key;
  let id = lgCache.get(key);
  if (!id || !document.getElementById(id)) {
    const { url, s } = lgMap(w, h, rad, innerWidth, innerHeight), NS = 'http://www.w3.org/2000/svg', mg = s / 2 + 3 * LG.blur * .35, mx = mg / w * 100, my = mg / h * 100;
    if (!lgDefs || !lgDefs.isConnected) {
      const svg = document.createElementNS(NS, 'svg');
      svg.setAttribute('width', '0'); svg.setAttribute('height', '0'); svg.setAttribute('aria-hidden', 'true'); svg.style.position = 'fixed';
      lgDefs = svg.appendChild(document.createElementNS(NS, 'defs')); document.body.appendChild(svg);
    }
    id = 'lg' + lgN++;
    const f = document.createElementNS(NS, 'filter');
    Object.entries({ id, x: -mx + '%', y: -my + '%', width: 100 + 2 * mx + '%', height: 100 + 2 * my + '%', 'color-interpolation-filters': 'sRGB' }).forEach(([k, v]) => f.setAttribute(k, v));
    f.innerHTML = `<feImage href="${url}" x="0" y="0" width="${w}" height="${h}" preserveAspectRatio="none" result="m"/><feDisplacementMap in="SourceGraphic" in2="m" scale="${s}" xChannelSelector="R" yChannelSelector="G" result="d"/><feGaussianBlur in="d" stdDeviation="${LG.blur * .35}" result="b"/><feColorMatrix in="b" type="saturate" values="1.5"/>`;
    lgDefs.appendChild(f); lgCache.set(key, id);
  }
  el.style.backdropFilter = el.style.webkitBackdropFilter = `url(#${id})`;
}
const lgRO = lgOK && 'ResizeObserver' in window ? new ResizeObserver(es => requestAnimationFrame(() => es.forEach(e => lgApply(e.target)))) : null;
lgAll = () => { if (lgRO) document.querySelectorAll('.lg').forEach(el => { lgRO.observe(el); lgApply(el); }); };
lgAll();
