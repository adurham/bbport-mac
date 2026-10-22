#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""bbport: attribute the remaining long frames in a BB_FRAME_LOG run.

The pipeline campaign removed the compile class from a covered area (docs/PIPELINE_SIZING.md,
docs/KK_AIR_ARCHIVE.md). What is left over 50 ms has to be named before a lever is chosen, so this
reads one run's frames.csv (frame_stats.cpp: one line per flip, with the port's per-frame phase
deltas) and answers, as numbers:

  1. How many flips, over how long, and how many over the threshold (and how much time they hold).
  2. For each long frame, which phase owns it - compile (pipeline_sync / compiles), the port's own
     translation, descriptors, IPC, the guest's page faults, the guest's *file reads* (streaming),
     the guest's own CPU, or a GPU-thread stall that is none of those (record thread / driver).
  3. The same split summed over all long frames, so the classes can be ranked by wall time.
  4. The ten worst frames with every candidate column, for eyeballing and for a doc table.

  python3 tools/macos/hitch_attribution.py out/sizing/TAG/frames.csv [--ms 50] [--top 10]
                                            [--min-draws 300] [--json PATH] [--csv PATH]

Nothing here launches anything; it only reads what a run wrote. Columns are the header written by
OpenFrameLog (gpu/platform/videoout/frame_stats.cpp). Note the two sides of a flip: the *guest*
columns (game_*) are the game process's own accounting of what delayed its submission, the *GPU*
columns are bb-gpu's command thread (ga_*) and the draw recording thread (b_*, cat_*), and
gpu_idle_ms is the command thread waiting for the guest - by construction
frame_ms = gpu_idle_ms + ga_wall_ms.
"""

import argparse
import csv
import json
import sys

# The candidate owners of a long frame, in the order the classifier tests them. Each entry is
# (name, columns, what it means, lever family). Test order matters: a compile is a certainty, the
# guest's file reads are a certainty, and the rest compete by size.
CANDIDATES = [
    ("compile", ["pipeline_sync_ms"], "pipeline created on the command thread", "precompile"),
    ("port_translate", ["translate_ms"], "GCN -> SPIR-V on the command thread", "port"),
    ("descriptor", ["descriptor_ms"], "descriptor sets built for the frame", "port"),
    ("ipc", ["ipc_ms"], "round trips to the game process", "ipc"),
    ("guest_read", ["game_read_ms"], "the guest blocked on file reads (asset streaming)", "streaming"),
    ("guest_fault", ["game_fault_ms"], "the guest blocked on page faults", "memory"),
    ("guest_call", ["game_call_ms"], "the guest blocked in a call into the port", "ipc"),
    ("gpu_drain", ["fault_handler_ms", "fault_drain_ms"], "bb-gpu servicing the guest's page faults",
     "memory"),
    ("record_thread", ["b_busy_ms"], "the draw recording thread (stage B)", "gpu-thread"),
    ("present_wait", ["present_wait_ms"], "waiting for an older frame's GPU tick", "present"),
    ("guest_cpu", ["game_cpu_ms"], "the guest's own CPU time in the interval", "guest-cpu"),
]

# Columns that are sub-counts of others and must not be added to a total.
OVERLAP = {
    "tick_wait_ms": "includes present_wait_ms",
    "ga_wall_ms": "is frame_ms - gpu_idle_ms",
    "stage_a_cpu_ms": "is ga_user_ms + ga_sys_ms sampled per submission",
    "game_majflt": "a count, not ms",
}


def load(path):
    rows = []
    with open(path, newline="") as f:
        for d in csv.DictReader(line for line in f if not line.startswith("#")):
            row = {}
            for k, v in d.items():
                if not k or k.endswith("_mb") or k in ("translates", "pipelines_sync", "async_skips",
                                                       "descriptor_batches", "ipc_calls",
                                                       "game_faults", "game_calls", "game_reads",
                                                       "game_majflt", "fault_handlers",
                                                       "fault_drains", "ga_drains", "ga_yields",
                                                       "images", "draws", "dispatches",
                                                       "submissions", "compiles", "read_faults"):
                    try:
                        row[k] = float(v) if v not in (None, "") else 0.0
                    except ValueError:
                        pass
                else:
                    try:
                        row[k] = float(v)
                    except (TypeError, ValueError):
                        pass
            if "frame_ms" in row and "t_s" in row:
                rows.append(row)
    return rows


def value(row, col):
    v = row.get(col)
    return float(v) if isinstance(v, float) else 0.0


def classify(row):
    """Which side of the flip owns a long frame, and which named phase inside that side.

    frame_ms = gpu_idle_ms + ga_wall_ms by construction (frame_stats.cpp), so the first question is
    which side the time sat on: bb-gpu busy (ga_wall) or waiting for the guest to submit (gpu_idle).
    The named slices then compete inside that side only - a guest stall cannot be owned by a GPU
    counter, and game_cpu_ms is CPU time summed over the guest's threads (it can exceed frame_ms),
    so it is evidence, never an owner.
    """
    frame = value(row, "frame_ms")
    idle = value(row, "gpu_idle_ms")
    wall = value(row, "ga_wall_ms")
    sync = value(row, "pipeline_sync_ms")
    if sync >= 0.2 * frame or (value(row, "compiles") > 0 and sync >= 5.0):
        return "compile", sync
    if idle >= wall:
        guest = [("guest_read", value(row, "game_read_ms")),
                 ("guest_fault", value(row, "game_fault_ms")),
                 ("guest_call", value(row, "game_call_ms"))]
        name, amount = max(guest, key=lambda kv: kv[1])
        if amount >= 5.0:
            return name, amount
        return "guest_late_other", 0.0
    gpu = [("port_translate", value(row, "translate_ms")),
           ("descriptor", value(row, "descriptor_ms")),
           ("ipc", value(row, "ipc_ms")),
           ("gpu_drain", value(row, "fault_handler_ms") + value(row, "fault_drain_ms"))]
    name, amount = max(gpu, key=lambda kv: kv[1])
    if amount >= 5.0:
        return name, amount
    return "gpu_thread_other", 0.0


def main():
    ap = argparse.ArgumentParser(description="attribute a run's long frames")
    ap.add_argument("frames", nargs="+", help="frames.csv from one run (BB_FRAME_LOG)")
    ap.add_argument("--ms", type=float, default=50.0, help="hitch threshold in ms (default 50)")
    ap.add_argument("--min-draws", type=float, default=300.0,
                    help="gameplay flip filter (default 300; use 0 to keep everything)")
    ap.add_argument("--top", type=int, default=10, help="worst frames to print")
    ap.add_argument("--json", help="write the summary as JSON here")
    ap.add_argument("--csv", help="write the long frames (all columns) as CSV here")
    args = ap.parse_args()

    all_rows = []
    for path in args.frames:
        for row in load(path):
            row["_file"] = path
            all_rows.append(row)
    if not all_rows:
        sys.exit(f"no frames in {args.frames}")

    play = [r for r in all_rows if value(r, "draws") >= args.min_draws]
    if not play:
        sys.exit(f"no gameplay flips with draws >= {args.min_draws:g} (run never reached an area?)")
    span = value(play[-1], "t_s") - value(play[0], "t_s")
    long_frames = [r for r in play if value(r, "frame_ms") >= args.ms]

    by_class = {}
    for r in long_frames:
        name, amount = classify(r)
        by_class.setdefault(name, []).append((r, amount))

    print(f"== {', '.join(args.frames)}")
    print(f"flips {len(all_rows)}, gameplay flips {len(play)} over {span:.1f} s "
          f"({len(play) / span:.1f} fps), draws/median "
          f"{sorted(value(r, 'draws') for r in play)[len(play) // 2]:.0f}")
    total_ms = sum(value(r, "frame_ms") for r in long_frames)
    print(f"long frames >= {args.ms:g} ms: {len(long_frames)} of {len(play)} "
          f"({100.0 * len(long_frames) / len(play):.1f}%), {total_ms / 1000:.2f} s in them, "
          f"worst {max((value(r, 'frame_ms') for r in long_frames), default=0):.0f} ms")
    print()

    print(f"{'owner':16s} {'frames':>6s} {'ms total':>9s} {'mean ms':>8s} {'share of long ms':>16s}")
    ranked = sorted(by_class.items(), key=lambda kv: -sum(a for _r, a in kv[1]))
    for name, items in ranked:
        if not items:
            continue
        ms = sum(a for _r, a in items)
        share = 100.0 * ms / total_ms if total_ms else 0.0
        print(f"{name:16s} {len(items):6d} {ms:9.1f} {ms / len(items):8.1f} {share:15.1f}%")

    print()
    print("Worst frames:")
    head = ["t_s", "frame_ms", "draws", "compiles", "pipeline_sync_ms", "translate_ms", "ipc_ms",
            "game_read_ms", "game_read_mb", "game_fault_ms", "game_call_ms", "game_cpu_ms",
            "gpu_idle_ms", "ga_wall_ms", "b_busy_ms", "present_wait_ms", "write_fault_ms"]
    print("  " + " ".join(f"{h:>10s}" for h in head))
    for r in sorted(long_frames, key=lambda r: -value(r, "frame_ms"))[:args.top]:
        cells = []
        for h in head:
            v = value(r, h)
            cells.append(f"{v:10.2f}" if h not in ("draws", "compiles") else f"{v:10.0f}")
        owner, _amount = classify(r)
        print("  " + " ".join(cells) + f"   <- {owner}")

    if args.csv:
        cols = [c for c in long_frames[0].keys()] if long_frames else []
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols)
            w.writeheader()
            for r in long_frames:
                w.writerow(r)
        print(f"\nlong frames -> {args.csv}")

    if args.json:
        summary = {
            "files": args.frames,
            "threshold_ms": args.ms,
            "flips": len(all_rows),
            "gameplay_flips": len(play),
            "gameplay_span_s": round(span, 2),
            "fps": round(len(play) / span, 2) if span else None,
            "long_frames": len(long_frames),
            "long_ms_total": round(total_ms, 1),
            "worst_ms": round(max((value(r, "frame_ms") for r in long_frames), default=0.0), 1),
            "by_owner": {
                name: {
                    "frames": len(items),
                    "ms_total": round(sum(a for _r, a in items), 1),
                    "mean_ms": round(sum(a for _r, a in items) / len(items), 1),
                }
                for name, items in by_class.items()
                if items
            },
            "worst": [
                {k: value(r, k) for k in
                 ("t_s", "frame_ms", "draws", "compiles", "pipeline_sync_ms", "translate_ms",
                  "ipc_ms", "game_read_ms", "game_read_mb", "game_fault_ms", "game_call_ms",
                  "game_cpu_ms", "gpu_idle_ms", "ga_wall_ms", "b_busy_ms", "present_wait_ms")}
                for r in sorted(long_frames, key=lambda r: -value(r, "frame_ms"))[:args.top]
            ],
        }
        with open(args.json, "w") as f:
            json.dump(summary, f, indent=1)
        print(f"summary -> {args.json}")


if __name__ == "__main__":
    main()
