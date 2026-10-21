#!/usr/bin/env python3
"""Metal System Trace + BB_PASS_TRACE -> vertex time per NAMED render encoder.

    python3 tools/macos/mtl_pass_join.py out/mtl-TAG-gpu.xml out/pass-TAG.txt [--json out.json]

The trace (metal-gpu-intervals) carries per-encoder vertex/fragment time but no identity: the
command buffers a KosmicKrisp backend makes have empty labels. BB_PASS_TRACE (bench/vertex-passes)
writes one line per render encoder from inside the port - target size and attachment formats,
draws, indices (x instances), vertex/pixel shader hashes - in encoder order, segmented by the game's
frame counter. Same run, same frames.

Two joins, in order:
 1. FRAME ORDINAL. Both sides are split into frames; if a frame's encoder counts agree exactly, the
    k-th trace render encoder is the k-th port line. Reported as a match rate; only agreeing frames
    are used.
 2. RANK-SEQUENCE (fallback, when the counts never agree - the trace also carries encoders the port
    does not open through Scheduler::BeginRendering, and the reverse). Both sides are reduced to a
    one-frame template (vertex us per position / indices per position) and aligned by a DP that
    requires order, allows either side to skip, and scores a pair by the difference of the two
    sequences' normalized RANKS (big draws vs big vertex time). The join is only believed if the
    matched pairs' Spearman correlation is high; that number is printed.
"""
import collections
import json
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(__file__))
from mtl_gaps import load, txt  # noqa: E402


# ---------------------------------------------------------------- trace side
def trace_render(path):
    """Ordered render encoders (vertex or fragment time) of bb-gpu, with their frame number."""
    rows = load(path)
    iv = []
    for v in rows:
        proc = v.get('process')
        if proc is None or 'bb-gpu' not in (proc.get('fmt') or ''):
            continue
        if ('event-depth' in v and v['event-depth'].get('fmt')) != '0':
            continue
        iv.append(dict(s=int(txt(v['start'])), d=int(txt(v['duration'])),
                       ch=v['channel-name'].get('fmt'),
                       cb=v['cmdbuffer-id'].get('fmt') if 'cmdbuffer-id' in v else '',
                       enc=v['encoder-id'].get('fmt') if 'encoder-id' in v else '',
                       frame=txt(v['frame-number']) if 'frame-number' in v else None))
    enc = collections.OrderedDict()
    for r in sorted(iv, key=lambda r: r['s']):
        e = enc.setdefault((r['cb'], r['enc']), dict(V=0, F=0, C=0, s=r['s'], frame=r['frame']))
        k = {'Vertex': 'V', 'Fragment': 'F', 'Compute': 'C'}.get(r['ch'])
        if k:
            e[k] += r['d']
    return [e for e in sorted(enc.values(), key=lambda e: e['s']) if e['V'] or e['F']], iv


def trace_frames(path):
    """frame number -> render encoders (numbers when the trace has them, else period-split)."""
    render, iv = trace_render(path)
    numbered = [e for e in render if e['frame'] is not None]
    if len(numbered) > 0.8 * len(render):
        frames = collections.defaultdict(list)
        for e in render:
            frames[e['frame']].append(e)
        return frames, 'frame-number'

    # No frame numbers: split on the sequence's own period. The standing route repeats its frame
    # structure exactly, so the lag whose segments agree best with each other (lowest mean
    # within-position spread of log vertex time) is the encoders-per-frame.
    vs = [math.log1p(e['V'] / 1e3) for e in render]
    best = None
    for lag in range(140, 260):
        if lag * 3 > len(vs):
            break
        nseg = len(vs) // lag
        seg = [vs[k * lag:(k + 1) * lag] for k in range(nseg)]
        spread = statistics.fmean(
            statistics.pstdev([seg[k][i] for k in range(nseg)]) for i in range(lag))
        if best is None or spread < best[0]:
            best = (spread, lag)
    lag = best[1] if best else max(1, len(render))
    frames = collections.defaultdict(list)
    for i, e in enumerate(render):
        frames[i // lag].append(e)
    return frames, f'period {lag} (mean within-position spread {best[0]:.3f})'


# ---------------------------------------------------------------- port side
def pass_frames(path):
    """frame number -> ordered BB_PASS_TRACE lines.

    PT <frame> <ordinal> <WxH> nc=<n> c0=<fmt> c1=<fmt> d=<fmt> dr=<draws> idx=<indices>
       inst=<instances> vs=<h>[,<h>] ps=<h>[,<h>] t=<ns> [what]
    """
    out = collections.defaultdict(list)
    with open(path) as f:
        for line in f:
            if not line.startswith('PT '):
                continue
            p = line.split()
            rec = {'frame': int(p[1]), 'ord': int(p[2]), 'size': p[3], 'what': p[-1] if '=' not in p[-1] else ''}
            for tok in p[4:]:
                if '=' in tok:
                    k, _, val = tok.partition('=')
                    rec[k] = val
            out[rec['frame']].append(rec)
    return out


def ident(rec):
    """The identity of a pass: what it draws into, and with what."""
    vs = rec.get('vs', '-').split(',')[0] if rec.get('vs', '-') != '-' else '-'
    return (rec['size'], rec.get('nc', '?'), rec.get('c0', '-'), rec.get('d', '-'), vs)


def ranks(xs):
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    r = [0.0] * len(xs)
    for pos, i in enumerate(order):
        r[i] = pos / max(len(xs) - 1, 1)
    return r


def spearman(xs, ys):
    if len(xs) < 4:
        return 0.0
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sx = sum((x - mx) ** 2 for x in xs) ** 0.5
    sy = sum((y - my) ** 2 for y in ys) ** 0.5
    return cov / (sx * sy) if sx and sy else 0.0


def median_template(frames, attr):
    """Median of a per-position attribute over the frames that share the modal encoder count.

    Returns (count, [median value per position]).
    """
    counts = collections.Counter(len(v) for v in frames.values() if isinstance(v, list))
    n = counts.most_common(1)[0][0]
    segs = [v for v in frames.values() if isinstance(v, list) and len(v) == n]
    out = []
    for i in range(n):
        vals = [attr(seg[i]) for seg in segs]
        out.append(statistics.median(vals))
    return n, out, len(segs)


def align_dp(port, trace):
    """Order-preserving alignment of a port frame to a trace frame, on ranks of work.

    Every port pass is matched (a port pass always produces an encoder); the trace may carry
    encoders the port does not open, so skipping a trace position is cheap and skipping a port
    position is impossible. Cost of a pair = |rank difference|, ranks taken over log work so a
    handful of huge passes do not flatten the rest.
    """
    pa = [math.log1p(float(r.get('idx', 0))) for r in port]
    tv = [math.log1p(e['V'] / 1e3) for e in trace]
    ra, rb = ranks(pa), ranks(tv)
    n, m = len(port), len(trace)
    SKIP_TRACE = 0.02
    INF = 1e9
    cost = [[0.0] * (m + 1) for _ in range(n + 1)]
    back = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        cost[i][0] = INF
        back[i][0] = 1
    for j in range(1, m + 1):
        cost[0][j] = cost[0][j - 1] + SKIP_TRACE
        back[0][j] = 2
    for i in range(1, n + 1):
        ci, cp, bi = cost[i], cost[i - 1], back[i]
        for j in range(1, m + 1):
            best = cp[j - 1] + abs(ra[i - 1] - rb[j - 1])
            how = 0
            skip_t = ci[j - 1] + SKIP_TRACE
            if skip_t < best:
                best, how = skip_t, 2
            ci[j] = best
            bi[j] = how
    pairs = []
    i, j = n, m
    while i > 0 and j > 0:
        how = back[i][j]
        if how == 0:
            pairs.append((i - 1, j - 1))
            i, j = i - 1, j - 1
        elif how == 1:
            i -= 1
        else:
            j -= 1
    pairs.reverse()
    return pairs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    jpath = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if jpath in args:
        args.remove(jpath)
    if len(args) < 2:
        print(__doc__)
        return 2
    tf, how = trace_frames(args[0])
    pf = pass_frames(args[1])
    tlist = [v for k, v in sorted(((k, v) for k, v in tf.items() if isinstance(k, int)),
                                 key=lambda kv: kv[0]) if len(v) > 20]
    plist = [v for k, v in sorted(pf.items()) if len(v) > 20]
    if not tlist or not plist:
        print(f'no usable frames: trace {len(tlist)}, pass trace {len(plist)}')
        return 1
    print(f'trace frames {len(tlist)} (segmented by {how}), pass-trace frames {len(plist)}')

    # ---- 1. frame ordinal
    best = None
    for off in range(-8, 9):
        match = sum(1 for i, t in enumerate(tlist)
                    if 0 <= i + off < len(plist) and len(plist[i + off]) == len(t))
        if best is None or match > best[1]:
            best = (off, match)
    off, match = best
    ncmp = sum(1 for i in range(len(tlist)) if 0 <= i + off < len(plist))
    print(f'frame-ordinal join: offset {off}, {match}/{ncmp} frames agree on encoder count '
          f'({100.0 * match / max(ncmp, 1):.0f}%)')

    rows = []
    used = 0
    if match >= max(5, 0.25 * ncmp):
        for fi, (i, t) in enumerate(enumerate(tlist)):
            j = i + off
            if not (0 <= j < len(plist)) or len(plist[j]) != len(t):
                continue
            used += 1
            for e, rec in zip(t, plist[j]):
                rows.append(dict(rec=rec, v=e['V'] / 1e3, f=e['F'] / 1e3, how='ordinal', fi=fi))
        mode = 'frame ordinal'
    else:
        # ---- 2. rank-sequence DP on the frame templates (median per position)
        pn, pidx, npf = median_template(pf, lambda r: float(r.get('idx', 0)))
        tn, tv_ns, ntf = median_template(tf, lambda e: e['V'])
        pframe = next(v for v in pf.values() if isinstance(v, list) and len(v) == pn)
        ttempl = [dict(V=v, F=0.0) for v in tv_ns]
        pairs = align_dp(pframe, ttempl)
        corr = spearman([float(pframe[i].get('idx', 0)) for i, _ in pairs],
                        [tv_ns[j] / 1e3 for _, j in pairs])
        print(f'rank-sequence join: port template {pn} encoders ({npf} frames), trace template '
              f'{tn} encoders ({ntf} frames), {len(pairs)} matched pairs, '
              f'Spearman(indices, vertex) = {corr:.2f}')
        if corr < 0.5:
            print('  the two sequences do not line up (correlation too low): no attribution')
            return 1
        for fi, t in enumerate(tlist):
            for pi, ti in pairs:
                if ti < len(t):
                    rows.append(dict(rec=pframe[pi], v=t[ti]['V'] / 1e3, f=t[ti]['F'] / 1e3,
                                     how='rank-dp', fi=fi))
        used = len(tlist)
        mode = f'rank-sequence DP (Spearman {corr:.2f})'

    n = float(used)
    total_v = sum(r['v'] for r in rows)
    vpf = total_v / n
    print(f'join mode: {mode}; {used} frames, {len(rows) / n:.1f} encoders/frame, '
          f'vertex {vpf / 1e3:.2f} ms/frame')

    # ---- per-frame rank of the heaviest encoders
    by_rank = collections.defaultdict(list)
    per_frame = collections.defaultdict(list)
    for r in rows:
        per_frame[r['fi']].append(r)
    for fi in sorted(per_frame):
        for rank, r in enumerate(sorted(per_frame[fi], key=lambda r: -r['v'])):
            by_rank[rank].append(r)

    print('\nTOP ENCODERS BY VERTEX TIME (averaged over the joined frames)')
    print(f'{"#":>3} {"vertex us":>9} {"share":>6} {"target":>10} {"nc":>2} {"c0":>17} {"d":>17} '
          f'{"dr":>4} {"indices":>9} {"vs":>16} {"frag us":>8}')
    for rank in range(min(12, len(by_rank))):
        rs = by_rank[rank]
        ids = collections.Counter(ident(r['rec']) for r in rs).most_common(1)[0][0]
        v = statistics.fmean(r['v'] for r in rs)
        print(f'{rank + 1:>3} {v:>9.1f} {100 * v / vpf:>5.1f}% {ids[0]:>10} {ids[1]:>2} '
              f'{ids[2]:>17} {ids[3]:>17} '
              f'{statistics.fmean(float(r["rec"].get("dr", 0)) for r in rs):>4.0f} '
              f'{statistics.fmean(float(r["rec"].get("idx", 0)) for r in rs):>9.0f} '
              f'{ids[4][:16]:>16} {statistics.fmean(r["f"] for r in rs):>8.1f}')

    # ---- grouped by identity
    groups = collections.defaultdict(list)
    for r in rows:
        groups[ident(r['rec'])].append(r)
    print('\nBY PASS IDENTITY (grouped over the joined frames)')
    print(f'{"enc/fr":>7} {"vertex us":>9} {"ms/frame":>8} {"share":>6} {"target":>10} {"c0":>17} '
          f'{"d":>17} {"dr":>4} {"indices":>9} {"vs":>16} {"ps":>16}')
    for ids, g in sorted(groups.items(), key=lambda kv: -sum(r['v'] for r in kv[1]))[:14]:
        v = sum(r['v'] for r in g) / n
        print(f'{len(g) / n:>7.1f} {statistics.fmean(r["v"] for r in g):>9.1f} {v / 1e3:>8.3f} '
              f'{100 * v / vpf:>5.1f}% {ids[0]:>10} {ids[2]:>17} {ids[3]:>17} '
              f'{statistics.fmean(float(r["rec"].get("dr", 0)) for r in g):>4.0f} '
              f'{statistics.fmean(float(r["rec"].get("idx", 0)) for r in g):>9.0f} {ids[4][:16]:>16} '
              f'{(g[0]["rec"].get("ps", "-").split(",")[0] if g[0]["rec"].get("ps", "-") != "-" else "-")[:16]:>16}')

    heavy = [r for r in rows if r['v'] >= 100]
    small = [r for r in rows if r['v'] < 10]
    print(f'\n>=100 us: {len(heavy) / n:.1f}/frame, {sum(r["v"] for r in heavy) / n / 1e3:.2f} ms/frame '
          f'({100 * sum(r["v"] for r in heavy) / max(total_v, 1):.0f}% of vertex)')
    print(f'<10 us:   {len(small) / n:.1f}/frame, {sum(r["v"] for r in small) / n / 1e3:.2f} ms/frame')
    print(f'corr(indices, vertex us) over the joined pairs = '
          f'{spearman([float(r["rec"].get("idx", 0)) for r in rows], [r["v"] for r in rows]):.2f}')

    if jpath:
        out = dict(mode=mode, frames=used, vertex_ms_per_frame=round(vpf / 1e3, 3),
                   heavy_ge100_us_per_frame=round(len(heavy) / n, 1),
                   heavy_ms_per_frame=round(sum(r['v'] for r in heavy) / n / 1e3, 3),
                   top=[dict(rank=r + 1,
                             vertex_us=round(statistics.fmean(x['v'] for x in by_rank[r]), 1),
                             size=ident(by_rank[r][0]['rec'])[0],
                             c0=ident(by_rank[r][0]['rec'])[2],
                             depth=ident(by_rank[r][0]['rec'])[3],
                             vs=ident(by_rank[r][0]['rec'])[4],
                             draws=round(statistics.fmean(float(x['rec'].get('dr', 0)) for x in by_rank[r])),
                             indices=round(statistics.fmean(float(x['rec'].get('idx', 0)) for x in by_rank[r])))
                         for r in sorted(by_rank)])
        with open(jpath, 'w') as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
