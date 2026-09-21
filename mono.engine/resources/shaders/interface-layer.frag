#version 450
#extension GL_GOOGLE_include_directive : require
#include "projection.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D batchColour;
layout(set = 2, binding = 1) uniform sampler2D batchZ;
layout(set = 2, binding = 2) uniform sampler2D opaqueZ;
layout(set = 2, binding = 3) uniform sampler2D previousZ;
layout(set = 3, binding = 0, std140) uniform Capture {
	mat4 InverseViewProjection;
	vec4 CameraDepth;
	vec4 Flags;
}
capture;

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec4 colour = texelFetch(batchColour, pixel, 0);
	float raw = texelFetch(batchZ, pixel, 0).r;
	if (colour.a <= 0.0 || raw >= 1.0 || raw > texelFetch(opaqueZ, pixel, 0).r) discard;
	float previous = texelFetch(previousZ, pixel, 0).r;
	if (capture.Flags.y != 0.0) {
		if (previous >= 1.0 || raw != previous) discard;
	} else if (capture.Flags.x != 0.0 && (previous >= 1.0 || raw <= previous))
		discard;
	vec3 world = WorldAtHardwareDepth(capture.InverseViewProjection, inUv, raw);
	float distance = dot(capture.CameraDepth, vec4(world, 1.0));
	if (distance <= 0.0) discard;
	// Scratch colour is already premultiplied by the interface blend state.
	outColour = colour;
	gl_FragDepth = raw;
}
