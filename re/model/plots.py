from load import load
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
C = load()
def sel(**kw):
    out = []
    for k, c in C.items():
        h = c['h']
        if k < 270 and all(h.get(a) == b for a, b in kw.items()): out.append((k, c))
    return out
fig, ax = plt.subplots(2, 2, figsize=(16, 10))
# bendDep sweep (+7, bendLen 50, port 50)
for k, c in sel(n2=67, e1_1=50, e1_2=50):
    h = c['h']; t = c['t'] - h['partPos']; ax[0][0].plot(t, c['porta'] / 100 + 69, label=f"dep {h['e1_0']}")
ax[0][0].set_title('+7, bendLen 50, port 50: bendDep'); ax[0][0].legend()
for k, c in sel(n2=67, e1_0=50, e1_2=50):
    h = c['h']; t = c['t'] - h['partPos']; ax[0][1].plot(t, c['porta'] / 100 + 69, label=f"len {h['e1_1']}")
ax[0][1].set_title('+7, bendDep 50, port 50: bendLen'); ax[0][1].legend()
for k, c in sel(n2=67, e1_0=0, e1_1=0):
    h = c['h']; t = c['t'] - h['partPos']; ax[1][0].plot(t, c['porta'] / 100 + 69, label=f"port {h['e1_2']}")
ax[1][0].set_title('+7, bend 0: port'); ax[1][0].legend()
for k, c in sel(n2=53, e1_0=0, e1_1=0):
    h = c['h']; t = c['t'] - h['partPos']; ax[1][1].plot(t, c['porta'] / 100 + 69, label=f"port {h['e1_2']}")
ax[1][1].set_title('-7, bend 0: port'); ax[1][1].legend()
for a in ax.flat: a.grid(True); a.set_xlim(1700, 3900)
plt.tight_layout(); plt.savefig('sweep1.png', dpi=60)
