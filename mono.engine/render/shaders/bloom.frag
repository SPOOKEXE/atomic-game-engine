#version 450

// The glow around things brighter than the screen can show.
//
// **Bright-pass and blur in one, which is not how a production bloom is
// built.** A real one downsamples through a chain and blurs each level, so a
// wide glow costs a few small taps rather than many large ones — that is what
// the catalogue's `reduce-chain` and `upscale` are for, and a chain wiring
// those with `blur` gets it. This is the single-pass version so that dropping a
// `bloom` node in produces the effect rather than producing nothing.
//
// **The threshold is in linear light and above 1.0 deliberately.** Bloom is
// what a lens does with light brighter than the sensor can record; thresholding
// below white makes ordinary bright surfaces glow, which is the look people
// describe as hazy.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outBloom;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;

// What this pass is shading. **Every raster pass is given this block**, a
// shipped effect and one an author writes alike — see `PassUniforms` in
// `Renderer.cpp`. A shader that does not declare it is not penalised: the
// pipeline reserves the slot either way and an unused buffer costs nothing,
// which is what keeps `tint.frag` and every custom shader written before this
// existed working unchanged.
layout(set = 3, binding = 0) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Planes; // near, far, 1/near, 1/far
	vec4 Target; // width, height, 1/width, 1/height
	vec4 View;   // seconds, vertical field of view, aspect, unused
}
pass;

const float THRESHOLD = 1.0;
const float RADIUS = 3.0;

void main() {
	vec2 texel = pass.Target.zw * RADIUS;

	// A separable Gaussian would be two passes and this is one, so the taps are
	// a fixed 3x3 tent — wide enough to read as a glow at this radius and cheap
	// enough not to need the chain.
	const float weights[9] = float[](
		0.0625, 0.125, 0.0625,
		0.125,  0.25,  0.125,
		0.0625, 0.125, 0.0625
	);

	vec3 total = vec3(0.0);
	int index = 0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec3 sampled = texture(sourceImage, inUv + vec2(x, y) * texel).rgb;

			// Thresholded per tap rather than after the blur: blurring first
			// drags dim pixels above the line by averaging them with bright
			// ones, which spreads a glow out of surfaces that never emitted.
			vec3 over = max(sampled - THRESHOLD, vec3(0.0));
			total += over * weights[index];
			index++;
		}
	}

	outBloom = vec4(total, 1.0);
}
