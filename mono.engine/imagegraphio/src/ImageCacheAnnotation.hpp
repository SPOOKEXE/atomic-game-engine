#pragma once

#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraph/Document.hpp>

#include <optional>
#include <string_view>

namespace engine::imagegraphio::detail {
	inline constexpr std::string_view ImageCacheUse = "cache_use";
	inline constexpr std::string_view ImageCacheData = "cache_data";
	inline constexpr std::string_view ImageCacheLayout = "composer_sprite_cache_layout";
	inline constexpr std::string_view ImageCacheHash = "composer_sprite_cache_data_hash";
	inline bool SourceImageCacheType(std::string_view type) {
		return type == "pc.image" || type == "pc.image_sequence" || type == "pc.image_animated";
	}
	struct ImageCacheAttributes {
		std::optional<bool> Enabled;
		std::optional<std::string_view> Data;
		std::optional<bake::SpriteCacheLayout> Layout;
		std::optional<std::string_view> Hash;
	};
	// Layout is native metadata bound to the exact foreign cache text. It never
	// changes the meaning of an unobserved GameMaker surface buffer.
	template <class Json>
	bool ReadImageCacheAnnotation(
		const Json &source, std::string_view type, ImageCacheAttributes &result, std::string &failure
	) {
		ImageCacheAttributes candidate;
		const auto reject = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto attributes = source.find("attri");
		if (SourceImageCacheType(type) && attributes != source.end()) {
			if (!attributes->is_object()) return reject("source image attributes must be an object");
			const auto use = attributes->find("cache_use");
			if (use != attributes->end()) {
				if (!use->is_boolean()) return reject("source image cache_use must be a boolean");
				candidate.Enabled = use->template get<bool>();
			}
			const auto data = attributes->find("cache_data");
			if (data != attributes->end()) {
				if (!data->is_string()) {
					if (candidate.Enabled.value_or(false))
						return reject("enabled source image cache_data must be text");
				} else {
					const auto &text = data->template get_ref<const std::string &>();
					if (text.size() <= bake::SpriteCacheLimits::MaximumEncodedBytes)
						candidate.Data = text;
					else if (candidate.Enabled.value_or(false))
						return reject("enabled source image cache_data exceeds native text bounds");
				}
			}
		}
		const auto envelope = source.find("atomic_game_engine");
		if (envelope != source.end() && envelope->is_object() && envelope->contains("sprite_cache")) {
			if (!SourceImageCacheType(type))
				return reject("sprite cache annotation requires a mapped image node");
			const auto version = envelope->find("version");
			if (version == envelope->end() || !version->is_number_integer() || *version != 1)
				return reject("engine node annotation version is unsupported");
			const auto &cache = envelope->at("sprite_cache");
			if (cache.is_object() && !cache.contains("layout") && !cache.contains("data_hash")) {
				result = candidate;
				return true;
			}
			if (!cache.is_object() || !cache.contains("layout") || !cache.at("layout").is_string() ||
				!cache.contains("data_hash") || !cache.at("data_hash").is_string())
				return reject("sprite cache annotation requires named layout and data hash");
			candidate.Layout =
				bake::ParseSpriteCacheLayoutName(cache.at("layout").template get_ref<const std::string &>());
			candidate.Hash = cache.at("data_hash").template get_ref<const std::string &>();
			if (!candidate.Layout || !candidate.Data)
				return reject("sprite cache annotation has no bounded cache text or known layout");
			const auto hash = bake::SpriteCacheDataHash(*candidate.Data);
			if (!hash || *candidate.Hash != std::string_view(hash->data(), hash->size()))
				return reject("sprite cache annotation does not match exact cache_data bytes");
		}
		result = candidate;
		return true;
	}
	template <class Json>
	bool WriteImageCacheAnnotation(Json &source, const imagegraph::Node &node, std::string &failure) {
		const bool *enabled = nullptr;
		const std::string *data = nullptr, *layout = nullptr, *hash = nullptr;
		const auto reject = [&](const char *message) {
			failure = message;
			return false;
		};
		for (const auto &property : node.SourceProperties) {
			if (property.Port != ImageCacheUse && property.Port != ImageCacheData &&
				property.Port != ImageCacheLayout && property.Port != ImageCacheHash)
				continue;
			if (!SourceImageCacheType(node.Type))
				return reject("sprite cache properties require an image node");
			if (property.Port == ImageCacheUse) {
				if (enabled || !std::holds_alternative<bool>(property.Data))
					return reject("image cache_use requires one boolean property");
				enabled = &std::get<bool>(property.Data);
				continue;
			}
			const auto *text = std::get_if<std::string>(&property.Data);
			if (!text || text->size() > bake::SpriteCacheLimits::MaximumEncodedBytes)
				return reject("image cache property requires bounded text");
			auto **target = property.Port == ImageCacheData		? &data
							: property.Port == ImageCacheLayout ? &layout
																: &hash;
			if (*target) return reject("image cache properties must be unique");
			*target = text;
		}
		ImageCacheAttributes previous;
		if (!ReadImageCacheAnnotation(source, node.Type, previous, failure)) return false;
		if (!SourceImageCacheType(node.Type)) return true;
		if (bool(layout) != bool(hash))
			return reject("image cache layout and data hash must be supplied together");
		if (layout) {
			const auto parsed = bake::ParseSpriteCacheLayoutName(*layout);
			const auto actual = data ? bake::SpriteCacheDataHash(*data) : std::nullopt;
			if (!parsed || !actual || *hash != std::string_view(actual->data(), actual->size()))
				return reject("image cache layout is not bound to exact authored cache_data");
			const auto envelope = source.find("atomic_game_engine");
			if (envelope != source.end() &&
				(!envelope->is_object() || !envelope->contains("version") ||
				 !envelope->at("version").is_number_integer() || envelope->at("version") != 1))
				return reject("engine node annotation version is unsupported");
		}
		if (enabled || data) {
			if (!source.contains("attri")) source["attri"] = Json::object();
			if (!source["attri"].is_object()) return reject("source image attributes must be an object");
			if (enabled)
				source["attri"]["cache_use"] = *enabled;
			else
				source["attri"].erase("cache_use");
			if (data) source["attri"]["cache_data"] = *data;
			// An inactive oversized foreign cache remains archive-owned and untouched.
			else if (previous.Data)
				source["attri"].erase("cache_data");
		} else if (source.contains("attri") && source["attri"].is_object()) {
			source["attri"].erase("cache_use");
			if (previous.Data) source["attri"].erase("cache_data");
		}
		if (layout) {
			if (!source.contains("atomic_game_engine")) source["atomic_game_engine"] = Json{{"version", 1}};
			auto &envelope = source["atomic_game_engine"];
			if (!envelope.contains("sprite_cache")) envelope["sprite_cache"] = Json::object();
			envelope["sprite_cache"]["layout"] = *layout;
			envelope["sprite_cache"]["data_hash"] = *hash;
		} else if (previous.Layout) {
			auto &cache = source["atomic_game_engine"]["sprite_cache"];
			cache.erase("layout");
			cache.erase("data_hash");
			if (cache.empty()) source["atomic_game_engine"].erase("sprite_cache");
		}
		return true;
	}
}
