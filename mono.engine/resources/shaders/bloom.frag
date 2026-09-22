#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;
layout(set = 3, binding = 0, std140) uniform Bloom {
	// x: intensity, y: threshold, z: radius in display pixels.
	vec4 settings;
	// xy: target size, zw: one target pixel in UV space.
	vec4 target;
} bloom;

vec3 Extract(vec3 colour) {
	return max(colour - vec3(bloom.settings.y), vec3(0.0));
}

void main() {
	if (bloom.settings.x <= 0.0 || bloom.settings.z <= 0.0) {
		outColour = vec4(0.0);
		return;
	}

	vec2 radius = bloom.settings.z * bloom.target.zw;
	vec3 filtered = Extract(texture(sourceImage, inUv).rgb) * 0.2;
	for (int axis = 0; axis < 2; axis++) {
		vec2 offset = axis == 0 ? vec2(radius.x, 0.0) : vec2(0.0, radius.y);
		filtered += Extract(texture(sourceImage, inUv + offset).rgb) * 0.1;
		filtered += Extract(texture(sourceImage, inUv - offset).rgb) * 0.1;
	}
	for (int x = -1; x <= 1; x += 2)
		for (int y = -1; y <= 1; y += 2)
			filtered += Extract(texture(sourceImage, inUv + radius * vec2(x, y)).rgb) * 0.1;

	outColour = vec4(filtered, 1.0);
}
