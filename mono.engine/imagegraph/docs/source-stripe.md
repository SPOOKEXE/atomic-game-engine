# Stripe CPU shader profile

`pc.stripe` implements pinned `Node_Stripe` and `sh_stripe` at
`b69eca232217360cf1502ef0223523d818606652`. This is a native CPU formula profile:
pixel-center coordinates, double arithmetic, nearest clamped numeric/UV reads,
filtered gradient maps and white vertex color. It owns its output images and
borrows resolved inputs only for evaluation. Desktop float/trigonometry, raster
coverage, vertex color and device texture swizzles remain external parity gates.
No licensed source execution or GPU comparison is claimed.

| Source slots | Durable controls | Source behavior |
| --- | --- | --- |
| 0,20 | `dimension`, `dimension_unit`, `mask` | Pixel, Project or Mask physical canvas; half-even output rounding and minimum 1 preserve raw geometry. |
| 21,22 | `uv_map`, `uv_mix` | Red/inverted-green UV interpolation; map alpha multiplies output independently of UV Mix. |
| 1,11 | `size`, `size_unit`, numeric map controls | Shader amount is raw canvas width divided by Size. |
| 2,12 | `angle`, numeric map controls | Degrees, sampled at original pixel UV before procedural UV remapping. |
| 10,14 | `strip_ratio`, numeric map controls | Mean RGB threshold map, then literal `.001` epsilon. |
| 5,13 | `random`, numeric map controls | Stateless sine-hash jitter of neighboring stripe boundaries. Numeric map alpha is ignored. |
| 4 | `position`, `position_unit` | Physical translation normalized by raw canvas dimensions. |
| 17 | `progress` | Fractional phase offset after jittered-cell normalization. |
| 23,24 | `tiled`, `amount` | Uploaded uniforms are never read by the pinned shader. Both are inert, even though the inspector toggles Size/Angle visibility. |
| 3,6 | `type`, `coloring` | Solid/Smooth/AA and Alternate/Palette/Random. |
| 8,9 | `color_1`, `color_2` | Alternate colors, including straight alpha. |
| 18 | `colors_2` | Ordered GLSL palette with 1 to 256 colors. |
| 7,15,16,25 | `colors`, gradient map controls, `shift` | Pinned gradient interpolation or filtered mapped gradient; wrapped stateless-hash progress. |
| 19 | `seed` | Explicit authored source seed when jitter or Random coloring can observe it. No replacement builtin RNG is invented. |
| attributes | process, array process, color depth | Bounded source processor scheduling and static/inherited surface format. No Active/Mix/Channel input exists. |

Numeric maps preserve one two-component source shader uniform. Native static
range controls represent that physical source slot; linked `pc.array` pairs
retain depth 1. Scalar values are duplicated. With no map, only the first
endpoint is read. `Vector2` cannot be authored directly on a scalar property.
Source unit conversion leaves Float/Slider-display endpoint arrays unscaled.

`Node_Processor.getInputsProcess` calls getters before selecting processor rows,
with the default `arrIndex=0`. `setUnitSimple` references `node.getDimension(0)`.
Scalar Reference Size therefore uses the first prepared physical Dimension's
width; Reference Position uses both first dimensions across later canvas rows.
Numeric Position links still apply units. Surface Position links return their
dimensions directly and bypass unit conversion.

The shader rotates normalized position with aspect `width/height`, takes a
floor slot, then jitters its two boundaries with the exact hash constants
`12.9898`, `78.233`, `43758.5453123`, `100000` and `/10`. It takes the fraction
of the normalized jittered position plus Progress. Alternate Solid uses
`phase >= ratio+.001`; Palette/Random choose bands with strict `>` instead.
Alternate Smooth uses the shader's literal PI sine; AA uses smoothstep edges
`+-3/max(raw width,raw height)` and ignores Ratio. Palette AA is identical to
Solid. Palette Smooth intentionally does not normalize its interpolation by
band width. Random coloring ignores Type and uses wrapped hash plus Shift.

`surface_set_shader` uses BLEND_ALPHA on transparent clear, preserving shader
RGBA. `mask_apply_empty` then draws through RGBA8 and back to the selected
format. Mean mask RGB times alpha affects output alpha; `mask_alpha_only` is
inert because no input junction is passed. Single-red safe drawing replaces
the mask shader, expands grayscale with alpha 1 and ignores this mask. Both
quantization boundaries remain observable in floating outputs.

Before the first allocation, row 0 preflights all original dimensions, units,
linked dimension images, masks, gradient key counts and processor row count.
Conservative independent extrema admit at most 64 million weighted pixel/key
visits and every output row at the largest layout. Output bytes use the resolved
static/inherited format, not an invented per-row Color Depth. The shared ledger
charges retained inputs and capacity-aware output allocation. There are no
per-pixel vectors or palette/gradient clones.

Zero divisors, nonfinite cell arithmetic, unordered AA edges, selected empty
runtime palette/gradient indexing and unobserved stochastic seeds fail with
producer/port diagnostics. Authored empty gradients fail `InvalidValue` during
public document admission. GLSL's 64 gradient keys and 256 palette colors are
explicit shader limits; larger HLSL arrays are a separate backend gate. Disabled
processing supports scalar controls; unprocessed outer arrays retain the shared
processor's explicit unsupported boundary. Source synthetic units, map flags
and Mask Alpha Only do not invent physical slots in inverse array scheduling.

Source SHA256:

- node_stripe.gml: `71e6366eba4c4f152bd048e8153c03f9472c5f2927b71b20664fd1aecb170b82`
- sh_stripe.fsh: `a750cf9419347a2c21565877f69e328d7273bdbaf1b0064bbe5d032949cde125`

The tests contain compiled graph goldens for all color/edge modes, source inert
controls, stateless hash, maps/ranges, dimensions/units, source instances,
persistence, all four schedules, heterogeneous rows and public work/byte
atomicity. IO tests edit the four nonadjacent numeric source slots and the
Gradient map flag while preserving unknown archive fields. Joined release65 CPU validation passes the Stripe/Dotted fixtures and both
IO mapped-range cases. Source GPU comparison, sanitizers and measured profiling
remain open.
