#version 450
#extension GL_GOOGLE_include_directive : require
#include "deferred-lighting.glsl"

layout(location = 0) out vec4 outContribution;
layout(location = 1) out float outVisibility;

// `pass.Shadow.z` is -1 for a spot light. Point lights render one cube face at
// a time, so the six passes partition fragments by their dominant light axis.
bool LocalShadowFace(vec3 fromLight) {
	if (pass.Shadow.z < 0.0) return true;
	int face = int(pass.Shadow.z + 0.5);
	vec3 axis = abs(fromLight);
	if (axis.x >= axis.y && axis.x >= axis.z) return face == (fromLight.x >= 0.0 ? 0 : 1);
	if (axis.y >= axis.z) return face == (fromLight.y >= 0.0 ? 2 : 3);
	return face == (fromLight.z >= 0.0 ? 4 : 5);
}

float LocalShadowVisibility(vec3 world, vec3 normal, vec3 direction, vec3 lightPosition) {
	if (pass.Shadow.x < 0.5) return 1.0;
	if (!LocalShadowFace(world - lightPosition)) return 0.0;
	// The local map is rasterized from a separate view. Offset its receiver along
	// the geometric normal before the shared depth comparison to avoid acne.
	return DirectionalShadowVisibility(
		shadowImage, pass.LightViewProjection, pass.Shadow, world + normal * 0.005, normal, direction
	);
}

void main() {
	vec2 geometryUv = inUv * pass.Target.zw;
	vec4 albedo = texture(albedoImage, geometryUv);
	vec4 packed = texture(normalImage, geometryUv);
	float depth = texture(depthImage, inUv).r;
	if (depth >= pass.Planes.y || packed.a < 0.5) {
		outContribution = vec4(0.0);
		outVisibility = 0.0;
		return;
	}
	vec3 normal = normalize(packed.xyz * 2.0 - 1.0);
	vec4 material = texture(materialImage, geometryUv);
	float metalness = clamp(material.g, 0.0, 1.0);
	vec3 world = WorldAt(inUv, depth);
	vec3 viewDirection = normalize(pass.Eye.xyz - world);
	vec3 toLight = -normalize(pass.Direction.xyz);
	vec3 halfway = normalize(viewDirection + toLight);
	vec3 baseReflectance = mix(vec3(0.04), albedo.rgb, metalness);
	vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDirection), 0.0), baseReflectance);
	vec3 diffuse = albedo.rgb * (1.0 - fresnel) * (1.0 - metalness);
	vec3 offset = lights.Position[0].xyz - world;
	float distanceToLight = length(offset);
	float range = lights.Position[0].w;
	if (range <= 0.0 || distanceToLight > range) {
		outContribution = vec4(0.0);
		outVisibility = 0.0;
		return;
	}
	vec3 direction = offset / max(distanceToLight, 1e-4);
	float lambert = max(dot(normal, direction), 0.0);
	float ratio = distanceToLight / range;
	float window = max(1.0 - ratio * ratio, 0.0);
	float falloff = window * window / (1.0 + distanceToLight * distanceToLight);
	float cone = lights.Direction[0].w;
	if (cone > -1.0) {
		float aligned = dot(-direction, normalize(lights.Direction[0].xyz));
		falloff *= smoothstep(cone, mix(cone, 1.0, 0.1), aligned);
	}
	float visibility = LocalShadowVisibility(world, normal, direction, lights.Position[0].xyz);
	// One selected light is bound by the capture path. Alpha remains zero so planes add as radiance.
	outContribution = vec4(diffuse * lights.Colour[0].rgb * lambert * falloff * visibility, 0.0);
	outVisibility = visibility;
}
