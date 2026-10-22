#!/usr/bin/env python3
"""tools/macos/motion_attach_report.py TAG...: the motion-attachment series table.

Per run: frame_ms p50/p99 over the flat window (60-110 s of gameplay) and the late window (last
75 s), t=0 = the first frame-log row with >= 500 draws (as null_render_report.py); and, from
the run's Metal trace (mtl_encoders.py), vertex / fragment / compute ms per frame plus the
G-buffer encoders' (>= 100 us vertex) fragment time, which is where the motion attachment's
tile load and store run. Writes out/mattach/report.json.
"""
import csv
import json
import os
import subprocess
import sys

here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def pct(xs, q):
    xs = sorted(xs)
    return round(xs[min(len(xs) - 1, int(len(xs) * q))], 2) if xs else None


def windows(tag):
    rows = list(csv.DictReader(open(os.path.join(here, 'out', f'frames-{tag}.csv'))))
    t0 = next((float(r['t_s']) for r in rows if float(r['draws'] or 0) >= 500), None)
    if t0 is None:
        return None
    play = [(float(r['t_s']) - t0, float(r['frame_ms'])) for r in rows if float(r['t_s']) >= t0]
    end = play[-1][0]
    flat = [f for t, f in play if 60 <= t < 110]
    late = [f for t, f in play if t >= end - 75]
    return dict(flat_p50=pct(flat, .5), flat_p99=pct(flat, .99), late_p50=pct(late, .5),
                late_p99=pct(late, .99), flat_n=len(flat), late_n=len(late), play_s=round(end))


def trace(tag):
    xml = os.path.join(here, 'out', f'mtl-{tag}-gpu.xml')
    if not os.path.exists(xml) or os.path.getsize(xml) == 0:
        return {}
    out = subprocess.run([sys.executable, os.path.join(here, 'tools/macos/mtl_encoders.py'), xml],
                         capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        p = line.split(None, 1)
        if len(p) == 2 and p[0] in ('frames', 'vertex_ms_per_frame', 'fragment_ms_per_frame',
                                     'compute_ms_per_frame', 'render_encoders_per_frame'):
            d[p[0]] = float(p[1])
    # G-buffer encoders' fragment time: encoders with >= 100 us of vertex (mtl_encoders' bucket).
    from motion_attach_price import price
    if d.get('frames'):
        b, _ = price(xml, d['frames'])
        d['heavy_enc'] = round(b['>=100us'][0], 1)
        d['heavy_V'] = round(b['>=100us'][1], 3)
        d['heavy_F'] = round(b['>=100us'][2], 3)
    return d


def main():
    res = {}
    for tag in sys.argv[1:]:
        w = windows(tag)
        res[tag] = dict(w or {}, **trace(tag))
    keys = ['flat_p50', 'flat_p99', 'late_p50', 'late_p99', 'vertex_ms_per_frame',
            'fragment_ms_per_frame', 'compute_ms_per_frame', 'heavy_enc', 'heavy_V', 'heavy_F',
            'render_encoders_per_frame', 'frames']
    short = ['flat50', 'flat99', 'late50', 'late99', 'V', 'F', 'C', 'gbEnc', 'gbV', 'gbF', 'renc', 'trFr']
    print(f'{"tag":>10} ' + ' '.join(f'{s:>7}' for s in short))
    for tag, d in res.items():
        print(f'{tag:>10} ' + ' '.join(f'{d.get(k, float("nan")):7.2f}' if d.get(k) is not None
                                        else f'{"-":>7}' for k in keys))
    os.makedirs(os.path.join(here, 'out', 'mattach'), exist_ok=True)
    json.dump(res, open(os.path.join(here, 'out', 'mattach', 'report.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
