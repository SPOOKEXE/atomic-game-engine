# Refract native source profile

`pc.refract` follows the pinned `Node_Refract` wrapper and `sh_refract` shader at
revision `b69eca232217360cf1502ef0223523d818606652`. This is a bounded CPU
pixel-center/binary64 reference profile. It is not a measured desktop or GPU
pixel-equivalence result. The upstream source notice is preserved in
[Pixel Composer MIT notice](../../../docs/pixel-composer-m0/PixelComposer-LICENSE.txt).

| Source control | Native behavior |
| --- | --- |
| Surface In, Active | Required main image; scalar inactive copies it without sampler access. |
| Normal Map, Depth Map | Explicit nearest/clamp auxiliary textures, regardless of base interpolation. |
| Height, Distance, IOR | Source Float values, optional mapped endpoint pair, mean RGB map progress. A missing optional control map disables its use and uses the first endpoint. |
| Perspective | Incident XY from original pixel UV times Perspective; incident Z remains -1. |
| UV Map, UV Mix | Explicit nearest UV texture; remaps only normal/depth lookups. UV alpha is unused. |
| Interpolate | Inherited, Pixel, Bilinear, Bicubic, Lanczos3, separator, CleanEdge at source index 6. |
| Oversample | Final constructor default 3 Clamp; Empty, Black, Clamp, XY repeat, and the six per-axis repeat policies retain source indices. |
| Mask, Mix, Channel | Existing source processor finish, including mask inversion, alpha-only and feather. |
| Color Depth | Main input depth, inherited project depth, and seven explicit typed formats. |
| Array Process | Loop, Hold, Expand and literal source inverse schedule. Static UI attributes are not physical input slots. |

The three mapped controls retain authored/link precedence over synthetic range
fields. Source numeric surface getters project dimensions before processor
selection, so a linked surface Float/Slider yields width and height rows. Normal,
depth, UV and control maps remain images. No persistent cache, host pointer or
new public payload is introduced.

## Source equations and preserved quirks

Normal is `normalize(2*R-1, 2*G-1, B)`, with raw blue. Incident is
`normalize((u-.5)*Perspective, (v-.5)*Perspective, -1)`. Depth distance is the
first Distance endpoint plus depth luminance times depth alpha times Height.
IOR is the shader eta directly, without taking its reciprocal. Negative refract
discriminant produces zero shift, preserving total internal reflection. The
[GLSL specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)
provides the built-in geometric function contract; the wrapper supplies these
specific arguments.

The mapped Distance branch writes the **depth multiplier**, leaving the offset
at `distance.x`. This is retained literally. Control-map progress is mean RGB,
without alpha or luminance weighting. The two-argument final `sampleTexture`
overload passes mapBlend 0, so it does not remap the displaced base lookup through
UV a second time. Normal/depth texture lookup does use UV remapping.

The wrapper's `shader_set_surface` returns before binding a missing map. Both
normal/depth shader lookups are unconditional. Missing normal/depth therefore
reports an unobserved ambient sampler binding rather than inventing a flat
normal or zero depth. Inactive and source red-channel safe-draw branches do not
read those samplers. `shader_set_f_map` explicitly disables missing optional
Height/Distance/IOR maps, which is a different, source-defined case.

Red-channel main surfaces install the source `sh_draw_r8/r16/r32` safe-draw
shader instead of Refract. Native output replicates red into RGB with alpha 1,
then performs the common mask/channel finish. Auxiliary red-channel textures
retain raw texture-channel expansion rather than this main draw conversion.

## Sampling

Bicubic is the shader's eased bilinear coordinate formula. Lanczos3 retains its
nine combined filtered taps through the existing private sampler. CleanEdge is
ported privately for Refract so no other caller changes its interpolation
contract. It reads exactly 21 nearest texels, retains size+0.0001 and ceil/fract
coordinates, and attempts Up, Back and Corner slices in order. All five slicing
branches, exact RGBA comparisons, weighted RGBA distances, width 1, checkerboard
exclusion and red>=0 result sentinel follow the shader. Signed floating colors
therefore keep the same sentinel rule. Oversampling wraps or clamps before the
same selected sampler; Oversample separator choices retain the initialized empty result; an Interpolate separator uses the shader's filtered texture fallback.

Constructor metadata is recovered narrowly: the final top-level
`attribute_interpolation(false,true)` selects the extended choices, and the
later literal `attributes.oversample=3` overrides factory defaults. Other
constructors are unchanged. Extractor fixtures reject unknown option/default
expressions or competing overrides, and a real pinned constructor extraction
matches the two modified catalogue input records.

## Admission, ownership and error publication

Before output allocation or an inactive copy, row 0 considers the largest main
and mask image across original image rows, the complete processor row count,
and the greatest original numeric feather when a mask exists. A missing mask
makes feather inert, as in mask_apply_input. A conservative 4096 arithmetic/read
units per output pixel covers normalization, mapped/UV reads, all three CleanEdge
slice decisions with 21 neighbor reads, the 36 texel reads of Lanczos filtered
taps, and common finishing/typed writes. Additional mask work is quoted as
`maskPixels * rows * (128*radius + 32)`. Total work is capped at 64 million units.

Whole-batch bytes quote 16 bytes per main pixel for every row plus image/array/row
metadata; 32 bytes per largest mask pixel cover two widest-format feather
scratch images; radius doubles cover feather weights. The shared allocator
separately owns actual inputs, outputs, scratch, row storage and previous public
results. Preflight is conservative admission, not a second retained pixel copy.
Finite-range failures stop the private loop immediately. Zero normal length and
nonfinite refraction arithmetic have explicit diagnostics. Outputs remain atomic
through the public evaluator on every refusal.

## Verification and remaining observations

36 native fixtures include authored Compile/Evaluate graphs with independent
refraction oracles; the Distance-map assignment; missing map diagnostics;
inactive and red-channel routes; exact-zero float normal assembled through real
Matrix nodes; all sampling policies; a 5x5 diagonal CleanEdge versus Pixel golden;
five independent slicing goldens; masks, formats, surface getters, array
schedules, persistence and inherited instances; selected public Image identity
and value-only API refusal; whole-array work and previous-result byte atomicity.
One IO fixture covers all three source numeric/map slot pairs, unchanged bytes,
range edits, unknown archive fields, mapped toggles and invalid-edit atomicity.

Native release75 validation passes 36 Refract cases with 2,458 assertions, the
mapped-input IO case with 178 assertions, all 96 metadata tests and the selected
joined CPU suites. The initial mask-feather fixture failure is retained; its
corrected oracle uses independent source Gaussian equations and intermediate
RGBA8 quantization. Full imagegraph validation passes 2,553 cases with 1,853,752
assertions. Evidence is retained under
`.cache/build/dev/evidence/pixel-composer-2026-10-04/` and in the native validation
ledger. Profiling, device and licensed desktop comparison remain open. Floating
uniform precision, device texture sampling/coverage and the original
application's ambient bindings require actual recorded renderer evidence for a
desktop/GPU parity claim.
