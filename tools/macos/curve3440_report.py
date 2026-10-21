#!/usr/bin/env python3
# tools/macos/curve3440_report.py: analyze one 3440x1440 preset-curve run (curve3440.sh's tags).
#   python3 tools/macos/curve3440_report.py curve-p4 curve-p3 ...
# Reads per tag: out/frames-<tag>.csv (BB_FRAME_LOG), out/run-<tag>.log, out/curve3440/foot-<tag>.txt.
# Standing window: the last WINDOW_S s of gameplay rows (draws>=300 per frame); compiles>0 rows
# are excluded from the percentiles but counted. Writes out/curve3440/curve-3440.json and prints
# a compact table.
import csv
import json
import statistics
import sys
from pathlib import Path

CHECKOUT = Path(__file__).resolve().parents[2]
OUT = CHECKOUT / "out"
CURVE = OUT / "curve3440"
WINDOW_S = 75.0


def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * p
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def analyze(tag):
    row = {"tag": tag, "status": "ok", "attempt": None, "issues": []}
    done = CURVE / f"{tag}.done"
    attempts_row_fn = (CURVE / "attempts.jsonl")
    attempts = []
    if attempts_row_fn.exists():
        attempts = [json.loads(x) for x in open(CURVE / "attempts.jsonl")
                    if x.strip().startswith("{") and f'"tag":"{tag}"' in x]
    if attempts:
        row["attempt"] = attempts[-1].get("attempt")
        first = attempts[0].get("status")
        if first != "ok":
            row["issues"].append(f"first attempt {first}: {attempts[0].get('note', '')[:80]}")
    if not done.exists():
        row["status"] = "not-done"
        return row
    log = OUT / f"run-{tag}.log"
    if not log.exists():
        row["status"] = "no-log"
        return row
    text = log.read_text(errors="replace")
    rasters = []
    for line in text.splitlines():
        if (r := line.find("Scene resolution: raster")) >= 0:
            seg = line[r + len("Scene resolution: raster"):]
            rasters.append(seg.split(",")[0].strip())
    row["raster"] = rasters[-1] if rasters else None
    fsr = [ln for ln in text.splitlines() if "Upscaler: FSR 3.1 available" in ln]
    row["fsr_context_line"] = fsr[0].strip()[:100] if fsr else None

    # Frame stats windows (once per 5 s): draw the GPU-thread us/draw and the window FPS.
    stats_lines = [ln for ln in text.splitlines() if ln.startswith("Frame stats:")]
    good = []
    for ln in stats_lines:
        try:
            fps = float(ln.split("FPS")[0].split(":")[1])
        except ValueError:
            continue
        draws = 0.0
        usd = None
        for tok in ln.split(","):
            tok = tok.strip()
            if tok.endswith("draws/frame"):
                try:
                    draws = float(tok.split()[0])
                except ValueError:
                    pass
            if "GPU thread" in tok:
                try:
                    usd = float(tok.split()[-2])  # 'GPU thread 7.33 us/draw'
                except ValueError:
                    pass
        compiles = "0 shader/pipeline compiles" in ln
        if draws >= 300:
            good.append((fps, draws, usd, compiles))
    if good:
        standing = good[-14:]  # ~70 s of 5 s windows
        row["window_fps_p50"] = statistics.median(w[0] for w in standing)
        row["draws_per_frame"] = round(statistics.mean(w[1] for w in standing), 1)
        usd_vals = [w[2] for w in standing if w[2] is not None]
        row["us_per_draw"] = round(statistics.mean(usd_vals), 2) if usd_vals else None
    row["frame_stats_windows"] = len(stats_lines)

    csvp = OUT / f"frames-{tag}.csv"
    if not csvp.exists():
        row["status"] = "no-frames"
        return row
    with csvp.open() as fh:
        r = csv.DictReader(fh)
        cols = {name: i for i, name in enumerate(r.fieldnames or [])}
        need = ["t_s", "frame_ms", "draws", "compiles", "present_wait_ms", "tick_wait_ms",
                "ga_wall_ms", "b_busy_ms", "ipc_ms", "translate_ms"]
        missing = [c for c in need if c not in cols]
        if missing:
            row["status"] = "bad-csv"
            row["issues"].append(f"missing columns {missing}")
            return row
        rows = [x for x in r
                if x["draws"] != "" and float(x["draws"] or 0) >= 300]
    if not rows:
        row["status"] = "no-gameplay-rows"
        return row
    t_last = float(rows[-1]["t_s"])
    win = [x for x in rows if float(x["t_s"]) >= t_last - WINDOW_S]
    no_compiles = [x for x in win if int(float(x["compiles"] or 0)) == 0]
    frames = sorted(float(x["frame_ms"]) for x in no_compiles)
    if not frames:
        row["status"] = "window-empty"
        return row
    dur = float(win[-1]["t_s"]) - float(win[0]["t_s"]) if len(win) > 1 else WINDOW_S
    row["window_s"] = round(dur, 1)
    row["frames"] = len(win)
    row["fps"] = round(len(win) / dur, 2) if dur > 0 else None
    row["p50_ms"] = round(pct(frames, 0.50), 2)
    row["p90_ms"] = round(pct(frames, 0.90), 2)
    row["p99_ms"] = round(pct(frames, 0.99), 2)
    row["worst_ms"] = round(max(frames), 2)
    cvs = lambda d, q: round(pct(sorted(float(x[d] or 0) for x in no_compiles), q), 2)
    for col in ["present_wait_ms", "tick_wait_ms", "ga_wall_ms", "b_busy_ms", "ipc_ms", "translate_ms"]:
        row[col + "_p50"] = cvs(col, 0.50)
    row["draws_p50"] = round(pct(sorted(float(x["draws"]) for x in no_compiles), 0.50), 1)

    footp = CURVE / f"foot-{tag}.txt"
    if footp.exists():
        s = [ln.split() for ln in footp.read_text().splitlines() if ln.strip()]
        feet = [(t, int(a.split("=")[1]), int(b.split("=")[1])) for t, a, b in s if t.isdigit()]
        live = [f for f in feet if f[1] > 0]  # 0 = the process was already gone at that sample
        if live:
            last = live[-6:] or live
            row["footprint_steady_mib"] = round(statistics.median(f[1] for f in last), 0)
            row["footprint_max_mib"] = max(f[1] for f in live)
            row["freepages_min_mib"] = min(f[2] for f in feet)
    return row


def main():
    tags = sys.argv[1:] or [p.name.removesuffix(".done") for p in CURVE.glob("*.done")]
    if not tags:
        print("no done tags under out/curve3440")
        return 1
    out = [analyze(t) for t in tags]
    (CURVE / "curve-3440.json").write_text(json.dumps(out, indent=1) + "\n")
    print(f"{'tag':9} {'raster':11} {'fps':>6} {'p50':>6} {'p90':>6} {'p99':>6} {'worst':>6} "
          f"{'draws':>6} {'us/drw':>6} {'foot MiB':>8}")
    for r in out:
        if r.get("status") != "ok":
            print(f"{r['tag']:9} {r.get('status', '?')} {r.get('issues', [])}")
            continue
        g = lambda k, d: (r.get(k) if r.get(k) is not None else d)
        print(f"{r['tag']:9} {str(g('raster', '?')):11} {g('fps', 0):>6.2f} {g('p50_ms', 0):>6.2f} "
              f"{g('p90_ms', 0):>6.2f} {g('p99_ms', 0):>6.2f} {g('worst_ms', 0):>6.2f} "
              f"{g('draws_per_frame', 0):>6.0f} {g('us_per_draw', 0):>6.2f} "
              f"{g('footprint_steady_mib', 0):>8.0f}")
    print(f"wrote {CURVE / 'curve-3440.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())