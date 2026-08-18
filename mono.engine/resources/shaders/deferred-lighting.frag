#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;

layout(set = 2, binding = 0) uniform sampler2D albedoImage;
layout(set = 2, binding = 1) uniform sampler2D normalImage;
layout(set = 2, binding = 2) uniform sampler2D materialImage;
layout(set = 2, binding = 3) uniform sampler2D emissiveImage;
layout(set = 2, binding = 4) uniform sampler2D depthImage;
layout(set = 2, binding = 5) uniform sampler2D occlusionImage;
layout(set = 2, binding = 6) uniform sampler2D shadowImage;

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
	vec4 FogColour;
	// x: fog start. y: fog end.
	vec4 Fog;
	// x: shadow map present. y: one shadow texel.
	vec4 Shadow;
} pass;

layout(set = 3, binding = 1) uniform Lights {
	vec4 Position[MAX_LIGHTS];
	vec4 Colour[MAX_LIGHTS];
	vec4 Direction[MAX_LIGHTS];
	vec4 Count;
} lights;

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
	float nearPlane = pass.Planes.x;
	float farPlane = pass.Planes.y;
	float raw = (farPlane - nearPlane * farPlane / max(distance, 1e-4)) /
		max(farPlane - nearPlane, 1e-6);
	vec4 world = pass.InverseViewProjection * vec4(uv * 2.0 - 1.0, raw, 1.0);
	return world.xyz / max(world.w, 1e-6);
}

float ShadowFactor(vec3 world, vec3 normal, vec3 toLight) {
	if (pass.Shadow.x < 0.5) {
		return 1.0;
	}
	vec4 lightPosition = pass.LightViewProjection * vec4(world, 1.0);
	vec3 projected = lightPosition.xyz / max(lightPosition.w, 1e-6);
	vec2 uv = vec2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || projected.z > 1.0) {
		return 1.0;
	}
	float bias = 0.0015 + 0.0045 * (1.0 - max(dot(normal, toLight), 0.0));
	vec2 texel = vec2(pass.Shadow.y);
	vec2 offsets[4] = vec2[4](
		vec2(-0.5, -0.5) * texel,
		vec2(0.5, -0.5) * texel,
		vec2(-0.5, 0.5) * texel,
		vec2(0.5, 0.5) * texel
	);
	float lit = 0.0;
	for (int index = 0; index < 4; index++) {
		lit += projected.z - bias <= texture(shadowImage, uv + offsets[index]).r ? 1.0 : 0.0;
	}
	return lit * 0.25;
}

vec3 LocalLight(vec3 world, vec3 normal, vec3 albedo) {
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
		float lambert = max(dot(normal, direction), 0.0);
		float ratio = distanceToLight / range;
		float window = max(1.0 - ratio * ratio, 0.0);
		float falloff = window * window / (1.0 + distanceToLight * distanceToLight);
		float cone = lights.Direction[index].w;
		if (cone > -1.0) {
			float aligned = dot(-direction, normalize(lights.Direction[index].xyz));
			falloff *= smoothstep(cone, mix(cone, 1.0, 0.1), aligned);
		}
		total += albedo * lights.Colour[index].rgb * lambert * falloff;
	}
	return total;
}

void main() {
	vec2 geometryUv = inUv * pass.Target.zw;
	vec4 albedo = texture(albedoImage, geometryUv);
	vec4 packed = texture(normalImage, geometryUv);
	vec3 packedNormal = packed.xyz * 2.0 - 1.0;
	float depth = texture(depthImage, inUv).r;
	if (depth >= pass.Planes.y || packed.a < 0.5) {
		outColour = vec4(pass.FogColour.rgb, 1.0);
		return;
	}

	vec3 normal = normalize(packedNormal);
	vec4 material = texture(materialImage, geometryUv);
	vec3 emissive = texture(emissiveImage, geometryUv).rgb;
	float occlusion = material.b * texture(occlusionImage, inUv).r;
	float roughness = clamp(material.r, 0.045, 1.0);
	vec3 toLight = -normalize(pass.Direction.xyz);
	vec3 world = WorldAt(inUv, depth);
	vec3 viewDirection = normalize(pass.Eye.xyz - world);
	vec3 halfway = normalize(viewDirection + toLight);
	float lambert = max(dot(normal, toLight), 0.0);
	vec3 fresnel = FresnelSchlick(max(dot(halfway, viewDirection), 0.0), vec3(0.04));
	float distribution = DistributionGGX(normal, halfway, roughness);
	float geometry = GeometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
		GeometrySchlickGGX(lambert, roughness);
	vec3 specular = distribution * geometry * fresnel /
		max(4.0 * max(dot(normal, viewDirection), 0.0) * lambert, 1e-4);
	float sky = max(normal.y, 0.0);
	vec3 ambient = albedo.rgb * (pass.Ambient.rgb + pass.OutdoorAmbient.rgb * sky) * occlusion;
	vec3 direct = (albedo.rgb * (1.0 - fresnel) * lambert + specular) * pass.Direct.rgb *
		ShadowFactor(world, normal, toLight);
	vec3 lit = ambient + direct + LocalLight(world, normal, albedo.rgb) + emissive;
	float fogRange = max(pass.Fog.y - pass.Fog.x, 0.0001);
	float fog = clamp((distance(world, pass.Eye.xyz) - pass.Fog.x) / fogRange, 0.0, 1.0);
	outColour = vec4(mix(lit, pass.FogColour.rgb, fog), albedo.a);
}
