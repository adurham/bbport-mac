#!/usr/bin/env python3
"""A/B of BB_FRAME_LOG csvs by draw-count bucket (docs/KK_GPU_WALL.md).

    python3 tools/macos/ab_compare.py out/frames-legacy1.csv out/frames-lite1.csv ...

Steady gameplay frames only: draws >= 500, frame_ms < 100, no pipeline compile on the GPU thread.
Frame time depends strongly on what is on screen (draw count), so the comparison is per bucket;
the 'all' row mixes buckets and is only a sanity check.
"""
import csv
import sys

import numpy as np

COLS = ['frame_ms', 'present_wait_ms', 'stage_a_cpu_ms', 'b_busy_ms', 'cat_emit_ms', 'draws']
BUCKETS = [(500, 650), (650, 750), (750, 850), (850, 950), (950, 1300)]


def load(path):
    rows = []
    with open(path) as f:
        for d in csv.DictReader(line for line in f if not line.startswith('#')):
            try:
                rows.append({k: float(v) for k, v in d.items() if k and v not in (None, '')})
            except ValueError:
                pass
    return [r for r in rows[1:] if r.get('draws', 0) >= 500 and r.get('frame_ms', 999) < 100
            and r.get('pipeline_sync_ms', 0) < 1]


def main():
    print(f'{"run":14s} {"bucket":>9s} {"n":>5s} ' + ' '.join(f'{c[:12]:>12s}' for c in COLS))
    for path in sys.argv[1:]:
        rows = load(path)
        name = path.rsplit('frames-', 1)[-1].removesuffix('.csv')
        for lo, hi in [(0, 99999)] + BUCKETS:
            b = [r for r in rows if lo <= r['draws'] < hi]
            if len(b) < 50:
                continue
            label = 'all' if hi == 99999 else f'{lo}-{hi}'
            print(f'{name:14s} {label:>9s} {len(b):5d} ' +
                  ' '.join(f'{np.median([r.get(c, np.nan) for r in b]):12.2f}' for c in COLS))


if __name__ == '__main__':
    main()
