#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D aImage;
layout(set = 2, binding = 1) uniform sampler2D bImage;

void main() {
	vec4 bottom = texture(aImage, inUv);
	vec4 top = texture(bImage, inUv);
	outColour = top + bottom * (1.0 - clamp(top.a, 0.0, 1.0));
}
