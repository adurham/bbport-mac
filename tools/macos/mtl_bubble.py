#!/usr/bin/env python3
"""The per-frame gap census: find the recurring bubbles in a metal-gpu-intervals export.

    python3 tools/macos/mtl_bubble.py <gpu.xml> <wall_frame_ms> [proc]

Prints
  * the gap-size census (all gaps, and >=150 us with their two sides),
  * the recurrent-bubble class: count/frame, duration mean/sd/min/max, and what
    encoder sits on either side,
  * the phase-locked frame layout (frames anchored on the bubble's end),
  * and a check of whether `start-latency` is commit-based: for two intervals that
    share a cmdbuffer-id, (lat_a - lat_b) must equal (start_a - start_b) exactly.
    If it does, "start-latency >= gap" really does mean the work was committed
    before the GPU went dry.

Same XML export as tools/macos/mtl_gaps.py (xctrace export --xpath ...metal-gpu-intervals).
"""
import bisect
import collections
import statistics
import sys
import xml.etree.ElementTree as ET

path = sys.argv[1]
wall_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
proc = sys.argv[3] if len(sys.argv) > 3 else 'bb-gpu'


def load(path):
    ids, cols = {}, []

    def reg(e):
        i = e.get('id')
        if i is not None:
            ids[i] = e
        for c in e:
            reg(c)

    rows = []
    for _, el in ET.iterparse(path, events=('end',)):
        if el.tag == 'mnemonic':
            cols.append(el.text)
            continue
        if el.tag != 'row':
            continue
        reg(el)
        vals = {}
        for name, k in zip(cols, list(el)):
            if k.tag == 'sentinel':
                continue
            if k.get('ref') is not None:
                k = ids[k.get('ref')]
            vals[name] = k
        rows.append(vals)
    return rows


def txt(v):
    return v.text if v is not None else None


def num(v):
    try:
        return int(txt(v))
    except (TypeError, ValueError):
        return -1


d0 = []
for v in load(path):
    p = v.get('process')
    if p is None or proc not in (p.get('fmt') or ''):
        continue
    if (v['event-depth'].get('fmt') if 'event-depth' in v else '') != '0':
        continue
    d0.append(dict(s=num(v['start']), d=num(v['duration']),
                   ch=v['channel-name'].get('fmt'),
                   fr=num(v['frame-number']) if 'frame-number' in v else -1,
                   lat=num(v['start-latency']) if 'start-latency' in v else -1,
                   cb=txt(v['cmdbuffer-id']) if 'cmdbuffer-id' in v else ''))
d0.sort(key=lambda r: r['s'])
t0, t1 = d0[0]['s'], max(r['s'] + r['d'] for r in d0)
span = (t1 - t0) / 1e6

gaps = []
for i in range(len(d0) - 1):
    a, b = d0[i], d0[i + 1]
    e = a['s'] + a['d']
    if b['s'] > e:
        gaps.append((b['s'] - e, i, a, b))

idle = sum(g[0] for g in gaps) / 1e6
print(f"window {span:.1f} ms, {len(d0)} depth-0 intervals, {len(gaps)} gaps, idle {idle:.2f} ms")
nf = span / wall_ms if wall_ms else 0.0
if wall_ms:
    print(f"wall frame {wall_ms:.2f} ms -> {nf:.1f} frames")
    print(f"idle {idle / nf:.2f} ms/frame")

big = [g for g in gaps if g[0] >= 150000]
print(f"\ngaps >=150 us: {len(big)}, {sum(g[0] for g in big) / 1e6:.2f} ms")
for dur, i, a, b in sorted(big, key=lambda g: -g[0])[:12]:
    print(f"  {dur / 1000:8.0f} us  prev {a['ch'][:4]:4s} {a['d'] / 1000:6.1f} "
          f"(lat {a['lat'] / 1000:6.0f})  next {b['ch'][:4]:4s} {b['d'] / 1000:6.1f} "
          f"(lat {b['lat'] / 1000:6.0f})  {'queued' if b['lat'] >= dur else 'cpu-late'}")

# The recurrent bubble class: the tight cluster of gaps >=1 ms that recurs at the
# frame period. Detected off the gap-size mode above 1 ms rather than hard-coded.
cand = [g for g in gaps if g[0] >= 1000000]
if not cand:
    print("\nno >=1 ms gaps")
    sys.exit(0)
durs = sorted(g[0] / 1000 for g in cand)
mode = statistics.median(durs)
bub = [g for g in cand if 0.85 * mode <= g[0] / 1000 <= 1.15 * mode]
print(f"\nrecurrent bubble class (mode {mode:.0f} us +-15%): {len(bub)} gaps")
bd = [g[0] / 1000 for g in bub]
print(f"  duration mean {statistics.mean(bd):.0f} sd {statistics.pstdev(bd):.0f} "
      f"min {min(bd):.0f} max {max(bd):.0f} us")
if wall_ms:
    print(f"  {len(bub) / nf:.2f} /frame -> {sum(bd) / 1e3 / nf:.2f} ms/frame")
print("  prev channel", collections.Counter(g[2]['ch'] for g in bub).most_common(),
      "prev dur us", statistics.median(g[2]['d'] / 1000 for g in bub))
print("  next channel", collections.Counter(g[3]['ch'] for g in bub).most_common(),
      "next dur us", statistics.median(g[3]['d'] / 1000 for g in bub))
lead = [(g[3]['lat'] - g[0]) / 1000 for g in bub]
print(f"  next-CB latch - gap: mean {statistics.mean(lead):.0f} us  "
      f"(>0: the next command buffer was already committed when the GPU went dry)")

# Phase-locked frame layout, anchored on the bubble's end.
starts = [r['s'] for r in d0]
anch = [g[3]['s'] for g in bub]
per = []
for k in range(len(anch) - 1):
    d = (anch[k + 1] - anch[k]) / 1e6
    if 0.8 * wall_ms < d < 1.2 * wall_ms:
        per.append(d)
if per:
    print(f"\nframe period from bubble-to-bubble: {statistics.mean(per):.2f} ms (n={len(per)})")
    busy = collections.Counter()
    cnt = collections.Counter()
    gph = collections.Counter()
    for k in range(len(anch) - 1):
        s, e = anch[k], anch[k + 1]
        if not (0.8 * wall_ms * 1e6 < e - s < 1.2 * wall_ms * 1e6):
            continue
        i = bisect.bisect_left(starts, s)
        pend = s
        while i < len(d0) and d0[i]['s'] < e:
            r = d0[i]
            b = int((r['s'] - s) / 1e6)
            busy[b] += r['d']
            cnt[b] += 1
            gph[b] += max(0, r['s'] - pend)
            pend = r['s'] + r['d']
            i += 1
        gph[min(18, int((pend - s) / 1e6))] += max(0, e - pend)
    n = len(per)
    print("  ms  busy/frame  gap/frame  intervals/frame")
    for b in range(int(wall_ms) + 1):
        print(f"  {b:2d}  {busy[b] / n / 1e6:10.2f}  {gph[b] / n / 1e6:10.2f}  {cnt[b] / n:8.1f}")

# Is start-latency commit-based? Two intervals of one command buffer were committed
# together, so their latches must differ by exactly their start times.
diffs = []
slopes = []
for i in range(len(d0) - 1):
    a, b = d0[i], d0[i + 1]
    if a['cb'] == b['cb'] and a['lat'] > 0 and b['lat'] > 0 and b['s'] > a['s']:
        diffs.append((a['lat'] - b['lat']) - (a['s'] - b['s']))
        slopes.append((a['lat'] - b['lat']) / (b['s'] - a['s']))
print(f"\nstart-latency basis: {len(diffs)} same-cmdbuffer pairs, "
      f"median |diff| {statistics.median(abs(x) for x in diffs) / 1000:.1f} us, "
      f"median slope {statistics.median(slopes):+.2f}")
print("  slope -1.00 with a zero spread => commit-based: 'queued' vs 'cpu-late' is a real split")
