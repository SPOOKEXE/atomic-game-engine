# Dotted CPU shader profile

`pc.dotted` implements `Node_Dotted` at source pin
`b69eca232217360cf1502ef0223523d818606652`. The native profile evaluates the
pinned GLSL expressions in double precision, at pixel centers, with nearest
clamped texture reads and white vertex colour. It owns only its output images.
Desktop float arithmetic, trigonometry, raster coverage, ambient drawing colour
and device texture swizzles remain separate parity gates. No GPU or licensed
GameMaker execution is claimed by these CPU fixtures.

## Source inputs

| Source slot | Durable controls | Behaviour |
| --- | --- | --- |
| 0, 1 | `dimension`, `dimension_unit`, `mask` | Raw physical dimensions drive shader geometry; Pixel, Project and Mask units resolve before half-even surface rounding. |
| 21, 22 | `uv_map`, `uv_mix` | Interpolates normalized coordinates with map red and inverted green. Map alpha multiplies final alpha independently of UV Mix. |
| 14 | `position`, `position_unit` | Pixel or Reference translation, then normalized rotation around zero. |
| 4, 5 | `angle`, `angle_mapped`, `angle_map_range`, `angle_map` | Degrees; a mapped read uses original pixel coordinates and mean RGB. |
| 13 | `pattern` | Grid or Hexagonal. Hexagonal adjusts row spacing and alternates half-column offsets. |
| 2, 3 | `size`, `size_unit`, `size_mapped`, `size_map_range`, `size_map` | Scalar Reference Size multiplies by raw canvas width. Float arrays remain unscaled by the pinned unit getter. |
| 15 | `spacing` | Anisotropic spacing appears twice in the search-extent calculation. |
| 9, 10 | `dot_size`, `dot_size_mapped`, `dot_size_map_range`, `dot_size_map` | Dot threshold reads at cell origin plus Position, not the original pixel. Numeric maps ignore alpha. |
| 6, 12 | `render_mode`, `smoothness` | Pixel step, AA smoothstep or Smooth distance ramp. Smoothness only affects AA. |
| 7, 8, 11 | `bg_color`, `dot_color`, `intensity` | Background, Solid colour and accumulated dot intensity. |
| 23 | `blend_mode` | Normal uses the shader's composed-alpha division. Additive adds all foreground components directly. |
| 16–20, 24 | `dot_color_mode`, `palette`, `gradient`, `texture`, `seed`, `shift` | Solid, ordered Palette, shader-hashed Random gradient or cell-origin Texture. Shift wraps Random gradient progress. |
| attributes | `attribute_process`, `attribute_array_process`, `attribute_color_depth` | Existing bounded processor scheduling and all seven named surface formats. Dotted declares no Active, Mix or Channel input. |

Mapped mode preserves a two-component shader uniform. An absent map uses its
first endpoint. Static native range fields represent the source numeric slot;
linked `pc.array` endpoint pairs retain depth one. Explicit scalar values are
duplicated. Numeric endpoint pairs use linked source arrays or static synthetic
range fields; a `Vector2` authored on the numeric scalar property is rejected. This differs from interpreting a fixed range as processor rows.
The source Float unit conversion leaves depth-one Float arrays unchanged,
whereas a scalar uses the reference vector's first component. Position follows
the source vector conversion and uses both reference dimensions.

## Kernel and source quirks

Search extents are `min(16, ceil(dimension / min(Size endpoints) / Spacing²))`.
For each neighbour, distance is twice its distance from `(0.5,0.5)`. Pixel uses
`step(1-threshold, 1-distance)`. Equality is covered, so a zero Pixel
threshold still covers a pixel exactly at its dot center. AA uses ordered
smoothstep edges separated by
Smoothness. Smooth uses `max(0, threshold-distance)` and returns zero for a zero
threshold. Negative finite extents produce the source empty neighbour loop.

Palette uses shader modulo of row plus column followed by integer truncation.
Random uses the literal sine hash constants `1892.9898`, `78.23453`, `437.54123`
and `100000`, then the pinned shader gradient interpolation. It does not replace
GameMaker's builtin RNG. The GLSL limits are 256 Palette colours and 64 Gradient
keys; HLSL's larger arrays are outside this profile.

Solid retains its RGB even when coverage is zero. Consequently Additive Solid
adds that RGB outside the dots. Other colour modes multiply all four components
by coverage before Normal blending. Normal uses
`alpha = fg.a + bg.a * (1-fg.a)` and divides composed RGB by that alpha; zero
composed alpha returns zero. It does not introduce an extra normal draw blend,
because `surface_set_shader(..., BLEND.over)` selects source override factors.

`mask_apply_empty` is a separate draw through RGBA8 and back into the selected
format. The native implementation fuses those independent per-pixel passes but
preserves both quantization boundaries. RGB remains unpremultiplied while alpha
uses mask mean RGB times mask alpha. The synthetic `mask_alpha_only` flag is
inert because this source call passes no input junction. For single-red output,
`draw_surface_safe` selects the red draw shader instead of the mask shader:
the intermediate is grayscale with alpha one, and this mask is ignored.

## Admission and explicit boundaries

Before the first output allocation, row zero visits original dimensions, unit
choices, masks, Size values/ranges, Spacing, colour modes, gradient key counts
and the resolved static/inherited output format. Color Depth is a synthetic
attribute with no processor source index; authored arrays are rejected, and
linked depth requires conditional format routing that this graph contract
rejects. It is not a later-row numeric control. Conservative independent extrema admit at most 64 million
neighbour/key visits across the whole processor batch. They also admit every
output row at the largest row layout. Input payloads remain charged by the
shared evaluator. Actual images use the existing capacity-aware `NewImage`
reservation. No per-pixel vector or palette/gradient clone is allocated.

Authored empty Gradient keys fail public document admission before execution.
Undefined divisions/nonfinite samples, selected empty runtime Palette/Gradient indexing,
and selected missing Texture samplers fail explicitly. Random mode requires an
authored seed. Missing textures are not replaced by an invented colour. AA with
nonpositive Smoothness refuses unordered/equal smoothstep edges, which GLSL
[defines as undefined](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.1.20.pdf).
The shared disabled-processor array boundary remains explicit; this executor
does not invent a raw uniform upload shape for that route.

The draft includes 25 core cases and one IO case covering all three mapped
source slots. Literal Grid, Hexagonal, AA and Smooth byte goldens come from an
independent scalar formula script. Compiled graph, linked pairs, heterogeneous
rows, persistence, format, alpha and atomic work/byte failure fixtures require
the joined runtime gate. Strict syntax alone is not runtime acceptance.

## Pinned provenance

| Source | SHA-256 |
| --- | --- |
| `scripts/node_dot_pattern/node_dot_pattern.gml` | `881248ecf0325f5ce1ec1902aef4799afb26eeb63f06c785302c7dd090e495f8` |
| `shaders/sh_dotted/sh_dotted.fsh` | `459ec06875dde800d9a6557b555003f5d7c77993fe996e6f6ccc56166d552221` |
| `scripts/node_value/node_value.gml` | `6ca786dca5c9bb7a9116faaee9e8d24e0e5c86aea71ac376230fbccb491e17dd` |
| `scripts/node_value_types/node_value_types.gml` | `9574e5f50450dd94cfc682a346f586880271a8985b9cc3205da697811884fddf` |
| `scripts/node_processor/node_processor.gml` | `9e3513102079f0ca9cb9dcf58cce898cb2ac80f963415e65d8ac6ba017f827cc` |
| `scripts/shader_functions/shader_functions.gml` | `3fc33618476756eec19830b3e1ef62b521637a443cefe366000bb890372e1cc7` |
| `scripts/mask_function/mask_function.gml` | `94c22c8b1d3085c8ada6c673e5836326a8c866579b35ebb4487c343fb4f94023` |
| `shaders/sh_mask_empty/sh_mask_empty.fsh` | `aeacb3cf2133dfe1f3b3adf2d3aabde00a0b177461085bb25b65e5620d40bf6f` |

Derived code retains the module's distributed Pixel Composer MIT notice.
