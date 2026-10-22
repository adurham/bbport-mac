# KosmicKrisp preload artefacts (`kk_preload`)

This directory holds the three artefacts that let KosmicKrisp (patch 0008, `docs/KK_AIR_ARCHIVE.md`)
create the game's pipelines without compiling them: precompiled AIR libraries, Metal 4 pipeline
archives and a Mesa shader cache. They are used only when `bbport.ini` has `kk_preload=1`
(`BB_KK_PRELOAD=0/1` overrides it for one run). **Default: off.** `tools/macos/run.sh` reads the key
and, when it is on, sets `BB_KK_AIR_DIR`, `BB_KK_ARCHIVES` and `MESA_SHADER_CACHE_DIR` and logs:

    KK preload: on (kk_preload=1), ~/Games/Bloodborne/kk-artefacts: 438 AIR libraries, 14 archive segments, Mesa cache ...

The driver then logs `KK archive: loaded ...` per segment, `KK archive: 14 archives passed as
lookupArchives to every pipeline` and, every 2 s while it changes, `KK air: H libraries from AIR, M
compiled from MSL, ...; P pipelines (..., F created in < 1 ms)`.

Location: `~/Games/Bloodborne/kk-artefacts` (`BB_KK_PRELOAD_DIR` overrides it). It is outside the
checkout because the files are binaries (36 MB) tied to one driver build, and the repository keeps no
generated binaries (the FSR 4.1.1 assets live outside it for the same reason). The README in that
directory is a copy of this file.

## Layout

| path | content | size |
|---|---|---|
| `air/<blake3>.metallib` + `air/manifest.txt` | 438 AIR libraries (`xcrun metal` of the MSL the driver generates; key = BLAKE3 of tag `bbport-kk-air-1`, MSL version 4.1, math modes, MSL text) | ~5 MB |
| `archive/seg-NNNN.mtl4archive` | 14 de-duplicated MTL4 archive segments: 0003-0009 (yharnam, incl. the area-load set), 0013-0015 (attack), 0019-0020 (combat), 0030 (interact), 0034 (warp) | 12 MB |
| `mesa/mesa_shader_cache/` | Mesa's disk cache (NIR->MSL), filled by the same driver build | 19 MB |
| `DRIVER_UUID`, `DRIVER_SHA256` | the driver build these belong to | |

**Driver build:** KosmicKrisp 3af1126 + Mesa dc41592 + `tools/macos/kosmickrisp-patches/0001-0008`
(series hash `242464ada4dcf639`, the `.bbport-patches` stamp), LC_UUID
`C5B640F8-264A-36F4-8F4A-9C5F656F20CF`, sha256 `87ea3824...b982fe`, macOS 27.2 (26B5112a), Metal
toolchain 32023.921. Installed at `bbport-mac-native/deps-arm64/lib/kosmickrisp/` on 2026-10-22 (the
previous 0001-0004 driver, UUID 43D6ABE8-..., is kept in `lib/kosmickrisp.backup-20261022/`).

Mesa's cache keys on the driver's build-id (its LC_UUID), so a rebuilt driver misses every entry of
`mesa/`. `run.sh` compares the UUID of the driver bb-gpu will load with `DRIVER_UUID` and uses the
cache only when they match; on first use it clones it (APFS clone) into
`<data dir>/out/kk-mesa-<uuid>/`, so the shipped copy is never written. AIR and archives do not depend
on the driver binary, only on the MSL it generates: a driver change that alters the MSL turns the
affected pipelines into misses (they compile as before, nothing breaks).

## Capture routes

Union capture over the warm route pack (`tools/macos/warmfill.sh`), held-out route `roam`:
yharnam (300 s), attack (300 s), combat (360 s), spin (240 s), sweep (240 s), interact (180 s),
warp (300 s), each hidden (`BB_WARM_ONLY=1`), empty store, cold Metal cache. Each run was ended by the
footprint watchdog 1.5-2.5 min in, so the archive covers the first area load and 1-2 min of walking in
each route. The union is 327 port pipeline keys; the held-out roam route was 100% covered.

## Rebuild (all in `bbport-kkair-build`, `docs/KK_AIR_ARCHIVE.md`)

1. **AIR** (after any change to the MSL KosmicKrisp generates): pass A of
   `tools/macos/kkair_union.sh` dumps the MSL that has no AIR yet
   (`BB_KK_AIR_DIR=air BB_KK_ARCHIVE_CAPTURE=passA/<route>`), then
   `PRUNE=1 bash tools/macos/kkair_build_air.sh out/kkair/artefacts/air` compiles each `<key>.metal`
   with the flags in `manifest.txt`.
2. **Archive** (after the AIR changed): pass B of `kkair_union.sh`
   (`BB_KK_AIR_DIR=air BB_KK_ARCHIVES=base.archive.d BB_KK_ARCHIVE_CAPTURE=base.archive.d`); then
   keep the area-load segments of the first route plus each route's own new-pipeline segments
   (`segments_by_route.txt`) in `base.archive.dedup.d`. **Never ship `passA/`**: those entries are
   keyed on runtime-compiled libraries and miss when the AIR is loaded.
3. **Mesa cache** (after ANY driver rebuild): the `mesa/` of the last pass-B run of that driver build.
4. Install: `bash tools/macos/kk_preload_install.sh [out/kkair/artefacts] [~/Games/Bloodborne/kk-artefacts]`
   (`KK_DRIVER=<dylib>` names the driver whose UUID is stamped; default the capture driver).

## What it does and does not cover

On the held-out route the area's driver time went from 39-41 s to 0.18-0.25 s, PSO p50 from 17.9 to
0.125 ms, 0 libraries from MSL, 98% archive hits, hitch frames (> 50 ms) 43-70 -> 19-28. What still
compiles: pipelines whose state only a specific player position/state produces (on roam 6 keys of
~254 were in no capture route; 11 of 214 PSOs took >= 1 ms, 393 ms in total), and anything outside
the captured areas (other areas, bosses, cutscenes): those compile exactly as they do today, then the
port's own store keeps them.

## 2026-10-22 top-up: the load burst after the position moved

`docs/HITCH_ATTRIBUTION_2026-10-22.md` found the one compile left in a covered area: a 30-pipeline
burst at the area load, **2 016.9 ms** on a cold Metal cache and **329 ms** on the next launch, all
30 archive misses. 26 of the run's 31 created keys appeared in no capture route: the set was a
snapshot of the save position as it was captured, and the owner has walked since.

The top-up is the same three layers, one route, on today's position (all hidden, `STORE=copy`):

1. **MSL**: a pass-A run with `BB_KK_AIR_DIR=<artefacts/air>` and no archive loaded dumps the MSL the
   AIR lacks - 234 files the first time, 6 more on a second pass after the first rebuild. Offline
   `PRUNE=1 kkair_build_air.sh` -> **438 -> 678 libraries**.
2. **Archive**: a pass-B run with the rebuilt AIR and `BB_KK_ARCHIVES=<shipped segments>` +
   `BB_KK_ARCHIVE_CAPTURE=<new dir>` writes AIR-keyed entries; the new segments are kept beside the
   14 shipped ones -> **19 segments, 48 MB, 59 ms to load all**.
3. **Mesa**: the pass-B runs used a workdir without `mesa/`, so Mesa kept generating NIR->MSL
   (81% of what was left: 296 ms of 365). One run with the shipped artefacts (so `run.sh` clones the
   Mesa cache into the data dir and extends it) and that cache copied back -> **19 -> 28 MB**.

The 30-pipeline burst, same route, same store, measured at each step (driver time and its layers):

| capture state | driver | NIR->MSL | MTLLibrary | pipeline state | archive misses |
|---|---|---|---|---|---|
| before (Metal cold, first launch) | 2 016.9 ms | 251.9 | 1 096.2 | 620.1 | 30 |
| before (Metal warm, next launch) | 329 ms | - | 194 | 123 | 30 |
| + AIR 678 + 3 segments | 389.6 ms | 260.8 | 22.0 | 9.7 | 30 |
| + extended Mesa cache | **135.2 ms** | **0** | 30.4 | 29.4 | **0** |

So the load hitch is 2.1 s on a first launch and 0.17 s afterwards, against 2.1 s / 0.33 s before the
top-up. The remaining 135 ms is the port's own translation (33 ms) plus Metal creating the library
and the pipeline state for pipelines the archive now answers for - the floor of a first draw of a new
state, not a compile storm.

**Caveat on this measurement:** the proof run's frame rate was unusable (19 fps, 1 068 frames over
50 ms all owned by IPC) because four other Hermes sessions were running on the machine; only the
compile numbers above are quoted from it. The steady-state numbers in
`HITCH_ATTRIBUTION_2026-10-22.md` come from the quiet runs.
