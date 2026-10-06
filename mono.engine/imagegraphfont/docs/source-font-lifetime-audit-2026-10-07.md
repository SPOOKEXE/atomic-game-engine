# source font lifetime audit, 2026-10-07

## source behavior

the pinned upstream tree is commit `b69eca232217360cf1502ef0223523d818606652`.
in `scripts/node_text/node_text.gml`, `generateFont` caches GameMaker handles
globally by `path|size|aa|sdf`. it checks playback and file existence before
the cache. no reset was found in that pinned tree.

`file_exists_empty` in `scripts/file_functions/file_functions.gml` checks for a
nonempty path and file existence. it does not inspect file length.

## native behavior

[GraphFontHost.cpp](../src/GraphFontHost.cpp) requires an exact node, path, and
role grant plus policy approval. each observation reads the current bounded
file bytes. [GraphFontInputs.cpp](../src/GraphFontInputs.cpp) keeps its
provider with the configuration revision. it does not cache raw font bytes.

[FontTextFontState.cpp](../../imagegraph/src/FontTextFontState.cpp) skips the
provider while playing. [SourceFontObservationContext.hpp](../../imagegraph/src/SourceFontObservationContext.hpp)
uses exact source receipts without calling the provider.

source handles and native raw-byte retention do not promise the same lifetime.
do not add a cache until admission is bounded and revision or recording
semantics are defined.

## mismatch and evidence

the concrete mismatch to test in a licensed build is overwriting an existing
font at the same path and settings while paused. source code reuses its handle;
native observation reads the new bytes. this audit makes no font completion or
exact parity claim.

the recorded prior full batch in
`.cache/build/release-tests/evidence/composer-continuation-2026-10-05/composer-gpu-expanded-batch1-results.json`
reports the font row at 37 cases,
1686 assertions, exit 0. the current `test_imagegraphfont` sha256 still matches
that row: `93cd8d70799454ad3d95b4746a472edd2b76f815b6e1ea13329dcd596ee8e765`.
this audit adds no tests and does not rerun them.

[GraphFontHost.cpp tests](../tests/GraphFontHost.cpp) cover held playback,
missing files and exact receipt replay. those cases do not prove the overwrite
and recreate behavior listed below.

## licensed follow-up

- overwrite at the same path and settings
- delete, then recreate the file
- change size, antialiasing, or sdf settings
- change the fallback font
- compare unicode and platform raster output
