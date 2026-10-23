#!/usr/bin/env python3
"""bbport M2: heap stream checks (renc_hoist_test heap ...).

usage: heap_check.py <hoist-prefix> <m1-prefix> [--control-reset-late]

Inputs per prefix: <p>.mtl (Metal calls, global CPU order), <p>.heap (one
line per heap reset: 'seq draw pre|post bottom'); <hoist>.counters (per
draw: hoisted, HEAP-refusal counter delta).

GPU execution order of an op = (end line of its encoder, its own line): an
encoder's work executes as a unit, and encoders execute in the order they
are ended (on the shared buffer only one encoder is open at a time; the M1
pre/post command buffers are appended at cs_end in pre, gfx, post order,
which is also their end order).

Hoisted draws (P1 tessellation, P2 unroll) must show, in execution order,
  heap reset (write of the heap bottom) -> every tess/unroll dispatch of the
  draw -> the pass's render encoder,
all on ONE hoist encoder (Hn) that ends immediately before the pass's
render encoder is created; HEAP counter +0 for the draw.
Refused draws (P3: BARRIER; P4: pre_gfx busy, HEAP reported) keep the M1
placement: the reset on an in-pass POST compute encoder created after the
pass's render encoder, as in the M1 log.

--control-reset-late (negative control): run the hoisted-draw ordering
check on the M1 log, i.e. on the post_gfx placement a hoisted draw must not
use. It must FAIL.
"""
import re
import sys

hp, mp = sys.argv[1], sys.argv[2]
control = '--control-reset-late' in sys.argv[3:]


class Log:
    def __init__(self, p):
        self.L = open(p + '.mtl').read().splitlines()
        self.heap = [l.split() for l in open(p + '.heap').read().splitlines()]
        self.disp, self.created, self.ended, self.kind = {}, {}, {}, {}
        for i, l in enumerate(self.L):
            o = l.split(' ', 1)[0]
            m = re.match(r'^\S+ dispatch prog=(\d+) seq=(\d+)$', l)
            if m:
                self.disp[int(m.group(2))] = (o, i, int(m.group(1)))
            if ' new_compute_encoder' in l or ' new_render_encoder' in l:
                self.created[o] = i
                self.kind[o] = l.split(' ', 1)[1]
            if l.endswith(' end_encoding'):
                self.ended[o] = i

    def key(self, s):
        enc, line, _ = self.disp[s]
        return (self.ended[enc], line)

    def render_of(self, s):
        """the pass's render encoder: the first one created after the draw's
        last pre dispatch's encoder ends, or the one open at the dispatch"""
        enc, line, _ = self.disp[s]
        after = [o for o, i in self.created.items()
                 if o.startswith('R') and i > self.ended[enc]]
        opened = [o for o, i in self.created.items()
                  if o.startswith('R') and i < line and self.ended[o] > line]
        cands = opened or after
        return min(cands, key=lambda o: self.created[o])


H, M = Log(hp), Log(mp)
counters = {}
for l in open(hp + '.counters').read().splitlines():
    m = re.match(r'^draw (\S+)\s+hoisted=(\d) heap_refusals\+=(\d+)$', l)
    if m:
        counters[m.group(1)] = (int(m.group(2)), int(m.group(3)))
want = {'P1-tess': (1, 0), 'P2-unroll': (1, 0),
        # P3 is refused (in-pass barrier) and is the recording's first heap user:
        # the counter now means "refused AND first heap use applied", so it is 1.
        'P3-unroll-refused': (0, 1), 'P4-tess-busy': (0, 1)}
ok = True
if not control:
    print('per-draw (hoisted, HEAP refusals added): %s -> %s' % (
        counters, 'ok' if counters == want else 'FAIL, expected %s' % want))
    ok &= counters == want

# the draw's own heap-using dispatches: from its reset up to the next reset
# (the hoisted draws P1, P2 have nothing else in between)
seqs = [int(r[0]) for r in H.heap]
nexts = seqs[1:] + [max(H.disp) + 1]


def check_hoisted(G, s, users, name, bottom):
    steps, good = [], True
    renc, rline, _ = G.disp[s]
    steps.append('reset seq=%d write=%s on %s' % (s, bottom, renc))
    for q in users:
        qe, _, qp = G.disp[q]
        after = G.key(s) < G.key(q)
        steps.append('seq=%d prog=%d on %s%s' % (q, qp, qe, '' if after else ' (BEFORE THE RESET)'))
        good &= after and qe == renc
    r = G.render_of(users[-1])
    reset_first = G.ended[renc] < G.created[r]
    nxt = G.L[G.ended[renc] + 1] if G.ended[renc] + 1 < len(G.L) else ''
    adjacent = nxt.startswith(r + ' new_render_encoder')
    steps.append('%s new_render_encoder%s' % (r, '' if reset_first else ' (RESET ENDS AFTER IT)'))
    good &= reset_first and adjacent and renc.startswith('H')
    print('  %s %s: %s -> %s' % ('ok' if good else 'FAIL', name, ' -> '.join(steps),
                                 'hoist encoder ends right before it' if adjacent else 'not adjacent'))
    return good


for (s, name, place, bottom), nxt in zip(H.heap, nexts):
    s = int(s)
    users = [q for q in range(s + 1, nxt) if q in H.disp]
    if want[name][0] == 1:
        if control:
            ok &= check_hoisted(M, s, users, name + ' [M1 post_gfx placement]', bottom)
        else:
            ok &= place == 'pre' and check_hoisted(H, s, users, name, bottom)
    elif not control:
        enc, line, _ = H.disp[s]
        r = H.render_of(s)
        menc = M.disp[s][0]
        mplace = [x for x in M.heap if int(x[0]) == s][0][2]
        good = (place == 'post' and mplace == 'post' and
                H.kind[enc] == 'new_compute_encoder post' and
                H.created[r] < H.created[enc] and H.kind[enc] == M.kind[menc])
        print('  %s %s (not hoisted): reset seq=%d write=%s on %s (%s), created after %s; M1: %s (%s)' % (
            'ok' if good else 'FAIL', name, s, bottom, enc, H.kind[enc], r, menc, M.kind[menc]))
        ok &= good
print('heap stream: %s' % ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
