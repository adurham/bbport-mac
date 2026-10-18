#!/usr/bin/env python3
"""commit_trace.py: read BB_COMMIT_TRACE=1's counter lines (docs/FRAME_HANDOFF_WAIT.md).

The counter lives in the driver (kk_queue.c + wsi_common_metal_layer.m). Per Metal commit it
knows which queue wait was enqueued ahead of it (:274 waitForEvent, :294 waitForDrawable) and the
GPU's own start/end stamps from the MTL4 commit feedback, so:

    dry = this commit's GPUStartTime - the latest GPUEndTime the queue had reported

is the time the GPU had that commit queued and did not run it. A queue wait shows up as dry on
the commit that follows it (the wait is enqueued at the queue tail, before that commit).

Output: one summary line per 2 s with, per wait site, the dry total, the busy total, the peak and
the counts above 0.5 ms and 1.0 ms — over the whole population, not a filtered sample — plus
detail lines for commits that started 0.3-12 ms late.

    python3 tools/macos/commit_trace.py out/run1.log

Sanity check to demand of any run: in gameplay the total dry must be ~32 % of wall time (E2's
5.86-6.01 ms of a 18.4 ms frame, docs/GPU_TIMELINE_RECONCILIATION.md). If it is not, the metric
is not measuring the gap and its per-tag split means nothing.
"""

import argparse
import re
import sys
from collections import defaultdict

COMMIT_RE = re.compile(
    r"BBCT commit seq=(\d+) tag=(\w+) n=(\d+) inflight=(\d+) rerecord=(\d+) "
    r"commit_ms=(-?\d+\.\d+) wait_ms=(-?\d+\.\d+) gpu_start_ms=(-?\d+\.\d+) "
    r"gpu_end_ms=(-?\d+\.\d+) dry_ms=([+-]?\d+\.\d+)"
)
ACQ_RE = re.compile(r"BBCT acquire t_ms=(\d+\.\d+) got=(\d) block_us=(-?\d+) inflight=(\d+)")
WIN_RE = re.compile(r"BBCT window=(\d+)ms commits=(\d+) busy_ms=(\d+\.\d+) dry_ms=(\d+\.\d+) "
                    r"over500=(\d+) over1000=(\d+)")
SITE_RE = re.compile(
    r"(none|event|drawable)=(\d+)/dry(\d+\.\d+)ms/busy(\d+\.\d+)ms/peak(\d+)us/o500=(\d+)/o1000=(\d+)"
)
START_RE = re.compile(r"BBCT start t0_ms=(\d+\.\d+) full=(\d)")
LAYER_RE = re.compile(r"BBCT layer (.*)")


def pct(values, q):
    if not values:
        return 0.0
    s = sorted(values)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=float, default=0.0,
                    help="ignore the first N seconds of windows (load/title phase)")
    args = ap.parse_args()

    t0 = None
    layers, commits, acquires, windows = [], [], [], []

    for line in open(args.log, "r", errors="replace"):
        if "BBCT " not in line:
            continue
        if (m := START_RE.search(line)):
            t0 = float(m.group(1))
            continue
        if (m := LAYER_RE.search(line)):
            layers.append(m.group(1).strip())
            continue
        if (m := COMMIT_RE.search(line)):
            commits.append((m.group(2), int(m.group(3)), int(m.group(4)),
                            float(m.group(8)), float(m.group(10).lstrip("+"))))
            continue
        if (m := ACQ_RE.search(line)):
            acquires.append((float(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))))
            continue
        if (m := WIN_RE.search(line)):
            sites = {s[0]: dict(n=int(s[1]), dry=float(s[2]), busy=float(s[3]),
                                peak=int(s[4]), o500=int(s[5]), o1000=int(s[6]))
                     for s in SITE_RE.findall(line)}
            windows.append(dict(ms=int(m.group(1)), commits=int(m.group(2)),
                                busy=float(m.group(3)), dry=float(m.group(4)),
                                o500=int(m.group(5)), o1000=int(m.group(6)), sites=sites))
            continue

    for l in layers:
        print(f"layer: {l}")
    if not windows and not commits:
        print("no counter lines: BB_COMMIT_TRACE did not reach the driver")
        return 1

    # ---- windows: the whole-population view ------------------------------------------------
    if windows:
        elapsed = sum(w["ms"] for w in windows) / 1000.0
        skip_ms = args.skip * 1000.0
        seen = 0.0
        agg = defaultdict(lambda: dict(n=0, dry=0.0, busy=0.0, peak=0, o500=0, o1000=0))
        tot = dict(dry=0.0, busy=0.0, commits=0, o500=0, o1000=0)
        wall = 0.0
        for w in windows:
            seen += w["ms"]
            if seen <= skip_ms:
                continue
            wall += w["ms"]
            tot["dry"] += w["dry"]
            tot["busy"] += w["busy"]
            tot["commits"] += w["commits"]
            tot["o500"] += w["o500"]
            tot["o1000"] += w["o1000"]
            for tag, s in w["sites"].items():
                a = agg[tag]
                for k in ("n", "dry", "busy", "o500", "o1000"):
                    a[k] += s[k]
                a["peak"] = max(a["peak"], s["peak"])
        if wall > 0:
            print()
            print(f"windows: {len(windows)} at 2 s = {elapsed:.0f} s of run; "
                  f"counted {wall / 1000:.0f} s (skipped the first {args.skip:.0f} s)")
            print(f"  dry  = {tot['dry']:.0f} ms of {wall:.0f} ms wall = "
                  f"{100 * tot['dry'] / wall:.1f} %   (E2's idle: 31.8 %)")
            print(f"  busy = {tot['busy']:.0f} ms of {wall:.0f} ms wall = "
                  f"{100 * tot['busy'] / wall:.1f} %   (E2's busy: 68 %)")
            print(f"  commits {tot['commits']} in the windows; dry>=0.5 ms: {tot['o500']}, "
                  f"dry>=1.0 ms: {tot['o1000']}")
            print()
            print(f"  {'site':9s} {'commits':>8s} {'dry ms':>8s} {'%wall':>6s} {'busy ms':>8s} "
                  f"{'peak us':>8s} {'o500':>5s} {'o1000':>5s} {'dry ms/site':>11s}")
            for tag in ("none", "event", "drawable"):
                if tag not in agg:
                    continue
                a = agg[tag]
                print(f"  {tag:9s} {a['n']:8d} {a['dry']:8.0f} {100 * a['dry'] / wall:6.2f} "
                      f"{a['busy']:8.0f} {a['peak']:8d} {a['o500']:5d} {a['o1000']:5d} "
                      f"{a['dry'] / max(1, 1000 * wall / 1000):11.0f}")

            # the frame arithmetic: 18.45 ms is the standing config's frame at 54.2 fps
            frames = wall / 18.45
            print()
            print(f"  per-frame reading ({frames:.0f} frames at 18.45 ms = 54.2 fps, "
                  f"{tot['commits'] / max(1, frames):.1f} commits/frame):")
            for tag in ("none", "event", "drawable"):
                if tag not in agg:
                    continue
                a = agg[tag]
                print(f"    {tag:9s} {a['n'] / max(1, frames):5.1f} commits/frame, "
                      f"{(a['dry'] / wall) * 18.45:6.2f} ms/frame of dry")

    # ---- detail lines: the shape of the slow ones ------------------------------------------
    if commits:
        print()
        print(f"detail lines: {len(commits)} commits that started 0.3-12 ms late "
              f"(BB_COMMIT_TRACE_FULL=1 logs every commit)")
        by_tag = defaultdict(list)
        for c in commits:
            by_tag[c[0]].append(c[4])
        print(f"  {'site':9s} {'n':>6s} {'p50 ms':>7s} {'p90':>7s} {'max':>7s}")
        for tag in ("none", "event", "drawable"):
            v = by_tag.get(tag)
            if not v:
                continue
            print(f"  {tag:9s} {len(v):6d} {pct(v, 0.5):7.3f} {pct(v, 0.9):7.3f} {max(v):7.3f}")
        band = [c for c in commits if 1.0 <= c[4] <= 1.8]
        print(f"  the 1.36 ms band (1.0-1.8 ms): {len(band)} commits", end="")
        for tag in ("none", "event", "drawable"):
            t = [c[4] for c in band if c[0] == tag]
            if t:
                print(f"; {tag} {len(t)} (inflight<=%d)" % max(c[2] for c in band if c[0] == tag),
                      end="")
        print()

    # ---- drawable side ---------------------------------------------------------------------
    if acquires:
        blocks = [a[2] for a in acquires]
        print()
        print(f"drawable acquires that blocked >= 200 us: {len(acquires)} "
              f"({sum(1 for a in acquires if a[1] == 0)} returned nothing)")
        print(f"  block_us p50={pct(blocks, 0.5):.0f} p90={pct(blocks, 0.9):.0f} max={max(blocks)}")
        print(f"  drawables in flight at acquire: max={max(a[3] for a in acquires)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
