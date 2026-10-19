#!/usr/bin/env python3
"""Watch pm4_repro summary.txt files and append one JSON record per run to the JSONL.

Every run's log is also copied to out/pm4_prefix/<label>-<tag>.log so the raw evidence
survives the label's out/pm4_repro/<label>/ directory being rewritten.
"""
import json
import os
import re
import shutil
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUTDIR = os.path.join(ROOT, "out", "pm4_prefix")
JSONL = sys.argv[1]
ARM = sys.argv[2]
SHA = sys.argv[3]
BUILD = sys.argv[4]
SECS_LOG = sys.argv[5]
# remaining args: label|summary_path
pairs = [a.split("|", 1) for a in sys.argv[6:]]

seen = set()
starts = {}

ASSERT_RE = re.compile(r"assert_fail|GPU library assertion failed|Unreachable code")
BAD_RE = re.compile(r"Bad PM4 packet")
STOP_RE = re.compile(r"STOP:")


def read_starts():
    try:
        with open(SECS_LOG, errors="replace") as f:
            for line in f:
                m = re.search(r"(\d\d:\d\d:\d\d) run (\S+)/(\S+) \((\d+)s\)", line)
                if m:
                    starts[(m.group(2), m.group(3))] = (m.group(1), int(m.group(4)))
    except FileNotFoundError:
        pass


def classify(log_path, out_path):
    text = ""
    if os.path.exists(log_path):
        with open(log_path, errors="replace") as f:
            text = f.read()
    out = ""
    if os.path.exists(out_path):
        with open(out_path, errors="replace") as f:
            out = f.read()
    fatal = ""
    bad = False
    other_assert = False
    stopline = ""
    for line in text.splitlines():
        if not bad and BAD_RE.search(line):
            fatal = line.strip()
            bad = True
        if not other_assert and ASSERT_RE.search(line):
            other_assert = True
            if not fatal:
                fatal = line.strip()
        if not stopline and STOP_RE.search(line):
            stopline = line.strip()
    exited = "GPU process exited" in text
    m = re.search(r"GPU process exited \((\d+)\)", text)
    exit_code = m.group(1) if m else None
    if bad:
        outcome = "bad_pm4_assert"
    elif other_assert or stopline:
        outcome = "other_gpu_assert"
    elif exited and exit_code == "0":
        outcome = "clean"
    else:
        outcome = "dead"
    # valid: the title's Continue took (the run left the title) and the area's cloth threads exist
    valid = ("the title's Continue took" in out) and ("CSClothThread" in text)
    if bad and "0x70d1b005" in fatal:
        outcome = "bad_pm4_0x70d1b005"
    sub = ""
    sm = re.search(r"submission (\d+)", fatal or "")
    if sm:
        sub = sm.group(1)
    maxd = 0
    for mm in re.finditer(r"([0-9]+) draws/frame", text):
        maxd = max(maxd, int(mm.group(1)))
    return dict(valid=bool(valid), outcome=outcome, fatal=fatal,
                stop_line=stopline, exit_code=exit_code, submission=sub,
                max_draws=int(maxd))


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(os.path.dirname(JSONL), exist_ok=True)
    read_starts()
    while True:
        read_starts()
        for label, summary in pairs:
            if not os.path.exists(summary):
                continue
            with open(summary, errors="replace") as f:
                lines = [l.strip() for l in f if l.strip()]
            for line in lines:
                m = re.match(r"(\S+) (\S+) max_draws=(\d+) bad_pm4=(\d+) asserts=(\d+)", line)
                if not m:
                    continue
                lab, tag = m.group(1), m.group(2)
                if (lab, tag) in seen:
                    continue
                seen.add((lab, tag))
                d = os.path.join(ROOT, "out", "pm4_repro", lab)
                log = os.path.join(d, tag + ".log")
                out = os.path.join(d, tag + ".out")
                route = tag.rsplit("-", 1)[0]
                rec = classify(log, out)
                start, secs = starts.get((lab, tag), ("", None))
                rec.update({
                    "arm": ARM,
                    "config": os.environ.get("PREFIX_CONFIG", ""),
                    "build_sha": SHA,
                    "build": BUILD,
                    "label": lab,
                    "tag": tag,
                    "route": route,
                    "seconds": secs,
                    "started": start,
                    "finished": time.strftime("%H:%M:%S"),
                    "load": [round(x, 2) for x in os.getloadavg()],
                    "summary_line": line,
                    "host": "bbport-prefix-build" if ARM == "A" else "bbport-pm4-build",
                })
                dst = os.path.join(OUTDIR, "%s-%s.log" % (lab, tag))
                if os.path.exists(log):
                    shutil.copy2(log, dst)
                    rec["log"] = os.path.relpath(dst, ROOT)
                with open(JSONL, "a") as f:
                    f.write(json.dumps(rec, sort_keys=True) + "\n")
                print(json.dumps(rec, sort_keys=True), flush=True)
        time.sleep(5)


if __name__ == "__main__":
    main()
