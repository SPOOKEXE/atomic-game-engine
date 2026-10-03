# HLSL source argument persistence

This profile targets Pixel Composer source revision
`b69eca232217360cf1502ef0223523d818606652`. The evidence is
`scripts/node_hlsl/node_hlsl.gml` (fixed five inputs, repeating name/type/value
triples and `refreshDynamicInput`), `scripts/node_value/node_value.gml`
(Generic_float storage), and `scripts/node_processor/node_processor.gml`
(`getInputsProcess`, `getInputSingle`).

| Source selector | Native socket | Saved source value |
|---|---|---|
| 0 Float | Scalar | Exact finite JSON number |
| 1 Int | Integer | Exact finite JSON number, including a fractional real |
| 2 Vec2 | Array | Two numeric components |
| 3 Vec3 | Array | Three numeric components |
| 4 Vec4 | Array | Four numeric components |
| 5 Mat3 | Array | Nine numeric components |
| 6 Mat4 | Array | Sixteen numeric components |
| 7 Sampler | Image | Raw numeric marker or source image link |
| 8 Colour | Colour | Packed numeric source colour |

Changing the selector changes the base NodeValue's type enum. It does not replace
that object with an Int prototype, so import does not round fractional Int
storage. Integer JSON leaves remain int64 values, including values beyond exact
double precision. Unsigned leaves above INT64_MAX and nonfinite values refuse.

Vectors and matrices also admit ordered outer processor rows with the same
component width in every row. Numeric Scalar and Integer leaves can coexist;
there is no recursive array or guessed matrix broadcasting. This preserves saved
processor transport. The source refresh operation separately checks the outer
array length and can reset it to zeros. Persistence does not claim that arbitrary
saved rows survive every source refresh event.

Native Colour edits save as source packed RGBA numbers. Native Vector2/3/4 edits
save as flat numeric tuples. Checked reimport compares that explicit canonical
projection while preserving the caller's native document. Existing raw markers,
compact and expanded keys, source animation flags, instance ownership and
explicit instance overrides retain their source representation.

Declaration typing requires a static authored selector. A native literal Junction
can be handled by the separately checked core route solver. A linked source node
input bypass or animated selector is not assumed static from its current value.
Sampler links and dynamic input bypasses retain the real source socket indices.

The tests use checked PXC archives and edit/save/reimport, including all nine
selectors, every byte value in each packed colour channel, raw fractional Int,
mixed numeric tuples, nested rows, instance overrides and atomic malformed-shape
refusal. GPU execution and official application acceptance are separate gates.
