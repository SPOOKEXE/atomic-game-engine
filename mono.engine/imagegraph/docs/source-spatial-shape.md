# Source Shape Path 3D

`pc.path_shape_3_d` implements the pinned plain `Node_Path_Shape_3D` value producer. This is a bounded CPU geometry and sampling profile. Licensed GameMaker runtime equivalence and GPU preview equivalence have not been measured.

The source pin is `b69eca232217360cf1502ef0223523d818606652`. `source-spatial-shape-hashes.json` records the exact constructor and helper bytes used for this implementation.

| Control | Source default | Use |
| --- | --- | --- |
| Position | `(0,0,0)` | Center before absolute axis permutation |
| Half Size | `(.5,.5,.5)` | XY radii; sphere Z extent |
| Shape | `0` | Raw physical scroll index |
| Up Axis | `2`, Z | `0` swaps absolute X/Z; `1` swaps absolute Y/Z |
| Rotation | `0` degrees | XY rotation before axis permutation |
| Sides | `6` | Polygon and star, minimum three |
| Revolution | `4` | Spring, sphere and spiral point count multiplier |
| Pitch | `.2` | Spring Z rise and spiral radial growth |
| Inner Radius | `.5` | Star inner radius multiplier, no invented clamp |
| Reverse | `false` | Spiral pitch replacement and rotation offset |
| Resolution | `64` | Ellipse and open-shape detail |

Shape indices are physical widget positions: Rectangle `0`, Ellipse `1`, Regular Polygon `2`, separator `3`, Star `4`, separator `5`, Spring `6`, Spring Sphere `7`, Spiral `8`. The scroll item sprite index is separate from the selector value. The two separators have no source shape object and refuse execution.

This constructor inherits plain `Node`, not `Node_Processor`. It receives whole getter results and does not schedule array rows. Position and Half Size support numeric scalar repetition, tuple zero padding/truncation to three components, and flat general numeric tuples. A scalar surface projects to `(width,height,0)` before unit handling; a whole surface sequence projects to `(1,1,0)`. Nested coordinate arrays and arrays used by scalar arithmetic refuse execution. Unused detail controls are not interpreted by the kernel. Path objects are not silently sampled: the source Vec3 getter repeats that object rather than providing numeric coordinates.

Ellipse and regular polygon Y use the source negative sine convention. A star alternates outer and inner radii. A spring advances Z by `i/resolution*pitch`. A sphere uses `sqrt(1-(2*i/steps-1)^2)` and interpolates Z between the two half-size extents. A spiral grows by `i/resolution*pitch`; Reverse replaces pitch with `1/abs(revolution)` and subtracts the full raw revolution angle. Positive fractional point counts preserve the loop's effective ceiling and the original nonintegral count in sphere and reverse arithmetic. Rotation reproduces the source `point_rotate(-rotation)` shortcuts for zero and negative 180 degrees. Axis swapping acts on absolute coordinates.

Closed shapes append a literal first point. Their length is the sum of adjacent Euclidean distances, including Z. The source length array contains one entry per stored point, with a trailing zero. The existing `Node_Path_3D` sampler's previous-Z length behavior is preserved for legacy paths and is not reused for this class. Ratio uses signed fractional parts and remainder rather than positive wrapping or open-path clamping. A ratio of one samples the first point; negative ratios can extrapolate. This class has no source `getPointSegment` method.

## Empty updates and replay

The source constructor initializes empty points, empty length arrays and length zero. Update sets the new shape's loop state and replaces points before `if(array_empty(points)) return`. That return leaves prior lengths, accumulated lengths and total unchanged. Retaining the previous geometry would therefore be incorrect.

The native carrier records this class with `SourcePolyline` and represents an empty update's observable cached length and segment count with the optional `SourceEmptyCache`. Its points remain empty. Positive Resolution with Revolution zero reaches this branch for the open shapes without dividing by zero, except Reverse Spiral, which divides by zero revolution. Zero Resolution evaluates division before the empty return and remains an explicit arithmetic boundary. Fresh empty state samples the source default zero point. Empty state with retained length entries cannot sample safely because the source indexes `points[(i+0)%np]` with `np=0`. Consumers refuse that geometry atomically. A retained zero total still has length entries and remains distinct from fresh empty state.

The existing caller-owned `DataReplayState` stores one latest owned path value per node/processor identity. Empty updates borrow its prior length/count; nonempty updates replace that cache. There is no hidden global state or retained prior drawable geometry. The temporal cone identifies this producer as a data owner. Existing host revision, observation and explicit `Clear()` contracts control its lifetime. Clearing the caller's replay owner resets the constructor state. Native stateful evaluation publishes the output and replay update together; failed budgets leave both unchanged.

The compact cache does not expose the source UI preview, BoundingBox or accumulated-length-array methods as new public APIs. Existing implemented 3D consumers use length and ratio sampling. Future consumers requiring the accumulated array must add its complete representation rather than derive it from the cached total.

## Ownership and native codec

`PathValue3D` remains an eight-byte owned payload; `Value` remains 88 bytes and `ElementValue` remains 80 bytes. The source marker uses existing boolean padding. `SourcePolylineEmptyCache3D` is 16 bytes and its optional storage is 24 bytes. `PathData3D` grows from 144 to 168 bytes, measured with GCC and Clang. Existing payload accounting uses actual `sizeof(PathData3D)` and retained vector capacity, including recursive wrappers and replay clones.

The native version-nine codec writes the distinct `p3s` tag with named `source_polyline` and `source_empty_cache` fields. Legacy `p3` and `p3o` grammars remain unchanged. Decoding checks fixed field names without allocating a token, marker/presence values, finite nonnegative cached length, nonzero bounded segment count, source class exclusivity, zero control handles and the literal closed endpoint. Cached empty metadata requires empty anchors. Invalid input leaves the destination document unchanged.

Geometry count, source class validation, replay validation workspace, both output/state owned payloads, node identity and update containers are admitted before allocation. Capacity increases receive additional charges. Point count is at most `Limits::MaximumPathAnchors`; one closed endpoint consumes one of those slots. Coordinate, angular and Euclidean overflow refuse publication. A zero-length populated path retains its source geometry but its division-by-zero sampler is unavailable. C++ double arithmetic, standard trigonometric functions and ordinary comparisons form the native deterministic profile; no licensed transcendental or exceptional arithmetic parity is claimed.

## Verification

The joined release64 gate passes 15 graph cases covering all seven shapes, geometry goldens, axis/rotation, length and signed ratios, native codec and malformed fields, keyframes/save, general tuples and surface getters, transform/sample/instancer consumers, empty transitions and atomic refusal. Standalone GCC/Clang layout checks are retained. Licensed source execution, source GPU preview and representative profiling remain open.
