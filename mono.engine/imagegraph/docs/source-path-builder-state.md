# Path Builder source state

This is an audit of the pinned Pixel Composer source for `Node_Path_Builder`.
It records the source contract and current native representation gaps. It does
not claim a native implementation, source fix, or licensed executable parity.

## Pinned source

- Commit: `b69eca232217360cf1502ef0223523d818606652`
- Node: `scripts/node_path_builder/node_path_builder.gml`
- Native graph node id: `pc.path_builder`
- Inputs: `Point array` defaults to `[]`; `Loop` defaults to `false`; dynamic `Points` inputs have array depth 2.
- Output: `Path`, set to the node itself after source update succeeds.

## Source array and anchor behavior

The node branches on the primary array depth. Depth 1 is normalized as one
point path and accepts dynamic point inputs at depth 1. Depth 2 is normalized as
one path containing points and accepts dynamic inputs at depth 2 as additional
paths. Depth 3 is already grouped as multiple paths and is used directly.
Other depths leave the output unset. The empty default follows the source's
single-path normalization and yields one zero-padded anchor in this source
profile.

Each point anchor copies seven numeric fields by indices 0 through 6. Missing
fields are filled with zero through `array_safe_get`. The value at index 6 is
copied as authored numeric data but is not read by position or weight sampling.
`getPointDistance` writes position into the caller's output object and carries
through its existing weight value, which defaults to 1 for a new `__vec2P`. The seven fields
are authored point data, not an integer path or point index.

`getPointRatio` wraps the ratio with `frac` for looping paths. For non-looping
paths it clamps to the inclusive source expression range 0 through 0.99, so a
ratio of 1 samples at 0.99. Distance sampling does not apply that ratio clamp;
its segment interpolation parameter is computed from distance divided by
segment length and can extrapolate for distances beyond the selected segment.
Distance cache keys use the path index and distance formatted to six decimal
places. The cache is cleared when the source node updates.

## Open source question

`updateLength` declares an outer loop variable named `p`, then declares another
`p` in the inner `var l = 0, _ox = 0, _oy = 0, _nx = 0, _ny = 0, p = 0` statement.
The official [GameMaker local variable documentation](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Overview/Variables/Local_Variables.htm)
says a `var` local belongs to its function or event. It does not establish how
the pinned compiler treats this repeated declaration in the nested loop. If the
inner declaration resets the same function-local `p`, the outer line loop may
not advance. That outcome is an inference, not a verified compiler result or
licensed runtime observation. Pinned compiler behavior remains unknown.

## Native representation gap

The current generic IO reader accepts flat scalar arrays for the point input
and rejects grouped depth-2 and depth-3 arrays. The ordinary native
`PathRuntime` representation cannot express this source node's grouped point
arrays as-is. These limits describe the current adapter and runtime model; they
do not establish how the licensed executable behaves for every malformed or
edge-case input.

No exact native Path Builder executor, source correction, or executable parity
result is claimed here. Validation remains pending.
