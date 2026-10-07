# Source Perlin

`pc.perlin` runs the pinned `Node_Perlin` and `sh_perlin_tiled` equations from `b69eca232217360cf1502ef0223523d818606652`. Source files are `scripts/node_perlin/node_perlin.gml`, `shaders/sh_perlin_tiled/sh_perlin_tiled.fsh`, and the shared shader, value, drawing, surface, and mask helpers.

Grug keep the source hash. It smoothsteps the absolute signed fractional sine hash, adds Phase divided by 360 in float, then makes a gradient angle with 6.28319. Four corner gradients and cubic fractional easing feed sequential mixes. This is not a generic Perlin library.

Tile floors both Scale lanes and wraps lattice corners by the current octave scale. Its initial coordinates use fract of UV minus normalized Position. Tile ignores Rotation. Untiled coordinates use the source row-vector rotation, with degrees converted to radians after float upload. Each octave multiplies position and scale by Scaling and its weight by Amplitude.

Add sums weighted noise and divides by accumulated weights. Max starts at zero and keeps the greatest weighted sample. Add with nonpositive Iteration refuses its source zero-over-zero result. Max with nonpositive Iteration applies levels to zero. A consumed zero weight total, zero tiled modulus, equal input levels, shader overflow, or floating storage overflow receives a named diagnostic.

Greyscale evaluates one channel. RGB and HSV evaluate three source coordinate offsets, apply levels inside each Perlin call, and then apply the three color ranges. HSV uses the pinned shader conversion. Values are not clamped before typed output storage.

Scale is two scalar shader lanes. An active Scale Map mixes those two endpoints by nearest mean RGB, then uses the same value on both axes. It is not four-lane per-axis range interpolation. The corrected native Scale Map Range default is `[4,4]`. Authored or linked Scale pairs supply actual source lanes; scalar values duplicate. Nested mapped Scale arrays refuse because source vector depth plus mapped depth retains a nested array for a vec2 uniform upload. Unmapped Scale tuple arrays use ordinary processor row selection.

Scaling and Amplitude duplicate selected unmapped scalars. Their active mapped ranges use source endpoints, with nearest mean RGB ignoring map alpha. Missing maps use the low endpoint. The helper always sets map flags, so there is no guessed stale uniform state. Unlinked catalogue defaults use the mapped source range only when the mapped toggle is enabled.

Position Reference units use the first prepared Dimension row. Linked surfaces expose dimensions and bypass units, while linked whole SurfaceArrays project `[1,1]`. `draw_empty` uses allocated half-even dimensions for UV coordinates. Raw Dimension still divides Position. UV mapping flips Y, mixes coordinates, and retains UV alpha even at zero Mix.

All seven explicit source depths are supported. Raw Atlas UV, Mask, and consumed numeric map bindings refuse. Mask Alpha Only is inert. Source masking reads the selected stored output, multiplies alpha by mean mask RGB times mask alpha, writes default RGBA8 scratch, then copies back to selected depth. Grug keep this precision loss. An explicitly requested noise field owns the generated raster as a two-dimensional scalar recipe. Sampling follows the stored red channel and does not change the surface output.

The selected batch is quoted before any output or observer call. Each pixel costs 512 scalar units plus 2048 per positive iteration per evaluated channel. The complete batch cap is 64 million units. Conservative retained bytes include maximum selected dimensions, 16-byte target pixels, metadata, optional mask target, and requested field raster copy. Actual mask scratch charges four bytes per pixel. Admission scans mapped octave scales and Add weight totals before allocation. Seed must be explicit.

Grug claim source equations and bounded native behavior. CPU math functions and GPU float behavior can differ. Licensed runtime pixel parity is not claimed.
