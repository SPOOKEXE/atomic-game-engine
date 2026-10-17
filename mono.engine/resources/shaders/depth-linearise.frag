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
	// xy: output pixels; zw: active depth rectangle within its allocation.
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
	ivec2 activeDepthSize = ivec2(round(vec2(textureSize(depthImage, 0)) * pass.Target.zw));
	bool samePixelGrid = all(equal(ivec2(pass.Target.xy), activeDepthSize));
	// A matching grid must not mix geometry depth into an adjacent clear pixel.
	float raw = samePixelGrid ? texelFetch(depthImage, ivec2(gl_FragCoord.xy), 0).r
		: texture(depthImage, inUv * pass.Target.zw).r;
	if (pass.Direction.w > 1.5) {
		outLinear = vec4(raw < 0.999999 ? 1.0 : 0.0);
		return;
	}
	float farPlane = pass.Planes.y;
	// D24 clear values arrive slightly below one on Vulkan. Treat that narrow
	// range as sky so screen-space shafts do not mistake the clear plane for a
	// solid occluder.
	float linear = raw >= 0.999999
		? (pass.Direction.w > 0.5 ? 0.0 : farPlane)
		: dot(pass.CameraDepth, vec4(WorldAtHardwareDepth(pass.InverseViewProjection, inUv, raw), 1.0));
	outLinear = vec4(linear);
}
