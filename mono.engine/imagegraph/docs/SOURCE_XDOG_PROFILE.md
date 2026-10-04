# XDoG Threshold native CPU profile

`pc.xdo_g_threshold` preserves `Node_XDoG_Threshold` at source revision
`b69eca232217360cf1502ef0223523d818606652`. Source-derived portions retain the
[upstream MIT notice](../../../docs/pixel-composer-m0/PixelComposer-LICENSE.txt).
The implementation evaluates bounded native CPU equations. Native graph and
archive tests pass; see the native validation ledger for joined CPU results.
No Pixel Composer runtime or GPU was executed.

| Physical slot | Control | Meaning |
| --- | --- | --- |
| 0 | Surface In | Required original input |
| 1, 2 | Mask, Mix | Apply only to Surface Out |
| 3, 4 | Invert Mask, Mask Feather | Common processor mask modifiers |
| 5, 6 | Active, Channel | Source processor controls |
| 7 | Radius | Float, default .25, Reference units by default |
| 8 | k | Second Gaussian radius multiplier, default 8 |
| 9, 12 | Gamma, Gamma Map | Float pair, default scalar 1 |
| 10, 13 | Epsilon, Epsilon Map | Slider pair, default scalar .1 |
| 11, 14 | Smoothness, Smoothness Map | Slider pair, default scalar .1 |
| 15 | Edge | Absolute difference when true |

Radius Reference units use the first prepared original Surface In width. The
numeric getter applies units before processor row selection. A linked Surface
returns its width and height as numeric rows before units. All three mapped
controls preserve authored physical values ahead of synthetic static ranges;
missing optional maps use the first range endpoint. Maps use unfiltered mean
RGB, with alpha ignored.

Surface-origin radius rows retain their socket domain after numeric projection.
Their dimensions already use pixel units, so Reference scaling is bypassed even
when the processor row no longer carries a borrowed radius image.

Both Gaussian passes use the original input format and the shared pinned
`blurSurface` equations. Each completed blur is copied into RGBA8 staging,
matching `surface_verify` without a format argument. The difference is
`g1 - gamma*g2`, optionally absolute, with alpha replaced by 1. It is stored in
resolved processor depth before threshold sampling. Threshold luma is mean RGB
multiplied by alpha; its edges are `max(0, epsilon-smoothness)` and
`min(1, epsilon+smoothness)`. The unused shader `tanh` helper is not executed.
Both declared outputs retain their named ports and resolved format.

Only Surface Out receives final Mask, Mix, mask modifiers and Channel. DoG is
unmodified. Source `Active=false` copies Surface In to Surface Out and retains
its previous DoG resource. This stateless native evaluator publishes the main
copy and diagnoses DoG until a real previous-output observation is supplied.
Undefined smoothstep edges diagnose only Surface Out and preserve readable DoG.
[Khronos GLSL](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.1.20.pdf)
defines no result when the first edge is greater than or equal to the second.

The source safe-draw red-format shader bypasses Gaussian filtering. Its resulting
red-format image is copied with plain `draw_surface` into RGBA8 staging, so the
native plain-texture profile keeps red-only samples there. Device texture
swizzling remains a renderer observation gate.

Preflight inspects original rows before inactive copying. It charges at most
64 million arithmetic/read units across all processor rows: 2048 per main pixel
plus 512 per pixel per maximum absolute Gaussian radius, and a separate
64 + 128*feather quote per mask pixel. The byte quote includes 32 bytes per
original main pixel per row for both worst-case RGBA32 outputs, both output
metadata sets, 64 bytes per largest main pixel for simultaneous staging/scratch,
32 bytes per largest mask pixel and kernel storage. Allocations retain their
actual reservations while alive. Public refusals preserve earlier results.

The numerical profile uses native double arithmetic, half-even kernel length,
filtered Gaussian taps and format quantization after each pass. Source-equation
fixtures preserve expression order: reciprocal-square-root times exponential,
sequential kernel normalization, RGB times (weight times alpha), normalized
g2 times gamma, and mean of normalized DoG channels. Gaussian tap weights
interpolate the computed `step / strength * maximum` index, including its
native rounding. Algebraic reassociation can change a quantized byte and is
not used to define the fixture oracle. Existing shared
Gaussian behavior treats weights outside its 256 native slots as zero. Source
GPU array reads, persistent shader uniforms such as unused UV bindings, desktop
floating-point operation ordering, source GPU raster coverage, sampler states
and licensed-runtime parity require actual device observations. These are not
claimed by source-equation fixtures.

Pinned source hashes are in the packet manifest. Tests use authored graphs,
independent Gaussian/difference/threshold equations, original-row dimensions,
source mapped edits, persistence, typed formats, output-specific refusals and
whole-batch work/byte atomicity. Strict syntax checks do not establish runtime
acceptance.
