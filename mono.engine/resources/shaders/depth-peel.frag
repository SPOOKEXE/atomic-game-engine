#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;
layout(location = 6) flat in uint inAppearance;
layout(location = 7) flat in vec3 inSurfaceColour;
layout(location = 8) flat in vec4 inEmission;
layout(location = 9) flat in uvec2 inFeaturePolicy;

layout(location = 0) out float outDepthMetres;
layout(location = 1) out float outValidity;

// DrawSlots keeps its complete ten-sampler material binding. The peel replaces
// the unused shadow input with the visible hardware-depth image.
layout(set = 2, binding = 0) uniform sampler2D firstDepthMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;
layout(set = 2, binding = 3) uniform sampler2D beamMap;
layout(set = 2, binding = 4) uniform sampler2D normalMap;
layout(set = 2, binding = 5) uniform sampler2D roughnessMap;
layout(set = 2, binding = 6) uniform sampler2D occlusionMap;
layout(set = 2, binding = 7) uniform sampler2D emissiveMap;
layout(set = 2, binding = 8) uniform sampler2D heightMap;
layout(set = 2, binding = 9) uniform sampler2D metalnessMap;
layout(set = 2, binding = 10) uniform sampler2D packedPbrMap;

layout(set = 3, binding = 0) uniform Lighting {
	vec4 Direction;
	vec4 Ambient;
	vec4 Direct;
	vec4 Flags;
	vec4 BaseColour;
	vec4 Surface;
	vec4 Material;
	vec4 Flipbook;
	vec4 Mirror;
	vec4 PaneNormal;
	vec4 SeamPlane;
	vec4 SeamFirst;
	vec4 SeamSecond;
	vec4 SeamCentre;
	vec4 OutdoorAmbient;
	vec4 FogColour;
	vec4 Fog;
	vec4 Eye;
	vec4 MaterialExtra;
	uvec4 RenderFeatures;
	vec4 PackedPbrChannels;
} lighting;

#include "seam-mask.glsl"

float PackedPbrValue(vec2 uv, float channel) {
	return texture(packedPbrMap, uv)[int(channel + 0.5)];
}

layout(set = 3, binding = 1) uniform Peel {
	// xyz and offset convert world position to camera-forward metres.
	vec4 CameraDepth;
	// x is one normalized source quantum. y selects next representable float.
	vec4 DepthRule;
} peel;

#include "opaque-eligibility.glsl"

float NextSourceDepth(float first) {
	if (peel.DepthRule.y > 0.5) {
		return uintBitsToFloat(floatBitsToUint(first) + 1u);
	}
	return first + peel.DepthRule.x;
}

void main() {
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float first = texelFetch(firstDepthMap, pixel, 0).r;
	if (first >= 1.0 || gl_FragCoord.z < NextSourceDepth(first)) {
		discard;
	}
	SampleEligibleOpaqueSurface();
	outDepthMetres = dot(peel.CameraDepth, vec4(inWorldPosition, 1.0));
	outValidity = 1.0;
}
