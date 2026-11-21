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

// Value noise in [0, 1], for presentation detail only. Built on the field's
// own hash so it needs no texture and repeats nowhere a viewer can reach.
float ProfileNoise(vec3 position) {
	ivec3 cell = ivec3(floor(position));
	vec3 fraction = position - vec3(cell);
	fraction = fraction * fraction * (3.0 - 2.0 * fraction);
	float corners[8];
	for (int index = 0; index < 8; ++index) {
		ivec3 corner = cell + ivec3(index & 1, (index >> 1) & 1, (index >> 2) & 1);
		uint key = uint(corner.x) * 73856093u ^ uint(corner.y) * 19349663u ^ uint(corner.z) * 83492791u;
		corners[index] = float(WeatherHash(key)) / 4294967295.0;
	}
	float x0 = mix(mix(corners[0], corners[1], fraction.x), mix(corners[2], corners[3], fraction.x), fraction.y);
	float x1 = mix(mix(corners[4], corners[5], fraction.x), mix(corners[6], corners[7], fraction.x), fraction.y);
	return mix(x0, x1, fraction.z);
}

// Detail erosion: each preset's own small-scale texture on top of its body.
// Relative, never a fixed subtraction, so a thin sheet keeps its cover.
// Cumulus and Voxel keep the canonical edge.
float ProfileErode(uint profile, float density, vec3 world, float height, float cellSize) {
	if (!(density > 0.0)) return density;
	if (profile == PROFILE_STRATUS)
		// A soft fine ripple: at most a fifth thinner, never a hole.
		return density * (1.0 - 0.2 * ProfileNoise(world / (1.5 * cellSize)));
	if (profile == PROFILE_STORM) {
		// Turbulent at two scales, eating the thin edges and keeping the
		// core, more towards the top of the tower.
		float turbulence =
			0.6 * ProfileNoise(world / (2.0 * cellSize)) + 0.4 * ProfileNoise(world / (0.7 * cellSize));
		float edge = mix(0.04, 0.18, height) * turbulence;
		return max(0.0, (density - edge) / (1.0 - edge));
	}
	return density;
}

// One octave of `ProfileDetail` in [-1, 1], faded out as the clipmap voxel
// nears its period: a voxel that cannot hold the octave would alias it into
// per-voxel blocks, so a coarse level keeps only the octaves it can carry.
float ProfileOctave(vec3 world, float period, float voxel) {
	float keep = 1.0 - smoothstep(0.5, 1.0, voxel / period);
	return keep > 0.0 ? keep * (2.0 * ProfileNoise(world / period) - 1.0) : 0.0;
}

// Sub-cell texture for each clipmap level, baked once per voxel there.
// `voxel` is the level's voxel size: the broad octave survives to the
// coarsest level, so distant clouds keep a texture, and finer octaves join as
// the voxels shrink. The coarse path past the last level calls it per ray
// sample with a one-cell voxel, which leaves only the broad octave; the fine
// ones per sample would alias into speckle.
// Multiplicative, averaging one, so it textures a cloud without thinning it:
// the field's densities are small and a subtracted edge would erase them.
// Voxel keeps its blocks.
float ProfileDetail(uint profile, float density, vec3 world, float cellSize, float voxel) {
	if (!(density > 0.0) || profile == PROFILE_VOXEL) return density;
	float detail = 0.3 * ProfileOctave(world, 2.0 * cellSize, voxel) +
		0.45 * ProfileOctave(world, 0.75 * cellSize, voxel) + 0.25 * ProfileOctave(world, 0.4 * cellSize, voxel);
	float contrast = profile == PROFILE_STORM ? 0.6 : profile == PROFILE_STRATUS ? 0.3 : 0.45;
	return density * (1.0 + contrast * detail);
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
