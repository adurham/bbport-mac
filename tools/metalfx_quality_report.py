#!/usr/bin/env python3
"""tools/metalfx_quality_report.py: the MetalFX-vs-FSR numbers and crops, from the dumps.

    # 1. the jitter/camera lines of each capture, one line per dumped frame
    python3 tools/metalfx_quality_report.py --root out/quality --only jitter --tags st mv
    # 2. MetalFX over the same frames (tools/macos/metalfx-spike/build/spike_quality), then
    python3 tools/metalfx_quality_report.py --root out/quality --only report \
        --tags st mv --arm mfx-f1-dr1 [--native nat]

Writes, in <root>/:
  jitter-<tag>.txt                 "<frame> <jx> <jy>" from the port's own 'Dump camera:' lines
  crops/<arm>-<tag>-<region>.png   1:1 crops (moving edge, fine detail) + a whole-frame view
  report.md / report.txt           the tables below

Arms: fsr (the port's own dump-<tag>/fNNN_output_*.raw, FSR 3.1 preset 4 + RCAS) and mfx-<arm>/
(the MetalFX output for the same frames).
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import metalfx_quality as mq  # noqa: E402


def extract_jitter(root, tag):
    """"Dump camera: frame N ... jitter X Y ..." lines -> <root>/jitter-<tag>.txt."""
    log = Path(root) / f"run-{tag}.log"
    if not log.exists():
        print(f"no {log}")
        return None
    out = Path(root) / f"jitter-{tag}.txt"
    rows = []
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"^Dump camera: frame (\d+) .*? jitter (\S+) (\S+) prev_jitter (\S+) (\S+)", line)
        if m:
            rows.append((int(m.group(1)), float(m.group(2)), float(m.group(3))))
    rows.sort()
    out.write_text("".join(f"{i} {x:.9g} {y:.9g}\n" for i, x, y in rows))
    print(f"{out}: {len(rows)} frames" + ("" if rows else "  <-- NO DUMP CAMERA LINES"))
    return out


def mfx_frames(root, arm, tag):
    d = Path(root) / f"mfx-{arm}-{tag}"
    pat = re.compile(r"f(\d+)_out_(\d+)x(\d+)_rgba16f\.raw$")
    out = []
    for p in sorted(d.glob("f*_out_*.raw")):
        m = pat.search(p.name)
        if m:
            w, h = int(m.group(2)), int(m.group(3))
            out.append((int(m.group(1)), mq.read_raw(p, w, h, "rgba16f")))
    out.sort(key=lambda t: t[0])
    return out


def pair_arms(root, tag, arm):
    """[(frame, fsr_rgb, mfx_rgb)] for the frames both arms have."""
    fsr = {f: (rgb, 4) for f, rgb, _p in mq.load_series(root, tag, "output")}
    # Quantise to 8 bits: the port's own upscaler output (ui_image) is R8G8B8A8_UNORM, so this
    # keeps the FSR arm from being judged on a finer grid than the MetalFX one.
    mfx = {f: np.round(np.clip(rgb, 0.0, 1.0) * 255.0) / 255.0 for f, rgb in mfx_frames(root, arm, tag)}
    common = sorted(set(fsr) & set(mfx))
    return [(f, fsr[f][0], mfx[f]) for f in common]


def flicker(series):
    """Mean |luma(N) - luma(N-1)| over consecutive frames of one arm."""
    diffs = []
    prev = None
    for _f, rgb in series:
        a = mq.luma(rgb)
        if prev is not None and prev.shape == a.shape:
            diffs.append(float(np.mean(np.abs(a - prev))))
        prev = a
    return (float(np.mean(diffs)), len(diffs)) if diffs else (0.0, 0)


def bilinear_3x(img, ow, oh):
    """Plain bilinear stretch of the render-size input to the output size: the "no upscaler"
    baseline the two arms have to beat."""
    h, w = img.shape[:2]
    ys = (np.arange(oh) + 0.5) * (h / oh) - 0.5
    xs = (np.arange(ow) + 0.5) * (w / ow) - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, h - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x1 = np.clip(x0 + 1, 0, w - 1)
    wy = np.clip(ys - y0, 0, 1)[:, None, None]
    wx = np.clip(xs - x0, 0, 1)[None, :, None]
    top = img[y0][:, x0] * (1 - wx) + img[y0][:, x1] * wx
    bot = img[y1][:, x0] * (1 - wx) + img[y1][:, x1] * wx
    return top * (1 - wy) + bot * wy


def save_crop(rgb, x, y, w, h, path, reinhard=True):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    crop = rgb[y:y + h, x:x + w]
    if crop.size == 0:
        return
    # Nearest-neighbour 2x, so the pixels the owner judges are the pixels we measured.
    big = np.repeat(np.repeat(crop, 2, axis=0), 2, axis=1)
    mq.to_png(big, path, reinhard=reinhard)


def save_pair(fsr, mfx, x, y, w, h, path, zoom=3):
    """Side by side (FSR | MetalFX) of the same crop, exposed for viewing and zoomed with
    nearest-neighbour: what the eye judges is what the metrics measured."""
    from PIL import Image, ImageDraw
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    crops = [np.clip(rgb[y:y + h, x:x + w], 0.0, None) for rgb in (fsr, mfx)]
    if any(c.size == 0 for c in crops):
        return
    # ONE exposure for both halves: a per-half white point makes the same scene look like two
    # different rooms (and hides exactly the contrast difference a sharpness comparison is about).
    p99 = max(float(np.percentile(c, 99.5)) for c in crops) or 1.0
    parts = [mq.srgb(np.clip(c / p99 * 0.9, 0.0, 1.0)) for c in crops]
    zoomed = [np.repeat(np.repeat(p, zoom, 0), zoom, 1) for p in parts]
    gap = np.ones((zoomed[0].shape[0], 8, 3), np.float32)
    both = np.concatenate([zoomed[0], gap, zoomed[1]], axis=1)
    img = Image.fromarray((both * 255.0 + 0.5).astype(np.uint8))
    draw = ImageDraw.Draw(img)
    for i, text in enumerate(("FSR 3.1 preset 4 + RCAS", "MetalFX temporal")):
        draw.rectangle([i * (zoomed[0].shape[1] + 8), 0, i * (zoomed[0].shape[1] + 8) + 260, 18],
                       fill=(0, 0, 0))
        draw.text((i * (zoomed[0].shape[1] + 8) + 4, 4), text, fill=(255, 255, 0))
    img.save(path)


def save_view(rgb, path, factor=3):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    mq.to_png(rgb[::factor, ::factor], path, reinhard=True)


def avg_metrics(imgs, crop=None):
    """Mean (and spread across frames) of each metric on the same crop of several frames."""
    keys = ("mean_luma", "mean_grad", "grad_per_luma", "hf_ratio", "hf_frac", "grad_p95", "grad_p99")
    per = []
    for img in imgs:
        if crop:
            x, y, w, h = crop
            img = img[y:y + h, x:x + w]
        per.append(mq.describe(img))
    out = {}
    for k in keys:
        v = np.array([d[k] for d in per], float)
        out[k] = float(v.mean())
        out[k + "_sd"] = float(v.std())
    return out


def fmt_row(name, d):
    return (f"| {name} | {d['mean_grad']:.5f} +-{d['mean_grad_sd']:.5f} | {d['grad_per_luma']:.5f} | "
            f"{d['hf_ratio']:.4f} | {d['hf_frac']:.4f} | {d['mtf50']:.3f} | {d['mtf10']:.3f} |")


def ratio(mfx_d, fsr_d, key="mean_grad"):
    """Percent change of the MetalFX value from the FSR value."""
    a = mfx_d.get(key, mfx_d.get("v", 0.0))
    b = fsr_d.get(key, fsr_d.get("v", 0.0))
    return (a / b - 1.0) * 100.0 if b else 0.0


HEADER = ("| arm | mean \\|grad\\| | \\|grad\\| p95 | \\|grad\\| p99 | HF ratio | "
          "HF frac >=0.25cp | T(0.10-0.20) | T(0.20-0.40) |\n|---|---|---|---|---|---|---|---|")


def spectral_cols(crop_fsr, crop_mfx):
    a, b = mq.luma(crop_fsr), mq.luma(crop_mfx)
    return mq.spectral_ratio(a, b, (0.10, 0.20)), mq.spectral_ratio(a, b, (0.20, 0.40))


def report(root, tags, arm, native_tag, conv_arm=None):
    root = Path(root)
    lines = []
    w = lines.append
    w("# MetalFX vs FSR 3.1 preset 4 + RCAS on the port's own frames (3.00x, 1146x480 -> 3440x1440)\n")
    w("Both arms read the SAME dumped frame: the port's scene colour, its depth and the resolved "
      "motion image (BB_DUMP_TRIGGER), and both write 3440x1440. The MetalFX output is quantised to "
      "8 bits (the port's own upscaler output format) first, so neither arm is judged on a finer "
      "grid. Each row is the mean over the last 3 dumped frames with the crop fixed; +- is the "
      "spread across them. T(0.10-0.20) and T(0.20-0.40) are the ratio of the MetalFX radial "
      "spectrum to the FSR one in that band: 1.00 = the same energy at that scale, >1 = MetalFX "
      "keeps more.\n")
    for tag in tags:
        pairs = pair_arms(root, tag, arm)
        if not pairs:
            w(f"\n## {tag}: no paired frames\n")
            continue
        conv = pair_arms(root, tag, conv_arm) if conv_arm else []
        n = len(pairs)
        use = pairs[-3:]
        fsrs = [a for _f, a, _b in use]
        mfxs = [b for _f, _a, b in use]
        w(f"\n## {tag}: {n} frames, metrics over frames {use[0][0]}-{use[-1][0]}\n")

        def table(crop=None, title=""):
            w(("\n**" + title + "**\n") if title else "")
            w(HEADER)
            img = fsrs[-1]
            rows = [("FSR 3.1 + RCAS", avg_metrics(fsrs, crop), img)]
            rows.append(("MetalFX", avg_metrics(mfxs, crop), mfxs[-1]))
            if conv:
                cimgs = [b for _f, _a, b in conv[-3:]]
                rows.append((f"MetalFX converged (x{conv_arm})", avg_metrics(cimgs, crop), cimgs[-1]))
            for name, d, this in rows:
                x0, y0, cw0, ch0 = crop if crop else (0, 0, img.shape[1], img.shape[0])
                if name.startswith("FSR"):
                    t1 = t2 = 1.0
                else:
                    t1, t2 = spectral_cols(img[y0:y0 + ch0, x0:x0 + cw0],
                                           this[y0:y0 + ch0, x0:x0 + cw0])
                w(f"| {name} | {d['mean_grad']:.5f} +-{d['mean_grad_sd']:.5f} | {d['grad_p95']:.4f} | "
                  f"{d['grad_p99']:.4f} | {d['hf_ratio']:.4f} | {d['hf_frac']:.4f} | "
                  f"{t1:.2f} | {t2:.2f} |")
            return rows

        table(None, "whole frame (3440x1440)")
        w(f"\nwhole frame: MetalFX is {ratio(avg_metrics(mfxs), avg_metrics(fsrs)):+.1f}% vs FSR on "
          f"mean |grad|." + (f" Converged: "
          f"{ratio(avg_metrics([b for _f, _a, b in conv[-3:]]), avg_metrics(fsrs)):+.1f}%."
          if conv else "") + "\n")

        motion = mq.load_series(root, tag, "motion")
        cw, ch = 384, 216
        if motion:
            _mf, mvec, _p = motion[len(motion) // 2]
            mx, my = mq.crop_top_motion(mvec, cw // 3, ch // 3)
            ox, oy = mx * 3, my * 3
            mag = np.hypot(mvec[..., 0], mvec[..., 1])
            w(f"**moving-edge crop** (most motion in the sequence): render ({mx},{my}) 128x72 -> "
              f"output ({ox},{oy}) {cw}x{ch}; motion that frame: median {np.median(mag):.2f}, "
              f"p99 {np.percentile(mag, 99):.2f}, max {mag.max():.2f} px/frame\n")
        else:
            ox, oy = mq.crop_top_gradient(mq.luma(fsrs[-1]), cw, ch)
            w("**moving-edge crop**: no motion dump; busiest region\n")
        crop = (ox, oy, cw, ch)
        table(crop)
        w(f"\nedge crop: MetalFX is {ratio(avg_metrics(mfxs, crop), avg_metrics(fsrs, crop)):+.1f}% "
          f"vs FSR on mean |grad|." + (f" Converged: "
          f"{ratio(avg_metrics([b for _f, _a, b in conv[-3:]], crop), avg_metrics(fsrs, crop)):+.1f}%."
          if conv else "") + "\n")
        for name, img in (("fsr", fsrs[-1]), ("mfx", mfxs[-1])):
            save_crop(img, ox, oy, cw, ch, root / "crops" / f"{name}-{tag}-moving.png")
        save_pair(fsrs[-1], mfxs[-1], ox, oy, cw, ch, root / "crops" / f"compare-{tag}-moving.png")

        hf = np.abs(mq.luma(fsrs[-1]) - mq.gaussian_blur(mq.luma(fsrs[-1]), 1.0))
        dx, dy = mq.crop_top_gradient(hf, cw, ch)
        w(f"**fine-detail crop** (busiest small-scale energy): output ({dx},{dy}) {cw}x{ch}\n")
        dcrop = (dx, dy, cw, ch)
        table(dcrop)
        w(f"\ndetail crop: MetalFX is {ratio(avg_metrics(mfxs, dcrop), avg_metrics(fsrs, dcrop)):+.1f}% "
          f"vs FSR on mean |grad|." + (f" Converged: "
          f"{ratio(avg_metrics([b for _f, _a, b in conv[-3:]], dcrop), avg_metrics(fsrs, dcrop)):+.1f}%."
          if conv else "") + "\n")
        for name, img in (("fsr", fsrs[-1]), ("mfx", mfxs[-1])):
            save_crop(img, dx, dy, cw, ch, root / "crops" / f"{name}-{tag}-detail.png")
        save_pair(fsrs[-1], mfxs[-1], dx, dy, cw, ch, root / "crops" / f"compare-{tag}-detail.png")
        for name, img in (("fsr", fsrs[-1]), ("mfx", mfxs[-1])):
            save_view(img, root / "crops" / f"{name}-{tag}-view.png")

        f_a, n_a = flicker([(f, a) for f, a, _b in pairs])
        f_b, _ = flicker([(f, b) for f, _a, b in pairs])
        note = ""
        if conv:
            f_c, _ = flicker([(f, b) for f, _a, b in conv])
            note = (f" Replayed so the history converges ({len(conv) * 8} encodes): {f_c:.3e} "
                    f"({(f_c / f_a - 1) * 100:+.0f}% vs FSR).")
        w(f"**temporal**: mean |delta luma| between consecutive frames over {n_a} pairs -> FSR "
          f"{f_a:.3e}, MetalFX {f_b:.3e} ({(f_b / f_a - 1) * 100:+.0f}% vs FSR).{note}\n")
    text = "\n".join(x for x in lines if x is not None)
    (root / "report.md").write_text(text + "\n")
    (root / "report.txt").write_text(text + "\n")
    print(text)


def alignment_note(name, dx, dy, peak):
    same = "aligned" if abs(dx) <= 1 and abs(dy) <= 1 else "NOT aligned"
    return (f"- pose check vs native, {name}: offset ({dx}, {dy}) px, phase-correlation peak "
            f"{peak:.3f} -> {same}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="out/quality")
    ap.add_argument("--only", choices=["jitter", "report", "both"], default="both")
    ap.add_argument("--tags", nargs="+", default=["st", "mv"])
    ap.add_argument("--arm", default="f1-dr1")
    ap.add_argument("--native", default=None)
    ap.add_argument("--conv-arm", default=None)
    a = ap.parse_args()
    if a.only in ("jitter", "both"):
        for t in a.tags:
            extract_jitter(a.root, t)
    if a.only in ("report", "both"):
        report(a.root, a.tags, a.arm, a.native, a.conv_arm)
