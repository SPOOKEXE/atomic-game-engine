#pragma once

// Deterministic compact representations for editable values. These functions
// only describe a presentation copy; editable components retain float/byte
// authored values and collision always reads those values.
// arch-waiver public-header: forward API. EditableMesh and EditableImage expose
// this policy in their public component records.
// @tier L7 · shared

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace engine::scene {

	enum class EditablePackingFormat : uint8_t {
		Float32,
		Float16,
		Float8E4M3FN,
		Signed16,
		Unsigned16,
		Signed8,
		Unsigned8,
		Signed4,
		Unsigned4,
		Boolean,
	};

	enum class EditablePackingAttribute : uint8_t {
		Position = 1 << 0,
		Normal = 1 << 1,
		UV = 1 << 2,
		Colour = 1 << 3,
		Alpha = 1 << 4,
	};

	constexpr uint8_t operator|(EditablePackingAttribute left, EditablePackingAttribute right) {
		return static_cast<uint8_t>(left) | static_cast<uint8_t>(right);
	}

	// A format is meaningful only for the selected semantic attributes. Integer
	// formats linearly map [Minimum, Maximum] to their complete code range;
	// nearest ties choose the even code, and values outside the range saturate.
	struct EditablePacking {
		uint8_t Attributes = 0;
		EditablePackingFormat Format = EditablePackingFormat::Float32;
		float Minimum = 0.0f;
		float Maximum = 1.0f;
		uint32_t Revision = 0;

		bool Enabled() const {
			return Attributes != 0 && Format != EditablePackingFormat::Float32;
		}
	};

	inline constexpr std::string_view EditablePackingFormatName(EditablePackingFormat format) {
		switch (format) {
		case EditablePackingFormat::Float32:
			return "float32";
		case EditablePackingFormat::Float16:
			return "float16";
		case EditablePackingFormat::Float8E4M3FN:
			return "float8-e4m3fn";
		case EditablePackingFormat::Signed16:
			return "int16";
		case EditablePackingFormat::Unsigned16:
			return "uint16";
		case EditablePackingFormat::Signed8:
			return "int8";
		case EditablePackingFormat::Unsigned8:
			return "uint8";
		case EditablePackingFormat::Signed4:
			return "int4";
		case EditablePackingFormat::Unsigned4:
			return "uint4";
		case EditablePackingFormat::Boolean:
			return "bool";
		}
		return {};
	}

	inline bool ParseEditablePackingFormat(std::string_view text, EditablePackingFormat &format) {
		for (const EditablePackingFormat candidate :
			 {EditablePackingFormat::Float32,
			  EditablePackingFormat::Float16,
			  EditablePackingFormat::Float8E4M3FN,
			  EditablePackingFormat::Signed16,
			  EditablePackingFormat::Unsigned16,
			  EditablePackingFormat::Signed8,
			  EditablePackingFormat::Unsigned8,
			  EditablePackingFormat::Signed4,
			  EditablePackingFormat::Unsigned4,
			  EditablePackingFormat::Boolean}) {
			if (EditablePackingFormatName(candidate) == text) {
				format = candidate;
				return true;
			}
		}
		return false;
	}

	inline size_t EditablePackedByteCount(EditablePackingFormat format, size_t values) {
		switch (format) {
		case EditablePackingFormat::Float32:
			return values * 4;
		case EditablePackingFormat::Float16:
		case EditablePackingFormat::Signed16:
		case EditablePackingFormat::Unsigned16:
			return values * 2;
		case EditablePackingFormat::Float8E4M3FN:
		case EditablePackingFormat::Signed8:
		case EditablePackingFormat::Unsigned8:
			return values;
		case EditablePackingFormat::Signed4:
		case EditablePackingFormat::Unsigned4:
			return (values + 1) / 2;
		case EditablePackingFormat::Boolean:
			return (values + 7) / 8;
		}
		return 0;
	}

	namespace detail {
		inline uint32_t RoundEven(float value) {
			const float low = std::floor(value);
			const float fraction = value - low;
			if (fraction < 0.5f) return static_cast<uint32_t>(low);
			if (fraction > 0.5f) return static_cast<uint32_t>(low + 1.0f);
			return static_cast<uint32_t>(low) + (static_cast<uint32_t>(low) & 1u);
		}

		inline uint16_t FloatToHalf(float value) {
			const uint32_t bits = std::bit_cast<uint32_t>(value);
			const uint32_t sign = (bits >> 16) & 0x8000u;
			int exponent = static_cast<int>((bits >> 23) & 0xffu) - 127 + 15;
			uint32_t mantissa = bits & 0x7fffffu;
			if (!std::isfinite(value)) return static_cast<uint16_t>(sign | 0x7bffu);
			if (exponent <= 0) {
				if (exponent < -10) return static_cast<uint16_t>(sign);
				mantissa = (mantissa | 0x800000u) >> (1 - exponent);
				return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13));
			}
			if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7bffu);
			return static_cast<uint16_t>(
				sign | (static_cast<uint32_t>(exponent) << 10) | ((mantissa + 0x1000u) >> 13)
			);
		}

		inline float HalfToFloat(uint16_t value) {
			const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
			uint32_t exponent = (value >> 10) & 0x1fu;
			uint32_t mantissa = value & 0x3ffu;
			if (exponent == 0) {
				if (mantissa == 0) return std::bit_cast<float>(sign);
				while ((mantissa & 0x400u) == 0) {
					mantissa <<= 1;
					exponent--;
				}
				mantissa &= 0x3ffu;
				exponent++;
			}
			const uint32_t bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
			return std::bit_cast<float>(bits);
		}

		inline float Float8ToFloat(uint8_t value) {
			const float sign = (value & 0x80u) == 0 ? 1.0f : -1.0f;
			const uint8_t exponent = (value >> 3) & 0x0fu;
			const uint8_t mantissa = value & 0x07u;
			if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -9);
			if (exponent == 15 && mantissa == 7) return sign * 448.0f;
			return sign *
				   std::ldexp(1.0f + static_cast<float>(mantissa) / 8.0f, static_cast<int>(exponent) - 7);
		}

		inline uint8_t FloatToFloat8(float value) {
			if (!std::isfinite(value)) value = std::copysign(448.0f, value);
			uint8_t best = 0;
			float error = std::numeric_limits<float>::infinity();
			for (uint16_t candidate = 0; candidate < 256; candidate++) {
				const float decoded = Float8ToFloat(static_cast<uint8_t>(candidate));
				const float candidateError = std::abs(decoded - value);
				if (candidateError < error || (candidateError == error && candidate < best)) {
					best = static_cast<uint8_t>(candidate);
					error = candidateError;
				}
			}
			return best;
		}
	}

	inline bool PackEditableValues(
		std::span<const float> values, const EditablePacking &policy, std::vector<std::byte> &out
	) {
		if (!std::isfinite(policy.Minimum) || !std::isfinite(policy.Maximum) ||
			policy.Maximum < policy.Minimum)
			return false;
		out.assign(EditablePackedByteCount(policy.Format, values.size()), std::byte{0});
		const auto integer = [&](float value, uint32_t maximum) {
			if (!std::isfinite(value)) return uint32_t{0};
			const float extent = policy.Maximum - policy.Minimum;
			if (extent == 0.0f) return uint32_t{0};
			return std::min(
				maximum,
				detail::RoundEven(std::clamp((value - policy.Minimum) / extent, 0.0f, 1.0f) * maximum)
			);
		};
		const auto signedInteger = [&](float value, int32_t minimum, int32_t maximum) {
			if (!std::isfinite(value) || policy.Maximum == policy.Minimum) return minimum;
			const float normalized =
				std::clamp((value - policy.Minimum) / (policy.Maximum - policy.Minimum), 0.0f, 1.0f);
			return minimum + static_cast<int32_t>(
								 detail::RoundEven(normalized * static_cast<float>(maximum - minimum))
							 );
		};
		for (size_t index = 0; index < values.size(); index++) {
			const float value = values[index];
			switch (policy.Format) {
			case EditablePackingFormat::Float32: {
				const uint32_t raw = std::bit_cast<uint32_t>(value);
				for (size_t byte = 0; byte < 4; byte++)
					out[index * 4 + byte] = std::byte(raw >> (byte * 8));
				break;
			}
			case EditablePackingFormat::Float16: {
				const uint16_t raw = detail::FloatToHalf(value);
				out[index * 2] = std::byte(raw);
				out[index * 2 + 1] = std::byte(raw >> 8);
				break;
			}
			case EditablePackingFormat::Float8E4M3FN:
				out[index] = std::byte(detail::FloatToFloat8(value));
				break;
			case EditablePackingFormat::Signed16: {
				const uint16_t raw = static_cast<uint16_t>(signedInteger(value, -32768, 32767));
				out[index * 2] = std::byte(raw);
				out[index * 2 + 1] = std::byte(raw >> 8);
				break;
			}
			case EditablePackingFormat::Unsigned16: {
				const uint16_t raw = static_cast<uint16_t>(integer(value, 65535));
				out[index * 2] = std::byte(raw);
				out[index * 2 + 1] = std::byte(raw >> 8);
				break;
			}
			case EditablePackingFormat::Signed8:
				out[index] = std::byte(static_cast<uint8_t>(signedInteger(value, -128, 127)));
				break;
			case EditablePackingFormat::Unsigned8:
				out[index] = std::byte(integer(value, 255));
				break;
			case EditablePackingFormat::Signed4: {
				const uint8_t raw = static_cast<uint8_t>(signedInteger(value, -8, 7)) & 15u;
				out[index / 2] |= std::byte(raw << ((index & 1) * 4));
				break;
			}
			case EditablePackingFormat::Unsigned4: {
				const uint8_t raw = static_cast<uint8_t>(integer(value, 15));
				out[index / 2] |= std::byte(raw << ((index & 1) * 4));
				break;
			}
			case EditablePackingFormat::Boolean:
				if (value >= 0.5f) out[index / 8] |= std::byte(1u << (index & 7));
				break;
			}
		}
		return true;
	}

	inline bool UnpackEditableValues(
		std::span<const std::byte> packed,
		size_t count,
		const EditablePacking &policy,
		std::vector<float> &out
	) {
		if (packed.size() != EditablePackedByteCount(policy.Format, count) ||
			!std::isfinite(policy.Minimum) || !std::isfinite(policy.Maximum) ||
			policy.Maximum < policy.Minimum)
			return false;
		out.resize(count);
		const auto normalized = [&](uint32_t code, uint32_t maximum) {
			return policy.Minimum + (policy.Maximum - policy.Minimum) * static_cast<float>(code) / maximum;
		};
		for (size_t index = 0; index < count; index++) {
			switch (policy.Format) {
			case EditablePackingFormat::Float32: {
				uint32_t raw = 0;
				for (size_t byte = 0; byte < 4; byte++)
					raw |= std::to_integer<uint8_t>(packed[index * 4 + byte]) << (byte * 8);
				out[index] = std::bit_cast<float>(raw);
				break;
			}
			case EditablePackingFormat::Float16: {
				const uint16_t raw = static_cast<uint16_t>(std::to_integer<uint8_t>(packed[index * 2])) |
									 static_cast<uint16_t>(std::to_integer<uint8_t>(packed[index * 2 + 1]))
										 << 8;
				out[index] = detail::HalfToFloat(raw);
				break;
			}
			case EditablePackingFormat::Float8E4M3FN:
				out[index] = detail::Float8ToFloat(std::to_integer<uint8_t>(packed[index]));
				break;
			case EditablePackingFormat::Signed16: {
				const uint16_t raw = static_cast<uint16_t>(std::to_integer<uint8_t>(packed[index * 2])) |
									 static_cast<uint16_t>(std::to_integer<uint8_t>(packed[index * 2 + 1]))
										 << 8;
				out[index] = normalized(static_cast<uint32_t>(static_cast<int16_t>(raw) + 32768), 65535);
				break;
			}
			case EditablePackingFormat::Unsigned16: {
				const uint16_t raw = static_cast<uint16_t>(std::to_integer<uint8_t>(packed[index * 2])) |
									 static_cast<uint16_t>(std::to_integer<uint8_t>(packed[index * 2 + 1]))
										 << 8;
				out[index] = normalized(raw, 65535);
				break;
			}
			case EditablePackingFormat::Signed8:
				out[index] = normalized(
					static_cast<uint32_t>(static_cast<int8_t>(std::to_integer<uint8_t>(packed[index])) + 128),
					255
				);
				break;
			case EditablePackingFormat::Unsigned8:
				out[index] = normalized(std::to_integer<uint8_t>(packed[index]), 255);
				break;
			case EditablePackingFormat::Signed4: {
				const uint8_t nibble =
					(std::to_integer<uint8_t>(packed[index / 2]) >> ((index & 1) * 4)) & 15u;
				const int8_t signedValue =
					(nibble & 8u) ? static_cast<int8_t>(nibble | 0xf0u) : static_cast<int8_t>(nibble);
				out[index] = normalized(static_cast<uint32_t>(signedValue + 8), 15);
				break;
			}
			case EditablePackingFormat::Unsigned4:
				out[index] =
					normalized((std::to_integer<uint8_t>(packed[index / 2]) >> ((index & 1) * 4)) & 15u, 15);
				break;
			case EditablePackingFormat::Boolean:
				out[index] = (std::to_integer<uint8_t>(packed[index / 8]) >> (index & 7)) & 1u;
				break;
			}
		}
		return true;
	}
}
