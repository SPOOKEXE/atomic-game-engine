# Surface Replace CPU source profile

The pinned source is Pixel Composer b69eca232217360cf1502ef0223523d818606652.

| File | SHA256 |
|---|---|
| scripts/node_surface_replace/node_surface_replace.gml | 033596b39965c90bd46d6855e06523d0eac8c51319e13440ae7626e6a34c2b65 |
| shaders/sh_surface_replace_fast_find/sh_surface_replace_fast_find.fsh | fc4e05b31a9750e374801adbc678f084abda8719f5e5ab471824a060cda0c4b3 |
| shaders/sh_surface_replace_find/sh_surface_replace_find.fsh | 8fac4082b584579960483c114684e808c59faf28629b4ce4f11a85e99571fdce |
| shaders/sh_surface_replace_fast_replace/sh_surface_replace_fast_replace.fsh | 0d2cbd7636a7e8670f67fe5ae5bcd0f08714016dc3d38f9756a91799ac9f90d9 |
| shaders/sh_surface_replace_replace/sh_surface_replace_replace.fsh | 5d83ed6aaa129de6347e268541168e791eab8f5e3b80c1d85c621bcf8a5bea72 |

The constructor declares Base Image0, Target Image1 and Replacement Image2.
Target/replacement each have source array depth1: they are whole surface lists,
while base images and scalar controls follow the processor's Loop/Hold/Expand
row schedule. A narrow processor exception preserves only these two whole lists,
including item order and repeated surface indices. Nested surface lists are
explicitly unsupported by the existing flat native processor representation.
Array Mode7 selects match-index or shader randomized selection; Seed8 is required
for the latter. Fast Mode5, Color Threshold3, Pixel Threshold6, Draw Base Image4
and Replace Empty9 are all consumed. Source matchTemplate reads Mode7 and Seed8
from original getInputData, rather than selected per-row _data. Arrays with
multiple mode leaves, or seed leaves when Randomized is selected, reach scalar
uniform array uploads with unobserved renderer state and are diagnosed. Scalar
mode/seed and selected Fast/threshold/draw rows complete where fragments are
defined. No source controls are mappable, and this
node declares no Active, Mask, Mix, Channel or color-depth input.

Fast find skips transparent base/target pixels and checks each candidate's
forward target texels. A match requires RGBA Euclidean distance at most twice
Color Threshold and sufficient matching count against target area times
`1-Pixel Threshold`. Reads at the base's far edge clamp in the native nearest
profile. Full find searches covering origins that keep the target inside the
base, then compares every target pixel, including transparent ones. It chooses
the first strictly better score in source X-then-Y loop order. The result stores
target-relative coordinates and a normalized replacement index.

Each find pass adds into a cleared RGBA8 result. Replacement uses
`ri=pass%replacement_count`, `index=ri/max(target_count,replacement_count)`.
Fast replace scans possible stamp origins in source order and folds nonzero-alpha
replacement pixels using the shader's normalized blendColor. Full replace reads
the quantized match coordinates directly. Both write color and eraser surfaces
with normal blend factors. The final pass optionally draws the base, applies the
eraser with bm_subtract, then composites replacement with the pinned BLEND_ALPHA
RGB ONE/inverse-source-alpha and alpha ONE/ONE factors. Every RGBA8 draw boundary
clamps and quantizes before the next pass.

The native blend profile uses the documented factors in the authoritative
[gpu_set_blendmode reference](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Drawing/GPU_Control/gpu_set_blendmode.htm):
normal source-alpha/inverse-source-alpha, additive source-alpha/ONE and subtract
ZERO/inverse-source-color. This is an explicit native profile, not an observed
licensed desktop renderer result. The manual's general blend guide contains a
conflicting subtract example; the function reference's factor table is used.

Both replacement shaders omit MRT writes when no candidate matches or a selected
replacement texel is transparent. The GLSL source contains neither an output
initialization nor discard for those paths. Native evaluation refuses any
fragment reaching such a path, preserving the prior public result. Transparent
output is not invented. Empty replacement lists also refuse their undefined
modulo. A single-red base safe draw replaces the shader and does not establish
the second MRT output, so it remains an explicit observation gate.

All source/target/replacement surfaces are borrowed only during evaluation.
Three cleared RGBA8 intermediates own bounded local pixel storage. Before row0
allocates, the complete original base shape, largest target/replacement areas,
list lengths, original Fast Mode values and processor row count admit a 64M
conservative work bound. Full mode includes target-area squared search. The same
preflight admits all output rows plus the largest three simultaneous temporary
surfaces against the shared byte budget; actual capacity and published outputs
receive ordinary reservations. Errors never publish partial public pixels.

CPU double arithmetic, nearest clamp sampling, RGBA8 native quantization and
shader sin/fract randomized arithmetic are named reference choices. Licensed
float bit parity, initial ambient texture filtering/draw tint, hardware MRT
coverage and unwritten-fragment behavior require renderer observations. No GPU
execution, renderer capture or profiling result is claimed by this contract.
