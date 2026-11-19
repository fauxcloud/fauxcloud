// outlook.js -- the strip's words and glyphs for the codes the firmware
// computes (src/forecast.cpp; /api/live station.outlook). Pure, no DOM, one global like FCT. Whole phrases with named
// placeholders; the firmware's HA text table says the same (test/tools/test_table_sync.py). An unknown code
// renders nothing (the table is append-only: a later METAR / sky source adds codes, an old page stays quiet).
// Glyphs: Lucide (ISC; notices: licenses.txt from THIRD_PARTY_LICENSES.md), 24 px, drawn in the strip's ink.
const FCO = (() => {
  const W = {
    dry: 'No rain.', night_clear: 'Clear night.', rain_light: 'Light rain.', rain_moderate: 'Moderate rain.',
    rain_heavy: 'Heavy rain.', sleet_snow: 'Sleet or snow.', recent_rain: 'Recent rain.', day_sunny: 'Sunny.', day_spells: 'Sunny spells.', day_nosun: 'No sun.',
    p_steady: 'Pressure steady.', p_rising: 'Pressure rising.', p_rising_fast: 'Pressure rising fast.',
    p_falling: 'Pressure falling: less settled weather possible.',
    p_falling_fast: 'Pressure falling fast: unsettled spell possible.', cooling_fast: 'Pressure steady, cooling fast.',
    collecting: 'Pressure outlook in {left_h} h.', p_paused: 'Pressure outlook paused (calibration or clock).',
    min_range: 'Minimum around {range} tonight.', fog_possible: 'Mist or fog possible.',
    icy: 'Icy surfaces possible.', gusts: 'Gusts to {gust}.', hoar_frost: 'Hoar frost likely on cold surfaces.',
    ground_frost: 'Ground frost possible.', severe_frost: 'Severe frost.', freezing: 'Freezing.', hot: 'Hot.',
    windy: 'Windy.', humid: 'Humid.', calm: 'Calm.',
    time: 'Outlook waits for a valid clock.',
    // the main airport's report, scoped "reported nearby" (the source: outlook.in.near, the ⓘ)
    wx_snow_near: 'Snow reported nearby.', wx_rain_near: 'Rain reported nearby.', wx_drizzle_near: 'Drizzle reported nearby.',
    thunder_near: 'Thunder reported nearby.', fog_near: 'Fog reported nearby.', mist_near: 'Mist reported nearby.',
  };
  const CS = '<path d="M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242"/>';  // the shared cloud
  const G = {
    'cloud-sun': '<path d="M12 2v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="M20 12h2"/><path d="m19.07 4.93-1.41 1.41"/><path d="M15.947 12.65a4 4 0 0 0-5.925-4.128"/><path d="M13 22H7a5 5 0 1 1 4.9-6H13a3 3 0 0 1 0 6Z"/>',
    'cloud-moon': '<path d="M10.188 8.5A6 6 0 0 1 16 4a1 1 0 0 0 6 6 6 6 0 0 1-3 5.197"/><path d="M13 16a3 3 0 1 1 0 6H7a5 5 0 1 1 4.9-6Z"/>',
    moon: '<path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"/>',
    'cloud-drizzle': CS + '<path d="M8 19v1"/><path d="M8 14v1"/><path d="M16 19v1"/><path d="M16 14v1"/><path d="M12 21v1"/><path d="M12 16v1"/>',
    'cloud-rain': CS + '<path d="M16 14v6"/><path d="M8 14v6"/><path d="M12 16v6"/>',
    'cloud-rain-wind': CS + '<path d="m9.2 22 3-7"/><path d="m9 13-3 7"/><path d="m17 13-3 7"/>',
    'thermometer-snowflake': '<path d="M2 12h10"/><path d="M9 4v16"/><path d="m3 9 3 3-3 3"/><path d="M12 6 9 9 6 6"/><path d="m6 18 3-3 1.5 1.5"/><path d="M20 4v10.54a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z"/>',
    snowflake: '<path d="m10 20-1.25-2.5L6 18"/><path d="M10 4 8.75 6.5 6 6"/><path d="m14 20 1.25-2.5L18 18"/><path d="m14 4 1.25 2.5L18 6"/><path d="m17 21-3-6h-4"/><path d="m17 3-3 6 1.5 3"/><path d="M2 12h6.5L10 9"/><path d="m20 10-1.5 2 1.5 2"/><path d="M22 12h-6.5L14 15"/><path d="m4 10 1.5 2L4 14"/><path d="m7 21 3-6-1.5-3"/><path d="m7 3 3 6h4"/>',
    wind: '<path d="M12.8 19.6A2 2 0 1 0 14 16H2"/><path d="M17.5 8a2.5 2.5 0 1 1 2 4H2"/><path d="M9.8 4.4A2 2 0 1 1 11 8H2"/>',
    info: '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/><path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="m19.07 4.93-1.41 1.41"/>',
    cloud: '<path d="M17.5 19H9a7 7 0 1 1 6.71-9h1.79a4.5 4.5 0 1 1 0 9Z"/>',
    // the tile notice (Lucide circle-alert)
    'circle-alert': '<circle cx="12" cy="12" r="10"/><path d="M12 8v4"/><path d="M12 16h.01"/>',
    // shared with the airport view (FCO.G): fog / mist and thunder, fog_possible too
    'cloud-fog': CS + '<path d="M16 17H7"/><path d="M17 21H9"/>',
    'cloud-lightning': '<path d="M6 16.326A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 .5 8.973"/><path d="m13 12-3 5h4l-3 5"/>',
  };
  // the state's glyph: the rain classes and the moon (a clear night) say what a rule observes; every other state (no
  // rain, recent rain, none) takes the neutral sign, cloud-sun by day and cloud-moon at night, drawn dimmed so a full-ink
  // glyph always means a claim.
  // A T3 state is a sourced claim: full ink, the airport view's family. The day sky words (L1 in use) are
  // claims too: sun, cloud-sun, cloud in full ink.
  const SG = { night_clear: 'moon', rain_light: 'cloud-drizzle', rain_moderate: 'cloud-rain', rain_heavy: 'cloud-rain-wind', sleet_snow: 'snowflake',
    wx_snow_near: 'snowflake', wx_rain_near: 'cloud-rain', wx_drizzle_near: 'cloud-drizzle', day_sunny: 'sun', day_spells: 'cloud-sun',
    day_nosun: 'cloud' };
  const STATUS = /^(collecting|p_paused)$/;
  const WG = { icy: 'snowflake', gusts: 'wind', hoar_frost: 'thermometer-snowflake', ground_frost: 'thermometer-snowflake',
    thunder_near: 'cloud-lightning', fog_near: 'cloud-fog', mist_near: 'cloud-fog', fog_possible: 'cloud-fog' };
  // no qualifier after a T3 state (the firmware sends none)
  const NOQ = /_near$/;
  const svg = (n, c) => G[n] ? `<svg${c ? ` class="${c}"` : ''} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${G[n]}</svg>` : '';
  // one phrase with its placeholders filled; fmt.gust converts km/h, fmt.range a °C range for the page's units.
  // Unknown code -> null.
  const mc = v => String(v).replace('-', '−');
  function phrase(code, o, fmt) {
    const t = code && W[code];
    if (!t) return null;
    const p = o.p || {};
    return t.replace('{left_h}', o.left_h).replace('{gust}', fmt && fmt.gust ? fmt.gust(p.gust) : p.gust + ' km/h')
      .replace('{range}', () => fmt && fmt.range ? fmt.range(p.min_lo, p.min_hi) : mc(p.min_lo) + ' to ' + mc(p.min_hi) + ' °C');
  }
  // -> { glyph, l1, l2, warn, wglyph } or null (absent, all-null, gated on time, or a code this page does not know)
  function lines(o, fmt) {
    if (!o || o.gate === 'time') return null;
    // the pressure line's status codes (collecting, paused) say nothing about the weather: not on the strip, only in
    // the ⓘ; the code must still be known
    const l1 = o.state ? phrase(o.state, o, fmt) : '', nx = o.next ? phrase(o.next, o, fmt) : '', w = o.warn ? phrase(o.warn, o, fmt) : '';
    const l2 = nx && STATUS.test(o.next) ? '' : nx;
    if (l1 === null || l2 === null || w === null || (!l1 && !l2 && !w)) return null;
    // line 1 = the state + the qualifier, two whole phrases (as the HA sentence joins them)
    const q = o.qual && !NOQ.test(o.state || '') ? phrase(o.qual, o, fmt) : '';
    if (q === null) return null;
    const g = o.state && SG[o.state];
    return { glyph: g ? svg(g) : svg(o.in && o.in.night === true ? 'cloud-moon' : 'cloud-sun', 'dim'), l1: [l1, q].filter(Boolean).join(' '), l2, warn: w,
      wglyph: o.warn ? svg(WG[o.warn]) : '' };
  }
  return { lines, phrase, svg, W, G, SG, WG };
})();
