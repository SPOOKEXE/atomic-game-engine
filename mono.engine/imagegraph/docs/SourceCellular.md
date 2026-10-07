# Source Cellular

`pc.cellular` implements all four types and all three source patterns from pin `b69eca232217360cf1502ef0223523d818606652`. Source files are `scripts/node_cellular/node_cellular.gml` and `shaders/sh_cell_noise`, `shaders/sh_cell_noise_edge`, `shaders/sh_cell_noise_random`, and `shaders/sh_cell_noise_crystal`, each with its matching `.fsh` file.

Grug keep separate source paths. Point finds nearest point distance. Edge finds a nearest candidate then measures bisector distances. Cell uses the winning point for grey or RGB hashes and applies its gap test. Crystal finds the second-nearest squared distance in a 3D neighborhood. Crystal ignores Pattern, Phase, radial controls, Gap, and Colored. Its Minimum loop changes nothing before the final inversion.

Edge and Cell second grid passes always wrap by maximum Scale and omit Randomness, even in Uniform pattern. Cell starts its nearest search at distance one. If no candidate wins, source leaves later-read values uninitialized. Native execution refuses this case instead of inventing a winner. The Edge search has the same guard at its distance-eight cutoff.

Cell applies amplitude before its gap test, then applies amplitude again for Additive. Grey Cell inversion happens before levels; colored Cell ignores inversion. Minimum Cell inversion happens after levels. Point, Edge, and Crystal apply middle and contrast before levels, then inversion. Iteration zero or negative skips noise loops. Amplitude is validated only when consumed: Additive for Point, Edge, and Crystal; every blend mode for Cell.

The source shader helper always uploads two Scale lanes and resets its map flag. Unmapped selected scalars duplicate into both lanes. Mapped authored ranges retain both endpoints, while mapped authored scalars duplicate. Scale maps read nearest mean RGB and ignore alpha. Tiled Point, Edge, and Cell floor Scale and maximum Scale. Crystal ignores Pattern and does not floor Scale.

Dimension projects linked surface arrays before processor selection and collapses equal sizes. Position Reference units use the first prepared Dimension row. Linked surfaces expose dimensions and bypass units; a linked whole SurfaceArray projects `[1,1]`. Source `draw_empty` covers the allocated half-even dimensions, so pixel-center texture coordinates use the allocated size. Raw Dimension still controls aspect and position division. Rotation uses the source row-vector matrix. Phase divides authored degrees by 360 before float upload; Rotation converts degrees to radians before float upload.

UV mapping flips Y, mixes coordinates, and retains raw UV alpha even at zero Mix. Raw Atlas UV, Mask, and consumed Scale Map bindings refuse. Mask Alpha Only is inert. The output supports all seven explicit depths. Masking reads the selected stored output, writes default RGBA8 scratch, then copies back to the selected depth. Grug keep this precision loss.

Seed must be explicit. Consumed zero divisors, invalid radial float-to-int conversion, integer count overflow, undefined amplitude, equal input levels, missing nearest candidates, and nonfinite shader or storage results receive named diagnostics. Unused shader work is skipped when it cannot affect the result.

Admission quotes every selected row before the first output. Base cost is 512 scalar work units per pixel. Each consumed iteration adds 1024 plus 256 per candidate visit. Grid Point has nine visits, Edge and Cell have 34, and Crystal has 27. Radial quote counts every ring plus every candidate, with two passes for Edge and Cell. The complete batch is capped at 64 million work units. Whole-batch target bytes use a conservative 16-byte pixel quote plus metadata and optional mask target. Actual mask workspace charges four bytes per pixel.

Grug claim source equations and bounded native behavior. CPU sine and GPU float behavior can differ; licensed runtime pixel parity is not claimed.
