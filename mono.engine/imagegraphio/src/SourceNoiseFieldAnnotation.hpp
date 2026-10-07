#pragma once

// Native typed-field choices never occupy a foreign source socket.
#include <engine/imagegraph/NoiseField.hpp>

#include <algorithm>
#include <optional>
#include <string>

namespace engine::imagegraphio::detail {
	struct SourceNoiseFieldAnnotation {
		std::optional<int64_t> OutputType;
		bool OverrideInstance = false;
	};
	template <class Json>
	bool ReadSourceNoiseFieldAnnotation(
		const Json &source, std::string_view type, SourceNoiseFieldAnnotation &result, std::string &failure
	) {
		result = {};
		const auto envelope = source.find("atomic_game_engine");
		if (envelope == source.end()) return true;
		const bool reserved = envelope->is_object() && envelope->contains("noise_field");
		if (!imagegraph::IsNoiseImageGenerator(type) && !reserved) return true;
		const auto reject = [&](const char *message) {
			failure = message;
			return false;
		};
		if (!envelope->is_object()) return reject("engine node annotation must be an object");
		const auto version = envelope->find("version");
		if (version == envelope->end() || !version->is_number_integer() || *version != 1)
			return reject("engine node annotation version is unsupported");
		if (!reserved) return true;
		if (!imagegraph::IsNoiseImageGenerator(type))
			return reject("noise field annotation requires a mapped noise generator");
		const auto &field = envelope->at("noise_field");
		if (!field.is_object()) return reject("noise field annotation must be an object");
		const auto output = field.find("output_type");
		if (output != field.end()) {
			if (!output->is_number_integer() || *output < 1 || *output > 3)
				return reject("noise field output type must be Scalar, RG or RGB");
			result.OutputType = output->template get<int64_t>();
		}
		const auto override = field.find("override_instance");
		if (override != field.end()) {
			if (!override->is_boolean()) return reject("noise field instance override must be boolean");
			result.OverrideInstance = override->template get<bool>();
		}
		return true;
	}
	template <class Json>
	bool WriteSourceNoiseFieldAnnotation(Json &source, const imagegraph::Node &node, std::string &failure) {
		SourceNoiseFieldAnnotation previous;
		if (!ReadSourceNoiseFieldAnnotation(source, node.Type, previous, failure)) return false;
		if (!imagegraph::IsNoiseImageGenerator(node.Type)) return true;
		if (node.Values.size() > imagegraph::Limits::MaximumPropertiesPerNode ||
			node.InstanceOverrides.size() > imagegraph::Limits::MaximumPropertiesPerNode) {
			failure = "noise field selector exceeds native property bounds";
			return false;
		}
		const imagegraph::EnumValue *output = nullptr;
		for (const auto &value : node.Values) {
			if (value.Port != "output_type") continue;
			const auto *choice = std::get_if<imagegraph::EnumValue>(&value.Data);
			if (output || !choice || choice->Value < 1 || choice->Value > 3) {
				failure = "noise field output type requires one Enum Scalar, RG or RGB";
				return false;
			}
			output = choice;
		}
		const auto overrides =
			std::count(node.InstanceOverrides.begin(), node.InstanceOverrides.end(), "output_type");
		if (overrides > 1) {
			failure = "noise field instance override has duplicate identity";
			return false;
		}
		const bool existing =
			source.contains("atomic_game_engine") && source["atomic_game_engine"].contains("noise_field");
		if (!output && !overrides && !existing) return true;
		if (!source.contains("atomic_game_engine")) source["atomic_game_engine"] = Json{{"version", 1}};
		auto &envelope = source["atomic_game_engine"];
		if (!envelope.contains("noise_field")) envelope["noise_field"] = Json::object();
		auto &field = envelope["noise_field"];
		if (output)
			field["output_type"] = output->Value;
		else
			field.erase("output_type");
		if (overrides)
			field["override_instance"] = true;
		else
			field.erase("override_instance");
		return true;
	}
} // namespace engine::imagegraphio::detail
