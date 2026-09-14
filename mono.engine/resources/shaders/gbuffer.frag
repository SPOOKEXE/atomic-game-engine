#version 450
#extension GL_GOOGLE_include_directive : require

// Records the opaque material once so lighting and screen-space effects shade
// the visible pixel rather than every fragment that happened to cover it.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;
layout(location = 6) flat in uint inAppearance;
layout(location = 7) flat in vec3 inSurfaceColour;
layout(location = 8) flat in vec4 inEmission;
layout(location = 9) flat in uvec2 inFeaturePolicy;
layout(location = 10) flat in uint inObjectLabel;
layout(location = 11) flat in uint inSemanticLabel;
layout(location = 12) flat in uint inPartLabel;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out uint outObjectId;
layout(location = 5) out uint outSemanticId;
layout(location = 6) out uint outPartId;

// DrawSlots binds the renderer's complete material table for every material
// pipeline. Keeping the same binding layout makes SurfaceAppearance data flow
// through the deferred path without a second material binding convention.
layout(set = 2, binding = 0) uniform sampler2D shadowMap;
layout(set = 2, binding = 1) uniform sampler2D surfaceMap;
layout(set = 2, binding = 2) uniform sampler2D colourMap;
layout(set = 2, binding = 3) uniform sampler2D beamMap;
layout(set = 2, binding = 4) uniform sampler2D normalMap;
layout(set = 2, binding = 5) uniform sampler2D roughnessMap;
layout(set = 2, binding = 6) uniform sampler2D occlusionMap;
layout(set = 2, binding = 7) uniform sampler2D emissiveMap;
layout(set = 2, binding = 8) uniform sampler2D heightMap;
layout(set = 2, binding = 9) uniform sampler2D metalnessMap;

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
	vec4 OutdoorAmbient;
	vec4 FogColour;
	vec4 Fog;
	vec4 Eye;
	vec4 MaterialExtra;
	uvec4 RenderFeatures;
} lighting;

#include "opaque-eligibility.glsl"

void main() {
	OpaqueSurfaceSample surface = SampleEligibleOpaqueSurface();
	vec3 normal = normalize(inNormal);

	if (lighting.Material.x > 0.5) {
		vec3 mapped = texture(normalMap, surface.cellUv).xyz * 2.0 - 1.0;
		normal = normalize(CotangentFrame(normal, inWorldPosition, surface.cellUv) * mapped);
	}

	float roughness = lighting.Material.y > 0.5 ? texture(roughnessMap, surface.cellUv).r : 0.65;
	float materialOcclusion = lighting.Material.z > 0.5 ? texture(occlusionMap, surface.cellUv).r : 1.0;
	vec3 emissive = (surface.features & FEATURE_EMISSION) != 0u && lighting.Material.w > 0.5
		? texture(emissiveMap, surface.cellUv).rgb * inEmission.rgb * inEmission.a
		: vec3(0.0);
	float metalness =
		lighting.MaterialExtra.x > 0.5 ? texture(metalnessMap, surface.cellUv).r : 0.0;

	vec3 mappedColour = surface.colour.rgb * lighting.BaseColour.rgb;
	vec3 albedo = inColour.rgb * mappedColour * inSurfaceColour;
	if (surface.alphaMode == 0u) {
		albedo = mix(inColour.rgb, albedo, surface.materialAlpha);
	} else if (surface.alphaMode == 2u) {
		albedo = inColour.rgb * mappedColour * mix(vec3(1.0), inSurfaceColour, surface.materialAlpha);
	}
	outAlbedo = vec4(albedo, surface.alpha);
	outNormal = vec4(normal * 0.5 + 0.5, 1.0);
	outMaterial = vec4(clamp(roughness, 0.045, 1.0), clamp(metalness, 0.0, 1.0), materialOcclusion, 0.0);
	outEmissive = vec4(emissive, 1.0);
	outObjectId = inObjectLabel;
	outSemanticId = inSemanticLabel;
	outPartId = inPartLabel;
}
