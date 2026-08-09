#version 450

// Two images into one.
//
// **Half and half, and the factor input is deliberately not read here.** The
// catalogue declares an optional `factor` texture, and a shader that sampled it
// would need it wired — an optional input that is absent packs no binding, so
// the slots below it shift and the shader reads the wrong image. So the shipped
// default is the unweighted blend, and a chain that has a factor mask to apply
// names its own shader with three samplers. Getting a weighted blend wrong
// silently is worse than not shipping one.

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D imageA;
layout(set = 2, binding = 1) uniform sampler2D imageB;

void main() {
	outColour = mix(texture(imageA, inUv), texture(imageB, inUv), 0.5);
}
