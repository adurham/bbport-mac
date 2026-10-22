#!/usr/bin/env python3
"""Which encoders carry the object-motion attachment, and which of them have moving draws.

    python3 tools/macos/motion_pass_census.py out/pass-TAG.txt

Needs a BB_PASS_TRACE from bench/motion-attachment (lines end in mv=<moving> ms=<still> motion
draws). An encoder carries the motion attachment when nc=8 (the G-buffer: 7 guest targets + the
port's RGBA32F slot 7). Reports per frame: such encoders, their motion draws (moving / still),
the encoders with NO moving draw (they could run without the attachment's load/store), and the
same for encoders with no motion draw at all.
"""
import collections
import sys


def main():
    frames = collections.defaultdict(list)
    for line in open(sys.argv[1]):
        if not line.startswith('PT'):
            continue
        p = line.split()
        kv = dict(x.split('=', 1) for x in p[4:] if '=' in x)
        if 'mv' not in kv:
            # PassTraceExternal lines (present pass, scene resample) carry no draw columns.
            kv['mv'] = kv['ms'] = '0'
        frames[int(p[1])].append(dict(t=p[3], nc=int(kv['nc']), mv=int(kv['mv']), ms=int(kv['ms']),
                                      dr=int(kv['dr'])))
    # Drop the window's first and last frame (partial).
    keys = sorted(frames)[1:-1]
    n = len(keys)
    tot = collections.Counter()
    runs = collections.Counter()   # length of maximal runs of motion-free encoders between movers
    for f in keys:
        enc = [e for e in frames[f] if e['nc'] == 8]
        tot['enc'] += len(enc)
        tot['mv'] += sum(e['mv'] for e in enc)
        tot['ms'] += sum(e['ms'] for e in enc)
        tot['dr'] += sum(e['dr'] for e in enc)
        tot['enc_moving'] += sum(1 for e in enc if e['mv'] > 0)
        tot['enc_no_moving'] += sum(1 for e in enc if e['mv'] == 0)
        tot['enc_no_motion'] += sum(1 for e in enc if e['mv'] == 0 and e['ms'] == 0)
        tot['ms_in_no_moving'] += sum(e['ms'] for e in enc if e['mv'] == 0)
        # Index of the last encoder with a moving draw: after it, nothing in the frame writes a
        # vector the compose can accept.
        last = max((i for i, e in enumerate(enc) if e['mv'] > 0), default=-1)
        first = min((i for i, e in enumerate(enc) if e['mv'] > 0), default=len(enc))
        tot['enc_before_first_mover'] += first
        tot['enc_after_last_mover'] += len(enc) - 1 - last
        tot['all_motion'] += sum(1 for e in frames[f] if e['mv'] or e['ms'])
        tot['all_mv'] += sum(e['mv'] for e in frames[f])
        tot['all_ms'] += sum(e['ms'] for e in frames[f])
    print(f'frames {n}')
    for k in ['enc', 'dr', 'mv', 'ms', 'enc_moving', 'enc_no_moving', 'enc_no_motion',
              'ms_in_no_moving', 'enc_before_first_mover', 'enc_after_last_mover', 'all_motion',
              'all_mv', 'all_ms']:
        print(f'{k:>24} {tot[k] / n:8.2f} /frame')


if __name__ == '__main__':
    main()
