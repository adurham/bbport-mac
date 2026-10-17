#!/usr/bin/env python3
"""Per-run raw numbers for the BB_BARRIER_MASKS A/B (out/frames-m*.csv, run-m*.log, mtl-m*-gpu.xml)."""
import glob, os, re, subprocess, sys
import numpy as np
sys.path.insert(0, 'tools/macos')  # run from the checkout root
from ab_compare import load

runs = sorted({os.path.basename(p)[7:-4] for p in glob.glob('out/frames-m*.csv')},
              key=lambda t: (int(re.sub(r'\D', '', t) or 0), t))
print(f'{"run":10s} {"n":>5s} {"p50":>7s} {"p25":>7s} {"p75":>7s} {"mean":>7s} {"present":>7s} '
      f'{"enc_waited":>10s} {"before_vtx":>10s} {"vtx%":>6s} {"vtxmem":>8s} {"vtximg":>8s} '
      f'{"busy":>6s} {"ovl%":>6s} {"idle":>6s}')
for t in runs:
    rows = [r for r in load(f'out/frames-{t}.csv') if 850 <= r['draws'] < 950]
    ft = np.array([r['frame_ms'] for r in rows])
    pw = np.array([r.get('present_wait_ms', np.nan) for r in rows])
    log = open(f'out/run-{t}.log', errors='replace').read() if os.path.exists(f'out/run-{t}.log') else ''
    kk = re.findall(r'render encoders that waited \(vk\) (\d+), of them before the vertex stage (\d+); '
                    r'vertex-side barriers from memory/buffer (\d+), from images (\d+)', log)
    w = b = vm = vi = 0
    if len(kk) >= 2:  # last 10 s window: delta of cumulative counters
        a, z = [list(map(int, x)) for x in kk[-2:]]
        w, b, vm, vi = [z[i] - a[i] for i in range(4)]
    busy = ovl = idle = float('nan')
    xml = f'out/mtl-{t}-gpu.xml'
    if os.path.exists(xml) and os.path.getsize(xml) > 1000:
        out = subprocess.run([sys.executable, 'tools/macos/mtl_gaps.py', xml], capture_output=True,
                             text=True).stdout
        m = re.search(r'busy ([\d.]+) ms/frame \(([\d.]+)%\), ([^\n]*)', out)
        if m:
            busy = float(m.group(1))
            o = re.search(r'>=2 channels ([\d.]+) ms/frame \(([\d.]+)% of busy\)', out)
            ovl = float(o.group(2)) if o else float('nan')
        g = re.search(r'idle gaps \d+ \([\d.]+/frame\), ([\d.]+) ms/frame', out)
        idle = float(g.group(1)) if g else float('nan')
    print(f'{t:10s} {len(ft):5d} {np.median(ft):7.2f} {np.percentile(ft, 25):7.2f} '
          f'{np.percentile(ft, 75):7.2f} {ft.mean():7.2f} {np.nanmedian(pw):7.2f} {w:10d} {b:10d} '
          f'{(100 * b / w if w else float("nan")):6.1f} {vm:8d} {vi:8d} {busy:6.2f} {ovl:6.1f} {idle:6.2f}')
