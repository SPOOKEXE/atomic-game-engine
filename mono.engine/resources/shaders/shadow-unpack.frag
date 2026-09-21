#version 450

layout(set = 2, binding = 0, std430) readonly buffer PackedShadow {
	uint words[];
} packedShadow;

void main() {
	uint sampleIndex = uint(gl_FragCoord.y) * 2048u + uint(gl_FragCoord.x);
	uint block = sampleIndex >> 6u;
	uint base = packedShadow.words[block * 2u];
	uint descriptor = packedShadow.words[block * 2u + 1u];
	uint width = descriptor >> 26u;
	uint delta = 0u;
	if (width != 0u) {
		uint bitOffset = (sampleIndex & 63u) * width;
		uint wordOffset = (descriptor & 0x03ffffffu) + (bitOffset >> 5u);
		uint shift = bitOffset & 31u;
		delta = packedShadow.words[wordOffset] >> shift;
		// The final sample has no sentinel word after it.
		if (shift + width > 32u)
			delta |= packedShadow.words[wordOffset + 1u] << (32u - shift);
		if (width < 32u) delta &= (1u << width) - 1u;
	}
	gl_FragDepth = uintBitsToFloat(base + delta);
}
