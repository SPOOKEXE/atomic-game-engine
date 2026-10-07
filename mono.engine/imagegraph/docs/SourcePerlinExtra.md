# Source Extra Perlins

`pc.perlin_extra` is described here from pinned Pixel Composer commit
`b69eca232217360cf1502ef0223523d818606652`, specifically
`scripts/node_perlin_extra/node_perlin_extra.gml` and
`shaders/sh_perlin_extra/sh_perlin_extra.fsh`. This records the source
constructor, shader behavior, and bounded native CPU evaluator. Numeric tests
use independent binary32 references; they do not establish parity with a
particular GPU or Pixel Composer runtime.

## Constructor and inputs

The node is named Extra Perlins and uses `sh_perlin_extra`. Its visible
inputs are arranged as Output, Noise, Transform, and Rendering groups:

| Input | Default | Shader property / source role |
| --- | --- | --- |
| UV Map | unlinked | `useUvMap`, `uvMap`; remaps coordinates and supplies output alpha |
| UV Mix | 1 | `uvMapMix`; blends original and remapped UVs |
| Mask | unlinked | Declared by the node constructor; mask application is not in this fragment shader |
| Seed | source seed default | `seed` |
| Noise Type | 0, Absolute worley | `type`: Absolute worley, Fluid, Noisy, Camo, Blocky, Max, Vine |
| Iteration | 2 | `iteration` |
| Tile | true | `tile` |
| Parameter A | 0 | `paramA`; hidden for type 0, shown for types 1 through 6 |
| Parameter B | 1 | `paramB`; hidden during `onProcessData` |
| Position | `[0,0]` | `position` |
| Rotation | 0 | `rotation` in degrees, converted to radians in the shader |
| Scale | `[4,4]` | `scale`; mapped to a scalar interpolation between its two components when linked to a surface |
| Level In | `[0,1]` | `levelIn` |
| Level Out | `[0,1]` | `levelOut` |
| Color Mode | 0, Greyscale | `colored`: Greyscale, RGB, HSV |
| Color R/G/B Range | each `[0,1]` | `colorRanR/G/B`; shown outside Greyscale and renamed H/S/V in HSV mode |

The constructor attaches `setMappable` to Scale, Parameter A, and Parameter B.
The shader samples each linked map's mean RGB and interpolates between the
corresponding two authored values. Parameter B is prepared from its
uniform/map in the shader, but no later shader expression reads `B`; its effect
is therefore inert in this shader. The constructor also declares input 22 as a
comment only, not as an input.

The C hotkey cycles Color Mode modulo three, and T cycles Noise Type modulo
seven. Scale, Position, and Rotation receive S, G, and R hotkeys respectively.
The source display updates Scale's value type to integer when Tile is on and
float when Tile is off. This is input UI behavior, not a shader-side clamp.

## Shader equations

The fragment shader maps `v_vTexcoord` through UV Map when enabled. It flips
the sampled map's Y coordinate, blends it with the original coordinate by UV
Mix, and carries sampled alpha to the output even when UV Mix is zero. It then
multiplies Y by `dimension.y / dimension.x`, subtracts Position divided by
Dimension, applies the shader's rotation matrix, and multiplies by Scale.

Each noise octave hashes four cell corners. With Tile enabled, the corner cell
coordinates are wrapped by the current scale. The default interpolation is
the cubic polynomial `f*f*(3-2*f)`. Type 0 instead hashes two-component
gradients and returns the absolute interpolated dot product. Types 1 through 6
interpolate scalar corner hashes before their octave-specific accumulation.

| Type | Shader behavior |
| --- | --- |
| Absolute worley (0) | Uses gradient dot products, interpolates them, and takes the absolute value. Despite the menu label, this branch does not compute nearest-point Worley distance. |
| Fluid (1) | Doubles scale, halves amplitude, and warps position by `1 + octave noise + A`. |
| Noisy (2) | Doubles scale, halves amplitude, offsets position by a seeded random vector divided by scale, then multiplies position by `2 + A`. |
| Camo (3) | Runs three-octave groups with alternating scale, position, and amplitude changes; each group contributes a `smoothstep` of its accumulated noise. |
| Blocky (4) | Replaces interpolation with repeated cubic smoothing controlled by A, with the repetition count changing across octaves. |
| Max (5) | Takes the maximum octave value and multiplies scale and position by `1 + 0.1*A` per octave. |
| Vine (6) | Evaluates four offset noise fields and sums the absolute differences of opposing pairs. This branch returns before Level In/Out remapping. |

For non-Vine types, Level In/Out linearly remap the accumulated scalar. RGB
mode evaluates three offset noise fields and maps them through the three
color ranges. HSV mode uses the same ranges for hue, saturation, and value,
converts to RGB, and multiplies by vertex color. Greyscale writes the scalar
to RGB. All branches then multiply alpha by the UV-map alpha. The shader has
no final `else` for an unsupported `colored` integer, leaving its RGB result
unwritten for such a value.

## Undefined source cases

The source shader does not define portable results for consumed zero or
non-finite divisors, non-finite intermediate values, or output values that
cannot be represented by the destination format. Examples include zero
Dimension width, zero tiled scale used by `mod`, equal Level In endpoints for
types 0 through 5, and nonpositive A in Camo's
`smoothstep(0.5-A, 0.5+A, ...)`. These are source undefined or non-finite
cases, not evidence for a particular fallback value. Vine skips Level In/Out,
so equal level endpoints are not consumed there. Iteration zero makes the
octave amplitude denominator zero, but also makes the octave loop empty; the
shader does not use that amplitude in the accumulated result in this case.

## Native CPU behavior

The native evaluator preserves the requested output format. With a mask, it
stores the rendered image, multiplies only stored alpha by the mask's mean RGB
and alpha, then passes the result through RGBA8 scratch storage before copying
it back to the requested format. UV-map alpha is applied even when UV Mix is
zero. Parameter B is inert, and Vine skips the Level In/Out remap.
Position with Reference units is resolved against the first prepared Dimension
before processor rows are selected.

Positive iterations require mapped Scale. Parameter A must also be mapped for
Camo, Blocky, and Vine. Fluid, Noisy, and Max consume Parameter A when more
than one iteration runs; their last unused state update is skipped. Absolute
worley ignores Parameter A. Zero or negative iterations produce zero noise.
Non-Vine types still apply Level In/Out to zero, while Vine returns zero
without applying levels. A missing seed is refused only when a positive
iteration consumes it.

The evaluator quotes the entire selected batch before allocating output or
calling observers. Its work quote is 512 base units per allocated pixel, plus
`max(iteration, 0) * channels * typeFactor * (2048 + 512 * smoothBound)`, with a 64
million unit batch limit. `channels` is one for Greyscale and three for RGB or
HSV. `typeFactor` is three for Camo, four for Vine, and one for the other
types. `smoothBound` is zero except for Blocky. Exceeding the batch quote fails
at `surface_out`; an excessive Blocky smoothing loop fails at
`parameter_a`. Mask scratch is reserved before output allocation. Raw Atlas
bindings are refused when their UV, mask, Scale, or consumed Parameter A
sampler is used. These native refusals are evaluator limits, not fallback
values for undefined source arithmetic.

The shader's exact float results, texture sampling state, handling of
undefined operations, and raster coverage remain GPU/runtime-dependent. This
document does not claim licensed-runtime pixel parity.
