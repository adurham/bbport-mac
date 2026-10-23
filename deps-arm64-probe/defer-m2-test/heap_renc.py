#!/usr/bin/env python3
"""bbport M2: the recorded op list (BB_KK_RENC_LOG) of M1 vs the hoist mode.

usage: heap_renc.py <m1.renc> <hoist.renc> <m1.heap> <hoist.heap>

The two op lists must be identical except for one thing: the heap reset of a
draw that passed the hoist gate (kk_heap) is recorded as a PRE side op
instead of the M1 in-pass POST side op. Every differing line must be exactly
that: 'side post' -> 'side pre', same program, same flags, no reads, the
same single write (the heap's bottom field), and the number of such lines
must equal the number of resets the hoist log places 'pre' where M1 placed
them 'post'.
"""
import re
import sys

a = open(sys.argv[1]).read().splitlines()
b = open(sys.argv[2]).read().splitlines()
ha = [l.split() for l in open(sys.argv[3]).read().splitlines()]
hb = [l.split() for l in open(sys.argv[4]).read().splitlines()]
ok = True
if len(a) != len(b):
    print('line count differs: %d vs %d (FAIL)' % (len(a), len(b)))
    sys.exit(1)
bottoms = {r[3] for r in hb}
moved = [(x, y) for x, y in zip(ha, hb) if x[2] == 'post' and y[2] == 'pre']
other = [(x, y) for x, y in zip(ha, hb) if x[2] != y[2] and (x, y) not in moved]
if other or len(ha) != len(hb):
    print('reset placement changed other than post -> pre: %s (FAIL)' % other)
    ok = False
pat = re.compile(r'^(\S+) side (pre|post) prog=(\d+) flags=(0x[0-9a-f]+) reads=(\S*) writes=(\S*)$')
diffs = 0
for i, (x, y) in enumerate(zip(a, b)):
    if x == y:
        continue
    diffs += 1
    mx, my = pat.match(x), pat.match(y)
    good = (mx and my and mx.group(2) == 'post' and my.group(2) == 'pre' and
            mx.group(1) == my.group(1) and mx.group(3) == my.group(3) and
            mx.group(4) == my.group(4) and mx.group(5) == '' and
            my.group(5) == '' and mx.group(6) == my.group(6) and
            my.group(6) in bottoms)
    verdict = ('heap reset post -> pre, write=%s, no reads: ok' % my.group(6)) if good else 'FAIL'
    print('line %d: %s\n      -> %s : %s' % (i + 1, x, y, verdict))
    ok &= bool(good)
if diffs != len(moved):
    print('%d differing lines, %d resets moved post -> pre (FAIL)' % (diffs, len(moved)))
    ok = False
print('op list: %s (%d line(s) differ, all hoisted heap resets; everything else byte-identical)'
      % ('PASS' if ok else 'FAIL', diffs))
sys.exit(0 if ok else 1)
