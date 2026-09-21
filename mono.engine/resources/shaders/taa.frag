#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(location = 1) out vec4 outHistory;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 2, binding = 1) uniform sampler2D historyImage;
layout(set = 2, binding = 2) uniform sampler2D velocityImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;

void main() {
	vec3 current = texture(colourImage, inUv).rgb;
	vec2 velocity = texture(velocityImage, inUv).rg;
	vec3 history = texture(historyImage, clamp(inUv - velocity, vec2(0.0), vec2(1.0))).rgb;

	vec3 low = current;
	vec3 high = current;
	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			vec3 neighbour = texture(colourImage, inUv + vec2(x, y) * pass.Target.zw).rgb;
			low = min(low, neighbour);
			high = max(high, neighbour);
		}
	}
	history = clamp(history, low, high);
	float motion = clamp(length(velocity) * max(pass.Target.x, pass.Target.y), 0.0, 1.0);
	vec3 resolved = mix(history, current, mix(0.1, 0.75, motion));
	outColour = vec4(resolved, 1.0);
	outHistory = outColour;
}
