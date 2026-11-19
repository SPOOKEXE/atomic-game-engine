#ifndef WEATHER_PROFILE_GLSL
#define WEATHER_PROFILE_GLSL

// Presentation profiles for `CloudComputeShader`. They shape only what the
// cloud and cloud-shadow passes draw: the canonical field, CPU samples and
// weather events are untouched. Both passes call these with the same layer,
// so a cloud casts the body that is drawn. Cumulus is the canonical body.

const uint PROFILE_CUMULUS = 0u;
const uint PROFILE_STRATUS = 1u;
const uint PROFILE_STORM = 2u;
const uint PROFILE_VOXEL = 3u;

// Fraction through the layer, 0 at its base and 1 at its top.
float ProfileHeight(vec3 world, float base, float top) {
	return clamp((world.y - base) / max(top - base, 1e-3), 0.0, 1.0);
}

// Vertical body: where in the layer each preset keeps its density.
float ProfileDensity(uint profile, float density, float height) {
	if (profile == PROFILE_STRATUS)
		// A thin even sheet low in the layer, flattened towards uniform cover.
		return min(1.0, density * 1.6) * smoothstep(0.05, 0.15, height) * (1.0 - smoothstep(0.25, 0.4, height));
	if (profile == PROFILE_STORM)
		// Towers through almost the whole layer, denser than fair weather.
		return min(1.0, density * 1.5) * smoothstep(0.0, 0.05, height) * (1.0 - smoothstep(0.85, 1.0, height));
	// Cumulus, the default, is the canonical body unchanged, so worlds authored
	// before profiles existed draw and shade exactly as they did. Voxel only
	// moves where it samples.
	return density;
}

// Storm bases are dark: less light reaches the bottom of a deep tower.
float ProfileLight(uint profile, float height) {
	return profile == PROFILE_STORM ? mix(0.35, 1.0, height) : 1.0;
}

// Voxel clouds sample the field at block centres, two cells across.
vec3 ProfileSample(uint profile, vec3 world, float cellSize) {
	if (profile != PROFILE_VOXEL) return world;
	float block = 2.0 * cellSize;
	return (floor(world / block) + 0.5) * block;
}

#endif
