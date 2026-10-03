# Julia Set and Gabor Noise source CPU profiles

Both executors port Pixel Composer pin `b69eca232217360cf1502ef0223523d818606652`.
The retained manifest records constructor, fragment shader and shared getter hashes.
The upstream MIT notice remains in `assets/licenses/PixelComposer.txt`.

## Julia Set

`pc.julia_set` publishes `surface`. It reads Dimension and its Pixel/Project/Mask
unit, UV Map/Mix, Mask, C and C unit, Max Iteration, Diverge Threshold, Position
and Position unit, Rotation, and Scale. C/Position Reference units use the first
raw prepared canvas before processor selection, then the shader divides those
coordinates by the current raw canvas. Allocation dimensions use source
half-even rounding and the minimum-one rule. Numeric links receive units;
linked surfaces bypass them and supply dimensions.

The initial coordinate is `(uv - position/dimension) * 4/scale`, rotated as a
row vector. Each iteration tests squared magnitude before updating
`z = (x*x-y*y, 2*x*y) + C/dimension`. The output is the escape index divided by
Max Iteration, or one if every iteration completes. The shader's literal
`MAX_ITERATIONS = 128` is unused; an authored 256 executes 256 iterations.
Negative iterations preserve the empty loop ratio of one. Zero iteration,
zero Scale, zero raw canvas, arithmetic overflow and an iteration below the
native signed-32 upload profile have explicit refusal diagnostics. Positive
iterations are bounded by the whole-array work guard.

This source class has no Color Depth attribute. Its output is RGBA8, including
inside a source instance. A fabricated depth property is rejected by Compile.

## Gabor Noise

`pc.gabor_noise` publishes `surface_out`. It reads Dimension/unit, authored Seed,
UV Map/Mix, Mask, Density/map, Phase/map, Sharpness/map, Augment, Position/unit,
Rotation, Scale/map, Level In/Out, Color Depth, and source processor attributes.
Automatic source seed generation is an explicit unobserved-source boundary.
No replacement random generator is used. Seed participates in the shader's
floor modulo 10000; authored negative seeds retain that formula.

Each pixel sums the fixed 5 by 5 neighbor stencil. Source sine/fract hashes
supply a displacement and normalized direction; exponential radial weights
multiply cosine carriers. Phase is added to the direction projection and
again to the final carrier angle. Only the wave's X component is consumed;
its derivative components are inert. The ratio of weighted carrier sum to
weight sum feeds Level In/Out and the final `.5 + .5*level` grayscale conversion.
Zero input-level span, zero direction length, vanished/overflowed weights and
nonfinite arithmetic produce explicit refusals.

Coordinates apply width/height aspect to X before subtracting Position divided
by the raw canvas, rotating, and multiplying by the two Scale components.
The source shader computes mapped `sca` but transforms with uniform `scale`.
Therefore the Scale Map sampler is inert, while the two uniform Scale values
remain anisotropic. Scale's `setMappable(8)` has false vec4 mode: its synthetic
MapRange is Vec2 with default `{4,4}`, not a fabricated Vec4. Unmapped Vec2
metadata stays unchanged. Mapped numeric physical values and linked ranges
precede synthetic ranges; scalar physical values duplicate their endpoint.
Density/Sharpness/Phase sample mean RGB at original pixel UV, ignore map alpha,
and use the first endpoint when their sampler is missing. Rotation controls
are converted from degrees at the kernel boundary.

## Getters, masking and bounded execution

Scoped source getter projections cover Julia C/Position/Scale and Gabor
Position/Scale/Augment surface dimensions. A whole surface array is passed to
`surface_get_dimension` before processor row selection and yields its source
nonsurface fallback `{1,1}`. Gabor Level In/Out uses the source SliRange array
getter and the same surface dimension rule. Julia Int/Float/Slider and Gabor
Density/Sharpness/UV Mix surface getters retain scalar dimension arrays before
processor selection. Their whole surface-array fallback is also `{1,1}`.
Rotation's getter returns a raw surface handle rather than dimensions; that
licensed handle interpretation is not synthesized. Path-to-Vec2 links need the
source getter's authored path sampling ratio and remain outside the current
native compiler/getter contract.

UV Mix zero retains UV-map alpha. Masks multiply alpha by mean RGB times alpha;
Mask Alpha Only is source-inert for these source `mask_apply_empty` calls.
The mask copy retains the source RGBA8 intermediate boundary and safe-draw red
channel expansion. Gabor supports the existing seven explicit typed depths and
inherited source depth resolution. Julia remains RGBA8.

Row zero preflights the complete original numeric/nested/general arrays,
canvas units, linked dimensions, all mask canvases and processor row count.
A work unit counts one scalar read, arithmetic/comparison or math-function call.
Julia quotes 512 base units per pixel plus 16 per maximum positive original
iteration. The base covers coordinate arithmetic, UV/mask sampling and typed
writes; each iteration covers magnitude, comparison, recurrence and checks.
Gabor quotes 256 base units plus 96 for each of its 25 neighbors (2656 units per
pixel). Each cell allowance covers two two-component sine/fract hashes, seed
modulo arithmetic, displacement, direction normalization, squared distance,
phase, exponential weight, cosine carrier and accumulation. The base covers
three optional mapped-control reads, coordinates, UV/mask and typed writes.
These are conservative bounded-operation estimates, not timings or benchmarks. Both reject a conservative
whole-batch estimate above 64 million before output allocation. A whole-batch
output byte reservation uses 16 bytes per pixel for every row,
including Julia RGBA8, so later typed formats cannot outgrow the first-row
estimate. Per-row metadata adds all declared image/value/array/domain output
slots, shape ElementValue/ImageArrayItem/Image records and 256 bytes of
additional wrapper/name capacity. The reservation runs against currently
retained input storage before any image is published. The public Color Depth
attribute is static: authored arrays are rejected with TypeMismatch, linked
arrays with UnsupportedExecution. A private row-context test proves the widest
format quote with original low/high depth metadata without widening that public
contract. Common processor code retains each row and its topology,
source schedules, byte accounting and failure atomicity.

## Evidence and limits

The suites use literal formula-derived Julia/Gabor grayscale matrices, analytic
zero-density and zero-scale Gabor cases, source phase/rotation/level arithmetic,
actual `pc.array` schedules, heterogeneous canvases, later-row work refusal,
public byte-cap atomicity, native persistence and source instances. The IO suite
edits all four physical Gabor numeric/map slots and retains source attributes,
unknown JSON fields, numeric pairs and the map toggle, with failed-write
atomicity. `goldens.py` is an independent scalar formula calculation; it is not
a licensed render capture. Joined native execution remains required after
integration; syntax checks alone are not runtime evidence.

These are native CPU double-arithmetic profiles with nearest sampling and the
existing typed quantization contracts. GPU float precision, sine/exp/cos hash
rounding, driver filtering, integer/uniform setter conversions, implicit
unmapped scalar-to-vec2 uploads and licensed desktop raster parity remain named
external observations. No GPU run or desktop pixel capture is claimed.
The GLSL specification describes division-by-zero results as unspecified and
defines normalization through division by length; these executors refuse the
corresponding undefined arithmetic rather than supplying fabricated values.
[GLSL specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)
