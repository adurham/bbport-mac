#!/usr/bin/env python3
"""tools/macos/motion_dump_compare.py DIR_A DIR_B [DIR_C ...]: compare BB_DUMP_TRIGGER dumps of the
object-motion inputs (bench/motion-fast).

Each DIR holds fNNN_<name>_<w>x<h>_<format>.raw from one run (out/dump-TAG). For every pair of
directories (A vs B, A vs C, ...) and every image kind (objects rgba32f: the raw G-buffer motion
attachment, x/y pixels, z valid, w depth; motion rg16f: the resolved vectors FSR 3 reads; depth f32)
it reports, for the frame index both have:

  - bit-identical pixels (%),
  - objects: valid-pixel count (z > 0.99) each side and their overlap, the mean/p99/max |dxy|
    difference over pixels valid on both sides, and the share of pixels whose validity flips,
  - motion: mean/p99/max |dxy| difference (pixels), share of pixels differing by > 0.01 px.

Standing route: camera still, so two runs at the same session age see the same pose up to the game's
own animation (wind, idle, cloth). The verdict is relative: an A/B difference (gate off vs on) must
sit inside the same-mode difference (off vs off) to call the vectors unchanged.
Writes JSON to --json PATH if given.
"""
import json
import math
import os
import re
import struct
import sys
from array import array

PAT = re.compile(r'f(\d+)_(\w+?)_(\d+)x(\d+)_(\w+)\.raw$')


def load(path, fmt):
    data = open(path, 'rb').read()
    if fmt == 'rgba32f' or fmt == 'f32':
        a = array('f'); a.frombytes(data); return a
    if fmt == 'rg16f':
        n = len(data) // 2
        return struct.unpack(f'<{n}e', data)
    raise ValueError(fmt)


def files(d):
    out = {}
    for n in os.listdir(d):
        m = PAT.match(n)
        if m:
            out[(int(m.group(1)), m.group(2))] = (os.path.join(d, n), int(m.group(3)), int(m.group(4)), m.group(5))
    return out


def pct(xs, q):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * q))]


def compare(pa, pb, kind, fmt, w, h):
    a = load(pa, fmt); b = load(pb, fmt)
    raw_a = open(pa, 'rb').read(); raw_b = open(pb, 'rb').read()
    res = {'bytes_identical': raw_a == raw_b}
    if kind == 'objects':
        n = w * h
        same = sum(1 for i in range(n) if raw_a[16 * i:16 * i + 16] == raw_b[16 * i:16 * i + 16])
        va = vb = both = flips = 0
        diffs = []
        for i in range(n):
            ia = a[4 * i + 2] > 0.99; ib = b[4 * i + 2] > 0.99
            va += ia; vb += ib
            if ia != ib:
                flips += 1
            elif ia:
                both += 1
                dx = a[4 * i] - b[4 * i]; dy = a[4 * i + 1] - b[4 * i + 1]
                diffs.append(math.hypot(dx, dy))
        res.update(pixels_identical_pct=round(100 * same / n, 4), valid_a=va, valid_b=vb,
                   valid_both=both, validity_flip_pct=round(100 * flips / n, 4),
                   dxy_mean=round(sum(diffs) / len(diffs), 5) if diffs else None,
                   dxy_p99=round(pct(diffs, 0.99), 5) if diffs else None,
                   dxy_max=round(max(diffs), 5) if diffs else None)
    elif kind == 'motion':
        n = w * h
        same = sum(1 for i in range(n) if raw_a[4 * i:4 * i + 4] == raw_b[4 * i:4 * i + 4])
        diffs = [math.hypot(a[2 * i] - b[2 * i], a[2 * i + 1] - b[2 * i + 1]) for i in range(n)]
        diffs = [d for d in diffs if not math.isnan(d)]
        res.update(pixels_identical_pct=round(100 * same / n, 4),
                   dxy_mean=round(sum(diffs) / len(diffs), 5), dxy_p99=round(pct(diffs, 0.99), 5),
                   dxy_max=round(max(diffs), 5),
                   over_0p01_pct=round(100 * sum(1 for d in diffs if d > 0.01) / len(diffs), 4))
    else:
        n = len(a)
        same = sum(1 for i in range(n) if raw_a[4 * i:4 * i + 4] == raw_b[4 * i:4 * i + 4])
        diffs = [abs(a[i] - b[i]) for i in range(n)]
        res.update(pixels_identical_pct=round(100 * same / n, 4),
                   abs_mean=sum(diffs) / n, abs_max=max(diffs))
    return res


def main():
    args = sys.argv[1:]
    jpath = None
    if '--json' in args:
        jpath = args[args.index('--json') + 1]
        args = [x for x in args if x not in ('--json', jpath)]
    dirs = args
    base = files(dirs[0])
    report = {}
    for other in dirs[1:]:
        fo = files(other)
        key = f'{os.path.basename(dirs[0].rstrip("/"))} vs {os.path.basename(other.rstrip("/"))}'
        report[key] = {}
        for (frame, kind), (pa, w, h, fmt) in sorted(base.items()):
            if (frame, kind) not in fo:
                continue
            pb, w2, h2, fmt2 = fo[(frame, kind)]
            if (w, h, fmt) != (w2, h2, fmt2):
                report[key][f'f{frame}_{kind}'] = 'size/format differs'
                continue
            r = compare(pa, pb, kind, fmt, w, h)
            report[key][f'f{frame}_{kind}'] = r
            print(key, f'f{frame}', kind, json.dumps(r))
    if jpath:
        json.dump(report, open(jpath, 'w'), indent=1)


if __name__ == '__main__':
    main()
