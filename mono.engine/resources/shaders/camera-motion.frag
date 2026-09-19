#version 450
#extension GL_GOOGLE_include_directive : require

// Reprojects the visible current opaque depth through the preceding completed
// camera. It deliberately has no object history, so moving geometry is outside
// this bounded capture's validity contract.

#include "projection.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec2 outMotion;

layout(set = 2, binding = 0) uniform sampler2D depthImage;

layout(set = 3, binding = 0) uniform CameraMotion {
	mat4 InverseViewProjection;
	mat4 PreviousViewProjection;
	vec4 Target;
} motion;

void main() {
	float depth = texture(depthImage, inUv * motion.Target.zw).r;
	if (depth >= 1.0) {
		outMotion = vec2(0.0);
		return;
	}
	vec4 world = motion.InverseViewProjection * vec4(ClipCoordinates(inUv), depth, 1.0);
	if (abs(world.w) <= 1e-6) {
		outMotion = vec2(0.0);
		return;
	}
	vec4 previous = motion.PreviousViewProjection * (world / world.w);
	if (abs(previous.w) <= 1e-6) {
		outMotion = vec2(0.0);
		return;
	}
	vec2 previousNdc = previous.xy / previous.w;
	vec2 previousUv = vec2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
	outMotion = (inUv - previousUv) * motion.Target.xy;
}
