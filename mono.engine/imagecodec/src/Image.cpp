#include "Decoders.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <limits>

namespace engine::imagecodec {
	bool ValidateExtent(uint32_t width, uint32_t height, const Limits &limits, std::string &failure) {
		if (width == 0 || height == 0 || width > limits.MaximumWidth || height > limits.MaximumHeight ||
			static_cast<uint64_t>(width) * height > limits.MaximumPixelBytes / 4) {
			failure = "image: dimensions or pixel bytes exceed the limit";
			return false;
		}
		return true;
	}

	bool Decode(
		std::span<const std::byte> bytes,
		Format format,
		Image &out,
		std::string &failure,
		const Limits &limits
	) {
		ENGINE_PROFILE("imagecodec decode");
		if (bytes.size() > limits.MaximumEncodedBytes) {
			failure = "image: encoded bytes exceed the limit";
			return false;
		}
		bool decoded = false;
		switch (format) {
		case Format::Png:
			decoded = DecodePng(bytes, out, failure, limits);
			break;
		case Format::Jpeg:
			decoded = DecodeJpeg(bytes, out, failure, limits);
			break;
		default:
			failure = "image: unknown format";
			return false;
		}
		if (decoded) {
			core::Metrics::Count("imagecodec.decode.operations", 1);
			core::Metrics::Count("imagecodec.decode.input_bytes", static_cast<double>(bytes.size()));
			core::Metrics::Count("imagecodec.decode.pixel_bytes", static_cast<double>(out.Pixels.size()));
		}
		return decoded;
	}

	bool DecodeRaw(
		uint32_t width,
		uint32_t height,
		std::span<const std::byte> bytes,
		Image &out,
		std::string &failure,
		const Limits &limits
	) {
		ENGINE_PROFILE("imagecodec raw");
		if (!ValidateExtent(width, height, limits, failure)) return false;
		if (bytes.size() > limits.MaximumEncodedBytes ||
			bytes.size() != static_cast<uint64_t>(width) * height * 4) {
			failure = "image: raw RGBA8 byte count disagrees with dimensions or limit";
			return false;
		}
		Image decoded{width, height, {bytes.begin(), bytes.end()}};
		core::Metrics::Count("imagecodec.raw.operations", 1);
		core::Metrics::Count("imagecodec.raw.pixel_bytes", static_cast<double>(bytes.size()));
		out = std::move(decoded);
		failure.clear();
		return true;
	}

	namespace {
		int Sextet(char value) {
			if (value >= 'A' && value <= 'Z') return value - 'A';
			if (value >= 'a' && value <= 'z') return value - 'a' + 26;
			if (value >= '0' && value <= '9') return value - '0' + 52;
			if (value == '+') return 62;
			if (value == '/') return 63;
			return -1;
		}
	}

	bool DecodeBase64(
		std::string_view encoded, std::vector<std::byte> &out, std::string &failure, size_t maximumBytes
	) {
		ENGINE_PROFILE("imagecodec base64 decode");
		if (encoded.size() % 4 != 0 || encoded.size() / 4 > maximumBytes / 3 + (maximumBytes % 3 != 0)) {
			failure = "base64: encoded length exceeds limit or has incomplete quartet";
			return false;
		}
		const size_t padding =
			encoded.empty() ? 0 : (encoded.back() == '=' ? 1 + (encoded[encoded.size() - 2] == '=') : 0);
		const size_t decodedBytes = encoded.size() / 4 * 3 - padding;
		if (decodedBytes > maximumBytes) {
			failure = "base64: decoded bytes exceed the limit";
			return false;
		}
		for (size_t offset = 0; offset < encoded.size(); offset += 4) {
			const bool last = offset + 4 == encoded.size();
			const int first = Sextet(encoded[offset]);
			const int second = Sextet(encoded[offset + 1]);
			const int third = encoded[offset + 2] == '=' ? 0 : Sextet(encoded[offset + 2]);
			const int fourth = encoded[offset + 3] == '=' ? 0 : Sextet(encoded[offset + 3]);
			if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
				(encoded[offset + 2] == '=' && (!last || padding != 2 || (second & 15) != 0)) ||
				(encoded[offset + 3] == '=' && (!last || (padding == 1 && (third & 3) != 0)))) {
				failure = "base64: noncanonical alphabet or padding";
				return false;
			}
		}
		std::vector<std::byte> decoded(decodedBytes);
		size_t destination = 0;
		for (size_t offset = 0; offset < encoded.size(); offset += 4) {
			const uint32_t quartet =
				static_cast<uint32_t>(Sextet(encoded[offset])) << 18 |
				static_cast<uint32_t>(Sextet(encoded[offset + 1])) << 12 |
				static_cast<uint32_t>(encoded[offset + 2] == '=' ? 0 : Sextet(encoded[offset + 2])) << 6 |
				static_cast<uint32_t>(encoded[offset + 3] == '=' ? 0 : Sextet(encoded[offset + 3]));
			decoded[destination++] = static_cast<std::byte>(quartet >> 16);
			if (destination < decodedBytes) decoded[destination++] = static_cast<std::byte>(quartet >> 8);
			if (destination < decodedBytes) decoded[destination++] = static_cast<std::byte>(quartet);
		}
		core::Metrics::Count("imagecodec.base64.decode.operations", 1);
		core::Metrics::Count("imagecodec.base64.decode.bytes", static_cast<double>(decoded.size()));
		out = std::move(decoded);
		failure.clear();
		return true;
	}

	bool EncodeBase64(
		std::span<const std::byte> bytes, std::string &out, std::string &failure, size_t maximumBytes
	) {
		ENGINE_PROFILE("imagecodec base64 encode");
		const size_t quartets = bytes.size() / 3 + (bytes.size() % 3 != 0);
		if (bytes.size() > maximumBytes || quartets > std::numeric_limits<size_t>::max() / 4) {
			failure = "base64: bytes or encoded length exceed the limit";
			return false;
		}
		constexpr std::string_view ALPHABET =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string encoded(quartets * 4, '=');
		for (size_t offset = 0, destination = 0; offset < bytes.size(); offset += 3, destination += 4) {
			const size_t available = std::min<size_t>(3, bytes.size() - offset);
			const uint32_t value = static_cast<uint32_t>(bytes[offset]) << 16 |
								   (available > 1 ? static_cast<uint32_t>(bytes[offset + 1]) << 8 : 0) |
								   (available > 2 ? static_cast<uint32_t>(bytes[offset + 2]) : 0);
			encoded[destination] = ALPHABET[(value >> 18) & 63];
			encoded[destination + 1] = ALPHABET[(value >> 12) & 63];
			if (available > 1) encoded[destination + 2] = ALPHABET[(value >> 6) & 63];
			if (available > 2) encoded[destination + 3] = ALPHABET[value & 63];
		}
		core::Metrics::Count("imagecodec.base64.encode.operations", 1);
		core::Metrics::Count("imagecodec.base64.encode.bytes", static_cast<double>(encoded.size()));
		out = std::move(encoded);
		failure.clear();
		return true;
	}
}
