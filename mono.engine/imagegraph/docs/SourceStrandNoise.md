# Source Strand Noise

`pc.noise_strand` follows pinned `b69eca232217360cf1502ef0223523d818606652`: `scripts/node_noise_strand/node_noise_strand.gml`, `scripts/__node_shader/__node_shader.gml`, `scripts/__node_shader_generator/__node_shader_generator.gml`, and `shaders/sh_noise_strand/sh_noise_strand.fsh`.

Grug keep source float equations. Strand count truncates Density times the selected raw axis dimension into a signed shader integer. Each strand uses the source sine hash with Seed modulo 100000 divided by 10. Independent hashes pick its initial point, slope, opacity, and curve strength. The curve uses Curve Scale, the other dimension, and Curve Shift times the source tau.

Position Reference units use the first prepared Dimension row. Linked surfaces expose their dimensions and bypass units; whole SurfaceArrays project `[1,1]`. The shader adds Position directly to aspect-adjusted normalized UV. Grug keep this unusual source unit behavior.

Line takes the maximum smooth line times Opacity. Band uses the source step expression and permits zero Thickness. Area adds smooth one-sided coverage divided by strand count and ignores Opacity. Source scalar choices clamp through the source getter. Converted array choices can reach other integer values: nonzero Axis uses Y; modes outside Line, Band, and Area still compute strand coordinates but leave brightness zero.

Covered positive loops require explicit resolved Seed. Line and Area refuse Thickness when the binary32 smoothstep edges are equal or reversed, as specified by the [GLSL smoothstep contract](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#common-functions). Signed integer conversion overflow, nonfinite shader arithmetic, equal Level In endpoints, and unrepresentable floating output receive named diagnostics. Zero or negative strand counts skip loop controls and Seed, then apply levels and UV alpha.

The generic generator clears its target and draws a raw-dimension sprite. Allocation rounds half-even and clamps to at least one pixel. Pixel centers outside that sprite remain clear. Wholly uncovered draws skip shader controls. UV sampling uses raw sprite coordinates, flips mapped Y, mixes coordinates, and keeps sampled alpha even at zero UV Mix. Raw UV and Mask Atlas bindings refuse.

All seven source surface depths are supported. Mask Alpha Only is inert. Masking reads selected typed output, multiplies alpha by mean mask RGB times mask alpha, writes default RGBA8 scratch, then copies back to selected depth. Requested scalar two-dimensional fields own a copy of the stored red raster.

Every selected processor row is admitted before output allocation or observer calls. Covered allocated pixels cost `512 + 512 * max(strandCount, 0)` scalar work units. Wholly uncovered pixels cost 512. The whole batch cap is 64 million. Conservative retained bytes include target storage, metadata, mask scratch, and optional field raster. Actual mask scratch charges four bytes per pixel.

Grug claim pinned equations and bounded native behavior. CPU and GPU math can differ. Licensed runtime pixel parity and measured speed are not claimed.
