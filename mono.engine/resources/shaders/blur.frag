#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	vec4 Eye;
	vec4 CameraDepth;
	uvec4 RenderFeatures;
	vec4 Parameters[3];
} pass;

void main() {
	vec4 settings = pass.Parameters[0];
	float radius = settings.y;
	float sigma = max(settings.z, 0.01);
	vec2 direction = vec2(cos(settings.w), sin(settings.w)) * pass.Target.zw;
	vec4 total = vec4(0.0);
	float weights = 0.0;
	for (int offset = -32; offset <= 32; ++offset) {
		float distance = float(offset);
		if (abs(distance) > radius) continue;
		float weight = int(round(settings.x)) == 0
			? exp(-0.5 * distance * distance / (sigma * sigma))
			: 1.0;
		total += texture(sourceImage, inUv + direction * distance) * weight;
		weights += weight;
	}
	outColour = total / max(weights, 1e-6);
}
