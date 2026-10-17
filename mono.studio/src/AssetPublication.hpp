#pragma once

// Publication must use the same identity as delivery before replacing any accepted files.

#include <engine/assets/Signature.hpp>

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <studio/ContentSources.hpp>

namespace studio {

	inline std::optional<engine::assets::SigningKey>
	ReadAssetPublishingKey(std::string_view hexSeed, const ContentSources &content, std::string &failure) {
		if (hexSeed.size() != engine::assets::SigningKey::SEED_BYTES * 2) {
			failure = "the signing key is 64 hex characters";
			return std::nullopt;
		}
		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		for (size_t index = 0; index < seed.size(); ++index) {
			const char *first = hexSeed.data() + index * 2;
			unsigned int byte = 0;
			const auto parsed = std::from_chars(first, first + 2, byte, 16);
			if (parsed.ec != std::errc{} || parsed.ptr != first + 2) {
				failure = "the signing key is not hex";
				return std::nullopt;
			}
			seed[index] = static_cast<std::byte>(byte);
		}
		auto signing = engine::assets::SigningKey::FromSeed(seed);
		if (!signing) {
			failure = "that is not a usable signing key";
			return std::nullopt;
		}
		const auto publisher = content.ToSettings().Publisher;
		if (publisher.IsZero()) {
			failure = "configure a content publisher before publishing";
			return std::nullopt;
		}
		if (signing->Public() != publisher) {
			failure = "signing key does not match the configured content publisher";
			return std::nullopt;
		}
		failure.clear();
		return signing;
	}
}
