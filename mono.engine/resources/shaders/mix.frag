#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D aImage;
layout(set = 2, binding = 1) uniform sampler2D bImage;

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

vec3 Overlay(vec3 bottom, vec3 top) {
	return mix(2.0 * bottom * top, 1.0 - 2.0 * (1.0 - bottom) * (1.0 - top), step(0.5, bottom));
}

void main() {
	vec4 bottom = texture(aImage, inUv);
	vec4 top = texture(bImage, inUv);
	int operation = int(round(pass.Parameters[0].x));
	float factor = clamp(pass.Parameters[0].y, 0.0, 1.0);
	vec4 result;
	if (operation == 1) {
		result = bottom + top * factor;
	} else if (operation == 2) {
		result = mix(bottom, bottom * top, factor);
	} else if (operation == 3) {
		result = mix(bottom, 1.0 - (1.0 - bottom) * (1.0 - top), factor);
	} else if (operation == 4) {
		result = mix(bottom, vec4(Overlay(bottom.rgb, top.rgb), top.a), factor);
	} else if (operation == 5) {
		result = bottom - top * factor;
	} else if (operation == 6) {
		result = mix(bottom, abs(bottom - top), factor);
	} else if (operation == 7) {
		float opacity = clamp(top.a * factor, 0.0, 1.0);
		float bottomOpacity = clamp(bottom.a, 0.0, 1.0) * (1.0 - opacity);
		float resultOpacity = opacity + bottomOpacity;
		vec3 premultiplied = top.rgb * opacity + bottom.rgb * bottomOpacity;
		result = vec4(resultOpacity > 1e-6 ? premultiplied / resultOpacity : vec3(0.0), resultOpacity);
	} else {
		result = mix(bottom, top, factor);
	}
	int clampMode = int(round(pass.Parameters[0].z));
	if (clampMode == 1) result = max(result, vec4(0.0));
	if (clampMode == 2) result = clamp(result, vec4(0.0), vec4(1.0));
	outColour = result;
}
