import sys, numpy as np, pickle
from load import load
from model import *
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
T = pickle.load(open('tables.pkl', 'rb'))
path, lo, hi = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
C = load(path)
e = sorted(evaluate(T, C, range(lo, hi)), key=lambda x: -x[1])[:6]
fig, ax = plt.subplots(2, 3, figsize=(20, 9))
for a, (k, m, p95, mx) in zip(ax.flat, e):
    c = C[k]; h = c['h']; tq = (c['t'] - h['partPos']) * MS_PER_TICK
    a.plot(tq, c['porta'] / 100 + 69, 'k', label='VOCALOID')
    a.plot(tq, model_curve(T, notes_of(c), tq), 'r--', label='model')
    a.set_title(f"#{k} {h['n1']}->{h['n2']} e1 {[h[f'e1_{i}'] for i in range(4)]} e2 {[h[f'e2_{i}'] for i in range(4)]} mean {m:.0f}c", fontsize=8)
    a.grid(True); a.set_xlim(h.get('pos1', 0) * MS_PER_TICK - 200 if h['n1'] >= 0 else h['pos2'] * MS_PER_TICK - 200, (h['pos2'] + h['dur2']) * MS_PER_TICK + 100)
ax[0][0].legend(); plt.tight_layout(); plt.savefig('worst.png', dpi=55)
