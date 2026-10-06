import os, sys, numpy as np
P = os.environ.get('LABCSV', 'set2.csv')

def parse_header(line):
    tok = line.strip().split(',')
    h = {}; i = 2
    while i < len(tok):
        name = tok[i]
        if name == 'vib':
            h['vib'] = [int(x) for x in tok[i+1:i+4]]; i += 4; continue
        v = tok[i+1] if i + 1 < len(tok) else ''
        try: h[name] = int(v)
        except ValueError: h[name] = v
        i += 2
    return int(tok[1]), h

def load(path=None):
    cases = {}; cur = None
    for line in open(path or P):
        if line.startswith('#case'):
            k, h = parse_header(line)
            cur = {'h': h, 'rows': []}; cases[k] = cur
        else:
            cur['rows'].append([float(x) for x in line.split(',')[1:]])
    for c in cases.values():
        a = np.array(c['rows']); c['t'] = a[:, 0]; c['note'] = a[:, 1]; c['porta'] = a[:, 2]; c['pit'] = a[:, 3]; c['final'] = a[:, 4]
        del c['rows']
    return cases
