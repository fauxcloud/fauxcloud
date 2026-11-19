// test/tools/js_decl.js -- takes one top-level declaration out of a page script by name, for the node vectors that
// run a few functions of dash.js / dash-sheet.js in a vm without the whole page (text_vectors.js, format_vectors.js).
'use strict';

// One top-level declaration out of a source: `function NAME(` or `const NAME =` at a line start, to the end of its
// statement: brackets balanced, strings and template literals (with their ${} expressions) skipped. No regex literals
// in the declarations taken (own / esc come as stubs for that reason).
function skipStr(src, i) {
  const q = src[i];
  for (i++; src[i] !== q; i++) { if (i >= src.length || src[i] === '\n') throw new Error('unclosed string'); if (src[i] === '\\') i++; }
  return i;
}
function skipTpl(src, i) {
  for (i++; src[i] !== '`'; i++) {
    if (i >= src.length) throw new Error('unclosed template');
    if (src[i] === '\\') i++;
    else if (src[i] === '$' && src[i + 1] === '{') {
      let d = 1;
      for (i += 2; d; i++) {
        const c = src[i];
        if (c === "'" || c === '"') i = skipStr(src, i);
        else if (c === '`') i = skipTpl(src, i);
        else if (c === '{') d++;
        else if (c === '}') d--;
      }
      i--;
    }
  }
  return i;
}
function decl(src, name) {
  const m = new RegExp('^(?:function ' + name + '\\(|const ' + name + ' = )', 'm').exec(src);
  if (!m) throw new Error('no declaration ' + name);
  let d = 0, i = m.index;
  const fn = m[0].startsWith('function');
  for (; i < src.length; i++) {
    const c = src[i];
    if (c === '/' && src[i + 1] === '/') { i = src.indexOf('\n', i); continue; }  // a line comment (may hold an apostrophe)
    if (c === "'" || c === '"') { i = skipStr(src, i); continue; }
    if (c === '`') { i = skipTpl(src, i); continue; }
    if ('([{'.includes(c)) d++;
    else if (')]}'.includes(c)) { d--; if (fn && d === 0 && c === '}') { i++; break; } }
    else if (!fn && c === ';' && d === 0) { i++; break; }
  }
  return src.slice(m.index, i);
}

// core.js's text helpers for a vm without the page (FC.w as the English identity, FC.t = core.js's fill with
// counts, FC.pr the English count rule), copied word for word and checked against core.js
const FILL = "const form = (s, n) => typeof s == 'object' ? s[FC.pr(n)] ?? s.other : s;\n" +
  "const fill = (s, o = {}) => form(s, o.n).replace(/\\{(\\w+)\\}/g, (m, k) => k in o ? o[k] : m);";
function fcText(core) {
  if (!FILL.split('\n').every(l => core.includes(l)) || !core.includes("w: (f, t) => t, t: fill, form, pr: n => n == 1 ? 'one' : 'other', en: (t, k) => t[k] }"))
    throw new Error('the text helpers changed in core.js: update js_decl.js fcText');
  return FILL + "\nObject.assign(FC, { w: (f, t) => t, t: fill, form, pr: n => n == 1 ? 'one' : 'other', en: (t, k) => t[k] });\n";
}

// the same helpers as a live object, for the vectors that hand FC to a page script (English, as core.js has them)
const stubForm = (s, n) => typeof s == 'object' ? s[n == 1 ? 'one' : 'other'] ?? s.other : s;
const fcStub = (o = {}) => Object.assign(o, { w: (f, t) => t, form: stubForm, pr: n => n == 1 ? 'one' : 'other', en: (t, k) => t[k],
  t: (s, v = {}) => stubForm(s, v.n).replace(/\{(\w+)\}/g, (m, k) => k in v ? v[k] : m) });

module.exports = { decl, fcText, fcStub };
