#version 450
#extension GL_GOOGLE_include_directive : require

#include "projection.glsl"

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
	vec4 CameraDepth;
	vec4 FogColour;
	vec4 Fog;
	vec4 Shadow;
} pass;

void main() {
	float raw = texture(depthImage, inUv * pass.Target.zw).r;
	float farPlane = pass.Planes.y;
	float linear = raw >= 1.0
		? (pass.Direction.w > 0.5 ? 0.0 : farPlane)
		: dot(pass.CameraDepth, vec4(WorldAtHardwareDepth(pass.InverseViewProjection, inUv, raw), 1.0));
	outLinear = vec4(linear);
}
