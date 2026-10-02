# Pure palette source CPU profile

The four pure palette nodes use ordinary source processor rows and immutable typed
colour arrays. Flat general source array leaves may carry native colours or
integral packed 32-bit colours. Nested processor rows remain separate. Invalid
packed conversions and unresolved non-array inputs refuse execution explicitly.
Result palette storage, durable output text and temporary owned work arrays are
admitted through the evaluation ledger before allocation.

`pc.palette` sorts Trim Range endpoints, clamps them to [0,1], multiplies by input
length, floors both endpoints and copies the resulting exclusive interval.

`pc.palette_sort` preserves actual sparse source enum indices: 0 Brightness,
2 HSV, 3 SHV, 4 VHS, 6 RGB, 7 GBR, 8 BRG, 10 Custom. Separator indices 1, 5 and 9
leave original order unchanged before optional reversal. RGB compares weighted descending byte keys. HSV retains fractional builtin
getter values in its weighted descending keys. Brightness uses .299 Red + .587 Green + .114 Blue.
Custom strings assign base-256 priority per UTF-8 character, uppercase descending
and lowercase ascending through `256-component`. Unknown characters preserve
the source zero-component rule and its ordinal-based direction. Custom L uses
`colorBrightness(false)` with .224 Blue, deliberately distinct from .114 in the
ordinary brightness comparator. Keys are limited to 127 codepoints so numeric
priority accumulation stays finite.

`pc.palette_replace` compares normalized RGB Euclidean distance, accepts threshold
equality, and keeps the first equal-distance candidate. It substitutes the whole
target colour, including alpha. The pinned call passes an overflow enum as a
fourth argument to `array_safe_get_fast`, but that helper takes only array, index
and default. Short or empty target palettes therefore preserve original colour;
no looping is performed.

`pc.palette_shrink` returns the original palette when its length is at most Amount.
Histogram interpolates independent RGB or HSV bounds and emits opaque colours.
K-mean initializes centres with uniform bounds or exact recorded `random(1)` draws,
runs exactly ten nearest-assignment/mean passes, resets empty clusters to zero,
then selects original palette representatives, sorts original indices and removes
duplicates. Original alpha survives representative selection. Integer Shift uses
the pinned signed `frac` branch when nonzero; Shift zero uses `i/(size-1)`.
A singular one-centre Histogram or unshifted uniform initialization cannot invent
a finite colour: it requires an exact bounded host output receipt. Source builtin
draw captures bind the authored node, signed frame, processor row and resolved
controls, and verify operation, [0,1) bounds, exact count and every observed result.
The evaluator reads no ambient random generator.

Copied and selected leaves retain their original packed numeric or Colour identity,
including signed integer -1 and general array storage. Decoded colours are used
only for distance and sorting. RGB histogram channels use the project's half-even
make_color_rgba path. HSV getters preserve fractional values, and HSV histogram
channels use the official HTML5 make_color_hsv half-up conversion.

The runtime reference is [YoYoGames Function_Graphics.js](https://raw.githubusercontent.com/YoYoGames/GameMaker-HTML5/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/functions/Function_Graphics.js)
at commit `60e51be51ce7f3d52025ef18106cf172b8e22a00`, specifically Color_RGBtoHSV
and make_color_hsv. This proves that runtime's formulas; licensed desktop runtime
parity has not been established. Equal-key sorting uses stable native scan order.
No GPU or source-runner bitwise parity is claimed. Singular arithmetic receipts
are supported only for a single processor row: HostNodeCapture lacks row identity,
so batched singular rows refuse explicitly at Amount.

Replacement and clustering have an explicit 2^24
comparison cap across the entire processor batch; division-based admission
avoids overflow. Clustering checks its eleven assignment/representative scans
before owned workspace growth. Invalid UTF-8, oversized custom keys, malformed
observations and workload refusal leave caller results unchanged.

Pinned source commit: `b69eca232217360cf1502ef0223523d818606652`.

| Source | SHA-256 |
| --- | --- |
| `scripts/node_palette/node_palette.gml` | `53c7d2d5db7ffd2d607d88a42e4ce286f9ad4ddd513ef7d7a3845ada69a24b97` |
| `scripts/node_palette_sort/node_palette_sort.gml` | `0e69f3c37e280496016750defe74a37bf82251f8b373a2831bf6cd4f482a2b12` |
| `scripts/node_palette_replace/node_palette_replace.gml` | `92a507df55ebd1365160870a74b7616b5c703bc4dbf0fc3f6882e2e02605dec8` |
| `scripts/node_palette_shrink/node_palette_shrink.gml` | `3eb2cfe7dc30a051aecb6560003dc23fd4ee9b0d2b8d250fbde0a010c3ebe762` |
| `scripts/color_function/color_function.gml` | `cbe9a62a8b56257756bd3fecf1c7eb9797eda9695d34b0b7b390d0bda0120177` |
| `scripts/array_functions/array_functions.gml` | `02b928bd2c3e0c94192aec6e0ccd3fcac0734e0a5bb1ad588117fde674018581` |

The focused suite covers actual compiled graphs, trim boundaries, persistence,
sparse and custom sorting goldens, the brightness coefficient difference,
threshold equality and short-target fallback, histogram RGB/HSV goldens,
ten-pass representative selection, recorded random calls, singular arithmetic
receipts, nested processor rows, finite custom keys and comparison-budget
failure atomicity. The joined development and release core suites each passed 1,343 cases and
1,812,127 assertions, including all fourteen Palette cases.
