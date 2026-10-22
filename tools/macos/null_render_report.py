#!/usr/bin/env python3
"""tools/macos/null_render_report.py [--out DIR] TAG...: the BB_NULL_RENDER window table.

Per run (out/frames-TAG.csv + out/run-TAG.log): frame_ms p50/p99 in three windows of gameplay time
(t=0 = first frame-log row with >= 500 draws): 0-60 s, 60-110 s (flat), the last 75 s (late), plus
the guest-side proxies for the same windows: game process CPU/frame, bb-gpu present_wait,
submissions/frame and the guest's wait for the previous frame. From the run log's per-5 s
'Game process: CPU by thread' lines: GXRenderThread's % of one core (its CPU ms per frame = % x
frame_ms / 100) and the game's total. Decision rule (task brief): late p50 >= 18 ms -> renderer
work capped (guest-side); <= 14 ms -> renderer work in play; between -> both + the GPU's share.
Writes docs/null-render/report.json.
"""
import csv
import json
import os
import re
import statistics
import sys

here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def pct(xs, q):
    xs = sorted(xs)
    return round(xs[min(len(xs) - 1, int(len(xs) * q))], 2) if xs else None


def med(xs):
    xs = [x for x in xs if x is not None]
    return round(statistics.median(xs), 2) if xs else None


def num(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError, TypeError):
        return None


def thread_lines(log):
    """(run-log line index, GXRenderThread %, game total %) per 'Game process: CPU by thread' line,
    and the gameplay start's line index (first 'Frame stats' line with >= 500 draws/frame)."""
    out, start, stats_i = [], None, 0
    if not os.path.exists(log):
        return out, start
    for line in open(log, errors='replace'):
        if line.startswith('Frame stats'):
            stats_i += 1
            m = re.search(r'(\d+) draws/frame', line)
            if start is None and m and int(m.group(1)) >= 500:
                start = stats_i
        if line.startswith('Game process: CPU by thread'):
            g = re.search(r'game (\d+)', line)
            r = re.search(r'GXRenderThread (\d+)', line)
            out.append((stats_i, int(r.group(1)) if r else None, int(g.group(1)) if g else None))
    return out, start


def main():
    args = sys.argv[1:]
    out_dir = os.path.join(here, 'out')
    if '--out' in args:
        out_dir = args[args.index('--out') + 1]
        args = [a for a in args if a not in ('--out', out_dir)]
    report = {}
    for tag in args:
        path = os.path.join(out_dir, f'frames-{tag}.csv')
        if not os.path.exists(path):
            print(f'{tag}: no frame log'); continue
        rows = list(csv.DictReader(l for l in open(path) if not l.startswith('#')))
        game = [r for r in rows if (num(r, 'draws') or 0) >= 500]
        if not game:
            print(f'{tag}: never reached gameplay (no row >= 500 draws) - not a number'); continue
        t0 = float(game[0]['t_s'])
        end = float(rows[-1]['t_s']) - t0
        wins = {'0-60': (0, 60), '60-110': (60, 110), f'last75 ({end - 75:.0f}-{end:.0f})': (end - 75, end + 1)}
        res = {'gameplay_s': round(end, 1), 'windows': {}}
        tl, tl_start = thread_lines(os.path.join(out_dir, f'run-{tag}.log'))
        for name, (lo, hi) in wins.items():
            w = [r for r in rows if lo <= float(r['t_s']) - t0 < hi]
            fm = [num(r, 'frame_ms') for r in w if num(r, 'frame_ms')]
            d = dict(frames=len(w), frame_p50=pct(fm, 0.5), frame_p99=pct(fm, 0.99))
            for k in ('game_cpu_ms', 'present_wait_ms', 'submissions', 'draws', 'dispatches',
                      'stage_a_cpu_ms', 'game_fault_ms', 'b_busy_ms'):
                d[k] = med([num(r, k) for r in w])
            # 5 s stats windows -> gameplay seconds (approximate: one line per >= 5 s window)
            if tl_start is not None:
                sel = [x for x in tl if lo <= (x[0] - tl_start) * 5 < hi]
                d['gxrender_pct'] = med([x[1] for x in sel])
                d['game_threads_pct'] = med([x[2] for x in sel])
                if d['gxrender_pct'] is not None and d['frame_p50']:
                    d['gxrender_cpu_ms_per_frame'] = round(d['gxrender_pct'] * d['frame_p50'] / 100, 2)
            res['windows'][name] = d
        late = list(res['windows'].values())[-1]['frame_p50']
        res['verdict'] = ('renderer work CAPPED - the guest paces late (>= 18 ms)' if late >= 18 else
                          'renderer work IN PLAY late (<= 14 ms)' if late <= 14 else
                          'in between (14-18 ms): report both numbers')
        log = os.path.join(out_dir, f'run-{tag}.log')
        if os.path.exists(log):
            text = open(log, errors='replace').read()
            res['null_render_lines'] = len(re.findall(r'^Null render', text, re.M))
            res['asserts'] = {'gcn_to_vk.cpp:422': text.count('gcn_to_vk.cpp:422'),
                              'cp_graphics.cpp:203': text.count('cp_graphics.cpp:203')}
            waits = re.findall(r'guest waited for the previous frame ([\d.]+)x ([\d.]+) ms/frame', text)
            if waits:
                res['guest_prev_frame_wait_ms_last'] = float(waits[-1][1])
        report[tag] = res
        print(f'== {tag}  (gameplay {end:.0f} s, null lines {res.get("null_render_lines")}, asserts {res.get("asserts")})')
        print(f'{"window":22s} {"n":>5s} {"p50":>6s} {"p99":>6s} {"gameCPU":>8s} {"pwait":>6s} {"subs":>5s} {"draws":>6s} {"GXRT%":>6s} {"GXRTms":>7s}')
        for name, d in res['windows'].items():
            print(f'{name:22s} {d["frames"]:5d} {d["frame_p50"]!s:>6s} {d["frame_p99"]!s:>6s} {d["game_cpu_ms"]!s:>8s} '
                  f'{d["present_wait_ms"]!s:>6s} {d["submissions"]!s:>5s} {d["draws"]!s:>6s} '
                  f'{d.get("gxrender_pct")!s:>6s} {d.get("gxrender_cpu_ms_per_frame")!s:>7s}')
        print('verdict:', res['verdict'])
    dst = os.path.join(here, 'docs', os.environ.get('REPORT_DIR', 'null-render'))
    os.makedirs(dst, exist_ok=True)
    if report:
        with open(os.path.join(dst, 'report.json'), 'w') as f:
            json.dump(report, f, indent=1)


if __name__ == '__main__':
    main()
