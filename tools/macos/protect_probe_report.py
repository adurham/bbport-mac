#!/usr/bin/env python3
"""tools/macos/protect_probe_report.py TAG [TAG ...]: medians of the BB_PROTECT_PROBE split
(out/protect-TAG.csv) next to the frame log (out/frames-TAG.csv), over the last WINDOW s
(default 75), plus a 30 s time series of frame_ms / ipc_ms / game_fault_ms / game_cpu_ms
(the protect/ipc term is time-dependent within a run: read the series, not only the median)."""
import csv, os, statistics as st, sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out")
WINDOW = float(os.environ.get("WINDOW", "75"))


def rows(path):
    with open(path) as f:
        data = [r for r in csv.DictReader(l for l in f if not l.startswith("#"))]
    return [r for r in data if None not in r.values() and "" not in r.values()]


def last(data):
    tmax = float(data[-1]["t_s"])
    return [r for r in data if float(r["t_s"]) > tmax - WINDOW]


def med(data, k):
    return st.median(float(r[k]) for r in data)


for tag in sys.argv[1:]:
    fr = [r for r in last(rows(os.path.join(OUT, f"frames-{tag}.csv"))) if r["compiles"] == "0"]
    print(f"== {tag}: {len(fr)} frames in the last {WINDOW:.0f} s")
    for k in ["frame_ms", "b_busy_ms", "stage_a_cpu_ms", "ipc_ms", "ipc_calls", "game_fault_ms",
              "game_faults", "fault_handler_ms", "game_call_ms", "game_cpu_ms", "draws"]:
        print(f"   {k:18s} {med(fr, k):9.3f}")
    probe = os.path.join(OUT, f"protect-{tag}.csv")
    if os.path.exists(probe):
        pr = last(rows(probe))
        for k in ["revoke_ms", "revoke_calls", "revoke_pages", "restore_ms", "restore_calls",
                  "restore_pages", "trap_on_calls", "trap_off_calls", "record_ms", "other_ms",
                  "p_lock_ms", "p_sys_ms", "p_sys_calls", "p_sys_pages", "p_service_ms",
                  "g_sys_ms", "g_sys_calls"]:
            print(f"   {k:18s} {med(pr, k):9.3f}")
    allf = [r for r in rows(os.path.join(OUT, f"frames-{tag}.csv")) if int(r["draws"]) > 500]
    buckets = {}
    for r in allf:
        buckets.setdefault(int(float(r["t_s"]) // 30), []).append(r)
    print("   t(s)   " + " ".join(f"{k * 30:6d}" for k in sorted(buckets)))
    for k in ["frame_ms", "ipc_ms", "game_fault_ms", "game_cpu_ms"]:
        print(f"   {k[:6]:6s} " + " ".join(f"{st.median(float(r[k]) for r in v):6.1f}"
                                         for _, v in sorted(buckets.items())))
