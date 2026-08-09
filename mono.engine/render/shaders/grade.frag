#version 450

// Vignette and film grain, in one pass.
//
// **One pass for both because each alone is a full-screen read.** The catalogue
// lists vignette, grain, chromatic aberration and a LUT as one `grade` kind for
// this reason: four nodes doing a texture fetch each to modify the same pixel is
// four times the bandwidth for one result.
//
// Chromatic aberration and the LUT are not here yet — a LUT wants a second
// sampled input and a strength parameter, and both are a row in the catalogue
// rather than a change to this file.
//
// Shipped by the engine as `grade`'s default.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;

float Hash(vec2 at) {
	return fract(sin(dot(at, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
	vec4 source = texture(sourceImage, inUv);

	// **Measured from the centre in a square space**, so a wide window darkens
	// its corners rather than its sides — a vignette computed in UV alone is an
	// ellipse that gets more wrong the wider the frame is.
	vec2 offset = inUv - 0.5;
	float distance = length(offset) * 1.41421356;
	float vignette = smoothstep(1.0, 0.35, distance);

	// **Grain in luminance rather than per channel.** Per-channel noise is
	// coloured speckle; a film grain moves brightness, and the difference is
	// obvious the moment somebody looks at a dark area.
	float grain = (Hash(inUv * 1024.0) - 0.5) * 0.025;

	outColour = vec4(clamp(source.rgb * vignette + grain, 0.0, 1.0), source.a);
}
