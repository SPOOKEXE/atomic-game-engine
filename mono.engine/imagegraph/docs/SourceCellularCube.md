# Source Cellular Cube

`pc.cellular_cube` follows the pinned Pixel Composer constructor
`scripts/node_cellular_cube/node_cellular_cube.gml` and shader
`shaders/sh_cellular_cube/sh_cellular_cube.fsh` at commit
`b69eca232217360cf1502ef0223523d818606652`. Numeric fixtures use independent
binary32 shader-equation samples. They do not claim pixel parity with a GPU
runtime.

The node has fourteen source inputs and produces `surface_out` and
`cross_section`. The surface output ray-marches a cube or sphere. The cross
section samples a plane at the selected axis and position and is independent
of camera rotation and shape. Both outputs use the selected dimensions and
color depth. A ray miss retains sampled RGB in the surface output and sets its
alpha to zero; the cross section keeps alpha one on covered pixels.

The hash creates one feature point in each neighboring 3D cell. `cell` checks
the 27 cells around the current point and retains the nearest distance,
starting at one. The octave loop accumulates those distances with halving
amplitude. The source constructor defaults to one iteration. Level remaps the
result to grayscale. Nonpositive iteration leaves the value at zero before
that remap.

The camera uses the shader's explicit inverse of `rotateX * rotateY * rotateZ`.
The selected axis permutes surface hit coordinates as X,Y,Z; Y,Z,X; or Z,X,Y.
Object rotation applies `rotateX * rotateY * rotateZ` to both outputs before
the object scale and noise transform.

The native evaluator writes both outputs and preserves their selected format.
Positive iteration requires a resolved finite seed. Nonpositive iteration
does not consume the seed or noise transform controls. Equal Level endpoints
and consumed values that are not finite or representable are refused at the
named input.

Before allocating either output or notifying observers, the evaluator admits
the complete selected batch. Each allocated pixel costs 512 base units. A
covered pixel adds 24,576 units plus 16,384 units per positive iteration. The
batch limit is 64 million units. Work failures report `surface_out`, and byte
admission reserves both output surfaces.
