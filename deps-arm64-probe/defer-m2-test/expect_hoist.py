#!/usr/bin/env python3
"""bbport M2: build the EXPECTED hoist-mode Metal call log from the M1 log.

usage: expect_hoist.py <m1.mtl> <hoist.hoist> > expected.mtl

The only change M2 may make to the global Metal call order is: every hoisted
pre-gfx dispatch (its seq listed in <hoist.hoist>) moves, verbatim (barrier,
dispatch, bindings snapshot), into a hoist compute encoder that is created
and ended immediately before its pass's render encoder, in record order.
Everything else stays where it was. Mechanically:
  1. take each hoisted dispatch triple out of the M1 log;
  2. a compute encoder left with no work is dropped entirely; a compute
     encoder that lost its first work moves its creation line to just
     before its first remaining work line (it is created on first use);
  3. per pass, the triples of the dispatches that the hoist log says were
     hoisted are emitted as one block 'Hk new_compute_encoder hoist cb=..',
     triples (encoder renamed Hk), 'Hk barrier_after_stages', 'Hk
     end_encoding', immediately before that pass's 'new_render_encoder';
  4. compute encoders are renumbered C1.. by creation order.
Plus one schedule change that is not a move of hoisted work (step 0): a
CARRIED compute encoder - created before a pass began (in the M1 log: created
before that pass's render encoder and still open when it is created; an
in-pass M1 encoder is always created after it, by the sync) - is ended by
cs_start_render in hoist mode (BB_KK_DEFER_HOIST, kk_cmd_buffer.c) instead of
at that pass's cs_end: its end lines move to just before the pass's hoist
block / render encoder, and any pre-gfx work the M1 log puts on it after the
render encoder goes to a fresh pre compute encoder created on first use,
ended where the carried one ended in M1.
run.sh then cmp's this against the real hoist-mode log, byte for byte.
"""
import re
import sys

m1 = open(sys.argv[1]).read().splitlines()
hoisted = [int(x) for x in open(sys.argv[2]).read().split()]
hset = set(hoisted)

# 0. carried compute encoders: split at the render encoder they are open across
def _carried(L):
    created, ended = {}, {}
    for i, l in enumerate(L):
        o = l.split(' ', 1)[0]
        if re.match(r'^C\d+$', o) and ' new_compute_encoder' in l:
            created[o] = i
        if re.match(r'^C\d+$', o) and l.endswith(' end_encoding'):
            ended[o] = i
    rcreated = [i for i, l in enumerate(L) if re.match(r'^R\d+ new_render_encoder', l)]
    out = {}
    for o, c in created.items():
        e = ended.get(o)
        across = [r for r in rcreated if c < r < (e if e is not None else len(L))]
        if across:
            out[o] = across[0]
    return out
carried = _carried(m1)
if carried:
    L = list(m1)
    ins = {}  # index of the R line -> lines to put before it
    for o, r in carried.items():
        num = int(o[1:])
        ob = 'C%d' % (num + 1000)
        end_idx = [i for i, l in enumerate(L) if l == o + ' end_encoding'][0]
        bar = L[end_idx - 1]
        assert bar.startswith(o + ' barrier_after_stages '), bar
        ins.setdefault(r, []).extend([bar, o + ' end_encoding'])
        for i in range(r + 1, len(L)):
            if L[i].split(' ', 1)[0] == o:
                L[i] = ob + L[i][len(o):]
        # created on first use (step 2 moves this line there / drops it)
        ins.setdefault(r + 1, []).append(ob + ' new_compute_encoder pre')
    m1 = []
    for i, l in enumerate(L):
        m1.extend(ins.get(i, []))
        m1.append(l)

# 1. locate and cut the hoisted triples
lines = list(m1)
triples = {}
cut = set()
for i, l in enumerate(lines):
    m = re.match(r'^(\S+) dispatch prog=\d+ seq=(\d+)$', l)
    if not m or int(m.group(2)) not in hset:
        continue
    enc, s = m.group(1), int(m.group(2))
    assert lines[i - 1].startswith(enc + ' barrier_after_encoder_stages'), lines[i - 1]
    assert lines[i + 1].startswith(enc + '   bindings'), lines[i + 1]
    triples[s] = (enc, [lines[i - 1], l, lines[i + 1]])
    cut.update((i - 1, i, i + 1))
missing = hset - set(triples)
assert not missing, 'hoisted seq not in the M1 log: %s' % sorted(missing)
rest = [(i, l) for i, l in enumerate(lines) if i not in cut]

# 2. compute encoders: drop empty ones, move creation to first remaining work
def obj(l):
    return l.split(' ', 1)[0]
def is_lifecycle(l):
    rest_ = l.split(' ', 1)[1]
    return (rest_.startswith('new_compute_encoder') or rest_ == 'end_encoding'
            or rest_.startswith('barrier_after_stages'))
work = {}
for _, l in rest:
    o = obj(l)
    if re.match(r'^C\d+$', o) and not is_lifecycle(l):
        work.setdefault(o, 0)
        work[o] += 1
out = []
pending_new = {}
for _, l in rest:
    o = obj(l)
    if re.match(r'^C\d+$', o):
        if not work.get(o):
            continue  # encoder held only hoisted work: gone
        if ' new_compute_encoder' in l:
            pending_new[o] = l
            continue
        if o in pending_new:
            out.append(pending_new.pop(o))
    out.append(l)
assert not pending_new

# 3. hoist blocks, one per pass, before that pass's render encoder. The pass
# of a hoisted dispatch is the first render encoder created after it in M1
# record order... except that in M1 the pass's render encoder already exists
# when its (in-pass) pre dispatch runs: the M1 sync created it. So the pass
# is the render encoder that was open (last created) at the triple's position.
render_at = []  # (m1 index, Rn)
for i, l in enumerate(lines):
    m = re.match(r'^(R\d+) new_render_encoder', l)
    if m:
        render_at.append((i, m.group(1)))
def pass_of(seq):
    idx = next(i for i, l in enumerate(lines)
               if re.match(r'^\S+ dispatch prog=\d+ seq=%d$' % seq, l))
    before = [r for (i, r) in render_at if i < idx]
    return before[-1]
cb_of = {}
for l in lines:
    m = re.match(r'^(R\d+) new_render_encoder cb=(\S+)$', l)
    if m:
        cb_of[m.group(1)] = m.group(2)
blocks = {}
for s in hoisted:  # record order
    blocks.setdefault(pass_of(s), []).append(s)
# a compute encoder's end barrier as M1 logs it (ALL -> ALL)
end_barrier = next(l.split(' ', 2)[2] for l in lines
                   if re.match(r'^C\d+ barrier_after_stages ', l))
final = []
nh = 0
for l in out:
    m = re.match(r'^(R\d+) new_render_encoder', l)
    if m and m.group(1) in blocks:
        nh += 1
        h = 'H%d' % nh
        final.append('%s new_compute_encoder hoist cb=%s' % (h, cb_of[m.group(1)]))
        for s in blocks[m.group(1)]:
            enc, tri = triples[s]
            final.extend(h + t[len(enc):] for t in tri)
        final.append('%s barrier_after_stages %s' % (h, end_barrier))
        final.append('%s end_encoding' % h)
    final.append(l)

# 4. renumber compute encoders by creation order
ren = {}
for l in final:
    m = re.match(r'^(C\d+) new_compute_encoder', l)
    if m:
        ren[m.group(1)] = 'C%d' % (len(ren) + 1)
def rn(l):
    o = obj(l)
    return (ren[o] + l[len(o):]) if o in ren else l
sys.stdout.write(''.join(rn(l) + '\n' for l in final))
