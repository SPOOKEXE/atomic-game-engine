#ifndef WEATHER_FIELD_GLSL
#define WEATHER_FIELD_GLSL

// Read access to a resident weather field, shared by every pass that consumes
// one. Before including this, a shader declares the field's storage and names
// it with two macros:
//
//   WEATHER_FIELD_CELLS  the packed `uvec2` cell array
//   WEATHER_FIELD_PAGES  the sorted `ivec4` page key array
//
// and declares a global `WeatherField effectiveField`, set from the clock.

// The packed bytes of one cell. Signed channels are biased by 127.
const uint WEATHER_CHANNEL_DENSITY = 0u;
const uint WEATHER_CHANNEL_TEMPERATURE = 1u;
const uint WEATHER_CHANNEL_VAPOUR = 2u;
const uint WEATHER_CHANNEL_PRESSURE = 3u;
const uint WEATHER_CHANNEL_WIND_X = 4u;
const uint WEATHER_CHANNEL_WIND_Y = 5u;
const uint WEATHER_CHANNEL_WIND_Z = 6u;
const uint WEATHER_CHANNEL_ICE = 7u;

// One channel's byte from a packed cell.
uint WeatherChannelCode(uvec2 cell, uint channel) {
	return ((channel < 4u ? cell.x : cell.y) >> (8u * (channel & 3u))) & 255u;
}

// Page keys sort by LOD, then x, y and z.
bool PageKeyLess(ivec4 first, ivec4 second) {
	uint firstLod = uint(first.w) & 31u;
	uint secondLod = uint(second.w) & 31u;
	if (firstLod != secondLod) return firstLod < secondLod;
	if (first.x != second.x) return first.x < second.x;
	if (first.y != second.y) return first.y < second.y;
	return first.z < second.z;
}

ivec3 PageOrigin(ivec3 coordinate, int extent) {
	// Unsigned masking floors negative coordinates to the power-of-two page boundary.
	return ivec3(uvec3(coordinate) & uvec3(~(uint(extent) - 1u)));
}

// The resident page holding a canonical cell at one LOD, or -1.
int FindPage(ivec3 coordinate, uint lod) {
	ivec4 key = ivec4(PageOrigin(coordinate, 8 << lod), int(lod));
	uint low = 0u;
	uint high = effectiveField.Bricks.w;
	while (low < high) {
		uint middle = low + (high - low) / 2u;
		if (PageKeyLess(WEATHER_FIELD_PAGES[middle], key)) low = middle + 1u;
		else high = middle;
	}
	return low < effectiveField.Bricks.w && all(equal(WEATHER_FIELD_PAGES[low].xyz, key.xyz)) &&
		(uint(WEATHER_FIELD_PAGES[low].w) & 31u) == lod ? int(low) : -1;
}

#endif
