#!/usr/bin/env python3
"""Pull N real KosmicKrisp vertex+fragment MSL pairs out of Mesa's on-disk shader cache.

KosmicKrisp patch 0001 makes Mesa keep a disk cache (~/.cache/mesa_shader_cache); each entry is a
small header followed by a zlib stream holding the compiled kk_shader, whose msl_compile_data is the
MSL source KosmicKrisp hands to MTL4Compiler. Pairs are matched on the FragmentIn user(vary_NN)
varyings the vertex shader's VertexOut provides, so every pair links into a render pipeline.

usage: extract_msl.py OUTDIR [N=20] [SEED=7]
writes OUTDIR/NN.vs.metal, NN.fs.metal and manifest.txt ("NN slot:type ...", colour outputs).
"""
import glob, hashlib, os, random, re, sys, zlib

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 7)
root = os.path.expanduser("~/.cache/mesa_shader_cache")

srcs = {"vertex": {}, "fragment": {}}
for f in glob.glob(root + "/*/*"):
    b = open(f, "rb").read()
    i = b.find(b"\x78\xda")
    if i < 0:
        continue
    try:
        d = zlib.decompressobj().decompress(b[i:])
    except zlib.error:
        continue
    for m in re.finditer(rb"#include <metal_stdlib>", d):
        e = d.find(b"\0", m.start())
        s = d[m.start(): e if e > 0 else len(d)]
        k = re.search(rb"^(vertex|fragment) ", s, re.M)
        if k:
            srcs[k.group(1).decode()][hashlib.sha1(s).hexdigest()] = s


def users(s, struct):
    m = re.search(rb"struct " + struct + rb" \{(.*?)\};", s, re.S)
    if not m:
        return {}
    return {x.group(3).decode(): x.group(1).decode()
            for x in re.finditer(rb"(\w+) (\w+) \[\[user\((\w+)\)", m.group(1))}


def colours(s):
    m = re.search(rb"struct FragmentOut \{(.*?)\};", s, re.S)
    if not m:
        return []
    return [(int(x.group(2)), x.group(1).decode())
            for x in re.finditer(rb"(\w+) \w+ \[\[color\((\d+)\)", m.group(1))]


V = [(h, s, users(s, b"VertexOut")) for h, s in srcs["vertex"].items()
     if b"[[position]]" in s and b"stage_in" not in s]
F = [(h, s, users(s, b"FragmentIn"), colours(s)) for h, s in srcs["fragment"].items() if colours(s)]
F.sort(key=lambda x: len(x[1]))
cands = F[len(F) // 5: 4 * len(F) // 5]          # middle 60 % by size: typical, not the extremes
random.shuffle(cands)
pairs, used = [], set()
for h, s, fi, cols in cands:
    vm = [v for v in V if v[0] not in used and all(v[2].get(k) == t for k, t in fi.items())]
    if not vm:
        continue
    vm.sort(key=lambda v: abs(len(v[1]) - 60000))
    used.add(vm[0][0])
    pairs.append((vm[0][1], s, cols))
    if len(pairs) == n:
        break

os.makedirs(out, exist_ok=True)
lines = []
for i, (vs, fs, cols) in enumerate(pairs):
    open(f"{out}/{i:02d}.vs.metal", "wb").write(vs)
    open(f"{out}/{i:02d}.fs.metal", "wb").write(fs)
    lines.append(f"{i:02d} " + " ".join(f"{c}:{t}" for c, t in cols))
open(f"{out}/manifest.txt", "w").write("\n".join(lines) + "\n")
print(f"{len(pairs)} pairs from {len(srcs['vertex'])} vertex / {len(srcs['fragment'])} fragment MSL sources -> {out}")
