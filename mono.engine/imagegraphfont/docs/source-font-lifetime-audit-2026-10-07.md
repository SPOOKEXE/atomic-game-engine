# source font lifetime audit, 2026-10-07

## source behavior

the pinned upstream tree is commit `b69eca232217360cf1502ef0223523d818606652`.
in `scripts/node_text/node_text.gml`, `generateFont` caches GameMaker handles
globally by `path|size|aa|sdf`. it checks playback and file existence before
the cache. no reset was found in that pinned tree.

`file_exists_empty` in `scripts/file_functions/file_functions.gml` checks for a
nonempty path and file existence. it does not inspect file length.

## native behavior

[GraphFontHost.cpp](../src/GraphFontHost.cpp) requires an exact node, path and
role grant plus policy approval before every observation. grug check file
existence before looking up retained bytes by resolved path, pixel size,
antialias and sdf settings. cache holds at most 64 entries and 16 MiB of
raw bytes plus key storage. full cache refuses growth without eviction.

successful observations keep bytes through overwrite and zero-byte replacement.
deleting the file reports absent without removing its key. recreating it reuses
retained bytes. matching granted nodes and roles share a key, while requested
characters and measurements are decoded anew from the retained file.

[GraphFontInputs.cpp](../src/GraphFontInputs.cpp) keeps the provider with its
configuration revision. successful Replace starts a fresh lifetime. Bind and
Replace count live cached storage; core charges provider residency at evaluation
start and transfers admitted growth before checking returned observations.

[FontTextFontState.cpp](../../imagegraph/src/FontTextFontState.cpp) skips the
provider while playing. [SourceFontObservationContext.hpp](../../imagegraph/src/SourceFontObservationContext.hpp)
uses exact source receipts without calling the provider.

native revision lifetime differs from the source global GameMaker handle cache.
coverage and distance output remain named native profiles. grug make no licensed
raster or global handle lifetime parity claim.

## validation

[validation evidence](../../../docs/pixel-composer-m0/native-font-lifetime-validation-2026-10-07.json)
records both joined check batches and their repairs. final 20 checks pass:
5,274 C++ cases plus startup, 101 Python tests and 16 CLI checks. render includes
31 cases under the approved offscreen Vulkan setup. sources and binaries stay
unchanged throughout each joined batch.

[GraphFontHost.cpp tests](../tests/GraphFontHost.cpp) cover overwrite, zero-byte
replacement, deletion, recreation, granted sharing, new characters, changed
control keys, both cache limits, refused decode and revision replacement.
[font workload tests](../tests/FontBoundaryWorkloads.cpp) check cold reads and
warm hits beside real BDF, Inter, SDF and Unicode output.

existing font boundary profile passes seven workloads, each with eight warmup
calls and five samples. every font workload reads once then reports 12 hits;
phase scopes, heap attribution, byte counts and output fingerprints pass. grug
keep benchmark output on stdout and make no speedup claim.

## licensed follow-up

- overwrite at the same path and settings
- delete, then recreate the file
- change size, antialiasing, or sdf settings
- change the fallback font
- compare unicode and platform raster output
