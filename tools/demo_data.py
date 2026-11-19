#!/usr/bin/env python3
# tools/demo_data.py -- the /try/ demo's data: a synthetic two-year weather history for a made-up station "Garden" at
# lat 52.23, lon 21.01 (a generic city-centre point, CET/CEST), written as the JSON bodies the dashboard and the admin read
# (docs/api.md), plus live snapshots for a few scenarios. Seeded and stdlib only: two runs give the same bytes.
#
# The weather is simulated in 10-minute steps (pressure waves + fronts + the S2 tide, cloud tied to pressure, an annual
# and a daily temperature cycle, dew point from a spread, wind with a prevailing WSW direction veering with the
# pressure trend, rain bands at fronts, summer showers, snow that melts into the unheated gauge later, clear-sky sun
# by solar elevation). Hour rows are aggregated from the steps and day rows from the hour rows, the way the firmware
# does it (history.cpp), so every derived number agrees with its parts. See demo/README.md for the files and the
# time-shift contract. Usage: python -I tools/demo_data.py [--out demo/fixtures]
import base64
import bisect
import json
import math
import random
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

SEED = 20261116
GEN_VERSION = 2
LAT, LON = 52.23, 21.01  # Warsaw city centre (a generic point, no address)
TZ, TZ_NAME = 'CET-1CEST,M3.5.0,M10.5.0/3', 'Europe/Warsaw'
SID, NAME, FW = 'DEMO1', 'Garden', 'fauxcloud 0.1.0-demo'
DAYS = 736                 # the daily flash ring's reach (docs/api.md §3.2)
YEAR, LEAD = 365, 8         # the hourly series: a year of times to replay, plus the 7-day sheet's reach before it
STEP, TIP = 600, 0.225      # simulation step (s); one gauge tip (mm)
UTC = timezone.utc


def ts(s):
    return int(datetime.strptime(s, '%Y-%m-%dT%H:%M').replace(tzinfo=UTC).timestamp())


T0 = ts('2026-11-16T11:15')  # the moment the demo set describes ("now" of the main scenario), 12:15 local


# ------------------------------------------------------------------ time
def iso(t):
    return datetime.fromtimestamp(t, UTC).strftime('%Y-%m-%dT%H:%M:%SZ')


def hkey(t):
    return datetime.fromtimestamp(t, UTC).strftime('%Y-%m-%dT%H:%MZ')


def _last_sunday(y, m):  # 01:00 UTC on the last Sunday of the month: the EU switch (the TZ rule above)
    d = datetime(y, m, 31, 1, tzinfo=UTC)
    return int((d - timedelta(days=(d.weekday() + 1) % 7)).timestamp())


def off(t):
    y = datetime.fromtimestamp(t, UTC).year
    return 7200 if _last_sunday(y, 3) <= t < _last_sunday(y, 10) else 3600


def loc(t):  # local wall time as a naive datetime
    return datetime.fromtimestamp(t + off(t), UTC).replace(tzinfo=None)


def dkey(t):
    return loc(t).strftime('%Y-%m-%d')


def midnight(t):  # UTC of the local midnight that starts t's local day
    m = loc(t).replace(hour=0, minute=0, second=0)
    u = int(m.replace(tzinfo=UTC).timestamp())
    return u - off(u - 7200)


def lst(t):  # local mean solar time, hours
    return (t / 3600 + LON / 15) % 24


# ------------------------------------------------------------------ sun
def sun(t):
    """Elevation and azimuth (degrees) of the sun at the station."""
    d = (t - 946728000) / 86400
    g = math.radians(357.529 + 0.98560028 * d)
    lam = math.radians(280.459 + 0.98564736 * d + 1.915 * math.sin(g) + 0.020 * math.sin(2 * g))
    e = math.radians(23.439 - 3.6e-7 * d)
    ra = math.atan2(math.cos(e) * math.sin(lam), math.cos(lam))
    dec = math.asin(math.sin(e) * math.sin(lam))
    ha = math.radians((18.697374558 + 24.06570982441908 * d) % 24 * 15 + LON) - ra
    la = math.radians(LAT)
    el = math.asin(math.sin(la) * math.sin(dec) + math.cos(la) * math.cos(dec) * math.cos(ha))
    az = math.atan2(-math.sin(ha), math.tan(dec) * math.cos(la) - math.sin(la) * math.cos(ha))
    return math.degrees(el), math.degrees(az) % 360


def clear_sky(el):  # Haurwitz-like global irradiance, W/m²
    s = math.sin(math.radians(el))
    return 1098 * s * math.exp(-0.057 / s) if s > 0.01 else 0.0


def s2(t):  # the semidiurnal pressure tide (web/tendency.js), hPa
    return 1.16 * math.cos(math.radians(LAT)) ** 3 * math.cos(2 * math.pi * (t / 3600 + LON / 15 - 10) / 12)


def shaded(el, az):
    return 0 < el < 9 and 200 <= az <= 245 or 0 < el < 6 and 95 <= az <= 120


def bump(t, a, b, ramp=36 * 3600):  # 0 outside [a, b], 1 inside, smooth edges
    return min(1.0, max(0.0, (t - a + ramp) / ramp), max(0.0, (b + ramp - t) / ramp))


def magnus(t):
    return math.exp(17.625 * t / (243.04 + t))


def rnd(x, d):
    if x is None:
        return None
    v = round(x + (1e-9 if x >= 0 else -1e-9), d)
    return 0.0 if v == 0 else (int(v) if d == 0 else v)


# ------------------------------------------------------------------ scripted weather (the scenarios need these)
HEAT = (ts('2026-08-16T00:00'), ts('2026-08-23T18:00'))
COLD = [(ts('2026-11-08T12:00'), ts('2026-11-13T12:00'), 6.0), (ts('2026-01-17T00:00'), ts('2026-01-27T00:00'), 7.0)]
CALM = [(ts('2026-11-09T06:00'), ts('2026-11-10T09:00')), (ts('2026-08-21T00:00'), ts('2026-08-22T03:00')),
        (ts('2026-01-21T15:00'), ts('2026-01-23T09:00')), (ts('2026-04-14T00:00'), ts('2026-04-16T12:00'))]
FRONTS_SCRIPTED = [(ts('2026-10-08T13:30'), 2.6, 9), (ts('2026-11-12T03:00'), 1.6, 9), (T0 + 15 * 3600, 1.6, 8),
                   (ts('2026-08-24T02:00'), 1.4, 6), (ts('2026-01-21T03:00'), 1.3, 9)]
STORM = (ts('2026-08-22T17:20'), 3000, 32)  # the heat wave's evening thunderstorm
# wet snow at about 0 °C on the cold spell's front (start, end, °C, mm/h): it melts in the gauge as it falls, so the station's
# rain rate, the airport's snow, the sky's flakes and the tiles tell one story; 2.8 mm/h (12 tips an hour) keeps a tip in every 10 minutes
WETSNOW = (ts('2026-01-21T05:00'), ts('2026-01-21T10:30'), -0.4, 2.8)
REL_ERR = (ts('2026-01-30T10:00'), ts('2026-04-05T09:00'), 22.0)  # the console's REL mis-set after a battery change, fixed later
THUNDER_MMH = 15  # a shower this strong is a thunderstorm
OUTAGE = [ts('2026-10-21T00:00') + 3600 * k for k in range(3)]  # three hours with no row (a power cut at the bridge)
REJECTED_HOUR = ts('2026-09-03T14:00')                          # one reading rejected by a filter


class Sim:
    """The weather in 10-minute steps; every random draw comes from one seeded generator, in a fixed order."""

    def __init__(self, t_start, t_end):
        R = self.R = random.Random(SEED)
        self.ph = [R.uniform(0, 2 * math.pi) for _ in range(3)]
        quiet = [(T0 - 40 * 3600, T0 + 14 * 3600)] + [(a - 86400, b) for a, b in CALM] + [(HEAT[0], HEAT[1] - 12 * 3600)] + [(a, b) for a, b, _ in COLD]
        self.fronts, t = [], t_start - 3 * 86400
        while t < t_end + 3 * 86400:
            w = self.winter(t)
            t += int(R.expovariate(1 / ((3.4 + 1.8 * (1 - w)) * 86400)))
            s, dur = R.uniform(0.5, 1.5), R.uniform(3, 11)
            if not any(a - 86400 < t < b + 86400 for a, b in quiet) and all(abs(t - f) > 2 * 86400 for f, _, _ in FRONTS_SCRIPTED):
                self.fronts.append((t, s, dur))
        self.fronts += FRONTS_SCRIPTED
        self.fronts.sort()
        self.showers, d = [], midnight(t_start)
        while d < t_end + 86400:  # summer afternoon showers on warm days
            su = 1 - self.winter(d)
            if R.random() < 0.16 * su ** 2 and not any(a - 86400 < d < b for a, b in CALM):
                st = d + int(R.uniform(12, 17.5) * 3600)
                self.showers.append((st, R.uniform(2400, 5400), R.uniform(4, 26)))
            d += 86400
        self.showers.append(STORM)
        self.showers.sort()
        self.shs = [s[0] for s in self.showers]
        self.drizzle = []
        self.a = self.nc = self.m = self.dw = 0.0
        # one stream per process, a fixed number of draws per step: tuning one process never moves another
        self.Rc, self.Rp, self.Rt, self.Rh, self.Rw, self.Rs = (random.Random(SEED * 10 + k) for k in range(6))
        self.acc = self.snow = 0.0

    @staticmethod
    def winter(t):  # 1 in mid-January, 0 in mid-July
        doy = loc(t).timetuple().tm_yday
        return (1 + math.cos(2 * math.pi * (doy - 15) / 365.25)) / 2

    def p_syn(self, t):
        A = 6.5 + 4.5 * self.winter(t)
        P = (2.9, 4.7, 7.3)
        p = 1015.0 + A * sum(k * math.sin(2 * math.pi * t / (q * 86400) + f) for k, q, f in zip((0.55, 0.35, 0.25), P, self.ph))
        p -= sum(9 * min(s, 1.6) * math.exp(-((t - tf) / 50000) ** 2) for tf, s, _ in self.near(t))
        p += sum(7 * bump(t, a, b) for a, b in CALM)
        return p

    def near(self, t):
        if not hasattr(self, 'ft'):
            self.ft = [f[0] for f in self.fronts]
        return self.fronts[bisect.bisect_left(self.ft, t - 3 * 86400):bisect.bisect_right(self.ft, t + 3 * 86400)]

    def step(self, t):
        """One 10-minute step starting at t: a dict of the sensor values in it."""
        w, su, hl = self.winter(t), 1 - self.winter(t), lst(t + STEP / 2)
        c_n, c_u = self.Rc.gauss(0, 0.06), self.Rc.random()
        p_f, p_s3, p_u, p_d, p_r = self.Rp.gauss(0, 0.5), self.Rp.gauss(0, 0.3), self.Rp.random(), self.Rp.random(), self.Rp.random()
        t_a, t_n, t_lo, t_hi = (self.Rt.gauss(0, 1) for _ in range(4))
        h_n, h_u = self.Rh.gauss(0, 0.3), self.Rh.random()
        w_m, w_n, w_u1, w_u2, w_d = self.Rw.gauss(0, 0.09), self.Rw.gauss(0, 0.4), self.Rw.random(), self.Rw.random(), self.Rw.gauss(0, 6)
        s_u, s_n = self.Rs.random(), self.Rs.gauss(0, 0.06)
        fr = self.near(t)
        calm = max(bump(t, a, b, 6 * 3600) for a, b in CALM)
        hw = bump(t, *HEAT)
        cs = sum(k * bump(t, a, b) for a, b, k in COLD)
        p_s = self.p_syn(t)
        dp3 = p_s - self.p_syn(t - 3 * 3600)
        # cloud: low pressure is cloudy; a front brings a deck ahead of it
        self.nc = 0.97 * self.nc + c_n
        c = 0.62 + 0.12 * w - (p_s - 1015) / 16 + self.nc - 0.45 * hw - 0.5 * calm
        for tf, s, dur in fr:
            c = max(c, 0.97 * math.exp(-((t - (tf - 3 * 3600)) / ((dur + 6) * 1800)) ** 2))
        c = min(1.0, max(0.0, c))
        # precipitation rate (mm/h) at the gauge's height
        rate = 0.0
        for tf, s, dur in fr:
            a, b = tf - 2 * 3600, tf + dur * 3600
            if a <= t < b:
                rate += s * (0.42 + 0.9 * su) * math.sin(math.pi * (t - a) / (b - a)) * math.exp(p_f)
        gust_boost, thunder = 0.0, False
        for st, ln, pk in self.showers[max(0, bisect.bisect_right(self.shs, t) - 2):bisect.bisect_right(self.shs, t)]:
            if st <= t < st + ln:
                x = (t - st) / ln
                rate += pk * (1 - abs(2 * x - 1)) * math.exp(p_s3)
                gust_boost = max(gust_boost, 18 + pk * 0.6)
                thunder = thunder or pk >= THUNDER_MMH
                c = max(c, 0.85)
        if not rate and c > 0.92 and w > 0.35 and not self.drizzle_on(t) and p_u < 0.004:
            self.drizzle.append((t, 3600 + 7200 * p_d, 0.3 + 0.6 * p_r))
        if self.drizzle_on(t):
            rate += self.drizzle_on(t)
        ws = bump(t, WETSNOW[0], WETSNOW[1], 2 * 3600)
        rate, c = max(rate, ws * WETSNOW[3]), max(c, ws)
        # temperature
        doy = loc(t).timetuple().tm_yday
        rho = math.exp(-STEP / (60 * 3600))
        self.a = rho * self.a + t_a * (2.6 + 1.4 * w) * math.sqrt(1 - rho * rho)
        front_t = sum((1.0 * s * (cs == 0) if t < tf else -2.6 * s * (1 - math.exp(-(t - tf) / 21600)) * math.exp(-(t - tf) / 259200))
                      for tf, s, _ in fr if t > tf - 86400)
        dtr = (4.2 + 6.3 * su) * (1 - 0.65 * c) * (1 + 0.5 * calm)
        T = (8.8 - 10.2 * math.cos(2 * math.pi * (doy - 20) / 365.25) + self.a + front_t + 7.8 * hw - cs
             + dtr / 2 * math.cos(2 * math.pi * (hl - 14.5) / 24) - (1.0 * calm if not 7 < hl < 17 else 0) + 0.08 * t_n)
        T += ws * (WETSNOW[2] + 0.08 * t_n - T)
        # dew point from the spread: wide on dry afternoons, narrow at night, under cloud and in rain
        day = max(0.0, math.cos(2 * math.pi * (hl - 14.5) / 24))
        sp = (2.2 + 6.5 * su + 3 * hw) * (0.35 + 0.65 * day) * (1 - 0.5 * c) + 0.6 + abs(h_n)
        if rate > 0.2:
            sp = 0.25 + h_u * 0.6
        rh = max(18.0, min(99.0, 100 * magnus(T - sp) / magnus(T)))
        # wind: a breezier afternoon, quiet nights, fronts and showers gusty
        self.m = 0.95 * self.m + w_m
        df = 0.62 + 0.55 * max(0.0, math.sin(math.pi * (hl - 9) / 12)) if 9 < hl < 21 else 0.62
        v = (6.0 + 4.0 * w) * df * math.exp(self.m) * (1 - 0.8 * calm) * (1 - 0.35 * (1 - c) * (1 - day))
        v += sum(15 * min(s, 1.3) * math.exp(-((t - tf) / 25000) ** 2) for tf, s, _ in fr) + gust_boost * 0.35
        v = max(0.0, v + w_n)
        g = v * (1.3 + 0.35 * w_u1) + 1.6 * w_u2 + gust_boost
        self.dw = 0.97 * self.dw + w_d
        wdir = (250 + 55 * math.tanh(dp3 / 2) + self.dw) % 360
        # the gauge: rain tips at once, dry snow waits in the funnel and melts in later, wet snow melts in as it falls
        snowing = T <= 0.5
        if snowing and ws < 0.5:
            self.snow += rate / 6
        else:
            self.acc += rate / 6
        if self.snow and T > 1.0:
            m = min(self.snow, 0.04 * (T - 1))
            self.snow -= m
            self.acc += m
        tips = int(self.acc / TIP)
        self.acc -= tips * TIP
        # sun
        el, az = sun(t + STEP / 2)
        tr = 1 - 0.78 * c ** 2.2
        if 0.25 < c < 0.85:
            tr = min(1.12, tr * (0.45 + 0.75 * c_u))
        if rate > 0:
            tr *= 0.35
        shade = shaded(el, az)  # the roof to the south-west and a tree to the east: diffuse light only
        sol = clear_sky(el) * tr * (0.22 if shade else 1)
        uv = 9.5 * max(0.0, math.sin(math.radians(el))) ** 2 * (1 - 0.65 * c ** 1.5) * (0.3 if shade else 1)
        p = p_s + s2(t + STEP / 2) + s_n
        return dict(t=t, T=T, Tlo=T - abs(0.15 * t_lo), Thi=T + abs(0.2 * t_hi), rh=rh, p=p, v=v, g=g, dir=wdir,
                    rate=rate, snowing=snowing and rate > 0, tips=tips, sol=sol, solmax=sol * (1 + 0.08 * s_u), uv=uv, c=c, el=el, az=az,
                    clear=clear_sky(el), thunder=thunder)

    def drizzle_on(self, t):
        return next((r for a, ln, r in self.drizzle[-1:] if a <= t < a + ln), 0.0)


def td_of(T, rh):
    x = math.log(rh / 100) + 17.625 * T / (243.04 + T)
    return 243.04 * x / (17.625 - x)


# ------------------------------------------------------------------ records (history.cpp's aggregation, web_render.cpp's JSON)
def vec_mean(pairs):
    """Speed-weighted direction and steadiness of (speed, dir) pairs; (None, None) when calm."""
    x = sum(v * math.sin(math.radians(d)) for v, d in pairs if v >= 1.0)
    y = sum(v * math.cos(math.radians(d)) for v, d in pairs if v >= 1.0)
    tot = sum(v for v, d in pairs if v >= 1.0)
    if tot <= 0:
        return None, None
    return round(math.degrees(math.atan2(x, y))) % 360, round(math.hypot(x, y) / tot, 2)


def hour_row(h, steps, n, q=()):
    """One hour record from its steps (all six, or the ones so far for the open hour)."""
    T = [round(s['T'], 1) for s in steps]
    rh = [round(s['rh']) for s in steps]
    td = [td_of(s['T'], s['rh']) for s in steps]
    v = [s['v'] for s in steps]
    rain = round(sum(s['tips'] for s in steps) * TIP, 2)
    peak = max(s['tips'] for s in steps) * TIP * 6
    rate_max = math.ceil(max(peak, rain) * 2) / 2 if rain else 0.0
    wd, steady = vec_mean([(s['v'], s['dir']) for s in steps])
    calm_n = round(n * sum(1 for x in v if x < 1.0) / len(steps))
    flags = ['baro_uncal'] if steps[0].get('uncal') else []
    return {'h': hkey(h), 'n': n, 'q': flags + list(q), 'epoch': 0, 'rej_fields': 1 if 'rejected' in q else 0,
            'temp_c': [rnd(min(s['Tlo'] for s in steps), 1), rnd(sum(T) / len(T), 1), rnd(max(s['Thi'] for s in steps), 1)],
            'dewpt_c': [None, rnd(sum(td) / len(td), 1), None],
            'humidity': [min(rh), rnd(sum(rh) / len(rh), 0), max(rh)],
            'pressure_hpa': [None, rnd(sum(s['p'] for s in steps) / len(steps), 1), None],
            'wind_kmh': [None, rnd(sum(v) / len(v), 1), None], 'gust_kmh': rnd(max(s['g'] for s in steps), 1),
            'wind_dir': wd, 'wind_steady': steady, 'calm_n': calm_n, 'rain_mm': rain, 'rain_daily_mm': None,
            'rain_rate_max': rate_max, 'uv': [None, None, rnd(max(s['uv'] for s in steps), 1)],
            'solar_wm2': [None, rnd(sum(s['sol'] for s in steps) / len(steps), 0), rnd(max(s['solmax'] for s in steps), 0)]}


UNCLEAN = {'stale', 'baro_uncal', 'clock_approx', 'restart', 'baro_boundary', 'baro_bridge', 'gap'}
Q_ORDER = ['stale', 'baro_uncal', 'clock_approx', 'restart', 'baro_boundary', 'baro_bridge', 'gap', 'console_reset',
           'rejected', 'rain_partial', 'epoch_split']


def day_row(d, H):
    """One day record from its hour records (history.cpp: the day closes over its hours)."""
    n = sum(x['n'] for x in H)
    wavg = lambda k, i=1: rnd(sum(x[k][i] * x['n'] for x in H) / n, 1)
    pairs = [(x['wind_kmh'][1], x['wind_dir']) for x in H if x['wind_dir'] is not None]
    wd, steady = vec_mean(pairs)
    rose = [0] * 8
    for x in H:
        if x['wind_dir'] is not None and x['calm_n'] * 2 < x['n']:
            rose[round(x['wind_dir'] / 45) % 8] += 1
    q = sorted({f for x in H for f in x['q']}, key=Q_ORDER.index)
    rain = rnd(sum(x['rain_mm'] for x in H), 2)
    return {'d': d, 'n': n, 'hours': len(H), 'clean_hours': sum(1 for x in H if not UNCLEAN & set(x['q'])), 'q': q, 'epoch': 0,
            'rej_fields': max(x['rej_fields'] for x in H),
            'temp_c': [min(x['temp_c'][0] for x in H), wavg('temp_c'), max(x['temp_c'][2] for x in H)],
            'dewpt_c': [min(x['dewpt_c'][1] for x in H), wavg('dewpt_c'), max(x['dewpt_c'][1] for x in H)],
            'humidity': [min(x['humidity'][0] for x in H), rnd(sum(x['humidity'][1] * x['n'] for x in H) / n, 0), max(x['humidity'][2] for x in H)],
            'pressure_hpa': [min(x['pressure_hpa'][1] for x in H), wavg('pressure_hpa'), max(x['pressure_hpa'][1] for x in H)],
            'wind_kmh': [None, wavg('wind_kmh'), None], 'gust_kmh': max(x['gust_kmh'] for x in H),
            'wind_dir': wd, 'wind_steady': steady, 'calm_hours': len(H) - sum(rose), 'rose': rose,
            'rain_mm': rain, 'rain_daily_mm': rain, 'rain_hours': sum(1 for x in H if x['rain_mm'] > 0),
            'rain_rate_max': max(x['rain_rate_max'] for x in H), 'uv': [None, None, max(x['uv'][2] for x in H)],
            'solar_wm2': [None, rnd(sum(x['solar_wm2'][1] for x in H) / len(H), 0), max(x['solar_wm2'][2] for x in H)]}


# ------------------------------------------------------------------ the whole history
class History:
    def __init__(self):
        self.d_end = midnight(T0)                        # T0's local day is the open day
        self.d_start = midnight(self.d_end - (DAYS - 1) * 86400 - 43200)
        self.sim = Sim(self.d_start, T0)
        self.steps, t = {}, self.d_start - 2 * 86400     # two spin-up days settle the random processes
        while t < T0 + 2 * 86400:                       # one day past T0 too: the WU forecast's "tomorrow"
            s = self.sim.step(t)
            s['uncal'] = t < self.d_start + 30 * 3600    # a new console: pressure at its factory value at first
            if s['uncal']:
                s['p'] = 1013.2
            if REL_ERR[0] <= t < REL_ERR[1]:
                s['p'] += REL_ERR[2]
            self.steps[t] = s
            t += STEP
        self.n_of = {}
        R = random.Random(SEED + 1)
        h = self.d_start
        while h < T0 + 2 * 86400:
            self.n_of[h] = 55 + (R.random() < 0.45)
            h += 3600

    def hour_steps(self, h, upto=None):
        return [self.steps[h + k * STEP] for k in range(6) if upto is None or h + k * STEP < upto]

    def hour(self, h):
        if h in OUTAGE:
            return None
        if h + 3600 == OUTAGE[0]:
            return hour_row(h, self.hour_steps(h), 32, ('gap',))
        if h - 3600 == OUTAGE[-1]:
            return hour_row(h, self.hour_steps(h), 41, ('restart', 'gap'))
        return hour_row(h, self.hour_steps(h), self.n_of[h], ('rejected',) if h == REJECTED_HOUR else ())

    def build(self):
        self.hours, self.x, h = [], {}, self.d_start  # every closed hour, one day past T0 too (the TAF's and WU's future)
        while h + 3600 <= hourfloor(T0) + 2 * 86400 - 3600:
            r = self.hour(h)
            if r:
                self.hours.append((h, r))
                st = self.hour_steps(h)
                self.x[h] = {'c': sum(q['c'] for q in st) / 6, 'pr': sum(q['rate'] for q in st) / 6, 'ts': any(q['thunder'] for q in st),
                             'clear': sum(q['clear'] for q in st) / 6, 'el': st[3]['el'], 'az': st[3]['az']}
            h += 3600
        self.days, by = [], {}
        for h, r in self.hours:
            by.setdefault(dkey(h), []).append(r)
        for d in sorted(by):
            if d < dkey(T0):
                self.days.append(day_row(d, by[d]))
        assert len(self.days) == DAYS, len(self.days)
        return self


def hourfloor(t):
    return t - t % 3600


# ------------------------------------------------------------------ the hourly series (columnar; demo/replay.js renders the rows)
COLS = ['n', 'q', 'tlo', 'tav', 'thi', 'td', 'rhlo', 'rhav', 'rhhi', 'p', 'w', 'g', 'dir', 'st', 'calm', 'rain', 'rmax', 'uv',
        'sol', 'solx', 'cloud', 'pr', 'ts']


def series(hist):
    """Every hour from LEAD + YEAR days before T0's day to the end of the day after it, one array per column. `q` is the
    quality bitmask (Q_ORDER), `cloud` %, `pr` the precipitation rate (mm/h, snow included), `ts` thunder; a lost hour has
    n 0 and nulls. Hours after T0 are the future the TAF and WU lines read; a page never sees them."""
    rows = dict(hist.hours)
    h0 = midnight(midnight(T0) - (YEAR + LEAD) * 86400 + 43200)
    cols = {k: [] for k in COLS}
    for h in range(h0, hourfloor(T0) + 2 * 86400 - 3600, 3600):
        r, x = rows.get(h), hist.x.get(h)
        vals = [0, 0] + [None] * 12 + [0] + [None] * 7 + [0] if not r else [
            r['n'], sum(1 << Q_ORDER.index(f) for f in r['q']), *r['temp_c'], r['dewpt_c'][1], *r['humidity'], r['pressure_hpa'][1],
            r['wind_kmh'][1], r['gust_kmh'], r['wind_dir'], r['wind_steady'], r['calm_n'], r['rain_mm'], r['rain_rate_max'], r['uv'][2],
            r['solar_wm2'][1], r['solar_wm2'][2], round(100 * x['c']), rnd(x['pr'], 1), int(x['ts'])]
        for k, v in zip(COLS, vals):
            cols[k].append(v)
    return {'station': SID, 'from': hkey(h0), 'step_s': 3600, 'hours': len(cols['n']), 'last_t0': iso(T0), 'cols': cols}


# ------------------------------------------------------------------ the learners, run over the history (/api/learn)
BARS = {'l1': [6, 12, 27], 'l2_wet': [9, 19, 47], 'l2_night': [24, 63, 92], 'l3': [237, 591, 1019], 'l4': [265, 569, 1358],
        'l5': [7, 18, 27], 'l6': [18, 42, 78], 'l7': [14, 51, 51]}  # learn.h LEARN_BARS (real samples to 1 / 2 / 3 bars)
USE_AT = {'l1': 2, 'l2_wet': 1, 'l2_night': 1, 'l5': 2, 'l6': 3}    # learn.h LEARN_USE_AT; L3, L4, L7 information only
CONF, EL = ['default', 'low', 'medium', 'high'], [5, 10, 15, 20, 30, 45]
SKY_ROWS = [(3, 21, 2, 72), (21, 41, 4, 36), (41, 61, 5, 24), (61, 81, 10, 12), (81, 90, 9, 1)]  # admin-learn.js / l1bCell


def lvl(bar, c):
    return sum(c >= b for b in BARS[bar])


def blk(bar, c, use=None, info=False):
    """conf, n (effective, with forgetting), need and the sheet state of a learner with c real samples."""
    lv = lvl(bar, c)
    st = 'default' if not lv else 'info' if info else 'in_use' if lv >= (use if use is not None else USE_AT[bar]) else 'learned'
    hi = BARS[bar][2]
    return {'n': rnd(c if c < hi else hi + (c - hi) * 0.25, 1), 'conf': CONF[lv], 'need': 0 if lv == 3 else BARS[bar][lv] - c, 'state': st}


def sky_cell(el, az):
    i = 0
    for lo, hi, dh, cols in SKY_ROWS:
        for e in range(lo, hi, dh):
            if e <= el < e + dh:
                return i + int(az / (360 / cols)) % cols
            i += cols
    return None


def norm_p(t):  # the built-in long-term sea-level pressure here (hPa), 15 days back (learn.cpp L7)
    return 1015.0 + 1.3 * math.cos(2 * math.pi * (loc(t - 15 * 86400).timetuple().tm_yday - 15) / 365.25)


class Learners:
    """The samples each learner takes, once over the whole history; a snapshot at t reads the ones before t."""

    def __init__(self, hist):
        self.hist = hist
        R, X = dict(hist.hours), hist.x
        self.l1, self.l1b, self.wet, self.nights, self.l3, self.l4, self.l5, self.l6, self.l7 = ([] for _ in range(9))
        for h, r in hist.hours:
            x, w = X[h], r['wind_kmh'][1]
            if x['c'] < 0.12 and not r['rain_mm'] and x['el'] >= 3:
                for k in range(6):
                    s = hist.steps[h + k * STEP]
                    if s['el'] >= 3:
                        self.l1b.append((h, sky_cell(s['el'], s['az']), 2 if shaded(s['el'], s['az']) else 1))
                if x['el'] >= 5 and not shaded(x['el'], x['az']) and x['clear'] > 0:
                    band = max(i for i, e in enumerate(EL) if x['el'] >= e)
                    self.l1.append((h, band * 2 + (lst(h + 1800) >= 12), r['solar_wm2'][1] / x['clear'] * (0.86 + 0.002 * EL[band])))
            if r['rain_mm'] > 0:
                self.wet.append((h, r['humidity'][2]))
            if 0 < w < 6:
                self.l3.append((h, w, r['n']))
            self.l4.append((h, w))
        days = sorted({dkey(h) for h, _ in hist.hours})
        for d in days:
            m = midnight(ts(d + 'T12:00'))
            hs = next((h for h in range(m + 12 * 3600, m + 23 * 3600, 3600) if h in X and X[h]['el'] < 0 <= X.get(h - 3600, {'el': 1})['el']), None)
            if hs is None or any(k not in R for k in range(hs - 3600, hs + 3 * 3600, 3600)):
                continue
            T = lambda k: R[k]['temp_c'][1]
            cl = sum(X[k]['c'] for k in range(hs, hs + 3 * 3600, 3600)) / 3
            rain = any(R[k]['rain_mm'] for k in range(hs, hs + 3 * 3600, 3600))
            if not rain:
                self.l5.append((hs + 3 * 3600, 'clear' if cl < 0.2 else 'overcast' if cl > 0.8 else 'mixed', (T(hs - 3600) - T(hs + 2 * 3600)) / 3))
            # the night after: from 3 h after sunset to sunrise
            he = hs + 3 * 3600
            end = next((h for h in range(he, he + 18 * 3600, 3600) if h in X and X[h]['el'] >= 0), None)
            NH = [k for k in range(he, end or he, 3600) if k in R]
            if not end or len(NH) < 8:
                continue
            ok = (sum(X[k]['c'] for k in NH) / len(NH) < 0.5 and sum(R[k]['wind_kmh'][1] for k in NH) / len(NH) < 10
                  and not any(R[k]['rain_mm'] for k in NH))
            rhmax = max(R[k]['humidity'][2] for k in NH)
            self.nights.append((end, ok, rhmax))
            if ok:
                te, tde = T(he), R[he]['dewpt_c'][1]
                self.l6.append((end, te - min(R[k]['temp_c'][0] for k in NH), te - tde, (end - he) / 3600 - 12, he, te, tde,
                                min(R[k]['temp_c'][0] for k in NH), rhmax))
        for d in hist.days:
            if d['hours'] >= 20 and 'baro_uncal' not in d['q']:
                self.l7.append((midnight(ts(d['d'] + 'T12:00')) + 86400, d['pressure_hpa'][1]))

    def at(self, t, metar=True):
        """/api/learn at t (the newest closed hour before t is the last fed)."""
        hist, H = self.hist, hourfloor(t)
        before = lambda L: [s for s in L if s[0] < H]
        # L1: per sun-height cell
        cells = []
        for i in range(12):
            S = [s for s in before(self.l1) if s[1] == i and s[0] >= H - 400 * 86400]
            b = blk('l1', len(S))
            e = rnd(sum(s[2] for s in S[-30:]) / len(S[-30:]), 2) if S else 0.55
            cells.append({'el': EL[i >> 1], 'pm': bool(i & 1), 'e': e, 'n': b['n'], 'conf': b['conf'], 'last': S[-1][0] // 86400 if S else 0,
                          'hit': False, 'need': b['need'], 'state': b['state'], 'from': None})
        seen = {}
        for h, cell, cls in before(self.l1b):
            if cell is not None:
                seen[cell] = cls
        mask = bytearray(238)
        for i, k in seen.items():
            mask[i >> 2] |= k << (2 * (i & 3))
        today = {cell for h, cell, cls in before(self.l1b) if h >= midnight(t) and cell is not None}
        # L2: the wettest readings, and clear nights
        W = before(self.wet)[-200:]
        Nn = [(e, rh) for e, ok, rh in self.nights if e < H and ok][-150:]
        we = rnd(sum(x[1] for x in W[-40:]) / max(1, len(W[-40:])), 1) if W else 100
        ne = rnd(sum(x[1] for x in Nn[-40:]) / max(1, len(Nn[-40:])), 1) if Nn else 100
        bw, bn = blk('l2_wet', len(W)), blk('l2_night', len(Nn))
        src = 'night' if lvl('l2_night', len(Nn)) and ne >= 92 else 'wet' if lvl('l2_wet', len(W)) else 'prior'
        s2v = ne if src == 'night' else we if src == 'wet' else 100
        bw['state'] = 'in_use' if src == 'wet' else bw['state'] if bw['state'] == 'default' else 'learned'
        bn['state'] = 'in_use' if src == 'night' else bn['state'] if bn['state'] == 'default' else 'learned'
        # L3, L4
        L3 = [s for s in before(self.l3) if s[0] >= H - 400 * 86400]
        l3v = sorted(s[1] for s in L3)
        L4 = sorted(s[1] for s in before(self.l4) if s[0] >= H - 365 * 86400)
        # L5: clear and overcast evenings
        E = before(self.l5)
        cl = [x[2] for x in E if x[1] == 'clear'][-60:]
        ov = [x[2] for x in E if x[1] == 'overcast'][-60:]
        bc, bo = blk('l5', len(cl)), blk('l5', len(ov))
        ec, eo = rnd(sum(cl[-25:]) / max(1, len(cl[-25:])), 2) if cl else 1.5, rnd(sum(ov[-25:]) / max(1, len(ov[-25:])), 2) if ov else 0.5
        use5 = lvl('l5', len(cl)) >= 2 and lvl('l5', len(ov)) >= 2
        thr = rnd((ec + eo) / 2, 2) if use5 else 1.0
        for b in (bc, bo):
            b['state'] = 'in_use' if use5 else b['state'] if b['state'] == 'default' else 'learned'
            b.pop('n')
        # L6: d = a + b (T - Td in the evening) + c (night length - 12), least squares over the latest admitted nights
        N6 = before(self.l6)[-80:]
        a = bb = cc = 0.0
        sd = mae = None
        if len(N6) >= 3:
            A = [[1, s[2], s[3]] for s in N6]
            y = [s[1] for s in N6]
            M = [[sum(r[i] * r[j] for r in A) for j in range(3)] for i in range(3)]
            v = [sum(r[i] * yy for r, yy in zip(A, y)) for i in range(3)]
            a, bb, cc = solve3(M, v)
            res = [yy - (a + bb * r[1] + cc * r[2]) for r, yy in zip(A, y)]
            sd, mae = rnd(math.sqrt(sum(e * e for e in res) / len(res)), 1), rnd(sum(abs(e) for e in res) / len(res), 1)
        b6 = blk('l6', len(N6), use=9)
        use6 = lvl('l6', len(N6)) == 3 and sd is not None and sd <= 2.0
        b6['state'] = 'in_use' if use6 else b6['state'] if b6['state'] == 'default' else 'learned'
        # L7: the daily mean pressure's running mean (half-life 30 days) against the built-in normal
        al, e7, n7, days, flag = 1 - 2 ** (-1 / 30), None, 0.0, 0, 'none'
        for end, pd in before(self.l7):
            e7 = pd if e7 is None else e7 + al * (pd - e7)
            n7, days = n7 * (1 - al) + 1, days + 1
            stage = 2 if days >= 51 else 1 if days >= 14 else 0
            o = e7 - norm_p(end)
            if flag == 'none' and stage:
                flag = 'high' if o >= (12 if stage == 2 else 20) else 'low' if o <= -(12 if stage == 2 else 20) else 'none'
            elif flag != 'none' and abs(o) < 9:
                flag = 'none'
        stage = 2 if days >= 51 else 1 if days >= 14 else 0
        # the outlook's snapshot of what it may read
        hn = H - 3600
        el, az = sun(hn + 1800)
        band = max((i for i, e in enumerate(EL) if el >= e), default=None)
        cell = cells[band * 2 + (lst(hn + 1800) >= 12)] if band is not None else None
        pred = self.tonight(t, a, bb, cc)
        night_end = next(h for h in range(H, H + 30 * 3600, 3600) if sun(h)[0] >= 0 > sun(h - 3600)[0])
        use = {'hour': hn, 'l1': any(c['state'] == 'in_use' for c in cells), 'l2': src != 'prior', 'l5': use5, 'l6': use6,
               'sky_ref': round(cell['e'] * hist.x[hn]['clear']) if cell and hn in hist.x else 0,
               'sky_ref_max': round(cell['e'] * hist.x[hn]['clear'] * 1.17) if cell and hn in hist.x else 0,
               'sky_cls': 255 if el < 0 else seen.get(sky_cell(el, az), 0), 'rh_sat': s2v, 'cool': thr,
               'min_lo': pred[0], 'min_hi': pred[1], 'night_end': night_end, 'metar': metar}
        last6 = N6[-1] if N6 else None
        ev = [x for x in E][-1:] or [None]
        return {'v': 1, 'state': 'learning', 'why': None, 'epoch': 0, 'last_hour': hn, 'last_day': int(dkey(midnight(t) - 3600).replace('-', '')),
                'relearn': False, 'migrated_from': 0, 'backfill': {'hours': 0, 'days': 0, 'done': True},
                'written': H - (H // 3600 % 6) * 3600, 'writes': 1 + (H // 3600) % 6, 'nvs': {'used': 342, 'free_entries': 288},
                'l1': {'def': 0.55, 'lo': 0.35, 'hi': 1.3, 'cells': cells},
                'l1b': {'sun': sum(1 for k in seen.values() if k == 1), 'shade': sum(1 for k in seen.values() if k == 2),
                        'unknown': 949 - len(seen), 'today': len(today), 'clear_today': bool(today), 'mask': base64.b64encode(bytes(mask)).decode()},
                'l2': {'wet': dict(e=we, **{k: bw[k] for k in ('n', 'conf')}, hit=False, need=bw['need'], state=bw['state']),
                       'night': dict(e=ne, **{k: bn[k] for k in ('n', 'conf')}, need=bn['need'], state=bn['state']),
                       's': s2v, 'src': src, 'stuck': False, 'state': 'in_use' if src != 'prior' else 'default'},
                'l3': dict(e=1.6 if len(l3v) > 20 else 1.0,  # the console reports whole mph: its smallest step
                           **{k: v for k, v in blk('l3', sum(s[2] for s in L3), info=True).items() if k in ('n', 'conf')},
                           hit=False, need=blk('l3', sum(s[2] for s in L3), info=True)['need'], state=blk('l3', sum(s[2] for s in L3), info=True)['state']),
                'l4': dict(p50=rnd(L4[len(L4) // 2], 1) if L4 else 3, p90=rnd(L4[len(L4) * 9 // 10], 1) if L4 else 12,
                           **{k: v for k, v in blk('l4', len(L4), info=True).items() if k in ('n', 'conf')}, hit=False,
                           need=blk('l4', len(L4), info=True)['need'], state=blk('l4', len(L4), info=True)['state']),
                'l5': {'clear': dict(e=ec, n=len(cl), **bc), 'overcast': dict(e=eo, n=len(ov), **bo), 'thr': thr,
                       'src': 'learned' if use5 else 'default', 'hit': False, 'state': 'in_use' if use5 else 'learned' if cl or ov else 'default'},
                'l6': {'a': rnd(a, 2), 'b': rnd(bb, 3), 'c': rnd(cc, 3), 'n': b6['n'], 'sd': sd, 'mae': mae, 'conf': b6['conf'],
                       'learned': lvl('l6', len(N6)) >= 1, 'hit': False, 'need': b6['need'], 'state': b6['state']},
                'relearn_hour': 0, 'bars': BARS,
                'l7': {'e': rnd(e7, 1), 'n': rnd(n7, 1), 'days': min(days, 61), 'stage': stage, 'flag': flag, 'uncal': False,
                       'off': rnd(e7 - norm_p(H), 1) if e7 is not None else None, 'norm': rnd(norm_p(H), 1),
                       'need': 0 if stage == 2 else (14 if not stage else 51) - days, 'state': 'info' if stage else 'default'},
                'use': use, 'gate_state': {'indoor': False, 'frozen_run': 0, 'geo_days': 0, 'dome': False, 'iced': False},
                'night': {'open': None, 'last': None if not last6 else {
                    'start': last6[4], 'len_h': round((last6[0] - last6[4]) / 3600), 'hours': round((last6[0] - last6[4]) / 3600), 'ok': True,
                    'calm': True, 'rain': False, 'hoar': last6[7] <= 0 and last6[7] - last6[6] <= 2, 'restart': False, 'rh_max': last6[8],
                    't2': rnd(last6[5], 1), 'td2': rnd(last6[6], 1), 't0': rnd(last6[7] + 0.4, 1), 'dt3': rnd(-last6[1] / 3, 1), 'tmin': rnd(last6[7], 1),
                    'l_h': rnd((last6[0] - last6[4]) / 3600 + 3, 1), 'w5': round((last6[0] - last6[4]) / 3600), 'rise': False, 'l6': 'ok',
                    'pred': rnd(last6[5] - (a + bb * last6[2] + cc * last6[3]), 1)}, 'last_admitted': bool(last6)},
                'evening': {'label': ev[0][1] if ev[0] else None, 'label_day': (ev[0][0] - 3 * 3600) // 86400 if ev[0] else None}}

    def tonight(self, t, a, b, c):
        """L6's minimum range for the coming night: the evening's reading put through the fitted relation, +-1.5 °C."""
        R = dict(self.hist.hours)
        he = next((h for h in range(hourfloor(t) - 12 * 3600, hourfloor(t) + 24 * 3600, 3600)
                   if h in self.hist.x and self.hist.x[h]['el'] < 0 <= self.hist.x.get(h - 3 * 3600, {'el': 1})['el'] and h >= hourfloor(t) - 12 * 3600), None)
        if he is None or he not in R:
            return None, None
        te, tde = R[he]['temp_c'][1], R[he]['dewpt_c'][1]
        p = te - (a + b * (te - tde) + c * 3)
        return math.floor(p - 1.5), math.ceil(p + 1.5)


def solve3(M, v):
    d = lambda m: (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                   + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    D = d(M)
    if abs(D) < 1e-9:
        return 0.0, 0.0, 0.0
    return tuple(d([[v[i] if j == k else M[i][j] for j in range(3)] for i in range(3)]) / D for k in range(3))


# ------------------------------------------------------------------ the bodies that do not change with time, and the index
AIRPORTS = [{'icao': 'EPMO', 'name': 'Warsaw Modlin', 'km': 33, 'cc': 'PL', 'fp': 0, 'auto': False, 'dt': 0.6, 'dp': 0.4},
            {'icao': 'EPWA', 'name': 'Warsaw Chopin', 'km': 10, 'cc': 'PL', 'fp': 0, 'auto': False, 'dt': 0.8, 'dp': 0.1}]
EXAMPLES = [('winter', 'January: wet snow in the morning, a hard frost at night', '2026-01-21T09:30', '2026-01-21T21:00'),
            ('spring', 'April: a frosty clear morning, a sunny afternoon', '2026-04-15T11:30', '2026-04-15T20:30'),
            ('summer', 'August: a hot afternoon, a thunderstorm in the evening', '2026-08-22T12:40', '2026-08-22T20:30'),
            ('autumn', 'October: pressure falling, a front brings rain and wind', '2026-10-08T06:20', '2026-10-08T20:30')]
SCENARIOS = [('today', 'A November noon: sunny spells, a front due tomorrow', iso(T0), {}),
             ('sunny', 'A heat-wave afternoon: sunny and hot', '2026-08-22T12:40', {}),
             ('rain', 'Moderate rain on a front, windy', '2026-10-08T14:27', {}),
             ('snow', 'Wet snow at about 0 °C: the gauge counts it, the airport reports snow', '2026-01-21T08:30', {}),
             ('thunderstorm', 'An evening thunderstorm: heavy rain, thunder reported nearby', '2026-08-22T17:40', {}),
             ('frost-night', 'A clear calm frosty night: ground frost and tonight\'s minimum', '2026-11-09T22:00', {}),
             ('pressure-off', 'The console\'s REL mis-set for two months: the pressure check speaks', '2026-03-31T11:00', {'metar': False, 'wufc': False}),
             ('offline', 'The console stopped uploading 25 minutes ago', iso(T0), {'late': 1500}),
             ('no-sources', 'Airport weather and the WU forecast switched off', iso(T0), {'metar': False, 'wufc': False})]
CALIB = {'temp_c_offset': 0, 'humidity_offset': 0, 'pressure_hpa_offset': 0, 'wind_dir_offset': 0, 'rain_factor': 1, 'wind_factor': 1}
BRIDGE = {'effective': {'tz': TZ, 'tz_name': TZ_NAME, 'lat': LAT, 'lon': LON, 'auto_publish_new_stations': True, 'lan_uploads': False,
                        'max_stations': 1, 'max_stations_cap': 1, 'mqtt_interval_last_seen_s': 300, 'forwarding': True, 'log_verbose': False},
          'overrides': {'tz': TZ, 'tz_name': TZ_NAME, 'lat': LAT, 'lon': LON, 'forwarding': True, 'max_stations': 1}}
STATION = {'station': SID, 'configured': True, 'stored': True,
           'effective': {'name': NAME, 'profile': 'yt60234', 'publish': True, 'stale_s': 240, 'limits': {}, 'jumps': {}, 'calib': CALIB,
                         'disabled': [], 'hist': [], 'promote': {}}, 'overrides': {'name': NAME}}


DIAG = dict(ap_clients=1, sta_up=True, sta_rssi=-62, mqtt_up=True, heap_free=61240, heap_min=38112, heap_largest=31732, uploads=0,
            queue_drops=0, other_hits=412, uploads_dropped_lan=0, uploads_dropped_key=0, uploads_dropped_blocked=0, uploads_too_long=0,
            uploads_no_slot=0, uploads_rate_limited=0, uploads_duplicate=318, stale_longest_run=3, stale_on_count=0, store_fail=0,
            store_drop_old=0, store_cut=0, store_fail_append=0, store_future_dropped=0, clock_held=False, clock_held_n=0, clock_ref=0,
            clock_cut_n=0, store_future_kept=0, store_retry=0, http_stream_abort=0, mqtt_init_fail=0, snap_from=1, hist_back=[0],
            dns_answered=2210, ntp_answered=31, ntp_unanswered=0, mqtt_published=0, mqtt_failed=0, fwd_ok=0, fwd_fail=1,
            fwd_attempt_fail=1, fwd_skip=2, fwd_drop=0, heap_low=False, heap_min_10m=44860, http_requests=0, http_304=0,
            heap_min8_t=812, heap_min8_ctx=['http', 'fwd'], heap_min8_socks=2, http_aborted=0, http_purged=0, http_shed=0, http_paced=3,
            sta_ip='192.168.1.20', fw_version=FW, hostname='fauxcloud', stations_allocated=1, stations_used=1, mode='run',
            admin_lock={'active': False, 'until_s': 0, 'failures': 0}, store_ms_max=41, build='0.1.0-demo', ota_state='stable',
            ota_slot=0x1D0000, last_reset='ota', reset_raw='SW_CPU_RESET', restarts=14, sta_reinits=0, mqtt_reconnects=1, last_crash=None)


def admin():
    tgt = lambda slot, typ, ident, mins, nok: {
        'slot': slot, 'type': typ, 'station': SID, 'id': ident, 'key': '<set>', 'min_s': mins, 'max_s': mins, 'floor_s': 300, 'https': True,
        'enabled': True, 'verified': True, 'paused': None, 'state': 'idle', 'next_s': 140 + 60 * slot,
        'last': {'t': None, 'status': 200, 'result': 'ok', 'ms': 612 + 97 * slot}, 'last_ok': None,
        'n': {'ok': nok, 'dup': 1, 'skip': 2, 'nodata': 0, 'reject': 0, 'refused': 0, 'limited': 0, 'drop': 0, 'heap': 0, 'fail': slot, 'unconfirmed': 0},
        'causes': {'dns': 0, 'connect': slot, 'read': 0, '5xx': 0, '429': 0, 'other': 0}, 'skip_why': None}
    return {
        'settings': {'settings': {'sta_ssid': 'DemoNet', 'sta_pass': '<set>', 'mqtt_host': '192.168.1.5', 'mqtt_port': 1883, 'mqtt_user': 'fauxcloud',
                                  'mqtt_pass': '<set>', 'ap_pass': '<set>', 'admin_password': '<set>', 'hostname': 'fauxcloud', 'setup_done': True},
                     'wifi_change': '', 'ap_ssid': 'fauxcloud-A1B2', 'sessions': 1, 'errors': [], 'notes': [], 'demo': True},
        'bridge': dict(BRIDGE, demo=True),
        'station': dict({'station': SID, 'configured': True, 'stored': True, 'key': 'bound', 'key_mismatch': 0, 'src_mac': '02:00:00:00:00:01',
                         'fp': 'match', 'cal_n': 1, 'learn_use': 4}, effective=STATION['effective'], overrides=STATION['overrides'], demo=True),
        'metar': {'available': True, 'on': True, 'location': True, 'time': True, 'demo': True,
                  'airports': [{'icao': a['icao'], 'name': a['name'], 'own': True, 'cc': a['cc'], 'km': a['km']} for a in AIRPORTS],
                  'state': 'idle', 'next_s': 420, 'wait_why': None,
                  'n': {'ok': 380, 'nodata': 0, 'fail': 2, 'yield': 5, 'skip': 1, 'heap': 0, 'limited': 0, 'untrusted': 0, 'paused': 0, 'missed': 1},
                  'run': {'state': 'idle', 'result': None}, 'lookup': {'state': 'idle', 'near': False, 'result': None, 'list': []}, 'last': None},
        'forwarding': {'enabled': True, 'tls': True, 'https_default': True, 'stations': [SID], 'demo': True,
                       'targets': [tgt(0, 'wu', 'IDEMO1', 300, 291), tgt(1, 'windy', 'DEMO-GARDEN', 310, 284)],
                       'heap': {'tick_us_max': 870}, 'task': {'up': True, 'stack_free': 2140},
                       'wufc': {'on': True, 'key': True, 'verified': True, 'loc': {'lat': round(LAT, 1), 'lon': round(LON, 1)}, 'state': 'idle',
                                'pause': 'none', 'next_t': None, 'last': None, 'run': {'state': 'idle', 'result': None},
                                'n': {'ok': 24, 'nodata': 0, 'fail': 1, 'limited': 0, 'refused': 0, 'skip': 0, 'heap': 0, 'missed': 0, 'bad': 0, 'paused': 0},
                                'tomorrow': None}},
        'forwarding-test': {'state': 'idle', 'slot': 0},
        'mqtt-test': {'state': 'idle', 'detail': ''},
        'sessions': [{'id': '3f9a12c0', 'ip': '192.168.1.10', 'ua': 'Mozilla/5.0 (demo)', 'created_s': 5400, 'last_s': 12, 'current': True}],
        'blocked': {'blocked': [], 'demo': True},
        'rings': {'rings': [{'id': SID, 'hours': 131 * 24 - 3, 'days': DAYS, 'slot': True}], 'parked': [], 'demo': True},
    }


def dump(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, separators=(',', ':'), ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')


def stamp(t):
    return datetime.fromtimestamp(t, UTC).strftime('%Y%m%dT%H%MZ')


def main(argv):
    out = Path(argv[argv.index('--out') + 1]) if '--out' in argv else Path(__file__).resolve().parent.parent / 'demo' / 'fixtures'
    for p in out.rglob('*.json'):  # this generator's files only: snap/ is demo/replay.js's
        if 'snap' not in p.relative_to(out).parts:
            p.unlink()
    hist = History().build()
    ser = series(hist)
    dump(out / 'history' / 'series.json', ser)
    dump(out / 'history' / 'daily.json', {'station': SID, 'open': None, 'days': hist.days})
    lr = Learners(hist)
    h0 = ts(ser['from'][:16]) + LEAD * 86400
    times = []
    d = loc(h0).replace(day=1, hour=0, minute=0)
    while True:  # the first of every month in the replayable year
        u = int(d.replace(tzinfo=UTC).timestamp())
        u -= off(u - 7200)
        if u > T0:
            break
        if u >= h0:
            times.append(u)
        d = (d + timedelta(days=32)).replace(day=1)
    # one snapshot per named time, stamped before its last upload (an upload lands up to 65 s before t0, `late` earlier)
    named = [ts(x[2]) for x in EXAMPLES] + [ts(x[3]) for x in EXAMPLES] + [ts(x[2][:16]) - x[3].get('late', 0) for x in SCENARIOS]
    times = sorted(set(times + [hourfloor(t - 120) for t in named]))
    learn = []
    for t in times:
        f = 'learn/%s.json' % stamp(t)
        dump(out / f, lr.at(t))
        learn.append({'t': iso(t), 'file': f})
    dump(out / 'static' / 'bridge.json', BRIDGE)
    dump(out / 'static' / 'station.json', STATION)
    dump(out / 'static' / 'diag.json', DIAG)  # /api/live's diag (LAN face); replay.js sets the counters
    for k, v in admin().items():
        dump(out / 'admin' / (k + '.json'), v)
    snap = lambda k: {'live': 'snap/%s/live.json' % k, 'frames': 'snap/%s/frames.json' % k}
    dump(out / 'index.json', {
        'demo': True, 'generator': 'tools/demo_data.py', 'replay': 'demo/replay.js', 'version': GEN_VERSION, 'seed': SEED, 't0': iso(T0),
        'station': SID, 'name': NAME, 'tz': TZ, 'tz_name': TZ_NAME, 'lat': LAT, 'lon': LON, 'tip_mm': TIP, 'upload_s': 65,
        'airports': AIRPORTS, 'rel_err': {'from': iso(REL_ERR[0]), 'to': iso(REL_ERR[1]), 'hpa': REL_ERR[2]},
        'diag': 'static/diag.json', 'series': {'file': 'history/series.json', 'from': ser['from'], 'first_t0': iso(h0), 'last_t0': iso(T0), 'hours': ser['hours']},
        'daily': {'file': 'history/daily.json', 'days': len(hist.days), 'first': hist.days[0]['d'], 'last': hist.days[-1]['d']},
        'learn': learn,
        'example_days': [{'id': k, 'title': title, 'day_t0': iso(ts(a)), 'night_t0': iso(ts(b)), 'day': snap(k + '-day'), 'night': snap(k + '-night')}
                         for k, title, a, b in EXAMPLES],
        'scenarios': {k: dict({'title': title, 't0': iso(ts(t[:16])), 'switches': sw}, **snap(k)) for k, title, t, sw in SCENARIOS},
        'default': 'today',
        'static': {'/api/bridge': 'static/bridge.json', '/api/station': 'static/station.json',
                   **{'/api/admin/' + {'blocked': 'station/blocked'}.get(k, k.replace('-', '/')): 'admin/%s.json' % k for k in admin()}},
    })
    total = sum(p.stat().st_size for p in out.rglob('*.json'))
    print('demo fixtures: %d series hours, %d days, %d learn snapshots, %d bytes -> %s (then: node demo/replay.js --write)'
          % (ser['hours'], len(hist.days), len(learn), total, out))


if __name__ == '__main__':
    main(sys.argv[1:])
