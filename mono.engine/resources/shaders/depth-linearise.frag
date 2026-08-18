#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outLinear;

layout(set = 2, binding = 0) uniform sampler2D depthImage;

layout(set = 3, binding = 0) uniform Pass {
	mat4 InverseViewProjection;
	mat4 LightViewProjection;
	vec4 Planes;
	vec4 Target;
	vec4 Direction;
	vec4 Ambient;
	vec4 OutdoorAmbient;
	vec4 Direct;
	vec4 Eye;
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
} pass;

void main() {
	float raw = texture(depthImage, inUv * pass.Target.zw).r;
	float nearPlane = pass.Planes.x;
	float farPlane = pass.Planes.y;
	float linear = raw >= 1.0
		? farPlane
		: (nearPlane * farPlane) / max(farPlane - raw * (farPlane - nearPlane), 1e-6);
	outLinear = vec4(linear);
}
