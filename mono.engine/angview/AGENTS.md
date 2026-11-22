# angview - module invariants

L4, `shared` tier, one layer above `ang` so the edge runs downward. Links
`core` and `ang`. Nothing here includes `imgui.h` or any device header.

## The interaction state machine lives here

Pan, zoom, selection, hover, node drags, wire drags, box select, touch and the
add-node palette are decided in `View`, from synthetic input in view-space
pixels. A painter forwards input and draws `Paint()`; it never keeps its own
drag state or computes geometry. Tests drive interactions without a renderer.

## Edits go through the graph

Every change is `Graph::Apply`. A drag previews in the layout and applies one
batch on release, so one gesture is one undo record. Link legality comes from
`ang::CheckNewLink`; do not add a second compatibility check here.

## Paint only on change

`Paint()` rebuilds the draw list only after a visible change: input that moved
something, a graph revision, a theme or run feedback. Nothing animates. Themes
are plain data; presets are `Dark` (default, true black, white text),
`HighContrast`, `Blueprint` and `Minimal`.
