# Source ambient occlusion CPU profile

`pc.ambient_occlusion` implements the pinned `Node_Ambient_Occlusion` and `sh_sao`
source at revision `b69eca232217360cf1502ef0223523d818606652`. The wrapper
hash is `f2c7cb8916ae5ffe0e673948a9d52c3d74ba2125af1d62a7dc5b5a15ca678c9b`;
the shader hash is `6cd55d8a13c4e33047b5dcaa841d907a2641a39c2ce602725a5eb076eb35d56f`.

Height is mean RGB multiplied by alpha. Mapping uses mean RGB without alpha.
The final nonblend ambient is clamped to zero by source `max(0,1-aa)` before
optional Blend Original. The pass retains all 65 source angular samples and
division by 64. Pixel Sweep
uses the binary angular order; Uniform Sweep repeats the zero direction. Radius
starts at zero and increments by one through the maximum height endpoint, with
an early break at the per-pixel effective height. The intensity curve uses the
source float curve evaluator and its portable 64-float capacity. Blend Multiply
and Subtract retain source alpha, including HDR colour values in float formats.
The main sampler follows `sampler_simple`: Pixel and CleanEdge disable hardware
filtering, while the other interpolation choices use bilinear filtering. Mapped
surfaces use nearest sampling. The existing oversample modes apply to radial
samples. White draw tint and the source cleared render target are assumed.

Reference height converts numeric input against the first original height-map
width before processor rows. Source Float/Slider surface getters return width
and height before unit conversion. A whole image array produces the source
nonsurface fallback pair `[1,1]`. Unmapped pairs have depth zero and become two
processor rows; mapped pairs have depth one and remain one endpoint range. With
processing disabled, precisely two numeric values remain one uploaded uniform:
unmapped Height and Intensity use the first endpoint, while the height loop
maximum uses both. Other unprocessed array inputs retain their existing refusal.
All original Height/Intensity arrays are admitted before active execution or
inactive surface copying. Unknown array shapes fail explicitly.

The source computes `0/0` at radius zero. Its subsequent GLSL `max` result with a
NaN operand is undefined, as documented by the
[Khronos built-in functions specification](https://github.khronos.org/Vulkan-Site/glsl/latest/chapters/builtinfunctions.html).
This native CPU profile deliberately uses ordered comparisons for reductions.
It preserves the literal radius-zero arithmetic and does not add an epsilon.
The deterministic finite output is not a claim about a particular driver's NaN
handling, licensed desktop execution, GPU equivalence, or visual parity.

Allocation-free row-zero admission conservatively combines maximum original
height endpoints, all original height-surface dimensions, Reference unit
conversion and processor count before inactive copying or AO allocation. The
portable curve cost is also charged. The aggregate cap is 64 million sample-work
units; each selected row retains its own validation and work check. Input projections charge their actual retained
vector capacity. Nonfinite uniforms, unsupported curve storage and excess work
produce explicit diagnostics with atomic caller output. Inactive nodes preserve
the original image format and bytes.

Independent checker integrals give centre ambient values 0.8361882729570022
(Pixel Sweep, height two), 0.8354046165539246 (Uniform Sweep, height two), and
0.6014213290669324 (Pixel Sweep, height four). Nearest sampling gives 45 occupied
directions and centre ambient 0.6484375. These analytic double calculations are
recorded by `reference.py`; shader arithmetic remains float. Compiled-graph
fixtures cover formats, blends, mapped controls, curve use, processor rows,
surface getters, Reference conversion, persistence and atomic refusal.

Validation includes strict syntax checking, the independent numerical reference,
and repaired CPU fixtures linked against a matching release archive. The joined
release core passed 1,716 cases and 1,817,515 assertions; I/O passed 182 cases
and 6,518 assertions. GPU/desktop comparison remains unverified.

Source Active rejects arrays and converts its input with scalar `bool()`.
The existing non-scalar Active boundary is preserved. Public batch fixtures
use scalar Active. A private context fixture checks that original-batch
admission precedes inactive copying without claiming public Active-array support.

If the first original main-surface item is itself an array, Node_Processor's
`getDimension(0)` returns project dimensions (`PROJ_SURF`). Reference height
therefore uses the resolved project width, not the first flattened descendant
and not the currently selected surface width. A private nested 3/7-width
fixture with project width nine verifies this distinct getter profile. This
does not widen the existing public nested processor-array admission boundary.
