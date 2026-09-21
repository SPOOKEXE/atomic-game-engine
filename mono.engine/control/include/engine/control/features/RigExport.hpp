#pragma once

// Read-only MCP access to the VM-neutral data-rig/v1 observation.
// @tier L13 · shared

#include <engine/control/DataFactoryReadFence.hpp>
#include <engine/control/Surface.hpp>
#include <engine/script/RigExport.hpp>
#include <engine/world/Universe.hpp>

#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace engine::control {
	using nlohmann::json;
	namespace rig_export_detail {
		// Maximum pretty-printed JSON bytes returned for one data-rig/v1 export.
		inline constexpr size_t MAXIMUM_BYTES = 4u * 1024u * 1024u;
		// Converts VM-neutral values to JSON, preserving unsigned fields named by the rig schema.
		inline bool Convert(
			const script::ScriptValue &source, json &destination, size_t depth, std::string_view field = {}
		) {
			if (depth > 16) return false;
			switch (source.Tag) {
			case script::ValueTag::Nil:
				destination = nullptr;
				return true;
			case script::ValueTag::False:
			case script::ValueTag::True:
				destination = source.Boolean;
				return true;
			case script::ValueTag::Number:
				if (!std::isfinite(source.Number)) return false;
				if (field == "slot" || field == "tick_seconds_numerator" ||
					field == "tick_seconds_denominator" || field == "start_tick" || field == "end_tick" ||
					field == "joint_slot" || field == "tick" || field == "seconds_numerator" ||
					field == "seconds_denominator") {
					constexpr double FIRST_UNREPRESENTABLE_UINT64 = 18'446'744'073'709'551'616.0;
					if (source.Number < 0.0 || source.Number >= FIRST_UNREPRESENTABLE_UINT64 ||
						std::floor(source.Number) != source.Number)
						return false;
					destination = static_cast<uint64_t>(source.Number);
					return true;
				}
				destination = source.Number;
				return true;
			case script::ValueTag::String:
				destination = source.Text;
				return true;
			case script::ValueTag::Array:
				destination = json::array();
				for (const auto &item : source.Items) {
					json value;
					if (!Convert(item, value, depth + 1)) return false;
					destination.push_back(std::move(value));
				}
				return true;
			case script::ValueTag::Map:
				destination = json::object();
				for (const auto &[name, item] : source.Entries) {
					json value;
					if (destination.contains(name) || !Convert(item, value, depth + 1, name)) return false;
					destination[name] = std::move(value);
				}
				return true;
			default:
				return false;
			}
		}
		// Converts one script result and rejects it when its serialized reply exceeds the MCP budget.
		inline bool Result(const script::ScriptValue &source, json &destination) {
			if (!Convert(source, destination, 0) || destination.dump(2).size() > MAXIMUM_BYTES) {
				destination = nullptr;
				return false;
			}
			return true;
		}
		// Rejects fields outside the top-level rig-export request shape.
		inline bool Only(const json &arguments, std::string &failure) {
			if (!arguments.is_object()) {
				failure = "arguments must be an object";
				return false;
			}
			for (const auto &[name, ignored] : arguments.items()) {
				(void)ignored;
				if (name != "instance_id" && name != "export_id" && name != "options") {
					failure = "unknown argument '" + name + "'";
					return false;
				}
			}
			return true;
		}
		// Recognizes export selectors and lifecycle fields required by a session-bound read.
		inline bool Option(std::string_view name, bool requireRevision) {
			return name == "entity_ids" || name == "limit" ||
				   (requireRevision && data_factory_read_fence::IsExpectedRevisionField(name));
		}
		// Resolves the caller's stable instance name before entering its world.
		inline world::WorldId
		World(world::Universe &universe, std::string_view instance, std::string &failure) {
			const world::WorldId id = universe.Find(core::Name(instance));
			if (!id.IsValid()) failure = "no scene called '" + std::string(instance) + "'";
			return id;
		}
	}
	inline Tool RigExportTool(world::Universe &universe, world::DataFactorySession *session) {
		world::Universe *worlds = session != nullptr ? &session->UniverseOf() : &universe;
		return Tool{
			"get_rig_export",
			"A bounded data-rig/v1 export of identified skeletons. Missing engine source data is declared "
			"rather than inferred.",
			[session] {
				json optionProperties{
					{"entity_ids",
					 json{
						 {"type", "array"},
						 {"maxItems", script::MAX_RIG_EXPORT_ENTITIES},
						 {"items",
						  json{
							  {"type", "string"},
							  {"minLength", 1},
							  {"maxLength", script::MAX_RIG_EXPORT_ENTITY_ID_BYTES}
						  }}
					 }},
					{"limit",
					 json{{"type", "integer"}, {"minimum", 0}, {"maximum", script::MAX_RIG_EXPORT_ENTITIES}}},
				};
				if (session != nullptr) {
					optionProperties["expected_tick"] = {{"type", "integer"}, {"minimum", 0}};
					optionProperties["expected_world_epoch"] = {{"type", "integer"}, {"minimum", 0}};
					optionProperties["expected_world_version"] = {{"type", "integer"}, {"minimum", 0}};
				}
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"instance_id", json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
						 {"export_id", json{{"type", "string"}, {"minLength", 1}, {"maxLength", 512}}},
						 {"options",
						  json{
							  {"type", "object"},
							  {"properties", std::move(optionProperties)},
							  {"additionalProperties", false},
							  {"required",
							   session != nullptr
								   ? json::array(
										 {"expected_tick", "expected_world_epoch", "expected_world_version"}
									 )
								   : json::array()}
						  }}
					 }},
					{"required", json::array({"instance_id", "export_id", "options"})},
					{"additionalProperties", false}
				};
			},
			[worlds, session](const json &arguments, std::string &failure) -> json {
				using namespace rig_export_detail;
				if (!Only(arguments, failure)) return nullptr;
				if (!arguments.contains("export_id") || !arguments["export_id"].is_string()) {
					failure = "export_id must be a string";
					return nullptr;
				}
				if (!arguments.contains("options") || !arguments["options"].is_object()) {
					failure = "options must be an object";
					return nullptr;
				}
				for (const auto &[name, ignored] : arguments["options"].items()) {
					(void)ignored;
					if (!Option(name, session != nullptr)) {
						failure = "unknown option '" + name + "'";
						return nullptr;
					}
				}
				std::vector<std::string> selection;
				if (arguments["options"].contains("entity_ids")) {
					const json &ids = arguments["options"]["entity_ids"];
					if (!ids.is_array() || ids.size() > script::MAX_RIG_EXPORT_ENTITIES) {
						failure = "options.entity_ids must contain at most 256 strings";
						return nullptr;
					}
					for (const json &item : ids) {
						if (!item.is_string()) {
							failure = "options.entity_ids must contain strings";
							return nullptr;
						}
						selection.push_back(item.get<std::string>());
					}
				}
				size_t limit = script::MAX_RIG_EXPORT_ENTITIES;
				if (arguments["options"].contains("limit")) {
					const json &value = arguments["options"]["limit"];
					if (!value.is_number_unsigned() ||
						value.get<uint64_t>() > script::MAX_RIG_EXPORT_ENTITIES) {
						failure = "options.limit must be an integer from 0 through 256";
						return nullptr;
					}
					limit = value.get<size_t>();
				}
				std::string instance;
				if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
				json fence;
				if (!data_factory_read_fence::Validate(
						session, instance, arguments["options"], fence, failure
					))
					return fence;
				const world::WorldId id = World(*worlds, instance, failure);
				if (!failure.empty()) return nullptr;
				json output;
				bool overflow = false;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					const script::RigExportResult result = script::GetRigExport(
						store, arguments["export_id"].get<std::string>(), selection, limit
					);
					if (!Result(result.Value, output)) {
						overflow = true;
						return;
					}
					if (std::string_view(result.Status) != "ok") failure = output.dump();
				});
				if (overflow) {
					failure = "rig export exceeds the 4 MiB response limit";
					return nullptr;
				}
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return output;
			}
		};
	}

	inline void Surface::AddRigExportTools(world::Universe &universe, world::DataFactorySession *session) {
		Add(RigExportTool(universe, session));
	}
	namespace features {
		// Registers bounded rig export, optionally fenced to the supplied session revision.
		inline Feature RigExport(world::Universe &universe, world::DataFactorySession *session = nullptr) {
			return Feature{"rig-export", [&universe, session](Surface &surface) {
							   surface.AddRigExportTools(universe, session);
						   }};
		}
	}
}
