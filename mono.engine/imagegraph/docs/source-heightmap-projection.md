# Source heightmap projection

`pc.heightmap_project_3_d` is the exact catalogue ID for the pinned
`b69eca232217360cf1502ef0223523d818606652` Heightmap Project 3D constructor.
The companion SHA-256 file identifies its wrapper, getters, shader and draw helpers.
This implementation is a bounded native CPU profile. Device and licensed visual
parity remain unobserved.

The shader traverses voxel cells using output Dimension, with voxel width
`2/max(width,height)`. It inverts `Rx*Ry*Rz`, subtracts Position and constructs
orthographic Scale or perspective FOV/Distance rays. Near-zero direction
components become positive `.001` without renormalization. Every occupied cell
satisfies `sc.y > ((1-heightmap.red)-HeightRange.x)/(HeightRange.y-HeightRange.x)`.
Heightmap alpha is ignored. Tiled wraps the occupancy x/z coordinates only.
Heightmap reads use plain nearest/bilinear texture sampling even when the
interpolation attribute selects bicubic or Lanczos.

Final Texture reads use the ray's voxel-entry x/z, rather than its cell centre.
Missing Texture falls back to Heightmap. Side overrides z faces near the outer
z boundary; otherwise Front overrides x faces near the outer x boundary.
Front flips its z coordinate. Bicubic/Lanczos Texture, Side and Front reads all
use Heightmap dimensions, as the wrapper submits that one `sampleDimension`.
Oversample applies to final texture reads and is independent of Tiled occupancy.
The ordinary sampler region makes raw CleanEdge choice 6 a plain nearest read;
there is no edge-slicing shader region here.

Height Color multiplies the final texture at `fract(fract(g+Shift)+1)`.
Ordinary `g` is `1-entry.y`; Normalize Height divides it by `1-sampledHeight`.
The source gradient modes 0..6 reuse the native source projection gradient math.
Up to 64 keys form the portable GLSL profile; the source HLSL shader has a
128-key define that this profile does not claim. Declared Colour scalar inputs
become a constant gradient. Declared Colour arrays become one gradient with
key times `i/count` before processor row selection, matching the Gradient
getter rather than the gradientObject array constructor's `i/(count-1)`.
This conversion has a separate scoped evaluator patch and owned input charge.
A generic array payload does not acquire a Colour socket domain.

The wrapper calls `shader_set_gradient` with one argument, so gradient maps
are disabled. A non-Gradient input that is not converted by the Colour getter
would leave source uniforms dependent on prior state. The native profile
returns a named unsupported diagnostic for that branch. It does not invent
a gradient-map interpretation or a white result for a supplied Surface colour.

Surface Out uses ordinary alpha blending over transparent black. Native fixed
point fragment colours clamp before blending; floating outputs preserve finite
signed/HDR colour. Depth stores Euclidean travel divided by Scale, remapped by
Depth Range. Normal stores the previous DDA step mask, including ties and an
all-zero first-hit mask. A first-cell entry plane may lie behind the eye; its
distance is retained. All misses clear all three outputs. The seven owned
formats retain native red-only channel decoding. Explicit Input depth observes
numeric Dimension slot zero and therefore uses the safe RGBA8 fallback.
Inherited depth follows captured project/group context.

Dimension has source tuple resize, round-even and unlinked Project-unit behavior.
Linked Dimension bypasses its unit. This constructor leaves `mask_input` undefined
and `use_mask` false, so an unlinked Mask unit has a named unsupported boundary.
The separate evaluator hook admits only this node's Vec3 View Angle/Position,
Range Height/Depth and Float/Slider FOV/Distance/Scale/Shift Surface getters.
Scalar Surface dimensions form two scalar processor rows. A whole Surface array
is a nonsurface to these getters and supplies `(1,1)` before selection.
Ordinary numeric tuples, processor array order, animation and native graph saves
retain the existing shared contracts. Disabled outer-array uniform upload and
unobserved path uniform objects keep named shared execution boundaries.

Before the first row allocates, bounded allocation-free scans inspect every
original Dimension/Distance/Projection leaf and dimension-image backing.
Maximum square dimensions, maximum required perspective traversal and all
original floating sampler backing bound the batch. The native work cap is
64 million weighted pixel/voxel/validation units across rows, including a fixed
worst-case budget for texture interpolation and 64-key gradient evaluation.
Three attachments use the widest format for whole-batch byte admission. Every
row reserves its three actual attachments before allocation. Undefined row
math clears unpublished outputs, and evaluation preserves the caller's previous
image or array on failure. Colour projection admits key count/bytes before
allocation and charges actual excess vector capacity into retained input storage.

Collapsed Height Range is refused when occupancy evaluates its division; a ray
that never samples the height domain may remain a defined miss. Zero Scale and
collapsed Depth Range are refused when a hit evaluates depth. Undefined
Normalize Height, Lanczos weight divisions and nonfinite shader math are explicit
native boundaries. CMYK interpolation through black retains the source undefined
arithmetic boundary. No finite substitute is asserted as source parity. These refusals are a native
policy; the [Khronos GLSL range and precision specification](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html)
does not require shader operations to propagate NaNs in the same way as the CPU.

Native texture addressing uses clamp and the existing deterministic sampler.
Hardware interpolation precision, texture swizzles, ambient draw tint/blend and
raster/fragment positions remain device comparison gates. The front branch's
source vertex tint uses native white; other texture branches have no vertex
tint multiplication. Shader UV-map uniforms have no authored control here and
the native profile evaluates them disabled.

The outside draft has strict syntax checks against a recorded immutable current66
public/private header snapshot. Its graph and pixel fixtures await a fresh joined
build. No old archive link, live mutation, GPU, profiler or licensed runtime result
is used as evidence.
