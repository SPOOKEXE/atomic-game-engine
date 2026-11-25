// Shared shading for native colour and optional unrounded capture responses.
#include "projection.glsl"
#include "ambient-lighting.glsl"
#include "weather-shadow-lighting.glsl"
#include "skin-gbuffer.glsl"

layout(location = 0) in vec2 inUv;

layout(set = 2, binding = 0) uniform sampler2D albedoImage;
layout(set = 2, binding = 1) uniform sampler2D normalImage;
layout(set = 2, binding = 2) uniform sampler2D materialImage;
layout(set = 2, binding = 3) uniform sampler2D emissiveImage;
layout(set = 2, binding = 4) uniform sampler2D depthImage;
layout(set = 2, binding = 5) uniform sampler2D occlusionImage;
layout(set = 2, binding = 6) uniform sampler2D shadowImage;

// The seam light-field captures: each portal mouth's far room rendered against
// a lit void, projected back out of the entrance as spill. Two mouths is one
// pair, which is the prototype's budget; the renderer binds its fallback
// texture and zeroes the seam when fewer are live.
layout(set = 2, binding = 7) uniform sampler2D seamLightA;
layout(set = 2, binding = 8) uniform sampler2D seamLightB;

#define PORTAL_BEAM_SAMPLER_BINDING 9
#include "portal-shadow-beams.glsl"

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
	// x: fog start. y: fog end.
	vec4 Fog;
	// x: shadow map present. y: one shadow texel.
	vec4 Shadow;
	// Per seam light projector. Centre: xyz the mouth's centre, w whether the
	// slot is live. Outward: xyz the unit normal toward the room being lit,
	// w the spill range along it. First and Second: the mouth's half axes.
	vec4 SeamCentre[2];
	vec4 SeamOutward[2];
	vec4 SeamFirst[2];
	vec4 SeamSecond[2];
}
pass;

layout(set = 3, binding = 1) uniform Lights {
	vec4 Position[MAX_LIGHTS];
	vec4 Colour[MAX_LIGHTS];
	vec4 Direction[MAX_LIGHTS];
	vec4 Count;
}
lights;

float DistributionGGX(vec3 normal, vec3 halfway, float roughness) {
	float alpha = roughness * roughness;
	float alphaSquared = alpha * alpha;
	float normalHalf = max(dot(normal, halfway), 0.0);
	float denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
	return alphaSquared / max(3.14159265 * denominator * denominator, 1e-5);
}

float GeometrySchlickGGX(float cosine, float roughness) {
	float radius = roughness + 1.0;
	float factor = radius * radius * 0.125;
	return cosine / max(cosine * (1.0 - factor) + factor, 1e-5);
}

vec3 FresnelSchlick(float cosine, vec3 base) {
	return base + (1.0 - base) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

vec3 WorldAt(vec2 uv, float distance) {
	return WorldAtLinearDepth(pass.InverseViewProjection, pass.CameraDepth, uv, distance);
}

#include "directional-shadow.glsl"

float ShadowFactor(vec3 world, vec3 normal, vec3 toLight) {
    return DirectionalShadowVisibility(shadowImage, pass.LightViewProjection, pass.Shadow, world, normal, toLight);
}

// Back-lit transmission weight: light arriving from behind the surface, wrapped
// so it fades in before the light is directly behind.
float TransmissionWrap(vec3 normal, vec3 toLight) {
	return clamp((dot(-normal, toLight) + 0.3) / 1.3, 0.0, 1.0);
}

// Diffuse local lights. transmitted is the skin colour that passes through thin
// parts (zero off skin) and lights the side facing away from the light.
// spread is the pre-integrated scatter angle, or negative for Lambert.
vec3 LocalLight(vec3 world, vec3 normal, vec3 albedo, vec3 transmitted, float spread, vec3 scatter) {
	vec3 total = vec3(0.0);
	for (int index = 0; index < MAX_LIGHTS; index++) {
		if (index >= int(lights.Count.x)) {
			break;
		}
		vec3 offset = lights.Position[index].xyz - world;
		float range = lights.Position[index].w;
		float distanceToLight = length(offset);
		if (range <= 0.0 || distanceToLight > range) {
			continue;
		}
		vec3 direction = offset / max(distanceToLight, 1e-4);
		float cosine = dot(normal, direction);
		vec3 lambert = spread < 0.0 ? vec3(max(cosine, 0.0)) : PreIntegratedDiffuse(cosine, spread, scatter);
		float ratio = distanceToLight / range;
		float window = max(1.0 - ratio * ratio, 0.0);
		float falloff = window * window / (1.0 + distanceToLight * distanceToLight);
		float cone = lights.Direction[index].w;
		if (cone > -1.0) {
			float aligned = dot(-direction, normalize(lights.Direction[index].xyz));
			falloff *= smoothstep(cone, mix(cone, 1.0, 0.1), aligned);
		}
		total += (albedo * lambert + transmitted * TransmissionWrap(normal, direction)) *
				 lights.Colour[index].rgb * falloff;
	}
	return total;
}

// Light spilling out of one portal entrance: the matching capture, projected
// from the mouth's rectangle onto nearby geometry.
//
// The seam is treated as a textured window: a receiver is projected back onto
// the mouth's plane, the capture is sampled there, and the sample is attenuated
// by distance from the plane, by incidence, and by how far outside the
// rectangle the receiver sits. The cone widens with distance so the pool reads
// as light through an opening rather than a hard-edged projector.
vec3 SeamSpill(int seam, vec3 world, vec3 normal, vec3 albedo) {
	if (pass.SeamCentre[seam].w < 0.5) {
		return vec3(0.0);
	}
	vec3 outward = pass.SeamOutward[seam].xyz;
	float range = pass.SeamOutward[seam].w;
	vec3 offset = world - pass.SeamCentre[seam].xyz;
	float depth = dot(offset, outward);
	if (depth <= 0.0 || depth >= range) {
		return vec3(0.0);
	}

	vec3 first = pass.SeamFirst[seam].xyz;
	vec3 second = pass.SeamSecond[seam].xyz;
	vec3 planar = offset - outward * depth;
	float alongFirst = dot(planar, first) / max(dot(first, first), 1e-4);
	float alongSecond = dot(planar, second) / max(dot(second, second), 1e-4);

	// The 45-degree spread: a receiver one half-axis past the rim is still lit
	// at one half-axis of depth. Sampling clamps to the rectangle, so the rim
	// of the pool repeats the capture's edge rather than the void.
	float spreadFirst = 1.0 + depth / max(length(first), 1e-3);
	float spreadSecond = 1.0 + depth / max(length(second), 1e-3);
	float outsideFirst = max(abs(alongFirst) - 1.0, 0.0) / max(spreadFirst - 1.0, 1e-3);
	float outsideSecond = max(abs(alongSecond) - 1.0, 0.0) / max(spreadSecond - 1.0, 1e-3);
	float fade = (1.0 - smoothstep(0.0, 1.0, outsideFirst)) * (1.0 - smoothstep(0.0, 1.0, outsideSecond));
	if (fade <= 0.0) {
		return vec3(0.0);
	}

	vec2 uv = vec2(clamp(alongFirst, -1.0, 1.0) * 0.5 + 0.5, 0.5 - clamp(alongSecond, -1.0, 1.0) * 0.5);
	vec3 captured = seam == 0 ? texture(seamLightA, uv).rgb : texture(seamLightB, uv).rgb;

	// Incidence is taken from the mouth's centre rather than the nearest rim
	// point: light through a doorway arrives from the whole opening, and the
	// rim direction is parallel to any floor the doorway meets - which would
	// zero exactly the pool a lit door visibly throws.
	float lambert = max(dot(normal, normalize(pass.SeamCentre[seam].xyz - world)), 0.0);
	float ratio = depth / range;
	float window = (1.0 - ratio * ratio) * (1.0 - ratio * ratio);
	return albedo * captured * lambert * window * fade;
}

vec4 ShadeDeferred(out vec4 directionalResponse) {
	directionalResponse = vec4(0.0, 0.0, 0.0, 1.0);
	vec2 geometryUv = inUv * pass.Target.zw;
	vec4 albedo = texture(albedoImage, geometryUv);
	vec4 packed = texture(normalImage, geometryUv);
	vec3 packedNormal = packed.xyz * 2.0 - 1.0;
	float depth = texture(depthImage, inUv).r;
	if (depth >= pass.Planes.y) {
		return vec4(pass.FogColour.rgb, 1.0);
	}
	// Depth with no material normal carries authored final colour in emissive.
	if (packed.a < 0.5) {
		return texture(emissiveImage, geometryUv);
	}

	vec3 normal = normalize(packedNormal);
	vec4 material = texture(materialImage, geometryUv);
	bool skin = IsSkinPixel(packed.a);
	vec3 emissive = texture(emissiveImage, geometryUv).rgb;
	float occlusion = material.b * texture(occlusionImage, inUv).r;
	float roughness = clamp(material.r, 0.045, 1.0);
	float metalness = GBufferMetalness(material, packed.a);
	vec3 toLight = -normalize(pass.Direction.xyz);
	vec3 world = WorldAt(inUv, depth);
	vec3 viewDirection = normalize(pass.Eye.xyz - world);
	vec3 halfway = normalize(viewDirection + toLight);
	float lambert = max(dot(normal, toLight), 0.0);
	// Skin is a dielectric with a fixed reflectance; its specular byte holds the second lobe.
	vec3 baseReflectance = skin ? vec3(0.028) : mix(vec3(0.04), albedo.rgb, metalness) * clamp(material.a, 0.0, 1.0);
	vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDirection), 0.0), baseReflectance);
	float distribution = DistributionGGX(normal, halfway, roughness);
	float geometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
					 GeometrySchlickGGX(lambert, roughness);
	vec3 specular =
		distribution * geometry * fresnel / max(4.0 * max(dot(normal, viewDirection), 0.0) * lambert, 1e-4);
	vec3 transmitted = vec3(0.0);
	if (skin) {
		// Second, sharper lobe for the oily layer, mixed over the broad skin lobe.
		float lobeByte = floor(material.a * 255.0 + 0.5);
		float secondRoughness = clamp(floor(lobeByte / 16.0) / 15.0, 0.045, 1.0);
		float secondWeight = mod(lobeByte, 16.0) / 15.0;
		float secondGeometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), secondRoughness) *
							   GeometrySchlickGGX(lambert, secondRoughness);
		vec3 second = DistributionGGX(normal, halfway, secondRoughness) * secondGeometry * fresnel /
					  max(4.0 * max(dot(normal, viewDirection), 0.0) * lambert, 1e-4);
		specular = mix(specular, second, secondWeight);
		// Thin parts pass light through, reddened by the profile's falloff.
		float thickness = material.g * 4.0;
		// The byte saturates at four radii, which means opaque, so it fades out there.
		transmitted = albedo.rgb * exp(-thickness / SkinProfileFalloff(albedo.a)) *
					  (1.0 - smoothstep(0.75, 1.0, material.g));
	}
	vec3 diffuse = albedo.rgb * (1.0 - fresnel) * (1.0 - metalness);
	vec3 ambient = AmbientRadiance(albedo.rgb, metalness, normal, pass.Ambient.rgb,
		pass.OutdoorAmbient.rgb, occlusion);
	vec3 falloff = SkinProfileFalloff(albedo.a);
	float spread = skin && IsPreIntegrated(albedo.a) ? texture(emissiveImage, geometryUv).a : -1.0;
	float cosine = dot(normal, toLight);
	vec3 response = spread < 0.0 ? vec3(lambert) : PreIntegratedDiffuse(cosine, spread, falloff);
	vec3 surfaceResponse = diffuse * response + specular;
	if (skin) {
		// A water-like coat (reflectance 0.02) takes its share of the light first.
		vec2 coat = SkinCoat(albedo.a);
		if (coat.x > 0.0) {
			float coatFresnel = FresnelSchlick(max(dot(halfway, viewDirection), 0.0), vec3(0.02)).r * coat.x;
			float coatGeometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), coat.y) *
								 GeometrySchlickGGX(lambert, coat.y);
			float coatLobe = DistributionGGX(normal, halfway, coat.y) * coatGeometry * coatFresnel /
							 max(4.0 * max(dot(normal, viewDirection), 0.0) * lambert, 1e-4);
			surfaceResponse = surfaceResponse * (1.0 - coatFresnel) + vec3(coatLobe);
		}
	}
	vec3 unshadowedDirect = surfaceResponse * pass.Direct.rgb;
	float shadow = ShadowFactor(world, normal, toLight);
	// The surface's own shadow begins at the terminator and would cut off the light
	// scattered past it.
	if (spread >= 0.0) shadow = mix(1.0, shadow, smoothstep(0.0, 0.1, cosine));
	float shadowVisibility = min(shadow, PortalBeamFactor(world));
	vec3 transmittedDirect = unshadowedDirect * WeatherShadowFactor(world);
	vec3 direct = transmittedDirect * shadowVisibility;
	// The surface's own shadow map would block light arriving from behind, so the
	// back-lit term is weather-shadowed only and its strength comes from thickness.
	direct += transmitted * TransmissionWrap(normal, toLight) * pass.Direct.rgb * WeatherShadowFactor(world);
	vec3 lit = ambient + direct + LocalLight(world, normal, diffuse, transmitted, spread, falloff) + emissive +
			   SeamSpill(0, world, normal, albedo.rgb) + SeamSpill(1, world, normal, albedo.rgb);
	float fogRange = max(pass.Fog.y - pass.Fog.x, 0.0001);
	float fog = clamp((distance(world, pass.Eye.xyz) - pass.Fog.x) / fogRange, 0.0, 1.0);
	// Fog attenuates the directional term; its own colour does not change with shadows.
	directionalResponse = vec4(transmittedDirect * (1.0 - fog), shadowVisibility);
	return vec4(mix(lit, pass.FogColour.rgb, fog), skin ? 1.0 : albedo.a);
}

vec4 ShadeDeferred() {
	vec4 directionalResponse;
	return ShadeDeferred(directionalResponse);
}
