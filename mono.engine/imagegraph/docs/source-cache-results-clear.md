# Cache Results Clear native ownership

The pinned source button recursively frees surface handles, keeps its cycle
array length and positions, resets surfaceIndex to zero and clears input/frame
cache flags. The callback does not invoke update. Exact files and hashes are in
`native-cache-results-clear-policy.json`.

The native action runs against the prepared owner and exact durable node ID,
with unchanged authored/input revisions. It retains each processor row's count
and signed observation metadata, replacing owned pixels with a private freed
slot record. The flat cleared array is uniformly typed Struct; existing flat
Any arrays cannot carry mixed native leaf types. It applies to both current DataReplay and the pre-frame checkpoint.
All other replay journals, provider sessions, feedback generations and playback
clock remain unchanged. The document is unchanged, so Clear is not an undoable
authored edit.

A bounded reverse dependency walk marks only affected outputs and captured
inputs unavailable. Studio invalidates those output indices in its presentation
cache across retained frames. Unaffected outputs and solver state remain usable.
At the same signed clock a dependent preview receives a diagnostic requiring a
fresh observation. This explicit native scheduler avoids rerunning unrelated
processors after a button click; source scheduler parity is not claimed.

A distinct clock or input generation performs normal compiled evaluation using the retained slot
shape. Seek/restart reconstruction overlays the current cleared cache once
before its first replay step. This overlay survives authored control revisions
for the same durable Cache Results node ID/type. A successful revision removing
the node or changing its type retires the overlay; readding that ID creates a
new source instance. Whole-owner Clear also retires it. A rigid refresh overlays it onto the immutable
pre-frame journal. Neither path can restore freed pixels. Source first-frame
rotation and Amount shrink/growth still determine the next count/current slot.
A new one-slot surface can recover when every output position is valid. Output
with remaining freed positions receives UnsupportedExecution and leaves the
cleared journal unchanged. No transparent pixels, numeric handles or source
undefined values are substituted. This refusal does not complete source
invalid-handle output/update transport.

Before any dependency/name walk, the action admits conservative complete
node-ID lookup and name-comparison work, including unique-table insertion and
retirement. Overlay membership is admitted before counting matching rows, then
the remaining document/target scans are admitted before copying. The native
limit is 64 million character/visit work units; a large valid graph can refuse
this action without retiring slots. This is an explicit bounded CPU profile,
not a source scheduler work limit.

Before cloning, the action admits old and replacement Data journals, copied
metadata/name tables, validation workspace and any retained prior output.
Failure preserves old journals, output pixels, checkpoint and invalidation
state. The clear helper's ENGINE_PROFILE span covers its real bounded clone and
slot retirement work. No shipped-cost or heap-attribution measurement is claimed.

Headless fixtures use actual compiled Cache Results/Sequence graphs with two
scopes, explicit feedback bindings, tight overlap budgets,
wrong/stale selections, unvisited processor rows, Amount edits, node
removal/readdition and type replacement, and restart/seek behavior. The
direct compiled kernel fixtures cover signed fractional source observations.
Cache Results host seek reconstruction rejects negative frames; feedback bindings
also require integer frames. The host fixture asserts that refusal preserves
the cleared journal, then verifies an admitted integer backward seek. The
physics fixture uses the actual native RigidProvider and rigid actor graph to
exercise a current-frame checkpoint refresh. Studio calls the same free action
adapter as the inspector button; its fixture checks per-output cache eviction
and document preservation. Joined release65 core, physics and headless Studio action checks pass. Live UI
drawing, sanitizers and measured profiling remain open.
