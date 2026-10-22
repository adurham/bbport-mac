#!/usr/bin/env python3
"""out/mc/analyze.py DUMPDIR RUNLOG: motion-vector correctness on a BB_DUMP_TRIGGER capture.

Per frame i (1..N-1, consecutive dumped frames):
  - resolved motion (what FSR reads): median/p99/max px, pixels with a non-zero vector
  - camera-only field recomputed offline from depth + the 'Dump camera:' matrices (mirrors
    camera_motion.comp mode 0) -> agreement with the resolved field (does compose accept any
    object vector?)
  - raw objects attachment channel stats; obj.zw vs camera-only
  - "moving" pixels: |obj.zw - camera_only| > 0.5 px (the port's own per-vertex history says this
    surface moves differently from the camera)
  - image-warp residual on those pixels: cur(p) vs prev(p + v + prev_jitter - jitter) for
    v = resolved, v = obj.zw, v = 0; lower = the vector predicts the image better
Writes PNGs to DUMPDIR/../png-<tag>/.
"""
import os
import re
import sys

import numpy as np
from PIL import Image

W, H = 1146, 480
d, log = sys.argv[1], sys.argv[2]
tag = os.path.basename(d.rstrip('/')).replace('dump-', '')
png = os.path.join(os.path.dirname(d.rstrip('/')), f'png-{tag}')
os.makedirs(png, exist_ok=True)

cams = {}
for line in open(log, errors='replace'):
    if line.startswith('Dump camera: frame'):
        t = line.split()
        f = int(t[3])
        def grab(name, n):
            k = t.index(name)
            return np.array([float(x) for x in t[k + 1:k + 1 + n]])
        cams[f] = dict(view=grab('view', 12), prev_view=grab('prev_view', 12), proj=grab('proj', 4),
                       prev_proj=grab('prev_proj', 4), jitter=grab('jitter', 2),
                       prev_jitter=grab('prev_jitter', 2))


def inv_affine(m):
    M = m.reshape(3, 4)
    R = np.linalg.inv(M[:, :3])
    return np.concatenate([R, (-R @ M[:, 3])[:, None]], 1)


def mul(a, b):  # 3x4 * 3x4 affine
    A = np.vstack([a.reshape(3, 4), [0, 0, 0, 1]]); B = np.vstack([b.reshape(3, 4), [0, 0, 0, 1]])
    return (A @ B)[:3]


def camera_only(depth, c):
    rep = mul(c['prev_view'], inv_affine(c['view']).ravel())
    proj, pp = c['proj'], c['prev_proj']
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    uv = np.stack([(xs + .5) / W, (ys + .5) / H], -1)
    suv = uv - c['jitter'] / np.array([W, H])
    ndc = suv * 2 - 1
    sky = depth >= 1.0
    denom = np.minimum(depth - proj[2], -1e-8)
    z = np.where(sky, 1.0, proj[3] / denom)
    view = np.stack([ndc[..., 0] * z / proj[0], ndc[..., 1] * z / proj[1], z, np.where(sky, 0., 1.)], -1)
    pv = view @ rep.T
    pndc = np.stack([pv[..., 0] * pp[0], pv[..., 1] * pp[1]], -1) / np.maximum(pv[..., 2:3], 1e-4)
    puv = pndc * .5 + .5
    return (puv - suv) * np.array([W, H])


def load(i, name, dt, ch):
    p = os.path.join(d, f'f{i:03d}_{name}')
    a = np.fromfile(p, dt).astype(np.float32)
    return a.reshape(H, W, ch) if ch > 1 else a.reshape(H, W)


def luma(i):
    a = np.fromfile(os.path.join(d, f'f{i:03d}_input_1146x480_rgba.raw'), np.uint8).reshape(H, W, 4)
    return (a[..., :3].astype(np.float32) @ np.array([.299, .587, .114], np.float32)) / 255.


def bilinear(img, x, y):
    x = np.clip(x, 0, W - 1.001); y = np.clip(y, 0, H - 1.001)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); fx = x - x0; fy = y - y0
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy) +
            img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)


def residual(cur, prev, v, c, mask):
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    off = c['prev_jitter'] - c['jitter']
    w = bilinear(prev, xs + v[..., 0] + off[0], ys + v[..., 1] + off[1])
    e = np.abs(w - cur)
    return float(e[mask].mean()) if mask.any() else float('nan')


def vis(v, scale=4.0):
    r = np.clip(.5 + v[..., 0] / (2 * scale), 0, 1); g = np.clip(.5 + v[..., 1] / (2 * scale), 0, 1)
    return (np.stack([r, g, np.full_like(r, .5)], -1) * 255).astype(np.uint8)


frames = sorted(int(f[1:4]) for f in os.listdir(d) if f.endswith('_motion_1146x480_rg16f.raw'))
print(f'capture {d}: frames {frames}, camera lines {sorted(cams)}')
consec = all(np.allclose(cams[f]['prev_view'], cams[f - 1]['view']) for f in frames[1:] if f in cams and f - 1 in cams)
print('consecutive (prev_view[i] == view[i-1]):', consec)
rows = []
for i in frames:
    mv = load(i, 'motion_1146x480_rg16f.raw', np.float16, 2)
    ob = load(i, 'objects_1146x480_rgba32f.raw', np.float32, 4)
    dep = load(i, 'depth_1146x480_f32.raw', np.float32, 1)
    c = cams[i]
    co = camera_only(dep.astype(np.float64), c).astype(np.float32)
    mag = np.hypot(mv[..., 0], mv[..., 1])
    res_vs_cam = np.hypot(*(mv - co).transpose(2, 0, 1))
    zw = ob[..., 2:4]
    zw_vs_cam = np.hypot(*(zw - co).transpose(2, 0, 1))
    moving = (zw_vs_cam > 0.5) & (dep < 1.0)
    # the compose's own accept test, evaluated on the dump
    thr = np.maximum(2e-7, (c['proj'][2] - dep) * 0.01)
    accept = (ob[..., 2] > 0.99) & (np.abs(ob[..., 3] - dep) < thr)
    # camera translation / rotation between frames
    Vc = inv_affine(c['view']); Vp = inv_affine(c['prev_view'])
    dpos = float(np.linalg.norm(Vc[:, 3] - Vp[:, 3]))
    rot = np.degrees(np.arccos(np.clip((np.trace(Vc[:, :3].T @ Vp[:, :3]) - 1) / 2, -1, 1)))
    row = dict(frame=i, cam_move=dpos, cam_rot_deg=float(rot),
               mv_med=float(np.median(mag)), mv_p99=float(np.percentile(mag, 99)), mv_max=float(mag.max()),
               mv_nonzero=int((mag > 1e-3).sum()),
               resolved_vs_camonly_med=float(np.median(res_vs_cam)), resolved_vs_camonly_p99=float(np.percentile(res_vs_cam, 99)),
               resolved_eq_camonly_frac=float((res_vs_cam < 0.05).mean()),
               obj_xy_nonzero=int((np.abs(ob[..., :2]) > 0).any(-1).sum()),
               obj_z_valid_like=int((ob[..., 2] > 0.99).sum()), obj_w_eq_depth=int((np.abs(ob[..., 3] - dep) < 1e-6).sum()),
               compose_accept=int(accept.sum()),
               zw_vs_camonly_med=float(np.median(zw_vs_cam)), moving_px=int(moving.sum()))
    if i - 1 in frames:
        cur, prev = luma(i), luma(i - 1)
        row['moving_resid_resolved'] = residual(cur, prev, mv, c, moving)
        row['moving_resid_objzw'] = residual(cur, prev, zw, c, moving)
        row['moving_resid_zero'] = residual(cur, prev, np.zeros_like(mv), c, moving)
        stat = (~moving) & (dep < 1.0)
        row['static_resid_resolved'] = residual(cur, prev, mv, c, stat)
        row['static_resid_zero'] = residual(cur, prev, np.zeros_like(mv), c, stat)
        # direction: on moving pixels, cosine between the resolved vector and the object vector
        a = mv[moving]; b = zw[moving]
        nm = np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1)
        ok = nm > 1e-6
        row['moving_cos_resolved_vs_obj_med'] = float(np.median((a[ok] * b[ok]).sum(1) / nm[ok])) if ok.any() else float('nan')
        row['moving_err_resolved_vs_obj_med'] = float(np.median(np.linalg.norm(a - b, axis=1))) if len(a) else float('nan')
    rows.append(row)
    if i in (frames[0], frames[len(frames) // 2], frames[-1]):
        img = np.fromfile(os.path.join(d, f'f{i:03d}_input_1146x480_rgba.raw'), np.uint8).reshape(H, W, 4)[..., :3]
        mk = img.copy(); mk[moving] = (mk[moving] * .4 + np.array([255, 0, 0]) * .6).astype(np.uint8)
        top = np.concatenate([img, mk], 1)
        bot = np.concatenate([vis(mv), vis(zw)], 1)
        Image.fromarray(np.concatenate([top, bot], 0)).save(os.path.join(png, f'f{i:03d}_in_moving_resolved_objzw.png'))
import json
for r in rows:
    print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()}))
json.dump(rows, open(os.path.join(png, 'analysis.json'), 'w'), indent=1)
