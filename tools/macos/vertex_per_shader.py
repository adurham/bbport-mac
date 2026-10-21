#!/usr/bin/env python3
"""Per-shader normalised vertex cost from single-hash BB_SKIP_VS arms (bench/vertex-shaders).

    python3 tools/macos/vertex_per_shader.py CONTROL_TAG [CONTROL_TAG ...] -- SKIP_TAG ...

Each SKIP_TAG is `skiptrace-<hash8>`: its Metal trace (out/mtl-TAG-gpu.xml, tools/macos/mtl_encoders.py)
gives vertex ms/frame two ways: (a) total vertex / frames (frame-number column), and (b) vertex us per
render encoder x the CONTROL's render encoders per frame (robust to the frame estimate, the
doc's cross-check). The shader's index-vertices/frame come from the control's BB_PASS_TRACE
(out/pass-CONTROL.txt; a multi-shader encoder's indices split evenly - flagged in the doc).
ns/index-vertex = (control vertex - skip vertex) / indices.
"""
import collections
import json
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(os.path.dirname(here))


def enc_stats(tag):
    xml = f'{root}/out/mtl-{tag}-gpu.xml'
    if not os.path.exists(xml) or os.path.getsize(xml) == 0:
        return None
    jp = f'{os.environ.get("TMPDIR", "/tmp")}/vps-{tag}.json'
    subprocess.run([sys.executable, f'{here}/mtl_encoders.py', xml, '--json', jp],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    d = json.load(open(jp))
    d['v_per_enc_us'] = d['vertex_ms_per_frame'] * 1000 / d['render_encoders_per_frame']
    return d


def shader_indices(tag):
    path = f'{root}/out/pass-{tag}.txt'
    if not os.path.exists(path):
        return None
    tot = collections.Counter(); dr = collections.Counter(); frames = set()
    for line in open(path):
        if not line.startswith('PT '):
            continue
        p = line.split(); frames.add(int(p[1]))
        d = dict(x.split('=', 1) for x in p[3:] if '=' in x)
        if d.get('vs', '-') == '-':
            continue
        hs = d['vs'].split(',')
        for h in hs:
            tot[h[-8:]] += int(d['idx']) / len(hs); dr[h[-8:]] += int(d['dr']) / len(hs)
    n = len(frames)
    return {h: (dr[h] / n, tot[h] / n) for h in tot}


def main():
    a = sys.argv[1:]
    cut = a.index('--')
    ctrls, skips = a[:cut], a[cut + 1:]
    cs = [s for s in (enc_stats(t) for t in ctrls) if s]
    idx = None
    for t in ctrls:
        idx = idx or shader_indices(t)
    cv = sum(s['vertex_ms_per_frame'] for s in cs) / len(cs)
    cenc = sum(s['render_encoders_per_frame'] for s in cs) / len(cs)
    cpe = sum(s['v_per_enc_us'] for s in cs) / len(cs)
    print(f'control {",".join(ctrls)}: vertex {cv:.3f} ms/frame '
          f'({", ".join("%.3f" % s["vertex_ms_per_frame"] for s in cs)}), '
          f'{cenc:.1f} render enc/frame, {cpe:.2f} us/enc')
    print('| shader | draws/fr | M idx/fr | skip vertex ms/fr | delta ms (a) | delta ms (b) | ns/idx (a) | ns/idx (b) | trace frames |')
    print('|---|---|---|---|---|---|---|---|---|')
    out = []
    # Exact: what the skip run's own pass trace no longer records (the gate returns before the draw
    # is counted), against the control's total. Falls back to the even split when the skip run has
    # no pass trace.
    def totals(tag):
        path = f'{root}/out/pass-{tag}.txt'
        if not os.path.exists(path):
            return None
        fr = set(); s = d = 0
        for line in open(path):
            if line.startswith('PT '):
                p = line.split(); fr.add(int(p[1]))
                kv = dict(x.split('=', 1) for x in p[3:] if '=' in x)
                s += int(kv['idx']); d += int(kv['dr'])
        return s / len(fr), d / len(fr)
    ctot = totals(ctrls[0])
    for t in skips:
        h = t.split('-')[-1]
        s = enc_stats(t)
        dr, ix = idx.get(h, (0, 0))
        st = totals(t)
        if st and ctot:
            dr, ix = ctot[1] - st[1], ctot[0] - st[0]
        if ix <= 0:
            print(f'| {h} | {dr:.1f} | {ix / 1e6:.3f} | (not a shader skip, or nothing removed) | | | | | |')
            continue
        if not s:
            print(f'| {h} | {dr:.1f} | {ix / 1e6:.3f} | (no trace) | | | | | |')
            continue
        da = cv - s['vertex_ms_per_frame']
        db = (cpe - s['v_per_enc_us']) * cenc / 1000
        sv = s['vertex_ms_per_frame']
        print(f'| {h} | {dr:.1f} | {ix / 1e6:.3f} | {sv:.3f} | {da:+.3f} | {db:+.3f} | '
              f'{da * 1e6 / ix:.3f} | {db * 1e6 / ix:.3f} | {s["frames"]:.0f} |')
        out.append(dict(shader=h, draws=dr, idx=ix, skip_vertex=s['vertex_ms_per_frame'], da=da, db=db))
    json.dump(dict(control_vertex=cv, rows=out), open(f'{root}/out/vertex/per_shader.json', 'w'), indent=1)


if __name__ == '__main__':
    main()
