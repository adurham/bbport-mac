#!/usr/bin/env python3
"""Price the object-motion attachment (the G-buffer's 8th, RGBA32F target) from existing runs.

    python3 tools/macos/motion_attach_price.py CTRL_XML MOTOFF_XML CTRL_PASS MOTOFF_PASS [W H]

Per Metal trace (metal-gpu-intervals export, depth-0 bb-gpu intervals as mtl_encoders.py), the
render encoders are bucketed by their vertex time (the G-buffer encoders are the heavy-vertex
ones; docs/VERTEX_PASSES_2026-10-21.md) and the vertex/fragment time per frame of each bucket is
compared control vs BB_OBJECT_MOTION=0. Fragment time is where an attachment's tile LOAD and
STORE actions run on a TBDR GPU, so the fragment delta in the G-buffer encoders bounds what the
attachment's load+store (plus the motion FS write) cost; the vertex delta is the epilogue.
From the pass traces: G-buffer encoders/frame carrying the attachment (nc=8 in the control),
and the bytes its load/store move per frame (W x H x 16 per action).
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from mtl_gaps import load, txt  # noqa: E402

BUCKETS = [(100e3, '>=100us'), (20e3, '20-100us'), (10e3, '10-20us'), (0, '<10us')]


def encoders(path):
    iv = []
    for v in load(path):
        proc = v.get('process')
        if proc is None or 'bb-gpu' not in (proc.get('fmt') or ''):
            continue
        if ('event-depth' in v and v['event-depth'].get('fmt')) != '0':
            continue
        iv.append((int(txt(v['start'])), int(txt(v['duration'])), v['channel-name'].get('fmt'),
                   v['cmdbuffer-id'].get('fmt') if 'cmdbuffer-id' in v else '',
                   v['encoder-id'].get('fmt') if 'encoder-id' in v else ''))
    t0 = min(s for s, *_ in iv)
    t1 = max(s + d for s, d, *_ in iv)
    enc = collections.defaultdict(lambda: dict(V=0, F=0))
    for s, d, ch, cb, e in iv:
        if ch == 'Vertex':
            enc[(cb, e)]['V'] += d
        elif ch == 'Fragment':
            enc[(cb, e)]['F'] += d
    return [e for e in enc.values() if e['V'] or e['F']], (t1 - t0) / 1e6


def price(path, frames):
    encs, span = encoders(path)
    out = {}
    for lo, name in BUCKETS:
        sel = [e for e in encs if e['V'] >= lo and not any(e['V'] >= l2 for l2, _ in BUCKETS if l2 > lo)]
        out[name] = (len(sel) / frames, sum(e['V'] for e in sel) / 1e6 / frames,
                     sum(e['F'] for e in sel) / 1e6 / frames)
    return out, frames


def pass_stats(path):
    frames = set()
    g = collections.Counter()
    for line in open(path):
        if not line.startswith('PT'):
            continue
        p = line.split()
        frames.add(p[1])
        kv = dict(x.split('=', 1) for x in p[4:] if '=' in x)
        if p[3] == '1146x480' and kv['c0'] == 'R8G8B8A8Unorm' and kv['d'].startswith('D32'):
            g[int(kv['nc'])] += 1
    return {k: v / len(frames) for k, v in g.items()}


def main():
    ctrl, motoff, cpass, mpass = sys.argv[1:5]
    w, h = (int(sys.argv[5]), int(sys.argv[6])) if len(sys.argv) > 6 else (1146, 480)
    # Frames in each trace window: the count mtl_encoders.py reports for the same export
    # (CTRL_FRAMES / MOTOFF_FRAMES).
    rc, fc = price(ctrl, float(os.environ['CTRL_FRAMES']))
    rm, fmo = price(motoff, float(os.environ['MOTOFF_FRAMES']))
    print(f'frames: control {fc:.0f}, motion-off {fmo:.0f}')
    print(f'{"bucket":>10} {"enc/f c":>8} {"enc/f m":>8} {"V c":>7} {"V m":>7} {"dV":>7} {"F c":>7} {"F m":>7} {"dF":>7}')
    for _, name in BUCKETS:
        a, b = rc[name], rm[name]
        print(f'{name:>10} {a[0]:8.1f} {b[0]:8.1f} {a[1]:7.3f} {b[1]:7.3f} {b[1]-a[1]:+7.3f} '
              f'{a[2]:7.3f} {b[2]:7.3f} {b[2]-a[2]:+7.3f}')
    pc, pm = pass_stats(cpass), pass_stats(mpass)
    print('G-buffer encoders/frame (1146x480 RGBA8+D32) by colour-attachment count: control',
          {k: round(v, 2) for k, v in sorted(pc.items())}, 'motion-off',
          {k: round(v, 2) for k, v in sorted(pm.items())})
    n = pc.get(8, 0)
    mb = w * h * 16 / 2**20
    print(f'attachment {w}x{h} RGBA32F = {mb:.1f} MiB; {n:.1f} encoders/frame carry it: '
          f'store {n:.1f} x {mb:.1f} = {n*mb:.0f} MiB, load {max(n-1,0):.1f} x {mb:.1f} = '
          f'{max(n-1,0)*mb:.0f} MiB per frame (first encoder clears)')


if __name__ == '__main__':
    main()
