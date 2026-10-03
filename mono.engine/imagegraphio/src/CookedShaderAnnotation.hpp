#pragma once

#include <engine/imagegraph/Document.hpp>

#include <optional>
#include <string_view>

namespace engine::imagegraphio::detail {
	inline constexpr std::string_view CookedSelector = "composer_cooked_shader";
	inline bool CanonicalCookedSelector(std::string_view value) {
		if (value.size() != 72 || !value.ends_with(".ashader")) return false;
		for (char digit : value.substr(0, 64))
			if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return false;
		return true;
	}
	// This namespace belongs to the engine, independently of the foreign node's
	// attri and sockets.
	template <class Json>
	bool ReadCookedAnnotation(
		const Json &source,
		std::string_view type,
		std::optional<std::string_view> &selector,
		std::string &failure
	) {
		selector.reset();
		const auto envelope = source.find("atomic_game_engine");
		if (envelope == source.end()) return true;
		const bool reserved = envelope->is_object() && envelope->contains("composer_cooked_shader");
		if (type != "pc.hlsl" && !reserved) return true;
		const auto reject = [&](const char *message) {
			failure = message;
			return false;
		};
		if (!envelope->is_object()) return reject("engine node annotation must be an object");
		const auto version = envelope->find("version");
		if (version == envelope->end() || !version->is_number_integer() || *version != 1)
			return reject("engine node annotation version is unsupported");
		if (!reserved) return true;
		if (type != "pc.hlsl") return reject("cooked shader annotation requires a mapped HLSL node");
		const auto &value = envelope->at("composer_cooked_shader");
		if (!value.is_string()) return reject("cooked shader annotation must be a canonical content name");
		const auto &text = value.template get_ref<const std::string &>();
		if (!CanonicalCookedSelector(text))
			return reject("cooked shader annotation must be a canonical content name");
		selector = text;
		return true;
	}
	template <class Json>
	bool WriteCookedAnnotation(Json &source, const imagegraph::Node &node, std::string &failure) {
		const std::string *selector = nullptr;
		for (const auto &property : node.SourceProperties) {
			if (property.Port != CookedSelector) continue;
			if (node.Type != "pc.hlsl" || selector || !std::holds_alternative<std::string>(property.Data)) {
				failure = "cooked shader selector requires one HLSL text property";
				return false;
			}
			selector = &std::get<std::string>(property.Data);
			if (!CanonicalCookedSelector(*selector)) {
				failure = "cooked shader selector must be a canonical content name";
				return false;
			}
		}
		std::optional<std::string_view> previous;
		if (!ReadCookedAnnotation(source, node.Type, previous, failure)) return false;
		if (!selector) {
			if (previous) source["atomic_game_engine"].erase("composer_cooked_shader");
			return true;
		}
		if (!source.contains("atomic_game_engine")) source["atomic_game_engine"] = Json{{"version", 1}};
		source["atomic_game_engine"]["composer_cooked_shader"] = *selector;
		return true;
	}
} // namespace engine::imagegraphio::detail
