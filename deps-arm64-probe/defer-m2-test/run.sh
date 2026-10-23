#!/bin/bash
# bbport M2 offline test: pre-gfx hoist (BB_KK_DEFER_HOIST), no GPU/Metal/game.
# Same build as defer-m1-test: the driver's exact compile flags, linked
# against the REAL kk_render_defer.c.o of the probe build + libmesa_util.
# Runs one synthetic op stream in three modes:
#   direct    (flag-set M0: direct backend)
#   deferred  (BB_KK_DEFER_RENDER=1, hoist unset: M1)
#   hoist     (BB_KK_DEFER_RENDER=1 BB_KK_ONE_CMDBUF=1 BB_KK_DEFER_HOIST=1: M2)
# and asserts:
#   1. M1 still holds on this stream: direct vs deferred, renc + mtl, raw cmp;
#   2. hoist vs deferred renc log (the recorded op list): raw cmp IDENTICAL;
#   3. hoist mtl log == the M1 mtl log transformed by expect_hoist.py (each
#      hoisted pre-gfx dispatch moved verbatim into a hoist compute encoder
#      created+ended immediately before its pass's render encoder, record
#      order kept, nothing else moved): raw cmp IDENTICAL;
#   4. structural: every hoist block is immediately followed by its pass's
#      new_render_encoder, and every hoisted seq precedes all of that pass's
#      render-encoder ops;
#   5. refusal-predicate unit table passes;
#   6. negative controls: a broken hoist (render encoder created before the
#      hoisted dispatch) and a perturbed binding must both FAIL check 3.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MESA=/Users/adam.durham/repos/bbport-mac-native/deps-arm64/src/mesa-kosmickrisp
BT=$MESA/build-arm64/externals/mesa/build-target
OUT=${OUT:-$HERE/out}
mkdir -p "$OUT"

FLAGS=$(python3 - "$BT" <<'EOF'
import json, shlex, sys
bt = sys.argv[1]
for e in json.load(open(bt + '/compile_commands.json')):
    if e['file'].endswith('vulkan/kk_render_defer.c'):
        a = shlex.split(e['command'])
        keep = []
        skip = False
        for i, x in enumerate(a[1:]):
            if skip: skip = False; continue
            if x in ('-o', '-MQ', '-MF', '-c'): skip = True; continue
            if x == '-MD': continue
            keep.append(x)
        print(' '.join(shlex.quote(k) for k in keep))
        break
EOF
)
[ -n "$FLAGS" ] || { echo "no compile command for kk_render_defer.c"; exit 2; }
OBJ=$BT/src/kosmickrisp/vulkan/libkk.a.p/kk_render_defer.c.o
[ -f "$OBJ" ] || { echo "missing $OBJ (build the driver first)"; exit 2; }

cd "$BT" || exit 2
eval clang $FLAGS -Wno-missing-prototypes -c "$HERE/renc_hoist_test.c" -o "$OUT/renc_hoist_test.o" || exit 2
clang -target aarch64-apple-darwin "$OUT/renc_hoist_test.o" "$OBJ" \
   "$BT/src/util/libmesa_util.a" "$BT/src/c11/impl/libmesa_util_c11.a" -lz -lm \
   -o "$OUT/renc_hoist_test" || exit 2
T=$OUT/renc_hoist_test
X="python3 $HERE/expect_hoist.py"

"$T" direct   "$OUT/direct"   >/dev/null || exit 2
"$T" deferred "$OUT/deferred" >/dev/null || exit 2
"$T" hoist    "$OUT/hoist"    | tee "$OUT/hoist.counters" || exit 2
"$T" hoist    "$OUT/porder" perturb-order >/dev/null || exit 2
"$T" hoist    "$OUT/pbind"  perturb-bind  >/dev/null || exit 2
rc=0
same() { if cmp "$1" "$2"; then echo "IDENTICAL"; else diff "$1" "$2"; echo "DIFFERENT (FAIL)"; rc=1; fi; }

echo "== 1. M1 on this stream: direct vs deferred"
echo -n "renc: "; same "$OUT/direct.renc" "$OUT/deferred.renc"
echo -n "mtl (global order): "; same "$OUT/direct.mtl" "$OUT/deferred.mtl"
echo "== 2. recorded op list: deferred (M1) vs hoist (M2) renc log ($(wc -l < "$OUT/hoist.renc" | tr -d ' ') lines)"
echo "   (the only allowed difference: a hoisted draw's heap reset node, side post -> side pre, same write, no reads)"
python3 "$HERE/heap_renc.py" "$OUT/deferred.renc" "$OUT/hoist.renc" "$OUT/deferred.heap" "$OUT/hoist.heap" || rc=1
echo "== hoisted dispatch seqs (record order): $(tr '\n' ' ' < "$OUT/hoist.hoist")"
[ -s "$OUT/hoist.hoist" ] || { echo "nothing hoisted (FAIL)"; rc=1; }
echo "== 3. the asserted difference: M1 mtl vs hoist mtl ($(wc -l < "$OUT/deferred.mtl" | tr -d ' ') / $(wc -l < "$OUT/hoist.mtl" | tr -d ' ') lines)"
diff "$OUT/deferred.mtl" "$OUT/hoist.mtl" > "$OUT/m1-vs-hoist.diff"
cat "$OUT/m1-vs-hoist.diff"
$X "$OUT/deferred.mtl" "$OUT/hoist.hoist" > "$OUT/expected.mtl" || { echo "expect_hoist.py failed"; exit 2; }
echo -n "hoist mtl vs expected (M1 with exactly the hoisted dispatches moved ahead of their render encoder), raw cmp: "
same "$OUT/expected.mtl" "$OUT/hoist.mtl"
echo "== 4. structure"
python3 - "$OUT/hoist.mtl" "$OUT/hoist.hoist" <<'EOF' || rc=1
import re, sys
L = open(sys.argv[1]).read().splitlines()
hs = [int(x) for x in open(sys.argv[2]).read().split()]
ok = True
for i, l in enumerate(L):
    m = re.match(r'^(H\d+) end_encoding$', l)
    if m:
        nxt = L[i + 1] if i + 1 < len(L) else ''
        r = re.match(r'^(R\d+) new_render_encoder', nxt)
        print('%s closes at line %d, next: %s -> %s' % (m.group(1), i + 1, nxt, 'ok' if r else 'FAIL'))
        ok &= bool(r)
for s in hs:
    i = next(k for k, l in enumerate(L) if re.search(r' dispatch prog=\d+ seq=%d$' % s, l))
    enc = L[i].split()[0]
    r = next((L[k].split()[0] for k in range(i, len(L)) if ' new_render_encoder' in L[k]), None)
    first = next(k for k, l in enumerate(L) if l.startswith(r + ' '))
    good = enc.startswith('H') and i < first
    print('seq=%d on %s at line %d, before %s (first op line %d): %s' % (s, enc, i + 1, r, first + 1, 'ok' if good else 'FAIL'))
    ok &= good
print('structure: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
EOF
echo "== 5. refusal predicate (incl. the pass write-set predicate: SHADER_WRITE / XFB / FEEDBACK)"
"$T" predicate || rc=1
echo "== 5b. oracle (BB_KK_HOIST_ORACLE): real decoded bindings vs the gate"
"$T" oracle || rc=1
echo "== 5c. oracle on over the whole hoist stream (no mismatch allowed)"
BB_KK_HOIST_ORACLE=1 "$T" hoist "$OUT/horacle" > "$OUT/horacle.counters" 2> "$OUT/horacle.err"; orc=$?
cat "$OUT/horacle.counters"
if [ $orc = 0 ] && cmp -s "$OUT/hoist.mtl" "$OUT/horacle.mtl" && cmp -s "$OUT/hoist.renc" "$OUT/horacle.renc"; then echo "oracle stream: PASS (Metal + renc logs identical to the oracle-off run)"; else cat "$OUT/horacle.err"; echo "oracle stream: FAIL (rc=$orc)"; rc=1; fi
echo "== 5d. BB_KK_HOIST_WHY_ALL over the hoist stream (counting only: logs must be identical)"
BB_KK_HOIST_WHY_ALL=1 "$T" hoist "$OUT/hwhy" > "$OUT/hwhy.counters" 2>/dev/null || rc=1
cat "$OUT/hwhy.counters"
if cmp -s "$OUT/hoist.mtl" "$OUT/hwhy.mtl" && cmp -s "$OUT/hoist.renc" "$OUT/hwhy.renc" && cmp -s "$OUT/hoist.hoist" "$OUT/hwhy.hoist"; then echo "why-all: decision unchanged (logs identical)"; else echo "why-all: CHANGED THE DECISION (FAIL)"; rc=1; fi
python3 - "$OUT/hwhy.counters" <<'PYEOF' || rc=1
import re, sys
t = open(sys.argv[1]).read()
c = dict((k, int(v)) for k, v in re.findall(r'(\w+)=(\d+)', t.split('why-all:')[1]))
# expected for this stream (counted by hand from the op list in renc_hoist_test.c):
# 12 draw gates (D1..D12), 5 refused (D3 D4 D5 D10 D12); refused only by the may-touch-anything family:
# D3 (tess with no VS reflection: ANY) and D12 (tess after a memory-writing
# draw: SHADER_WRITE); their ENC/BUSY cascade: D4 (ENC + BUSY + ANY_EARLIER,
# all caused by D3's M1 fallback). D6 (first heap user) is no longer refused
# (its heap reset is hoisted with it). Renderings with gated draws: A..H = 8,
# hoisted end to end: C, G; would hoist end to end if the family were
# satisfied: A (D3/D4 excused), C, E (its refusal is a late ANY), G, H.
# D7 (pass D: a compute encoder from work outside the pass is carried into
# it) is no longer refused BUSY: cs_start_render ends the carried encoder, so
# D7 hoists and rendering D is hoisted end to end (and would be if-any).
want = {'evals': 12, 'refused': 5, 'only_any': 2, 'any_cascade': 1,
        'renderings': 8, 'hoisted': 3, 'if_any': 6}
bad = [k for k in want if c.get(k) != want[k]]
print('why-all expected %s: %s' % (want, 'PASS' if not bad else 'FAIL %s' % {k: c.get(k) for k in bad}))
sys.exit(1 if bad else 0)
PYEOF
echo "== 6. negative controls (must be DETECTED)"
$X "$OUT/deferred.mtl" "$OUT/porder.hoist" > "$OUT/expected-porder.mtl"
if cmp -s "$OUT/expected-porder.mtl" "$OUT/porder.mtl"; then echo "perturb-order: NOT DETECTED (harness broken)"; rc=1; else echo "perturb-order (render encoder created before the hoisted dispatch):"; diff "$OUT/expected-porder.mtl" "$OUT/porder.mtl" | head -12; echo "perturb-order: detected"; fi
if cmp -s "$OUT/expected.mtl" "$OUT/pbind.mtl"; then echo "perturb-bind: NOT DETECTED (harness broken)"; rc=1; else diff "$OUT/expected.mtl" "$OUT/pbind.mtl"; echo "perturb-bind: detected"; fi
echo "== 7. heap stream: the pass's first heap user (kk_heap's reset)"
for m in direct deferred hoist; do "$T" heap $m "$OUT/heap-$m" > "$OUT/heap-$m.counters" || { echo "heap $m failed"; rc=1; }; done
cat "$OUT/heap-hoist.counters"
echo -n "7a. M1 unchanged on this stream: direct vs deferred renc: "; same "$OUT/heap-direct.renc" "$OUT/heap-deferred.renc"
echo -n "    direct vs deferred mtl (global order): "; same "$OUT/heap-direct.mtl" "$OUT/heap-deferred.mtl"
echo -n "    reset placement direct vs deferred: "; same "$OUT/heap-direct.heap" "$OUT/heap-deferred.heap"
echo "7b. recorded op list deferred vs hoist (only the hoisted resets may change, post -> pre):"
python3 "$HERE/heap_renc.py" "$OUT/heap-deferred.renc" "$OUT/heap-hoist.renc" "$OUT/heap-deferred.heap" "$OUT/heap-hoist.heap" || rc=1
$X "$OUT/heap-deferred.mtl" "$OUT/heap-hoist.hoist" > "$OUT/heap-expected.mtl" || { echo "expect_hoist.py failed"; exit 2; }
echo -n "7c. hoist mtl vs expected (M1 with exactly the hoisted dispatches moved), raw cmp: "
same "$OUT/heap-expected.mtl" "$OUT/heap-hoist.mtl"
echo "7d. per draw: reset -> heap-using dispatches -> render encoder; HEAP counter; non-hoisted placement"
python3 "$HERE/heap_check.py" "$OUT/heap-hoist" "$OUT/heap-deferred" || rc=1
echo "7e. negative control: a hoist whose reset lands AFTER the unroll/tess (reset left post) must be caught"
python3 "$HERE/heap_check.py" "$OUT/heap-hoist" "$OUT/heap-deferred" --control-reset-late > "$OUT/heap-control.txt" 2>&1
if [ $? = 0 ]; then cat "$OUT/heap-control.txt"; echo "reset-late control: NOT DETECTED (harness broken)"; rc=1; else grep FAIL "$OUT/heap-control.txt" | head -4; echo "reset-late control: detected"; fi
echo "== 8. BUSY cascade: draw A refused (in-pass barrier, M1 path: its heap reset is in-pass post-gfx work,"
echo "   promoted into the pre_gfx slot still open by EndRendering's cs_end), draw B = first gated draw of the next pass"
"$T" cascade hoist "$OUT/casc" > "$OUT/casc.counters" || { echo "cascade failed"; rc=1; }
"$T" cascade hoist "$OUT/casc-ctl" keep-open > "$OUT/casc-ctl.counters" || { echo "cascade control failed"; rc=1; }
"$T" cascade deferred "$OUT/casc-m1" > /dev/null || { echo "cascade M1 failed"; rc=1; }
cat "$OUT/casc.counters"
echo "   B's hoist (Metal log, from its hoist encoder to its pass's render encoder):"
python3 - "$OUT/casc.mtl" <<'PYEOF'
import sys
L = open(sys.argv[1]).read().splitlines()
i = next(k for k, l in enumerate(L) if ' new_compute_encoder hoist' in l)
j = next(k for k in range(i, len(L)) if ' new_render_encoder' in L[k])
for l in L[max(0, i - 3):j + 1]: print('     ' + l)
PYEOF
python3 - "$OUT/casc" "$OUT/casc-ctl" <<'PYEOF' || rc=1
import re, sys
def rd(p):
    t = open(p + '.counters').read()
    b = re.search(r'^B: seq=(\d+) hoisted=(\d) draws\+=(\d+) refused_busy\+=(\d+)', t, re.M)
    a = re.search(r'^A: hoisted=(\d) refused_barrier\+=(\d+)', t, re.M)
    hs = [int(x) for x in open(p + '.hoist').read().split()]
    return a, b, hs, open(p + '.mtl').read().splitlines()
a, b, hs, L = rd(sys.argv[1])
ok = a.group(1) == '0' and a.group(2) == '1'
print('A refused BARRIER (M1 path): %s' % ('ok' if ok else 'FAIL'))
for p in sys.argv[1:3]:
    m = re.search(r'^A2 \(same pass as A\): hoisted=(\d) refused_enc\+=(\d+) refused_busy\+=(\d+)', open(p + '.counters').read(), re.M)
    g = m.group(1) == '0' and m.group(2) == '1' and m.group(3) == '0'
    print('A2 (%s, same pass as A) refuses ENC, not BUSY (render encoder created by A\'s M1 sync): %s' % (p.rsplit('/', 1)[1], 'ok' if g else 'FAIL'))
    ok &= g
s = int(b.group(1))
# the hoisted dispatch is on an H encoder that ends right before the pass's render encoder,
# and the carried encoder (A's reset) ended before that hoist encoder was created
i = next(k for k, l in enumerate(L) if re.search(r' dispatch prog=\d+ seq=%d$' % s, l))
h = L[i].split()[0]
e = L.index(h + ' end_encoding')
good_b = (b.group(2) == '1' and b.group(3) == '1' and b.group(4) == '0' and s in hs and
          h.startswith('H') and L[e + 1].split()[1] == 'new_render_encoder')
c = next(l.split()[0] for l in L if l.endswith('new_compute_encoder post'))
hc = next(k for k, l in enumerate(L) if l.startswith(h + ' new_compute_encoder hoist'))
good_b &= L.index(c + ' end_encoding') < hc
print('B (seq=%d) hoisted on %s, BUSY +0, carried %s (A\'s reset) ended before %s was created: %s' % (s, h, c, h, 'ok' if good_b else 'FAIL'))
ok &= good_b
ca, cb, chs, CL = rd(sys.argv[2])
ctl = cb.group(2) == '0' and cb.group(4) == '1' and int(cb.group(1)) not in chs
print('control (carried encoder left open, today): B hoisted=%s refused_busy+=%s -> %s' % (
    cb.group(2), cb.group(4), 'refuses BUSY: detected' if ctl else 'NOT DETECTED (harness broken)'))
ok &= ctl
print('BUSY cascade: %s' % ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
PYEOF
$X "$OUT/casc-m1.mtl" "$OUT/casc.hoist" > "$OUT/casc-expected.mtl" || { echo "expect_hoist.py failed"; exit 2; }
echo -n "8b. cascade hoist mtl vs expected (M1 log, carried encoder ended at pass start, hoisted dispatches moved), raw cmp: "
same "$OUT/casc-expected.mtl" "$OUT/casc.mtl"
echo "== result: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
