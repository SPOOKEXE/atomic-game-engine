#ifndef WEATHER_COMMON_GLSL
#define WEATHER_COMMON_GLSL

struct WeatherField {
	uvec4 Grid;
	ivec4 Origin;
	uvec4 Identity;
	uvec4 Shape;
	uvec4 Moisture;
	ivec4 Dynamics;
	ivec4 Wind;
	uvec4 Time;
	uvec4 Phase;
	uvec4 Bricks;
	ivec4 Thermodynamics;
};

uint WeatherCellIndex(uvec3 point, uvec3 dimensions) {
	return point.x + dimensions.x * (point.y + dimensions.y * point.z);
}

uint WeatherBrickIndex(uvec3 point, uvec3 dimensions) {
	return point.x + dimensions.x * (point.y + dimensions.y * point.z);
}

uint WeatherHash(uint value) {
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}

uint WeatherSmooth(uint fraction) {
	return (fraction * fraction * (768u - 2u * fraction) + 32768u) >> 16u;
}

uint WeatherInterpolate(uint first, uint second, uint fraction) {
	return (first * (256u - fraction) + second * fraction + 128u) >> 8u;
}

uvec3 WeatherAxis(int coordinate, uint displacement, uint shift) {
	uint fraction = displacement & 255u;
	uint shifted = (uint(coordinate) - (displacement >> 8u) - (fraction != 0u ? 1u : 0u)) & 0x00ffffffu;
	uint remainder = fraction != 0u ? 256u - fraction : 0u;
	uint cellFraction = ((shifted & ((1u << shift) - 1u)) * 256u + remainder) >> shift;
	return uvec3(shifted >> shift, WeatherSmooth(cellFraction), 0x00ffffffu >> shift);
}

uint WeatherCorner(uint seed, uint purpose, uint epoch, uint x, uint y, uint z) {
	return WeatherHash(seed ^ purpose ^ WeatherHash(epoch) ^ (x * 0x9e3779b9u) ^ (y * 0x85ebca6bu) ^ (z * 0xc2b2ae35u)) >> 24u;
}

uint WeatherNoiseAt(uint seed, uint purpose, uint epoch, uvec3 x, uvec3 y, uvec3 z) {
	uint rows[4];
	for (uint index = 0u; index < 4u; ++index) {
		uint yCorner = (y.x + (index & 1u)) & y.z;
		uint zCorner = (z.x + (index >> 1u)) & z.z;
		rows[index] = WeatherInterpolate(WeatherCorner(seed, purpose, epoch, x.x, yCorner, zCorner),
			WeatherCorner(seed, purpose, epoch, (x.x + 1u) & x.z, yCorner, zCorner), x.y);
	}
	return WeatherInterpolate(WeatherInterpolate(rows[0], rows[1], y.y),
		WeatherInterpolate(rows[2], rows[3], y.y), z.y);
}

uint WeatherNoise(WeatherField field, ivec3 coordinate, uint purpose, uint shift) {
	uvec3 x = WeatherAxis(coordinate.x, field.Time.x, shift);
	uvec3 y = WeatherAxis(coordinate.y, field.Time.y, shift);
	uvec3 z = WeatherAxis(coordinate.z, field.Time.z, shift);
	uint seed = field.Identity.x ^ WeatherHash(field.Identity.y);
	uint epoch = (field.Phase.x >> 8u) | (field.Phase.y << 24u);
	uint phase = WeatherSmooth(field.Phase.x & 255u);
#ifdef WEATHER_BRANCHLESS_NOISE
	return WeatherInterpolate(WeatherNoiseAt(seed, purpose, epoch, x, y, z),
		WeatherNoiseAt(seed, purpose, epoch + 1u, x, y, z), phase);
#else
	// Uniform endpoint weights make the other epoch contribute exactly zero.
	uint first = WeatherNoiseAt(seed, purpose, epoch + uint(phase == 256u), x, y, z);
	if (phase == 0u || phase == 256u) return first;
	return WeatherInterpolate(first,
		WeatherNoiseAt(seed, purpose, epoch + 1u, x, y, z), phase);
#endif
}

// Modular subtraction first avoids signed overflow for extreme canonical coordinates.
uint WeatherEnvelope(WeatherField field, int y) {
	uint offset = uint(y) - field.Shape.x;
	if (y < int(field.Shape.x) || offset >= field.Shape.y) return 0u;
	uint fraction = (offset * 2u + 1u) * 255u / (field.Shape.y * 2u);
	return min(fraction * 2u, (255u - fraction) * 2u);
}

// Every operation agrees with Engine::weather's integer reference, including signed division.
uvec2 WeatherEvaluate(WeatherField field, ivec3 coordinate) {
	uint shift = min(field.Shape.z, 10u);
	int front = int(WeatherNoise(field, coordinate, 0x46524f4eu, min(shift + 2u, 12u))) - 127;
	int moisture = int(WeatherNoise(field, coordinate, 0x4d4f4953u, shift)) - 127;
	uint body = WeatherNoise(field, coordinate, 0x424f4459u, shift);
	uint detail = WeatherNoise(field, coordinate, 0x45524f53u, shift > 2u ? shift - 2u : 0u);
	int temperature = int(WeatherNoise(field, coordinate, 0x54454d50u, shift)) - 127;
	uint envelope = WeatherEnvelope(field, coordinate.y);
	int frontAmount = front * field.Dynamics.z / 255;
	uint vapour = uint(clamp(int(field.Moisture.x) + moisture / 4 + frontAmount / 4, 0, 255));
	uint shape = uint(clamp(int((body * 3u + detail) / 4u) + frontAmount / 3, 0, 255));
	uint threshold = 255u - min(field.Shape.w, 255u);
	uint covered = shape > threshold ? (shape - threshold) * 255u / max(field.Shape.w, 1u) : 0u;
	uint growth = 255u - uint(field.Dynamics.w) + uint(field.Dynamics.w) * body / 255u;
	int temperatureAnomaly = clamp((temperature - frontAmount / 2) * int(field.Moisture.y) / 127, -127, 127);
	int altitude = coordinate.y < int(field.Shape.x) ? 0 : int(min(uint(coordinate.y) - field.Shape.x, 65536u));
	int altitudeResponse = altitude * (field.Thermodynamics.y / 256) + altitude * (field.Thermodynamics.y % 256) / 256;
	int saturation = clamp(field.Thermodynamics.x - altitudeResponse + temperatureAnomaly * field.Thermodynamics.z / 256, 0, 255);
	uint excess = vapour > uint(saturation) ? vapour - uint(saturation) : 0u;
	uint condensate = covered * envelope / 255u * growth / 255u * excess / 255u;
	uint temperatureCode = uint(temperatureAnomaly + 127);
	uint pressureCode = uint(clamp(front * int(field.Moisture.z) / 127, -127, 127) + 127);
	uvec3 wind;
	for (uint axis = 0u; axis < 3u; ++axis) {
		int disturbance = int(WeatherNoise(field, coordinate, 0x57494e44u + axis, shift)) - 127;
		int shear = axis != 1u ? int(envelope) * field.Dynamics.x / 255 : 0;
		int lift = axis == 1u ? int(condensate) * field.Dynamics.y / 255 : 0;
		wind[axis] = uint(clamp(field.Wind[axis] + disturbance * int(field.Moisture.w) / 127 + shear + lift, -127, 127) + 127);
	}
	return uvec2(condensate | (temperatureCode << 8u) | (vapour << 16u) | (pressureCode << 24u),
		wind.x | (wind.y << 8u) | (wind.z << 16u) | (uint(field.Wind.w) << 24u));
}

#endif
