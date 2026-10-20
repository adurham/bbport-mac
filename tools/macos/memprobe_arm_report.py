#!/usr/bin/env python3
"""tools/macos/memprobe_arm_report.py TAG: the numbers for one arm of a memory-lever measurement.

Reads out/memprobe-arms/<tag>.{log,samples.csv} and writes <tag>-metrics.json, printing a short
summary. Everything comes from the run's own output:

  - phys_footprint: the 3 s sampler's `footprint -p` (the number jetsam acts on), start / steady
    mean over 120-300 s / end / max;
  - the renderer's own share and the driver's own share: the port's own budget line,
    "GPU: memory budget N MiB (Metal's limit ..., less ... outside graphics memory); graphics
    memory X MiB, of it Y MiB the renderer's and Z MiB the driver's own" (Y is
    Instance::GetDeviceMemoryUsage, Z the rest of macOS's graphics ledger);
  - with BB_MEM_BUDGET_RESERVE=1 also the reserve line's inputs and result;
  - KosmicKrisp's allocator ledger: "command allocators N holding M MiB", and `images N` for
    eviction churn;
  - hitches and pacing: one "Frame stats:" line per 5 s window (fps, worst frame ms, draws/frame),
    so "windows over 100 ms" is the count of windows whose worst frame was over 100 ms.

A run that died on one of the known pre-existing asserts is reported as dead, with no numbers.
"""
import json
import re
import statistics
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "out" / "memprobe-arms"

DEATH_MARKERS = ("Unknown data_format", "PM4 type 0", "Assertion failed", "assert")

RE_FRAME = re.compile(r"Frame stats: ([\d.]+) FPS, worst frame ([\d.]+) ms")
RE_DRAWS = re.compile(r"([\d.]+) draws/frame")
RE_ALLOC = re.compile(r"command allocators (\d+) holding (\d+) MiB")
RE_BUDGET = re.compile(
    r"GPU: memory budget (\d+) MiB \(Metal's limit (\d+) MiB, less (\d+) MiB outside "
    r"graphics memory\); graphics memory (\d+) MiB, of it (\d+) MiB the renderer's "
    r"and (\d+) MiB the driver's own"
)
RE_RESERVE = re.compile(
    r"GPU: memory budget (\d+) MiB \(reserve: hw\.memsize (\d+) MiB, less (\d+) MiB wired "
    r"and (\d+) MiB compressor, less (\d+) MiB reserve = (\d+) MiB, floor (\d+) MiB; "
    r"the port's own budget (\d+) MiB"
)
RE_TRIM = re.compile(r"PROBE TRIM (\d+): .*delta (-?\d+) MiB")


def median(values):
    return round(statistics.median(values), 1) if values else None


def main():
    tag = sys.argv[1]
    log_path = OUT / f"{tag}.log"
    csv_path = OUT / f"{tag}-samples.csv"
    metrics: dict[str, Any] = {"tag": tag}

    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    lines = log.splitlines()

    # A dead run is not a number: name the assert and say so.
    deaths = [m for m in DEATH_MARKERS[:3] if m in log]
    if "STOP:" in log:
        deaths.append("STOP:")
    metrics["dead"] = bool(deaths)
    if deaths:
        for line in lines:
            if any(m in line for m in DEATH_MARKERS[:3]):
                metrics["death_line"] = line.strip()[:300]
                break
        metrics["death_markers"] = deaths

    # Footprint from the sampler.
    fp = []
    if csv_path.exists():
        for row in csv_path.read_text().splitlines()[1:]:
            parts = row.split(",")
            if len(parts) >= 4 and parts[3] not in ("-", ""):
                try:
                    fp.append((int(parts[0]), int(parts[3])))
                except ValueError:
                    pass
    if fp:
        metrics["footprint_start_mib"] = fp[0][1]
        metrics["footprint_end_mib"] = fp[-1][1]
        metrics["footprint_max_mib"] = max(v for _, v in fp)
        steady = [v for t, v in fp if 120 <= t <= 300]
        metrics["footprint_steady_mean_mib"] = round(statistics.mean(steady)) if steady else None
        metrics["samples"] = len(fp)
        free = []
        for row in csv_path.read_text().splitlines()[1:]:
            parts = row.split(",")
            if len(parts) >= 5 and parts[4] not in ("-", ""):
                try:
                    free.append(int(parts[4]))
                except ValueError:
                    pass
        if free:
            metrics["free_inactive_min_mib"] = min(free)
            metrics["free_inactive_end_mib"] = free[-1]

    # Pacing, per 5 s window.
    fps, worst, draws = [], [], []
    for line in lines:
        m = RE_FRAME.search(line)
        if not m:
            continue
        fps.append(float(m.group(1)))
        worst.append(float(m.group(2)))
        d = RE_DRAWS.search(line)
        if d:
            draws.append(float(d.group(1)))
    if worst:
        metrics["windows"] = len(worst)
        metrics["windows_over_100ms"] = sum(1 for w in worst if w > 100.0)
        metrics["windows_over_200ms"] = sum(1 for w in worst if w > 200.0)
        metrics["worst_frame_median_ms"] = median(worst)
        metrics["worst_frame_max_ms"] = round(max(worst), 1)
        metrics["fps_median"] = median(fps)
        metrics["fps_mean"] = round(statistics.mean(fps), 1)
    if draws:
        metrics["draws_per_frame_max"] = round(max(draws))
        metrics["draws_per_frame_median"] = median(draws)
        metrics["gameplay_reached"] = max(draws) >= 200

    # Allocator ledger and images.
    alloc = [(int(a), int(b)) for a, b in RE_ALLOC.findall(log)]
    if alloc:
        metrics["allocators_first"] = alloc[0][0]
        metrics["allocators_last"] = alloc[-1][0]
        metrics["allocators_max"] = max(a for a, _ in alloc)
        metrics["allocator_mib_last"] = alloc[-1][1]
        metrics["allocator_mib_max"] = max(b for _, b in alloc)
        metrics["allocator_mib_median"] = median([b for _, b in alloc])
    images = [int(m) for m in re.findall(r"images (\d+), image views", log)]
    if images:
        metrics["images_max"] = max(images)
        # Churn, measured where it matters: after the first two minutes (the load's own fills are
        # not churn), how far the count swings between consecutive 10 s reports.
        settled = images[12:] if len(images) > 18 else images[len(images) // 2 :]
        metrics["images_settled_min"] = min(settled)
        metrics["images_settled_max"] = max(settled)
        swings = [abs(b - a) / max(a, b, 1) for a, b in zip(settled, settled[1:])]
        metrics["image_swing_median_pct"] = (
            round(100 * statistics.median(swings), 1) if swings else None
        )
        metrics["image_swing_max_pct"] = round(100 * max(swings), 1) if swings else None

    # Budget lines: renderer's own share, the driver's own share, and the reserve inputs.
    budget_lines = [tuple(int(x) for x in m) for m in RE_BUDGET.findall(log)]
    if budget_lines:
        renderer = [b[4] for b in budget_lines]
        driver = [b[5] for b in budget_lines]
        metrics["renderer_share_first_mib"] = renderer[0]
        metrics["renderer_share_last_mib"] = renderer[-1]
        metrics["renderer_share_max_mib"] = max(renderer)
        metrics["renderer_share_median_mib"] = median(renderer)
        metrics["driver_share_last_mib"] = driver[-1]
        metrics["driver_share_max_mib"] = max(driver)
        metrics["budget_mib_first"] = budget_lines[0][0]
        metrics["budget_mib_last"] = budget_lines[-1][0]
        metrics["budget_mib_min"] = min(b[0] for b in budget_lines)
    reserve_lines = [tuple(int(x) for x in m) for m in RE_RESERVE.findall(log)]
    if reserve_lines:
        last = reserve_lines[-1]
        metrics["reserve"] = {
            "budget_mib": last[0],
            "hw_memsize_mib": last[1],
            "wired_mib": last[2],
            "compressor_mib": last[3],
            "reserve_mib": last[4],
            "available_mib": last[5],
            "floor_mib": last[6],
            "port_budget_mib": last[7],
            "lines": len(reserve_lines),
            "binding": "reserve" if last[0] < last[7] else "port",
            "wired_max_mib": max(r[2] for r in reserve_lines),
            "compressor_max_mib": max(r[3] for r in reserve_lines),
        }

    # Trims.
    trims = RE_TRIM.findall(log)
    metrics["trims"] = len(trims)
    if trims:
        deltas = [int(d) for _, d in trims]
        metrics["trim_delta_median_mib"] = median(deltas)
        metrics["trim_delta_total_mib"] = sum(deltas)
        metrics["trim_delta_min_mib"] = min(deltas)
    metrics["loadonly_lines"] = sum(1 for line in lines if "not play: last frame" in line)

    path = OUT / f"{tag}-metrics.json"
    path.write_text(json.dumps(metrics, indent=2) + "\n")

    print(f"--- {tag}")
    if metrics["dead"]:
        print(f"DEAD RUN: {metrics.get('death_line', '; '.join(metrics['death_markers']))}")
        print(f"metrics -> {path}")
        return
    def get(key):
        return metrics.get(key, "?")
    print(
        f"footprint start {get('footprint_start_mib')} / steady {get('footprint_steady_mean_mib')}"
        f" / end {get('footprint_end_mib')} / max {get('footprint_max_mib')} MiB"
    )
    print(
        f"renderer's own {get('renderer_share_median_mib')} (max "
        f"{get('renderer_share_max_mib')}) MiB, driver's own "
        f"{get('driver_share_last_mib')} MiB, budget {get('budget_mib_first')}->"
        f"{get('budget_mib_last')} MiB"
    )
    if "reserve" in metrics:
        r = metrics["reserve"]
        print(
            f"reserve: {r['budget_mib']} MiB ({r['binding']} binding; memsize {r['hw_memsize_mib']}"
            f" - wired {r['wired_mib']} - compressor {r['compressor_mib']} - reserve"
            f" {r['reserve_mib']} = {r['available_mib']}, floor {r['floor_mib']}); "
            f"{r['lines']} lines"
        )
    print(
        f"allocators {get('allocators_first')}->{get('allocators_last')} (max "
        f"{get('allocators_max')}), held {get('allocator_mib_last')} MiB last / "
        f"{get('allocator_mib_max')} max"
    )
    print(
        f"{get('windows')} windows: {get('windows_over_100ms')} over 100 ms, fps median "
        f"{get('fps_median')}, worst-frame median {get('worst_frame_median_ms')} ms"
    )
    print(
        f"draws/frame max {get('draws_per_frame_max')} (gameplay {get('gameplay_reached')}), "
        f"trims {get('trims')}, images settled {get('images_settled_min')}-"
        f"{get('images_settled_max')}, swing median {get('image_swing_median_pct')}% max "
        f"{get('image_swing_max_pct')}%; free+inactive min {get('free_inactive_min_mib')} MiB"
    )
    print(f"metrics -> {path}")


if __name__ == "__main__":
    main()
