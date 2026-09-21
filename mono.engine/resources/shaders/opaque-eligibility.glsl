// Shared eligibility and UV policy for built-in opaque geometry passes.
// Callers declare the interpolants, material samplers, and `lighting` block.

const uint FEATURE_EMISSION = 1u << 2u;
const uint FEATURE_DISPLACEMENT = 1u << 9u;

struct OpaqueSurfaceSample {
	uint features;
	uint alphaMode;
	vec2 localUv;
	vec2 cellUv;
	vec4 colour;
	float materialAlpha;
	float alpha;
};

uint ResolvedSurfaceFeatures() {
	uint camera = (lighting.RenderFeatures.y | lighting.RenderFeatures.z) & ~lighting.RenderFeatures.w;
	return ((camera | inFeaturePolicy.x) & ~inFeaturePolicy.y) & lighting.RenderFeatures.x;
}

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

OpaqueSurfaceSample SampleEligibleOpaqueSurface() {
	OpaqueSurfaceSample result;
	result.features = ResolvedSurfaceFeatures();
	result.localUv = fract(inTexCoord);
	result.cellUv = result.localUv * lighting.Flipbook.x + lighting.Flipbook.yz;
	if ((result.features & FEATURE_DISPLACEMENT) != 0u && lighting.Surface.z > 0.5) {
		mat3 tangentFrame = CotangentFrame(normalize(inNormal), inWorldPosition, result.cellUv);
		vec3 tangentEye = transpose(tangentFrame) * normalize(lighting.Eye.xyz - inWorldPosition);
		float height = texture(heightMap, result.cellUv).r - 0.5;
		float grazing = max(abs(tangentEye.z), 0.2);
		result.localUv = fract(result.localUv - tangentEye.xy * (height * lighting.Surface.w / grazing));
		result.cellUv = result.localUv * lighting.Flipbook.x + lighting.Flipbook.yz;
	}
	result.colour = lighting.Surface.x > 0.5 ? texture(colourMap, result.cellUv) : vec4(1.0);
	result.alphaMode = inAppearance & 0xFFu;
	float cutoff = float((inAppearance >> 8u) & 0xFFu) / 255.0;
	result.materialAlpha = result.colour.a * lighting.BaseColour.a;
	if (result.alphaMode == 1u && inColour.a >= 0.98 && result.materialAlpha < cutoff) {
		discard;
	}
	result.alpha = result.alphaMode == 1u ? inColour.a * result.materialAlpha : inColour.a;
	if (dot(lighting.SeamPlane.xyz, lighting.SeamPlane.xyz) > 0.0 &&
		dot(inWorldPosition, lighting.SeamPlane.xyz) < lighting.SeamPlane.w) {
		discard;
	}
	return result;
}
