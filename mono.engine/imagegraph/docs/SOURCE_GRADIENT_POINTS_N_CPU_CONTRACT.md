# N-point gradient CPU profile

`pc.gradient_points_n` follows the pinned `Node_Gradient_Points_N` wrapper and
`sh_gradient_points_n` fragment shader. The native profile evaluates the literal
shader formulas in double precision at pixel centers, then uses the selected
native surface format. GPU arithmetic, uniform upload limits on specific devices
and executable source captures have not been validated.

The wrapper has Dimension and Blend Mode fixed controls. Each dynamic point adds
Point, Color and Influence, with Point's synthetic Pixel/Reference unit metadata.
It has no mask, UV map, palette or color-space control. This shader mixes RGB
channels directly. It does not use the LMS transform of the separate four-point
node.

Dimension allocation rounds half to even and clamps each axis to at least one.
The shader still uses the positive raw source dimension for point normalization
and the Gaussian/Linear range denominator. Project Dimension multiplies authored
Dimension by the captured project size; linked Dimension is already physical.
Reference Point multiplies unlinked coordinates by the raw source dimensions.
Linked Point uses the shared native physical-vector getter profile, matching the
existing spatial gradient family. No origin-node surface observation is invented.

The shader starts every output sample at `(0,0,0,1)`. Zero points therefore produce
opaque black. Exponential computes each distance, doubles the largest distance,
raises `(maximum-distance)/maximum` to the individual Influence, then normalizes
by the square root of the sum of squared weights. Gaussian uses
`exp(-(distance/(Influence/rawWidth))^2)` and normalizes by the weight sum. Linear
uses `max(0,(range-distance)/range)` and adds the weights without normalization.
All modes add weighted source alpha to the initial alpha of one. Float outputs
retain alpha and RGB above one. Normalized outputs clamp through the shared
surface writer. Signed Gaussian influence is squared; signed Linear influence is
preserved rather than clamped to a fabricated positive radius.

The literal shader has 64 point/color/range slots. The type-specific authored bound admits 64 groups of three
controls and one synthetic unit, for 256 declarations. A 65th group is refused by
Compile with LimitExceeded. The private executor also retains the shader-slot
guard for direct invocations that bypass document validation. Zero distance normalization, zero Gaussian/Linear
range, zero or nonfinite normalization sums and nonfinite weights receive named
Unsupported diagnostics. These paths are undefined or nonfinite in the shader;
this CPU profile does not guess their GPU result. Unresolved inherited/default
surface formats use the existing shared diagnostic. Public evaluation preserves
the caller's prior output on every failure.

Before the first row allocates pixels, a conservative scan of retained original
Dimension values admits maxima across all processor rows. The bound covers three
point passes plus one output write per pixel, multiplied by the full processor
row count, within 64 million work units. Gaussian and Linear use fewer passes;
this bound deliberately covers Exponential for every mode. Scans allocate no
workspace and respect the document's existing validated array-tree limits. Point
controls and weights use fixed 64-slot stack arrays. Output storage uses the
existing shared byte admission. Point row selection, Loop/Hold/Expand processing,
key evaluation and document persistence use the existing engine paths.

The focused graph suite checks literal outputs for all modes, independent source
alpha, signed influence, raw fractional dimensions, Reference units, dynamic
color keys, point processor rows, save/load, the shader slot boundary, and atomic
refusal for undefined formulas and an expensive later Dimension row. Strict
syntax uses actual CMake C++20 module/test flags. A standalone syntax check is not
joined runtime acceptance.

Pinned constructor and shader provenance:

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| `scripts/node_gradient_point_n/node_gradient_point_n.gml` | 7587 | `4bf3c140c198df497f338f56132ca8aa517da39ba8bbe03818e18c8e7ad3f739` |
| `shaders/sh_gradient_points_n/sh_gradient_points_n.fsh` | 1670 | `1005d9d5928d4500222000180821beda29f36631efc4053319d1efb0cab4046a` |

Source revision: `b69eca232217360cf1502ef0223523d818606652`.
