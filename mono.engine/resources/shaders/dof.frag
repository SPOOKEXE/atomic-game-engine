#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 3, binding = 0, std140) uniform LightingEffects {
	// x: intensity, y: focus distance, z: focus range, w: radius in pixels.
	vec4 depthOfField;
	vec4 godRays;
	// zw: one target pixel in UV space.
	vec4 target;
} effects;

void main() {
	vec4 source = texture(colourImage, inUv);
	if (effects.depthOfField.x <= 0.0 || effects.depthOfField.w <= 0.0) {
		outColour = source;
		return;
	}

	float depth = texture(depthImage, inUv).r;
	float focusRange = max(effects.depthOfField.z, 0.0001);
	float circle = clamp((abs(depth - effects.depthOfField.y) - focusRange) / focusRange, 0.0, 1.0);
	vec2 radius = effects.depthOfField.w * effects.depthOfField.x * circle * effects.target.zw;
	vec4 filtered = source * 0.2;
	for (int axis = 0; axis < 2; ++axis) {
		vec2 offset = axis == 0 ? vec2(radius.x, 0.0) : vec2(0.0, radius.y);
		filtered += texture(colourImage, inUv + offset) * 0.2;
		filtered += texture(colourImage, inUv - offset) * 0.2;
	}
	outColour = filtered;
}
