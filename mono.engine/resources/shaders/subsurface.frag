#version 450
#extension GL_GOOGLE_include_directive : require

// Separable screen-space subsurface scattering for skin pixels (see
// skin-gbuffer.glsl). Run twice by the deferred lighting node: a horizontal
// blur of lit colour into scratch (Pass.Step.w 0), then a vertical blur of the
// scratch composited over lit with alpha = skin (Pass.Step.w 1). Taps spread
// up to the pixel's scatter radius, wider in red than blue by the profile,
// and fall off with depth difference so light does not leak across silhouettes.
// Specular is blurred with diffuse; its narrow centre weight keeps highlights.

#include "skin-gbuffer.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D sourceImage;
layout(set = 2, binding = 1) uniform sampler2D depthImage;
layout(set = 2, binding = 2) uniform sampler2D normalImage;
layout(set = 2, binding = 3) uniform sampler2D albedoImage;
layout(set = 2, binding = 4) uniform sampler2D emissiveImage;

layout(set = 3, binding = 0) uniform Pass {
	// xy: one output pixel's step in source UV along the blur axis, in full-size
	// pixels. z: pixels per world unit at depth 1. w: 0 blur, 1 composite.
	vec4 Step;
	// xy: source UV scale (0.5 when the scratch was written at half size).
	// zw: G-buffer UV scale for this view.
	vec4 Scale;
	// x: far plane depth.
	vec4 Planes;
}
pass;

const int TAPS = 8;

void main() {
	vec2 geometryUv = inUv * pass.Scale.zw;
	vec4 packed = texture(normalImage, geometryUv);
	vec4 centre = texture(sourceImage, inUv * pass.Scale.xy);
	float depth = texture(depthImage, inUv).r;
	bool composite = pass.Step.w > 0.5;
	float profile = texture(albedoImage, geometryUv).a;
	if (!IsSkinPixel(packed.a) || IsPreIntegrated(profile) || depth >= pass.Planes.x) {
		outColour = composite ? vec4(0.0) : centre;
		return;
	}
	float radius = texture(emissiveImage, geometryUv).a;
	vec3 falloff = SkinProfileFalloff(profile);
	float pixels = radius * pass.Step.z / max(depth, 1e-4);
	if (pixels < 0.5) {
		outColour = composite ? vec4(centre.rgb, 1.0) : centre;
		return;
	}
	vec3 total = centre.rgb;
	vec3 weights = vec3(1.0);
	for (int index = -TAPS; index <= TAPS; index++) {
		if (index == 0) continue;
		float along = float(index) / float(TAPS);
		vec2 uv = inUv + pass.Step.xy * along * pixels;
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
		if (!IsSkinPixel(texture(normalImage, uv * pass.Scale.zw).a) ||
			IsPreIntegrated(texture(albedoImage, uv * pass.Scale.zw).a))
			continue;
		float sampleDepth = texture(depthImage, uv).r;
		float depthWeight = exp(-abs(sampleDepth - depth) / max(radius, 1e-4));
		// Gaussian per channel with its width set by the profile.
		vec3 sigma = falloff * 0.5;
		vec3 weight = exp(-(along * along) / (2.0 * sigma * sigma)) * depthWeight;
		total += texture(sourceImage, uv * pass.Scale.xy).rgb * weight;
		weights += weight;
	}
	outColour = vec4(total / weights, composite ? 1.0 : centre.a);
}
