"""Reference real-time pitch model for VOCALOID4, fitted to rendered lab data.

Per note (times in ms from its onset):
  approach : previous pitch (or -50 cents at a phrase start) -> dip at Te,
             S-shaped (transition) or linear (phrase start)
  dip      : note - 3*bendDep cents, reached at Te(bendLen, direction)
  recovery : dip -> note over a bendLen-dependent curve
  release  : at a phrase end, linear fall to -45 cents over the last 142 ms
"""
import numpy as np
from load import load

MS_PER_TICK = 1000 / 960          # lab data is at 120 bpm
LENS = [0, 25, 50, 75, 100]

def to_ms(c, which='pos2'):
    h = c['h']
    return (c['t'] - h['partPos'] - h[which]) * MS_PER_TICK

TS = -100.0                          # transitions start 100 ms before the boundary

def te_of(direction, blen):
    if direction == 'up': return 96 + 1.51 * blen      # rising, risePort > 0
    if direction == 'down': return 39 + 1.57 * blen   # falling, or rising with risePort 0
    return 98.0                      # phrase start

IV_BUCKETS = [1, 2, 5, 12]

def build_tables():
    S2 = load('set2.csv')
    T = {'rec': {}, 'shape': {}}
    # recovery curves (1 at dip -> 0 at note), per direction and bendLen, sampled each ms after Te
    for k in range(141, 191):
        c = S2[k]; h = c['h']
        if h['e2_0'] != 100: continue
        d = 'up' if h['n2'] > 60 else 'down'
        t = to_ms(c); q = c['porta'] / 100 + 69
        te = te_of(d, h['e2_1'])
        g = np.arange(0, 1001)
        v = np.interp(te + g, t, (q - h['n2']) / -3.0)
        v[g > 950] = 0
        T['rec'][(d, h['e2_1'])] = np.clip(v, -0.5, 1.5)
    iv = [1, 2, 5, 12, -1, -2, -5, -12]
    for k in range(53, 141):
        c = S2[k]; h = c['h']; i = (k - 53) // 11; port = ((k - 53) % 11) * 10
        if port not in (0, 50): continue
        kind = 'down' if iv[i] < 0 or port == 0 else 'up'
        t = to_ms(c); q = c['porta'] / 100 + 69
        te = te_of(kind, 0)
        tau = np.linspace(0, 1, 201)
        u = (np.interp(TS + tau * (te - TS), t, q) - 60) / iv[i]
        T['shape'][(kind, abs(iv[i]), iv[i] > 0)] = u
    # phrase-start recovery from the single-note sweep (dur 1920, bendLen 0..100 step 10)
    for k in range(33, 44):
        c = S2[k]; h = c['h']; t = to_ms(c); p = c['porta'] - c['note']
        g = np.arange(0, 1001)
        v = np.interp(98 + g, t, p / -150.0)
        v[g > 950] = 0
        T['rec'][('start', h['e2_1'])] = np.clip(v, -0.5, 1.5)
    return T

def rec_curve(T, d, blen, x):
    lens = sorted(L for (dd, L) in T['rec'] if dd == d)
    blen = min(max(blen, lens[0]), lens[-1])
    hi = next(L for L in lens if L >= blen); lo = max(L for L in lens if L <= blen)
    f = 0 if hi == lo else (blen - lo) / (hi - lo)
    r = (1 - f) * T['rec'][(d, lo)] + f * T['rec'][(d, hi)]
    return np.interp(x, np.arange(len(r)), r, right=0)

def shape_u(T, kind, interval, rising, tau):
    if interval == 0: interval = 1
    b = IV_BUCKETS
    iv = min(max(interval, b[0]), b[-1])
    hi = next(x for x in b if x >= iv); lo = max(x for x in b if x <= iv)
    f = 0 if hi == lo else (np.log(iv) - np.log(lo)) / (np.log(hi) - np.log(lo))
    g = np.linspace(0, 1, 201)
    s = (1 - f) * T['shape'][(kind, lo, rising)] + f * T['shape'][(kind, hi, rising)]
    return np.interp(tau, g, s)

def model_curve(T, notes, tq):
    """notes: dicts on/off (ms), num, dep, blen, rise. Returns semitones, nan = silent."""
    out = np.full(len(tq), np.nan)
    for j, n in enumerate(notes):
        prev = notes[j - 1] if j > 0 and notes[j - 1]['off'] >= n['on'] - 1 else None
        nxt = notes[j + 1] if j + 1 < len(notes) and notes[j + 1]['on'] <= n['off'] + 1 else None
        rising = prev is not None and n['num'] >= prev['num']
        if prev is None: d = 'start'
        elif rising and n['rise'] > 0: d = 'up'
        else: d = 'down'
        ts = TS if prev is not None else 0
        te = te_of(d, n['blen'])
        dip = n['num'] - 0.03 * n['dep']
        lo = n['on'] + ts; hi = n['off']
        m = (tq >= lo) & (tq < hi)
        t = tq[m] - n['on']
        p = np.empty_like(t)
        a = t < te
        if d == 'start':
            p[a] = n['num'] - 0.5 + (dip - (n['num'] - 0.5)) * np.clip(t[a] / te, 0, 1)
        else:
            tau = np.clip((t[a] - ts) / (te - ts), 0, 1)
            u = shape_u(T, d, abs(n['num'] - prev['num']), rising, tau)
            p[a] = prev['num'] + (dip - prev['num']) * u
        b = ~a
        rk = 'start' if prev is None else ('up' if rising else 'down')
        p[b] = n['num'] + (dip - n['num']) * rec_curve(T, rk, n['blen'], t[b] - te)
        if not nxt:
            L = n['off'] - n['on']
            rel = t > L - 142
            p[rel] -= (t[rel] - (L - 142)) / 142 * 0.45
        out[m] = p
    return out

def notes_of(c):
    h = c['h']
    notes = []
    if h['n1'] >= 0:
        notes.append(dict(on=h['pos1'] * MS_PER_TICK, off=(h['pos1'] + h['dur1']) * MS_PER_TICK, num=h['n1'],
                          dep=h['e1_0'], blen=h['e1_1'], rise=h['e1_2']))
    notes.append(dict(on=h['pos2'] * MS_PER_TICK, off=(h['pos2'] + h['dur2']) * MS_PER_TICK, num=h['n2'],
                      dep=h['e2_0'], blen=h['e2_1'], rise=h['e2_2']))
    return notes

def evaluate(T, C, ids):
    errs = []
    for k in ids:
        c = C[k]; h = c['h']
        if h['vib'][0]: continue
        tq = (c['t'] - h['partPos']) * MS_PER_TICK
        pm = model_curve(T, notes_of(c), tq)
        truth = c['porta'] / 100 + 69
        ok = ~np.isnan(pm)
        if ok.sum() == 0: continue
        e = np.abs(pm[ok] - truth[ok]) * 100
        errs.append((k, e.mean(), np.percentile(e, 95), e.max()))
    return errs

if __name__ == '__main__':
    T = build_tables()
    import pickle; pickle.dump(T, open('tables.pkl', 'wb'))
    for name, path, ids in (('set1 A (both notes same expr)', 'set1.csv', range(0, 270)),
                            ('set1 B (one param)', 'set1.csv', range(270, 318)),
                            ('set1 C (gaps/lengths)', 'set1.csv', range(318, 334)),
                            ('set1 D (single notes)', 'set1.csv', range(334, 343)),
                            ('set2 H (port x interval)', 'set2.csv', range(53, 141)),
                            ('set2 H2 (dep/len transitions)', 'set2.csv', range(141, 191))):
        C = load(path)
        e = np.array([x[1:] for x in evaluate(T, C, ids)])
        print(f'{name:32s} mean {e[:,0].mean():6.1f}c  p95 {e[:,1].mean():6.1f}c  worst {e[:,2].max():6.1f}c')
