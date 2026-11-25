// Skin pixels in the G-buffer. gbuffer.frag marks them with normal alpha 2/3 and
// repacks channels a skin surface does not use:
//   albedo.a    bits 0-1 subsurface profile id, 2-4 clear coat weight, 5-6 clear coat
//               roughness, 7 pre-integrated skin
//   emissive.a  scatter radius in world units, or for pre-integrated skin that
//               radius times the surface's curvature
//   material.g  thickness in quarters of the scatter radius (no metalness)
//   material.a  second specular lobe: roughness nibble * 16 + weight nibble
// Default pixels write normal alpha 1 and unlit ones 0.

bool IsSkinPixel(float normalAlpha) {
	return normalAlpha > 0.5 && normalAlpha < 0.84;
}

// Metalness as the lighting expects it; skin is a dielectric.
float GBufferMetalness(vec4 material, float normalAlpha) {
	return IsSkinPixel(normalAlpha) ? 0.0 : clamp(material.g, 0.0, 1.0);
}

// Per-channel scatter distance relative to the scatter radius, by profile id:
// 1 skin, 2 deep skin, 3 wax. Red travels furthest in skin.
vec3 SkinProfileFalloff(float profileByte) {
	int profile = int(profileByte * 255.0 + 0.5) & 3;
	if (profile == 2) return vec3(1.0, 0.3, 0.18);
	if (profile == 3) return vec3(1.0, 0.85, 0.7);
	return vec3(1.0, 0.42, 0.26);
}

// Clear coat roughness the G-buffer can hold, by level.
const vec4 COAT_ROUGHNESS = vec4(0.05, 0.12, 0.22, 0.35);

// Packs the skin byte. The weight is dithered by pixel so its eight levels do not band.
float PackSkinByte(float profile, bool preIntegrated, float coat, float coatRoughness, vec2 pixel) {
	int id = int(profile + 0.5);
	if (id < 1 || id > 3) id = 1;
	float noise = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
	int weight = coat <= 0.0 ? 0 : int(clamp(floor(clamp(coat, 0.0, 1.0) * 7.0 + noise), 0.0, 7.0));
	int level = coatRoughness < 0.085 ? 0 : coatRoughness < 0.17 ? 1 : coatRoughness < 0.285 ? 2 : 3;
	return float(id | weight << 2 | level << 5 | (preIntegrated ? 128 : 0)) / 255.0;
}

// Clear coat weight (x) and roughness (y) of a skin byte.
vec2 SkinCoat(float profileByte) {
	int value = int(profileByte * 255.0 + 0.5);
	return vec2(float((value >> 2) & 7) / 7.0, COAT_ROUGHNESS[(value >> 5) & 3]);
}

// Whether a skin pixel scatters in the lighting rather than the screen-space pass.
bool IsPreIntegrated(float profileByte) {
	return profileByte * 255.0 > 127.5;
}

// Pre-integrated diffuse response: the clamped cosine convolved with each channel's
// Gaussian scatter over a ring. spread is the scatter radius times the curvature, so
// it is an angle. In closed form (a ramp convolved with a Gaussian of width s) it is
// x * cdf(x / s) + s * pdf(x / s), which replaces a curvature by N.L table.
vec3 PreIntegratedDiffuse(float cosine, float spread, vec3 falloff) {
	vec3 s = clamp(spread * falloff, vec3(0.005), vec3(0.75));
	vec3 t = vec3(cosine) / s;
	vec3 density = 0.39894228 * exp(-0.5 * t * t);
	// Logistic approximation of the normal distribution function.
	vec3 cumulative = 1.0 / (1.0 + exp(-1.702 * t));
	return max(cosine * cumulative + s * density, vec3(0.0));
}
