#!/usr/bin/env python3
"""Turn mtl-split-bench ROW lines into the results table + least-squares fits."""
import sys
from collections import defaultdict, OrderedDict

MB = 1e6
rows = defaultdict(list)  # (label, N, enc, draws) -> [fresh_enc_B ...]
reused = defaultdict(list)
reset_B = set()
order = OrderedDict()
for line in open(sys.argv[1]):
    if not line.startswith("ROW "):
        continue
    _, label, n, enc, draws, rep, f_reset, f_enc, f_gpu, u_enc, u_gpu = line.split()
    k = (label, int(n), int(enc), int(draws))
    order[k] = None
    rows[k].append(int(f_gpu))
    reused[k].append(int(u_gpu))
    reset_B.add(int(f_reset))


def lsq(xs, ys):
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    ss_res = sum((y - a - b * x) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys) or 1
    return a, b, 1 - ss_res / ss_tot


def lsq2(x1, x2, ys):
    # y = a + b*x1 + c*x2 via normal equations
    import itertools
    n = len(ys)
    S = [[0.0] * 3 for _ in range(3)]; T = [0.0] * 3
    for u, v, y in zip(x1, x2, ys):
        r = (1.0, u, v)
        for i in range(3):
            T[i] += r[i] * y
            for j in range(3):
                S[i][j] += r[i] * r[j]
    # gaussian elimination
    M = [S[i] + [T[i]] for i in range(3)]
    for i in range(3):
        p = max(range(i, 3), key=lambda r: abs(M[r][i])); M[i], M[p] = M[p], M[i]
        for r in range(3):
            if r != i:
                f = M[r][i] / M[i][i]
                M[r] = [a - f * b for a, b in zip(M[r], M[i])]
    a, b, c = (M[i][3] / M[i][i] for i in range(3))
    my = sum(ys) / n
    ss_res = sum((y - a - b * u - c * v) ** 2 for u, v, y in zip(x1, x2, ys))
    ss_tot = sum((y - my) ** 2 for y in ys) or 1
    return a, b, c, 1 - ss_res / ss_tot


out = []
p = out.append
p("mtl-split-bench — MTL4CommandAllocator.allocatedSize after reset + N command buffers (Apple M4 Max, macOS 27.2)")
p("allocatedSize right after reset (every run): %s B" % sorted(reset_B))
p("fresh = new allocator, reset, encode N CBs, commit, wait; MB = 1e6 B; 3 reps each; spread = max-min")
p("")
p("%-16s %5s %5s %7s %12s %10s %10s %14s" % ("sweep", "N_cb", "encs", "draws", "fresh_MB", "spread_B", "MB/cb", "reused_hwm_MB"))
for k in order:
    label, n, enc, draws = k
    v = rows[k]
    p("%-16s %5d %5d %7d %12.3f %10d %10.3f %14.3f" % (
        label, n, enc, draws, sum(v) / len(v) / MB, max(v) - min(v), sum(v) / len(v) / MB / n, max(reused[k]) / MB))
p("")
p("FITS (least squares on per-rep values)")
for label in ("A_const_total", "B_200_per_cb", "B50_50_per_cb", "C_compute_const", "F_nocommit"):
    ks = [k for k in order if k[0] == label]
    xs, ys = [], []
    for k in ks:
        for y in rows[k]:
            xs.append(k[1]); ys.append(y / MB)
    a, b, r2 = lsq(xs, ys)
    p("  %-16s capacity_MB = %.3f + %.4f * N_cb   (R^2=%.6f)" % (label, a, b, r2))
# joint fit: all render rows with enc_per_cb=1 -> y = a + b*N + c*draws
x1, x2, ys = [], [], []
for k in order:
    if k[0] in ("A_const_total", "B_200_per_cb", "B50_50_per_cb", "D_1cb_draws", "F_nocommit"):
        for y in rows[k]:
            x1.append(k[1]); x2.append(k[3]); ys.append(y / MB)
a, b, c, r2 = lsq2(x1, x2, ys)
p("  joint render     capacity_MB = %.3f + %.4f * N_cb + %.6f * draws   (R^2=%.6f)" % (a, b, c, r2))
ks = [k for k in order if k[0] == "D_1cb_draws" and k[3] >= 1000]
xs = [k[3] for k in ks for _ in rows[k]]; ys = [y / MB for k in ks for y in rows[k]]
a, b, r2 = lsq(xs, ys)
p("  D (1 CB, >=1000 draws) capacity_MB = %.3f + %.6f * draws  (R^2=%.6f) -> %.0f B/draw" % (a, b, r2, b * MB))
ks = [k for k in order if k[0] == "E_1cb_encoders"]
xs = [k[2] for k in ks for _ in rows[k]]; ys = [y / MB for k in ks for y in rows[k]]
a, b, r2 = lsq(xs, ys)
p("  E (1 CB, 0 draws)    capacity_MB = %.3f + %.6f * encoders (R^2=%.6f) -> %.0f B/encoder" % (a, b, r2, b * MB))
print("\n".join(out))
