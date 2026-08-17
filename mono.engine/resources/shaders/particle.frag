#version 450

// One particle's fragment: the texture, tinted.
//
// A particle has no normal, so environmental light is sampled as one
// orientation-free value per emitter. This keeps `LightInfluence` meaningful
// without paying the opaque shader's per-fragment light loop on every quad.

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec3 inWorldPosition;

layout(set = 2, binding = 0) uniform sampler2D particleTexture;

layout(set = 3, binding = 0) uniform Material {
	// x: whether `particleTexture` holds this emitter's texture. An untextured
	// emitter draws its colour as a flat quad, which is a visible square rather
	// than nothing - the missing-texture rule the mesh path already follows.
	// y: the blend from alpha to additive. z: lighting influence.
	vec4 Flags;
	vec4 Illumination;
	vec4 FogColour;
	vec4 Fog;
	vec4 Eye;
} material;

layout(location = 0) out vec4 outColour;

void main() {
	vec4 texel = material.Flags.x > 0.5 ? texture(particleTexture, inTexCoord) : vec4(1.0);
	vec4 result = texel * inColour;
	result.rgb *= mix(vec3(1.0), material.Illumination.rgb, material.Flags.z);

	float fogInterval = max(material.Fog.y - material.Fog.x, 0.0001);
	float fog = clamp((distance(inWorldPosition, material.Eye.xyz) - material.Fog.x) / fogInterval, 0.0, 1.0);
	result.rgb = mix(result.rgb, material.FogColour.rgb, fog);

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

	// Premultiplied output makes `LightEmission` continuous. The pipeline uses
	// ONE and ONE_MINUS_SRC_ALPHA, so reducing the output alpha preserves more
	// of the destination while the source contribution stays fixed. At one this
	// is additive; at zero it is ordinary alpha blending.
	result.rgb *= result.a;
	result.a *= 1.0 - material.Flags.y;
	outColour = result;
}
