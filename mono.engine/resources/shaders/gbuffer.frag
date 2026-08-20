#version 450

// Records the opaque material once so lighting and screen-space effects shade
// the visible pixel rather than every fragment that happened to cover it.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColour;
layout(location = 4) in vec2 inTexCoord;
layout(location = 5) in vec3 inWorldPosition;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outEmissive;

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
} lighting;

mat3 CotangentFrame(vec3 normal, vec3 position, vec2 uv) {
	vec3 positionX = dFdx(position);
	vec3 positionY = dFdy(position);
	vec2 uvX = dFdx(uv);
	vec2 uvY = dFdy(uv);
	vec3 tangent = positionX * uvY.y - positionY * uvX.y;
	vec3 bitangent = -positionX * uvY.x + positionY * uvX.x;
	float scale = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-8));
	return mat3(tangent * scale, bitangent * scale, normal);
}

void main() {
	vec3 normal = normalize(inNormal);
	vec2 localUv = fract(inTexCoord);
	vec2 cellUv = localUv * lighting.Flipbook.x + lighting.Flipbook.yz;
	if (lighting.Surface.z > 0.5) {
		mat3 tangentFrame = CotangentFrame(normal, inWorldPosition, cellUv);
		vec3 tangentEye = transpose(tangentFrame) * normalize(lighting.Eye.xyz - inWorldPosition);
		float height = texture(heightMap, cellUv).r - 0.5;
		float grazing = max(abs(tangentEye.z), 0.2);
		localUv = fract(localUv - tangentEye.xy * (height * lighting.Surface.w / grazing));
		cellUv = localUv * lighting.Flipbook.x + lighting.Flipbook.yz;
	}
	vec4 sampled = lighting.Surface.x > 0.5 ? texture(colourMap, cellUv) : vec4(1.0);
	float alpha = inColour.a * sampled.a * lighting.BaseColour.a;
	if (lighting.Surface.y > 0.0 && alpha < lighting.Surface.y) {
		discard;
	}
	if (dot(lighting.SeamPlane.xyz, lighting.SeamPlane.xyz) > 0.0 &&
		dot(inWorldPosition, lighting.SeamPlane.xyz) < lighting.SeamPlane.w) {
		discard;
	}

	if (lighting.Material.x > 0.5) {
		vec3 mapped = texture(normalMap, cellUv).xyz * 2.0 - 1.0;
		normal = normalize(CotangentFrame(normal, inWorldPosition, cellUv) * mapped);
	}

	float roughness = lighting.Material.y > 0.5 ? texture(roughnessMap, cellUv).r : 0.65;
	float materialOcclusion = lighting.Material.z > 0.5 ? texture(occlusionMap, cellUv).r : 1.0;
	vec3 emissive = lighting.Material.w > 0.5 ? texture(emissiveMap, cellUv).rgb : vec3(0.0);

	outAlbedo = vec4(inColour.rgb * sampled.rgb * lighting.BaseColour.rgb, alpha);
	outNormal = vec4(normal * 0.5 + 0.5, 1.0);
	outMaterial = vec4(clamp(roughness, 0.045, 1.0), 0.0, materialOcclusion, 0.0);
	outEmissive = vec4(emissive, 1.0);
}
