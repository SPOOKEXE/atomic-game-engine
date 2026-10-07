# Source Perlin Cube

`pc.perlin_cube` is described from the pinned Pixel Composer constructor
`scripts/node_perlin_cube/node_perlin_cube.gml` and shader
`shaders/sh_perlin_cube/sh_perlin_cube.fsh` at commit
`b69eca232217360cf1502ef0223523d818606652`. The independent numeric fixtures
cover the equations below, not pixel parity with a particular GPU runtime.

## Inputs and outputs

The node has fourteen source input slots. The slot order is Dimension, Shape,
Camera Rotation, Camera Scale, Cross Axis, Cross Position, Surface Axis, Seed,
Object Rotation, Object Scale, Noise Scale, Iteration, Noise Position, and
Level. The constructor labels Camera Scale and Object Scale as Scale, and the
shader uses `orthoScale` and `shapeScale` respectively.

It produces `surface_out` and `cross_section` at the selected dimension and
color depth. `surface_out` ray-marches a cube or sphere and samples noise at the
hit point. A miss still samples the marched position and retains its RGB, while
setting alpha to zero. `cross_section` samples a plane selected by Cross Axis
and Cross Position. It does not depend on the camera or cube/sphere hit, and
its alpha is one for covered pixels.

## Shader transforms

The camera builds `rotateX * rotateY * rotateZ`, then applies the shader's
explicit matrix inverse to the ray and eye. Surface Axis permutes hit-point
coordinates as X,Y,Z; Y,Z,X; or Z,X,Y. Object Rotation applies the same
X-times-Y-times-Z matrix to both outputs, then each coordinate is multiplied
by its Object Scale lane. Noise Position is added after the scaled point is
multiplied by Noise Scale.

The cube uses a signed box distance with half extent 0.5. The sphere uses
`length(point) - 0.5`. The ray marcher advances by the signed distance, stops
when it is below `1e-5`, and treats depth 10 as a miss. Cross Axis places
Cross Position in the selected component and screen UV in the other two.

The shader hashes eight corners with its seed-dependent 3D hash, interpolates
with `f*f*(3-2*f)`, and accumulates octaves with a normalized geometric
amplitude. Level remaps the resulting scalar to grayscale. Nonpositive
Iteration skips the octave loop, leaving zero noise before Level remapping.

## Native CPU behavior

The native evaluator writes both outputs in the selected format. It preserves
RGB from the sampled noise on a surface miss and sets only that output's alpha
to zero. A covered sample with positive Iteration requires a resolved finite
Seed. Nonpositive Iteration produces zero noise without consuming Seed or the
3D transform controls. Equal Level endpoints are refused because the shader
would divide by zero. Consumed nonfinite or out-of-float-range inputs fail at
their named port.

Before allocating either output or notifying observers, the evaluator checks
the complete selected batch. Each allocated pixel costs 512 base units. A
covered pixel adds 24,576 units plus 4,096 units per positive iteration. The
batch limit is 64 million units. Work-limit failures report `surface_out`. Both
output surfaces are included in the byte reservation.

The shader's exact float results, raster coverage, and GPU ray-march behavior
remain runtime-dependent. These tests do not claim licensed-runtime pixel
parity.
