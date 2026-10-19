#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One JSONL line per PM4 run (docs/PM4_PREREG_2026-10-19.md).

Reads a run's log (out/last-run.log copied by pm4_repro.sh) and appends the record the analysis
uses: validity (reached the title's Continue AND created CSClothThread0-5), the hash-mismatch count
from BB_SUBMIT_CHECK, asserts, bad packets, draws/frame and the machine's load average. A run that
never reached the site is recorded as invalid, not as a zero (prereg).

  python3 tools/macos/pm4_record.py --label phase0-d500/attack-1.log --arm p0-old-d500 --run 2 \\
      --delay 500 --async --log out/pm4_repro/phase0-d500/attack-1.log
"""
import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_JSONL = os.path.join(ROOT, "docs", "pm4-results", "%s.jsonl" % datetime.now().strftime("%Y-%m-%d"))


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def count(pattern, text, flags=0):
    return len(re.findall(pattern, text, flags))


def load_average():
    try:
        out = subprocess.run(["sysctl", "-n", "vm.loadavg"], capture_output=True, text=True,
                             timeout=5).stdout
        return " ".join(out.replace("{", " ").replace("}", " ").split())
    except (OSError, subprocess.SubprocessError):
        try:
            return "%.2f %.2f %.2f" % os.getloadavg()
        except OSError:
            return "?"


def parse(log_path, secs):
    text = read(log_path)
    reached = count(r"pad file: buttons 0x4000", text) > 0
    cloth = len(set(re.findall(r"guest thread 'CSClothThread(\d)'", text)))
    # BB_SUBMIT_CHECK's window line: 'Submit check: N of M graphics submissions changed ...'
    mismatches, checked = 0, 0
    for m in re.finditer(r"Submit check: (\d+) of (\d+) graphics submissions changed", text):
        mismatches, checked = int(m.group(1)), int(m.group(2))
    detail = count(r"^Submit check: submission \d+", text, re.MULTILINE)
    if checked == 0:
        mismatches = detail  # no window line (short run): the per-submission lines are all we have
    asserts = count(r"Bad PM4 packet|Unreachable code|GPU library assertion failed|assert_fail", text)
    bad_pm4 = count(r"Bad PM4 packet", text)
    draws = [int(x) for x in re.findall(r"(\d+) draws/frame", text)]
    valid = reached and cloth == 6
    reason = ""
    if not reached:
        reason = "the title's Continue was never pressed (no pad 0x4000 line)"
    elif cloth != 6:
        reason = "only %d of CSClothThread0-5 were created: the run did not reach the area load" % cloth
    return {
        "valid": valid,
        "invalid_reason": reason,
        "reached_continue": reached,
        "cloth_threads": cloth,
        "mismatches": mismatches,
        "checked": checked,
        "detail_lines": detail,
        "asserts": asserts,
        "bad_pm4": bad_pm4,
        "max_draws": max(draws) if draws else 0,
        "secs": secs,
        "log": os.path.relpath(log_path, ROOT),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True, help="the run's label (unique in the record)")
    ap.add_argument("--arm", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--run", type=int, default=1)
    ap.add_argument("--route", default="attack")
    ap.add_argument("--delay", type=int, default=0, help="BB_SUBMIT_DELAY_US")
    ap.add_argument("--async", dest="async_", action="store_true",
                    help="BB_SUBMIT_ASYNC=1 (the old path)")
    ap.add_argument("--secs", type=int, default=0)
    ap.add_argument("--jsonl", default=DEFAULT_JSONL)
    ap.add_argument("--note", default="")
    args = ap.parse_args()

    record = {
        "label": args.label,
        "ts": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        "arm": args.arm,
        "route": args.route,
        "run": args.run,
        "delay_us": args.delay,
        "async": bool(args.async_),
        "load": load_average(),
        "note": args.note,
    }
    record.update(parse(args.log, args.secs))
    binary = os.path.join(ROOT, "out", "gpu-arm64", "bb-gpu")
    try:
        st = os.stat(binary)
        record["bb_gpu_bytes"] = st.st_size
        record["bb_gpu_mtime"] = datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds")
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(args.jsonl)), exist_ok=True)
    with open(args.jsonl, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, sort_keys=False) + "\n")
    print("%s %s valid=%s mismatches=%d checked=%d asserts=%d max_draws=%d"
          % (args.label, args.arm, record["valid"], record["mismatches"], record["checked"],
             record["asserts"], record["max_draws"]))
    if not record["valid"]:
        print("  INVALID: %s" % record["invalid_reason"], file=sys.stderr)


if __name__ == "__main__":
    main()
