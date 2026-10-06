from load import load
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
C = load()
fig, ax = plt.subplots(4, 1, figsize=(16, 14))
n = 0
for k, c in C.items():
    h = c['h']
    if h['vib'][0] == 1 and h['vib'][1] == 99:
        a = ax[min(3, (h['vib'][2] - 1) // 4)]
        t = c['t'] - h['partPos'] - h['pos2']
        a.plot(t, c['final'] - c['pit'], label=f"type {h['vib'][2]}")
for a in ax: a.grid(True); a.legend(fontsize=7); a.set_xlim(-50, 2000)
plt.tight_layout(); plt.savefig('vib.png', dpi=55)
for k, c in C.items():
    h = c['h']
    if h['vib'][0] == 1 and h['vib'][2] == 1:
        t = c['t'] - h['partPos'] - h['pos2']; d = c['final'] - c['pit']
        import numpy as np
        nz = t[np.abs(d) > 0.5]
        print('len', h['vib'][1], 'vib starts at', nz.min() if len(nz) else None)
