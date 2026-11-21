#ifndef WEATHER_CLOUDS_GLSL
#define WEATHER_CLOUDS_GLSL

// The cloud march and its bake variant share every field read. The march
// is `weather-clouds.comp`; `weather-bake.comp` defines WEATHER_BAKE.

#include "weather-common.glsl"
// The bake variant (`weather-bake.comp`) has no samplers, so its buffers
// start at binding 0.
#ifdef WEATHER_BAKE
#define WEATHER_BUFFER_BASE 0
#else
#define WEATHER_BUFFER_BASE 6
#endif
#define WEATHER_CLOCK_BINDING (WEATHER_BUFFER_BASE + 4)
#include "weather-clock.glsl"
#include "weather-assembly.glsl"
#include "weather-profile.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
#ifndef WEATHER_BAKE
layout(set = 0, binding = 0) uniform sampler2D depthImage;
// Raw density baked from the field by `weather-bake.comp`; see `pass.Bake*`.
layout(set = 0, binding = 2) uniform sampler3D bakedDensity;
// Finished density near the camera from `weather-detail.comp`, one texture per
// clipmap level, finest first; see `pass.Detail*`.
layout(set = 0, binding = 3) uniform sampler3D detailDensity0;
layout(set = 0, binding = 4) uniform sampler3D detailDensity1;
layout(set = 0, binding = 5) uniform sampler3D detailDensity2;
#endif
layout(set = 0, binding = WEATHER_BUFFER_BASE, std430) readonly buffer Cells { uvec2 cells[]; } inputField;
layout(set = 0, binding = WEATHER_BUFFER_BASE + 1, std430) readonly buffer Summaries { uvec4 values[]; } inputSummary;
layout(set = 0, binding = WEATHER_BUFFER_BASE + 2, std430) readonly buffer Pages { ivec4 keys[]; } pageTable;
layout(set = 0, binding = WEATHER_BUFFER_BASE + 3, std430) readonly buffer PreviousPages { ivec4 keys[]; } previousPageTable;
#ifdef WEATHER_BAKE
layout(set = 1, binding = 0, r16f) writeonly uniform image3D bakeImage;
#else
layout(set = 1, binding = 0, rgba16f) writeonly uniform image2D outputImage;
layout(set = 1, binding = 1, rgba16f) writeonly uniform image2D cloudDepthImage;
#endif
struct WeatherSunShadow {
	vec4 Regions[4];
	vec4 Controls;
	vec4 ToSun;
	vec4 Receiver;
};

layout(set = 2, binding = 0) uniform Pass {
	mat4 InverseViewProjection;
	vec4 Eye;
	vec4 Minimum;
	vec4 Maximum;
	vec4 ColourExtinction;
	vec4 Sun;
	vec4 SunColour;
	vec4 Ambient;
	uvec4 Quality;
	WeatherField Field;
	uvec4 ImageExtent;
	WeatherSunShadow SunShadow;
	// The baked density box: canonical cell origin and cell size, then the
	// voxel counts and whether it is in use. Voxel v holds the field at
	// canonical cell (origin + v), so hardware trilinear matches the field's.
	vec4 BakeOrigin;
	vec4 BakeVoxels;
	// Clipmap levels (`weather-detail.comp`), finest first: origin in the
	// level's own voxels and its voxel size, then the voxel counts and
	// whether it is in use.
	vec4 DetailOrigin[3];
	vec4 DetailVoxels[3];
} pass;

// The cloud shadow map cached this frame, read as each sample's sun
// transmittance. Zero cascades means none, and every sample is fully lit.
#ifndef WEATHER_BAKE
#define WEATHER_SHADOW_INLINE_STATE
#define WEATHER_SHADOW_SAMPLER_SET 0
#define WEATHER_SHADOW_SAMPLER_BINDING 1
#define weatherShadow pass.SunShadow
#include "weather-shadow-lighting.glsl"
#endif

WeatherField effectiveField;

#define WEATHER_FIELD_CELLS inputField.cells
#define WEATHER_FIELD_PAGES pageTable.keys
#include "weather-field.glsl"

// Bound the LOD approximation at the actual integration samples. A few
// residency probes cannot certify contrast between those probes.
float allowedDensityError = 0.0;
bool sampledCompact = false;

vec3 WorldAt(vec2 uv, float depth) {
	vec4 position = pass.InverseViewProjection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	return position.xyz / position.w;
}

// Henyey-Greenstein phase for asymmetry `g` at the cosine between the ray and
// the direction to the sun.
float CloudPhase(float g, float cosine) {
	return (1.0 - g * g) / (12.566370614359 * max(pow(1.0 + g * g - 2.0 * g * cosine, 1.5), 0.0001));
}

bool Interval(vec3 origin, vec3 direction, float maximum, out float enter, out float exit) {
	enter = 0.0;
	exit = maximum;
	for (uint axis = 0u; axis < 3u; ++axis) {
		if (abs(direction[axis]) < 1e-7) {
			if (origin[axis] < pass.Minimum[axis] || origin[axis] > pass.Maximum[axis]) return false;
			continue;
		}
		float first = (pass.Minimum[axis] - origin[axis]) / direction[axis];
		float second = (pass.Maximum[axis] - origin[axis]) / direction[axis];
		enter = max(enter, min(first, second));
		exit = min(exit, max(first, second));
	}
	return exit > enter;
}

// Codes this ray has rebuilt for cells no resident page holds. Each such sample
// rebuilds eight with the full noise set, so a long horizon ray through
// unresident cloud could otherwise run thousands of evaluations for one pixel.
// Past the budget the ray stops. Resident pages, including the approximation
// guard over them, are not counted, so a fully resident layer is unchanged.
const uint RECONSTRUCTION_BUDGET = 1024u;
uint reconstructions = 0u;

float ReconstructedCode(ivec3 coordinate) {
	if (reconstructions >= RECONSTRUCTION_BUDGET) return 0.0;
	++reconstructions;
	return float(WeatherDensityCode(effectiveField, coordinate)) / 255.0;
}

float DensityCode(ivec3 point) {
	point = clamp(point, ivec3(0), ivec3(effectiveField.Grid.xyz) - 1);
	return float(WeatherChannelCode(inputField.cells[WeatherCellIndex(uvec3(point), effectiveField.Grid.xyz)], WEATHER_CHANNEL_DENSITY)) / 255.0;
}

int FindPreviousPage(ivec3 coordinate, uint lod) {
	ivec4 key = ivec4(PageOrigin(coordinate, 8 << lod), int(lod));
	uint low = 0u;
	uint high = pass.Quality.z;
	while (low < high) {
		uint middle = low + (high - low) / 2u;
		if (PageKeyLess(previousPageTable.keys[middle], key)) low = middle + 1u;
		else high = middle;
	}
	return low < pass.Quality.z && all(equal(previousPageTable.keys[low], key)) ? int(low) : -1;
}

uint PreviousLodAt(vec3 canonical) {
	if (pass.Quality.z == 0u) return 0u;
	uint maximumLod = uint(previousPageTable.keys[pass.Quality.z - 1u].w) & 31u;
	ivec3 coordinate = ivec3(floor(canonical));
	for (uint lod = 0u; lod <= maximumLod; ++lod)
		if (FindPreviousPage(coordinate, lod) >= 0) return lod;
	return 0u;
}

int PageAt(vec3 canonical) {
	ivec3 coordinate = ivec3(floor(canonical));
	for (uint lod = 0u; lod <= effectiveField.Phase.w; ++lod) {
		int page = FindPage(coordinate, lod);
		if (page >= 0) return page;
	}
	return -1;
}

float SparseCode(ivec3 coordinate, uint lod) {
	int index = FindPage(coordinate, lod);
	if (index >= 0) {
		ivec4 page = pageTable.keys[index];
		uint descriptor = uint(page.w);
		uint kind = (descriptor >> 5u) & 3u;
		if (kind == 2u) return 0.0;
		uvec3 point = uvec3((coordinate - page.xyz) / (1 << (descriptor & 31u)));
		uint baseWord = descriptor >> 7u;
		if (kind == 0u) {
			uint cell = baseWord / 2u + point.x + 8u * (point.y + 8u * point.z);
			return float(WeatherChannelCode(inputField.cells[cell], WEATHER_CHANNEL_DENSITY)) / 255.0;
		}
		if (kind == 1u) {
			uvec2 address = WeatherAssemblyAddress(point);
			uvec2 scalarWords = inputField.cells[baseWord / 2u + address.x * 4u];
			if (WeatherAssemblyUnsafe(scalarWords))
				return float(WeatherDensityCode(effectiveField, coordinate)) / 255.0;
			sampledCompact = true;
			return float(WeatherAssemblyDensityCode(scalarWords, address.y)) / 255.0;
		}
	}
	// Border and missing pages reconstruct canonical samples, so allocation
	// refusal cannot make a hole nearby; only a ray past its reconstruction
	// budget stops short.
	return ReconstructedCode(coordinate);
}

bool SparseEmpty(vec3 canonical, int selected) {
	if (selected < 0) return false;
	uint descriptor = uint(pageTable.keys[selected].w);
	// The empty certificate covers every canonical sample in the page and its
	// interpolation halo for every time, independent of a sampled summary.
	if (((descriptor >> 5u) & 3u) == 2u) return true;
	uint lod = descriptor & 31u;
	// Coarse point samples cannot certify the occupancy between canonical cells.
	if (lod > 0u) return false;
	if (pass.Quality.z > 0u && uintBitsToFloat(pass.Quality.w) < 1.0) return false;
	int stride = 1 << lod;
	vec3 local = canonical - vec3(pageTable.keys[selected].xyz);
	vec3 edge = min(local, vec3(8 * stride) - local);
	if (min(min(edge.x, edge.y), edge.z) < 1.5 * float(stride)) return false;
	// The edge margin keeps all eight lod-0 trilinear corners in this same page.
	return inputSummary.values[selected].x == 0u;
}

float SparseDensityAtLod(vec3 canonical, uint lod) {
	int stride = 1 << lod;
	vec3 point = canonical / float(stride);
	ivec3 cell = ivec3(floor(point)) * stride;
	vec3 weight = fract(point);
	float low = mix(mix(SparseCode(cell, lod), SparseCode(cell + ivec3(stride, 0, 0), lod), weight.x),
		mix(SparseCode(cell + ivec3(0, stride, 0), lod), SparseCode(cell + ivec3(stride, stride, 0), lod), weight.x), weight.y);
	float high = mix(mix(SparseCode(cell + ivec3(0, 0, stride), lod), SparseCode(cell + ivec3(stride, 0, stride), lod), weight.x),
		mix(SparseCode(cell + ivec3(0, stride, stride), lod), SparseCode(cell + ivec3(stride), lod), weight.x), weight.y);
	return mix(low, high, weight.z);
}

float CanonicalCode(ivec3 coordinate) {
	return float(WeatherDensityCode(effectiveField, coordinate)) / 255.0;
}

float CanonicalDensityAtLod(vec3 canonical, uint lod) {
	int stride = 1 << lod;
	vec3 point = canonical / float(stride);
	ivec3 cell = ivec3(floor(point)) * stride;
	vec3 weight = fract(point);
	float low = mix(mix(CanonicalCode(cell), CanonicalCode(cell + ivec3(stride, 0, 0)), weight.x),
		mix(CanonicalCode(cell + ivec3(0, stride, 0)), CanonicalCode(cell + ivec3(stride, stride, 0)), weight.x), weight.y);
	float high = mix(mix(CanonicalCode(cell + ivec3(0, 0, stride)), CanonicalCode(cell + ivec3(stride, 0, stride)), weight.x),
		mix(CanonicalCode(cell + ivec3(0, stride, stride)), CanonicalCode(cell + ivec3(stride)), weight.x), weight.y);
	return mix(low, high, weight.z);
}

float SparseCurrentDensity(vec3 canonical, int selected, out bool approximated) {
	uint lod = selected >= 0 ? uint(pageTable.keys[selected].w) & 31u : 0u;
	// Compact means need the same optical error check as coarse samples.
	// Comparing them with their own decoded means would not bound that error.
	sampledCompact = false;
	float density = SparseDensityAtLod(canonical, lod);
	approximated = lod > 0u || sampledCompact;
	if (selected < 0) return density;
	ivec4 page = pageTable.keys[selected];
	float stride = float(1 << lod);
	vec3 local = canonical - vec3(page.xyz);
	vec3 edge = min(local, vec3(8.0 * stride) - local);
	float edgeDistance = min(min(edge.x, edge.y), edge.z) / stride;
	if (edgeDistance >= 1.5) return density;
	for (uint parentLod = lod + 1u; parentLod <= effectiveField.Phase.w; ++parentLod) {
		if (FindPage(ivec3(floor(canonical)), parentLod) < 0) continue;
		float parent = SparseDensityAtLod(canonical, parentLod);
		// Blend one density before integrating; overlap never doubles optical depth.
		approximated = true;
		return mix(parent, density, smoothstep(0.5, 1.5, edgeDistance));
	}
	return density;
}

float SparseDensity(vec3 canonical, int selected) {
	bool approximated;
	float density = SparseCurrentDensity(canonical, selected, approximated);
	float transition = clamp(uintBitsToFloat(pass.Quality.w), 0.0, 1.0);
	if (pass.Quality.z > 0u && transition < 1.0) {
		// Retain topology only. Reevaluate its samples at the accepted current
		// time so a transition never blends two different weather clocks.
		float previous = CanonicalDensityAtLod(canonical, PreviousLodAt(canonical));
		density = mix(previous, density, transition);
		approximated = true;
	}
	if (!approximated) return density;
	float fine = CanonicalDensityAtLod(canonical, 0u);
	// One guard bounds compact, spatial LOD and topology blending together.
	return abs(density - fine) <= allowedDensityError ? density : fine;
}

float DensityAt(vec3 world, int selected) {
	vec3 lattice = world / pass.Minimum.w;
	if (effectiveField.Origin.w == 0) return SparseDensity(lattice, selected);
	vec3 point = (lattice - vec3(effectiveField.Origin.xyz)) / float(effectiveField.Origin.w);
	ivec3 cell = ivec3(floor(point));
	vec3 weight = fract(point);
	float low = mix(mix(DensityCode(cell), DensityCode(cell + ivec3(1, 0, 0)), weight.x),
		mix(DensityCode(cell + ivec3(0, 1, 0)), DensityCode(cell + ivec3(1, 1, 0)), weight.x), weight.y);
	float high = mix(mix(DensityCode(cell + ivec3(0, 0, 1)), DensityCode(cell + ivec3(1, 0, 1)), weight.x),
		mix(DensityCode(cell + ivec3(0, 1, 1)), DensityCode(cell + ivec3(1, 1, 1)), weight.x), weight.y);
	return mix(low, high, weight.z);
}

bool EmptyNeighborhood(vec3 world, out int selected) {
	selected = -1;
	// Sparse borders may reconstruct missing neighbours; their old maxima cannot prove emptiness.
	if (effectiveField.Origin.w == 0) {
		vec3 canonical = world / pass.Minimum.w;
		selected = PageAt(canonical);
		return SparseEmpty(canonical, selected);
	}
	ivec3 cell = ivec3(floor((world / pass.Minimum.w - vec3(effectiveField.Origin.xyz)) / float(effectiveField.Origin.w)));
	ivec3 brick = clamp(cell / 8, ivec3(0), ivec3(effectiveField.Bricks.xyz) - 1);
	// Interpolation reaches neighboring bricks. Their maxima must also be zero before skipping.
	for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
		ivec3 point = clamp(brick + ivec3(x, y, z), ivec3(0), ivec3(effectiveField.Bricks.xyz) - 1);
		if (inputSummary.values[WeatherBrickIndex(uvec3(point), effectiveField.Bricks.xyz)].x != 0u) return false;
	}
	return true;
}

#ifdef WEATHER_BAKE
// The field's exact value at one canonical cell. A voxel sits on a cell, so
// trilinear weights are zero and one read replaces `DensityAt`'s eight: the
// cell of a dense resident page, zero in a certified empty page, otherwise
// the canonical code. Compact and coarse pages approximate, so they take the
// canonical code too, which is what `DensityAt` falls back to with no error
// allowed. Dense fields read their grid.
float BakedCellDensity(ivec3 cell) {
	if (effectiveField.Origin.w != 0) {
		vec3 world = vec3(cell) * pass.Minimum.w;
		int selected;
		return EmptyNeighborhood(world, selected) ? 0.0 : DensityAt(world, selected);
	}
	int page = FindPage(cell, 0u);
	if (page >= 0) {
		ivec4 key = pageTable.keys[page];
		uint descriptor = uint(key.w);
		uint kind = (descriptor >> 5u) & 3u;
		if (kind == 2u) return 0.0;
		if (kind == 0u) {
			uvec3 point = uvec3(cell - key.xyz);
			uint index = (descriptor >> 7u) / 2u + point.x + 8u * (point.y + 8u * point.z);
			return float(WeatherChannelCode(inputField.cells[index], WEATHER_CHANNEL_DENSITY)) / 255.0;
		}
	}
	return float(WeatherDensityCode(effectiveField, cell)) / 255.0;
}

// One thread per (x, z) column of the baked box, writing every voxel of it.
void main() {
	effectiveField = WeatherClockField(pass.Field);
	ivec3 voxels = ivec3(pass.BakeVoxels.xyz);
	ivec2 column = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(column, voxels.xz))) return;
	// Exact cell values: the LOD guard accepts no approximation error.
	allowedDensityError = 0.0;
	for (int y = 0; y < voxels.y; ++y) {
		ivec3 voxel = ivec3(column.x, y, column.y);
		float density = 0.0;
		if (effectiveField.Phase.z != 0xffffffffu) density = BakedCellDensity(ivec3(pass.BakeOrigin.xyz) + voxel);
		imageStore(bakeImage, voxel, vec4(density));
	}
}
#else
// Raw density from the baked box, or a negative value when `world` is
// outside it and the field must be read directly. Away from the box edge it
// is the cubic B-spline of the bake, so distant cloud edges do not show the
// cell grid as blocks; the outer voxels, whose B-spline taps would reach past
// the baked counts, and voxel clouds, which keep their blocks, use trilinear.
float BakedDensity(vec3 world, uint profile) {
	if (pass.BakeVoxels.w == 0.0) return -1.0;
	vec3 voxel = world / pass.BakeOrigin.w - pass.BakeOrigin.xyz;
	if (any(lessThan(voxel, vec3(0.0))) || any(greaterThan(voxel, pass.BakeVoxels.xyz - 1.0))) return -1.0;
	vec3 size = vec3(textureSize(bakedDensity, 0));
	if (profile != PROFILE_VOXEL && all(greaterThanEqual(voxel, vec3(1.5))) &&
		all(lessThanEqual(voxel, pass.BakeVoxels.xyz - 2.5)))
		return max(WeatherSmoothDensity(bakedDensity, voxel + 0.5, size), 0.0);
	return texture(bakedDensity, (voxel + 0.5) / size).r;
}

// Finished density from one clipmap level, and how much of it to use: one
// inside, falling to zero over the outer tenth of the level so the switch to
// the next does not show as a line. Zero weight outside. Voxel clouds keep
// their blocks, so they never read a level. Level 0's voxels are a quarter
// cell and read trilinear; the coarser levels' voxels reach a whole cell, where
// trilinear shows the voxel grid as blocks in distant cloud edges, so they
// read the cubic B-spline instead.
vec2 DetailSample(uint level, vec3 world) {
	vec4 origin = pass.DetailOrigin[level];
	vec4 counts = pass.DetailVoxels[level];
	if (counts.w == 0.0 || pass.ImageExtent.z == PROFILE_VOXEL) return vec2(0.0);
	vec3 voxel = world / origin.w - origin.xyz;
	vec3 fraction = voxel / counts.xyz;
	if (any(lessThan(fraction, vec3(0.0))) || any(greaterThan(fraction, vec3(1.0)))) return vec2(0.0);
	float density = level == 0u ? texture(detailDensity0, voxel / vec3(textureSize(detailDensity0, 0))).r
		: level == 1u			? WeatherSmoothDensity(detailDensity1, voxel, vec3(textureSize(detailDensity1, 0)))
								: WeatherSmoothDensity(detailDensity2, voxel, vec3(textureSize(detailDensity2, 0)));
	if (density < 0.0) return vec2(0.0);
	vec2 horizontal = min(fraction.xz, 1.0 - fraction.xz);
	return vec2(density, smoothstep(0.0, 0.1, min(horizontal.x, horizontal.y)));
}

// The coarse path: the baked box, or the field itself outside it, shaped by
// the preset and textured like the coarsest clipmap level (one voxel per
// cell), so clouds keep the same detail across the switch out of the last
// level. Zero where the field is empty.
float CoarseDensity(uint profile, vec3 world) {
	vec3 sampled = ProfileSample(profile, world, pass.Minimum.w);
	float raw = BakedDensity(sampled, profile);
	if (raw < 0.0) {
		int selected;
		if (EmptyNeighborhood(sampled, selected)) return 0.0;
		raw = DensityAt(sampled, selected);
	}
	if (raw <= 0.0) return 0.0;
	float height = ProfileHeight(world, pass.Minimum.y, pass.Maximum.y);
	float density = ProfileErode(profile, ProfileDensity(profile, raw, height), world, height, pass.Minimum.w);
	return ProfileDetail(profile, density, world, pass.Minimum.w, pass.Minimum.w);
}

void main() {
	effectiveField = WeatherClockField(pass.Field);
	// The `CloudComputeShader` preset; see weather-profile.glsl.
	uint profile = pass.ImageExtent.z;
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	// The budgeted corner of the image this pass marches; see WeatherRayExtent.
	ivec2 size = pass.ImageExtent.w != 0u
		? ivec2(pass.ImageExtent.w & 0xffffu, pass.ImageExtent.w >> 16u) : imageSize(outputImage);
	if (any(greaterThanEqual(pixel, size))) return;
	vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
	ivec2 fullExtent = all(greaterThan(pass.ImageExtent.xy, uvec2(0)))
		? ivec2(pass.ImageExtent.xy) : textureSize(depthImage, 0);
	ivec2 depthPixel = clamp(ivec2(uv * vec2(fullExtent)), ivec2(0), fullExtent - 1);
	float sceneDepth = texelFetch(depthImage, depthPixel, 0).r;
	vec3 surface = WorldAt(uv, sceneDepth);
	vec3 delta = surface - pass.Eye.xyz;
	float rayLength = length(delta);
	float maximum = sceneDepth >= 0.9999995 ? max(pass.Maximum.w, rayLength) : rayLength;
	vec4 metadata = vec4(sceneDepth >= .9999995 ? 1.0 : 0.0, 0.0, min(rayLength, 65504.0), 0.0);
	imageStore(outputImage, pixel, vec4(0.0, 0.0, 0.0, 1.0));
	imageStore(cloudDepthImage, pixel, metadata);
	// A stateful replay awaiting validation publishes a neutral initialized layer.
	if (effectiveField.Phase.z == 0xffffffffu) return;
	if (!(rayLength > 0.0) || !(maximum > 0.0)) return;
	vec3 ray = delta / rayLength;
	float enter;
	float exit;
	if (!Interval(pass.Eye.xyz, ray, maximum, enter, exit)) return;
	// An oblique capture starts at its arrival aperture, not its virtual eye.
	// Ordinary views use their near plane through this same inverse projection.
	enter = max(enter, max(0.0, dot(WorldAt(uv, 0.0) - pass.Eye.xyz, ray)));
	if (!(exit > enter)) return;
	allowedDensityError = max(0.0, uintBitsToFloat(pass.Quality.y)) /
		max(pass.ColourExtinction.w * (exit - enter), .000001);
	uint steps = clamp(pass.Quality.x, 1u, 256u);
	// Steps are spaced on a square curve, short beside the eye and long far
	// away. Even spacing over a ray that crosses kilometres of layer took
	// hundreds of metres per step, so a camera inside a cloud sampled the fog
	// around it at most once per pixel and the view read as clear until
	// temporal history filled it in. Each step still integrates its own
	// length, so the total extinction along the ray is unchanged.
	float span = exit - enter;
	float invSteps = 1.0 / float(steps);
	float transmittance = 1.0;
	vec3 scattered = vec3(0.0);
	float weightedDistance = 0.0;
	float opacityWeight = 0.0;
	float cosine = dot(ray, normalize(-pass.Sun.xyz));
	// Two lobes: the forward silver lining, and a weaker back lobe that keeps
	// a sunlit top bright when seen from above with the sun behind the viewer.
	float phase = mix(CloudPhase(0.6, cosine), CloudPhase(-0.3, cosine), 0.35);
	vec3 ambientLight = pass.ColourExtinction.rgb * pass.Ambient.rgb;
	vec3 sunLight = pass.ColourExtinction.rgb * pass.SunColour.rgb * phase;
	// Light scattered many times inside the cloud, which is most of what makes
	// a thick cloud white: roughly a diffuse reflector's share of the sun,
	// reaching deeper than the direct beam does.
	vec3 multipleLight = pass.ColourExtinction.rgb * pass.SunColour.rgb * 0.25;
	// View jitter only changes integration samples, never canonical weather cells.
	float jitter = (float(WeatherHash(uint(pixel.x) ^ WeatherHash(uint(pixel.y)) ^
		WeatherHash(effectiveField.Phase.x) ^ WeatherHash(effectiveField.Phase.y))) + .5) / 4294967296.0;
	for (uint index = 0u; index < steps; ++index) {
		float near = float(index) * invSteps;
		float far = float(index + 1u) * invSteps;
		float segmentStart = enter + span * near * near;
		float stepLength = span * (far * far - near * near);
		float distance = segmentStart + jitter * stepLength;
		vec3 world = pass.Eye.xyz + ray * distance;
		// The finest level that holds the sample, blended into the next one
		// out, and the coarse path past the last.
		vec2 levels[3];
		uint used = 0u;
		for (; used < 3u; ++used) {
			levels[used] = DetailSample(used, world);
			if (levels[used].y >= 1.0) break;
		}
		float density = used == 3u ? CoarseDensity(profile, world) : levels[used].x;
		for (uint level = min(used, 3u); level > 0u; --level)
			density = mix(density, levels[level - 1u].x, levels[level - 1u].y);
		if (!(density > 0.0)) continue;
		float height = ProfileHeight(world, pass.Minimum.y, pass.Maximum.y);
		float stepTransmittance = exp(-pass.ColourExtinction.w * density * stepLength);
		float contribution = transmittance * (1.0 - stepTransmittance);
		float sunShadow = WeatherShadowFactor(world);
		vec3 light = ambientLight + sunLight * sunShadow + multipleLight * sqrt(sqrt(sunShadow));
		scattered += contribution * light * ProfileLight(profile, height);
		weightedDistance += contribution * distance;
		opacityWeight += contribution;
		transmittance *= stepTransmittance;
		if (transmittance < 0.003 || reconstructions >= RECONSTRUCTION_BUDGET) break;
	}
	if (opacityWeight > .00001) {
		metadata.y = min(weightedDistance / opacityWeight, 65504.0);
		metadata.w = 1.0;
	}
	imageStore(outputImage, pixel, vec4(scattered, transmittance));
	imageStore(cloudDepthImage, pixel, metadata);
}
#endif

#endif
