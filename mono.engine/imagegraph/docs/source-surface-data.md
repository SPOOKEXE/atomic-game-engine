# Surface readback source CPU profile

The four nodes consume caller-owned immutable source surfaces and publish bounded
CPU data. No device readback, file access, ambient RNG or host output is invented.
Controls use ordinary source processor selection and existing unit resolution.
Output element counts and bytes are admitted before vector growth. Word traversal
and sampler work share a fixed 2^24 cap across all processor rows, with
overflow-safe division before source output growth or traversal. Word admission
accounts for two scans per row. Failed evaluation leaves the caller output intact.

Pixel Extract reads exactly width*height consecutive little-endian u32 words.
This is raw storage traversal even for float RGBA formats, rather than logical
pixel colour conversion. Output packed words retain nonnegative integer identity.
Skip None preserves all words; Empty skips zero words. The native Black profile
masks RGB before comparison. The pinned Black predicate lacks parentheses around
`c & 0x00FFFFFF != 0`; [the official expression manual](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Overview/Expressions_And_Operators.htm)
warns target compilers can group mixed operations differently. Actual desktop
predicate parity remains unverified. Insufficient surface bytes for all source
u32 reads produce a named refusal, rather than fabricated buffer-overrun values.

Find Pixel scans the same raw u32 traversal in row-major coordinate order. It
skips transparent words only when Include Alpha is false. RGB distance is the
mean absolute channel difference in normalized units, with inclusive tolerance.
Alpha comparison is separately inclusive when enabled. First match is a Vector2;
no match is {-1,-1}. Find All produces a bounded Vector2 array, including an empty
array when there are no matches.

Surface To Points supports exactly the source's RGBA8, RGBA16F, RGBA32F, R16F and
R32F branches. RGBA reads Red/Green and skips Blue/Alpha. Single-channel floats
consume consecutive sample pairs and halve the point count. Odd single-channel
counts would produce a fractional source allocation size and refuse explicitly.
R8 and RGBA4 have no defined source buffer type and refuse. Float points preserve
finite HDR coordinates. Ranges use project dimensions for Reference units because
this source node explicitly returns PROJ_SURF from getDimension. Max Amount zero
uses all available points; positive values cap the output. Negative source array
lengths remain an explicit unsupported profile.

Sampler performs the exact nested i/j square loops, with side 2*SamplingSize-1.
Nonpositive sizes produce no loop iterations and zero channel sums. Raw source
oversample labels are preserved: 1 clamps, 2 uses signed safe_mod against width-1
and height-1, 3 adds opaque black outside the bounds. This differs from editor
labels. Zero modulus returns zero exactly as the pinned safe_mod helper defines.
Other labels fall through to the builtin readback. Unresolved out-of-bounds
builtin reads refuse by name; negative signed wrap results are not changed into
positive wrapping. Source averages are converted with half-even make_color_rgba,
and Alpha false forces opaque output.

[The pinned official HTML5 surface builtin](https://raw.githubusercontent.com/YoYoGames/GameMaker-HTML5/60e51be51ce7f3d52025ef18106cf172b8e22a00/scripts/functions/Function_Surface.js)
converts coordinates with yyGetInt32 before reading, so the CPU profile truncates
finite bounded coordinates. In-bounds RGBA8 reads use exact packed words. Float
surface_get_pixel_ext applies the pinned wrapper's separate weighted-channel
rounding before summing the packed integer, not independent byte rounding.
Unresolved non-RGBA8 unorm packing refuses. Licensed desktop readback parity has
not been established; no GPU or source-runner bitwise equality is claimed.

Pinned source commit: `b69eca232217360cf1502ef0223523d818606652`.

| Source | SHA-256 |
| --- | --- |
| `scripts/node_pixel_extract/node_pixel_extract.gml` | `2d54d106b8c2dd9a49b7c1cd9185779e50f1573f8b142fba0f7f582b8b578124` |
| `scripts/node_find_pixel/node_find_pixel.gml` | `58760b2ecb6011e8e11f887869c6cdec2248e8170db816cd32362530496cc532` |
| `scripts/node_surface_to_points/node_surface_to_points.gml` | `1465d0712b84d75d415ed0d2dc532a254838d34b9c6f21a90f1dd6147876de8b` |
| `scripts/node_sampler/node_sampler.gml` | `966281e41987343bacbe7db3069f330f005ef6ee5bf2e01d347781d6aaf10e27` |
| `scripts/surface_functions/surface_functions.gml` | `d27420ab408d66568062005e91b79dfbfb2a2e2f044441417959803b2ea1bfb2` |
| `scripts/safe_operation/safe_operation.gml` | `b3c744deeb5cba1541313e4a407c1099603bce6f27b456636f1619c87b45b332` |
| `scripts/node_data/node_data.gml` | `73529e1178a4b2768d8a2ac405a7ba1dd3173d5c43f96cfc6c6942db015cbf7d` |
| `scripts/color_function/color_function.gml` | `cbe9a62a8b56257756bd3fecf1c7eb9797eda9695d34b0b7b390d0bda0120177` |

The focused suite prepares twelve actual compiled graph cases: default controls,
raw word identity, skip modes, first/all match traversal, inclusive RGB and alpha
tolerances, project reference ranges, max-count capping, half/full-float pairs,
HDR coordinates, sampler labels and empty loops, real linked array controls,
graph persistence, and refusal atomicity. Joined runtime checks are pending.
