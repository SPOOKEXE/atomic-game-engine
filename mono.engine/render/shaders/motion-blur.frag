#version 450

// Smeared along whatever moved.
//
// **Reads a velocity buffer rather than differencing two frames.** Where a
// pixel was last frame is something the geometry pass knows exactly — it has
// both matrices — and something a post pass can only infer. The catalogue's
// `velocity` kind writes `RG16` screen-space motion, which is this input.
//
// **Screen-space offsets, so no matrices are needed here.** The velocity is
// already in the space this pass samples in, which is the entire point of
// producing it in the geometry pass.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D velocityImage;

const int TAPS = 8;
const float STRENGTH = 1.0;

void main() {
	vec2 velocity = texture(velocityImage, inUv).rg * STRENGTH;

	// **A still frame costs one tap.** Most pixels of most frames are not
	// moving, and the threshold is below one pixel of motion at any sane
	// resolution — below that the smear is shorter than the sample spacing and
	// only softens the image.
	if (dot(velocity, velocity) < 1e-8) {
		outColour = texture(colourImage, inUv);
		return;
	}

	vec4 total = vec4(0.0);
	for (int index = 0; index < TAPS; index++) {
		// Centred on the pixel and running both ways, so a moving object blurs
		// symmetrically instead of trailing to one side — a one-sided smear
		// reads as the object having jumped rather than moved.
		float offset = (float(index) / float(TAPS - 1)) - 0.5;
		total += texture(colourImage, inUv + velocity * offset);
	}

	outColour = total / float(TAPS);
}
