#!/usr/bin/env python3
"""The ears: record the Mac's microphone through a hidden game run that feeds the real
device, and report the acoustic level profile. If the speaker emits the game's audio the
intro hit shows up tens of dB above the room's noise floor; if the fold-down to the stereo
sink loses it, the recording stays at the floor.
"""
import os, subprocess, sys, time, struct, math

WORK = os.path.expanduser('~/repos/bbport-sound-build')
OUT = os.path.expanduser('~/.hermes/cache/scratch/mic_game.f32')


def run(seconds, with_game):
    env = dict(os.environ)
    env.pop('DYLD_LIBRARY_PATH', None)
    game = None
    if with_game:
        genv = dict(os.environ, BB_AUDIO_DEVICE='1', BB_WARM_SECONDS='120',
                    PATH='/opt/homebrew/bin:' + env.get('PATH', ''))
        log = open('/tmp/mic_game_run.out', 'wb')
        game = subprocess.Popen(['bash', 'tools/macos/sound_probe.sh'], cwd=WORK, env=genv,
                                stdout=log, stderr=subprocess.STDOUT)
        time.sleep(1.0)
    rec = subprocess.run(['/opt/homebrew/bin/ffmpeg', '-hide_banner', '-loglevel', 'error',
                          '-f', 'avfoundation', '-i', ':0', '-t', str(seconds),
                          '-f', 'f32le', '-ac', '1', '-ar', '48000', '-y', OUT],
                         env=env, capture_output=True, text=True, timeout=seconds * 3 + 120)
    if rec.returncode != 0:
        print('ffmpeg failed:', rec.stderr.strip()[:300])
    if game:
        try:
            game.wait(timeout=90)
        except subprocess.TimeoutExpired:
            game.kill()
    return OUT


def report(path):
    d = open(path, 'rb').read()
    n = len(d) // 4
    v = struct.unpack('<%df' % n, d[:n * 4]) if n else ()
    if not v:
        print('no samples'); return
    step = 24000  # 0.5 s
    prof = []
    for i in range(0, len(v) - step + 1, step):
        seg = v[i:i + step]
        prof.append(math.sqrt(sum(x * x for x in seg) / len(seg)))
    floor = sorted(prof)[len(prof) // 5] if prof else 0
    print('profile (0.5 s RMS, dBFS), floor(20th pct)=%.1f dBFS' % (20 * math.log10(floor or 1e-30)))
    line = ''
    for k, p in enumerate(prof):
        line += '%.0f ' % (20 * math.log10(p or 1e-30))
        if (k + 1) % 10 == 0:
            print('  %4.1f-%4.1fs: %s' % ((k + 1) * 0.5 - 5, (k + 1) * 0.5, line)); line = ''
    if line:
        print('  tail: %s' % line)
    peak = max(abs(x) for x in v)
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    print('peak %.6f (%.2f dBFS)  rms %.6f (%.2f dBFS)' % (peak, 20 * math.log10(peak or 1e-30), rms, 20 * math.log10(rms or 1e-30)))


if __name__ == '__main__':
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 45
    with_game = '--game' in sys.argv
    if not with_game:
        print('=== baseline (no game) ===')
    else:
        print('=== game run, hidden window, real device ===')
    report(run(secs, with_game))
