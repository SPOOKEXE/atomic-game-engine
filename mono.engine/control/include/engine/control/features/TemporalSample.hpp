#pragma once

// A bounded, retained-snapshot pose sample for a data factory. This stays
// beside DataScene because it is an MCP observation, while the lifecycle
// session remains the authority for whether that observation names a snapshot.

#include <engine/control/Surface.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/Components.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace temporal_sample_detail {
		// Byte limit shared by the instance and retained snapshot identifiers.
		inline constexpr size_t MAXIMUM_INSTANCE_OR_SNAPSHOT_ID = 128;
		// Byte limit for authored stable IDs used to select the camera and objects.
		inline constexpr size_t MAXIMUM_STABLE_ID = script::MAX_DATA_SCENE_ID_BYTES;
		// Maximum object poses returned alongside the selected camera pose.
		inline constexpr size_t MAXIMUM_OBJECTS = script::MAX_CAMERA_OBJECT_OBSERVATIONS;
		// JSON byte budget for one temporal sample reply.
		inline constexpr size_t MAXIMUM_RESPONSE_BYTES = 64u * 1024u;

		// Joins a stable error code with a human-readable validation detail.
		inline std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

		// Verifies an identifier has no malformed, overlong, or surrogate UTF-8 sequence.
		inline bool Utf8(std::string_view value) {
			for (size_t index = 0; index < value.size();) {
				const uint8_t first = static_cast<uint8_t>(value[index++]);
				if (first < 0x80) continue;
				const unsigned extra = first >= 0xC2 && first <= 0xDF	? 1
									   : first >= 0xE0 && first <= 0xEF ? 2
									   : first >= 0xF0 && first <= 0xF4 ? 3
																		: 4;
				if (extra == 4 || index + extra > value.size()) return false;
				uint32_t codepoint = first & ((1u << (7 - extra)) - 1u);
				for (unsigned part = 0; part < extra; ++part) {
					const uint8_t next = static_cast<uint8_t>(value[index++]);
					if ((next & 0xC0u) != 0x80u) return false;
					codepoint = (codepoint << 6u) | (next & 0x3Fu);
				}
				if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
					(extra == 3 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
					(codepoint >= 0xD800 && codepoint <= 0xDFFF))
					return false;
			}
			return true;
		}

		// Reads a nonempty, NUL-free UTF-8 identifier within its caller supplied byte limit.
		inline bool Text(
			const json &value, std::string_view name, size_t maximum, std::string &out, std::string &failure
		) {
			if (!value.is_string()) {
				failure = Error("validation_failed", std::string(name) + " must be a string");
				return false;
			}
			out = value.get<std::string>();
			if (out.empty() || out.size() > maximum || out.find('\0') != std::string::npos || !Utf8(out)) {
				failure = Error(
					"validation_failed",
					std::string(name) + " must be valid UTF-8 with 1 to " + std::to_string(maximum) + " bytes"
				);
				return false;
			}
			return true;
		}

		// Reads a nonnegative JSON integer without narrowing its world revision value.
		inline bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure) {
			if (!value.is_number_unsigned()) {
				failure = Error("validation_failed", std::string(name) + " must be an unsigned integer");
				return false;
			}
			out = value.get<uint64_t>();
			return true;
		}

		// Borrows a required request member so its type can be checked by the caller.
		inline bool Field(const json &values, std::string_view name, const json *&out, std::string &failure) {
			const auto found = values.find(std::string(name));
			if (found == values.end()) {
				failure = Error("validation_failed", std::string(name) + " is required");
				return false;
			}
			out = &*found;
			return true;
		}

		// Rejects fields outside the temporal-sample wire request.
		inline bool Only(const json &values, std::string &failure) {
			if (!values.is_object()) {
				failure = Error("validation_failed", "arguments must be an object");
				return false;
			}
			static constexpr std::string_view names[]{
				"instance_id",
				"snapshot_id",
				"expected_world_epoch",
				"expected_world_version",
				"expected_tick",
				"camera_id",
				"object_ids"
			};
			for (auto field = values.begin(); field != values.end(); ++field) {
				if (std::find(std::begin(names), std::end(names), std::string_view(field.key())) ==
					std::end(names)) {
					failure = Error("validation_failed", "unknown argument: " + field.key());
					return false;
				}
			}
			return true;
		}

		// Match camera metadata's small-drift rule so every exported rigid pose is
		// finite and unit length, while ordinary float roundoff does not discard a sample.
		inline bool RigidFrame(const core::CFrame &source, core::CFrame &canonical) {
			const double rotationLength = std::hypot(
				std::hypot(static_cast<double>(source.QuaternionX), source.QuaternionY),
				std::hypot(static_cast<double>(source.QuaternionZ), source.QuaternionW)
			);
			if (!std::isfinite(source.Position.X) || !std::isfinite(source.Position.Y) ||
				!std::isfinite(source.Position.Z) || !std::isfinite(rotationLength) ||
				std::abs(rotationLength - 1.0) > 0.001)
				return false;
			canonical = source;
			canonical.QuaternionX = static_cast<float>(canonical.QuaternionX / rotationLength);
			canonical.QuaternionY = static_cast<float>(canonical.QuaternionY / rotationLength);
			canonical.QuaternionZ = static_cast<float>(canonical.QuaternionZ / rotationLength);
			canonical.QuaternionW = static_cast<float>(canonical.QuaternionW / rotationLength);
			return true;
		}

		// Writes a rigid world transform as position and unit quaternion JSON arrays.
		inline json Frame(const core::CFrame &frame) {
			return {
				{"position", {frame.Position.X, frame.Position.Y, frame.Position.Z}},
				{"rotation", {frame.QuaternionX, frame.QuaternionY, frame.QuaternionZ, frame.QuaternionW}},
			};
		}

		// Reads the authored data-scene ID that remains stable across ECS entity allocation.
		inline bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out) {
			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, entity, core::Name(script::DATA_SCENE_ID_ATTRIBUTE), value) ||
				value.Type != ecs::PropertyType::String || value.String.empty())
				return false;
			out = value.String;
			return true;
		}
	}

	inline void
	Surface::AddTemporalSampleTools(world::Universe &universe, world::DataFactorySession &session) {
		using namespace temporal_sample_detail;
		world::Universe *worlds = &universe;
		world::DataFactorySession *lifecycle = &session;
		Add(Tool{
			"get_temporal_sample",
			"Copies one selected camera and up to 64 selected object poses from a named retained "
			"all-systems-paused snapshot.",
			[] {
				return json{
					{"type", "object"},
					{"properties",
					 json{
						 {"instance_id", json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
						 {"snapshot_id", json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
						 {"expected_world_epoch", json{{"type", "integer"}, {"minimum", 0}}},
						 {"expected_world_version", json{{"type", "integer"}, {"minimum", 0}}},
						 {"expected_tick", json{{"type", "integer"}, {"minimum", 0}}},
						 {"camera_id", json{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
						 {"object_ids",
						  json{{"type", "array"}, {"maxItems", 64}, {"items", json{{"type", "string"}}}}}
					 }},
					{"required",
					 json::array(
						 {"instance_id",
						  "snapshot_id",
						  "expected_world_epoch",
						  "expected_world_version",
						  "expected_tick",
						  "camera_id",
						  "object_ids"}
					 )},
					{"additionalProperties", false},
				};
			},
			[worlds, lifecycle](const json &arguments, std::string &failure) -> json {
				using namespace temporal_sample_detail;
				if (!Only(arguments, failure)) return nullptr;
				const json *field = nullptr;
				std::string instanceId, snapshotId, cameraId;
				uint64_t epoch = 0, version = 0, tick = 0;
				if (!Field(arguments, "instance_id", field, failure) ||
					!Text(*field, "instance_id", MAXIMUM_INSTANCE_OR_SNAPSHOT_ID, instanceId, failure) ||
					!Field(arguments, "snapshot_id", field, failure) ||
					!Text(*field, "snapshot_id", MAXIMUM_INSTANCE_OR_SNAPSHOT_ID, snapshotId, failure) ||
					!Field(arguments, "expected_world_epoch", field, failure) ||
					!UInt(*field, "expected_world_epoch", epoch, failure) ||
					!Field(arguments, "expected_world_version", field, failure) ||
					!UInt(*field, "expected_world_version", version, failure) ||
					!Field(arguments, "expected_tick", field, failure) ||
					!UInt(*field, "expected_tick", tick, failure) ||
					!Field(arguments, "camera_id", field, failure) ||
					!Text(*field, "camera_id", MAXIMUM_STABLE_ID, cameraId, failure))
					return nullptr;
				if (!Field(arguments, "object_ids", field, failure) || !field->is_array() ||
					field->size() > MAXIMUM_OBJECTS) {
					if (failure.empty())
						failure = Error("validation_failed", "object_ids must contain at most 64 strings");
					return nullptr;
				}
				std::vector<std::string> objectIds;
				objectIds.reserve(field->size());
				std::unordered_set<std::string> requested;
				for (const json &value : *field) {
					std::string id;
					if (!Text(value, "object_ids entry", MAXIMUM_STABLE_ID, id, failure)) return nullptr;
					if (id == cameraId) {
						failure = Error("validation_failed", "camera_id must not appear in object_ids");
						return nullptr;
					}
					if (!requested.emplace(id).second) {
						failure = Error("validation_failed", "object_ids must not repeat a stable id");
						return nullptr;
					}
					objectIds.push_back(std::move(id));
				}

				const world::DataFactoryReply current = lifecycle->Inspect(instanceId);
				if (current.Status != world::DataFactoryStatus::Ok) {
					failure = Error(world::Describe(current.Status), current.Detail);
					return nullptr;
				}
				if (current.WorldEpoch != epoch || current.WorldVersion != version ||
					current.Clock.Tick != tick) {
					failure = Error(
						"version_conflict", "temporal sample requires the current completed world version"
					);
					return nullptr;
				}
				const world::DataFactoryReply barrier =
					lifecycle->RenderSnapshotBarrier(instanceId, snapshotId);
				if (barrier.Status != world::DataFactoryStatus::Ok) {
					failure = Error(world::Describe(barrier.Status), barrier.Detail);
					return nullptr;
				}

				std::unordered_set<std::string> wanted = requested;
				wanted.emplace(cameraId);
				std::unordered_map<std::string, ecs::Entity> selected;
				bool duplicateIdentity = false;
				std::string duplicateId;
				json out;
				const world::WorldId world = worlds->Find(core::Name(barrier.InstanceId));
				const world::WorldStatus status = worlds->Enter(world, [&](ecs::Store &store) {
					store.Each<const ecs::InstanceName>([&](ecs::Entity entity, const ecs::InstanceName &) {
						std::string id;
						if (!StableId(store, entity, id) || !wanted.contains(id)) return;
						if (id.size() > MAXIMUM_STABLE_ID || id.find('\0') != std::string::npos ||
							!Utf8(id)) {
							duplicateIdentity = true;
							duplicateId = id;
							return;
						}
						if (!selected.emplace(id, entity).second) {
							duplicateIdentity = true;
							duplicateId = id;
						}
					});
					if (duplicateIdentity) return;
					const auto camera = selected.find(cameraId);
					if (camera == selected.end()) {
						failure = Error("validation_failed", "camera_id does not name a stable camera");
						return;
					}
					const auto *cameraRow = store.Get<scene::Camera>(camera->second);
					const auto *cameraTransform = store.Get<scene::Transform>(camera->second);
					core::CFrame cameraFrame;
					if (cameraRow == nullptr || cameraTransform == nullptr ||
						!RigidFrame(cameraTransform->Frame, cameraFrame)) {
						failure = Error(
							"validation_failed",
							"camera_id does not name a camera with a valid rigid world pose"
						);
						return;
					}
					json objects = json::array();
					std::sort(objectIds.begin(), objectIds.end());
					for (const std::string &id : objectIds) {
						json pose{
							{"available", false},
							{"reason", "stable_object_not_found"},
							{"world_from_object", nullptr}
						};
						if (const auto object = selected.find(id); object != selected.end()) {
							core::CFrame objectFrame;
							if (const auto *transform = store.Get<scene::Transform>(object->second);
								transform != nullptr && RigidFrame(transform->Frame, objectFrame))
								pose = {
									{"available", true},
									{"reason", ""},
									{"world_from_object", Frame(objectFrame)}
								};
							else
								pose = {
									{"available", false},
									{"reason", "object_transform_unavailable"},
									{"world_from_object", nullptr}
								};
						}
						objects.push_back({{"id", id}, {"pose", std::move(pose)}});
					}
					out = {
						{"schema_version", "temporal-sample/v1"},
						{"snapshot_id", snapshotId},
						{"instance_id", barrier.InstanceId},
						{"world_epoch", barrier.WorldEpoch},
						{"world_version", barrier.WorldVersion},
						{"tick", barrier.Clock.Tick},
						{"time_ns", barrier.Clock.TimeNanoseconds},
						{"rational_time_available", barrier.Clock.RationalTimeAvailable},
						{"dt_ns",
						 {{"numerator", barrier.Clock.Interval.NumeratorNanoseconds},
						  {"denominator", barrier.Clock.Interval.Denominator}}},
						{"camera", {{"id", cameraId}, {"world_from_camera", Frame(cameraFrame)}}},
						{"objects", std::move(objects)},
					};
				});
				if (status != world::WorldStatus::Ok && failure.empty()) {
					failure = Error("validation_failed", "scene is unavailable");
					return nullptr;
				}
				if (duplicateIdentity && failure.empty()) {
					failure = Error("identity_conflict", "stable authored id is not unique: " + duplicateId);
					return nullptr;
				}
				if (!failure.empty()) return nullptr;
				if (out.dump().size() > MAXIMUM_RESPONSE_BYTES) {
					failure =
						Error("resource_limit", "temporal sample exceeds the 65536-byte response limit");
					return nullptr;
				}
				return out;
			}
		});
	}

	namespace features {
		// Registers snapshot-bound camera and object pose sampling for a data-factory session.
		inline Feature TemporalSample(world::Universe &universe, world::DataFactorySession &session) {
			return Feature{"temporal_sample", [&universe, &session](Surface &surface) {
							   surface.AddTemporalSampleTools(universe, session);
						   }};
		}
	}
}
