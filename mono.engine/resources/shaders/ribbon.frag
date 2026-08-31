#version 450

// A ribbon's fragment: the texture along its length, tinted by the sequence.
//
// Unlit, for the particle pass's reason: a beam's colour comes from its
// `ColorSequence` and a trail's fades towards its tail, and there is no normal on
// a strip that faces the camera to light against.

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColour;

layout(set = 2, binding = 0) uniform sampler2D ribbonTexture;

layout(set = 3, binding = 0) uniform Material {
	// x: whether `ribbonTexture` holds this run's texture. An untextured beam
	// draws its gradient flat, which is a visible strip rather than nothing -
	// the missing-texture rule the mesh and particle paths both follow.
	// y: wrap V as well as U for a tiled face Texture.
	// z, w: unused, named so the struct's size is stated.
	vec4 Flags;
} material;

layout(location = 0) out vec4 outColour;

void main() {
	// **`fract` on the length coordinate rather than a repeat sampler.** A beam's
	// texture scrolls, so its coordinate grows past 1 and past 2 - and a sampler
	// in clamp mode would smear the last column of texels down the rest of the
	// beam. Wrapping here rather than in the sampler state keeps the *across*
	// coordinate clamped, which is what stops a wide ribbon bleeding its top edge
	// into its bottom one.
	vec2 uv = vec2(
		fract(inTexCoord.x),
		material.Flags.y > 0.5 ? fract(inTexCoord.y) : clamp(inTexCoord.y, 0.0, 1.0)
	);

	vec4 texel = material.Flags.x > 0.5 ? texture(ribbonTexture, uv) : vec4(1.0);
	vec4 result = texel * inColour;

	// Below what an 8-bit target can represent at all. See the particle pass:
	// the tail of every fading effect is fragments that cost a blend and show
	// nothing.
	if (result.a < 0.001) {
		discard;
	}

	outColour = result;
}
