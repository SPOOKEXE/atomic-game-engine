#version 450

// A separable-weight gaussian in one pass.
//
// **Thirteen taps in a cross rather than a full 13x13 kernel.** A true
// two-dimensional gaussian at this radius is 169 samples; separating it into two
// passes is the usual answer and costs a second target and a second node. A
// cross approximates it for the thing a debug and post chain wants — softening —
// at thirteen samples and one pass, and the error is visible only on high
// frequency detail that is about to be blurred away anyway.
//
// **Shipped by the engine as `blur`'s default**, so the node works with no
// configuration; a node naming its own shader still wins.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;

void main() {
	// Weights of a 1D gaussian, sigma about 2. Normalised so the cross sums to
	// one — a kernel that does not brightens or darkens every frame it touches.
	const float WEIGHTS[7] = float[](0.196, 0.175, 0.121, 0.065, 0.027, 0.009, 0.002);

	vec2 texel = 1.0 / vec2(textureSize(sourceImage, 0));
	vec4 total = texture(sourceImage, inUv) * WEIGHTS[0];
	float sum = WEIGHTS[0];

	for (int step = 1; step < 7; step++) {
		float weight = WEIGHTS[step];
		vec2 offset = texel * float(step);

		total += texture(sourceImage, inUv + vec2(offset.x, 0.0)) * weight;
		total += texture(sourceImage, inUv - vec2(offset.x, 0.0)) * weight;
		total += texture(sourceImage, inUv + vec2(0.0, offset.y)) * weight;
		total += texture(sourceImage, inUv - vec2(0.0, offset.y)) * weight;
		sum += weight * 4.0;
	}

	outColour = total / sum;
}
