// Skin pixels in the G-buffer. gbuffer.frag marks them with normal alpha 2/3 and
// repacks channels a skin surface does not use:
//   albedo.a    subsurface profile id / 255; 128 is added for pre-integrated skin
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
	int profile = int(profileByte * 255.0 + 0.5) & 127;
	if (profile == 2) return vec3(1.0, 0.3, 0.18);
	if (profile == 3) return vec3(1.0, 0.85, 0.7);
	return vec3(1.0, 0.42, 0.26);
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
