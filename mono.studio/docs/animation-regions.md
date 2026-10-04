# Animation regions

Project animation regions own their authored bounds and source record identity.
Selection is transient editor state. Document replacement, Undo and Redo clear
selection; a region edit follows its selected row through the exact sort
permutation. Imported duplicate regions retain distinct source record ownership.

Create, settings, resize, remove and clear actions commit one staged document
through the existing history owner. Refusal preserves the document and redo
branch. Edited settings round frame fields to even; untouched signed and
fractional imported bounds retain their authored values.

Playback freezes the selected endpoints by value. Explicit source endpoints
have priority, followed by selected endpoints, then the project range. Legacy
native First/Last fields remain explicit until Clear source range opts into
missing source endpoints. Undo restores the prior native window.

Native playback retains the bounded fixed-tick cadence. Pingpong uses the pinned
source upper bounce max(0, last - 1) and lower bounce at zero, including when the
selected start differs from zero. This cadence has not been compared with a
licensed source runtime capture.

Selection changes cancel pending preview admission and invalidate completed
preview identity while retaining feedback, manual caches and providers. Frozen
source-cache observations carry project frame and resolved last endpoint
separately from a scoped node request clock. Binding these observations does
not establish completed cache-group loading within the selected interval.

Thirteen headless region cases cover the actual ImGui gestures, history refusal,
stale settings, endpoint precedence, frozen observations and native playback.
The joined CPU cohort also passes. Live Studio and device checks remain separate.
