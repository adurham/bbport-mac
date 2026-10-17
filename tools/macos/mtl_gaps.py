#!/usr/bin/env python3
"""Metal System Trace (metal-gpu-intervals export) -> where the GPU time and the GPU idle go.

    xcrun xctrace record --template 'Metal System Trace' --attach <bb-gpu pid> --time-limit 3s \
        --output out/x.trace
    xcrun xctrace export --input out/x.trace --xpath \
        '/trace-toc/run[1]/data/table[@schema="metal-gpu-intervals"]' > out/x-gpu.xml
    python3 tools/macos/mtl_gaps.py out/x-gpu.xml [frames]

Reports (bb-gpu only, depth-0 intervals = one per encoder per channel):
  - busy (union of all channels) and how much of it has >=2 channels active at once (overlap)
  - fragment/vertex/compute time and per-encoder size distribution per frame
  - every idle gap, classified by what is on both sides of it:
        same-cmdbuf   the gap sits between two encoders of ONE Metal command buffer
        same-submit   between two command buffers of one queue commit (gpu-submission-id)
        cross-submit  between two commits (the CPU had not handed the next one over, or the
                      queue serialised it)
    plus the gap-size histogram, so 'idle' can be attributed to encoder boundaries vs submission
    boundaries.
Frames: given on the command line, else counted from the frame-number column.
"""
import collections
import sys
import xml.etree.ElementTree as ET


def load(path):
    ids = {}
    cols = []
    rows = []

    def reg(e):
        i = e.get('id')
        if i is not None:
            ids[i] = e
        for c in e:
            reg(c)

    for ev, el in ET.iterparse(path, events=('end',)):
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
        # keep el: later rows may ref ids defined inside it
    return rows


def txt(v):
    return v.text if v is not None else None


def main():
    rows = load(sys.argv[1])
    out = []
    for v in rows:
        proc = v.get('process')
        if proc is None or 'bb-gpu' not in (proc.get('fmt') or ''):
            continue
        out.append(dict(
            s=int(txt(v['start'])), d=int(txt(v['duration'])),
            ch=v['channel-name'].get('fmt'),
            depth=v['event-depth'].get('fmt') if 'event-depth' in v else '',
            frame=int(txt(v['frame-number'])) if 'frame-number' in v and txt(v['frame-number']) else -1,
            cb=v['cmdbuffer-id'].get('fmt') if 'cmdbuffer-id' in v else '',
            enc=v['encoder-id'].get('fmt') if 'encoder-id' in v else '',
            sub=txt(v['gpu-submission-id']) if 'gpu-submission-id' in v else '',
            label=(v['event-label'].get('fmt') if 'event-label' in v else '')[:60],
        ))
    d0 = sorted([r for r in out if r['depth'] == '0'], key=lambda r: r['s'])
    t0 = min(r['s'] for r in d0)
    t1 = max(r['s'] + r['d'] for r in d0)
    span = (t1 - t0) / 1e6
    frames = float(sys.argv[2]) if len(sys.argv) > 2 else max(1, len({r['frame'] for r in d0}))
    print(f'bb-gpu depth-0 intervals {len(d0)}, span {span:.1f} ms, frames {frames:.0f} '
          f'({span / frames:.2f} ms/frame)')
    print('channels:', collections.Counter(r['ch'] for r in d0).most_common())

    # busy + overlap via a sweep
    ev = []
    for r in d0:
        ev.append((r['s'], 1))
        ev.append((r['s'] + r['d'], -1))
    ev.sort()
    level = 0
    last = ev[0][0]
    by_level = collections.Counter()
    for t, k in ev:
        by_level[min(level, 3)] += t - last
        level += k
        last = t
    busy = sum(v for k, v in by_level.items() if k >= 1)
    print(f'busy {busy / 1e6 / frames:.2f} ms/frame ({100 * busy / 1e6 / span:.1f}%), '
          f'>=2 channels {by_level[2] / 1e6 / frames + by_level[3] / 1e6 / frames:.2f} ms/frame '
          f'({100 * (by_level[2] + by_level[3]) / max(busy, 1):.1f}% of busy), '
          f'idle {by_level[0] / 1e6 / frames:.2f} ms/frame')
    for ch in ('Fragment', 'Vertex', 'Compute', 'Blit'):
        ds = sorted(r['d'] for r in d0 if r['ch'] == ch)
        if not ds:
            continue
        tot = sum(ds)
        line = f'  {ch:9s} {tot / 1e6 / frames:6.2f} ms/frame, {len(ds) / frames:6.1f} enc/frame |'
        for lo, hi in [(0, 20e3), (20e3, 100e3), (100e3, 500e3), (500e3, 1e12)]:
            b = [x for x in ds if lo <= x < hi]
            line += f' {lo / 1e3:.0f}-{hi / 1e3 if hi < 1e12 else 999:.0f}us {len(b) / frames:5.1f}/{sum(b) / 1e6 / frames:5.2f}ms'
        print(line)
    print(f'  command buffers/frame {len({r["cb"] for r in d0}) / frames:.1f}, '
          f'submissions/frame {len({r["sub"] for r in d0}) / frames:.1f}, '
          f'encoders/frame {len({(r["cb"], r["enc"]) for r in d0}) / frames:.1f}')

    # idle gaps: merge overlapping intervals, keep the last/next interval on each side
    merged = []
    for r in d0:
        e = r['s'] + r['d']
        if merged and r['s'] <= merged[-1]['e']:
            if e > merged[-1]['e']:
                merged[-1]['e'] = e
                merged[-1]['last'] = r
        else:
            merged.append({'s': r['s'], 'e': e, 'first': r, 'last': r})
    kinds = collections.Counter()
    kinds_n = collections.Counter()
    sizes = collections.Counter()
    sizes_n = collections.Counter()
    pairs = collections.Counter()
    for a, b in zip(merged, merged[1:]):
        g = b['s'] - a['e']
        x, y = a['last'], b['first']
        if x['cb'] == y['cb'] and x['cb']:
            k = 'same-cmdbuf'
        elif x['sub'] == y['sub'] and x['sub']:
            k = 'same-submit'
        else:
            k = 'cross-submit'
        kinds[k] += g
        kinds_n[k] += 1
        for lo, hi, name in [(0, 5e3, '<5us'), (5e3, 20e3, '5-20us'), (20e3, 100e3, '20-100us'),
                             (100e3, 500e3, '0.1-0.5ms'), (500e3, 1e12, '>0.5ms')]:
            if lo <= g < hi:
                sizes[name] += g
                sizes_n[name] += 1
                if g >= 100e3:
                    pairs[(k, x['ch'], y['ch'])] += g
    tot = sum(kinds.values())
    print(f'idle gaps {len(merged) - 1} ({(len(merged) - 1) / frames:.1f}/frame), '
          f'{tot / 1e6 / frames:.2f} ms/frame:')
    for k in ('same-cmdbuf', 'same-submit', 'cross-submit'):
        print(f'  {k:13s} {kinds_n[k] / frames:6.1f}/frame {kinds[k] / 1e6 / frames:6.2f} ms/frame '
              f'(mean {kinds[k] / max(kinds_n[k], 1) / 1e3:6.1f} us)')
    for name in ('<5us', '5-20us', '20-100us', '0.1-0.5ms', '>0.5ms'):
        print(f'  size {name:10s} {sizes_n[name] / frames:6.1f}/frame {sizes[name] / 1e6 / frames:6.2f} ms/frame')
    print('  gaps >=0.1 ms by (kind, before, after) channel:')
    for k, v in pairs.most_common(8):
        print(f'    {v / 1e6 / frames:6.2f} ms/frame {k}')


if __name__ == '__main__':
    main()
