# Native spatial gradient profile

`pc.gradient_grid` and `pc.gradient_points` produce actual bounded CPU surfaces. They use the normal processor row evaluator and copied authored controls. They have no source mask socket. Their images can feed ordinary masked compositor nodes. `pc.gradient_points_n` is a separate source node and is outside this implementation.

## Source provenance

The source project is pinned at `b69eca232217360cf1502ef0223523d818606652`.

| File | SHA-256 |
| --- | --- |
| scripts/node_gradient_grid/node_gradient_grid.gml | 9fe9e6fc350e91b424c3e38ec5d60eff6a6b8de0995a746dae243bd587dff7db |
| scripts/node_gradient_points/node_gradient_points.gml | a847ed36612ee9c81322c1bb7e19f87a27c355ad625b632285cfffff0642db57 |
| shaders/sh_gradient_points/sh_gradient_points.fsh | 60a458b2e03144a76102b84e64c166ba1631595cac7ddf759d1ccc162e72e579 |

Grid follows `processData` literally: ordered anchor/color pairs, bilinear subdivision, clamped color remapping, nested RGB byte truncation, independent alpha interpolation, and in-place nine-neighbor geometry smoothing in source row order. The final corner uses the color-remapped x fraction in geometry, preserving the source quirk. It emits the two source triangles per cell in source order with override blending. The source `preGetInputs` resets a mismatched anchor count to regular white anchors; native evaluation renders that reset result without mutating the authored document. Native dynamic IDs retain catalogue template identities, including `anchor_i_0`, `anchor_i_unit_0`, and `color_i_0`.

The byte truncation follows `merge_color` in the official GameMaker HTML5 runtime pinned at `60e51be51ce7f3d52025ef18106cf172b8e22a00`, `scripts/functions/Function_Graphics.js`. Native triangle samples use pixel centers, inclusive barycentric edges, both windings, degenerate-triangle omission and ordered overwrite. Exact licensed desktop primitive raster edge rules and byte conversion remain external reference gates.

Four Points follows the literal shader. It divides resolved pixel centers by the unrounded source dimension; uses twice the greatest distance as the falloff denominator; and computes each strength exponent. `Normalize weight` true chooses the source L2 vector normalization, while false divides by the weight sum. RGB mixes all four components directly. OKLAB uses the shader's literal LMS matrices, cube roots and 2.2 powers, with alpha mixed independently. Palette selection uses the first four entries with opaque black for missing entries. UV sampling uses native nearest/clamp addressing, flips sampled green, interpolates by UV Mix, and applies sampled alpha even when UV Mix is zero. Supported surface formats retain normal native storage behavior.

Zero maximum distance, zero/nonfinite normalization, and nonfinite shader power results return a named diagnostic atomically. The shader's inverse LMS coefficients can produce negative roundoff for saturated primary colors before its fractional power; native evaluation reports that undefined source result instead of inventing a clamp. GPU float precision, source texture filtering state, primitive raster behavior and device-specific handling of undefined shader arithmetic remain reference gates. This CPU profile is not a claim of GPU or licensed source-executable parity.

## Bounds and ownership

All values and images belong to the evaluation result or caller-owned request. No renderer, filesystem handle, RNG state or host pointer is retained. The kernels use the existing node execution profiling boundary.

The first processor row performs an allocation-free work preflight over borrowed original dimension/unit/subdivision/smoothing values, nested source arrays and dimension image arrays. Conservative maxima are multiplied by the complete processor count before any generated output or vertex workspace is allocated. This may refuse heterogeneous batches whose combination of maxima exceeds the native bound; it does not change accepted rows' selected controls. Grid admits topology and work before allocation: positive integral Grid/subdivision, at most 4096 vertices, at most the native link limit in triangles, and 64 million admitted work units across the entire processor batch. One triangle/pixel test and one smoothing neighbor each cost one unit; subdivision/anchor preparation costs ten units per vertex. This bounds work without claiming an instruction count. Workspace for vertices and anchors is reserved before growing vectors. Actual vector capacities are charged before geometry loops. Grid port names use bounded stack buffers and Four Points uses literal control IDs, so per-control string construction allocates no heap storage. Palette color validation borrows leaf values without copying rejected payloads. Its Grid input rejects arrays as the source constructor does. Other source controls follow processor row expansion.

Four Points bounds its four distance calculations to 64 million across the entire image batch before allocating the output. UV, colors, dimensions and animation observations are resolved through existing source getters and processor row selection. Publication occurs only after successful evaluation; overflow, undefined controls and work-budget refusals preserve prior caller results.

## Verification

Headless suites use actual Compile/Evaluate/EvaluateArray graphs. Literal tests cover RGB/LMS colors, independent alpha, sum/L2 weighting, short palettes, UV XY/alpha and mix, non-square unrounded dimensions, color batches, source triangles, nested color truncation, geometry smoothing, the final-corner quirk, default/reset geometry, persisted graphs, animation seeks, a masked compositor consumer and whole-batch/topology refusals preserving previous images. Expensive second-row dimension, subdivision and smoothing cases use a byte cap smaller than the first output and require a work-specific diagnostic, distinguishing preflight refusal from an allocation failure. Accepted heterogeneous rows match independent single-row evaluations. Live Studio, GPU comparison and licensed source output comparison were not run.
