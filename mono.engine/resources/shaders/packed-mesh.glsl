// Byte-addressed editable mesh streams. Each stream starts on a word boundary;
// values inside it stay tightly packed, including four-bit and one-bit tails.
layout(set = 0, binding = 4, std430) readonly buffer PackedMeshBytes {
	uint packedMeshWords[];
};

layout(set = 1, binding = 1) uniform PackedMeshDescriptor {
	uvec4 positionStream;
	uvec4 normalStream;
	uvec4 uvStream;
	vec4 positionRange;
	vec4 normalRange;
	vec4 uvRange;
} packedMesh;

uint PackedByte(uint byteOffset) {
	uint word = packedMeshWords[byteOffset >> 2u];
	return (word >> ((byteOffset & 3u) * 8u)) & 255u;
}

uint PackedBits(uint byteOffset, uint bitOffset, uint width) {
	uint at = byteOffset + (bitOffset >> 3u);
	uint shift = bitOffset & 7u;
	uint bits = PackedByte(at);
	if (shift + width > 8u) bits |= PackedByte(at + 1u) << 8u;
	return (bits >> shift) & ((1u << width) - 1u);
}

float PackedFloat8(uint bits) {
	float sign = (bits & 128u) == 0u ? 1.0 : -1.0;
	uint exponent = (bits >> 3u) & 15u;
	uint mantissa = bits & 7u;
	if (exponent == 0u) return sign * float(mantissa) * exp2(-9.0);
	if (exponent == 15u && mantissa == 7u) return sign * 448.0;
	return sign * (1.0 + float(mantissa) / 8.0) * exp2(float(int(exponent) - 7));
}

float PackedNormalized(uint code, uint maximum, vec2 range) {
	return range.x + (range.y - range.x) * float(code) / float(maximum);
}

float PackedValue(uvec4 stream, vec2 range, uint index) {
	uint format = stream.z;
	uint byteOffset = stream.x;
	if (format == 0u) {
		uint at = byteOffset + index * 4u;
		uint bits = PackedByte(at) | (PackedByte(at + 1u) << 8u) |
			(PackedByte(at + 2u) << 16u) | (PackedByte(at + 3u) << 24u);
		return uintBitsToFloat(bits);
	}
	if (format == 1u) {
		uint at = byteOffset + index * 2u;
		uint bits = PackedByte(at) | (PackedByte(at + 1u) << 8u);
		return unpackHalf2x16(bits).x;
	}
	if (format == 2u) return PackedFloat8(PackedByte(byteOffset + index));
	if (format == 3u) {
		uint at = byteOffset + index * 2u;
		uint bits = PackedByte(at) | (PackedByte(at + 1u) << 8u);
		return PackedNormalized((bits + 32768u) & 65535u, 65535u, range);
	}
	if (format == 4u) {
		uint at = byteOffset + index * 2u;
		return PackedNormalized(PackedByte(at) | (PackedByte(at + 1u) << 8u), 65535u, range);
	}
	if (format == 5u)
		return PackedNormalized((PackedByte(byteOffset + index) + 128u) & 255u, 255u, range);
	if (format == 6u)
		return PackedNormalized(PackedByte(byteOffset + index), 255u, range);
	if (format == 7u)
		return PackedNormalized((PackedBits(byteOffset, index * 4u, 4u) + 8u) & 15u, 15u, range);
	if (format == 8u)
		return PackedNormalized(PackedBits(byteOffset, index * 4u, 4u), 15u, range);
	return float(PackedBits(byteOffset, index, 1u));
}

vec3 PackedPosition(uint vertex) {
	uint first = vertex * packedMesh.positionStream.w;
	return vec3(
		PackedValue(packedMesh.positionStream, packedMesh.positionRange.xy, first),
		PackedValue(packedMesh.positionStream, packedMesh.positionRange.xy, first + 1u),
		PackedValue(packedMesh.positionStream, packedMesh.positionRange.xy, first + 2u)
	);
}

vec3 PackedNormal(uint vertex) {
	uint first = vertex * packedMesh.normalStream.w;
	return vec3(
		PackedValue(packedMesh.normalStream, packedMesh.normalRange.xy, first),
		PackedValue(packedMesh.normalStream, packedMesh.normalRange.xy, first + 1u),
		PackedValue(packedMesh.normalStream, packedMesh.normalRange.xy, first + 2u)
	);
}

vec2 PackedUV(uint vertex) {
	uint first = vertex * packedMesh.uvStream.w;
	return vec2(
		PackedValue(packedMesh.uvStream, packedMesh.uvRange.xy, first),
		PackedValue(packedMesh.uvStream, packedMesh.uvRange.xy, first + 1u)
	);
}
