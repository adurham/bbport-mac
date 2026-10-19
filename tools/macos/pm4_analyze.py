#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The morning report of the PM4 campaign (docs/PM4_PREREG_2026-10-19.md).

Reads the run record (one JSON object per line, plus a trailing 'SWEEPDONE <n> runs' line) and
prints the report by the rules fixed before the first run: counts per arm, mismatch rates, asserts,
invalid runs, per-run machine load, D* and the verdict.

  python3 tools/macos/pm4_analyze.py                       # docs/pm4-results/<today>.jsonl
  python3 tools/macos/pm4_analyze.py PATH.jsonl --md out/report.md
"""
import argparse
import json
import math
import os
import sys
from collections import OrderedDict
from datetime import date

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load(path):
    runs, notes = [], []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith("{"):
                notes.append(line)  # 'SWEEPDONE <n> runs'
                continue
            try:
                runs.append(json.loads(line))
            except json.JSONDecodeError as e:
                notes.append("line %d is not JSON (%s): %.80s" % (n, e, line))
    return runs, notes


def wilson(k, n, z=1.96):
    """95% interval of k successes in n trials (Wilson). None when n is 0."""
    if n == 0:
        return None
    p = k / n
    d = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / d
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, centre - half), min(1.0, centre + half)


def fmt_rate(k, n):
    if n == 0:
        return "n/a"
    ci = wilson(k, n)
    return "%d/%d = %.1f%% (95%% %.1f-%.1f%%)" % (k, n, 100 * k / n, 100 * ci[0], 100 * ci[1])


def arm_key(r):
    return r.get("arm") or "unknown"


def summarize(runs):
    arms = OrderedDict()
    for r in runs:
        a = arms.setdefault(
            arm_key(r),
            {"runs": 0, "valid": 0, "invalid": 0, "mismatch_runs": 0, "mismatches": 0,
             "checked": 0, "asserts": 0, "bad_pm4": 0, "loads": [], "delay": set(),
             "async": set(), "max_draws": 0},
        )
        a["runs"] += 1
        a["delay"].add(r.get("delay_us", 0))
        a["async"].add(bool(r.get("async", False)))
        if not r.get("valid"):
            a["invalid"] += 1
            continue
        a["valid"] += 1
        m = int(r.get("mismatches", 0) or 0)
        a["mismatches"] += m
        a["mismatch_runs"] += 1 if m else 0
        a["checked"] += int(r.get("checked", 0) or 0)
        a["asserts"] += int(r.get("asserts", 0) or 0)
        a["bad_pm4"] += int(r.get("bad_pm4", 0) or 0)
        a["max_draws"] = max(a["max_draws"], int(r.get("max_draws", 0) or 0))
        try:
            a["loads"].append(float(str(r.get("load", "")).split()[0]))
        except (ValueError, IndexError):
            pass
    return arms


def pick_dstar(arms):
    """The smallest delay at which 2 of 3 valid old-path runs saw a mismatch."""
    candidates = []
    for name, a in arms.items():
        if a["valid"] == 0 or not any(a["async"]) or "old" not in name:
            continue
        delay = max(a["delay"]) if a["delay"] else 0
        candidates.append((delay, name, a))
    candidates.sort()
    for delay, name, a in candidates:
        if a["mismatch_runs"] >= 2 and a["valid"] >= 3:
            return delay, name
    return None, None


def verdict(arms, dstar):
    old = [a for n, a in arms.items() if "old" in n and a["valid"]]
    fixed = [a for n, a in arms.items() if "fixed" in n and a["valid"]]
    old_runs = sum(a["valid"] for a in old)
    old_mm = sum(a["mismatch_runs"] for a in old)
    old_checked = sum(a["checked"] for a in old)
    fix_runs = sum(a["valid"] for a in fixed)
    fix_mm = sum(a["mismatch_runs"] for a in fixed)
    fix_asserts = sum(a["asserts"] for a in fixed)
    if not fixed:
        if old_runs >= 3 and old_mm == 0:
            return ("**fix closes the race, causality unproven in-game** — the positive control was "
                    "negative: %d valid old-path runs checked %d submissions and no submission "
                    "changed between submit and copy, so the in-game harness does not exercise the "
                    "race and the fixed arm was not launched (prereg: control negative). The "
                    "protocol itself is covered by the synthetic result "
                    "(docs/PM4_SYNTHETIC_2026-10-19.md)." % (old_runs, old_checked))
        return ("**unproven at this sample size** — no valid fixed-path runs in the record "
                "(old: %d valid, %d with a mismatch)." % (old_runs, old_mm))
    if fix_mm:
        rate_fix = fix_mm / fix_runs
        rate_old = (old_mm / old_runs) if old_runs else 0.0
        if old_runs and rate_fix >= 0.8 * rate_old:
            return ("**fix does not address the bug** — fixed %s vs old %s."
                    % (fmt_rate(fix_mm, fix_runs), fmt_rate(old_mm, old_runs)))
        return ("**unproven at this sample size** — fixed %s still tears (old %s)."
                % (fmt_rate(fix_mm, fix_runs), fmt_rate(old_mm, old_runs)))
    if old_mm and dstar is not None:
        return ("**fix closes the race, causality supported** — old path tore in %s, fixed path in "
                "%s (%d valid runs, %d asserts)."
                % (fmt_rate(old_mm, old_runs), fmt_rate(fix_mm, fix_runs), fix_runs, fix_asserts))
    return ("**fix closes the race, causality unproven in-game** — old path: %s, fixed path: %s "
            "(%d valid runs, %d asserts). The in-game harness never reproduced a tear, so the "
            "assertion's cause is not shown in-game; the synthetic result is the evidence for the "
            "protocol." % (fmt_rate(old_mm, old_runs), fmt_rate(fix_mm, fix_runs), fix_runs,
                           fix_asserts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?",
                    default=os.path.join(ROOT, "docs", "pm4-results", "%s.jsonl" % date.today()))
    ap.add_argument("--md", help="also write the report here")
    args = ap.parse_args()

    if not os.path.exists(args.path):
        sys.exit("no run record at %s" % args.path)
    runs, notes = load(args.path)
    arms = summarize(runs)
    dstar, dstar_arm = pick_dstar(arms)

    out = []
    w = out.append
    w("# PM4 campaign report — %s" % os.path.basename(args.path).replace(".jsonl", ""))
    w("")
    w("Generated by `tools/macos/pm4_analyze.py` from `%s` (%d run records%s). Rules: "
      "`docs/PM4_PREREG_2026-10-19.md`."
      % (os.path.relpath(args.path, ROOT), len(runs),
         ", " + "; ".join(notes) if notes else ""))
    w("")
    w("## Per arm")
    w("")
    w("| arm | runs | valid | invalid | runs with a mismatch | mismatches | checked submissions | "
      "mismatches / 100k | asserts | bad_pm4 | mean load |")
    w("|---|---|---|---|---|---|---|---|---|---|---|")
    for name, a in arms.items():
        per100k = (100000.0 * a["mismatches"] / a["checked"]) if a["checked"] else float("nan")
        mean = ("%.2f" % (sum(a["loads"]) / len(a["loads"]))) if a["loads"] else "?"
        w("| %s | %d | %d | %d | %d | %d | %d | %s | %d | %d | %s |"
          % (name, a["runs"], a["valid"], a["invalid"], a["mismatch_runs"], a["mismatches"],
             a["checked"], ("%.2f" % per100k) if a["checked"] else "n/a", a["asserts"],
             a["bad_pm4"], mean))
    w("")
    w("## D* (smallest delay with a mismatch in 2 of 3 valid old-path runs)")
    w("")
    if dstar is None:
        best = max([0] + [a["valid"] for n, a in arms.items() if "old" in n])
        if best < 3:
            w("**D\\* is undefined** — no old-path delay has 3 valid runs yet.")
        else:
            w("**D\\* is undefined** — no old-path delay reached 2 of 3 valid runs with a mismatch. "
              "The hidden warm route did not exercise the race at a rate this harness can see, so "
              "the fixed arm cannot be validated in-game from this control (prereg: control "
              "negative).")
    else:
        w("**D\\* = %d us** (arm `%s`)." % (dstar, dstar_arm))
    w("")
    w("## Verdict (mechanical, per the preregistered rule)")
    w("")
    w(verdict(arms, dstar))
    w("")
    w("## Per run")
    w("")
    w("| label | arm | delay us | valid | mismatches | checked | asserts | bad_pm4 | max draws | "
      "load |")
    w("|---|---|---|---|---|---|---|---|---|---|")
    for r in runs:
        delay = r.get("delay_us", 0)
        w("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |"
          % (r.get("label", "?"), arm_key(r), delay, "yes" if r.get("valid") else "NO",
             r.get("mismatches", 0), r.get("checked", 0), r.get("asserts", 0),
             r.get("bad_pm4", 0), r.get("max_draws", 0), r.get("load", "?")))
    invalid = [r for r in runs if not r.get("valid")]
    w("")
    w("## Invalid runs (%d)" % len(invalid))
    w("")
    if not invalid:
        w("None.")
    for r in invalid:
        w("- `%s` (%s): %s — log `%s`"
          % (r.get("label", "?"), arm_key(r), r.get("invalid_reason", "no reason recorded"),
             r.get("log", "?")))
    w("")
    text = "\n".join(out)
    print(text)
    if args.md:
        os.makedirs(os.path.dirname(os.path.abspath(args.md)), exist_ok=True)
        with open(args.md, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print("wrote %s" % args.md, file=sys.stderr)


if __name__ == "__main__":
    main()
