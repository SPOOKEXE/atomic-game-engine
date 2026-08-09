#version 450

// The deferred split of the opaque pass: surface properties, not light.
//
// **Three targets, because the whole point is that shading happens later.** The
// forward pass computes a colour; this records what a colour would be computed
// *from*, so a later pass shades once per pixel instead of once per fragment.
// `PIPELINE_NODES.md` §1.4 is a real frame doing exactly this, and §1.5 fault 9
// is what the forward pass pays without it.
//
// **The vertex stage is `opaque.vert` unchanged**, for `overdraw.frag`'s reason:
// what is recorded has to be what the forward pass would have shaded, and a
// second vertex shader is a second description of where the geometry is.
//
// **Normals in `RGB10A2` rather than `RGBA16`.** §1.5 fault 6 is a captured
// frame that used four times the bits for exactly this, and ten bits an axis is
// past the precision a normal read back through a sampler can carry.
//
// **The alpha of the normal target carries a material tag**, which is what the
// captured frame does — glass and window share a value, concrete another. It is
// the cheapest place to put a per-pixel classification a later pass branches on,
// and it costs nothing because the fourth channel is there whether used or not.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec4 inLightPosition;
layout(location = 3) in vec4 inSurfacePosition;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;

// **Slots 0 to 2 are `DrawSlots`' fixed layout, shared with `opaque.frag`.**
// This shader reads only the third, and declaring the two it ignores is not
// optional: the pass binds three and a pipeline built for one would put the
// *shadow atlas* in `colourMap`. That was the bug — the G-buffer's albedo was
// the shadow map, and a probe that checked three targets were written rather
// than what was in them did not notice.
layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;

// The material maps, bound only by this pass. An absent one is the engine's
// default sheet — white — which is why each is gated on its flag rather than
// tested against its texel: white is a legitimate roughness of 1.
layout(set = 2, binding = 3) uniform sampler2D normalMap;
layout(set = 2, binding = 4) uniform sampler2D roughnessMap;
layout(set = 2, binding = 5) uniform sampler2D occlusionMap;
layout(set = 2, binding = 6) uniform sampler2D emissiveMap;

// **The same block `opaque.frag` declares, in full.** It is pushed once by
// `DrawSlots` and both shaders read it, so a std140 block that disagreed about
// its layout would have one of them reading garbage from the middle. Only
// `Maps` and `Surface` are used here; the rest is declared to keep the shapes
// identical.
layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
	vec4 Flags;
	vec4 BaseColour;
	vec4 Surface;
	vec4 Flipbook;

	// x: normal map present. y: roughness. z: occlusion.
	// x: normal. y: roughness. z: occlusion. w: emissive.
	vec4 Maps;
}
lighting;

void main() {
	vec4 sampled = texture(colourMap, inTexCoord);

	// **Albedo carries opacity in alpha, and it is not decoration.** A deferred
	// pass cannot blend, so anything not fully opaque has to be sorted into the
	// forward tail — and this is the channel that says which.
	outAlbedo = vec4(sampled.rgb * inColour.rgb, inColour.a);

	// Packed from [-1, 1] into [0, 1]. Normalised here rather than trusting the
	// interpolation, which is not unit-length across a triangle.
	vec3 normal = normalize(inNormal);

	// **Perturbed in world space from a screen-space basis**, because this
	// engine's vertex format carries no tangent. Deriving one from the
	// derivatives of position and UV is the standard substitute and is exact
	// wherever the UVs are not degenerate — a mesh with a collapsed UV island
	// gets the geometric normal, which is what it would have had anyway.
	if (lighting.Maps.x > 0.5) {
		vec3 dpx = dFdx(inWorldPosition);
		vec3 dpy = dFdy(inWorldPosition);
		vec2 duvx = dFdx(inTexCoord);
		vec2 duvy = dFdy(inTexCoord);

		float determinant = duvx.x * duvy.y - duvy.x * duvx.y;
		if (abs(determinant) > 1e-12) {
			vec3 tangent = normalize((dpx * duvy.y - dpy * duvx.y) / determinant);

			// Gram-Schmidt: the tangent has to be perpendicular to the normal,
			// and the derivative-derived one is only approximately so.
			tangent = normalize(tangent - normal * dot(normal, tangent));
			vec3 bitangent = cross(normal, tangent);

			vec3 sampledNormal = texture(normalMap, inTexCoord).xyz * 2.0 - 1.0;
			normal = normalize(mat3(tangent, bitangent, normal) * sampledNormal);
		}
	}

	outNormal = vec4(normal * 0.5 + 0.5, 0.0);

	// **Roughness, metalness, and room for two more.** The material asset has
	// four maps published beside every colour map and nothing reads them yet —
	// `ROADMAP.md` v0.10 says that is waiting on exactly this pass. Constants
	// until it does, and constants that are obviously placeholders rather than
	// plausible values somebody might trust.
	// **Roughness, metalness, occlusion, and room for one more.** Roughness and
	// occlusion are sampled when the material named them and fall back to
	// constants when it did not — a fully rough, unoccluded surface, which is
	// what an untextured material looks like. Metalness has no map yet: nothing
	// in the seeded set publishes one, so a channel read from a texture nobody
	// ships would be a feature that is always zero.
	float roughness = lighting.Maps.y > 0.5 ? texture(roughnessMap, inTexCoord).r : 1.0;
	float occlusion = lighting.Maps.z > 0.5 ? texture(occlusionMap, inTexCoord).r : 1.0;
	// **Emissive strength in alpha, not a third colour target.** What a surface
	// emits has a colour, and storing it properly wants three more channels —
	// but a fourth target costs bandwidth on every pixel of every frame to carry
	// something almost nothing in a scene uses. So the *strength* lives here and
	// the *hue* comes from the albedo already beside it, which is right for the
	// overwhelming majority of emissive surfaces: a screen, a sign, a lamp
	// filament all glow the colour they are.
	float emission = lighting.Maps.w > 0.5 ? texture(emissiveMap, inTexCoord).r : 0.0;
	outMaterial = vec4(roughness, 0.0, occlusion, emission);
}
