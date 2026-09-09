#version 450
#extension GL_GOOGLE_include_directive : require
#include "surface-shading.glsl"

layout(set = 2, binding = 10) uniform sampler2D opaqueZ;
layout(set = 2, binding = 11) uniform sampler2D previousZ;
layout(set = 3, binding = 3, std140) uniform LayerCapture {
	vec4 Eye;
	vec4 Forward;
	vec4 Flags;
} capture;

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float distance = dot(inWorldPosition - capture.Eye.xyz, capture.Forward.xyz);
	float opaque = texelFetch(opaqueZ, pixel, 0).r;
	if (distance <= 0.0 || gl_FragCoord.z >= opaque) discard;
	if (capture.Flags.y != 0.0) {
		float selected = texelFetch(previousZ, pixel, 0).r;
		if (selected >= 1.0 || gl_FragCoord.z != selected) discard;
	} else if (capture.Flags.x != 0.0) {
		float previous = texelFetch(previousZ, pixel, 0).r;
		if (previous >= 1.0 || gl_FragCoord.z <= previous) discard;
	}
	shadeSurface();
	if (outColour.a <= 0.0) discard;
}
