#version 450
#extension GL_GOOGLE_include_directive : require
#include "deferred-lighting.glsl"

layout(location = 0) out vec4 outContribution;

void main() {
	vec2 geometryUv = inUv * pass.Target.zw;
	vec4 albedo = texture(albedoImage, geometryUv);
	vec4 packed = texture(normalImage, geometryUv);
	float depth = texture(depthImage, inUv).r;
	if (depth >= pass.Planes.y || packed.a < 0.5) {
		outContribution = vec4(0.0);
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
	// One selected light is bound by the capture path. Alpha remains zero so planes add as radiance.
	outContribution = vec4(LocalLight(world, normal, diffuse), 0.0);
}
