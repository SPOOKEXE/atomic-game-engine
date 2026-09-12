#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outEdges;

layout(set = 2, binding = 0) uniform sampler2D colourImage;
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;

float Luma(vec3 colour) {
	return dot(colour, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
	float centre = Luma(texture(colourImage, inUv).rgb);
	float left = Luma(texture(colourImage, inUv - vec2(pass.Target.z, 0.0)).rgb);
	float top = Luma(texture(colourImage, inUv - vec2(0.0, pass.Target.w)).rgb);
	vec2 delta = abs(vec2(centre - left, centre - top));
	float localContrast = max(delta.x, delta.y);
	vec2 edges = step(vec2(max(0.05, localContrast * 0.5)), delta);
	outEdges = vec4(edges, 0.0, 1.0);
}
