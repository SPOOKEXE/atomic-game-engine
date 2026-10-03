# Draw Gradient CPU contract

`pc.gradient` evaluates the pinned Draw Gradient wrapper and GLSL shader as a
bounded CPU image profile. Its output is `surface_out`. This implementation does
not claim licensed executable or GPU pixel parity.

## Source

Pinned Pixel Composer commit: `b69eca232217360cf1502ef0223523d818606652`.

| Source | Bytes | SHA-256 |
| --- | ---: | --- |
| `scripts/node_gradient/node_gradient.gml` | 9083 | `d55c626179c4ee3cf5511f140c2b83929f4dce92226daa9baec76b2bcc8fb624` |
| `shaders/sh_gradient/sh_gradient.fsh` | 13219 | `51989bdefd4ec7d77f8489c221b6a2b0d912c20d469d1f69019610e6d4764530` |

The local `__gradTypes` menu maps Linear, Circular, Radial and Diamond to
0, 1, 2 and 3. The extractor verifies that literal declaration, its Type input,
and the separately verified `__enum_array_gen` source index mapping. The four
numeric mapped slots use two endpoints, preserving the source `setMappable`
transport rather than inventing four coordinates.

## Controls and arithmetic

| Controls | CPU behavior |
| --- | --- |
| Dimension, Dimension Unit | Raw positive shader dimensions. Unlinked Project and Mask units multiply the authored components by project or mask dimensions. Linked dimensions are physical and bypass consumer conversion. Allocation rounds half to even and clamps a positive fractional side to at least one pixel. |
| Gradient | Typed keys and seven existing source shader interpolation modes: RGB, Constant, HSV, LMS, Gamma, HSV inverse and CMYK. Key alpha interpolates separately. Mapped gradients sample the explicit four-coordinate range with the shared filtered gradient sampler. |
| Type, Angle, Radius | Four source shapes. Angle is degrees. Radius is multiplied by the square root of two. Circular and Diamond use Shape and optional raw aspect ratio. Radial angle is calculated before aspect scaling. |
| Center, Center Unit, Shape, Uniform ratio | Unlinked Reference center scales by raw dimensions; linked or Pixel center is physical. Shader center divides by raw dimensions. Shape scales Circular and Diamond axes. |
| Angle/Radius/Shift/Scale mapped slots | Nearest map samples use mean RGB, ignoring map alpha. Missing maps use the first endpoint. Explicit scalar source slots repeat that scalar at both endpoints. |
| Shift, Scale, Loop | `(progress + shift - .5) / scale + .5`, followed by None, double-fract Loop, or GLSL-mod Pingpong. |
| Inverse Axis, Inverse Curve | Source shape-specific inverse progress, evaluated only when the inverse amount is nonzero. |
| Progress Remap, Curve | Existing source curve evaluation uses eight Newton steps. Progress Remap runs before gradient sampling. Curve remaps the final mean RGB brightness after levels. |
| Level In, Level Out | Per-channel range mapping, then brightness curve scaling. Black uses the curve target directly. Alpha is unaffected. |
| UV Map, UV Mix, Mask | Nearest UV sampling with source green inversion and UV alpha multiplication, including at zero UV mix. RGBA mask alpha multiplies gradient alpha; mask RGB is unused. |
| Surface precision, array processing | Existing processor precision resolution, Loop/Hold/Expand/Expand Inverse selection and candidate output admission. Scalar, vector, Curve and Gradient row values retain typed payloads. |

A single-channel R8/R16/R32 base mask follows the pinned safe-draw wrapper:
`__channel_pre` replaces the gradient shader with `sh_draw_rN`. Its red sample
expands to RGB with alpha one. Gradient controls and their unused divisions do
not affect this branch. Mapped and UV textures keep ordinary single-channel
sampling; they do not use this safe-draw expansion.

## Bounds and reference conventions

Before the first processor row allocates output, borrowed original controls and
mask frame dimensions determine conservative maximum width and height. The work
limit is 64 million units for the entire batch. Per-pixel units include 48
shape/map/read/write units, nine times the sum of maximum anchor counts for the
three curves, and the maximum gradient key count. This is an admission bound,
not a timing estimate. Common byte admission additionally bounds all owned images
and intermediate processor results.

The GLSL uniform profile supports one through nine curve anchors and one through
64 unmapped gradient keys. Larger uploads need a represented backend contract;
they receive named Unsupported diagnostics. Empty curves, zero curve header
scale and consumed nonfinite divisions are refused atomically. CMYK interpolation
through a black endpoint produces the source zero denominator and is refused by
the finite native image profile. A zero
radius or shape component does not prevent a Linear branch that never consumes
it. Native `atan2(0,0)` is zero; no GPU result at that undefined GLSL point is
asserted.

Typed authored Curve and Gradient arrays are admitted only at their four exact
compound control ports. Genuine linked `pc.array` values use those same scoped
ports. Other node types and controls retain existing validation.

## Validation

`SourceGradientDrawNodes.cpp` contains 24 actual Compile/Evaluate fixtures with
literal pixel-center goldens. They cover defaults, four shapes, raw fractional
and mask dimensions, source units, all loops and gradient modes, numeric and
gradient mapping, curves, UV and mask alpha, R-mask shader replacement,
processor arrays, linked Curve/Gradient rows, persistence, animation seeks and
atomic work/byte/nonfinite refusals. The LMS midpoint literal was calculated
from the pinned shader matrices independently of the native helper.

The outside draft passed strict syntax using the current CMake C++20 compiler
flags with warnings as errors. Extractor tests passed 14 cases and enum behavior
tests passed 17 cases. Fresh joined runtime tests remain required before this
contract can be recorded as accepted implementation evidence.
