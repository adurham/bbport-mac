#!/usr/bin/env python3
"""tools/macos/kkair_union_report.py OUT_KKAIR HELDOUT ROUTE...: coverage of the union capture
(kkair_union.sh) and how it generalises to the held-out route.

Per capture route (pass B run B-<route>): pipelines seen, how many the shipped segments already
covered (the driver's own "created in < 1 ms" count, i.e. archive hits), and by port pipeline key how
many were new against every earlier route. For the held-out proof: the fraction of its pipeline keys
any capture route created, and the per-pipeline PSO split (hit < 1 ms vs miss).
"""
import json, os, re, statistics, sys

out, held, routes = sys.argv[1], sys.argv[2], sys.argv[3:]


def keys(d):
    p = os.path.join(out, d, "pipe.txt")
    if not os.path.exists(p):
        return set()
    return {m.group(1) for l in open(p, errors="replace") if l.startswith("PIPE ")
            for m in [re.search(r"key=([0-9a-f]+)", l)] if m}


def kkair_last(d):
    p = os.path.join(out, d, "out", "last-run.log")
    if not os.path.exists(p):
        return None
    last = None
    for l in open(p, errors="replace"):
        m = re.match(r"KK air: (\d+) libraries from AIR, (\d+) compiled from MSL, (\d+) AIR files unusable; (\d+) pipelines"
                     r"(?: \((\d+) with lookupArchives over (\d+) archives, (\d+) created in < 1 ms\))?", l)
        if m:
            last = [int(x) if x else 0 for x in m.groups()]
    return dict(zip("air msl bad pipes lookup archives fast".split(), last)) if last else None


def kk_pso(d):
    p = os.path.join(out, d, "kk.txt")
    v = [float(m.group(1)) for l in open(p, errors="replace") if l.startswith("KKPIPE")
         for m in [re.search(r"pso_us=([0-9.]+)", l)] if m] if os.path.exists(p) else []
    return v


res = {"routes": [], "heldout": held}
seen = set()
for r in routes:
    for pfx in ("A", "B"):
        pass
    kb = keys(f"B-{r}") or keys(f"A-{r}")
    ka = keys(f"A-{r}")
    allk = ka | kb
    new = allk - seen
    k = kkair_last(f"B-{r}") or {}
    res["routes"].append({"route": r, "pipelines_passA": len(ka), "pipelines_passB": len(kb),
                          "new_keys_vs_earlier_routes": len(new), "already_seen": len(allk) - len(new),
                          "passB_kkair": k})
    seen |= allk
res["union_keys"] = len(seen)
for tag in (f"H-{held}-ctrl", f"H-{held}-warm", f"H-{held}-proof"):
    hk = keys(tag)
    pso = kk_pso(tag)
    res[tag] = {"pipelines": len(hk), "in_capture_union": len(hk & seen),
                "frac_in_union": round(len(hk & seen) / len(hk), 4) if hk else None,
                "kkair": kkair_last(tag),
                "kk_pso_n": len(pso), "kk_pso_hits_lt1ms": sum(1 for x in pso if x < 1000),
                "kk_pso_miss_total_ms": round(sum(x for x in pso if x >= 1000) / 1000, 1),
                "kk_pso_miss_p50_ms": round(statistics.median([x for x in pso if x >= 1000]) / 1000, 2) if any(x >= 1000 for x in pso) else None}
# Pipelines of the held-out route that never appeared in any capture route: what they cost in the control.
ctrl_keys, proof_keys = keys(f"H-{held}-ctrl"), keys(f"H-{held}-proof")
res["heldout_only_keys_proof"] = len(proof_keys - seen)
res["heldout_only_keys_ctrl"] = len(ctrl_keys - seen)
print(json.dumps(res, indent=1))
