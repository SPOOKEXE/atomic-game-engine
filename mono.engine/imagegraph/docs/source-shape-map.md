# Shape Map CPU reference profile

Pinned Pixel Composer revision: `b69eca232217360cf1502ef0223523d818606652`. Both source shader `sampler_ext` regions are byte-identical. Source SHA256:

- `scripts/node_shape_map/node_shape_map.gml`: `9d41992fcc52bd4f094415f22edab64fffb03712ecab49b8faa32209823c318a`
- `shaders/sh_shape_map_circle/sh_shape_map_circle.fsh`: `fd281844acac751eababe08023400a060df039de94722126ffbaf99df4325a86`
- `shaders/sh_shape_map_polygon/sh_shape_map_polygon.fsh`: `47f461d30a8c1d4703b9f9fb4f9612959a8198e3fcdf6d4833b3308e462fa934`

Additional source evidence: `shader_functions.gml` interpolation/target setup, `surface_functions.gml` safe draw and `_draw_defines.gml` blend factors. Complete evidence hashes and sampler equality are retained in the review handoff.

The native executor processes the complete source node's Circle and Polygon routes. Circle uses half-centered normalized coordinates, radius `length(point)*2/scale`, and `(atan(y,x)+radians(angle))/PI` for its angular coordinate. Its rejection is only `radius>1`, so a negative scale retains the source's negative-radius behavior. Polygon divides centered coordinates by scale, adds PI to its angle, chooses the nearest angular sector with `floor(.5+angle/sector)`, and multiplies the distance by sqrt(3) only for three sides. It discards polygon distance outside [0,1]. The polygon's angular half-sector offset differs from Circle and is preserved.

Both routes multiply mapped angular/radial coordinates by Map Scale and apply floor-based `fract` before `texture2Dintp`. Radius is authored but never read by source processData or either fragment kernel. Oversample is set by the source interpolation helper but the kernels call `texture2Dintp` directly, so its outside-addressing switch is never used. No UV map, mask, channel, Mix, or inherited shader uniform is invented.

Pixel, Bilinear, Bicubic and the nine paired Lanczos3 taps follow the source sampler. Inherited attributes use the centrally resolved group/project settings. Inherited CleanEdge retains the existing native sampler's named unsupported diagnostic. The exact source GPU atan(0,0) result is unspecified; this native CPU profile uses atan2(0,0)=0 for odd-size center pixels. Native double arithmetic, pixel-center raster coverage, source GPU precision and that center behavior still require source/GPU comparison. This profile is not GPU parity acceptance.

`surface_set_shader` clears to transparent and uses source BLEND_ALPHA, whose factors preserve one fragment's raw RGBA over a transparent target. Clipped pixels remain transparent. Input, inherited and concrete output depths use the shared format resolver. Single-red source surfaces replace the active mapping shader through `draw_surface_safe` and are copied with red expanded into RGB and opaque alpha when the chosen target stores RGBA. Inactive processing copies the source surface and bypasses active divisors.

## Admission and failure

The first processor row scans the complete original ImageArray and borrowed original Surface/Atlas value arrays and combines its largest width-times-height with the whole ProcessorCount before allocating an output. A conservative64-unit cost per pixel covers mapping arithmetic, up to36 underlying Lanczos texel reads, output conversion and inactive copies. The entire batch is bounded by64million units, using division-safe admission. This conservative maximum includes clipped pixels and smaller rows; it does not estimate elapsed time. There are no producer-owned vectors or scratch allocations.

`NewImage` admits the selected format's exact layout bytes and durable output-port storage before allocation. The generic evaluator retains and charges previous selected publication, inputs, batch folding and candidate outputs, and publishes only on success. Byte or work refusal leaves previous published pixels unchanged.

Zero scale and zero polygon sides are named undefined-divisor refusals. Finite controls can still overflow derived coordinates; every mapped coordinate is checked before `fract` and texture reads. Lanczos paired-tap positions and normalization are checked before undefined coordinates can reach integer texel conversion. Signed scale and signed nonzero polygon sides otherwise retain source arithmetic. These guards belong to the new executor and do not modify the shared sampler.

## Validation

The executor and compiled-graph fixtures cover independent literal circle/polygon pixels, triangle radius, raw alpha, angle seam, signed repetition, unused controls, negative circle scale, sampling modes, safe red conversion, numeric depth, persistence and an invert consumer, array dimensions, inherited animated instance controls, repeated ticks, byte refusal and complete-batch work refusal. The tight-byte private executor fixture supplies a valid original second image or Atlas and proves work admission happens before output allocation.

Joined release65 CPU validation passes, including these 19 cases and the complete core suite (2,098 cases, 1,826,683 assertions). Initial invalid invert fixtures were repaired before acceptance. Sanitizers, measured profiling and source GPU comparison remain separate gates.
