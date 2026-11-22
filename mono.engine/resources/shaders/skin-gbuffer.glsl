// Skin pixels in the G-buffer. gbuffer.frag marks them with normal alpha 2/3 and
// repacks channels a skin surface does not use:
//   albedo.a    subsurface profile id / 255
//   emissive.a  scatter radius in world units
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
	int profile = int(profileByte * 255.0 + 0.5);
	if (profile == 2) return vec3(1.0, 0.3, 0.18);
	if (profile == 3) return vec3(1.0, 0.85, 0.7);
	return vec3(1.0, 0.42, 0.26);
}
