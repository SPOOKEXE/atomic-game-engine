# Pin CPU passthrough contract

Pinned source: Pixel Composer `b69eca232217360cf1502ef0223523d818606652`,
`scripts/node_pin/node_pin.gml`, 5,319 bytes, SHA-256
`e74a3ba74e0e010d67aa6cfb23b7a0bc241b10b729d589b8634b0c840ec12c3a`.

`Node_Pin` inherits plain `Node`, uses `doUpdateLite`, and creates Any In at
source slot 0 with literal numeric default 0, and Any Out at source slot 0.
Its update writes `outputs[0].setValue(inputs[0].getValue())`. The generated
native port names are exactly `in` and `out`. It does not batch whole input
arrays, transform values, flatten image sequences or retain temporal state.

Three controls are editor display settings: Label Position (T/B/L/R, default
0), Label Scale (default 1), and Label Color (source theme `COLORS._main_text`).
`onValueUpdate` caches them for `drawNode`; the data update never reads them.
The CPU route therefore ignores their display effect while preserving normal
compiler validation of declared controls. It does not invent a numeric label
color default from the unobserved theme. Drawing, hover and source mutable
junction colors remain editor capabilities, with no device parity claim.

Pin copies the complete resolved payload into the owned evaluator result.
Native typed leaves retain their representation, including int64 values above
2^53; Struct, Undefined and heterogeneous nested arrays are transported as
runtime values. Native document round trips preserve the authored producer
and links. Runtime-only values such as Undefined are produced by real upstream
nodes rather than serialized as unsupported authored defaults.

Real images retain dimensions, format, bytes and hash. Image arrays retain
complete nested shape and independent owned image buffers. Embedded Atlas
surfaces do not replace the Atlas carrier. Copies do not claim source mutable
handle alias identity or source handle numbers; Pin performs no comparison or
interpretation of those handles.

Before copying a Value, existing recursive runtime validation and clone-byte
accounting admits the entire tree. Images use checked layout and shared output
admission. Image arrays validate depth, element count, referenced indices,
metadata and every owned image payload before growth. The bounded traversal
visits at most the existing 4,096 array elements and admitted image list; it
uses no ambient host, randomness or external callback. Failed evaluation
preserves the caller's prior result. The meaningful evaluator unit is profiled
as `imagegraph.pin`; no measured performance conclusion is claimed.

Ten actual Compile/Evaluate graph cases cover zero default, all four label
positions, typed Any leaves, whole nested values and downstream access,
Struct/Undefined, upstream linear key animation, typed images, nested image
arrays, byte refusal/atomic output preservation and unknown-control rejection.
All ten cases pass in the joined release65 CPU build. Sanitizers, measured
profiling and licensed source-runner comparison remain open.
