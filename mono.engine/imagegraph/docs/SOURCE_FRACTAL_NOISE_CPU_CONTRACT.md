# Simplex and Ridge Noise CPU contract

`pc.noise_simplex` and `pc.ridge_noise` implement the pinned Pixel Composer
wrappers and their active shaders as bounded CPU reference kernels. They require
an authored, linked, animated, or captured seed. They never initialize a seed
from ambient randomness and never mutate a shared random stream.

The reference profile uses CPU double arithmetic, white draw tint, clamp-to-edge
nearest texture reads for the noise shaders and UV/maps, and the engine's typed
surface stores. It preserves source draw boundaries and intermediate format
quantization. The literal fixtures come from a separate scalar transcription of
the pinned shader formulas. They are not measurements of the source executable
or of GPU float arithmetic. Exact GPU noise/hash, tint-state and device parity
remain observation gates. Flat Ridge gradients use the explicit native
`atan2(0, 0) = 0` convention; GLSL leaves that case undefined.
[GLSL specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)

## Controls and getters

Both routes resolve Dimension with the source unit choice. Unlinked Mask Size
multiplies each authored Dimension component by the corresponding mask extent.
Linked Dimension values remain physical and bypass the consumer unit, including
when its Mask input is absent. Projected sides round half to even and clamp to
at least one; nonfinite projections and native dimension limits are checked
before allocation.
Position's Reference mode multiplies its two components by the output dimensions
before the shaders divide Position by Dimension. Rotation is authored in degrees.
UV Map reads `(red, 1 - green)`, blends coordinates by UV Mix, and contributes
its alpha independently of that mix. Mask uses `mask_apply_empty`, including its
RGBA8 temporary and its disregard for `mask_alpha_only`.

| Simplex control | Source default | CPU behavior |
| --- | --- | --- |
| Iteration | 1 | Integer getter rounds endpoints before the shader samples a mapped range; fractional mapped counts retain shader loop/break behavior. |
| Iteration Map | absent | Nearest mean-RGB sampling at aspect-adjusted UV interpolates the pair. Mapped toggle defaults off; its native synthetic range starts at `[0, 1]`. |
| Tile | true | Four translated octave evaluations blend with the shader's inverted UV weights. |
| Position | `[0, 0]`, Reference | Physical or dimension-relative position, including animation and instance inheritance. |
| Rotation | 0 degrees | Row-vector GLSL rotation convention. |
| Scale | `[.25, .25]`, Reference | Unmapped components are independent physical scales. Mapped default remains a two-component pair, not a four-component rectangle. |
| Scale Map | absent | Mean RGB interpolates the pair and broadcasts one scale to both dimensions before dimension division. Mapped toggle defaults off. |
| Scaling | 2 | Multiplies the octave coordinates after each evaluation. |
| Amplitude | .5 | Source geometric normalization and successive amplitude multiplication. |
| Level In / Out | `[0, 1]` / `[0, 1]` | Applied only to the tiled branch, preserving the untiled early return. |
| Color Mode | Greyscale (0) | Greyscale, RGB (1), or HSV (2). |
| Color R/G/B Range | each `[0, 1]` | RGB/HSV use the exact source channel offsets and affine range conversion. |

A mapped iteration pair has source `array_depth1` and remains one shader input.
Its output is one image. `EvaluateArray` reports `InvalidOutput` and preserves
its candidate when called on that scalar image output.

The active Simplex kernel is the wrapper shader's IQ two-dimensional noise.
Its unused Ian3D function is not substituted. The hash scales `sin(dot(cell,
constants))` by `seed / 100`, rather than using a different canonical noise hash.

| Ridge control | Source default | CPU behavior |
| --- | --- | --- |
| Scale | `[2, 2]` | Direct shader scale, with no unit conversion. |
| Iteration | 1 | Integer getter rounds. Nonpositive counts return the initial height surface. |
| Mode | Sine (0) | Sine or Sharp (1), preserving the source stripe equations. |
| Heightmap | absent | Supplied surface is stretched with nearest safe drawing; otherwise IQ initial height uses the source amplitude. |
| Ridge Scale | 32 | Divided by Iteration Factor after each pass. |
| Blending | 4 | Two-pass source Gaussian when greater than 1, divided by Iteration Factor after each pass. |
| Cell Scale | 4 | Positive values perform the source nine-neighbor cell search; other values keep the fixed origin. |
| Iteration Factor | 1.5 | Evolves scale, amplitude, blur and ridge frequency. |
| Ridge Rotation | 0 degrees | Added to the direction from the transposed red-channel gradient. |
| Ridge Contrast | .5 | Source contrast affine expression, then lower clamp. |
| Blend Mode | Mix (0) | Mix adds ridge height and alpha to the base; Overlay (1) uses its source height branch and max RGB. |
| Ridge Multiply | false | Multiplies the stripe by gradient length times Ridge Multiplier. |
| Ridge Multiplier | 16 | Applied only when Ridge Multiply is enabled. |
| Level | `[0, 1]` | Applied after contrast and optional gradient multiplication. |
| Oversample | inherited | Controls Gaussian's outside sampling; the noise shaders themselves use ordinary texture reads. |
| Interpolate | inherited | Inert in this wrapper, which does not read it. |

Ridge's red gradient components are intentionally transposed. Each pass reads
its base at raw UV, even when the geometry uses a UV map. RGB/alpha are stored in
the chosen format between passes. Initial supplied Heightmap skips the UV alpha,
matching the wrapper's direct safe draw. Gaussian uses its own bilinear reads,
alpha-aware accumulation, kernel normalization and two format-quantized passes.
The default project sampler inherits Pixel interpolation and Repeat XY
oversampling through the existing sampler resolver.

## Single-channel source quirks

`draw_surface_safe` calls `__channel_pre`, which replaces the active shader with
`sh_draw_r8`, `sh_draw_r16`, or `sh_draw_r32` for a single-channel input. Those
shaders replicate red to RGB and use alpha 1. Consequently:

- R8/R16/R32 noise outputs ignore the mask shader, including a black mask, but
  still traverse the RGBA8 temporary and copy its quantized red back.
- Ridge's R8/R16/R32 Gaussian safe draws replace both Gaussian passes, retaining
  the unblurred surface. The Ridge shader itself uses ordinary `draw_surface`
  and still executes for single-channel formats.
- A single-channel supplied Heightmap expands red during its initial safe draw.

RGBA formats retain Gaussian and mask arithmetic. No shared blur or mask helper
is changed by these node-specific source behaviors.

## Bounds and refusal

A division-based 64,000,000 work-unit cap scans the retained original controls
and image arrays once at processor row 0 before any owned noise output/scratch
growth. These scans visit only payloads already bounded by the document/evaluation
array limits; later rows reuse admission without rescanning whole arrays. The
bound defensively observes Pixel, Project and Mask modes independently, applies project
scaling only when Project occurs. It includes every Mask Size frame and multiplies
its extent maxima by the retained authored Dimension component maxima.
Per-axis negative extrema also detect multiplication overflow before any row
allocates output. Linked
physical dimensions bypass those unit projections. Projected native dimension
limits are admitted before noise output allocation. The work bound combines
maximum dimensions, iteration endpoints, enabled tile/color branches, absolute
blur and minimum absolute iteration factor across all rows, then multiplies by
the processor count. This
may refuse batches whose worst combinations are not scheduled together. Actual
row controls remain unchanged. Synthetic Dimension Unit attributes are not
processor-batched in native graphs: a linked unit array is refused by the scalar
integer reader. The mixed-unit bound is tested using private selected/original
views rather than claiming that graph schedule is supported. A zero factor with a multi-iteration worst case
has no finite bound and is refused; the selected row's known undefined zero-factor
execution retains its named unsupported diagnostic. Simplex units include fixed
UV/map reads, output/mask writes and each three-corner IQ evaluation. Ridge units
include initial/final work, gradient reads, nine cell neighbors, row writes,
Gaussian center/offset reads, pass writes/copy and kernel generation/normalization.
Ridge additionally bounds the exponent range and controls preflight scans. Scratch, kernel and
publication bytes use the existing per-evaluation ledger. Candidate output is
published only after successful evaluation.

Invalid modes, unavailable seed, missing Mask Size input, over-budget work,
invalid dimensions and exhausted byte budgets report durable node/port
identities. Undefined divisions, nonfinite evolving controls, and nonfinite or
unrepresentable source arithmetic produce named refusals. Zero-iteration and
untiled early-return branches retain the source's unused-control behavior.
Neither refused evaluation nor a failed processor batch replaces caller output.

Both nodes use existing processor rows, timeline getters and authored instance
inheritance. They have no private persistent state or new public carriers.
Document roundtrip preserves authored controls and links through the existing
codec.

## Pinned evidence

Pixel Composer revision `b69eca232217360cf1502ef0223523d818606652`:

| File | SHA-256 |
| --- | --- |
| `scripts/node_noise_simplex/node_noise_simplex.gml` | `65046db95e059408cf28e5cabe8eca76673bfea5eabb917b4a516f0d111a0657` |
| `shaders/sh_simplex/sh_simplex.fsh` | `8a0a75723030b49ee185e542c2e551e255d3c621d31353e6bab346f2a1e1c752` |
| `scripts/node_ridge_noise/node_ridge_noise.gml` | `fc90e002bd92c7707a5da827bb55170dd06163062b1465abfb271c908a8745ca` |
| `shaders/sh_noise_ridge/sh_noise_ridge.fsh` | `a0b7c4ebcf361a9d745d0401c61cff5ffdfd6eabc2f07b395ddbfccadd8cadad` |
| `shaders/sh_noise_ridge_init/sh_noise_ridge_init.fsh` | `883affd0515be3a5e494b0120ce24e79e39e5221d012b4adc99cea46ce68449c` |
| `scripts/surface_draw_functions/surface_draw_functions.gml` | `c0519138b9a9022d3b5ffeaf90242cf9106253991a5ecea2f3e4d001e9ceb720` |
| `scripts/blurSurface/blurSurface.gml` | `63494c62cdb7781d5c85cc0d1577239e0b132a2efde6c27753b1df73be400805` |
| `scripts/mask_function/mask_function.gml` | `94c22c8b1d3085c8ada6c673e5836326a8c866579b35ebb4487c343fb4f94023` |
| `scripts/node_value/node_value.gml` | `6ca786dca5c9bb7a9116faaee9e8d24e0e5c86aea71ac376230fbccb491e17dd` |
| `scripts/node_value_int/node_value_int.gml` | `75ae2d6a3df5c6d7f62e50adf0bfc1f1467c51e6519bb80011f6164536dff8d2` |
| `scripts/node_value_dimension/node_value_dimension.gml` | `7a31f24df14a2fe19f5566ab7d2e919f792faccac83ecbdff7f59a91e93f54e9` |
| `scripts/node_value_vec2/node_value_vec2.gml` | `c37cd3670441eb5dc0655db8d8751d22b135577e71a513936d7354704a501112` |

The test file contains 38 authored-graph cases and one private defensive preflight case: default/explicit seeds, tiling,
normalized octaves, channel offsets/ranges, transform units, levels, UV alpha,
masks, mapped ranges and linked range arrays, float and single-channel formats,
Ridge cell/gradient/stripe/blend/multiplier controls, Gaussian oversampling,
animation/instance seek, processor row order, persistence and atomic work/byte
refusals. Mask-unit regressions additionally cover raw nonunit and fractional
components, heterogeneous mask arrays, linked physical dimensions, later-row
projection/work limits, nonfinite multiplication and tight float byte budgets.
Joined Compile/Evaluate runtime validation remains required after
integration; outside strict syntax validation alone is not acceptance.
