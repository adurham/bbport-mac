#!/usr/bin/env bash
# tools/macos/warm_merge.sh: moves the pipeline-cache blobs a sandboxed warm run made into the
# real pipeline store. docs/PIPELINE_WARMTH.md, "Filling the cache from an area on purpose".
#
#   tools/macos/warm_merge.sh [options] <sandbox user dir> <real user dir>
#
# Why this is safe: the store is content-addressed (gpu/render/blob_store.cpp, pipeline_store.cpp).
# A blob's name is a hash of what it holds — a permutation's `.meta`, its SPIR-V `_N.spv`, a
# pipeline's key `.key` — so merging two stores is a set union, and a name present on both sides
# holds the same bytes. Nothing is deleted or renamed, only `<real user>/cache/<store>/` is
# written, and a copy goes in through a temporary name and a rename, as the game's own writer
# does (BlobStore::WriteFile), so a reader never sees a half-written blob.
#
# Options:
#   --dry-run         report what would be copied; write nothing
#   --store NAME      merge one store directory (default: every store in the sandbox that holds
#                     blobs, so the native `<serial>-arm64` one is merged whichever build made it)
#   --prefer-sandbox  a name whose bytes differ is kept from the sandbox instead of from the real
#                     store (default: the real store's copy wins and is counted as a conflict)
#   --adopt-profile   install the sandbox's profile.bin when it differs (see "The profile gate")
#   --json FILE       also write the result as JSON (warmfill.sh aggregates it)
#   -q, --quiet       print the store lines only
#
# The profile gate: `profile.bin` holds the cache format's version constants and the device
# profile (PipelineCache::SaveProfile, WarmUp). The game clears the *whole* store on the next start
# when it does not match the running build, so after a version bump the store the sandbox ran on is
# not the store the game will use, and merging blobs alone changes nothing: the game drops them at
# its next start. --adopt-profile installs the sandbox's profile.bin, which is what makes the
# merged blobs count — valid exactly when the sandbox run used the same build as the game will run
# (warmfill.sh runs both from one checkout, which is the case it is made for). Without the flag a
# differing profile is reported and left alone.
set -euo pipefail

prog=$(basename -- "$0")

die() {
    echo "STOP ($prog): $*" >&2
    exit 1
}
usage() {
    echo "usage: $prog [--dry-run] [--prefer-sandbox] [--adopt-profile] [--store NAME] [--json FILE] [-q] SANDBOX_USER_DIR REAL_USER_DIR" >&2
    exit 1
}

quiet=0
say() { ((quiet)) || echo "$@"; }

# The blobs the game loads: anything else in the store directory (its own `.tmp` files, a
# `.DS_Store`) is ignored, never copied, and never counted as a blob. `profile.bin` is not a
# per-draw blob either — it is the gate the merge handles on its own.
blob_ext() {
    case $1 in
    spv | key | meta) return 0 ;;
    *) return 1 ;;
    esac
}

# A blob the loader can take without tripping its own corruption path. A damaged `.key` makes
# PipelineCache::WarmUp clear the entire store (`num_damaged` → rebuild), so this is the last place
# a truncated or half-flushed file can be caught. LoadPipelineStage rejects SPIR-V without the
# magic number or with fewer than 5 words; the others only have to hold their header.
valid_blob() {
    local file=$1 size
    size=$(stat -f%z "$file") || return 1
    case ${file##*.} in
    spv)
        ((size >= 20 && size % 4 == 0)) || return 1
        # The SPIR-V magic 0x07230203 as the four bytes the file holds, 03 02 23 07 (little-endian
        # is what LoadPipelineStage's `spv[0] != 0x07230203u` reads them as).
        [[ $(od -An -N4 -tx1 "$file" | tr -d ' \n') == 03022307 ]] || return 1
        ;;
    key) ((size >= 8)) || return 1 ;;
    bin) ((size >= 16)) || return 1 ;;
    meta) ((size >= 16)) || return 1 ;;
    *) return 1 ;;
    esac
    return 0
}

# In through a temporary name and a rename, as the game's writer does: a reader (the game, the
# next merge) never sees a short file, and an interrupted merge leaves a `.warmfill-tmp`, which is
# not a blob name and is ignored by the loader.
put_blob() { # src dst
    local src=$1 dst=$2 tmp=$2.warmfill-tmp
    rm -f "$tmp"
    # Files already on this volume are clones (APFS): free and instant. Elsewhere, a plain copy.
    if ! cp -c "$src" "$tmp" 2>/dev/null && ! cp -p "$src" "$tmp"; then
        rm -f "$tmp"
        return 1
    fi
    mv -f "$tmp" "$dst"
}

dry_run=0
prefer_sandbox=0
adopt_profile=0
json_out=
store_only=
while [[ $# -gt 0 ]]; do
    case $1 in
    --dry-run) dry_run=1 ;;
    --prefer-sandbox) prefer_sandbox=1 ;;
    --adopt-profile) adopt_profile=1 ;;
    --store)
        store_only=${2:?--store needs a name}
        shift
        ;;
    --json)
        json_out=${2:?--json needs a file}
        shift
        ;;
    -q | --quiet) quiet=1 ;;
    -h | --help) usage ;;
    --*) die "unknown option $1" ;;
    *) break ;;
    esac
    shift
done
[[ $# -eq 2 ]] || usage
src_user=$(cd -- "$1" 2>/dev/null && pwd) || die "no such sandbox user dir: $1"
dst_user=$(cd -- "$2" 2>/dev/null && pwd) || die "no such real user dir: $2"
[[ $src_user != "$dst_user" ]] || die "the sandbox user dir and the real one are the same directory"

dst_cache=$dst_user/cache
src_cache=$src_user/cache

# The store directories to merge. A store is named after the game serial, and which stores exist
# depends on the path that ran there (blob_store.cpp: the x86-64 process and the arm64 one keep
# their own). The native `<serial>-arm64` is merged first; `sort -r` puts it before `<serial>`.
stores() {
    local dir name
    for dir in "$src_cache"/*; do
        [[ -d $dir ]] || continue
        name=${dir##*/}
        if [[ -n $store_only && $name != "$store_only" ]]; then
            continue
        fi
        if ! compgen -G "$dir/*.key" >/dev/null && ! compgen -G "$dir/*.meta" >/dev/null; then
            continue
        fi
        printf '%s\n' "$name"
    done | sort -r
}

store_count=0
total_added=0
total_identical=0
total_conflict=0
total_replaced=0
total_invalid=0
json_stores=()

for store in $(stores); do
    src=$src_cache/$store
    dst=$dst_cache/$store
    store_count=$((store_count + 1))
    if [[ ! -d $dst ]] && ((!dry_run)); then
        mkdir -p "$dst"
    fi
    added=0 identical=0 conflict=0 replaced=0 invalid=0 other=0
    added_key=0
    added_meta=0
    added_spv=0

    # The store holds blobs and whatever the file system leaves behind (a .DS_Store): both globs,
    # so an unexpected dotfile is counted instead of silently ignored.
    for src_file in "$src"/* "$src"/.[!.]*; do
        [[ -f $src_file ]] || continue
        name=${src_file##*/}
        if [[ $name == profile.bin ]]; then
            continue # the gate: its own policy, below
        fi
        if ! blob_ext "${name##*.}"; then
            other=$((other + 1))
            continue
        fi
        if ! valid_blob "$src_file"; then
            invalid=$((invalid + 1))
            say "  skipped (not a loadable blob): $store/$name"
            continue
        fi
        dst_file=$dst/$name
        if [[ ! -f $dst_file ]]; then
            if ((dry_run)); then
                say "  would add $store/$name"
            elif ! put_blob "$src_file" "$dst_file"; then
                say "  WARNING: could not copy $store/$name"
                continue
            fi
            added=$((added + 1))
            case ${name##*.} in
            key) added_key=$((added_key + 1)) ;;
            meta) added_meta=$((added_meta + 1)) ;;
            spv) added_spv=$((added_spv + 1)) ;;
            esac
        elif cmp -s -- "$src_file" "$dst_file"; then
            identical=$((identical + 1))
        elif ((prefer_sandbox)); then
            if ((dry_run)); then
                say "  would replace $store/$name (bytes differ)"
            elif ! put_blob "$src_file" "$dst_file"; then
                say "  WARNING: could not replace $store/$name"
                continue
            fi
            replaced=$((replaced + 1))
        else
            # Same name, different bytes: a name is a hash of the contents, so this is either a
            # collision or two cache generations in one store. The real store's file is kept — the
            # conservative choice — and reported.
            conflict=$((conflict + 1))
            say "  kept the real store's $store/$name (its bytes differ from the sandbox's; --prefer-sandbox takes the sandbox's)"
        fi
    done

    # profile.bin, the gate: identical → nothing to do; absent in the real store → install it (that
    # store would otherwise be rebuilt, and the merged blobs with it); different → report it, and
    # install it only with --adopt-profile.
    profile=absent
    if [[ -f $src/profile.bin ]]; then
        if [[ ! -f $dst/profile.bin ]]; then
            if ((dry_run)); then
                profile=would-install
            elif valid_blob "$src/profile.bin" && put_blob "$src/profile.bin" "$dst/profile.bin"; then
                profile=installed
            else
                profile=invalid
            fi
        elif cmp -s -- "$src/profile.bin" "$dst/profile.bin"; then
            profile=identical
        elif ((adopt_profile)); then
            if ((dry_run)); then
                profile=would-adopt
            elif put_blob "$src/profile.bin" "$dst/profile.bin"; then
                profile=adopted
            else
                profile=invalid
            fi
        else
            profile=differs-kept
        fi
    fi

    case $profile in
    installed | would-install)
        say "store $store: profile.bin installed (the real store had none: it would be rebuilt)"
        ;;
    adopted | would-adopt)
        say "store $store: profile.bin adopted from the sandbox (the real store's is from another build; without it the game clears the store at its next start)"
        ;;
    differs-kept)
        say "store $store: profile.bin DIFFERS from the sandbox's and was left alone: the real store is from another build, so the game clears it (and the merged blobs) at its next start. Pass --adopt-profile when the sandbox ran this build."
        ;;
    invalid)
        say "store $store: WARNING: a profile.bin was too short to load; the real store's was left alone"
        ;;
    esac

    say "store $store: added $added ($added_key key, $added_meta meta, $added_spv spv), $identical identical, $conflict conflict (real kept), $replaced replaced, $invalid invalid, $other not a blob; profile.bin $profile"

    total_added=$((total_added + added))
    total_identical=$((total_identical + identical))
    total_conflict=$((total_conflict + conflict))
    total_replaced=$((total_replaced + replaced))
    total_invalid=$((total_invalid + invalid))
    json_stores[${#json_stores[@]}]="{\"store\":\"$store\",\"added\":$added,\"added_key\":$added_key,\"added_meta\":$added_meta,\"added_spv\":$added_spv,\"identical\":$identical,\"conflicts_kept\":$conflict,\"replaced\":$replaced,\"invalid\":$invalid,\"not_blob\":$other,\"profile\":\"$profile\"}"
done

if [[ -n $json_out ]]; then
    {
        printf '{"sandbox":"%s","real":"%s","dry_run":%s,"added":%d,"identical":%d,"conflicts_kept":%d,"replaced":%d,"invalid":%d,"stores":[' \
            "$src_user" "$dst_user" "$((dry_run ? 1 : 0))" \
            "$total_added" "$total_identical" "$total_conflict" "$total_replaced" "$total_invalid"
        (IFS=,; echo "${json_stores[*]:-}")
        printf ']}\n'
    } >"$json_out"
fi

if ((store_count == 0)); then
    echo "STOP ($prog): no pipeline store with blobs in $src_cache" >&2
    exit 2
fi
echo "warm merge: added $total_added blobs ($total_identical identical, $total_conflict kept conflicts, $total_replaced replaced, $total_invalid invalid) into $dst_cache"
