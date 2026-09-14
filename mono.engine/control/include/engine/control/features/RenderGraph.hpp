#pragma once

#include <engine/control/Surface.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace engine::control::features {

	inline constexpr size_t MAXIMUM_GRAPH_NODES = 256;
	inline constexpr size_t MAXIMUM_GRAPH_RESOURCES = 512;
	inline constexpr uint32_t MAXIMUM_GRAPH_DIMENSION = 16384;
	inline constexpr size_t MAXIMUM_GRAPH_JSON_BYTES = 4 * 1024 * 1024;

	inline void RenderGraph(Surface &surface) {
		if (!surface.RenderGraph()) {
			return;
		}
		surface.Add(
			Tool{
				"get_render_graph",
				"Read the installed render graph, compiled schedule, and bounded memory profile.",
				[] {
					return nlohmann::json{
						{"type", "object"},
						{"additionalProperties", false},
						{"properties",
						 nlohmann::json{
							 {"instance_id",
							  nlohmann::json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
							 {"pipeline",
							  nlohmann::json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
							 {"view_width",
							  nlohmann::json{
								  {"type", "integer"}, {"minimum", 1}, {"maximum", MAXIMUM_GRAPH_DIMENSION}
							  }},
							 {"view_height",
							  nlohmann::json{
								  {"type", "integer"}, {"minimum", 1}, {"maximum", MAXIMUM_GRAPH_DIMENSION}
							  }},
							 {"expected_world_epoch", nlohmann::json{{"type", "integer"}, {"minimum", 0}}},
							 {"expected_world_version", nlohmann::json{{"type", "integer"}, {"minimum", 0}}},
						 }},
						{"required",
						 {"instance_id",
						  "pipeline",
						  "view_width",
						  "view_height",
						  "expected_world_epoch",
						  "expected_world_version"}},
					};
				},
				[&surface](const nlohmann::json &arguments, std::string &failure) {
					if (!surface.RenderGraph()) {
						failure = "unavailable: render graph provider is not installed";
						return nlohmann::json(nullptr);
					}
					if (!arguments.is_object()) {
						failure = "validation_failed: arguments must be an object";
						return nlohmann::json(nullptr);
					}
					for (const char *field : {"instance_id", "pipeline"}) {
						const bool valid = arguments.contains(field) && arguments[field].is_string();
						const std::string value = valid ? arguments[field].get<std::string>() : std::string();
						if (!valid || value.empty() || value.size() > 128) {
							failure =
								std::string("validation_failed: ") + field + " must contain 1 to 128 bytes";
							return nlohmann::json(nullptr);
						}
					}
					nlohmann::json validated = arguments;
					for (const char *field : {"view_width", "view_height"}) {
						const bool valid = arguments.contains(field) && arguments[field].is_number_unsigned();
						const uint64_t value = valid ? arguments[field].get<uint64_t>() : 0;
						if (!valid || value == 0 || value > MAXIMUM_GRAPH_DIMENSION) {
							failure = std::string("validation_failed: ") + field +
									  " is outside the bounded view size";
							return nlohmann::json(nullptr);
						}
						validated[field] = static_cast<uint32_t>(value);
					}
					for (const char *field : {"expected_world_epoch", "expected_world_version"})
						if (!arguments.contains(field) || !arguments[field].is_number_unsigned()) {
							failure =
								std::string("validation_failed: ") + field + " must be an unsigned integer";
							return nlohmann::json(nullptr);
						}
					nlohmann::json result = surface.RenderGraph()(validated, failure);
					if (result.is_null()) return result;
					if (result.dump().size() > MAXIMUM_GRAPH_JSON_BYTES) {
						failure = "resource_limit: render graph reply exceeds 4 MiB";
						return nlohmann::json(nullptr);
					}
					if ((result.contains("nodes") && !result["nodes"].is_array()) ||
						(result.contains("resources") && !result["resources"].is_array()) ||
						(result.contains("nodes") && result["nodes"].size() > MAXIMUM_GRAPH_NODES) ||
						(result.contains("resources") &&
						 result["resources"].size() > MAXIMUM_GRAPH_RESOURCES)) {
						failure = "resource_limit: render graph node or resource bound exceeded";
						return nlohmann::json(nullptr);
					}
					return result;
				}
			}
		);
	}
}
