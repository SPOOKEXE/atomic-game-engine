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
layout(location = 10) flat in uvec3 inLabels;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out uint outObjectId;
layout(location = 5) out uint outSemanticId;
layout(location = 6) out uint outPartId;
layout(location = 7) out vec2 outMeshUv;

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
layout(set = 2, binding = 10) uniform sampler2D packedPbrMap;
// Skin shading maps; DrawSlots binds them only on this pass.
layout(set = 2, binding = 11) uniform sampler2D thicknessMap;
layout(set = 2, binding = 12) uniform sampler2D secondaryRoughnessMap;

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
	vec4 Transmission;
	// WeatherShadowState, unused here; it keeps Skin at its C++ offset.
	vec4 CloudShadowReserved[7];
	// x 1 screen-space or 2 pre-integrated, y profile id, z scatter radius, w thickness (world units).
	vec4 Skin;
	// x second lobe roughness, y its weight, z thickness map bound, w lobe map
	// bound + 2 * (packed PBR weight channel + 1), zero channel part for none.
	vec4 SkinLobe;
} lighting;

#include "seam-mask.glsl"

float PackedPbrValue(vec2 uv, float channel) {
	return texture(packedPbrMap, uv)[int(channel + 0.5)];
}

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
	// Emissive tint and strength are authored independently of an optional map.
	// A missing map therefore leaves their constant radiance intact instead of
	// turning the surface black.
	vec3 emissive = (surface.features & FEATURE_EMISSION) != 0u
		? inEmission.rgb * inEmission.a *
			(lighting.Material.w > 0.5 ? texture(emissiveMap, surface.cellUv).rgb : vec3(1.0))
		: vec3(0.0);
	float metalness =
		lighting.MaterialExtra.x > 0.5 ? texture(metalnessMap, surface.cellUv).r : 0.0;
	if (lighting.MaterialExtra.y > 0.5) {
		if (lighting.PackedPbrChannels.x < 4.0) roughness = PackedPbrValue(surface.cellUv, lighting.PackedPbrChannels.x);
		if (lighting.PackedPbrChannels.y < 4.0) materialOcclusion = PackedPbrValue(surface.cellUv, lighting.PackedPbrChannels.y);
		if (lighting.PackedPbrChannels.w < 4.0) metalness = PackedPbrValue(surface.cellUv, lighting.PackedPbrChannels.w);
	}

	vec3 mappedColour = surface.colour.rgb * lighting.BaseColour.rgb;
	vec3 albedo = inColour.rgb * mappedColour * inSurfaceColour;
	if (surface.alphaMode == 0u) {
		albedo = mix(inColour.rgb, albedo, surface.materialAlpha);
	} else if (surface.alphaMode == 2u) {
		albedo = inColour.rgb * mappedColour * mix(vec3(1.0), inSurfaceColour, surface.materialAlpha);
	}
	outAlbedo = vec4(albedo, surface.alpha);
	outNormal = vec4(normal * 0.5 + 0.5, 1.0);
	outMaterial = vec4(clamp(roughness, 0.045, 1.0), clamp(metalness, 0.0, 1.0), materialOcclusion, clamp(lighting.MaterialExtra.z, 0.0, 1.0));
	outEmissive = vec4(emissive, clamp(lighting.MaterialExtra.w, 0.0, 1.0));
	if (lighting.Skin.x > 0.5) {
		// Skin pixels repack channels a skin surface does not use (see
		// deferred-lighting.glsl `SkinSurface`): normal alpha 2/3 marks skin, albedo
		// alpha holds the profile, emissive alpha the scatter radius, metalness the
		// thickness in quarters of the radius, and specular the second lobe as two
		// nibbles.
		float radius = max(lighting.Skin.z, 1e-4);
		float thickness = lighting.Skin.w * (lighting.SkinLobe.z > 0.5 ? texture(thicknessMap, surface.cellUv).r : 1.0);
		bool transmits = lighting.Skin.w > 0.0;
		float lobeFlags = floor(lighting.SkinLobe.w + 0.5);
		vec2 lobe = mod(lobeFlags, 2.0) > 0.5 ? texture(secondaryRoughnessMap, surface.cellUv).rg : lighting.SkinLobe.xy;
		float weightChannel = floor(lobeFlags / 2.0) - 1.0;
		if (weightChannel >= 0.0) lobe.y = PackedPbrValue(surface.cellUv, weightChannel);
		lobe = floor(clamp(lobe, 0.0, 1.0) * 15.0 + 0.5);
		bool preIntegrated = lighting.Skin.x > 1.5;
		outAlbedo.a = (min(clamp(lighting.Skin.y, 0.0, 255.0), 127.0) + (preIntegrated ? 128.0 : 0.0)) / 255.0;
		outNormal.a = 2.0 / 3.0;
		// Geometric normal, so pores and wrinkles do not read as curvature.
		vec3 across = fwidth(normalize(inNormal));
		float curvature = length(across) / max(length(fwidth(inWorldPosition)), 1e-5);
		outEmissive.a = preIntegrated ? min(radius * curvature, 1.0) : radius;
		outMaterial.g = transmits ? clamp(thickness / radius * 0.25, 0.0, 1.0) : 1.0;
		outMaterial.a = (lobe.x * 16.0 + lobe.y) / 255.0;
	}
	outObjectId = inLabels.x;
	outSemanticId = inLabels.y;
	outPartId = inLabels.z;
	// This is the mesh-authored coordinate before repeat, flipbook, parallax,
	// or any material lookup changes it.
	outMeshUv = inTexCoord;
}
