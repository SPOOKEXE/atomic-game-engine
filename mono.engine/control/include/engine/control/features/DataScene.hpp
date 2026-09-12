#pragma once

// Read-only MCP access to the script module's data-scene observations.
//
// @tier L13 · shared

#include <engine/control/Surface.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/world/Universe.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace engine::control {

	using nlohmann::json;

	namespace data_scene_detail {
		inline constexpr size_t MAXIMUM_DEPTH = 16;
		inline constexpr size_t MAXIMUM_RESULT_BYTES = 64u * 1024u;

		inline bool Only(const json &value, std::initializer_list<const char *> names, std::string &failure) {
			if (!value.is_object()) {
				failure = "arguments must be an object";
				return false;
			}
			for (const auto &[name, ignored] : value.items()) {
				(void)ignored;
				bool known = false;
				for (const char *allowed : names)
					known = known || name == allowed;
				if (!known) {
					failure = "unknown argument '" + name + "'";
					return false;
				}
			}
			return true;
		}

		inline bool Spend(size_t &bytes, size_t amount) {
			if (amount > MAXIMUM_RESULT_BYTES - bytes) return false;
			bytes += amount;
			return true;
		}

		inline bool Finite(float value) {
			return std::isfinite(value);
		}

		inline bool
		JsonValue(const script::ScriptValue &source, json &destination, size_t depth, size_t &bytes) {
			if (depth > MAXIMUM_DEPTH) return false;
			switch (source.Tag) {
			case script::ValueTag::Nil:
				if (!Spend(bytes, 4)) return false;
				destination = nullptr;
				return true;
			case script::ValueTag::False:
			case script::ValueTag::True:
				if (!Spend(bytes, 5)) return false;
				destination = source.Boolean;
				return true;
			case script::ValueTag::Number:
				if (!std::isfinite(source.Number) || !Spend(bytes, 32)) return false;
				destination = source.Number;
				return true;
			case script::ValueTag::String:
				if (!Spend(bytes, source.Text.size() + 2)) return false;
				destination = source.Text;
				return true;
			case script::ValueTag::Array: {
				destination = json::array();
				for (const script::ScriptValue &item : source.Items) {
					json converted;
					if (!Spend(bytes, 1) || !JsonValue(item, converted, depth + 1, bytes)) return false;
					destination.push_back(std::move(converted));
				}
				return true;
			}
			case script::ValueTag::Map: {
				destination = json::object();
				for (const auto &[name, item] : source.Entries) {
					json converted;
					if (!Spend(bytes, name.size() + 4) || !JsonValue(item, converted, depth + 1, bytes) ||
						destination.contains(name))
						return false;
					destination[name] = std::move(converted);
				}
				return true;
			}
			case script::ValueTag::Vector3:
				if (!Finite(source.Vector.X) || !Finite(source.Vector.Y) || !Finite(source.Vector.Z) ||
					!Spend(bytes, 96))
					return false;
				destination = json{{"x", source.Vector.X}, {"y", source.Vector.Y}, {"z", source.Vector.Z}};
				return true;
			case script::ValueTag::Color3:
				if (!Finite(source.Colour.R) || !Finite(source.Colour.G) || !Finite(source.Colour.B) ||
					!Spend(bytes, 96))
					return false;
				destination = json{{"r", source.Colour.R}, {"g", source.Colour.G}, {"b", source.Colour.B}};
				return true;
			case script::ValueTag::CFrame:
				if (!Finite(source.Frame.Position.X) || !Finite(source.Frame.Position.Y) ||
					!Finite(source.Frame.Position.Z) || !Finite(source.Frame.QuaternionX) ||
					!Finite(source.Frame.QuaternionY) || !Finite(source.Frame.QuaternionZ) ||
					!Finite(source.Frame.QuaternionW) || !Spend(bytes, 192))
					return false;
				destination = json{
					{"position", {source.Frame.Position.X, source.Frame.Position.Y, source.Frame.Position.Z}},
					{"rotation",
					 {source.Frame.QuaternionX,
					  source.Frame.QuaternionY,
					  source.Frame.QuaternionZ,
					  source.Frame.QuaternionW}}
				};
				return true;
			}
			return false;
		}

		inline json Result(const script::DataSceneResult &result, std::string &failure) {
			json converted;
			size_t bytes = 0;
			if (!JsonValue(result.Value, converted, 0, bytes)) {
				failure = "data-scene result exceeds the safe response limit";
				return nullptr;
			}
			if (converted.dump().size() > MAXIMUM_RESULT_BYTES) {
				failure = "data-scene result exceeds the safe response limit";
				return nullptr;
			}
			if (std::strcmp(result.Status, "ok") != 0) failure = converted.dump();
			return converted;
		}

		inline world::WorldId World(world::Universe &universe, const json &arguments, std::string &failure) {
			if (!arguments.contains("instance_id") || !arguments.at("instance_id").is_string()) {
				failure = "instance_id must name a scene";
				return {};
			}
			const std::string instance = arguments.at("instance_id").get<std::string>();
			if (instance.empty() || instance.size() > 128 || instance.find('\0') != std::string::npos) {
				failure = "instance_id must be a non-empty identifier of at most 128 bytes";
				return {};
			}
			const world::WorldId id = universe.Find(core::Name(instance));
			if (!id.IsValid()) failure = "no scene called '" + instance + "'";
			return id;
		}
	}

	inline void
	Surface::AddDataSceneTools(world::Universe &universe, std::shared_ptr<script::DataCaptureBridge> bridge) {
		world::Universe *worlds = &universe;
		auto schema = [] {
			return json{
				{"type", "object"},
				{"properties",
				 json{
					 {"instance_id", json{{"type", "string"}, {"minLength", 1}}},
					 {"options", json{{"type", "object"}}}
				 }},
				{"required", json::array({"instance_id", "options"})},
				{"additionalProperties", false}
			};
		};
		Add(Tool{
			"get_scene_snapshot",
			"A bounded read-only snapshot of one scene, using stable authored ids.",
			schema,
			[worlds](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
				if (!arguments.contains("options") || !arguments["options"].is_object()) {
					failure = "options must be an object";
					return nullptr;
				}
				if (!Only(arguments["options"], {"limit"}, failure)) return nullptr;
				const world::WorldId id = World(*worlds, arguments, failure);
				if (!failure.empty()) return nullptr;
				size_t limit = script::MAX_DATA_SCENE_ENTITIES;
				if (arguments["options"].contains("limit")) {
					const json &value = arguments["options"]["limit"];
					if (!value.is_number_unsigned() ||
						value.get<uint64_t>() > script::MAX_DATA_SCENE_ENTITIES) {
						failure = "options.limit must be an integer from 0 through 10000";
						return nullptr;
					}
					limit = value.get<size_t>();
				}
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					out = Result(script::GetSceneSnapshot(store, limit), failure);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});

		Add(Tool{
			"get_camera_rendering_data",
			"Camera calibration for a stable authored camera id, or the scene's active camera.",
			schema,
			[worlds](const json &arguments, std::string &failure) -> json {
				using namespace data_scene_detail;
				if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
				if (!arguments.contains("options") || !arguments["options"].is_object()) {
					failure = "options must be an object";
					return nullptr;
				}
				if (!Only(arguments["options"], {"camera_id"}, failure)) return nullptr;
				const world::WorldId id = World(*worlds, arguments, failure);
				if (!failure.empty()) return nullptr;
				std::string wanted;
				if (arguments["options"].contains("camera_id")) {
					if (!arguments["options"]["camera_id"].is_string()) {
						failure = "options.camera_id must be a string";
						return nullptr;
					}
					wanted = arguments["options"]["camera_id"].get<std::string>();
				}
				json out;
				const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
					ecs::Entity camera;
					if (wanted.empty()) {
						if (const auto *active = store.Resource<scene::ActiveCamera>())
							camera = active->Entity;
					} else
						store.Each<const ecs::InstanceName>([&](ecs::Entity entity,
																const ecs::InstanceName &) {
							ecs::AttributeValue value;
							if (ecs::GetAttribute(
									store, entity, core::Name(script::DATA_SCENE_ID_ATTRIBUTE), value
								) &&
								value.Type == ecs::PropertyType::String && value.String == wanted) {
								if (camera != ecs::NULL_ENTITY) {
									failure = "camera stable authored id is not unique";
									return;
								}
								camera = entity;
							}
						});
					if (camera == ecs::NULL_ENTITY) {
						failure = wanted.empty() ? "scene has no active camera"
												 : "no camera has that stable authored id";
						return;
					}
					out = Result(script::GetCameraRenderingData(store, camera), failure);
				});
				if (status != world::WorldStatus::Ok && failure.empty()) failure = "scene is unavailable";
				return out;
			}
		});

		const auto observation =
			[worlds, schema, bridge](std::string name, std::string description, bool channels) {
				return Tool{
					std::move(name),
					std::move(description),
					schema,
					[worlds, bridge, channels](const json &arguments, std::string &failure) -> json {
						using namespace data_scene_detail;
						if (!Only(arguments, {"instance_id", "options"}, failure)) return nullptr;
						if (!arguments.contains("options") || !arguments["options"].is_object()) {
							failure = "options must be an object";
							return nullptr;
						}
						if (!arguments["options"].empty()) {
							failure = "options must be empty";
							return nullptr;
						}
						const world::WorldId id = World(*worlds, arguments, failure);
						if (!failure.empty()) return nullptr;
						json out;
						const world::WorldStatus status = worlds->Enter(id, [&](ecs::Store &store) {
							out = Result(
								channels ? script::GetCaptureChannels(store, bridge)
										 : script::GetResources(store),
								failure
							);
						});
						if (status != world::WorldStatus::Ok && failure.empty())
							failure = "scene is unavailable";
						return out;
					}
				};
			};
		Add(observation("get_capture_channels", "Capture channel capability metadata for one scene.", true));
		Add(observation("get_resources", "Durable resource metadata for one scene.", false));
	}

	namespace features {
		inline Feature
		DataScene(world::Universe &universe, std::shared_ptr<script::DataCaptureBridge> bridge = {}) {
			return Feature{"data_scene", [&universe, bridge = std::move(bridge)](Surface &surface) {
							   surface.AddDataSceneTools(universe, bridge);
						   }};
		}
	}
}
