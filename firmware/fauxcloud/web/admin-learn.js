// admin-learn.js -- lazy: the admin "Learning" sheet:
// per learner its name, its value as "Measured …" (learned, in use) or "Default … (not yet
// learned)", "In use" with the one sentence of what it changes, "Still learning: about {k} more …" from the firmware's
// count (`need`; one table of bars, `bars`), a 3-bar meter whose tooltip names the bars; L3 / L4 for information; L7 its
// stage sentence; the sky map (L1b); the actionable warnings; "Restart learning". No n_eff, no gate counts (they stay
// in /api/learn and the bug-report download). Whole templates with named placeholders, units in the template.
(() => {
  const W = FC.w('admin-learn', {
    status: 'Each part below is used by the outlook once it is learned; until then the standard rules apply.',
    backfill: 'Reading the stored history: {n} hours so far.',
    paused: 'Paused: set the bridge location (Location and time).',
    nostore: 'No history store: learning from the last 48 hours only.',
    ro: 'Read-only: a newer firmware wrote this learning state.',
    written: 'Saved {ago}.', unsaved: 'Not saved yet: the first save comes within 6 hours.',
    t1: 'Clear-sky level', t1b: 'Shade around the sensor', t2: 'Humidity at saturation', t3: 'Smallest wind reported',
    t4: 'Usual wind here', t5: 'Evening cooling', t6: 'Night frost', t7: 'Pressure check',
    meas: 'Measured {v}', def: 'Default {v} (not yet learned)', std: 'Standard {v} (not yet learned)', nl: 'Not learned yet',
    inuse: 'In use.', info: 'For information; the outlook does not use it.',
    // what each part says, measured (and what it changes once in use)
    l1: 'Measured at {k} of 12 sun heights; the others fill in as the sun climbs through the year.',
    // the learner counts clear-sky hours, not hours of sunshine (a bright, hazy hour is not one)
    l1c: 'It counts hours when the light reaches the clear-sky level; bright haze does not count.',
    l1s: 'Sun heights next to a learned one start from its value ({b}° band).',
    l1u: '“Sunny.”, “Sunny spells.” and “No sun.” come from it at the learned sun heights.',
    l1b: 'Trees or buildings hide the sun from the sensor in {shade} of the {seen} sky cells seen so far.',
    l1b0: 'No shade found in the {seen} sky cells seen so far.', l1bd: 'No sky cells seen yet: the map below fills on clear days.',
    l2: 'This sensor shows about {s} in saturated air.',
    l2n: 'Clear nights here top out at about {v}, too low to take as saturated air.',
    l2u: '“Mist or fog possible.” takes {s} as saturated air on calm nights.',
    l3: 'The console reports wind in steps; its smallest reading above 0 is about {v}.',
    l4: 'Half of the hours here have a mean wind under {p50}; one hour in ten is above {p90}.',
    l5: 'After sunset, clear evenings here cool {c} °C an hour and cloudy ones {o} °C an hour.',
    l5u: '“Clear night.” needs a cooling of {t} °C an hour.',
    l5ins: 'Not learned: clear and cloudy evenings here are too alike to tell apart.',
    l5wait: 'It waits for the clear-sky level at evening sun heights, usually from spring.',
    l6: 'On a clear, calm 12-hour night that is 5 °C above the dew point in the evening, the air here cools about {d} °C.',
    l6u: '“Minimum around … tonight.” comes from it on clear nights.',
    l6miss: 'Its guesses still miss by about {sd} °C; {lim} °C is allowed.',
    l7ok: 'No large error found over {n} days.',
    // the notice's ending (baro_check, notices.cpp) and its 60-day wording
    l7hi: 'The pressure here averaged about {d} above what is usual here. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.',
    l7lo: 'The pressure here averaged about {d} below what is usual here. Set REL in the console’s SET menu to the current sea-level pressure of a nearby official station.',
    l7none: 'No usual pressure is known for this place, so there is no check.',
    l7met: 'Airport weather is on: its pressure check is used instead.',
    l7uncal: 'The console’s pressure is at its factory value; the check waits until REL is set.',
    // "Still learning" -- the same words in every block, the unit in the template
    s1: 'Still learning: about {k} more clear-sky hours.', s2: 'Still learning: about {k} more clear nights.',
    s3: 'Still learning: about {k} more readings of light wind.', s4: 'Still learning: about {k} more hours of wind.',
    s5: 'Still learning: about {c} more clear and {o} more cloudy evenings.', s6: 'Still learning: about {k} more clear calm nights.',
    s7: 'Checking: about {k} more days before a first check.',
    s1b: 'Fills on clear days.', s1bn: '{n} of 949 sky cells seen.',
    spaused: 'Paused until the bridge has a location.',
    // the meter: its bars, and the counts behind them (tooltip on hover, a toast on tap)
    b0: 'No bars.', b1: 'One bar of three.', b2: 'Two bars of three.', b3: 'Three bars of three.',
    tip1: 'One bar at {a} clear-sky hours at one sun height, two at {b}, three at {c}.',
    tip1b: 'One bar once the first sky cell is seen. The map is never done in one go: the sun’s path moves through the year.',
    tip2: 'One bar at {a} clear nights, two at {b}, three at {c}.',
    tip3: 'One bar at {a} readings of light wind, two at {b}, three at {c}.',
    tip4: 'One bar at {a} hours of wind, two at {b}, three at {c}.',
    tip5: 'One bar at {a} clear and {a} cloudy evenings, two at {b} of each, three at {c} of each.',
    tip6: 'One bar at {a} clear calm nights, two at {b}, three at {c}; in use from three bars with guesses within 2.0 °C.',
    tip7: 'A first check after {a} days, a finer one after {b} days.',
    sky: 'Sky around the sensor', sky0: 'No sky cells seen yet: the map fills on clear days.',
    skyi: 'Each cell is a patch of sky the sun crosses, by direction and height.',
    skya: 'Sky map: {sun} cells with sun, {shade} in shade, {unknown} not seen yet.',
    legend: 'Gold: the sun reached the sensor there. Grey: something blocked it. Outline: not seen yet; the sun’s path changes with the season.',
    rl: 'Restart learning', rls: 'Moved or replaced the sensor? Learning restarts from what it knows; the outlook uses the standard rules until each part is learned again.',
    rlq: 'Restart learning for {name}? Every part goes back to the standard rules until it is learned again.',
    rly: 'Restart', rln: 'Cancel', rlok: 'Learning restarts.', rlerr: 'No answer from fauxcloud.',
    w_indoor: 'Light sensor looks covered', w_indoor_s: 'Two dark hours in daylight. Is it indoors or covered? Clears with the next bright hour.',
    w_geo: 'Sunlight reported at night', w_geo_s: 'For 3 days now. Check the clock and the bridge location.',
    w_loc: 'No location set', w_loc_s: 'The sky learners wait for it (Location and time).',
    w_frozen: 'Temperature not changing', w_frozen_s: 'The same reading for 6 hours or more. Clears with the next change.',
    w_stuck: 'Humidity stuck at its top', w_stuck_s: 'More than 12 hours at its highest reading while it warmed in the sun.',
    sr_warn: 'Warning:', loading: 'Loading…', none: 'No learning state for this station.',
    rate: '{v} °C an hour', title: 'Learning', title_st: 'Learning · {name}', no_station: 'No station yet.',
  });
  const fmt = (t, v = {}) => t.replace(/\{(\w+)\}/g, (_, n) => esc(v[n]));
  const fill = (k, v) => fmt(W[k], v);
  const n1 = v => (Math.round(v * 10) / 10).toFixed(1);
  const km = v => n1(v) + ' km/h';
  const f2 = v => v.toFixed(2);
  const LV = ['default', 'low', 'medium', 'high'];
  // the meter is a button: hover shows its title, a tap shows the same text as a toast (4 s, longer than the default)
  const bars = (c, tip) => {
    const on = Math.max(0, LV.indexOf(c)), lbl = W['b' + on] + ' ' + tip;
    return `<button type="button" class="lv" title="${esc(tip)}" aria-label="${esc(lbl)}" data-tip="${esc(tip)}">${[1, 2, 3].map(i =>
      `<i${on >= i ? ' class="on"' : ''}></i>`).join('')}</button>`;
  };
  const tipOf = (k, b) => fill(k, { a: b[0], b: b[1], c: b[2] });
  const blk = (ic, t, v, c, tip, s, st, warn) => `<div class="lb${warn ? ' w' : ''}">${icon(ic)}<b>${warn ? `<span class="sr">${W.sr_warn} </span>` : ''}${t}${v ? ` <span>· ${v}</span>` : ''}</b>${c == null ? '<span></span>' : bars(c, tip)}<p>${s}</p>${st ? `<p class="nt">${st}</p>` : ''}</div>`;
  const join = (...a) => a.filter(Boolean).join(' ');
  // the value line by the firmware's state: default -> "Default …", learned / in use / info -> "Measured …"
  const val = (state, v, dk) => state === 'default' ? fill(dk || 'def', { v }) : fill('meas', { v });
  // the mask: 2 bits per cell (0 unknown, 1 sun, 2 shade), rows as the firmware's l1bCell; unknown cells as an outline
  const ROWS = [[3, 21, 2, 72], [21, 41, 4, 36], [41, 61, 5, 24], [61, 81, 10, 12], [81, 90, 9, 1]];
  function sky(b64, d) {
    const raw = atob(b64 || ''), H = 90;
    let i = 0, g = '';
    for (const [lo, hi, dh, cols] of ROWS)
      for (let el = lo; el < hi; el += dh)
        for (let c = 0; c < cols; c++, i++) {
          const k = (raw.charCodeAt(i >> 2) >> (2 * (i & 3))) & 3, x = c * 360 / cols, w = 360 / cols, y = H - el - dh;
          g += k ? `<rect x="${x}" y="${y}" width="${w}" height="${dh}" fill="${k === 1 ? 'var(--sunc)' : '#9fb0d8'}"${k === 1 ? ' opacity=".75"' : ''}/>`
            : `<rect x="${x + .3}" y="${y + .3}" width="${w - .6}" height="${dh - .6}" fill="none" stroke="var(--ink-3)" stroke-opacity=".25" stroke-width=".4"/>`;
        }
    return `<p class="hint" style="margin-bottom:6px">${W.skyi}</p><svg viewBox="0 0 360 ${H}" style="width:100%;height:auto;display:block" role="img" aria-label="${fill('skya', d)}">${g}
      ${[90, 180, 270].map(a => `<line x1="${a}" x2="${a}" y1="0" y2="${H}" stroke="var(--ink-3)" stroke-opacity=".5" stroke-width=".5"/>`).join('')}
      ${['N', 'E', 'S', 'W'].map((t, j) => `<text x="${90 * j + 3}" y="9" font-size="8" fill="var(--ink-2)">${t}</text>`).join('')}<text x="357" y="9" font-size="8" text-anchor="end" fill="var(--ink-2)">N</text></svg>
      <p class="hint" style="margin-top:6px">${W.legend}</p>`;
  }
  let ask = false;  // the restart's confirm step is open
  function body(d, name) {
    if (!d) return `<p class="hint loading">${W.loading}</p>`;
    if (d.err) return `<p class="hint">${W.none}</p>`;
    const paused = d.why === 'no_location', B = d.bars || {};
    const st = d.state === 'backfill' ? fill('backfill', { n: d.backfill.hours }) : paused ? fill('paused') : d.why === 'no_store' ? fill('nostore')
      : d.why === 'blob_newer' ? fill('ro') : fill('status');
    // warnings: only while a state the user can act on is on now (weather pauses such as a frosted sensor never show)
    const gs = d.gate_state || {}, warn = [];
    if (paused) warn.push(['sun', 'w_loc']);
    if (gs.indoor) warn.push(['sun', 'w_indoor']);
    if (gs.geo_days >= 3) warn.push(['sun', 'w_geo']);
    if (gs.frozen_run >= 6) warn.push(['thermometer', 'w_frozen']);
    if (d.l2.stuck) warn.push(['droplet', 'w_stuck']);
    const p = s => paused ? W.spaused : s;
    // L1: the measured sun heights; the value of the highest one (the noon cell), else the default
    const cells = d.l1.cells, on = cells.filter(c => c.state !== 'default'), use1 = on.some(c => c.state === 'in_use');
    const top = on.reduce((a, c) => !a || c.el > a.el || (c.el === a.el && c.n > a.n) ? c : a, null);
    const best = cells.reduce((a, c) => LV.indexOf(c.conf) > LV.indexOf(a.conf) ? c : a, cells[0]);
    const next1 = cells.reduce((a, c) => c.state === 'default' || c.state === 'learned' ? Math.min(a, c.need) : a, Infinity);
    const seed = cells.find(c => c.from != null);
    const v1 = Math.round(100 * (top ? top.e : d.l1.def)) + ' %';
    const b = d.l1b, seen = b.sun + b.shade;
    // L2: S from its source; the night count leads the "Still learning" line
    const l2 = d.l2, s2 = n1(l2.s) + ' %', n2 = l2.src === 'wet' ? l2.wet : l2.night;
    const l3 = d.l3, l4 = d.l4, l5 = d.l5, l6 = d.l6, l7 = d.l7;
    const ins = l5.src === 'inseparable', v5 = { c: f2(l5.clear.e), o: f2(l5.overcast.e), t: f2(l5.thr) };
    const l5c = LV[Math.min(LV.indexOf(l5.clear.conf), LV.indexOf(l5.overcast.conf))];
    const pmCell = [1, 3].some(i => cells[i] && (cells[i].conf === 'medium' || cells[i].conf === 'high'));
    const dd = n1(l6.a + 5 * l6.b + 12 * l6.c), sd6 = l6.sd == null ? null : n1(l6.sd);
    const off5 = l7.off == null ? 0 : Math.round(Math.abs(l7.off) / 5) * 5;
    const rows = [
      blk('sun', W.t1, val(on.length ? 'learned' : 'default', v1), best.conf, tipOf('tip1', B.l1 || []),
        join(on.length ? fill('l1', { k: on.length }) : '', W.l1c, seed ? fill('l1s', { b: seed.from }) : '', use1 ? W.inuse + ' ' + W.l1u : ''),
        p(next1 < Infinity && !use1 ? fill('s1', { k: next1 }) : '')),
      blk('house', W.t1b, '', seen ? 'low' : 'default', W.tip1b,
        !seen ? W.l1bd : fill(b.shade ? 'l1b' : 'l1b0', { shade: b.shade, seen }), p(seen ? fill('s1bn', { n: seen }) : W.s1b)),
      blk('droplet', W.t2, val(l2.state, l2.state === 'default' ? '100 %' : s2), l2.state === 'default' ? 'default' : n2.conf, tipOf('tip2', B.l2_night || []),
        l2.state === 'in_use' ? join(fill('l2', { s: s2 }), W.inuse, fill('l2u', { s: s2 })) : l2.state === 'learned' ? fill('l2', { s: s2 })
          : l2.night.state !== 'default' && !l2.stuck ? fill('l2n', { v: n1(l2.night.e) + ' %' }) : '',
        l2.state === 'in_use' ? '' : fill('s2', { k: l2.night.need })),
      blk('wind', W.t3, val(l3.state, km(l3.e)), l3.conf, tipOf('tip3', B.l3 || []),
        l3.state === 'default' ? '' : join(fill('l3', { v: km(l3.e) }), W.info), l3.state === 'default' ? fill('s3', { k: l3.need }) : ''),
      blk('wind', W.t4, val(l4.state, km(l4.p90)), l4.conf, tipOf('tip4', B.l4 || []),
        l4.state === 'default' ? '' : join(fill('l4', { p50: km(l4.p50), p90: km(l4.p90) }), W.info), l4.state === 'default' ? fill('s4', { k: l4.need }) : ''),
      blk('thermometer', W.t5, l5.state === 'default' || ins ? fill('std', { v: FC.t(W.rate, { v: '1.00' }) }) : fill('meas', { v: FC.t(W.rate, { v: f2(l5.thr) }) }), l5c,
        tipOf('tip5', B.l5 || []), ins ? W.l5ins : l5.state === 'default' ? (pmCell ? '' : W.l5wait) : join(fill('l5', v5), l5.state === 'in_use' ? W.inuse + ' ' + fill('l5u', v5) : ''),
        p(ins || l5.state === 'in_use' ? '' : fill('s5', { c: l5.clear.need, o: l5.overcast.need }))),
      blk('haze', W.t6, l6.state === 'default' ? W.nl : fill('meas', { v: dd + ' °C' + (sd6 == null ? '' : ' ± ' + sd6 + ' °C') }), l6.conf, tipOf('tip6', B.l6 || []),
        join(l6.state === 'default' ? '' : fill('l6', { d: dd }), l6.state === 'in_use' ? W.inuse + ' ' + W.l6u : '',
          sd6 != null && l6.n >= (B.l6 || [18])[0] && l6.state !== 'in_use' && l6.sd > (l6.learned ? 2 : 2.5) ? fill('l6miss', { sd: sd6, lim: l6.learned ? '2.0' : '2.5' }) : ''),
        p(l6.state === 'in_use' ? '' : fill('s6', { k: l6.need }))),
      blk('gauge', W.t7, '', ['default', 'low', 'high'][l7.stage] || 'default', tipOf('tip7', B.l7 || []),
        l7.norm == null ? W.l7none : d.use && d.use.metar ? W.l7met : l7.uncal ? W.l7uncal : !l7.stage ? '' : l7.flag === 'high' ? fill('l7hi', { d: off5 + ' hPa' }) : l7.flag === 'low' ? fill('l7lo', { d: off5 + ' hPa' })
          : fill('l7ok', { n: l7.days > 60 ? 60 : l7.days }), l7.norm != null && !l7.stage ? fill('s7', { k: l7.need }) : ''),
    ];
    const saved = d.written ? fill('written', { ago: ago(Math.max(0, Math.round(Date.now() / 1000 - d.written))) }) : W.unsaved;
    const rl = d.why === 'blob_newer' ? '' : `<h3>${W.rl}</h3><p class="hint">${ask ? fill('rlq', { name }) : W.rls}</p><div class="row">${ask
      ? `<button type="button" class="btn" data-rl="go">${W.rly}</button><button type="button" class="btn quiet" data-rl="no">${W.rln}</button>`
      : `<button type="button" class="btn quiet" data-rl="ask">${W.rl}</button>`}</div>`;
    return `<p class="hint">${st} ${saved}</p>
      ${warn.map(([ic, k]) => blk(ic, W[k], '', null, '', W[k + '_s'], '', true)).join('')}${rows.join('')}
      <h3>${W.sky}</h3><div class="lsky">${seen ? sky(b.mask, b) : `<p class="hint">${W.sky0}</p>`}</div>${rl}`;
  }
  // the block and meter rules are in app.css (admin face): the Weather description sheet draws the same blocks
  const CSS = '#shBody>.hint:first-of-type{margin-bottom:12px}';
  // the meter's text as a toast for 4 s (toast() keeps 2.6 s; its timer is the admin's toastT)
  const tip = t => {
    toast(t);
    if (typeof toastT !== 'undefined') {
      clearTimeout(toastT);
      toastT = setTimeout(() => { const el = document.getElementById('toast'); if (el) el.hidden = true; }, 4000);
    }
  };
  SHEETS.learn = function () {
    if (typeof document !== 'undefined' && !document.getElementById('lcss')) document.head.insertAdjacentHTML('beforeend', `<style id="lcss">${CSS}</style>`);
    const d = {};
    let cur = API.stations.length ? API.stations[0].id : '';
    ask = false;
    const nm = id => { const x = API.stations.find(s => s.id === id); return x ? x.cfg.name || x.id : id; };
    const load = id => FC.get('/api/learn?station=' + encodeURIComponent(id), { etag: false }).then(r => {
      d[id] = r.status === 404 || r.data.error ? { err: 1 } : r.data; if (SH && SH.key === 'learn') draw(); }, () => { d[id] = { err: 1 }; if (SH && SH.key === 'learn') draw(); });
    API.stations.forEach(x => load(x.id));
    return { icon: icon('activity'), title: cur ? FC.t(W.title_st, { name: nm(cur) }) : W.title,
      body: () => !API.stations.length ? `<p class="hint">${W.no_station}</p>`
        : (API.stations.length > 1 ? `<div class="tabs" role="tablist" style="margin-bottom:12px">${API.stations.map(x =>
          `<button type="button" role="tab" data-st="${esc(x.id)}" aria-selected="${x.id === cur}">${esc(x.cfg.name || x.id)}</button>`).join('')}</div>` : '') + body(d[cur], nm(cur)),
      click(b) {
        if (b.dataset.tip) tip(b.dataset.tip);
        else if (b.dataset.st) { cur = b.dataset.st; ask = false; draw(); }
        else if (b.dataset.rl === 'ask' || b.dataset.rl === 'no') { ask = b.dataset.rl === 'ask'; draw(); }
        else if (b.dataset.rl === 'go') {
          b.disabled = true;
          api('POST', '/api/learn/relearn?station=' + encodeURIComponent(cur), {}).then(r => { if (!r || !r.ok) { b.disabled = false; toast(W.rlerr); return; }
            ask = false; toast(W.rlok); setTimeout(() => load(cur), 2000); draw(); },
            () => { b.disabled = false; toast(W.rlerr); });
        }
      } };
  };
  SHEETS.learn.body = body;  // the node render test
  SHEETS.learn.tip = tip;    // the node test of the 4-s toast
})();
