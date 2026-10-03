# Source cylinder projection

`pc.surface_project_cylinder_3_d` implements a bounded CPU profile of the pinned
`b69eca232217360cf1502ef0223523d818606652` constructor and fragment shader.
`source-cylinder-projection-sha256.json` records the source files used.

The shader traverses voxels, rather than intersecting an analytic cylinder.
Output Dimension determines the voxel width `2 / max(width,height)` and the
camera aspect. Cylinder profile dimensions affect sampling only. The camera
inverts `Rx * Ry * Rz`, then subtracts Position. Orthographic rays use Scale;
perspective rays use FOV and Distance. Both use the source positive `0.001`
replacement for near-zero direction components without renormalizing.

At each voxel centre, the shader computes azimuth from x/z, gates the inclusive
Angle Range and y in `[0,1)`, and samples Cylinder at `(radius,y)`. From Center
uses `radius + .5`; the ordinary mode uses `radius * 2`. Exactly zero profile
alpha rejects occupancy. An optional Top sample at the voxel centre also
rejects occupancy when its alpha is zero. The final profile colour is sampled
again and multiplied by Top at the ray's voxel-entry x/z only when that sample
has positive alpha. These two Top tests deliberately use different positions
and alpha comparisons. The earlier Top-only assignment is overwritten in the
source shader and has no final effect.

Surface Out uses ordinary alpha blending over transparent black in the native
profile. Depth Pass stores world travel distance divided by Scale and remapped
through Depth Range. Normal Pass stores the previous DDA step mask, including
multi-axis ties and an all-zero mask for a first-voxel hit. It is not a geometric
cylinder normal. Every miss clears all three outputs. Right, Bottom, Back, Left
and Voxel Color are declared but never submitted to this shader; changing them
does not change its pixels.

All seven owned surface formats use the existing decoder and writer. Explicit
Input depth reads source slot zero, which is Dimension; its first numeric
component is not a surface, so the source safe-format fallback is RGBA8 rather
than the Cylinder profile format. Inherited depth follows the captured group
and project context. Fixed
point fragment colour clamps before blending; floating colour retains finite
signed/HDR values. Sampler Pixel and CleanEdge disable filtering; other source
interpolation choices use bilinear reads because the shader calls plain
`texture2D`. Its included interpolation/oversample wrappers are never called.
The CPU sampler uses clamp addressing. Native device addressing, single-channel
swizzles, interpolation precision, ambient draw tint, blend state and licensed
visual comparisons are unobserved verification gates. GPU parity is not claimed.

Dimension supports the source scalar/tuple resize, round-to-even and unlinked
Project-unit conversion. Camera vectors resize selected numeric tuples to
three coordinates; Angle Range and Depth Range use the first two. A linked
Surface supplies dimensions before tuple resize. A whole Surface array is a
nonsurface and supplies `(1,1)`. FOV, Distance and Scale inherit the Float
getter (`nodeValue_Slider` is defined in `node_value_float.gml`) and retain that pair as two scalar processor rows; the four vector/range
ports keep it as one tuple. The source declared socket type controls this
projection: a generic Atlas socket is not treated as a Surface merely because
its payload has a texture. Processor rows use the existing source depth
classifier and array selection. Animated
controls and saves use the ordinary durable graph/keyframe codec. Path objects
cannot be submitted as shader uniform arrays and return a named unsupported
execution diagnostic. Unprocessed outer arrays retain the existing processor
boundary; flat coordinate/uniform tuples do not invent outer rows.

Before the first row allocates, an allocation-free scan of the admitted
original Dimension, Distance and Projection leaves and dimension-image backing
bounds the complete batch. Conservative maximum square dimensions and the
widest supported format charge three attachments and their retained metadata.
Floating sampler validation scans are included using their original backing
byte counts. At most 64 million weighted pixel/voxel/validation operations are
admitted across all rows. The complete worst-case traversal is charged even for transparent profiles
or an early hit. Each row reserves all three attachments before allocating;
undefined hit math clears its unpublished outputs. Evaluation failures preserve
the caller's previous image or array.

Invalid projection, nonfinite/narrowed shader uniforms, undefined ray math and
nonfinite traversal return explicit diagnostics. A collapsed Depth Range or
zero Scale is refused only when a hit actually evaluates the division; a defined
miss remains transparent. The kernel does not invent finite values for an
undefined shader expression. GLSL `atan(y,x)` is undefined when both arguments
are zero according to the [Khronos GLSL specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html).
Ordinary admitted voxel centres avoid that pair; transcendental/compiler
rounding remains an open device comparison gate.

Joined release65 CPU validation passes all 25 Cylinder cases within the complete core suite. Initial fixtures were corrected for red-only native formats, valid authored coordinate rows and source Scale in the depth oracle. GPU and licensed runtime comparison remain open.
