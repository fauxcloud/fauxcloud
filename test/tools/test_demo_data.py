"""tools/demo_data.py and demo/fixtures (the /try/ demo): the hourly series rendered by the documented row rule against
the firmware's writer (web_render.cpp) and against the daily ring (each day = its hours, history.cpp's aggregation), the
numbers' consistency, the year's climate, the learner snapshots (learn.h's bars, states, use flags), index.json, the
replay-written snapshots and live frames (within their hour's lo / hi, times monotonic, age_s), determinism (two runs
give the same bytes, the committed fixtures are a fresh run), the time-value shapes and no foreign data.
The snapshots under snap/ are demo/replay.js's (test/tools/demo_vectors.js checks they are a fresh run).
Run: python -I -m unittest discover -s test/tools"""
import base64
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
GEN = ROOT / 'tools' / 'demo_data.py'
FIX = ROOT / 'demo' / 'fixtures'
spec = importlib.util.spec_from_file_location('demo_data', GEN)
dd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dd)
API = (ROOT / 'docs' / 'api.md').read_text(encoding='utf-8')
RENDER = (ROOT / 'firmware' / 'fauxcloud' / 'src' / 'web_render.cpp').read_text(encoding='utf-8')
LEARN_H = (ROOT / 'firmware' / 'fauxcloud' / 'src' / 'learn.h').read_text(encoding='utf-8')
ISO = re.compile(r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
HKEY = re.compile(r'^\d{4}-\d\d-\d\dT\d\d:00Z$')
DKEY = re.compile(r'^\d{4}-\d\d-\d\d$')


def files(d, snap=False):
    return {p.relative_to(d).as_posix(): p.read_bytes() for p in sorted(d.rglob('*.json')) if snap or 'snap' not in p.relative_to(d).parts}


def walk(o, path=''):
    if isinstance(o, dict):
        for k, v in o.items():
            yield from walk(v, path + '.' + k)
    elif isinstance(o, list):
        for v in o:
            yield from walk(v, path + '[]')
    else:
        yield path, o


def table_keys(heading):
    i = API.index(heading)
    keys, seen = [], False
    for line in API[i:].splitlines()[1:]:
        m = re.match(r'^\| `([a-z_0-9]+)`(?:(?:, | / )`([a-z_0-9]+)`)? \|', line)
        if m:
            keys += [k for k in m.groups() if k]
            seen = True
        elif seen and not line.startswith('|'):
            break
    return keys


def writer_keys(fn_from, fn_to):
    body = RENDER[RENDER.index(fn_from):RENDER.index(fn_to)]
    return set(re.findall(r'\\"([a-z_0-9]+)\\"', body)) | set(re.findall(r'(?:triple|one)\(o, "([a-z_0-9]+)"', body)) | {'q', 'wind_dir', 'wind_steady'}


def row_of(S, i):
    """The documented row rule (demo/README.md): one /api/hourly record from the series' column i (None for a lost hour)."""
    c = {k: v[i] for k, v in S['cols'].items()}
    if not c['n']:
        return None
    return {'h': dd.hkey(dd.ts(S['from'][:16]) + i * 3600), 'n': c['n'], 'q': [f for b, f in enumerate(dd.Q_ORDER) if c['q'] >> b & 1], 'epoch': 0,
            'rej_fields': c['q'] >> 8 & 1, 'temp_c': [c['tlo'], c['tav'], c['thi']], 'dewpt_c': [None, c['td'], None],
            'humidity': [c['rhlo'], c['rhav'], c['rhhi']], 'pressure_hpa': [None, c['p'], None], 'wind_kmh': [None, c['w'], None],
            'gust_kmh': c['g'], 'wind_dir': c['dir'], 'wind_steady': c['st'], 'calm_n': c['calm'], 'rain_mm': c['rain'], 'rain_daily_mm': None,
            'rain_rate_max': c['rmax'], 'uv': [None, None, c['uv']], 'solar_wm2': [None, c['sol'], c['solx']]}


def secs(s):
    return dd.ts(s[:16]) + int(s[17:19])


class Fixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        a, b = Path(cls.tmp.name) / 'a', Path(cls.tmp.name) / 'b'
        for d in (a, b):
            subprocess.run([sys.executable, '-I', str(GEN), '--out', str(d)], check=True, capture_output=True)
        cls.A, cls.B = files(a), files(b)
        cls.J = {k: json.loads(v) for k, v in files(FIX, snap=True).items()}
        cls.I, cls.S = cls.J['index.json'], cls.J['history/series.json']
        cls.D = cls.J['history/daily.json']['days']
        cls.rows = [row_of(cls.S, i) for i in range(cls.S['hours'])]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def snaps(self):
        out = [(e['id'] + '-' + k, e[k], e[k + '_t0'], {}) for e in self.I['example_days'] for k in ('day', 'night')]
        return out + [(k, s, s['t0'], s['switches']) for k, s in self.I['scenarios'].items()]

    # ---- determinism and size
    def test_two_runs_equal(self):
        self.assertEqual(sorted(self.A), sorted(self.B))
        for k in self.A:
            self.assertEqual(self.A[k], self.B[k], k)

    def test_committed_fixtures_are_a_fresh_run(self):
        got = {k: v.replace(b'\r\n', b'\n') for k, v in files(FIX).items()}
        self.assertEqual(sorted(got), sorted(self.A), 'run python -I tools/demo_data.py && node demo/replay.js --write')
        for k in self.A:
            self.assertEqual(got[k], self.A[k], k + ': run python -I tools/demo_data.py && node demo/replay.js --write')

    def test_size(self):
        self.assertLess(sum(p.stat().st_size for p in FIX.rglob('*.json')), 4_000_000)

    def test_generator_stdlib_only(self):
        mods = set(re.findall(r'^(?:import|from) ([a-z_0-9]+)', GEN.read_text(encoding='utf-8'), re.M))
        self.assertLessEqual(mods, {'base64', 'bisect', 'json', 'math', 'random', 'sys', 'datetime', 'pathlib'})

    # ---- the series and the daily ring
    def test_counts_and_reach(self):
        self.assertEqual(len(self.D), 736)
        self.assertEqual(self.S['hours'], len(self.S['cols']['n']))
        self.assertEqual(set(self.S['cols']), set(dd.COLS))
        self.assertTrue(all(len(v) == self.S['hours'] for v in self.S['cols'].values()))
        first, start = dd.ts(self.I['series']['first_t0'][:16]), dd.ts(self.S['from'][:16])
        self.assertGreaterEqual((dd.T0 - first) / 86400, 365)
        self.assertGreaterEqual((first - start) / 86400, 7)          # the 7-day sheet's reach before any t0
        self.assertGreater(start + self.S['hours'] * 3600, dd.T0 + 86400)  # tomorrow, for WU and the TAF
        self.assertEqual(self.D[-1]['d'], dd.dkey(dd.midnight(dd.T0) - 3600))
        ds = [x['d'] for x in self.D]
        self.assertEqual(ds, sorted(set(ds)))

    def test_rows_match_the_writer(self):
        hk, dk = writer_keys('void hourRow(', 'void dayRow('), writer_keys('void dayRow(', 'void renderHourly(')
        for x in self.rows:
            if x:
                self.assertEqual(set(x), hk)
        for x in self.D:
            self.assertEqual(set(x), dk)

    def test_days_are_their_hours(self):
        by = {}
        for x in self.rows:
            if x:
                by.setdefault(dd.dkey(dd.ts(x['h'][:16])), []).append(x)
        full = [d for d in self.D if d['d'] in by and d['d'] > self.S['from'][:10]]
        self.assertGreater(len(full), 360)
        for d in full:
            self.assertEqual(dd.day_row(d['d'], by[d['d']]), d, d['d'])

    def test_outage_rejected_and_new_console(self):
        hs = {x['h']: x for x in self.rows if x}
        for t in dd.OUTAGE:
            self.assertNotIn(dd.hkey(t), hs)
        self.assertEqual(hs[dd.hkey(dd.OUTAGE[0] - 3600)]['q'], ['gap'])
        self.assertEqual(hs[dd.hkey(dd.OUTAGE[-1] + 3600)]['q'], ['restart', 'gap'])
        self.assertEqual([x['h'] for x in self.rows if x and x['rej_fields']], [dd.hkey(dd.REJECTED_HOUR)])
        day = next(d for d in self.D if d['d'] == dd.dkey(dd.OUTAGE[0]))
        self.assertEqual((day['hours'], day['clean_hours']), (21, 19))
        self.assertEqual(self.D[0]['q'], ['baro_uncal'])

    def test_consistency(self):
        for x in [r for r in self.rows if r] + self.D:
            k = x.get('h') or x['d']
            self.assertLessEqual(x['dewpt_c'][1], x['temp_c'][1], k)
            self.assertTrue(x['temp_c'][0] <= x['temp_c'][1] <= x['temp_c'][2], k)
            self.assertTrue(x['humidity'][0] <= x['humidity'][1] <= x['humidity'][2] <= 99, k)
            self.assertGreaterEqual(x['gust_kmh'], x['wind_kmh'][1], k)
            self.assertEqual(x['wind_dir'] is None, x['wind_steady'] is None, k)
            self.assertEqual(x['rain_mm'] > 0, x['rain_rate_max'] > 0, k)
        for x in self.rows:
            if x:
                self.assertGreaterEqual(x['rain_rate_max'], x['rain_mm'])
                self.assertAlmostEqual(x['rain_mm'] / dd.TIP, round(x['rain_mm'] / dd.TIP), delta=0.03)
        for x in self.D:
            self.assertEqual(sum(x['rose']) + x['calm_hours'], x['hours'])

    def test_four_seasons(self):
        yr = self.D[-365:]
        mon = {}
        for d in yr:
            mon.setdefault(d['d'][5:7], []).append(d)
        mean = lambda m: sum(d['temp_c'][1] for d in mon[m]) / len(mon[m])
        self.assertTrue(-6 < mean('01') < 1 and 16 < mean('07') < 22, (mean('01'), mean('07')))  # Warsaw: about -2 and 19 °C
        self.assertTrue(450 < sum(d['rain_mm'] for d in yr) < 800)
        self.assertGreater(sum(1 for d in yr if d['temp_c'][2] >= 30), 3)        # the heat wave
        self.assertGreater(sum(1 for d in yr if d['temp_c'][0] < 0), 60)         # frost
        self.assertTrue(any(d['rain_rate_max'] > 7.6 for d in mon['08']))       # the thunderstorm
        self.assertLess(sum(d['rain_mm'] for d in mon['01']), 40)                # dry snow waits in the unheated gauge
        self.assertTrue(10 < next(d for d in yr if d['d'] == '2026-01-21')['rain_mm'] < 25)  # the wet snow melts in as it falls
        rel = [d['pressure_hpa'][1] for d in yr if '2026-02-10' <= d['d'] <= '2026-03-25']
        self.assertGreater(sum(rel) / len(rel), 1025)  # the console's REL mis-set by 22 hPa for two months

    # ---- the learners
    def test_learn_snapshots(self):
        bars = {k: [int(x) for x in v] for k, v in zip(['l1', 'l2_wet', 'l2_night', 'l3', 'l4', 'l5', 'l6', 'l7'],
                re.findall(r'\{LBR_\w+, (\d+), (\d+), (\d+)\}', LEARN_H))}
        doc = set(table_keys('#### 3.2.4')) - {'gates', 'need', 'hit'}  # gates: bench build only; need, hit: inside each block
        times = [dd.ts(e['t'][:16]) for e in self.I['learn']]
        self.assertEqual(times, sorted(times))
        for e in self.I['learn']:
            L = self.J[e['file']]
            self.assertLessEqual(doc, set(L), e['t'])
            self.assertEqual(L['bars'], bars)
            for c in L['l1']['cells']:
                lv = dd.CONF.index(c['conf'])
                self.assertEqual(c['state'], 'default' if not lv else 'in_use' if lv >= 2 else 'learned')
                self.assertEqual(c['need'] == 0, lv == 3)
                self.assertGreaterEqual(c['need'], 0)
            self.assertEqual(L['use']['l1'], any(c['state'] == 'in_use' for c in L['l1']['cells']))
            self.assertEqual(L['use']['l6'], L['l6']['state'] == 'in_use')
            self.assertEqual(L['use']['l5'], L['l5']['state'] == 'in_use')
            self.assertEqual(L['use']['l2'], L['l2']['state'] == 'in_use')
            b = L['l1b']
            self.assertEqual(b['sun'] + b['shade'] + b['unknown'], 949)
            m = base64.b64decode(b['mask'])
            cls = [(m[i >> 2] >> (2 * (i & 3))) & 3 for i in range(949)]
            self.assertEqual((cls.count(1), cls.count(2)), (b['sun'], b['shade']))
            d = e['t'][:10]  # L7 flags the REL error from late March and lets go after the fix, below 9 hPa
            if '2026-03-31' <= d <= '2026-04-15' or not '2026-03-01' <= d <= '2026-05-15':
                self.assertEqual(L['l7']['flag'], 'high' if '2026-03-31' <= d <= '2026-04-15' else 'none', e['t'])
        last = self.J[self.I['learn'][-1]['file']]
        self.assertLess(last['l1']['cells'][1]['n'], last['l1']['cells'][0]['n'])  # 5 deg, afternoon: mostly behind the roof
        self.assertTrue(all(last['use'][k] for k in ('l1', 'l2', 'l5', 'l6')))     # a year-old station: 4 of 4 in use
        self.assertGreater(last['l1b']['shade'], 5)

    # ---- index.json and the replay-written snapshots
    def test_index(self):
        I = self.I
        self.assertEqual([e['id'] for e in I['example_days']], ['winter', 'spring', 'summer', 'autumn'])
        lo = dd.ts(I['series']['first_t0'][:16])
        for k, o, t, sw in self.snaps():
            self.assertTrue(lo <= secs(t) <= dd.T0, k)
            self.assertIn(o['live'], self.J)
            self.assertIn(o['frames'], self.J)
        for p, f in I['static'].items():
            self.assertIn(f, self.J, p)
        self.assertEqual(set(I['scenarios']), {'today', 'sunny', 'rain', 'snow', 'thunderstorm', 'frost-night', 'pressure-off', 'offline', 'no-sources'})
        self.assertIs(I['demo'], True)
        for k, v in self.J.items():
            if k.startswith('admin/') and isinstance(v, dict) and 'test' not in k:
                self.assertIs(v.get('demo'), True, k)

    def test_snapshots(self):
        doc = set(table_keys('`station` object (keys in the order sent):'))
        want = {'winter-day': ('sleet_snow', 'freezing', 'p_steady', 'icy'), 'spring-day': ('day_sunny', None, 'p_steady', None),
                'summer-day': ('day_sunny', 'hot', 'p_steady', None), 'autumn-day': ('dry', None, 'p_falling', None),
                'winter-night': ('night_clear', 'freezing', 'p_steady', 'icy'), 'spring-night': ('night_clear', 'calm', 'p_falling', 'ground_frost'),
                'summer-night': ('night_clear', None, 'min_range', None), 'autumn-night': ('rain_light', None, 'p_rising', None),
                'today': ('day_spells', None, 'p_falling', None), 'sunny': ('day_sunny', 'hot', 'p_steady', None),
                'rain': ('rain_moderate', 'windy', 'p_steady', 'gusts'), 'snow': ('sleet_snow', 'freezing', 'p_steady', 'icy'),
                'thunderstorm': ('rain_heavy', None, 'p_steady', 'thunder_near'), 'frost-night': ('night_clear', 'freezing', 'min_range', 'ground_frost'),
                'pressure-off': ('day_sunny', None, 'p_steady', None), 'offline': ('day_spells', None, 'p_falling', None),
                'no-sources': ('day_spells', None, 'p_falling', None)}
        for k, o, t, sw in self.snaps():
            L = self.J[o['live']]
            st = L['station']
            self.assertLessEqual(set(st), doc, k)
            ol = st['outlook']
            self.assertEqual((ol['state'], ol['qual'], ol['next'], ol['warn']), want[k], k)
            self.assertEqual(list(ol), table_keys('Key order as written:'))
            self.assertEqual(L['now'], t)
            self.assertEqual(st['age_s'], secs(L['now']) - secs(st['last_seen']), k)
            self.assertEqual(st['online'], st['age_s'] < 240, k)
            self.assertEqual('wufc' in st, sw.get('wufc') is not False, k)
            self.assertLessEqual(st['values']['dewpt_c'], st['values']['temp_c'])
            self.assertEqual([n['id'] for n in st.get('notices', [])], ['baro_check'] if k == 'pressure-off' else [], k)
            if sw.get('metar') is False:
                self.assertIsNone(ol['in']['near'])
        self.assertFalse(self.J[self.I['scenarios']['offline']['live']]['station']['online'])
        snow = self.J[self.I['scenarios']['snow']['live']]['station']['values']  # wet snow the gauge counts, at about 0 °C
        self.assertTrue(snow['rain_rate_mmh'] > 0 and snow['temp_c'] <= 1, snow)
        fn = self.J[self.I['scenarios']['frost-night']['live']]['station']  # at or below 0 °C, inside the minimum range shown
        self.assertTrue(fn['outlook']['p']['min_lo'] <= fn['values']['temp_c'] <= 0, fn['values'])

    def test_frames_within_their_hours(self):
        h0, C = dd.ts(self.S['from'][:16]), self.S['cols']
        for k, o, t, sw in self.snaps():
            F = self.J[o['frames']]
            T = F['cols']['t']
            self.assertGreaterEqual(F['n'], 120)
            self.assertEqual(len(T), F['n'])
            self.assertTrue(all(b - a == 65 for a, b in zip(T, T[1:])), k)
            for j, u in enumerate(T):
                i = (u - u % 3600 - h0) // 3600
                if not C['n'][i]:
                    continue
                v = {c: F['cols'][c][j] for c in F['cols']}
                self.assertTrue(C['tlo'][i] <= v['temp_c'] <= C['thi'][i], (k, u, 'temp'))
                self.assertTrue(C['rhlo'][i] <= v['humidity'] <= C['rhhi'][i], (k, u, 'rh'))
                self.assertLessEqual(v['dewpt_c'], v['temp_c'])
                self.assertTrue(v['wind_kmh'] <= v['gust_kmh'] <= C['g'][i], (k, u, 'wind'))
                self.assertLessEqual(v['solar_wm2'], C['solx'][i], (k, u))
                self.assertLessEqual(v['uv'], C['uv'][i], (k, u))
                self.assertLess(abs(v['pressure_hpa'] - C['p'][i]), 1.5, (k, u))
                self.assertLessEqual(v['rain_rate_mmh'], max(C['rmax'][i] or 0, C['rmax'][i - 1] or 0), (k, u))
            self.assertIn(secs(self.J[o['live']]['station']['last_seen']), T, k)

    # ---- time shapes and foreign data
    def test_every_date_has_a_listed_shape(self):
        datey = re.compile(r'\d{4}-\d\d-\d\d|\d{4}/\d\d/\d\d|\b\d\d:\d\d\b')
        lo, hi = dd.ts('2024-11-01T00:00'), dd.ts('2026-11-20T00:00')
        allowed = ('.last_hour', '.written', '.use.hour', '.use.night_end', '.night.last.start', '.cols.t[]')
        for f, o in self.J.items():
            for path, v in walk(o):
                if isinstance(v, str) and not (ISO.match(v) or HKEY.match(v) or DKEY.match(v) or path.endswith(('.text', '.line', '.src_mac', '.ua'))):
                    self.assertIsNone(datey.search(v), (f, path, v))
                if isinstance(v, (int, float)) and not isinstance(v, bool) and lo <= v <= hi:
                    self.assertTrue(path.endswith(allowed), (f, path))

    def test_no_foreign_data(self):
        blob = '\n'.join(v.decode('utf-8') for v in files(FIX, snap=True).values())
        self.assertIsNone(re.search(r'[\w.+-]+@[\w-]+\.[\w.]+', blob))
        self.assertEqual(set(re.findall(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', blob)) - {'192.168.1.5', '192.168.1.10', '192.168.1.20', '192.168.77.2'}, set())
        self.assertEqual(set(re.findall(r'\b[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}\b', blob)), {'02:00:00:00:00:01'})
        self.assertIsNone(re.search(r'[A-Za-z]:\\\\|/Users/|/home/', blob))
        self.assertEqual(set(re.findall(r'"icao":"(\w+)"', blob)), {'EPMO', 'EPWA'})
        self.assertEqual(set(re.findall(r'Europe/\w+', blob)), {'Europe/Warsaw'})
        for f, o in self.J.items():
            for path, v in walk(o):
                if path.endswith(('.lat', '.lon')):
                    self.assertIn(v, (52.23, 21.01, 52.2, 21.0), (f, path))
        self.assertEqual(set(re.findall(r'"(?:station|id)":"([A-Z0-9]+)"', blob)) - {'DEMO1', 'IDEMO1'}, set())


if __name__ == '__main__':
    unittest.main()
