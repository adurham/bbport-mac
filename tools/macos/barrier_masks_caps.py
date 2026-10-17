#!/usr/bin/env python3
"""Pairwise capture diffs (out/cap-m*-N.png): mean abs diff, share of pixels off by >8 and >32
(8-bit), and the worst 64x64 tile's mean diff. legacy-vs-legacy pairs are the noise floor (the
scene animates: fog, candles, the idle character), legacy-vs-safe pairs are compared against it."""
import glob, itertools, re
import numpy as np
from PIL import Image

caps = {}
for p in sorted(glob.glob('out/cap-m*-*.png')):
    m = re.match(r'out/cap-m(legacy|safe)(\d+)-(\d+)\.png', p)
    if m:
        caps[(m.group(1), int(m.group(2)), int(m.group(3)))] = p


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)


def diff(a, b):
    d = np.abs(a - b).max(axis=2)
    h, w = d.shape
    t = d[: h // 64 * 64, : w // 64 * 64].reshape(h // 64, 64, w // 64, 64).mean(axis=(1, 3))
    return d.mean(), (d > 8).mean() * 100, (d > 32).mean() * 100, t.max()


imgs = {k: load(p) for k, p in caps.items()}
rows = {'legacy-legacy': [], 'safe-safe': [], 'legacy-safe': []}
for shot in sorted({k[2] for k in imgs}):
    keys = [k for k in imgs if k[2] == shot]
    for a, b in itertools.combinations(sorted(keys), 2):
        if imgs[a].shape != imgs[b].shape:
            continue
        kind = '-'.join(sorted([a[0], b[0]]))
        r = diff(imgs[a], imgs[b])
        rows[kind].append(r)
        print(f'shot {shot} {a[0]}{a[1]} vs {b[0]}{b[1]}: mean {r[0]:.2f}, >8 {r[1]:.2f}%, '
              f'>32 {r[2]:.2f}%, worst tile {r[3]:.1f}')
for kind, rs in rows.items():
    if rs:
        a = np.array(rs)
        print(f'{kind:14s} n={len(rs):2d}  mean diff {a[:, 0].mean():.2f} (max {a[:, 0].max():.2f}), '
              f'>8 {a[:, 1].mean():.2f}%, >32 {a[:, 2].mean():.2f}% (max {a[:, 2].max():.2f}%), '
              f'worst tile max {a[:, 3].max():.1f}')
