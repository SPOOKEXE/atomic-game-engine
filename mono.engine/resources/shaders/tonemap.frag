#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;

vec3 Aces(vec3 value) {
	return clamp(
		(value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14),
		0.0,
		1.0
	);
}

void main() {
	vec4 source = texture(colourImage, inUv);
	outColour = vec4(pow(Aces(max(source.rgb, vec3(0.0))), vec3(1.0 / 2.2)), source.a);
}
