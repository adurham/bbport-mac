#!/usr/bin/env python3
"""tools/macos/protect_async_report.py TAG...: the BB_PROTECT_ASYNC A/B (bench/protect-async).

Per run: frame p50 in the flat phase (gameplay t 60-110 s) and in the climbed phase (the last 75 s),
the ipc/wait split from the frame log and the BB_PROTECT_PROBE log, and the async counters
(out/pasync-TAG.csv; must be absent for a control run). Gameplay t=0 is the first frame-log row
with >= 500 draws. Prints a table and writes docs/protect-async/report.json.
"""
import csv
import json
import os
import statistics
import sys

here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
out = os.path.join(here, "out")


def rows(path):
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return list(csv.DictReader(f))


def med(xs):
    xs = [x for x in xs if x is not None]
    return round(statistics.median(xs), 3) if xs else None


def window(rs, lo, hi):
    return [r for r in rs if lo <= float(r["t_s"]) < hi]


def f(r, k):
    try:
        return float(r[k])
    except (KeyError, ValueError, TypeError):
        return None


def report(tag):
    frames = rows(os.path.join(out, f"frames-{tag}.csv"))
    if not frames:
        return {"tag": tag, "error": "no frame log"}
    start = next((float(r["t_s"]) for r in frames if (f(r, "draws") or 0) >= 500), None)
    if start is None:
        return {"tag": tag, "error": "never reached gameplay"}
    end = float(frames[-1]["t_s"])
    probe = rows(os.path.join(out, f"protect-{tag}.csv")) or []
    pasync_path = os.path.join(out, f"pasync-{tag}.csv")
    pasync = rows(pasync_path)
    # The probe and async logs start at bb-gpu's first flip, as the frame log does (same clock
    # origin within ~a frame): gameplay offsets apply to all three.
    res = {"tag": tag, "gameplay_start_s": round(start, 1), "end_s": round(end, 1),
           "pasync_log": os.path.exists(pasync_path)}
    for name, lo, hi in (("flat", start + 60, start + 110), ("late", end - 75, end + 1)):
        w = window(frames, lo, hi)
        pw = window(probe, lo, hi)
        aw = window(pasync, lo, hi) if pasync else []
        res[name] = {
            "frames": len(w),
            "frame_p50": med(f(r, "frame_ms") for r in w),
            "frame_p99": round(sorted(f(r, "frame_ms") for r in w)[int(len(w) * 0.99)], 2) if w else None,
            "draws": med(f(r, "draws") for r in w),
            "ipc_ms": med(f(r, "ipc_ms") for r in w),
            "ipc_calls": med(f(r, "ipc_calls") for r in w),
            "protect_ms": med(f(r, "protect_ms") for r in w),
            "game_cpu_ms": med(f(r, "game_cpu_ms") for r in w),
            "game_fault_ms": med(f(r, "game_fault_ms") for r in w),
            "fault_handler_ms": med(f(r, "fault_handler_ms") for r in w),
            "p_service_ms": med(f(r, "p_service_ms") for r in pw),
            "p_sys_ms": med(f(r, "p_sys_ms") for r in pw),
            "probe_revoke_ms": med(f(r, "revoke_ms") for r in pw),
            "probe_restore_ms": med(f(r, "restore_ms") for r in pw),
            "probe_calls": med((f(r, "revoke_calls") or 0) + (f(r, "restore_calls") or 0) for r in pw),
        }
        if res[name]["ipc_ms"] is not None and res[name]["p_service_ms"] is not None:
            res[name]["wait_queue_ms"] = round(res[name]["ipc_ms"] - res[name]["p_service_ms"], 3)
        if aw:
            sums = {k: sum(f(r, k) or 0 for r in aw) for k in aw[0] if k not in ("t_s", "frame_ms")}
            n = len(aw)
            res[name]["async"] = {
                "protect_calls_sent_per_frame": round(sums["appended"] / n, 1),
                "batches_per_frame": round(sums["batches"] / n, 1),
                "replies_awaited_per_frame": round(sums["replies_awaited"] / n, 2),
                "restores_no_reply_per_frame": round(sums["restores_no_reply"] / n, 1),
                "revokes_batched_per_frame": round(sums["revokes_batched"] / n, 1),
                "revokes_per_revoke_batch": round(sums["revokes_batched"] / sums["revoke_batches"], 2)
                if sums["revoke_batches"] else 0,
                "wait_ms_per_frame": round(sums["wait_ms"] / n, 3),
                "submit_wait_ms_per_frame": round(sums["submit_wait_ms"] / n, 3),
                "queue_ms_per_frame": round(sums["g_queue_ms"] / n, 3),
                "service_ms_per_frame": round(sums["g_service_ms"] / n, 3),
                "guest_fault_wait_ms_per_frame": round(sums["g_fault_wait_ms"] / n, 3),
                "violations": int(sums["violations"]),
                "skipped": int(sums["skipped"]),
                "out_of_order": int(sums["g_out_of_order"]),
                "refaults": int(sums["g_refaults"]),
            }
    if pasync:
        res["async_totals"] = {k: int(sum(f(r, k) or 0 for r in pasync))
                               for k in ("violations", "skipped", "g_out_of_order", "g_refaults",
                                         "barriers", "replies_awaited", "appended")}
    log = os.path.join(out, f"run-{tag}.log")
    if os.path.exists(log):
        text = open(log, errors="replace").read()
        res["log_protect_async_lines"] = sum(1 for l in text.splitlines() if l.startswith("Protect async"))
        res["log_asserts"] = sum(1 for l in text.splitlines()
                                 if any(k in l for k in ("gcn_to_vk", "Bad PM4", "Unreachable code",
                                                         "Assertion", "ended by signal")))
    return res


def main():
    tags = sys.argv[1:]
    results = [report(t) for t in tags]
    for r in results:
        if "error" in r:
            print(f"{r['tag']}: {r['error']}")
            continue
        print(f"== {r['tag']} (gameplay from {r['gameplay_start_s']} s, end {r['end_s']} s, "
              f"async log {'YES' if r['pasync_log'] else 'no'}, 'Protect async' lines "
              f"{r.get('log_protect_async_lines')}, asserts {r.get('log_asserts')})")
        for name in ("flat", "late"):
            w = r[name]
            print(f"   {name}: {w['frames']} frames, p50 {w['frame_p50']} p99 {w['frame_p99']}, "
                  f"draws {w['draws']}, ipc {w['ipc_ms']} ms ({w['ipc_calls']} waits), "
                  f"service {w['p_service_ms']}, wait/queue {w.get('wait_queue_ms')}, "
                  f"game cpu {w['game_cpu_ms']}, guest faults {w['game_fault_ms']} ms, "
                  f"handler {w['fault_handler_ms']}")
            if "async" in w:
                print(f"          async: {w['async']}")
        if "async_totals" in r:
            print(f"   async totals: {r['async_totals']}")
    os.makedirs(os.path.join(here, "docs", "protect-async"), exist_ok=True)
    with open(os.path.join(here, "docs", "protect-async", "report.json"), "w") as fp:
        json.dump(results, fp, indent=1)


if __name__ == "__main__":
    main()
