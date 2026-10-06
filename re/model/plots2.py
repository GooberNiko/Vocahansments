from load import load
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
C = load()
fig, ax = plt.subplots(1, 2, figsize=(16, 5))
for k, c in C.items():
    h = c['h']
    if h['n1'] == -1 and h['vib'][0] == 0:
        t = c['t'] - h['partPos'] - h['pos2']
        a = ax[0] if h['e2_1'] == 50 or h['e2_0'] == 0 else ax[1]
        a.plot(t, c['porta'] - c['note'], label=f"dep {h['e2_0']} len {h['e2_1']}")
for a in ax: a.grid(True); a.legend(); a.set_xlim(-100, 1000)
plt.tight_layout(); plt.savefig('single.png', dpi=60)
