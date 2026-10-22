#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""bbport: size the pipeline-precompile problem from instrumentation, not from estimates.

Reads what the run wrote and answers four questions, each as a number:

  1. Per-stage pipeline creation cost — the port's GCN->SPIR-V translation, KosmicKrisp's
     NIR->MSL, Metal's MTLLibrary, Metal's pipeline state — and the same split with the driver's
     on-disk shader cache hot and cold (BB_PIPE_TIMING + BB_KK_TIMING, two runs).
  2. The shape of the hitch frames that remain: how many, and whether each is N small serial
     compiles or a few large ones, and which gate of the async policy declined the pipeline.
  3. Shader coverage: distinct shaders BOUND in a run (BB_SHADER_BINDS), distinct shaders in the
     pipeline store (drawn: the store's .spv names are <program hash>_<perm>), distinct shaders on
     disc (the "OrbShdr" headers in the shader bundles and eboot.bin).
  4. Pipelines per shader, and the store's coverage of the game.

  python3 tools/macos/pipeline_sizing.py --hot out/sizing --json out/sizing/sizing.json

`--hot DIR` / `--cold DIR` each hold pipe.txt, kk.txt and (optionally) binds.txt from one run of
tools/macos/sizing_run.sh. Nothing here launches the game.
"""

import argparse
import collections
import glob
import json
import os
import re
import struct
import sys
import zlib

HITCH_MS = 50.0


# ---- the port's own log (BB_PIPE_TIMING) ------------------------------------------------------
def parse_pipe(path):
    """PIPE lines (one per pipeline created) and FRAME lines (one per frame the drawing thread saw)."""
    pipes, frames = [], []
    if not path or not os.path.exists(path):
        return pipes, frames
    for line in open(path, "r", errors="replace"):
        if line.startswith("PIPE "):
            fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            try:
                pipes.append(
                    {
                        "kind": fields["kind"],
                        "frame": int(fields["frame"]),
                        "key": fields["key"],
                        "tid": fields.get("tid", "0"),
                        "stages": [s for s in fields.get("stages", "").split(",") if s],
                        "rt": fields.get("rt", "0"),
                        "idx": int(fields.get("idx", 0)),
                        "indirect": int(fields.get("indirect", 0)),
                        "frame_draws": int(fields.get("frame_draws", 0)),
                        "async": fields.get("async", "?"),
                        "port_us": float(fields.get("port_us", 0)),
                        "drv_us": float(fields.get("drv_us", 0)),
                    }
                )
            except (KeyError, ValueError):
                continue
        elif line.startswith("FRAME "):
            fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            try:
                frames.append(
                    {
                        "n": int(fields["n"]),
                        "ms": float(fields["ms"]),
                        "draws": int(fields["draws"]),
                        "created": int(fields["created"]),
                        "created_ms": float(fields["created_ms"]),
                        "async": int(fields.get("async", 0)),
                        "skipped": int(fields.get("skipped", 0)),
                    }
                )
            except (KeyError, ValueError):
                continue
    return pipes, frames


# ---- the driver's log (BB_KK_TIMING) ----------------------------------------------------------
def parse_kk(path):
    """KKPIPE lines: hit (the disk cache had the MSL), nir_msl_us, lib_us, pso_us, per thread."""
    lines = []
    if not path or not os.path.exists(path):
        return lines
    for line in open(path, "r", errors="replace"):
        if not line.startswith("KKPIPE "):
            continue
        fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
        try:
            lines.append(
                {
                    "tid": fields.get("tid", "0"),
                    "hit": int(fields["hit"]),
                    "nir_msl_us": float(fields["nir_msl_us"]),
                    "lib_us": float(fields["lib_us"]),
                    "pso_us": float(fields["pso_us"]),
                }
            )
        except (KeyError, ValueError):
            continue
    return lines


def pair_layers(pipes, kk):
    """Attach each KKPIPE line to the PIPE line it belongs to.

    Both logs are written in creation order and both carry the creating thread, so the k-th
    graphics pipeline of a thread in the port's log is the k-th KKPIPE line of that thread. Only
    the synchronous creations (kind g/w) are paired: a background compile (kind s) creates the
    pipeline later on a worker thread, where it is timed on its own.
    """
    by_thread = collections.defaultdict(list)
    for line in kk:
        by_thread[line["tid"]].append(line)
    used = collections.Counter()
    paired = 0
    for pipe in pipes:
        if pipe["kind"] not in ("g", "w"):
            continue
        tid = pipe["tid"]
        pool = by_thread.get(tid, [])
        index = used[tid]
        if index < len(pool):
            layer = pool[index]
            pipe["kk"] = layer
            pipe["hit"] = layer["hit"]
            pipe["nir_msl_us"] = layer["nir_msl_us"]
            pipe["lib_us"] = layer["lib_us"]
            pipe["pso_us"] = layer["pso_us"]
            paired += 1
        used[tid] += 1
        pipe.setdefault("hit", None)
    return paired


def split_totals(pipes):
    """The four layers' totals over the paired pipelines, in ms."""
    totals = collections.Counter()
    counts = collections.Counter()
    for pipe in pipes:
        if pipe["kind"] not in ("g", "w", "c"):
            continue
        totals["port_ms"] += pipe["port_us"] / 1e3
        totals["driver_ms"] += pipe["drv_us"] / 1e3
        counts["n"] += 1
        if pipe.get("hit") is not None:
            totals["nir_msl_ms"] += pipe["nir_msl_us"] / 1e3
            totals["mtllib_ms"] += pipe["lib_us"] / 1e3
            totals["pso_ms"] += pipe["pso_us"] / 1e3
            counts["paired"] += 1
            counts["hit"] += pipe["hit"]
    # What the driver spent that its own timers do not name: SPIR-V->NIR, the pipeline cache's
    # lookup and blob write, descriptor/pipeline bookkeeping.
    if counts["paired"]:
        totals["other_ms"] = (totals["driver_ms"] - totals["nir_msl_ms"] - totals["mtllib_ms"] -
                              totals["pso_ms"])
    return totals, counts


def percentiles(values, points=(50, 90, 99, 100)):
    if not values:
        return {}
    ordered = sorted(values)
    out = {}
    for point in points:
        index = min(len(ordered) - 1, int(round(point / 100.0 * (len(ordered) - 1))))
        out[point] = round(ordered[index], 1)
    return out


def hitch_shape(pipes, frames):
    """Frames over the hitch threshold: how many, what they cost, and the pipeline set in each."""
    by_frame = collections.defaultdict(list)
    for pipe in pipes:
        by_frame[pipe["frame"]].append(pipe)
    # One record per frame number: the drawing thread writes one FRAME line per frame it saw, and a
    # frame must not be counted twice if the line was written more than once.
    seen = {}
    for frame in frames:
        seen[frame["n"]] = frame
    hitches = []
    for number in sorted(seen):
        frame = seen[number]
        if frame["ms"] < HITCH_MS:
            continue
        created = by_frame.get(number, [])
        hitches.append(
            {
                "frame": number,
                "ms": round(frame["ms"], 1),
                "draws": frame["draws"],
                "pipelines": len(created),
                "created_ms": round(frame["created_ms"], 1),
                "costs": sorted(round(p["port_us"] / 1e3 + p["drv_us"] / 1e3, 1) for p in created),
                "reasons": dict(collections.Counter(p["async"] for p in created)),
                "kinds": dict(collections.Counter(p["kind"] for p in created)),
            }
        )
    sizes = [h["pipelines"] for h in hitches]
    return {
        "frames_over_ms": HITCH_MS,
        "count": len(hitches),
        "total_ms": round(sum(h["ms"] for h in hitches), 1),
        "pipelines_in_them": sum(sizes),
        "pipelines_per_hitch": _dist(sizes),
        "worst": hitches[:0] + sorted(hitches, key=lambda h: -h["ms"])[:10],
        "all": hitches,
    }


def _dist(values):
    if not values:
        return {}
    return {
        "n": len(values),
        "max": max(values),
        "mean": round(sum(values) / len(values), 2),
        "hist": dict(sorted(collections.Counter(values).items())),
    }


def async_summary(pipes):
    """Why the background compiler declined the pipelines that stayed on the drawing thread."""
    reasons = collections.Counter()
    for pipe in pipes:
        if pipe["kind"] in ("g", "w", "c"):
            reasons[pipe["async"]] += 1
    return dict(reasons.most_common())


# ---- the store (what has been drawn) ----------------------------------------------------------
def store_stats(store):
    if not store or not os.path.isdir(store):
        return {}
    spv = [os.path.basename(p) for p in glob.glob(os.path.join(store, "*.spv"))]
    keys = [os.path.basename(p) for p in glob.glob(os.path.join(store, "*.key"))]
    metas = glob.glob(os.path.join(store, "*.meta"))
    pgm = set()
    perms = collections.Counter()
    for name in spv:
        match = re.match(r"(0x[0-9a-fA-F]+)_(\d+)\.spv$", name)
        if match:
            pgm.add(int(match.group(1), 16))
            perms[int(match.group(1), 16)] += 1
    graphics = sum(1 for k in keys if k.startswith("g_"))
    compute = sum(1 for k in keys if k.startswith("c_"))
    return {
        "dir": store,
        "files": len(glob.glob(os.path.join(store, "*"))),
        "keys": len(keys),
        "graphics_keys": graphics,
        "compute_keys": compute,
        "spv": len(spv),
        "meta": len(metas),
        "distinct_shaders": len(pgm),
        "shader_hashes": sorted(pgm),
        "permutations_per_shader_max": max(perms.values()) if perms else 0,
        "permutations_per_shader_mean": round(sum(perms.values()) / len(perms), 2) if perms else 0,
    }


# ---- the disc (what exists) -------------------------------------------------------------------
ORBSHDR = b"OrbShdr"


def orb_shaders(buf):
    """Every OrbShdr header in `buf`: (shader hash, version, type). Nothing is decoded."""
    out = []
    at = 0
    while True:
        at = buf.find(ORBSHDR, at)
        if at < 0:
            return out
        if at + 28 <= len(buf):
            version = buf[at + 7]
            bits = struct.unpack_from("<I", buf, at + 8)[0]
            shader_hash = struct.unpack_from("<Q", buf, at + 16)[0]
            out.append((shader_hash, version, (bits >> 2) & 0xF))
        at += len(ORBSHDR)


def dcx_decompress(data):
    """Sony DCX around a zlib blob (DCS/DCP/DCA), as the game's .dcx files are packed."""
    if data[:4] != b"DCX\x00":
        return data
    dcs = struct.unpack_from(">I", data, 0x08)[0]
    _uncompressed, compressed = struct.unpack_from(">II", data, dcs + 4)
    return zlib.decompress(data[len(data) - compressed:])


def disc_stats(game_dir):
    if not game_dir or not os.path.isdir(game_dir):
        return {}
    files = []
    bundles = sorted(glob.glob(os.path.join(game_dir, "dvdroot_ps4", "shader", "*.dcx")))
    for path in bundles:
        try:
            raw = dcx_decompress(open(path, "rb").read())
        except zlib.error as error:
            files.append({"file": os.path.basename(path), "error": str(error)})
            continue
        found = orb_shaders(raw)
        files.append(
            {
                "file": os.path.basename(path),
                "bytes": len(raw),
                "headers": len(found),
                "distinct": len({h for h, _, _ in found}),
            }
        )
    eboot = os.path.join(game_dir, "eboot.bin")
    hashes = set()
    headers = 0
    if os.path.exists(eboot):
        found = orb_shaders(open(eboot, "rb").read())
        headers = len(found)
        hashes |= {h for h, _, _ in found}
    for path in bundles:
        try:
            hashes |= {h for h, _, _ in orb_shaders(dcx_decompress(open(path, "rb").read()))}
        except zlib.error:
            continue
    return {
        "game_dir": game_dir,
        "bundles": files,
        "eboot_headers": headers,
        "headers": headers + sum(f.get("headers", 0) for f in files),
        "distinct_shaders": len(hashes),
        "shader_hashes": sorted(hashes),
        # The debug bundle repeats the release one; counted, and stated so it is not read as
        # extra coverage.
        "debug_bundle_distinct": next(
            (f["distinct"] for f in files if "debug" in f.get("file", "")), 0),
    }


# ---- the run's binds (BB_SHADER_BINDS) --------------------------------------------------------
def bind_stats(path):
    if not path or not os.path.exists(path):
        return {}
    hashes, stages, unresolved = set(), collections.Counter(), 0
    for line in open(path, "r", errors="replace"):
        if not line.startswith("BIND "):
            continue
        fields = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
        stage = fields.get("stage", "?")
        stages[stage] += 1
        digest = fields.get("hash", "none")
        if digest == "none":
            unresolved += 1
        else:
            hashes.add(int(digest, 16))
    return {
        "distinct_bound": len(hashes),
        "bound_hashes": sorted(hashes),
        "by_stage": dict(stages.most_common()),
        "unresolved_addresses": unresolved,
    }


def run_summary(directory):
    pipes, frames = parse_pipe(os.path.join(directory, "pipe.txt"))
    kk = parse_kk(os.path.join(directory, "kk.txt"))
    paired = pair_layers(pipes, kk)
    totals, counts = split_totals(pipes)
    created = [p for p in pipes if p["kind"] in ("g", "w", "c")]
    return {
        "dir": directory,
        "pipelines_created": len(created),
        "background_compiles": sum(1 for p in pipes if p["kind"] == "s"),
        "async_waited": sum(1 for p in pipes if p["kind"] == "w"),
        "kk_lines": len(kk),
        "paired": paired,
        "layers_ms": {k: round(v, 1) for k, v in totals.items()},
        "counts": dict(counts),
        "per_pipeline_us": {
            "port": percentiles([p["port_us"] for p in created]),
            "driver": percentiles([p["drv_us"] for p in created]),
            "nir_msl": percentiles([p.get("nir_msl_us", 0) for p in created if p.get("hit") is not None]),
            "mtllib": percentiles([p.get("lib_us", 0) for p in created if p.get("hit") is not None]),
            "pso": percentiles([p.get("pso_us", 0) for p in created if p.get("hit") is not None]),
        },
        "cache_hits": counts.get("hit", 0),
        "async_reasons": async_summary(pipes),
        "hitch": hitch_shape(pipes, frames),
        "frames_seen": len(frames),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--hot", help="directory of a run with the driver's shader cache warm")
    parser.add_argument("--cold", help="directory of a run after deleting that cache")
    parser.add_argument("--store", help="the pipeline store the run used (<user>/cache/<serial>-arm64)")
    parser.add_argument("--game", help="the game directory (CUSA03173), for the disc's shaders")
    parser.add_argument("--json", help="write the report here")
    args = parser.parse_args()

    report = {}
    for name in ("hot", "cold"):
        directory = getattr(args, name)
        if directory:
            report[name] = run_summary(directory)
            bind = bind_stats(os.path.join(directory, "binds.txt"))
            if bind:
                report[name]["binds"] = bind

    store = store_stats(args.store) if args.store else {}
    if store:
        report["store"] = {k: v for k, v in store.items() if k != "shader_hashes"}
    disc = disc_stats(args.game) if args.game else {}
    if disc:
        report["disc"] = {k: v for k, v in disc.items() if k != "shader_hashes"}

    if store:
        report["pipelines_per_shader"] = {
            "graphics_keys_per_shader": round(store["graphics_keys"] / store["distinct_shaders"], 2)
            if store["distinct_shaders"] else 0,
            "all_keys_per_shader": round(store["keys"] / store["distinct_shaders"], 2)
            if store["distinct_shaders"] else 0,
        }
    if store and disc:
        per_shader = store["graphics_keys"] / store["distinct_shaders"] if store["distinct_shaders"] else 0
        ceiling = per_shader * disc["distinct_shaders"]
        report["ceiling"] = {
            "disc_shaders": disc["distinct_shaders"],
            "graphics_pipelines_estimate": round(ceiling, 0),
            "store_graphics_keys": store["graphics_keys"],
            "coverage_pct": round(100.0 * store["graphics_keys"] / ceiling, 2) if ceiling else 0,
            "factor_to_full": round(ceiling / store["graphics_keys"], 1) if store["graphics_keys"] else 0,
        }
        # Do the two hash spaces agree? A store shader that is not on disc would mean the hashes are
        # not comparable (or the disc scan missed a container), which is worth saying out loud.
        if store.get("shader_hashes"):
            shared = set(store["shader_hashes"]) & set(disc["shader_hashes"])
            report["hash_overlap"] = {
                "store_shaders_on_disc": len(shared),
                "store_shaders": len(store["shader_hashes"]),
            }
    if "hot" in report and "cold" in report:
        hot, cold = report["hot"]["layers_ms"], report["cold"]["layers_ms"]
        report["hot_vs_cold"] = {
            k: round(cold.get(k, 0) - hot.get(k, 0), 1)
            for k in ("port_ms", "nir_msl_ms", "mtllib_ms", "pso_ms", "driver_ms")
        }

    text = json.dumps(report, indent=1)
    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w") as handle:
            handle.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
