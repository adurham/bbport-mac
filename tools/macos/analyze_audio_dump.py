#!/usr/bin/env python3
"""Analyse BB_AUDIO_DUMP files: headerless PCM, one per audio port.

Name: <base>.port<ID>.<ch>ch.<f32|s16>. Content is post-gain, post-remap PCM, exactly
what runtime_audio.c hands to SDL_PutAudioStreamData.
"""
import glob, os, struct, sys

def analyse(path):
    name = os.path.basename(path)
    parts = name.split('.')
    ch = int(parts[-2][:-2] if parts[-2].endswith('ch') else parts[-2].replace('ch', ''))
    fmt = parts[-1]
    sw = 4 if fmt == 'f32' else 2
    data = open(path, 'rb').read()
    frames = len(data) // (ch * sw)
    fmtc = ('<%df' % (ch * frames)) if fmt == 'f32' else ('<%dh' % (ch * frames))
    vals = struct.unpack(fmtc, data[:frames * ch * sw])
    # per-channel stats
    stats = []
    for c in range(ch):
        col = vals[c::ch]
        peak = max(abs(v) for v in col) if col else 0
        nz = sum(1 for v in col if v != 0)
        s = sum(float(v) * float(v) for v in col)
        rms = (s / len(col)) ** 0.5 if col else 0.0
        stats.append((peak, rms, nz, len(col)))
    scale = 1.0 if fmt == 'f32' else 32768.0
    allz = all(abs(v) == 0 for v in vals)
    return dict(name=name, ch=ch, fmt=fmt, frames=frames, nbytes=len(data),
                all_zero=allz, stats=stats, scale=scale,
                global_peak=max(abs(v) for v in vals) if vals else 0,
                global_rms=(sum(float(v) * float(v) for v in vals) / len(vals)) ** 0.5 if vals else 0.0)

def db(x, scale):
    x = float(x) / scale
    if x <= 0:
        return float('-inf')
    import math
    return 20 * math.log10(x)

def main(base):
    files = sorted(glob.glob(base + '.port*'))
    if not files:
        print('NO DUMP FILES for base', base)
        return 1
    print('%-42s %8s %8s %9s %9s %8s %6s' % ('file', 'frames', 'secs', 'peak', 'rms', 'dBFS', 'zeros%'))
    for f in files:
        r = analyse(f)
        secs = r['frames'] / 48000.0
        zpc = 100.0 * (1 - sum(s[2] for s in r['stats']) / max(1, sum(s[3] for s in r['stats'])))
        print('%-42s %8d %8.2f %9.6f %9.6f %8.2f %6.1f' % (
            r['name'], r['frames'], secs, r['global_peak'] / r['scale'],
            r['global_rms'] / r['scale'], db(r['global_rms'], r['scale']), zpc))
        for c, (peak, rms, nz, n) in enumerate(r['stats']):
            print('    ch%d: peak=%.6f (%.2f dBFS) rms=%.6f nonzero=%d/%d' % (
                c, peak / r['scale'], db(peak, r['scale']), rms / r['scale'], nz, n))
        print('    all_zero=%s' % r['all_zero'])
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'out/audio-dump'))
