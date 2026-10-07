# Source Caustic

`pc.caustic` runs the pinned `Node_Caustic` generator and `sh_water_caustic` shader math on the CPU. Source pin is `b69eca232217360cf1502ef0223523d818606652`.

Source files are `scripts/node_caustic/node_caustic.gml`, `scripts/__node_shader/__node_shader.gml`, `scripts/__node_shader_generator/__node_shader_generator.gml`, `scripts/shader_functions/shader_functions.gml`, and `shaders/sh_water_caustic/sh_water_caustic.fsh`.

Grug keep source float math. Each detail samples simplex three times and uses gradient lanes for two warps. Source uses coefficient minus six, unnormalized gradients, seed-dependent permutations, and exponential accumulation. Native code keeps those choices. This is not classic scalar simplex.

Position and Scale resolve Pixel or Reference units before float upload. Reference units use the first prepared Dimension row. A linked individual surface exposes its dimensions and bypasses units. A linked SurfaceArray collapses to `[1,1]` and bypasses units: source Vec2.getValue passes the whole array to surface_get_dimension, whose is_surface check excludes arrays. Raw Dimension controls sprite coverage, texture coordinates, aspect ratio, and transform. Output allocation rounds half-even and clamps each dimension to at least one. The source clears its target before drawing, so uncovered pixel centers stay transparent zero. Zero or negative raw dimensions can leave the whole target clear. Zero Scale refuses at its named port only when a covered pixel and positive Detail need the transform. Nonpositive Detail keeps zero RGB without evaluating its unused transform or amplitude.

Covered positive Detail requires both Intensity Mapped and Progress Mapped enabled. Their source helper uploads both uniform lanes and resets missing-map flags. Scalars duplicate into both endpoints; mapped ranges keep their two endpoints. Active maps sample nearest at the original texture coordinates. Mean RGB chooses the range value and ignores map alpha. Missing maps use the low endpoint. UV mapping flips Y, mixes coordinates, and retains UV alpha even at zero UV Mix. Raw Atlas bindings refuse for UV, Mask, and consumed numeric maps.

Grug refuse the source unmapped branch at the relevant control. `Node_Shader.setShader` uploads one scalar into the vec2 uniform there and leaves its UseSurf flag unchanged. Neither the prior flag nor driver behavior is captured by this evaluator. Guessing fresh uniform state would invent pixels. No-fragment draws and nonpositive Detail ignore these controls, so those branches do not require mapped toggles or parse unused ranges.

All seven explicit source depths are supported within this defined seam. Mask Alpha Only is inert. Masking reads the first stored output, multiplies alpha by mean mask RGB times mask alpha, writes a default RGBA8 scratch surface, then copies back to the selected depth. Grug keep this storage step because float output still loses precision when masked. The processor calls the generator once per output row. Its array check describes multiple render targets, so the mask still runs on each processor row.

Seed must be explicit. Nonpositive Detail loops zero times and produces zero RGB. Positive Detail on a covered canvas needs a finite source float amplitude. Nonfinite controls, derived shader overflow, and unrepresentable floating output refuse with a named diagnostic.

The selected batch is quoted before any output allocation. Work is 512 scalar units per pixel plus 4096 per positive detail when the sprite covers a pixel, capped at 64 million for the complete batch. A conservative whole-batch byte quote includes the largest selected dimensions, 16 bytes per pixel for each retained target, metadata, and optional mask scratch. Actual mask scratch charges four bytes per pixel. No procedural noise field output is added.

Grug claim source equations and bounded native behavior, not licensed runtime pixel parity. CPU math functions and GPU float behavior can differ.
