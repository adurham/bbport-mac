#!/usr/bin/env python3
"""Steady-state analysis of a BB_FRAME_LOG csv (frame_stats.cpp): what the GPU command thread's
frame costs, per draw and per frame.

    python3 tools/macos/frame_log_regress.py <frames.csv> [more.csv ...]

Frames kept: draws >= 300 (gameplay), frame_ms < 60, no pipeline compile on the GPU thread
(pipeline_sync_ms < 1). Columns used (see the header written by OpenFrameLog):
  stage_a_cpu_ms  the GPU command thread's CPU time (user+sys) over the frame; on macOS sampled at
                  each graphics submission, so it is per-submission granular
  kernel_ms       the same thread's SYSTEM (kernel) CPU time, not GPU time
  gpu_idle_ms     the same thread's wait for guest submissions (CommandProcessor::WaitForWork), not
                  the hardware's idle time
  present_wait_ms Presenter::PrepareFrame's BB_FRAMES_AHEAD wait for an older frame's GPU tick
  tick_wait_ms    every Scheduler::Wait (present_wait is one of them)
And, from this branch's probe (when present):
  ga_wall_ms/ga_user_ms/ga_sys_ms   the GPU command thread's busy wall and Mach CPU split
  ga_drain_ms/ga_drains             of that, stage A waiting for the draw recording thread (stage B)
  ga_vol_waits                      voluntary context switches (blocking events) while busy
  b_busy_ms                         stage B running packets
  b_decode_ms b_pipeline_ms b_bind_ms b_emit_ms   the four categories (see frame_stats.cpp)
"""
import csv
import sys

import numpy as np


def load(path):
    rows = []
    with open(path) as f:
        for d in csv.DictReader(line for line in f if not line.startswith('#')):
            try:
                rows.append({k: float(v) for k, v in d.items() if k and v not in (None, '')})
            except ValueError:
                pass
    return rows


def fit(x, y):
    b, a = np.polyfit(x, y, 1)
    return a, b, float(np.corrcoef(x, y)[0, 1])


def report(path):
    rows = load(path)
    need = ['frame_ms', 'stage_a_cpu_ms', 'draws', 'kernel_ms']
    rows = [r for r in rows if all(k in r for k in need)]
    keep = [i for i in range(1, len(rows)) if rows[i]['draws'] >= 300 and rows[i]['frame_ms'] < 60
            and rows[i].get('pipeline_sync_ms', 0) < 1]
    print(f'== {path}: {len(rows)} frames, {len(keep)} steady gameplay frames')
    if len(keep) < 50:
        return
    s = [rows[i] for i in keep]
    col = lambda k: np.array([r.get(k, np.nan) for r in s])
    f, a, d, k = col('frame_ms'), col('stage_a_cpu_ms'), col('draws'), col('kernel_ms')
    med = {c: float(np.nanmedian(col(c))) for c in
           ['frame_ms', 'stage_a_cpu_ms', 'kernel_ms', 'present_wait_ms', 'tick_wait_ms',
            'gpu_idle_ms', 'game_cpu_ms', 'ipc_ms', 'protect_ms', 'draws'] if c in s[0]}
    print('  medians:', ', '.join(f'{c} {v:.2f}' for c, v in med.items()))
    i0, sl, r = fit(d, a)
    print(f'  stage_a_cpu = {i0:.2f} ms + {sl * 1e3:.2f} us/draw (r {r:.2f})')
    i0, sl, r = fit(d, k)
    print(f'  kernel(sys) = {i0:.2f} ms + {sl * 1e3:.2f} us/draw (r {r:.2f});'
          f' user = stage_a - kernel median {np.median(a - k):.2f} ms')
    X = np.c_[np.ones(len(s)), f, d]
    c, *_ = np.linalg.lstsq(X, a, rcond=None)
    r2 = 1 - ((a - X @ c) ** 2).sum() / ((a - a.mean()) ** 2).sum()
    print(f'  stage_a_cpu = {c[0]:.2f} + {c[1]:.2f}*frame_ms + {c[2] * 1e3:.2f} us/draw (R2 {r2:.2f});'
          f' stage_a/frame median {np.median(a / f):.2f}, corr(stage_a, frame) {np.corrcoef(a, f)[0, 1]:.2f}')
    if 'present_wait_ms' in s[0]:
        prev = np.array([rows[i - 1]['kernel_ms'] for i in keep])
        pw, tw = col('present_wait_ms'), col('tick_wait_ms')
        print(f'  corr present_wait~kernel(same) {np.corrcoef(pw, k)[0, 1]:.2f},'
              f' ~kernel(prev) {np.corrcoef(pw, prev)[0, 1]:.2f};'
              f' tick_wait - present_wait median {np.median(tw - pw):.2f} ms')
    if 'game_cpu_ms' in s[0]:
        print(f'  corr tick_wait~game_cpu {np.corrcoef(col("tick_wait_ms"), col("game_cpu_ms"))[0, 1]:.2f},'
              f' frame~game_cpu {np.corrcoef(f, col("game_cpu_ms"))[0, 1]:.2f}')
    if 'ga_wall_ms' in s[0]:
        probe = ['ga_wall_ms', 'ga_user_ms', 'ga_sys_ms', 'ga_drain_ms', 'ga_drains', 'ga_vol_waits',
                 'b_busy_ms', 'b_decode_ms', 'b_pipeline_ms', 'b_bind_ms', 'b_bind_mb', 'b_emit_ms']
        print('  probe medians:', ', '.join(f'{c} {np.nanmedian(col(c)):.2f}' for c in probe if c in s[0]))
        wall, drain = col('ga_wall_ms'), col('ga_drain_ms')
        work = wall - drain
        i0, sl, r = fit(d, work)
        print(f'  stage A work (busy - drain) = {i0:.2f} ms + {sl * 1e3:.2f} us/draw (r {r:.2f}),'
              f' median {np.median(work):.2f} ms')
        b = col('b_busy_ms')
        i0, sl, r = fit(d, b)
        print(f'  stage B busy = {i0:.2f} ms + {sl * 1e3:.2f} us/draw (r {r:.2f}), median {np.median(b):.2f} ms')


if __name__ == '__main__':
    for p in sys.argv[1:]:
        report(p)
