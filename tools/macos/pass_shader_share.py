#!/usr/bin/env python3
"""BB_PASS_TRACE file -> per vertex shader draws and index-vertices per frame (bench/vertex-shaders).

    python3 tools/macos/pass_shader_share.py out/pass-TAG.txt [hash ...]

Exact per-draw attribution is not in the trace (an encoder line carries its draws/indices and the
SET of shaders), so an encoder with k vertex shaders splits its indices k ways (approximation,
flagged as 'shared'); 'solo' is the part from encoders that hold only that shader (exact).
"""
import collections, sys

path = sys.argv[1]
want = [h.rjust(16, '0') for h in sys.argv[2:]]
tot = collections.Counter(); solo = collections.Counter(); dr = collections.Counter()
frames = set(); total = 0
for line in open(path):
    if not line.startswith('PT '):
        continue
    p = line.split(); frames.add(int(p[1]))
    d = dict(x.split('=', 1) for x in p[3:] if '=' in x)
    if d.get('vs', '-') == '-':
        continue
    hs = d['vs'].split(','); idx = int(d['idx']); total += idx
    for h in hs:
        tot[h] += idx / len(hs); dr[h] += int(d['dr']) / len(hs)
        if len(hs) == 1:
            solo[h] += idx
n = len(frames)
print(f'frames {n}, index-vertices/frame {total / n / 1e6:.3f} M')
for h in (want or [h for h, _ in tot.most_common(12)]):
    print(f'{h} draws/fr {dr[h] / n:6.1f}  M idx/fr {tot[h] / n / 1e6:6.3f}  (solo-encoder {solo[h] / n / 1e6:.3f})')
