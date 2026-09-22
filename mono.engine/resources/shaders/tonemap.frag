#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D bloomImage;
layout(set = 3, binding = 0, std140) uniform Bloom {
	// x: bloom intensity, y: threshold, z: filter radius in pixels.
	vec4 settings;
	// xy: target size, zw: one target pixel in UV space.
	vec4 target;
} bloom;

vec3 Aces(vec3 value) {
	return clamp(
		(value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14),
		0.0,
		1.0
	);
}

void main() {
	vec4 source = texture(colourImage, inUv);
	vec3 radiance = source.rgb + texture(bloomImage, inUv).rgb * bloom.settings.x;
	outColour = vec4(pow(Aces(max(radiance, vec3(0.0))), vec3(1.0 / 2.2)), source.a);
}
