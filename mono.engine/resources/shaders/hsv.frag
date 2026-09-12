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

vec3 RgbToHsv(vec3 colour) {
	vec4 k = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(colour.bg, k.wz), vec4(colour.gb, k.xy), step(colour.b, colour.g));
	vec4 q = mix(vec4(p.xyw, colour.r), vec4(colour.r, p.yzx), step(p.x, colour.r));
	float difference = q.x - min(q.w, q.y);
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * difference + 1e-10)), difference / (q.x + 1e-10), q.x);
}

vec3 HsvToRgb(vec3 colour) {
	vec3 p = abs(fract(colour.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
	return colour.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), colour.y);
}

void main() {
	vec4 source = texture(sourceImage, inUv);
	vec4 settings = pass.Parameters[0];
	vec3 hsv = RgbToHsv(max(source.rgb, vec3(0.0)));
	hsv.x = fract(hsv.x + settings.x);
	hsv.y = clamp(hsv.y * settings.y, 0.0, 1.0);
	hsv.z *= settings.z;
	vec3 adjusted = HsvToRgb(hsv);
	outColour = vec4(mix(source.rgb, adjusted, settings.w), source.a);
}
