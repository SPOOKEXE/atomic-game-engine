# Prepared PXCX thumbnail saves

`Save PXCX` preserves source thumbnail bytes. `Save PXCX with preview` explicitly
replaces THMB with the completed selected image preview. It borrows raw pixels
from the bounded preview cache, without evaluating the graph or waiting for a
renderer fence during save. The image must belong to the current authoring and
captured-input revisions, complete output binding, signed fractional frame and
playback observation. Pending, stale, evicted and non-image previews refuse the
opt-in action. Input revision changes invalidate the preview cache before reuse.
The caller owns the revisions and attests the immutable graph generation; the
save API does not independently execute or fingerprint graph observations.

The original imported archive remains the authoring and undo baseline, including
source input/key identities and unknown records. Successful opt-in publication
adopts a separate checked archive and embedded reference image. Later ordinary
saves use its thumbnail after validating source graph edits against the original
baseline. Undo then saving restores original graph records while retaining the
explicitly regenerated thumbnail. Opening another project clears the published
fact and preview identity. Ordinary saves before the first opt-in publication
retain their existing source-preserving path.

All projection serialization, thumbnail conversion and archive readback finish
before creating the temporary file. The display reference and adoption identity
are prepared before atomic replacement. Refusal or publication failure preserves
both the prior destination and published state, and removes the temporary file.
The source archive and authored document are never modified by saving.

`SavePxcxProjectionAndAdopt` admits retained archive backing, previous reference
pixels, identity backing and phase staging against `maxBytes`. Thumbnail
conversion uses the bounded engine export helper. Projection and bake decoding
retain their existing independent format/workspace limits. This is a payload
admission policy, not a claim that vendor allocations or the whole Studio process
fit inside one heap quota. The native conversion profile is 256-square nearest
sampling at destination pixel centers, using source center-cover crop geometry
and straight RGBA8 conversion. GameMaker GPU filtering and preference-dependent
thumbnail sizes remain unverified; this action does not claim application parity.

Headless workflows compile and evaluate a real imported Solid image graph, then
save/reload its immutable preview. They check metadata and original archive
retention, edit/undo/subsequent-save behavior, each identity component, failed
conversion/budget admission and failed final replacement. GUI wiring is checked
by compilation; no live Studio or GPU verification is claimed by those tests.
