# Source Extra Voronoi

`pc.voronoi_extra` runs pinned `Node_Voronoi_Extra` and `sh_voronoi_extra` equations from `b69eca232217360cf1502ef0223523d818606652`. Source paths are `scripts/node_voronoi_extra/node_voronoi_extra.gml`, `scripts/__node_shader/__node_shader.gml`, `scripts/__node_shader_generator/__node_shader_generator.gml`, and `shaders/sh_voronoi_extra/sh_voronoi_extra.fsh`. The shader copyright and permission notice is retained in `SourceVoronoiExtra-LICENSE.txt`.

Block combines four animated triangular metrics, including the source inner column-vector rotation. It ignores Seed and Tile. Triangle uses the seed-dependent two-lane sine hash, animated points, triangle edge axes, and source axis permutations. Its searches visit 25 candidates then 49 border candidates. It must find a winner below the source distance-eight cutoff before reading the winner.

Mode has a two-item EScroll menu: Block and Triangle. Source scalar choices clamp to that range. Source array choices bypass the scalar clamp, so array Mode 2 reaches the hidden Square shader branch. Square uses nine nearest candidates then 25 bisector candidates, with Parameter A scaling point offsets. It also refuses a missing nearest winner. Other represented integer array modes keep the source default zero value and apply levels.

Only Triangle consumes Tile. Tiling floors Rotation down to quarter turns and floors each Scale divided by four. Triangle hashes wrapped cells modulo twice this scale. Untiled Triangle, Block, and Square use Rotation degrees converted to radians in float and Scale divided by four. The main transform uses the source row-vector matrix. Equal input levels, consumed zero tiled modulus, shader overflow, and unrepresentable floating storage refuse with named diagnostics.

Position Reference units use the first prepared Dimension row. Linked individual surfaces expose their dimensions and bypass units; linked whole SurfaceArrays project `[1,1]`. Raw Dimension supplies aspect, Position division, sprite coverage, and texture coordinates. Output allocation rounds half-even and clamps each dimension to at least one. The source clears its target before drawing the raw-size sprite. Uncovered pixel centers stay clear, including zero, negative, or too-small raw dimensions. These pixels do not consume Seed or levels.

Covered Triangle and Square require an explicit resolved Seed. Block needs no Seed because its shader path never reads it. UV mapping reads nearest, flips Y, mixes coordinates, and retains UV alpha even at zero Mix. Raw Atlas UV and Mask bindings refuse. All seven explicit source depths are supported. Mask Alpha Only is inert. Masking reads the selected stored output, multiplies alpha by mean mask RGB times mask alpha, writes default RGBA8 scratch, then copies back to the selected depth. Grug keep that precision loss.

An explicitly requested field owns the generated raster as a two-dimensional scalar recipe. Sampling uses the stored red channel. Field output does not change the surface result.

Admission quotes every selected row before output or observer calls. Each allocated pixel costs 512 base scalar work units, plus 512 for Block, 74 times 256 for Triangle, or 34 times 192 for Square when the sprite covers any pixel. Other modes and wholly uncovered draws use only the base cost. The complete batch cap is 64 million work units. Conservative retained bytes include maximum selected dimensions, 16-byte target pixels, metadata, optional mask target, and requested field raster copy. Actual mask scratch charges four bytes per pixel.

Grug claim source equations and bounded native behavior. CPU math functions and GPU float behavior can differ. Licensed runtime pixel parity is not claimed.
