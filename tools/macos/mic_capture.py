#!/usr/bin/env python3
"""Record the Mac's own microphone and report peak/RMS of what the speaker emitted.
Uses ffmpeg (brew, clean dyld) as a subprocess; no shell backgrounding.
  mic_capture.py <out.f32> <seconds> [play_wav ...]
"""
import os, struct, subprocess, sys, math, time

def capture(path, seconds, plays=()):
    env = dict(os.environ)
    env.pop('DYLD_LIBRARY_PATH', None)
    procs = []
    for w in plays:
        procs.append(subprocess.Popen(['/usr/bin/afplay', w], env=env))
    rec = subprocess.run(
        ['/opt/homebrew/bin/ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'avfoundation',
         '-i', ':0', '-t', str(seconds), '-f', 'f32le', '-ac', '1', '-ar', '48000', '-y', path],
        env=env, capture_output=True, text=True, timeout=seconds * 4 + 60)
    for p in procs:
        try: p.wait(timeout=5)
        except Exception: p.kill()
    if rec.returncode != 0:
        print('ffmpeg failed:', rec.stderr.strip()[:500])

def report(path):
    d = open(path, 'rb').read()
    n = len(d) // 4
    v = struct.unpack('<%df' % n, d[:n * 4]) if n else ()
    if not v:
        print('no samples')
        return
    peak = max(abs(x) for x in v)
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    # noise floor: 5th percentile of 20 ms windows
    step = 960
    wins = [math.sqrt(sum(v[i:i + step][j] ** 2 for j in range(len(v[i:i + step]))) / max(1, len(v[i:i + step])))
            for i in range(0, len(v) - step + 1, step)]
    wins.sort()
    floor = wins[len(wins) // 20] if wins else 0.0
    print('%-28s %.2fs peak=%.6f (%6.2f dBFS)  rms=%.6f (%6.2f dBFS)  noise floor=%6.2f dBFS' % (
        os.path.basename(path), len(v) / 48000.0, peak, 20 * math.log10(peak or 1e-30),
        rms, 20 * math.log10(rms or 1e-30), 20 * math.log10(floor or 1e-30)))

if __name__ == '__main__':
    out = sys.argv[1]; secs = float(sys.argv[2]); plays = sys.argv[3:]
    capture(out, secs, plays)
    report(out)
