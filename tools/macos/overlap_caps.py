#!/usr/bin/env python3
"""tools/macos/overlap_caps.py TAG [TAG ...]: pairwise diffs of the port's captures
(out/cap-TAG-N.png, same shot index N across runs): mean abs diff, % pixels off by >8 and >32
(8-bit, max over RGB) and the worst 64x64 tile's mean diff. Pairs whose two tags share an arm
(the text before the first '-' with a trailing 'tr' dropped) are the same-mode noise floor; the
rest are cross-mode. A cross-mode pair above the floor's max on any metric is flagged.
Also reports mean luma per capture (a black world reads ~0)."""
import itertools
import os
import re
import sys

import numpy as np
from PIL import Image

here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def arm(tag):
    a = tag.split('-')[0]
    return a[:-2] if a.endswith('tr') else a


def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)


def diff(a, b):
    d = np.abs(a - b).max(axis=2)
    h, w = d.shape
    t = d[: h // 64 * 64, : w // 64 * 64].reshape(h // 64, 64, w // 64, 64).mean(axis=(1, 3))
    return round(float(d.mean()), 3), round(float((d > 8).mean() * 100), 3), \
        round(float((d > 32).mean() * 100), 3), round(float(t.max()), 2)


def main():
    tags = sys.argv[1:]
    imgs = {}
    for t in tags:
        for n in range(1, 10):
            p = os.path.join(here, 'out', f'cap-{t}-{n}.png')
            if os.path.exists(p):
                imgs[(t, n)] = load(p)
    for (t, n), im in sorted(imgs.items()):
        luma = (0.2126 * im[..., 0] + 0.7152 * im[..., 1] + 0.0722 * im[..., 2]).mean()
        print(f'{t:14s} shot {n} {im.shape[1]}x{im.shape[0]} mean luma {luma:6.2f}')
    same, cross = [], []
    for (a, b) in itertools.combinations(tags, 2):
        for n in range(1, 10):
            if (a, n) in imgs and (b, n) in imgs and imgs[(a, n)].shape == imgs[(b, n)].shape:
                r = (a, b, n) + diff(imgs[(a, n)], imgs[(b, n)])
                (same if arm(a) == arm(b) else cross).append(r)
    hdr = f'{"pair":34s} shot  meanAD  %>8    %>32   worst-tile'
    print('\nsame-mode (noise floor):'); print(hdr)
    for r in same:
        print(f'{r[0] + " vs " + r[1]:34s} {r[2]:4d} {r[3]:7.3f} {r[4]:6.3f} {r[5]:6.3f} {r[6]:8.2f}')
    floor = [max((r[i] for r in same), default=None) for i in range(3, 7)]
    print('\ncross-mode:'); print(hdr)
    for r in cross:
        flag = ''
        if all(f is not None for f in floor):
            flag = ' ABOVE FLOOR' if any(r[3 + i] > floor[i] for i in range(4)) else ' within floor'
        print(f'{r[0] + " vs " + r[1]:34s} {r[2]:4d} {r[3]:7.3f} {r[4]:6.3f} {r[5]:6.3f} {r[6]:8.2f}{flag}')
    print('\nfloor max (meanAD, %>8, %>32, worst tile):', floor)


if __name__ == '__main__':
    main()
