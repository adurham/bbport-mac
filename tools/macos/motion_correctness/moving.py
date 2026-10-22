#!/usr/bin/env python3
"""out/mc/moving.py TAG LAYOUT: object-motion correctness on one capture.
LAYOUT 'shifted' (pre-fix: the vector sits in z,w; validity/depth lost) or 'fixed' (x,y,valid,depth).
Moving-object pixels = the port's own per-vertex history says the surface moves differently
from the camera by > 0.5 px (object vector vs camera-only field from depth + matrices). In the
'shifted' layout validity is unknown, so a pixel counts when its z,w vector is non-zero and
differs from the camera field (still draws there write exactly 0).
Reports: fraction of moving-object pixels whose RESOLVED vector (what FSR reads) is within
0.5 px of the object vector and points the same way (cos > 0.9); warp residual of luma on those
pixels with resolved / object / camera-only / zero vectors (lower = better prediction)."""
import json
import sys

import numpy as np

tag, layout = sys.argv[1], sys.argv[2]
sys.argv = ['x', f'dump-{tag}', f'run-{tag}.log']
exec(open('analyze.py').read().split('frames = sorted')[0])
rows = []
for i in range(1, 10):
    mv = load(i, 'motion_1146x480_rg16f.raw', np.float16, 2)
    ob = load(i, 'objects_1146x480_rgba32f.raw', np.float32, 4)
    dep = load(i, 'depth_1146x480_f32.raw', np.float32, 1)
    c = cams[i]
    co = camera_only(dep.astype(np.float64), c).astype(np.float32)
    if layout == 'fixed':
        vec = ob[..., :2]
        has = (ob[..., 2] > 0.99) & (np.abs(ob[..., 3] - dep) < np.maximum(2e-7, (c['proj'][2] - dep) * .01))
    else:
        vec = ob[..., 2:4]
        has = np.hypot(*vec.transpose(2, 0, 1)) > 1e-3
    diff = np.hypot(*(vec - co).transpose(2, 0, 1))
    moving = has & (diff > 0.5) & (dep < 1)
    a, b = mv[moving], vec[moving]
    err = np.linalg.norm(a - b, axis=1)
    nm = np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1)
    cos = np.where(nm > 1e-9, (a * b).sum(1) / np.maximum(nm, 1e-12), 0)
    good = (err < 0.5) & (cos > 0.9)
    cur, prev = luma(i), luma(i - 1)
    r = lambda v: residual(cur, prev, v, c, moving)
    rows.append(dict(frame=i, has_object_vector=int(has.sum()), moving_px=int(moving.sum()),
                     frac_resolved_correct=float(good.mean()) if moving.any() else None,
                     obj_vs_cam_med_px=float(np.median(diff[moving])) if moving.any() else None,
                     resid_resolved=r(mv), resid_object=r(vec), resid_camera=r(co), resid_zero=r(np.zeros_like(mv))))
for x in rows:
    print(json.dumps({k: (round(v, 4) if isinstance(v, float) else v) for k, v in x.items()}))
keys = [k for k in rows[0] if k != 'frame']
mean = {k: round(float(np.mean([x[k] for x in rows if x[k] is not None])), 4) for k in keys}
print('MEAN', json.dumps(mean))
json.dump(dict(rows=rows, mean=mean), open(f'png-{tag}/moving.json', 'w'), indent=1)
