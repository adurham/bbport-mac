#!/usr/bin/env python3
"""Metal System Trace (metal-gpu-intervals export) -> per-ENCODER vertex/fragment/compute split.

    python3 tools/macos/mtl_encoders.py out/mtl-TAG-gpu.xml [frames] [--json out.json]

Answers three questions (bench/null-render, docs/NULL_RENDER_2026-10-21.md):
  (a) Vertex vs fragment vs compute per encoder, and whether vertex time is FLAT per render
      encoder (a per-encoder kick/tiler floor, independent of geometry) or CONCENTRATED in a few
      heavy encoders (geometry: shadow cascades and the like). Reported as the vertex-duration
      distribution, the floor (median of the light encoders) x encoder count, and the share of
      vertex time owned by the top 5/10/20 encoders per frame.
  (b) Overlap: for consecutive render encoders, does encoder N's vertex phase start before
      encoder N-1's fragment phase ends? (Apple's TBDR can run a pass's tiler under the previous
      pass's fragment work when nothing forces a dependency.) Plus total >=2-channel time.
  (c) Render-encoder count per frame (an encoder = one (cmdbuffer-id, encoder-id) carrying a
      Vertex and/or Fragment interval), to compare with the guest's pass count from the run log.
Depth-0 intervals of bb-gpu only, as tools/macos/mtl_gaps.py.
"""
import collections
import json
import statistics
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from mtl_gaps import load, txt  # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    jpath = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if jpath in args:
        args.remove(jpath)
    rows = load(args[0])
    iv = []
    for v in rows:
        proc = v.get('process')
        if proc is None or 'bb-gpu' not in (proc.get('fmt') or ''):
            continue
        if ('event-depth' in v and v['event-depth'].get('fmt')) != '0':
            continue
        iv.append(dict(
            s=int(txt(v['start'])), d=int(txt(v['duration'])),
            ch=v['channel-name'].get('fmt'),
            cb=v['cmdbuffer-id'].get('fmt') if 'cmdbuffer-id' in v else '',
            enc=v['encoder-id'].get('fmt') if 'encoder-id' in v else '',
            frame=int(txt(v['frame-number'])) if 'frame-number' in v and txt(v['frame-number']) else -1,
        ))
    t0 = min(r['s'] for r in iv)
    t1 = max(r['s'] + r['d'] for r in iv)
    span_ms = (t1 - t0) / 1e6
    frames = float(args[1]) if len(args) > 1 else max(1, len({r['frame'] for r in iv}))

    enc = collections.OrderedDict()
    for r in sorted(iv, key=lambda r: r['s']):
        e = enc.setdefault((r['cb'], r['enc']), dict(V=0, F=0, C=0, B=0, vs=None, ve=None, fs=None, fe=None,
                                                      s=r['s'], vi=[], fi=[]))
        k = {'Vertex': 'V', 'Fragment': 'F', 'Compute': 'C'}.get(r['ch'], 'B')
        e[k] += r['d']
        if k in ('V', 'F'):
            e[k.lower() + 'i'].append((r['s'], r['s'] + r['d']))
        if k == 'V':
            e['vs'] = r['s'] if e['vs'] is None else min(e['vs'], r['s'])
            e['ve'] = max(e['ve'] or 0, r['s'] + r['d'])
        if k == 'F':
            e['fs'] = r['s'] if e['fs'] is None else min(e['fs'], r['s'])
            e['fe'] = max(e['fe'] or 0, r['s'] + r['d'])
    render = [e for e in enc.values() if e['V'] or e['F']]
    compute = [e for e in enc.values() if e['C'] and not (e['V'] or e['F'])]
    render.sort(key=lambda e: e['vs'] if e['vs'] is not None else e['fs'])

    out = {'frames': frames, 'span_ms': round(span_ms, 1), 'ms_per_frame': round(span_ms / frames, 2)}
    out['render_encoders_per_frame'] = round(len(render) / frames, 1)
    out['compute_encoders_per_frame'] = round(len(compute) / frames, 1)
    out['vertex_only'] = round(sum(1 for e in render if e['V'] and not e['F']) / frames, 1)
    out['fragment_only'] = round(sum(1 for e in render if e['F'] and not e['V']) / frames, 1)
    for k, name in (('V', 'vertex'), ('F', 'fragment'), ('C', 'compute')):
        out[name + '_ms_per_frame'] = round(sum(e[k] for e in enc.values()) / 1e6 / frames, 3)

    vs = sorted((e['V'] for e in render if e['V']), reverse=True)
    vtot = sum(vs)
    floor_us = statistics.median(vs[len(vs) // 4:]) / 1e3 if vs else 0  # median of the lighter 75%
    out['vertex_us_median'] = round(statistics.median(vs) / 1e3, 1) if vs else 0
    out['vertex_floor_us'] = round(floor_us, 1)
    out['vertex_floor_ms_per_frame'] = round(floor_us * len(vs) / 1e3 / frames, 3)
    per_frame_n = len(vs) / frames
    for top in (5, 10, 20):
        n = int(round(top * frames))
        out[f'vertex_share_top{top}'] = round(sum(vs[:n]) / max(vtot, 1), 3)
    hist = collections.Counter()
    for d in vs:
        for lo, hi, name in [(0, 10e3, '<10us'), (10e3, 20e3, '10-20us'), (20e3, 50e3, '20-50us'),
                             (50e3, 100e3, '50-100us'), (100e3, 1e12, '>=100us')]:
            if lo <= d < hi:
                hist[name] += d
                hist[name + '_n'] += 1
    out['vertex_hist'] = {k: round(v / 1e6 / frames, 3) if not k.endswith('_n') else round(v / frames, 1)
                          for k, v in hist.items()}
    # V vs F correlation among render encoders that carry both
    both = [(e['V'], e['F']) for e in render if e['V'] and e['F']]
    if len(both) > 3:
        xs, ys = [a for a, _ in both], [b for _, b in both]
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        sx = sum((x - mx) ** 2 for x in xs) ** 0.5
        sy = sum((y - my) ** 2 for y in ys) ** 0.5
        out['corr_vertex_fragment'] = round(cov / (sx * sy), 3) if sx and sy else None

    # (b) overlap: time where encoder N's vertex intervals actually run concurrently with encoder
    # N-1's fragment intervals (interval intersection, not start/end ordering: an encoder's vertex
    # and fragment phases interleave in tiles, so first/last stamps alone over-report).
    ov_n = ov_ns = pairs = 0
    order_inv = 0
    gaps = []
    for a, b in zip(render, render[1:]):
        if a['fe'] is None or b['vs'] is None:
            continue
        pairs += 1
        inter = 0
        for (fs, fe) in a['fi']:
            for (vs_, ve_) in b['vi']:
                lo, hi = max(fs, vs_), min(fe, ve_)
                if hi > lo:
                    inter += hi - lo
        if inter:
            ov_n += 1
            ov_ns += inter
        if b['vs'] < a['fe']:
            order_inv += 1
        else:
            gaps.append(b['vs'] - a['fe'])
    out['vertex_starts_before_prev_fragment_ends_per_frame'] = round(order_inv / frames, 2)
    out['overlap_pairs'] = pairs
    out['overlap_count_per_frame'] = round(ov_n / frames, 2)
    out['overlap_fraction'] = round(ov_n / max(pairs, 1), 4)
    out['overlap_ms_per_frame'] = round(ov_ns / 1e6 / frames, 3)
    out['frag_end_to_next_vertex_gap_us_median'] = round(statistics.median(gaps) / 1e3, 1) if gaps else None
    # any-channel concurrency
    ev = sorted([(r['s'], 1) for r in iv] + [(r['s'] + r['d'], -1) for r in iv])
    lvl, last, multi = 0, ev[0][0], 0
    for t, k in ev:
        if lvl >= 2:
            multi += t - last
        lvl += k
        last = t
    out['ge2_channels_ms_per_frame'] = round(multi / 1e6 / frames, 3)

    for k, v in out.items():
        print(f'{k:40s} {v}')
    print(f'(render encoders with vertex time: {per_frame_n:.1f}/frame)')
    if jpath:
        with open(jpath, 'w') as f:
            json.dump(out, f, indent=1)


if __name__ == '__main__':
    main()
