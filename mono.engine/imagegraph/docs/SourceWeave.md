# Source Weave

`pc.weave` follows pinned `b69eca232217360cf1502ef0223523d818606652`: `scripts/node_weave/node_weave.gml`, `scripts/__node_shader/__node_shader.gml`, `scripts/__node_shader_generator/__node_shader_generator.gml`, `scripts/shader_functions/shader_functions.gml`, `scripts/gradients_function/gradients_function.gml`, and `shaders/sh_weave/sh_weave.fsh`.

Grug keep source equations in binary32. Raw sprite UV subtracts Position divided by Dimension, rotates with the source row-vector matrix ([GLSL expressions](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#expressions)), and divides by Scale divided by Dimension. Position and Scale Reference units use the first prepared Dimension row. Linked individual surfaces expose dimensions and bypass units; whole SurfaceArrays project `[1,1]`.

Width selects horizontal and vertical thread coverage. Empty coverage returns BG Color before reading weave, random color, or shading controls. Random weave blends two seeded sine hashes. Checker uses the source signed GLSL modulo. Map computes inverse-rotated cell-center coordinates and selects an axis from nearest red times alpha. Neighboring cells decide which thread shades the other.

Solid and Axis colors use source Color and Color 2. Random color hashes its selected thread axis, applies Shift, and evaluates the uploaded gradient. The native float gradient retains all seven shader blend modes, including inverse HSV hue, cone-space blend, gamma blend, and CMYK. Undefined consumed gradient arithmetic receives a named diagnostic. Empty gradients and layouts beyond the GLSL 64-key limit refuse.

Shading uses the pinned curve region through the existing binary32 curve helper. Curves support the GLSL layout of two through nine anchors. The shader calls curveEval without checking shading_curve_use. The host setter skips curve and amount uploads when Shading Curved is disabled. A reachable shading call therefore requires Shading Curved enabled. Grug refuse missing source state rather than make up a linear curve. Branches that never shade can leave the toggle disabled.

Map texture sampling uses sampleMode outside `[0,1]`, but this generator never uploads that uniform. Consumed outside coordinates and missing Weave Map bindings refuse. In-domain nearest Map samples remain defined. Raw Atlas UV, Mask, and consumed Weave Map bindings refuse. Seed is required only for reachable random weave or random color branches. Zero Scale divisors, reachable zero weave modulo divisors, nonfinite math, and unrepresentable floating storage receive named diagnostics. Converted source array choices retain the shader's fallback branches.

The generic generator clears its target and draws a raw-dimension sprite. Allocation rounds half-even and clamps to at least one pixel. Pixel centers outside the raw footprint stay clear. Wholly uncovered draws skip shader controls. UV mapping flips Y, mixes coordinates, and retains UV alpha even at zero Mix.

All seven explicit surface depths are supported. Mask Alpha Only is inert. Masking reads selected typed output, multiplies alpha by mean mask RGB times mask alpha, writes default RGBA8 scratch, then copies back to selected depth.

Admission quotes all selected rows before output or observer calls. Covered allocated pixels cost 8192 scalar work units; wholly uncovered pixels cost 512. The whole selected batch cap is 64 million. The quote includes bounded curve and gradient scans, the preallocation branch proof, and draws. Conservative bytes include retained typed outputs, row metadata, and optional mask scratch. Scratch charges four bytes per pixel while live.

Grug claim bounded native behavior for defined source branches. CPU and GPU math can differ. Licensed runtime pixel parity and measured performance are not claimed.
