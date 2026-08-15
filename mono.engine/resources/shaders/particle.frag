#version 450

// One particle's fragment: the texture, tinted.
//
// **Unlit, and that is a decision rather than an omission.** A particle system's
// colour comes from its `ColorSequence` and its `LightEmission`, and running half
// a million fragments through the same lighting the opaque pass uses would be
// paying for a normal every particle does not have. `ParticleEmitter::
// LightInfluence` is where the lit variant would be selected from, and it is not
// implemented - `Particles.hpp` says so rather than the field pretending.

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColour;

layout(set = 2, binding = 0) uniform sampler2D particleTexture;

layout(set = 3, binding = 0) uniform Material {
	// x: whether `particleTexture` holds this emitter's texture. An untextured
	// emitter draws its colour as a flat quad, which is a visible square rather
	// than nothing - the missing-texture rule the mesh path already follows.
	// y, z, w: unused, named so the struct's size is stated rather than implied.
	vec4 Flags;
} material;

layout(location = 0) out vec4 outColour;

void main() {
	vec4 texel = material.Flags.x > 0.5 ? texture(particleTexture, inTexCoord) : vec4(1.0);
	vec4 result = texel * inColour;

	// **Discarded below a thousandth rather than blended.** A fully transparent
	// particle still costs a blend and a depth test, and the tail of every fading
	// effect is thousands of them - so the discard is not tidiness, it is the
	// difference between paying for the particles you can see and paying for
	// every particle that has not been retired yet.
	//
	// Not a `Clip` mode and not authored: this is below what an 8-bit target can
	// represent at all, so nothing visible is being thrown away.
	if (result.a < 0.001) {
		discard;
	}

	outColour = result;
}
