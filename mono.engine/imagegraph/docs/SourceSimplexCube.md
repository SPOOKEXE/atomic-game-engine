# Source Simplex Cube

`pc.simplex_cube` follows the pinned Pixel Composer constructor
`scripts/node_simplex_cube/node_simplex_cube.gml` and shader
`shaders/sh_simplex_cube/sh_simplex_cube.fsh` at commit
`b69eca232217360cf1502ef0223523d818606652`. Numeric fixtures use independent
binary32 shader-equation samples and do not claim pixel parity with a GPU
runtime.

The node has fourteen source inputs and writes `surface_out` and
`cross_section`. The surface output ray-marches a cube or sphere. The cross
section samples a plane at the selected axis and position and is independent
of camera rotation and shape. A ray miss retains sampled RGB in the surface
output and sets its alpha to zero. The cross section keeps alpha one on covered
pixels.

The shader skews each 3D coordinate into a simplex lattice, ranks the local
coordinates to select four corners, and evaluates a normalized hash-derived
gradient at each corner. Attenuation is zero outside its support radius. The
constructor defaults to four octaves. Each octave doubles the transformed
coordinate and halves its amplitude; the accumulated value is remapped to
grayscale by Level. Nonpositive iteration leaves zero before the Level remap.
The signed integer additions used for lattice and corner indices wrap to the
low 32 bits, as specified for GLSL integer arithmetic.

The camera uses the shader's explicit inverse of
`rotateX * rotateY * rotateZ`. The selected axis permutes surface hit
coordinates as X,Y,Z; Y,Z,X; or Z,X,Y. Object rotation applies
`rotateX * rotateY * rotateZ` to both outputs before object scaling and the
noise transform.

The native evaluator writes both outputs in the selected color format.
Positive iteration requires a finite resolved seed. Nonpositive iteration
skips seed and noise-coordinate processing. Equal Level endpoints are refused.
The evaluator refuses a zero-length normalized gradient at `seed` and a
floored lattice coordinate outside the signed 32-bit range at `noise_scale`.
Consumed nonfinite or out-of-range values are diagnosed at their source
control.

Before allocating outputs or notifying observers, the evaluator admits the
whole selected batch. Each allocated pixel costs 512 base units. A covered
pixel adds 24,576 units plus 16,384 units per positive iteration. The batch
limit is 64 million units. Work failures report `surface_out`, and byte
admission reserves both outputs.
