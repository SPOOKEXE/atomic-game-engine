// engine/surface: what an authored `Surface` and `Light` are written against.

struct SurfaceInput {
	vec2 UV;          // within the tile, after the flipbook cell
	vec2 MeshUV;      // as authored, UDIM tile included
	vec3 WorldPosition;
	vec3 Normal;      // geometric
	vec3 View;        // towards the eye
	vec4 Tint;        // part colour and opacity
	vec4 Attributes[4];
	float Time;
};

struct SurfaceOutput {
	vec3 Albedo;
	vec3 Normal;
	float Roughness;
	float Metalness;
	float Occlusion;
	float Specular;
	vec3 Emissive;
	float Alpha;
	// Tag other passes can test on pixels with a Light function, 0 to 1.
	float User;
	// Free values a Surface hands to its Light.
	vec4 Custom[4];
};

struct LightSample {
	vec3 Direction;   // towards the light
	vec3 Colour;      // after distance and cone, before Shadow
	float Shadow;     // 0 fully shadowed to 1 lit
	bool IsSun;
};

layout(location = 11) in vec4 inAttribute0;
layout(location = 12) in vec4 inAttribute1;
layout(location = 13) in vec4 inAttribute2;
layout(location = 14) in vec4 inAttribute3;

// Tangent-space normal `mapped` (0 to 1) at `uv`, in world space.
vec3 ApplyNormalMap(vec3 normal, vec2 uv, vec3 mapped) {
	return normalize(CotangentFrame(normal, inWorldPosition, uv) * (mapped * 2.0 - 1.0));
}

// Two normals over one base, the second detail on top of the first.
vec3 BlendNormals(vec3 base, vec3 detail, vec3 geometric) {
	return normalize(base + detail - geometric * dot(detail, geometric));
}

// The engine's own reading of the part's maps, for a Surface that adds to it.
#define ENGINE_DEFAULT_SURFACE
void DefaultSurface(SurfaceInput i, inout SurfaceOutput o) {
	vec4 sampled = lighting.Surface.x > 0.5 ? texture(colourMap, i.UV) : vec4(1.0);
	uint alphaMode = inAppearance & 0xFFu;
	float materialAlpha = sampled.a * lighting.BaseColour.a;
	if (alphaMode == 1u && inColour.a >= 0.98 && materialAlpha < float((inAppearance >> 8u) & 0xFFu) / 255.0) discard;
	vec3 mapped = sampled.rgb * lighting.BaseColour.rgb;
	vec3 albedo = inColour.rgb * mapped * inSurfaceColour;
	if (alphaMode == 0u) albedo = mix(inColour.rgb, albedo, materialAlpha);
	else if (alphaMode == 2u) albedo = inColour.rgb * mapped * mix(vec3(1.0), inSurfaceColour, materialAlpha);
	o.Albedo = albedo;
	o.Alpha = alphaMode == 1u ? inColour.a * materialAlpha : inColour.a;
	if (lighting.Material.x > 0.5) o.Normal = ApplyNormalMap(i.Normal, i.UV, texture(normalMap, i.UV).xyz);
	if (lighting.Material.y > 0.5) o.Roughness = texture(roughnessMap, i.UV).r;
	if (lighting.Material.z > 0.5) o.Occlusion = texture(occlusionMap, i.UV).r;
	if (lighting.MaterialExtra.x > 0.5) o.Metalness = texture(metalnessMap, i.UV).r;
	if (lighting.MaterialExtra.y > 0.5) {
		if (lighting.PackedPbrChannels.x < 4.0) o.Roughness = PackedPbrValue(i.UV, lighting.PackedPbrChannels.x);
		if (lighting.PackedPbrChannels.y < 4.0) o.Occlusion = PackedPbrValue(i.UV, lighting.PackedPbrChannels.y);
		if (lighting.PackedPbrChannels.w < 4.0) o.Metalness = PackedPbrValue(i.UV, lighting.PackedPbrChannels.w);
	}
	if ((ResolvedSurfaceFeatures() & FEATURE_EMISSION) != 0u && lighting.Material.w > 0.5)
		o.Emissive *= texture(emissiveMap, i.UV).rgb;
}
