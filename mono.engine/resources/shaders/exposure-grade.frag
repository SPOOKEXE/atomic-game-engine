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
	vec4 source = texture(sourceImage, inUv);
	vec4 grade = pass.Parameters[0];
	vec3 exposed = source.rgb * exp2(grade.x);
	vec3 contrasted = (exposed - vec3(grade.z)) * grade.y + vec3(grade.z);
	vec3 corrected = pow(max(contrasted, vec3(0.0)), vec3(1.0 / max(grade.w, 0.01)));
	outColour = vec4(corrected, source.a);
}
