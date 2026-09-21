#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outWeights;

layout(set = 2, binding = 0) uniform sampler2D edgeImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;

void main() {
	vec2 edges = texture(edgeImage, inUv).rg;
	float horizontalSpan = 0.0;
	float verticalSpan = 0.0;
	for (int offset = 1; offset <= 8; ++offset) {
		float distanceWeight = 1.0 - float(offset - 1) / 8.0;
		horizontalSpan += texture(edgeImage, inUv + vec2(float(offset) * pass.Target.z, 0.0)).g * distanceWeight;
		verticalSpan += texture(edgeImage, inUv + vec2(0.0, float(offset) * pass.Target.w)).r * distanceWeight;
	}
	vec2 weights = clamp(edges * (0.125 + vec2(verticalSpan, horizontalSpan) * 0.03125), 0.0, 0.5);
	outWeights = vec4(weights, weights.yx);
}
