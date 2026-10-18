#!/usr/bin/env python3
"""Compare an FI-output dump (fi) with the presented pair dump (real/interp) in out/dump.

Reads the raw BGRA dumps written by BB_SEQUENCE_DUMP_AT (presenter.cpp: "real"/"interp", 1512x850
or the window size) and the frame generation's own output ("fi", display size, 2560x1440).

Usage: python3 tools/macos/fg_pair_metrics.py [dir]
"""
import glob
import os
import re
import sys

import numpy as np


def load(path):
    m = re.search(r"_(\d+)x(\d+)_", os.path.basename(path))
    w, h = int(m.group(1)), int(m.group(2))
    return np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)


def luma(a):
    # BGRA bytes; the port dumps the display bytes as they are.
    b, g, r = a[..., 0].astype(np.float64), a[..., 1].astype(np.float64), a[..., 2].astype(np.float64)
    return 0.114 * b + 0.587 * g + 0.299 * r


def fit_gamma(real, other):
    r = real.reshape(-1, 3).mean(axis=1) / 255.0
    o = other.reshape(-1, 3).mean(axis=1) / 255.0
    m = r > 4 / 255.0
    slope, intercept = np.polyfit(np.log(r[m]), np.log(o[m]), 1)
    pred = 255.0 * np.exp(intercept) * np.power(r[m], slope)
    return slope, np.exp(intercept), np.abs(pred - o[m] * 255).mean()


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "out/dump"
    fi = sorted(glob.glob(os.path.join(d, "f*_fi_*.raw")))
    interp = sorted(glob.glob(os.path.join(d, "f*_interp_*.raw")))
    real = sorted(glob.glob(os.path.join(d, "f*_real_*.raw")))
    print(f"{d}: fi={len(fi)} interp={len(interp)} real={len(real)}")
    for name, group in (("real", real), ("interp", interp), ("fi", fi)):
        if group:
            print(f"  {name:7s} luma mean {np.mean([luma(load(p)).mean() for p in group[:8]]):6.2f}"
                  f"  ({os.path.basename(group[0])})")
    if not (fi and interp and real):
        return
    a, b, c = load(real[0]), load(interp[0]), load(fi[0])
    print(f"\nfirst frame: real {luma(a).mean():.2f}  interp {luma(b).mean():.2f}  fi {luma(c).mean():.2f}")
    # fi is at display size, the pair at window size: compare on the mean curve, not per pixel.
    print("interp vs real: gamma fit exponent %.4f scale %.4f residual %.3f/255"
          % fit_gamma(a, b))
    print("fi     vs real: gamma fit exponent %.4f scale %.4f residual %.3f/255"
          % fit_gamma(a, c))
    print("fi     vs interp: gamma fit exponent %.4f scale %.4f residual %.3f/255"
          % fit_gamma(c, b))


if __name__ == "__main__":
    main()
