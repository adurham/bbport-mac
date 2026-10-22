#!/usr/bin/env python3
"""bbport: the numbers for KosmicKrisp patch 0008 (shipped AIR + MTL4 archives), per run.

    python3 tools/macos/kkair_report.py out/kkair/ctrl-base out/kkair/proof1 ... [--json OUT]
    python3 tools/macos/kkair_report.py --compare A B [C D ...]   # frame captures, pairwise

Per run directory (tools/macos/kkair_run.sh): pipelines created (pipe.txt), the driver time of the
area's pipelines (sum of the port's drv_us, i.e. the whole vkCreate*Pipelines call), KosmicKrisp's
own split (kk.txt: NIR->MSL, mtl_new_library, PSO), failed pipelines and the patch's own log lines
(the run's log), hitch frames, the known asserts. Nothing here launches the game.
"""

import argparse
import glob
import json
import os
import re
import statistics
import sys


def pct(values, p):
    if not values:
        return None
    values = sorted(values)
    k = (len(values) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def kv(line):
    return dict(f.split("=", 1) for f in line.split()[1:] if "=" in f)


def run_report(d):
    r = {"dir": d}
    pipes, frames = [], []
    for line in open(os.path.join(d, "pipe.txt"), errors="replace"):
        if line.startswith("PIPE "):
            pipes.append(kv(line))
        elif line.startswith("FRAME "):
            frames.append(kv(line))
    kk = [kv(l) for l in open(os.path.join(d, "kk.txt"), errors="replace") if l.startswith("KKPIPE")]
    drv = [float(p["drv_us"]) for p in pipes if "drv_us" in p]
    port = [float(p["port_us"]) for p in pipes if "port_us" in p]
    lib = [float(k["lib_us"]) for k in kk]
    pso = [float(k["pso_us"]) for k in kk]
    nir = [float(k["nir_msl_us"]) for k in kk]
    r["pipelines"] = len(pipes)
    r["graphics"] = sum(1 for p in pipes if p.get("kind") == "g")
    r["compute"] = sum(1 for p in pipes if p.get("kind") == "c")
    r["kk_lines"] = len(kk)
    r["kk_cache_hits"] = sum(1 for k in kk if k.get("hit") == "1")
    r["driver_total_ms"] = round(sum(drv) / 1000, 1)
    r["port_total_ms"] = round(sum(port) / 1000, 1)
    r["driver_p50_us"] = pct(drv, 50)
    r["driver_p90_us"] = pct(drv, 90)
    r["mtllib_p50_us"] = pct(lib, 50)
    r["mtllib_p90_us"] = pct(lib, 90)
    r["mtllib_total_ms"] = round(sum(lib) / 1000, 1)
    r["pso_p50_us"] = pct(pso, 50)
    r["pso_p90_us"] = pct(pso, 90)
    r["pso_total_ms"] = round(sum(pso) / 1000, 1)
    r["nir_msl_total_ms"] = round(sum(nir) / 1000, 1)
    # pipelines that missed the AIR (lib over 5 ms is a real MSL compile)
    r["kk_lib_over_5ms"] = sum(1 for v in lib if v > 5000)
    r["kk_pso_over_5ms"] = sum(1 for v in pso if v > 5000)
    # frames
    fms = [float(f.get("ms", f.get("wall_ms", 0))) for f in frames]
    r["hitch_frames_over_50ms"] = sum(1 for v in fms if v > 50)
    r["hitch_total_ms"] = round(sum(v for v in fms if v > 50), 1)
    # log
    log = os.path.join(d, "out", "last-run.log")
    text = open(log, errors="replace").read() if os.path.exists(log) else ""
    r["log_bytes"] = len(text)
    r["failed_pipelines"] = len(re.findall(r"Failed to create (MTLRenderPipelineState|MTLComputePipelineState|MTLLibrary)", text))
    r["vk_pipeline_errors"] = len(re.findall(r"(?i)pipeline[^\n]*(VK_ERROR|failed)", text))
    r["kk_air_lines"] = [l for l in text.splitlines() if l.startswith("KK air") or l.startswith("KK archive")][-6:]
    r["kk_air_line_count"] = sum(1 for l in text.splitlines() if l.startswith("KK air") or l.startswith("KK archive"))
    r["asserts"] = sorted(set(re.findall(r"(gcn_to_vk\.cpp:\d+|cp_graphics\.cpp:\d+|Assertion[^\n]{0,120})", text)))
    m = re.findall(r"(\d+) draws/frame", text)
    r["max_draws_per_frame"] = max(map(int, m)) if m else 0
    r["reached_gameplay"] = r["max_draws_per_frame"] >= 500
    r["gpu_exit"] = re.findall(r"GPU process[^\n]*(?:exit|ended|died)[^\n]*", text)[-2:]
    r["captures"] = sorted(glob.glob(os.path.join(d, "captures", "*.png")))
    cfg = os.path.join(d, "config.txt")
    r["config"] = open(cfg).read().strip().splitlines() if os.path.exists(cfg) else []
    mem = os.path.join(d, "mem.txt")
    r["watchdog_stop"] = "STOP" in (open(mem).read() if os.path.exists(mem) else "")
    return r


def load(path):
    from PIL import Image
    import numpy as np

    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def compare(a, b):
    import numpy as np

    x, y = load(a), load(b)
    if x.shape != y.shape:
        return {"a": a, "b": b, "shape_mismatch": [x.shape, y.shape]}
    d = np.abs(x - y)
    lum = lambda im: im @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    lx, ly = lum(x), lum(y)
    return {
        "a": os.path.relpath(a),
        "b": os.path.relpath(b),
        "mean_abs": float(d.mean()),
        "p99_abs": float(np.percentile(d, 99)),
        "frac_px_over_0.1": float((d.max(axis=2) > 0.1).mean()),
        "luma_mean_a": float(lx.mean()),
        "luma_mean_b": float(ly.mean()),
        "identical": bool((d == 0).all()),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--json")
    ap.add_argument("--compare", nargs="+", help="run dirs: every capture index i of run A vs run B")
    args = ap.parse_args()
    out = {}
    if args.runs:
        out["runs"] = [run_report(d) for d in args.runs]
        for r in out["runs"]:
            print(
                f"{os.path.basename(r['dir']):14s} pipes={r['pipelines']:4d} (g{r['graphics']}/c{r['compute']}) "
                f"driver={r['driver_total_ms']:9.1f} ms p50={r['driver_p50_us']} us | "
                f"mtllib p50={r['mtllib_p50_us']} us total={r['mtllib_total_ms']} ms >5ms={r['kk_lib_over_5ms']} | "
                f"pso p50={r['pso_p50_us']} total={r['pso_total_ms']} | nir_msl={r['nir_msl_total_ms']} ms hits={r['kk_cache_hits']}/{r['kk_lines']} | "
                f"hitches={r['hitch_frames_over_50ms']} ({r['hitch_total_ms']} ms) failed={r['failed_pipelines']} "
                f"kkair_lines={r['kk_air_line_count']} draws={r['max_draws_per_frame']} asserts={r['asserts']} wd={r['watchdog_stop']}"
            )
            for l in r["kk_air_lines"]:
                print("    ", l)
    if args.compare:
        pairs = []
        runs = args.compare
        for i in range(0, len(runs) - 1, 2):
            ca = sorted(glob.glob(os.path.join(runs[i], "captures", "*.png")))
            cb = sorted(glob.glob(os.path.join(runs[i + 1], "captures", "*.png")))
            for a, b in zip(ca, cb):
                c = compare(a, b)
                pairs.append(c)
                print(json.dumps(c))
        out["compare"] = pairs
    if args.json:
        json.dump(out, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
