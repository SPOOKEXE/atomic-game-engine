#pragma once

// Offline CPU layout/conversion comparisons. None of these types is a renderer buffer ABI.

#include <engine/weather/Weather.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace engine::weather::comparison {

	using CellWords = std::array<uint32_t, 2>;
	using Fp8Cell = std::array<uint8_t, 8>;

	inline bool PackWords(std::span<const EncodedCell> input, std::span<CellWords> output) {
		if (input.size() != output.size()) return false;
		for (size_t cell = 0; cell < input.size(); cell++) {
			for (size_t word = 0; word < 2; word++) {
				uint32_t packed = 0;
				for (size_t byte = 0; byte < 4; byte++)
					packed |= uint32_t{input[cell].Codes[word * 4 + byte]} << (byte * 8);
				output[cell][word] = packed;
			}
		}
		return true;
	}

	inline bool UnpackWords(std::span<const CellWords> input, std::span<EncodedCell> output) {
		if (input.size() != output.size()) return false;
		for (size_t cell = 0; cell < input.size(); cell++) {
			for (size_t byte = 0; byte < 8; byte++)
				output[cell].Codes[byte] = static_cast<uint8_t>(input[cell][byte / 4] >> ((byte % 4) * 8));
		}
		return true;
	}

	inline bool PackPlanes(std::span<const EncodedCell> input, std::span<uint8_t> output) {
		if (input.size() > std::numeric_limits<size_t>::max() / 8 || output.size() != input.size() * 8)
			return false;
		for (size_t field = 0; field < 8; field++)
			for (size_t cell = 0; cell < input.size(); cell++)
				output[field * input.size() + cell] = input[cell].Codes[field];
		return true;
	}

	inline bool UnpackPlanes(std::span<const uint8_t> input, std::span<EncodedCell> output) {
		if (output.size() > std::numeric_limits<size_t>::max() / 8 || input.size() != output.size() * 8)
			return false;
		for (size_t field = 0; field < 8; field++)
			for (size_t cell = 0; cell < output.size(); cell++)
				output[cell].Codes[field] = input[field * output.size() + cell];
		return true;
	}

	constexpr bool SignedField(size_t field) {
		return field == 1 || (field >= 3 && field <= 6);
	}

	// Normalized E4M3 values expressed in units of 2^-9; only the finite [0,1] subset is used.
	constexpr uint32_t Fp8Magnitude(uint8_t code) {
		const uint32_t exponent = (code & 0x7fu) >> 3u;
		const uint32_t mantissa = code & 7u;
		return exponent == 0 ? mantissa : (8u + mantissa) << (exponent - 1u);
	}

	constexpr uint32_t RoundEven(uint32_t numerator, uint32_t denominator) {
		const uint32_t quotient = numerator / denominator;
		const uint32_t remainder = numerator % denominator;
		return quotient +
			   (remainder * 2 > denominator || (remainder * 2 == denominator && (quotient & 1u)) ? 1u : 0u);
	}

	constexpr uint8_t EncodeMagnitude(uint32_t magnitude, uint32_t scale) {
		uint32_t bestDistance = UINT32_MAX;
		uint8_t best = 0;
		for (uint32_t code = 0; code <= 0x38; code++) {
			const uint32_t first = magnitude * 512;
			const uint32_t second = Fp8Magnitude(static_cast<uint8_t>(code)) * scale;
			const uint32_t distance = first > second ? first - second : second - first;
			if (distance < bestDistance || (distance == bestDistance && !(code & 1u))) {
				bestDistance = distance;
				best = static_cast<uint8_t>(code);
			}
		}
		return best;
	}

	constexpr std::array<uint8_t, 256> EncodeTable(bool signedField) {
		std::array<uint8_t, 256> table{};
		for (uint32_t code = 0; code < 256; code++) {
			const int32_t value =
				signedField ? static_cast<int32_t>(std::min(code, 254u)) - 127 : static_cast<int32_t>(code);
			const uint32_t magnitude = static_cast<uint32_t>(value < 0 ? -value : value);
			table[code] = static_cast<uint8_t>(
				EncodeMagnitude(magnitude, signedField ? 127u : 255u) | (value < 0 ? 0x80u : 0u)
			);
		}
		return table;
	}

	constexpr std::array<uint8_t, 256> DecodeTable(bool signedField) {
		std::array<uint8_t, 256> table{};
		for (uint32_t code = 0; code < 256; code++) {
			if ((code & 0x7f) > 0x38) continue;
			const uint32_t magnitude =
				RoundEven(Fp8Magnitude(static_cast<uint8_t>(code)) * (signedField ? 127u : 255u), 512);
			const int32_t value =
				code & 0x80u ? -static_cast<int32_t>(magnitude) : static_cast<int32_t>(magnitude);
			table[code] = static_cast<uint8_t>(signedField ? value + 127 : magnitude);
		}
		return table;
	}

	inline constexpr auto UNSIGNED_ENCODE = EncodeTable(false);
	inline constexpr auto SIGNED_ENCODE = EncodeTable(true);
	inline constexpr auto UNSIGNED_DECODE = DecodeTable(false);
	inline constexpr auto SIGNED_DECODE = DecodeTable(true);

	inline bool PackFp8(std::span<const EncodedCell> input, std::span<Fp8Cell> output) {
		if (input.size() != output.size()) return false;
		for (const auto &cell : input)
			if (!Decode(cell)) return false;
		for (size_t cell = 0; cell < input.size(); cell++)
			for (size_t field = 0; field < 8; field++)
				output[cell][field] =
					(SignedField(field) ? SIGNED_ENCODE : UNSIGNED_ENCODE)[input[cell].Codes[field]];
		return true;
	}

	inline bool UnpackFp8(std::span<const Fp8Cell> input, std::span<EncodedCell> output) {
		if (input.size() != output.size()) return false;
		for (const auto &cell : input)
			for (size_t field = 0; field < 8; field++)
				if ((cell[field] & 0x7fu) > 0x38u || (!SignedField(field) && (cell[field] & 0x80u)))
					return false;
		for (size_t cell = 0; cell < input.size(); cell++)
			for (size_t field = 0; field < 8; field++)
				output[cell].Codes[field] =
					(SignedField(field) ? SIGNED_DECODE : UNSIGNED_DECODE)[input[cell][field]];
		return true;
	}
}
