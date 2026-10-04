# Saved source timeline bounds

`TimelineSettings.SourceBounds` keeps source authoring endpoints independently
of `Frames`. Each endpoint distinguishes missing, JSON null and explicit saved
one-based coordinates. Explicit signed and fractional values use canonical
`FrameTime`, bounded by `MaximumTick + 1`. This is the native supported numeric
profile. Larger or nonnumeric source fields remain retained opaque in the PXC
adapter.

`First` and `Last` remain the bounded integral native playback projection. They
represent exact source getters only when both getter results form an integral
ordered window inside the total. Other source states use the full native UI
window without changing the saved endpoints. Code needing source semantics uses
`SourceTimelineFirstFrame` and `SourceTimelineLastFrame`, which subtract one from
an explicit endpoint and otherwise use the transient selected region or total
fallback. An endpoint beyond total is not automatically an invalid export
clock. An export sink validates its own supported range and work limits.

`ProjectSourceTimelineWindow` does not normalize source fields.
`NormalizeSourceTimelineBounds` implements the pinned animation-controller
step-entry transition, including while paused. It follows the source truthy
endpoint gate, clears equal values to null, and otherwise applies the source
min/max expressions. Import, Match Animation Length and ordinary save do not
pretend that this later step has occurred. Match Animation Length changes only
the total and its derived native window.

Native format 9 writes one optional `source_timeline_bounds` record after the
existing timeline record. Missing and null endpoints have no numeric tokens;
explicit endpoints use tick, subframe and sign. Earlier documents with no record
read as before. Duplicate, misplaced, malformed, noncanonical and pre-v9 records
refuse atomically. Fixed storage is included in its containing document/context
size, and the retained-payload validator checks endpoint canonicality. Pending
host timeline equality includes these fields before reusing an observed effect.

Source provenance is commit b69eca232217360cf1502ef0223523d818606652,
`animation_controller.gml` getFirstFrame/getLastFrame/step and
`project_data.gml` serialize. The pinned deserialize writes saved range fields
back into a temporary map instead of restoring them to the animator. Native
saved-authoring preservation is therefore separate from licensed application
playback acceptance; that acceptance has not been run.
