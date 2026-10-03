# Source Weight Adjust

Joined release65 CPU validation passes the complete core suite (2,098 cases, 1,826,683 assertions), including the new weight and retained metadata cases. The public layout changes and recursive retained-byte accounting were rebuilt together.

## Source contract

The source is `scripts/node_path_weight_adjust/node_path_weight_adjust.gml` in Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. `source-path-weight-adjust-sha256.json` records the full wrapper, curveMap, 3D producers, bbox, transform, camera and official HTML5 math hashes read for this implementation. The math reference is the [official HTML5 runtime](https://github.com/YoYoGames/GameMaker-HTML5/blob/develop/scripts/functions/Function_Maths.js). Desktop licensed runner equivalence has not been captured.

| Source input | Native port | Behavior |
| --- | --- | --- |
| 0 Path | path | A planar or spatial path, retained as owned typed geometry. Catalogue noone produces the source zero point with default weight. |
| 7 Loop | loop | Direction probes use pfract(ratio±.001) when true, otherwise clamp to [0,.999]. |
| 4 Adjust Type | adjust_type | Constant, Curve, Direction. |
| 1 Apply Mode | apply_mode | Additive clamps the sum to zero; Multiplicative and Override preserve negative finite results. |
| 2 Value | value | Constant amount. |
| 3 Curve | curve | Source curveMap samples TOTAL_FRAMES+1 entries at tolerance .00001, even when its mode is inactive. |
| 6 Direction Shift | direction_shift | Source point_direction and angle_difference, including six-decimal HTML5 direction rounding. |
| 5 Value Range | value_range | Linear endpoints for curve or normalized absolute direction difference. Reversed/signed ranges remain signed. |

The wrapper derives from Node, so it does not expose Path.getTangent. A downstream Shift therefore follows its source fallback tangent cadence. It forwards line count, segment count, length, accumulated length and boundary. getPointDistance divides by getLength() for line zero while forwarding the requested line to getPointRatio.

Direction mode samples the original ratio for output XY, then overwrites temp_p at the backward and forward probes. Additive and multiplicative modes use the forward probe's weight. This source behavior is preserved rather than replacing it with the original point weight.

The source output is a __vec2P. Spatial children keep their source length math and weight, while output position keeps XY. Camera depth weight is delegated by the existing spatial runtime. No geometry is flattened into approximate anchors.

## Owned spatial metadata

A Weight operation owns one spatial child mutually exclusively with its planar input vector. The existing PathData3D allocation carries the shape constructor's exact XY bbox, because shape bbox is position±half size and can differ from sampled extrema. Empty shape updates retain the prior bbox and accumulated length table alongside the already retained length and segment count. Fresh empty constructors use the source BoundingBox noone (-4) coordinates.

Raw Path 3D bounds use the same resolution points as source updateLength. Its known previous-Z length quirk remains unchanged. Transform Path 3D bounds apply anchor/scale/position without quaternion rotation, as its source method does. Camera delegates bounds unchanged. Reverse/Trim/Array select child metadata without replacing it with sampled extrema.

A private bridge owns one preadmitted spatial runtime and breaks the private Path.hpp/Path3D.hpp include cycle. It does not rebuild the runtime for every sample. Borrowed caller payloads remain immutable; all metadata additions are owned typed fields.

Legacy/manual source polyline payloads may omit original bounds or stale accumulated metadata. Their geometry is preserved by the native decoder. Weight diagnoses missing required bounds; accumulated metadata access diagnoses omitted prior tables. It never fabricates missing source metadata from point extrema or zeros.

## Bounds and persistence

The curve table, owned input tree, numeric controls and cached metadata share existing recursive depth/element bounds. Constructor work preflights the curve table and recursively counted source chord/anchor preparation against the existing bounded work profile. Output clone/table storage is admitted before allocations. The bridge admits its runtime owner before construction; the existing recursive runtimes admit child/workspace storage. Cache metadata growth admits output and retained copies before copying.

The operation appends WeightAdjust after Shift, preserving existing operation indices and all Value/ElementValue alternatives. Native `po weight_adjust` stores exact controls, its sampled curve table, and the typed optional spatial child through the existing native path codec. Source polyline metadata uses an optional tagged block, accepting previous records that omitted it. Parser tables are bounded and admitted before growth. Equality includes durable geometry and tables but continues excluding evaluation-local Shift memo identities. The shared visitor traverses the new owned child so public outputs and saved payloads never export those identities.

Measured old/new layouts: SourcePathData2D 760/832; PathData3D 168/232; SourcePolylineEmptyCache3D 16/40. Path2D remains72, PathValue3D8, Value88, ElementValue80. These are header layout measurements, rebuilt coherently across the joined products; they are not runtime allocation measurements. Dynamic cached table bytes are counted separately from sizeof storage.

## Prepared validation

Fourteen compiled-graph cases pass in the joined CPU build. They cover signed constant modes, nonlinear TOTAL_FRAMES curve resolution, forward-probe weight, direction angles, native persistence, source noone, bounded table/byte refusal preserving outputs, payload invalidity, array selection, spatial shape XY/bbox, nested Shift identity stripping, raw spatial source length math, and empty spatial metadata retention/defaults. Existing source-spatial-shape clone-byte expectations include the newly owned accumulated table.

No measured timing, GPU parity or sanitizer acceptance is claimed. The executor has its normal ENGINE_PROFILE boundary. Joined CPU checks cover the new runtime owner, changed public layout and recursive retained payload bytes.
