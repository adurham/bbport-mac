#!/usr/bin/env python3
"""tools/metalfx_quality.py: the offline MetalFX-vs-FSR sharpness comparison on REAL frames.

Inputs are the port's own BB_DUMP_TRIGGER dumps (tools/macos/metalfx-quality-capture.sh) and the
MetalFX output for the same frames (tools/macos/metalfx-spike/spike_quality):

  out/quality/dump-<tag>/fNNN_output_WxH_rgba16f.raw   FSR 3.1 preset 4 + RCAS, display res
  out/quality/mfx-<arm>-<tag>/fNNN_out_WxH_rgba16f.raw MetalFX temporal, same frames, same res
  out/quality/dump-nat/fNNN_*                          upscaler off: the native 3440x1440 render
  out/quality/cap-<tag>-N.png                          the port's presented frame (what is on screen)

  python3 tools/metalfx_quality.py [--root out/quality] [--arms ...]

Metrics, all on identical crops in the same linear-light float space:
  mean |grad|          mean central-difference gradient magnitude of luma (per pixel)
  grad/luma            the same, divided by the crop's mean luma: exposure-independent edge energy
  HF ratio             variance of (image - 3x3 gaussian blur) / variance of the image
  MTF10/MTF50          radial power spectrum of a 256x256 window: the spatial frequency (cycles per
                       pixel) where the spectrum falls to 10% / 50% of its DC-normalised peak
  flicker              static view: mean |luma(frame N) - luma(frame N-1)| on the frame (lower is
                       more stable; a converged temporal filter should sit near the input's noise)
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

LW = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


# ---------------------------------------------------------------- dump readers

def read_raw(path, w, h, fmt):
    data = Path(path).read_bytes()
    if fmt in ("rgba16f",):
        a = np.frombuffer(data, np.float16)[: w * h * 4].reshape(h, w, 4).astype(np.float32)
        return np.nan_to_num(a[..., :3], nan=0.0, posinf=0.0, neginf=0.0)
    if fmt == "rgba32f":
        a = np.frombuffer(data, np.float32)[: w * h * 4].reshape(h, w, 4)
        return np.nan_to_num(a[..., :3], nan=0.0, posinf=0.0, neginf=0.0)
    if fmt == "r11g11b10f":
        packed = np.frombuffer(data, np.uint32)[: w * h].reshape(h, w)

        def small(bits, mb):
            e = (bits >> mb) & 31
            m = (bits & ((1 << mb) - 1)).astype(np.float32)
            sc = float(1 << mb)
            n = np.ldexp(1.0 + m / sc, e.astype(np.int32) - 15)
            d = np.ldexp(m / sc, -14)
            return np.where(e == 0, d, np.where(e == 31, 0.0, n)).astype(np.float32)

        rgb = np.stack([small(packed & 0x7FF, 6), small((packed >> 11) & 0x7FF, 6),
                        small(packed >> 22, 5)], axis=-1)
        return np.nan_to_num(rgb, nan=0.0, posinf=0.0, neginf=0.0)
    if fmt == "rg16f":
        a = np.frombuffer(data, np.float16)[: w * h * 2].reshape(h, w, 2).astype(np.float32)
        return np.nan_to_num(a, nan=0.0, posinf=1e6, neginf=-1e6)
    if fmt in ("rgba", "bgra"):
        a = np.frombuffer(data, np.uint8)[: w * h * 4].reshape(h, w, 4)[..., :3].astype(np.float32)
        if fmt == "bgra":
            a = a[..., ::-1]
        return a / 255.0
    raise ValueError(fmt)


def find_dump(root, tag, name):
    """<root>/dump-<tag>/fNNN_<name>_WxH_fmt.raw, any frame index, sorted by frame."""
    pat = re.compile(rf"f(\d+)_{name}_(\d+)x(\d+)_(\w+)\.raw$")
    found = []
    for p in sorted((Path(root) / f"dump-{tag}").glob("f*_*.raw")):
        m = pat.search(p.name)
        if m:
            found.append((int(m.group(1)), p, int(m.group(2)), int(m.group(3)), m.group(4)))
    found.sort()
    return found


def load_series(root, tag, name, limit=None):
    out = []
    for frame, path, w, h, fmt in find_dump(root, tag, name):
        out.append((frame, read_raw(path, w, h, fmt), path))
        if limit and len(out) >= limit:
            break
    return out


def srgb(linear):
    linear = np.clip(np.asarray(linear, np.float32), 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92,
                    1.055 * np.power(linear, 1 / 2.4) - 0.055)


def to_png(rgb, path, reinhard=True):
    """HDR linear -> viewable sRGB PNG (the same mapping as tools/dump_view.py)."""
    if reinhard:
        rgb = rgb / (1.0 + rgb)
    Image.fromarray((srgb(rgb) * 255.0 + 0.5).astype(np.uint8)).save(path)


# ---------------------------------------------------------------- metrics

def luma(rgb):
    # Clip fireflies so one specular pixel cannot dominate a mean.
    with np.errstate(all="ignore"):
        lum = np.asarray(rgb, dtype=np.float32) @ LW
    lum = np.nan_to_num(lum, nan=0.0, posinf=0.0, neginf=0.0)
    hi = np.percentile(lum, 99.9) if lum.size else 1.0
    return np.clip(lum, 0.0, max(hi, 1e-6))


def gaussian_blur(a, sigma=1.0):
    r = max(1, int(round(3 * sigma)))
    x = np.arange(-r, r + 1, dtype=np.float32)
    k = np.exp(-(x ** 2) / (2 * sigma ** 2))
    k /= k.sum()
    p = np.pad(a, r, mode="edge")
    tmp = sum(k[i] * p[:, i:i + a.shape[1]] for i in range(2 * r + 1))
    return sum(k[i] * tmp[i:i + a.shape[0], :] for i in range(2 * r + 1))


def mean_grad(a):
    dx = (a[1:-1, 2:] - a[1:-1, :-2]) * 0.5
    dy = (a[2:, 1:-1] - a[:-2, 1:-1]) * 0.5
    return float(np.mean(np.hypot(dx, dy)))


def grad_percentiles(a, qs=(95, 99)):
    dx = (a[1:-1, 2:] - a[1:-1, :-2]) * 0.5
    dy = (a[2:, 1:-1] - a[:-2, 1:-1]) * 0.5
    g = np.hypot(dx, dy).ravel()
    return [float(np.percentile(g, q)) for q in qs]


def hf_ratio(a):
    hp = a - gaussian_blur(a, 1.0)
    v = float(np.var(a))
    return float(np.var(hp) / v) if v > 1e-12 else 0.0


def mtf_like(a, size=256):
    """Radial power spectrum of the centre `size` window: MTF50/MTF10 in cycles/pixel."""
    h, w = a.shape
    s = min(size, h, w)
    y0, x0 = (h - s) // 2, (w - s) // 2
    win = a[y0:y0 + s, x0:x0 + s]
    win = win - win.mean()
    win = win * np.outer(np.hanning(s), np.hanning(s))
    spec = np.abs(np.fft.rfft2(win)) ** 2
    fy = np.fft.fftfreq(s)[:, None]
    fx = np.fft.rfftfreq(s)[None, :]
    f = np.sqrt(fx ** 2 + fy ** 2)
    # Radial average in 0.005 cycles/px bins, DC bin dropped.
    bins = np.arange(0.0, 0.52, 0.005)
    idx = np.clip(np.digitize(f, bins) - 1, 0, len(bins) - 2)
    tot = np.bincount(idx.ravel(), weights=spec.ravel(), minlength=len(bins) - 1)
    cnt = np.bincount(idx.ravel(), minlength=len(bins) - 1)
    prof = (tot / np.maximum(cnt, 1))[1:]  # drop the DC-containing bin
    fbins = bins[1:]
    if prof.max() <= 0:
        return 0.0, 0.0, prof, fbins
    prof = prof / prof.max()
    def cross(level):
        below = np.where(prof < level)[0]
        return float(fbins[below[0]]) if below.size else float(fbins[-1])
    return cross(0.5), cross(0.1), prof, fbins


def hf_energy_frac(a, threshold=0.25, size=256):
    """Fraction of the window's spectral energy above `threshold` cycles/pixel."""
    h, w = a.shape
    s = min(size, h, w)
    win = a[(h - s) // 2:(h - s) // 2 + s, (w - s) // 2:(w - s) // 2 + s]
    win = win - win.mean()
    spec = np.abs(np.fft.rfft2(win * np.outer(np.hanning(s), np.hanning(s)))) ** 2
    fy = np.fft.fftfreq(s)[:, None]
    fx = np.fft.rfftfreq(s)[None, :]
    f = np.sqrt(fx ** 2 + fy ** 2)
    total = spec.sum()
    return float(spec[f >= threshold].sum() / total) if total > 0 else 0.0


def radial_spectrum(a, size=256):
    """Radial-average power spectrum of the centre `size` window (Hann), and its bin centres."""
    h, w = a.shape
    s = min(size, h, w)
    win = a[(h - s) // 2:(h - s) // 2 + s, (w - s) // 2:(w - s) // 2 + s]
    win = (win - win.mean()) * np.outer(np.hanning(s), np.hanning(s))
    spec = np.abs(np.fft.rfft2(win)) ** 2
    fy = np.fft.fftfreq(s)[:, None]
    fx = np.fft.rfftfreq(s)[None, :]
    f = np.sqrt(fx ** 2 + fy ** 2)
    bins = np.arange(0.0, 0.52, 0.005)
    idx = np.clip(np.digitize(f, bins) - 1, 0, len(bins) - 2)
    tot = np.bincount(idx.ravel(), weights=spec.ravel(), minlength=len(bins) - 1)
    cnt = np.bincount(idx.ravel(), minlength=len(bins) - 1)
    centres = bins[:-1] + 0.0025
    return (tot / np.maximum(cnt, 1))[1:], centres[1:]  # drop the DC-containing first bin


def spectral_ratio(a, b, band=(0.10, 0.20)):
    """Mean of P_b(f) / P_a(f) over a frequency band, on the SAME crop: a relative transfer
    estimate (1.0 = the same energy at that scale, >1 = b keeps more). Absolute MTF of a single
    image is dominated by the content's own 1/f spectrum, so the ratio of two renders of the same
    frame is the honest measure."""
    pa, fb = radial_spectrum(a)
    pb, _ = radial_spectrum(b)
    sel = (fb >= band[0]) & (fb < band[1]) & (pa > 0)
    if not np.any(sel):
        return float("nan")
    return float(np.mean(pb[sel] / pa[sel]))


def describe(rgb, tag=""):
    a = luma(rgb)
    g = mean_grad(a)
    m = float(a.mean())
    m50, m10, _, _ = mtf_like(a)
    p95, p99 = grad_percentiles(a)
    return {
        "name": tag,
        "grad_p95": p95,
        "grad_p99": p99,
        "mean_luma": m,
        "mean_grad": g,
        "grad_per_luma": g / m if m > 1e-9 else 0.0,
        "hf_ratio": hf_ratio(a),
        "hf_frac": hf_energy_frac(a),
        "mtf50": m50,
        "mtf10": m10,
    }


def align_offset(ref, other, radius=24):
    """Phase correlation offset (x, y) of `other` relative to `ref` on luma, for the cross-run
    native comparison: the runs are separate, so the pose must be checked before pixels are
    compared."""
    a = ref - ref.mean()
    b = other - other.mean()
    A = np.fft.rfft2(a)
    B = np.fft.rfft2(b)
    cross = A * np.conj(B)
    cross /= np.abs(cross) + 1e-9
    cc = np.fft.irfft2(cross, s=a.shape)
    peak = np.unravel_index(np.argmax(cc), cc.shape)
    dy, dx = peak
    if dy > a.shape[0] // 2:
        dy -= a.shape[0]
    if dx > a.shape[1] // 2:
        dx -= a.shape[1]
    return int(dx), int(dy), float(cc.max())


def crop_top_gradient(a, cw, ch, exclude_center=False):
    """The cw x ch window with the most gradient energy (integral image), for a fair 'busiest
    region' crop that is identical for every arm."""
    e = np.hypot(*(np.gradient(a)))
    ii = e.cumsum(0).cumsum(1)
    ii = np.pad(ii, ((1, 0), (1, 0)))
    h, w = a.shape
    if h < ch or w < cw:
        return 0, 0
    win = ii[ch:, cw:] - ii[:-ch, cw:] - ii[ch:, :-cw] + ii[:-ch, :-cw]
    if exclude_center:
        win[h // 3:2 * h // 3, w // 3:2 * w // 3] = -1
    y, x = np.unravel_index(np.argmax(win), win.shape)
    return int(x), int(y)


def crop_top_motion(motion, cw, ch):
    """The cw x ch window (render res) with the most motion magnitude."""
    mag = np.hypot(motion[..., 0], motion[..., 1])
    return crop_top_gradient(mag, cw, ch)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="out/quality")
    args = ap.parse_args()
    print("this file is a library; the driver is tools/metalfx_quality_report.py")
