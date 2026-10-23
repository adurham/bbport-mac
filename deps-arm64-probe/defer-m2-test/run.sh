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
same "$OUT/deferred.renc" "$OUT/hoist.renc"
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
# 12 draw gates (D1..D12); refused only by the may-touch-anything family:
# D3 (tess with no VS reflection: ANY) and D12 (tess after a memory-writing
# draw: SHADER_WRITE); their ENC/BUSY cascade: D4 (ENC + BUSY + ANY_EARLIER,
# all caused by D3's M1 fallback). Renderings with gated draws: A..H = 8,
# hoisted end to end: G; would hoist end to end if the family were
# satisfied: A (D3/D4 excused), E (its refusal is a late ANY), G, H.
want = {'evals': 12, 'refused': 7, 'only_any': 2, 'any_cascade': 1,
        'renderings': 8, 'hoisted': 1, 'if_any': 4}
bad = [k for k in want if c.get(k) != want[k]]
print('why-all expected %s: %s' % (want, 'PASS' if not bad else 'FAIL %s' % {k: c.get(k) for k in bad}))
sys.exit(1 if bad else 0)
PYEOF
echo "== 6. negative controls (must be DETECTED)"
$X "$OUT/deferred.mtl" "$OUT/porder.hoist" > "$OUT/expected-porder.mtl"
if cmp -s "$OUT/expected-porder.mtl" "$OUT/porder.mtl"; then echo "perturb-order: NOT DETECTED (harness broken)"; rc=1; else echo "perturb-order (render encoder created before the hoisted dispatch):"; diff "$OUT/expected-porder.mtl" "$OUT/porder.mtl" | head -12; echo "perturb-order: detected"; fi
if cmp -s "$OUT/expected.mtl" "$OUT/pbind.mtl"; then echo "perturb-bind: NOT DETECTED (harness broken)"; rc=1; else diff "$OUT/expected.mtl" "$OUT/pbind.mtl"; echo "perturb-bind: detected"; fi
echo "== result: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
