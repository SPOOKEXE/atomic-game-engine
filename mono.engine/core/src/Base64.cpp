#include <engine/core/Base64.hpp>

#include <array>
#include <cstdint>

namespace engine::core {
	namespace {
		constexpr std::string_view ALPHABET =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		// Each alphabet character's value, or -1.
		constexpr std::array<int8_t, 256> Values() {
			std::array<int8_t, 256> values{};
			for (int8_t &value : values)
				value = -1;
			for (size_t index = 0; index < ALPHABET.size(); ++index)
				values[static_cast<uint8_t>(ALPHABET[index])] = static_cast<int8_t>(index);
			return values;
		}
		constexpr std::array<int8_t, 256> VALUES = Values();
	}

	std::string EncodeBase64(std::span<const std::byte> bytes) {
		std::string out;
		out.reserve((bytes.size() + 2) / 3 * 4);
		for (size_t at = 0; at < bytes.size(); at += 3) {
			const size_t left = bytes.size() - at;
			const uint32_t a = std::to_integer<uint8_t>(bytes[at]);
			const uint32_t b = left > 1 ? std::to_integer<uint8_t>(bytes[at + 1]) : 0u;
			const uint32_t c = left > 2 ? std::to_integer<uint8_t>(bytes[at + 2]) : 0u;
			const uint32_t triple = (a << 16) | (b << 8) | c;
			out += ALPHABET[(triple >> 18) & 63];
			out += ALPHABET[(triple >> 12) & 63];
			out += left > 1 ? ALPHABET[(triple >> 6) & 63] : '=';
			out += left > 2 ? ALPHABET[triple & 63] : '=';
		}
		return out;
	}

	bool DecodeBase64(std::string_view text, std::vector<std::byte> &out) {
		if (text.size() % 4 != 0) return false;
		out.clear();
		out.reserve(text.size() / 4 * 3);
		for (size_t at = 0; at < text.size(); at += 4) {
			uint32_t triple = 0;
			size_t padding = 0;
			for (size_t index = 0; index < 4; ++index) {
				const char character = text[at + index];
				if (character == '=') {
					// Padding only at the end, and never more than two.
					if (at + 4 != text.size() || index < 2) return false;
					++padding;
					triple <<= 6;
					continue;
				}
				if (padding != 0) return false;
				const int8_t value = VALUES[static_cast<uint8_t>(character)];
				if (value < 0) return false;
				triple = (triple << 6) | static_cast<uint32_t>(value);
			}
			out.push_back(static_cast<std::byte>((triple >> 16) & 255));
			if (padding < 2) out.push_back(static_cast<std::byte>((triple >> 8) & 255));
			if (padding < 1) out.push_back(static_cast<std::byte>(triple & 255));
		}
		return true;
	}
}
