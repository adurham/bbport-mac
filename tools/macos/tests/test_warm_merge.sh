#!/usr/bin/env bash
# tools/macos/tests/test_warm_merge.sh: proves tools/macos/warm_merge.sh merges a sandbox pipeline
# store into the real one the way docs/PIPELINE_WARMTH.md says it does — additively, without ever
# touching a save or any other file of the real user dir, and without copying a blob the loader
# would treat as damaged.
#
#   bash tools/macos/tests/test_warm_merge.sh
#
# Synthetic on purpose: the real store is 1100+ files of hashes and no case here depends on what
# they hold. Every case makes a scratch pair of user dirs under $TMPDIR and checks the file system
# afterwards, not the merge's own report (except where the report is the thing being checked).
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
merge=$root/tools/macos/warm_merge.sh
[[ -f $merge ]] || { echo "STOP: $merge not found" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/warmfill-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
pass=0
fail=0

# check DESCRIPTION COMMAND...: ok when the command succeeds.
check() {
    local desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n' "$desc" >&2
    fi
}

# merge_log LOG ARGS...: the merge, its output kept for the checks that read it.
merge_log() {
    local log=$1
    shift
    "$merge" "$@" >"$log" 2>&1
}

# A pair of user dirs with one native store and one x86-64 store, the layout of blob_store.cpp.
new_pair() {
    sand=$work/$1/sandbox-user
    real=$work/$1/real-user
    for side in "$sand" "$real"; do
        mkdir -p "$side/cache/CUSA00000-arm64" "$side/cache/CUSA00000"
        mkdir -p "$side/savedata/1/CUSA00000/SPRJ0005"
        printf "the player's save\n" >"$side/savedata/1/CUSA00000/SPRJ0005/userdata0000"
    done
    store=$sand/cache/CUSA00000-arm64
    rstore=$real/cache/CUSA00000-arm64
}

# A .meta/.key blob: the payload text, padded to the length with zeros. The merge only checks the
# length, but a case names its blobs by their text and reads them back with grep.
blob() { { printf '%s' "$3"; head -c "$2" /dev/zero; } | head -c "$2" >"$1"; }
# A .spv blob: the SPIR-V magic 0x07230203 as the file's first four bytes (03 02 23 07), a 5-word
# header and a whole number of words — what LoadPipelineStage accepts.
spv() { printf '\x03\x02\x23\x07\x00\x06\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >"$1"; }
# profile.bin: the header's length is what the merge checks, the text is how a case names it.
profile() { printf 'BBPC-the-cache-header-%s' "$2" >"$1"; }
# Everything of a user dir except the caches: what the merge must never add, drop or rewrite.
outside_cache() { find "$1" -not -path '*/cache/*' -print0 | sort -z | md5; }
none_exist() {
    local f
    for f in "$@"; do [[ -e $f ]] && return 1; done
    return 0
}

echo "== 1. a new blob is added, an identical one is not touched, nothing else moves"
new_pair case1
blob "$store/0x0000000000000001.meta" 32 a
blob "$rstore/0x0000000000000001.meta" 32 a # identical on both sides
blob "$rstore/0x0000000000000002.key" 64 b  # only in the real store
blob "$store/0x0000000000000003.key" 64 c   # only in the sandbox
profile "$store/profile.bin" sandbox
profile "$rstore/profile.bin" real
outside_before=$(outside_cache "$real")
mtime_before=$(stat -f%m "$rstore/0x0000000000000001.meta")
sleep 1.1 # a rewrite would move the mtime: the file system stamps whole seconds
check "the merge succeeded" merge_log "$work/case1.log" "$sand" "$real"
check "the new blob arrived" test -f "$rstore/0x0000000000000003.key"
check "and holds the sandbox's bytes" cmp -s "$store/0x0000000000000003.key" "$rstore/0x0000000000000003.key"
check "the identical blob was not rewritten (mtime unchanged)" \
    test "$(stat -f%m "$rstore/0x0000000000000001.meta")" = "$mtime_before"
check "the real store's own blob is still there" test -f "$rstore/0x0000000000000002.key"
check "every file outside cache/ is byte for byte what it was" \
    test "$(outside_cache "$real")" = "$outside_before"
check "the report counted one added and one identical" \
    grep -q 'added 1 (1 key, 0 meta, 0 spv), 1 identical' "$work/case1.log"
check "the report counted no conflict" grep -q '0 conflict' "$work/case1.log"
check "no .warmfill-tmp was left behind" test "$(find "$real" -name '*.warmfill-tmp' | wc -l | tr -d ' ')" = 0

echo "== 2. same name, different bytes: the real store's copy is kept; --prefer-sandbox flips it"
new_pair case2
blob "$store/0x0000000000000004.key" 64 sandbox-version
blob "$rstore/0x0000000000000004.key" 64 real-version-xx
check "the merge succeeded" merge_log "$work/case2.log" "$sand" "$real"
check "the real store's bytes were kept" grep -q 'real-version-xx' "$rstore/0x0000000000000004.key"
check "the conflict was counted" grep -q '1 conflict (real kept)' "$work/case2.log"
check "with --prefer-sandbox the sandbox's bytes win" \
    merge_log "$work/case2b.log" --prefer-sandbox "$sand" "$real"
check "the replaced bytes are the sandbox's" grep -q 'sandbox-version' "$rstore/0x0000000000000004.key"
check "and it was counted as replaced" grep -q '1 replaced' "$work/case2b.log"
check "still no .warmfill-tmp left behind" test "$(find "$real" -name '*.warmfill-tmp' | wc -l | tr -d ' ')" = 0

echo "== 3. a blob the loader would reject is not copied"
new_pair case3
printf 'not-spirv-at-all' >"$store/0x0000000000000005_0.spv"         # no magic
printf '\x03\x23\x02\x07\x00\x00' >"$store/0x0000000000000006_0.spv" # magic but 6 bytes
blob "$store/0x0000000000000007.key" 4 short                         # a .key under 8 bytes
blob "$store/0x0000000000000008.meta" 8 short                        # a .meta under 16 bytes
spv "$store/0x0000000000000009_0.spv"                                # a valid one
printf 'half a write' >"$store/0x000000000000000a.key.tmp"           # the writer's own temp name
printf 'junk' >"$store/.DS_Store"
check "the merge succeeded" merge_log "$work/case3.log" "$sand" "$real"
check "the valid blob was added" test -f "$rstore/0x0000000000000009_0.spv"
check "no damaged blob was copied" none_exist \
    "$rstore/0x0000000000000005_0.spv" "$rstore/0x0000000000000006_0.spv" \
    "$rstore/0x0000000000000007.key" "$rstore/0x0000000000000008.meta"
check "the damaged blobs were counted" grep -q '4 invalid' "$work/case3.log"
check "the .tmp and .DS_Store were not copied" \
    none_exist "$rstore/0x000000000000000a.key.tmp" "$rstore/.DS_Store"
check "and were counted as not-blobs" grep -q '2 not a blob' "$work/case3.log"

echo "== 4. profile.bin, the gate"
new_pair case4
blob "$store/0x000000000000000b.key" 64 x
profile "$store/profile.bin" sandbox
profile "$rstore/profile.bin" real
check "the merge succeeded with a differing profile" merge_log "$work/case4z.log" "$sand" "$real"
check "a differing one is left alone by default" grep -q 'BBPC-the-cache-header-real' "$rstore/profile.bin"
check "and the consequence is spelled out" grep -q 'profile.bin DIFFERS' "$work/case4z.log"
mtime_before=$(stat -f%m "$rstore/profile.bin")
profile "$store/profile.bin" real # the sandbox catches up with the real store's
sleep 1.1
check "an identical one is left alone" merge_log "$work/case4a.log" "$sand" "$real"
check "its mtime did not move" test "$(stat -f%m "$rstore/profile.bin")" = "$mtime_before"
check "and the report said so" grep -q 'profile.bin identical' "$work/case4a.log"
profile "$store/profile.bin" another-build
check "--adopt-profile takes the sandbox's" merge_log "$work/case4d.log" --adopt-profile "$sand" "$real"
check "the sandbox's profile is installed" grep -q 'another-build' "$rstore/profile.bin"
check "and it was logged as adopted" grep -q 'profile.bin adopted' "$work/case4d.log"
rm -f "$rstore/profile.bin"
check "installed when the real store has none" merge_log "$work/case4e.log" "$sand" "$real"
check "the file is there" test -f "$rstore/profile.bin"
check "and the report said so" grep -q 'profile.bin installed' "$work/case4e.log"

echo "== 5. --dry-run writes nothing anywhere"
new_pair case5
blob "$store/0x000000000000000c.key" 64 y
profile "$store/profile.bin" sandbox
before=$(find "$work/case5" -print0 | sort -z | md5)
check "the dry run succeeded" merge_log "$work/case5.log" --dry-run "$sand" "$real"
check "the tree is byte for byte what it was" test "$(find "$work/case5" -print0 | sort -z | md5)" = "$before"
check "and it reported what it would do" grep -q 'would add' "$work/case5.log"

echo "== 6. the two stores are kept apart; --store picks one"
new_pair case6
blob "$sand/cache/CUSA00000-arm64/0x000000000000000d.key" 64 native
blob "$sand/cache/CUSA00000/0x000000000000000e.key" 64 x86
check "--store merged the x86-64 store" merge_log "$work/case6.log" --store CUSA00000 "$sand" "$real"
check "its blob arrived" test -f "$real/cache/CUSA00000/0x000000000000000e.key"
check "the native store was left alone" test ! -f "$real/cache/CUSA00000-arm64/0x000000000000000d.key"
check "a plain merge takes both" merge_log "$work/case6b.log" "$sand" "$real"
check "the native blob arrived too" test -f "$real/cache/CUSA00000-arm64/0x000000000000000d.key"

echo "== 7. refusals"
new_pair case7
st=0
merge_log "$work/case7a.log" "$sand" "$sand" || st=$?
check "merging a user dir into itself is refused" test "$st" != 0
check "and it says why" grep -q 'same directory' "$work/case7a.log"
mkdir -p "$work/case7/empty-user/cache"
st=0
merge_log "$work/case7b.log" "$work/case7/empty-user" "$real" || st=$?
check "a sandbox with no store exits 2 (nothing to merge, not an error)" test "$st" = 2

echo "== 8. the snapshot warmfill takes is an independent copy (cp -cR / APFS clone)"
mkdir -p "$work/case8/user/cache/CUSA00000-arm64"
blob "$work/case8/user/cache/CUSA00000-arm64/0x000000000000000f.key" 64 clone-source
cp -cR "$work/case8/user" "$work/case8/copy"
check "the clone holds the file" test -f "$work/case8/copy/cache/CUSA00000-arm64/0x000000000000000f.key"
printf 'the warm run wrote this' >"$work/case8/copy/cache/CUSA00000-arm64/0x000000000000000f.key"
printf 'new blob' >"$work/case8/copy/cache/CUSA00000-arm64/0x0000000000000010.key"
check "a write in the copy does not reach the original (copy on write, not a hard link)" \
    grep -q clone-source "$work/case8/user/cache/CUSA00000-arm64/0x000000000000000f.key"
check "and the original gained nothing" \
    test "$(find "$work/case8/user" -type f | wc -l | tr -d ' ')" = 1

echo "== 9. against a real store's own blobs (every side here is a clone: nothing writes to the real user dir)"
if [[ -n ${WARMFILL_TEST_REAL_USER:-} ]]; then
    src_real=$WARMFILL_TEST_REAL_USER
    if [[ ! -d $src_real/cache ]]; then
        echo "  FAIL WARMFILL_TEST_REAL_USER has no cache/: $src_real" >&2
        fail=$((fail + 1))
    else
        real_before=$(find "$src_real" -print0 | sort -z | md5)
        mkdir -p "$work/case9"
        cp -cR "$src_real" "$work/case9/sandbox-user"
        cp -cR "$src_real" "$work/case9/real-user"
        n_blobs=$(find "$work/case9/sandbox-user/cache" -type f | wc -l | tr -d ' ')
        store_name=$(find "$work/case9/sandbox-user/cache" -mindepth 1 -maxdepth 1 -type d | sort | awk 'NR==1')
        store_name=$(basename "$store_name")
        echo "  ($n_blobs blobs in $store_name, cloned — not copied — twice)"
        # Three of the real store's own .key blobs, taken from the "real" side and re-added by the
        # merge: exactly what a warm fill does for the blobs a route wrote.
        keys=$(find "$work/case9/sandbox-user/cache" -type f -name '*.key' | sort | awk 'NR<=3')
        dropped=0
        for f in $keys; do
            rm -f "$work/case9/real-user/cache/$store_name/$(basename "$f")"
            dropped=$((dropped + 1))
        done
        check "the merge succeeded on real blobs" merge_log "$work/case9.log" \
            "$work/case9/sandbox-user" "$work/case9/real-user"
        check "exactly the $dropped dropped blobs came back" \
            grep -q "added $dropped ($dropped key, 0 meta, 0 spv), $((n_blobs - 1 - dropped)) identical" "$work/case9.log"
        check "not one real blob was rejected as damaged" grep -q '0 invalid' "$work/case9.log"
        check "the real store's profile.bin was seen as identical" grep -q 'profile.bin identical' "$work/case9.log"
        restored=0
        for f in $keys; do
            [[ $(md5 -q "$work/case9/real-user/cache/$store_name/$(basename "$f")") == "$(md5 -q "$f")" ]] || restored=1
        done
        check "every restored blob is byte-identical to the sandbox's copy of it" test "$restored" = 0
        check "the real user dir itself is untouched" \
            test "$(find "$src_real" -print0 | sort -z | md5)" = "$real_before"
    fi
else
    echo "  (skipped: set WARMFILL_TEST_REAL_USER=<a user dir with a filled store> to run it)"
fi

printf '\n%d checks passed, %d failed\n' "$pass" "$fail"
((fail == 0))
