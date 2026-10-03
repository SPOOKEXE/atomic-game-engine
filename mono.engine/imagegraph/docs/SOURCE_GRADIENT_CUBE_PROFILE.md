# Gradient Cube CPU source profile

`pc.gradient_cube` implements the pinned `Node_Gradient_Cube` constructor and
`sh_gradient_cube` equations at source revision
`b69eca232217360cf1502ef0223523d818606652`. Both declared outputs execute:
`surface_out` and `cross_section`. This is a bounded binary64 CPU equation and
pixel-center raster profile. No GameMaker device image or desktop oracle was run.

## Inputs and outputs

| Durable input | Source slot | Meaning |
| --- | ---: | --- |
| dimension | 0 | Pixel or Project canvas, physical surface getter |
| rotation | 1 | Camera Euler degrees, default `(30,45,0)` |
| scale | 2 | Orthographic camera scale, default `1` |
| colors | 3 | Palette, default opaque black and white |
| axis | 4 | Main palette position permutation XYZ/YZX/ZXY |
| shape | 5 | Cube or sphere signed distance function |
| axis_2 | 6 | Cross plane X/Y/Z |
| position | 7 | Cross plane position |
| rotation_2 | 8 | Palette coordinate Euler degrees, default zero |
| scale_2 | 9 | Component palette coordinate scale, default `(1,1,1)` |

There is no UV map, mask, Active, seed or mapped numeric input in this source
constructor. Common array processing, color depth and animation remain intact.
The generic Mask dimension-unit choice has no mask surface on this node and
cannot produce a fabricated mask reference.

Camera and object matrices preserve the shader's column-major constructors and
`rx * ry * rz` order. Camera inversion follows the literal cofactor formula.
The geometry stays centered at zero with half extent/radius `.5`; object rotation
and scale transform palette coordinates only. Main ray origins are
`((uv-.5)*2*scale,5)` with no aspect correction. Each march performs at most 256
steps, tests distance against `1e-5`, and uses depth `10` as the miss sentinel.
Cube distance is `length(max(abs(p)-.5,0)) + min(max(q.x,max(q.y,q.z)),0)`.
Sphere distance is `length(p)-.5`.

After marching, main position gains `.5`, receives the selected main-axis
permutation, then object rotation and component scale. Cross positions are
`(position,u,v)`, `(u,position,v)` or `(u,v,position)` and use the same object
transform. The first eight palette indices wrap modulo palette count. Mixing
proceeds along Z, then Y, then X without clamping palette coordinates.

Main alpha is hit coverage `1` or `0`, independently of palette alpha. RGB on a
miss remains the palette result. Source `BLEND_ALPHA` over cleared targets
preserves this RGB; it does not premultiply it by coverage. Cross output keeps
interpolated palette alpha. Source typed packed int64 colors retain alpha;
non-int64 integral numeric colors use `colToVec4`'s default alpha `1`.
Fractional or out-of-profile packed numeric conversion is diagnosed rather than
inventing GameMaker builtin numeric conversion semantics.

Scalar EButtons use the catalogue's source clamping. Selected array choices
bypass it. The shader's defined fallbacks remain executable: unknown shape has
initial distance zero and hits at depth zero, unknown main axis leaves XYZ
unchanged, and unknown cross axis keeps the initialized zero position.

## Getters, schedules and ownership

Exact scoped surface getters apply before processor selection. Vec3 controls
return `(width,height,0)` for a scalar surface and `(1,1,0)` for a whole surface
array, because the latter is a nonsurface to `surface_get_dimension`.
Float Scale and Slider Position return the width/height numeric array, producing
separate rows when processing is enabled. Dimension surface arrays produce
physical canvas rows and collapse equal dimensions as the existing source
Dimension getter does. Scalar numeric Vec3 values replicate components; short
numeric vectors pad with zero and excess components truncate to three.

Loop, Hold, Expand and Expand inverse use the existing source processor schedule.
Both output images stay independently owned through selection, downstream
consumption and array publication. Neither plan nor request retains generated
pixel ownership. Native documents save controls, instance inheritance and
animation, then regenerate both outputs. No public value layout or codec changes
are introduced.

## Admission and errors

Before the first output, original dimension and unit rows determine whole-batch
pixel extent. One pixel quotes `512 + 256*96 = 25,088` weighted work units for
matrix/palette arithmetic, both typed writes and worst-case signed-distance
steps. A unit is a scalar read, arithmetic/comparison or math-function call.
The maximum whole-batch quote is 64,000,000 units.

The private common admission helper accepts a target count, defaulting to one
for existing callers. This node supplies two. Both targets quote 16 bytes per
pixel, the widest supported format, plus bounded existing row/publication
metadata per target and processor row. Multiplication is admitted against the
maximum evaluation byte bound before reservation. Normal output allocation
also obeys the shared live-byte ledger. Work refusal occurs before the first
small row can allocate; byte refusal preserves the previous public result.

All seven explicit formats and inherited project depth use existing typed
surface writes, quantization and hashes. Empty palette upload leaves ambient
uniforms in the source and is diagnosed. More than 256 colors exceed this named
GLSL uniform profile and are diagnosed; HLSL's 1024-color declaration is a
separate renderer profile. Nonfinite ray distances, singular/nonfinite camera
inverses and nonfinite/unrepresentable typed samples are diagnosed atomically.
Nonpositive raw source quads depend on retained renderer target/coverage state
and are diagnosed rather than replaced by a baked image or persistent cache.

The CPU pixel-center coverage for fractional positive dimensions, binary64 math
versus shader precision, sampler/quad coverage, multi-target blending and exact
GameMaker desktop builtin conversions still need real renderer/oracle checks.
The literal source equations and authored graph tests do not establish GPU parity.

## Validation packet

35 authored Compile/Evaluate cases cover independent literal cube/sphere camera
and axis goldens; analytic RGB corner interpolation; float extrapolation and
rotation sign; palette alpha and miss RGB; palette wrap/unused corners; scalar
and array choice branches; surface/scalar/tuple getters; Project dimensions and
half-even allocation; four processor schedules for both outputs; seven typed
depths; downstream Invert; selected Image variant identity and explicit value-only
extraction refusal; native persistence,
instance inheritance and keyframed camera ticks; whole-batch work and two-target
byte refusal. The independent Python equations use a generic adjugate inverse,
not the producer's GLSL-index implementation. The focused native suite passes
35 cases and 1,608 assertions. Joined validation passes all 2,495 imagegraph
cases and the selected product suites. Logs and their hashes are retained in
`docs/pixel-composer-m0/native-validation-2026-10-02.json`. GPU, live Studio and
licensed-reference comparisons remain open.
