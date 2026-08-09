#version 450

// A copy, and the smallest pass in the catalogue.
//
// **Worth shipping precisely because it does nothing interesting.** A chain
// being built needs somewhere to put an image while the next pass is wired, and
// an author who has to write a copy shader to do that writes it slightly
// differently every time. Format conversion is the render pass's job — the
// target's format decides what this lands in — so a blit from RGBA16 to LDR is
// this shader and nothing extra.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;

void main() {
	outColour = texture(sourceImage, inUv);
}
