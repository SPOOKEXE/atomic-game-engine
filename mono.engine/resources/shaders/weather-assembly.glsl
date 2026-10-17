#ifndef WEATHER_ASSEMBLY_GLSL
#define WEATHER_ASSEMBLY_GLSL

// Private 32-byte assembly ABI matching render/WeatherAssembly.hpp.
// Scalar sums occupy bits0..54; bit55 requests canonical fallback for a newly sharp group.
// Remaining six words retain all24 original child wind bytes.

uint WeatherAssemblyScalar(uvec2 cell, uint purpose) {
	return purpose < 4u ? (cell.x >> (purpose * 8u)) & 255u : cell.y >> 24u;
}

vec3 WeatherAssemblyWind(uvec2 cell) {
	ivec3 codes = ivec3(cell.y & 255u, (cell.y >> 8u) & 255u, (cell.y >> 16u) & 255u) - ivec3(127);
	// These are the canonical codec's physical wind scales, not one packed FP24 number.
	return vec3(codes) * vec3(64.0, 16.0, 64.0) / 127.0;
}

bool WeatherAssemblyCanMerge(uvec2 children[8]) {
	for (uint purpose = 0u; purpose < 5u; ++purpose) {
		uint sum = 0u;
		uint squares = 0u;
		uint minimum = 255u;
		uint maximum = 0u;
		for (uint child = 0u; child < 8u; ++child) {
			uint scalar = WeatherAssemblyScalar(children[child], purpose);
			sum += scalar;
			squares += scalar * scalar;
			minimum = min(minimum, scalar);
			maximum = max(maximum, scalar);
		}
		// Biased signed codes have exactly the same variance and contrast as signed anomalies.
		if (8u * squares - sum * sum > 256u || maximum - minimum > 8u) return false;
	}
	vec3 winds[8];
	float speeds[8];
	for (uint child = 0u; child < 8u; ++child) {
		winds[child] = WeatherAssemblyWind(children[child]);
		speeds[child] = length(winds[child]);
	}
	for (uint first = 0u; first < 8u; ++first) for (uint second = first + 1u; second < 8u; ++second) {
		if (length(winds[first] - winds[second]) > 2.0 || abs(speeds[first] - speeds[second]) > 1.0) return false;
		bool firstZero = speeds[first] <= 0.1;
		bool secondZero = speeds[second] <= 0.1;
		if (firstZero != secondZero) return false;
		if (firstZero) continue;
		vec3 firstDirection = winds[first] / speeds[first];
		vec3 secondDirection = winds[second] / speeds[second];
		if (atan(length(cross(firstDirection, secondDirection)), dot(firstDirection, secondDirection)) > 0.15)
			return false;
	}
	return true;
}

void WeatherAssemblyPack(uvec2 children[8], out uint packedWords[8]) {
	for (uint word = 0u; word < 8u; ++word) packedWords[word] = 0u;
	for (uint purpose = 0u; purpose < 5u; ++purpose) {
		uint sum = 0u;
		for (uint child = 0u; child < 8u; ++child) sum += WeatherAssemblyScalar(children[child], purpose);
		uint offset = purpose * 11u;
		uint word = offset / 32u;
		uint shift = offset % 32u;
		packedWords[word] |= sum << shift;
		if (shift > 21u) packedWords[word + 1u] |= sum >> (32u - shift);
	}
	for (uint child = 0u; child < 8u; ++child) for (uint axis = 0u; axis < 3u; ++axis) {
		uint byteIndex = child * 3u + axis;
		packedWords[2u + byteIndex / 4u] |= ((children[child].y >> (axis * 8u)) & 255u) << ((byteIndex % 4u) * 8u);
	}
	// Classification may age while weather moves. Never use a lossy mean for a newly divergent front.
	if (!WeatherAssemblyCanMerge(children)) packedWords[1] |= 1u << 23u;
}

// Density and the unsafe marker occupy the first aligned word pair.
bool WeatherAssemblyUnsafe(uvec2 scalarWords) {
	return (scalarWords.y & (1u << 23u)) != 0u;
}

uint WeatherAssemblyDensityCode(uvec2 scalarWords, uint child) {
	uint sum = scalarWords.x & 2047u;
	return (sum >> 3u) + uint(child < (sum & 7u));
}

bool WeatherAssemblyUnsafe(uint packedWords[8]) {
	return WeatherAssemblyUnsafe(uvec2(packedWords[0], packedWords[1]));
}

uvec2 WeatherAssemblyDecode(uint packedWords[8], uint child) {
	uvec2 cell = uvec2(0u);
	for (uint purpose = 0u; purpose < 5u; ++purpose) {
		uint offset = purpose * 11u;
		uint word = offset / 32u;
		uint shift = offset % 32u;
		uint sum = packedWords[word] >> shift;
		if (shift > 21u) sum |= packedWords[word + 1u] << (32u - shift);
		sum &= 2047u;
		uint scalar = sum / 8u + (child < sum % 8u ? 1u : 0u);
		if (purpose < 4u) cell.x |= scalar << (purpose * 8u);
		else cell.y |= scalar << 24u;
	}
	for (uint axis = 0u; axis < 3u; ++axis) {
		uint byteIndex = child * 3u + axis;
		uint wind = (packedWords[2u + byteIndex / 4u] >> ((byteIndex % 4u) * 8u)) & 255u;
		cell.y |= wind << (axis * 8u);
	}
	return cell;
}

uvec2 WeatherAssemblyAddress(uvec3 point) {
	return uvec2((point.x >> 1u) + 4u * ((point.y >> 1u) + 4u * (point.z >> 1u)),
		(point.x & 1u) | ((point.y & 1u) << 1u) | ((point.z & 1u) << 2u));
}

#endif
