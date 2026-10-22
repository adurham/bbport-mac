# KK AIR + MTL4 archive artefacts (union capture, 2026-10-22)

Driver: `deps-kkroot/lib/kosmickrisp/libvulkan_kosmickrisp.dylib` (KK 3af1126 + mesa dc41592 + 0001-0008,
built 2026-10-22 02:24). The Mesa cache below is only valid for this exact binary (Mesa keys on build-id).
Bundle id io.github.bbport.kkair. Harness: `tools/macos/kkair_union.sh` (HELDOUT=roam,
CAPTURE_ROUTES="yharnam attack combat spin sweep interact warp"); log `out/kkair/union.log`.

## What ships

| artefact | path | content |
|---|---|---|
| AIR | `air/<blake3>.metallib` (+ `.metal` source, `manifest.txt` flags) | 438 libraries = union of every MSL dumped by proof-capture1 (383) and pass A of the 7 routes (+55) |
| MTL4 archive | `base.archive.d/seg-NNNN.mtl4archive` (35 segments, 82 MB) | all segments; load the directory with `BB_KK_ARCHIVES` |
| MTL4 archive, deduped | `base.archive.dedup.d/` (14 segments, 12 MB) | seg-0003..0009 + only the per-route new-pipeline segments (see below) |
| Mesa cache | `mesa/` (19 MB) | chained through every 0008 run; last refilled by B-warp |
| pass-A segments | `passA/<route>/` | runtime-MSL-keyed captures, **never ship** (they miss when the AIR is loaded, see below) |

## Which route produced which segment

| segments | produced by |
|---|---|
| seg-0000..0002 | proof capture2 (yharnam pad, 300 s, AIR of 370 libs) |
| seg-0003..0009 | B-yharnam |
| seg-0010..0015 | B-attack |
| seg-0016..0020 | B-combat |
| seg-0021..0023 | B-spin |
| seg-0024..0026 | B-sweep |
| seg-0027..0030 | B-interact |
| seg-0031..0034 | B-warp |

In each pass-B run the first three segments are the area-load set again, 58/10/247 pipelines, 1.06/0.29/9.02 MB.
The serializer captures every pipeline the run *creates*, including ones served from lookupArchives. The rest
are the route's own new pipelines: 0006-0009, 0013-0015, 0019-0020, 0030 and 0034. Those, plus 0003-0005, make up `base.archive.dedup.d`.

## Rebuild

1. Pass A: per route, `BB_KK_AIR_DIR=air BB_KK_ARCHIVE_CAPTURE=passA/<route>`, seeded with `mesa/`. This dumps the missing MSL.
2. `PRUNE=1 bash tools/macos/kkair_build_air.sh air`.
3. Pass B: per route, `BB_KK_AIR_DIR=air BB_KK_ARCHIVES=base.archive.d BB_KK_ARCHIVE_CAPTURE=base.archive.d`. This appends segments.
4. Ship `mesa/` from the last pass-B run.

Two capture passes are needed for one reason. An archive entry keys on the function hash of the MTLLibrary,
and a library compiled at run time from MSL hashes differently from the `xcrun metal` metallib of the same MSL.
Tested in isolation: an entry captured from the runtime library MISSES when the pipeline is built from the
metallib, and an entry captured from the metallib HITS for both. So a pipeline must be captured with every
one of its libraries already coming from the shipped AIR.
