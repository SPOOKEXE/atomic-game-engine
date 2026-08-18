#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D imageTexture;

layout(set = 3, binding = 0) uniform ImageUniforms {
	vec4 channelMode;
} image;

void main() {
	vec4 sampled = texture(imageTexture, inUv);
	vec4 colour = image.channelMode.x > 0.5 ? vec4(sampled.rrr, 1.0) : sampled;
	outColour = image.channelMode.y > 0.5 ? vec4(vec3(1.0) - colour.rgb, colour.a) : colour;
}
