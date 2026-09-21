#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D weightImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;

void main() {
	vec4 centre = texture(colourImage, inUv);
	vec4 weights = texture(weightImage, inUv);
	vec3 horizontal = 0.5 * (
		texture(colourImage, inUv - vec2(pass.Target.z, 0.0)).rgb +
		texture(colourImage, inUv + vec2(pass.Target.z, 0.0)).rgb
	);
	vec3 vertical = 0.5 * (
		texture(colourImage, inUv - vec2(0.0, pass.Target.w)).rgb +
		texture(colourImage, inUv + vec2(0.0, pass.Target.w)).rgb
	);
	float horizontalWeight = clamp(weights.r + weights.b, 0.0, 1.0);
	float verticalWeight = clamp(weights.g + weights.a, 0.0, 1.0);
	vec3 resolved = mix(centre.rgb, horizontal, horizontalWeight);
	resolved = mix(resolved, vertical, verticalWeight);
	outColour = vec4(resolved, centre.a);
}
