#pragma once

#include <engine/control/features/DataFactory.hpp>

// The data-factory lifecycle tools. The host owns the session; this adapter
// validates MCP input before inspecting or mutating that session.

#include <engine/control/DataFactoryOperationLedger.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/physics/Query.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/world/DataFactory.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace data_factory_detail {
		// Maximum UTF-8 byte length for client-supplied instance and operation ids.
		inline constexpr size_t MAXIMUM_ID = 128;
		// Maximum deterministic action names accepted by one step request.
		inline constexpr size_t MAXIMUM_ACTIONS = 32;

		// Converts a physics occupancy refusal into the stable control error code.
		inline const char *OccupancyReason(physics::ColliderOccupancy::Reason reason) {
			switch (reason) {
			case physics::ColliderOccupancy::Reason::None:
				return "";
			case physics::ColliderOccupancy::Reason::PhysicsUnprepared:
				return "physics_unprepared";
			case physics::ColliderOccupancy::Reason::CandidateOverflow:
				return "candidate_overflow";
			case physics::ColliderOccupancy::Reason::BakedGeometryUncertain:
				return "baked_geometry_uncertain";
			case physics::ColliderOccupancy::Reason::PhysicsStale:
				return "physics_stale";
			case physics::ColliderOccupancy::Reason::InvalidProbe:
				return "invalid_probe";
			}
			return "unknown";
		}

		// Parses exactly three finite JSON numbers into an engine vector.
		inline bool FiniteVector(const json &value, core::Vector3 &out) {
			if (!value.is_array() || value.size() != 3) return false;
			for (const json &part : value)
				if (!part.is_number() || !std::isfinite(part.get<double>())) return false;
			out = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
		}

		// Checks finite nonzero extents so a probe box has no inverted or collapsed axis.
		inline bool StrictFiniteBox(const core::Vector3 &minimum, const core::Vector3 &maximum) {
			const auto validAxis = [](float low, float high) {
				const double centre = (static_cast<double>(low) + static_cast<double>(high)) * 0.5;
				const double extent = (static_cast<double>(high) - static_cast<double>(low)) * 0.5;
				return low < high && std::isfinite(centre) && std::isfinite(extent) &&
					   centre >= -std::numeric_limits<float>::max() &&
					   centre <= std::numeric_limits<float>::max() &&
					   extent <= std::numeric_limits<float>::max();
			};
			return validAxis(minimum.X, maximum.X) && validAxis(minimum.Y, maximum.Y) &&
				   validAxis(minimum.Z, maximum.Z);
		}

		// Validates canonical UTF-8 for occupancy diagnostic text returned to the host.
		inline bool OccupancyUtf8(std::string_view value) {
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

		// Parsed optimistic-concurrency request shared by data-factory tool handlers.
		struct Request {
			// Factory instance selected by the host.
			std::string InstanceId;
			// Idempotency key shared by retries of one host operation.
			std::string OperationId;
			// Checkpoint selected for lifecycle operations.
			std::string CheckpointId;
			// Snapshot selected for fork or restore operations.
			std::string SnapshotId;
			// Requested render-history policy for render-only operations.
			std::string TemporalHistory;
			// Completed tick the host expects before mutation.
			uint64_t Tick = 0;
			// World incarnation the host expects before mutation.
			uint64_t Epoch = 0;
			// World lifecycle version the host expects before mutation.
			uint64_t Version = 0;
			// Tick targeted by a seek or capture operation.
			uint64_t TargetTick = 0;
			// Rational interval used by deterministic step operations.
			world::DataFactoryInterval Interval;
			// Pause scope required before lifecycle operations.
			world::DataFactoryPauseScope Scope = world::DataFactoryPauseScope::AllSystems;
			// Parent snapshot required by a fork operation.
			std::string BaseSnapshotId;
			// Conditional property changes applied by an intervention.
			std::vector<world::DataFactoryIntervention> Changes;
			// Deterministic action names requested for the next step.
			std::vector<world::DataFactoryAction> Actions;
		};

		// Formats a stable machine code and human detail as one tool failure string.
		inline std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

		// Serializes completed world lifecycle state without exposing session pointers.
		inline json Reply(const world::DataFactoryReply &reply) {
			return json{
				{"status", world::Describe(reply.Status)},
				{"detail", reply.Detail},
				{"instance_id", reply.InstanceId},
				{"world_epoch", reply.WorldEpoch},
				{"world_version", reply.WorldVersion},
				{"tick", reply.Clock.Tick},
				{"time_ns", reply.Clock.TimeNanoseconds},
				{"rational_time_available", reply.Clock.RationalTimeAvailable},
				{"dt_ns",
				 {{"numerator", reply.Clock.Interval.NumeratorNanoseconds},
				  {"denominator", reply.Clock.Interval.Denominator}}},
			};
		}

		// Converts an asynchronous render-only reply into submitted or pending host status.
		inline void
		RenderOnlyReply(const world::DataFactoryRenderOnlyReply &reply, json &result, std::string &failure) {
			result = Reply(reply);
			result["operation_id"] = reply.OperationId;
			result["temporal_history"] = "preserve";
			failure.clear();
			if (reply.Status == world::DataFactoryStatus::Pending) {
				result["status"] = "pending";
				return;
			}
			if (reply.Status == world::DataFactoryStatus::Ok) {
				result["status"] = "submitted";
				result["detail"] = "render-only frame command submitted; readback readiness is not tracked";
				return;
			}
			failure = Error(
				world::Describe(reply.Status),
				reply.Detail.empty() ? "render-only operation refused" : reply.Detail
			);
		}

		// Reads one bounded non-NUL identifier string and names validation failures by field.
		inline bool Text(const json &value, std::string_view name, std::string &out, std::string &failure) {
			if (!value.is_string()) {
				failure = Error("validation_failed", std::string(name) + " must be a string");
				return false;
			}
			out = value.get<std::string>();
			if (out.empty() || out.size() > MAXIMUM_ID || out.find('\0') != std::string::npos) {
				failure = Error("validation_failed", std::string(name) + " must contain 1 to 128 bytes");
				return false;
			}
			return true;
		}

		inline bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure) {
			// Conversion to uint64_t accepts negative signed JSON integers. Check
			// the JSON category first so negative values never wrap.
			if (!value.is_number_unsigned()) {
				failure = Error("validation_failed", std::string(name) + " must be an unsigned integer");
				return false;
			}
			out = value.get<uint64_t>();
			return true;
		}

		// Locates a required object field while preserving its JSON value for typed parsing.
		inline bool Field(const json &values, std::string_view name, const json *&out, std::string &failure) {
			const auto found = values.find(std::string(name));
			if (found == values.end()) {
				failure = Error("validation_failed", std::string(name) + " is required");
				return false;
			}
			out = &*found;
			return true;
		}

		// Refuses unrecognised request members so clients cannot silently misspell controls.
		inline bool
		Only(const json &values, std::initializer_list<std::string_view> names, std::string &failure) {
			for (auto field = values.begin(); field != values.end(); ++field) {
				if (std::find(names.begin(), names.end(), std::string_view(field.key())) == names.end()) {
					failure = Error("validation_failed", "unknown argument: " + field.key());
					return false;
				}
			}
			return true;
		}

		// Parses the shared instance identity and optimistic concurrency fence into Request.
		inline bool Base(
			const json &values,
			bool operationRequired,
			Request &request,
			json &normalized,
			std::string &failure
		) {
			if (!values.is_object()) {
				failure = Error("validation_failed", "arguments must be an object");
				return false;
			}
			const json *field = nullptr;
			if (!Field(values, "instance_id", field, failure) ||
				!Text(*field, "instance_id", request.InstanceId, failure))
				return false;
			if (!Field(values, "expected_tick", field, failure) ||
				!UInt(*field, "expected_tick", request.Tick, failure))
				return false;
			if (!Field(values, "expected_world_epoch", field, failure) ||
				!UInt(*field, "expected_world_epoch", request.Epoch, failure))
				return false;
			if (!Field(values, "expected_world_version", field, failure) ||
				!UInt(*field, "expected_world_version", request.Version, failure))
				return false;
			if (const auto operation = values.find("operation_id"); operation != values.end()) {
				if (!Text(*operation, "operation_id", request.OperationId, failure)) return false;
			} else if (operationRequired) {
				failure = Error("validation_failed", "operation_id is required");
				return false;
			}
			normalized = {
				{"instance_id", request.InstanceId},
				{"expected_tick", request.Tick},
				{"expected_world_epoch", request.Epoch},
				{"expected_world_version", request.Version}
			};
			if (!request.OperationId.empty()) normalized["operation_id"] = request.OperationId;
			return true;
		}

		// Parses a positive rational simulation interval in nanoseconds.
		inline bool Interval(const json &values, Request &request, json &normalized, std::string &failure) {
			const json *field = nullptr;
			if (!Field(values, "dt_ns", field, failure) || !field->is_object() ||
				!Only(*field, {"numerator", "denominator"}, failure)) {
				if (failure.empty()) failure = Error("validation_failed", "dt_ns must be an object");
				return false;
			}
			uint64_t denominator = 0;
			if (!Field(*field, "numerator", field, failure) ||
				!UInt(*field, "dt_ns.numerator", request.Interval.NumeratorNanoseconds, failure) ||
				request.Interval.NumeratorNanoseconds == 0) {
				if (failure.empty()) failure = Error("validation_failed", "dt_ns.numerator must be nonzero");
				return false;
			}
			const json *denominatorField = nullptr;
			if (!Field(values.at("dt_ns"), "denominator", denominatorField, failure) ||
				!UInt(*denominatorField, "dt_ns.denominator", denominator, failure) || denominator == 0 ||
				denominator > UINT32_MAX) {
				if (failure.empty())
					failure =
						Error("validation_failed", "dt_ns.denominator must be between 1 and 4294967295");
				return false;
			}
			request.Interval.Denominator = static_cast<uint32_t>(denominator);
			normalized["dt_ns"] = {
				{"numerator", request.Interval.NumeratorNanoseconds},
				{"denominator", request.Interval.Denominator}
			};
			return true;
		}

		// Parses optional bounded deterministic action names for a single step.
		inline bool Actions(const json &values, Request &request, json &normalized, std::string &failure) {
			const auto found = values.find("actions");
			if (found == values.end()) {
				normalized["actions"] = json::array();
				return true;
			}
			if (!found->is_array() || found->size() > world::MAXIMUM_DATA_FACTORY_ACTIONS) {
				failure = Error("validation_failed", "actions must be an array with at most 32 names");
				return false;
			}
			std::vector<world::DataFactoryAction> parsed;
			parsed.reserve(found->size());
			for (const json &action : *found) {
				std::string name;
				if (!Text(action, "actions entry", name, failure)) return false;
				parsed.push_back({std::move(name)});
			}
			normalized["actions"] = *found;
			request.Actions = std::move(parsed);
			return true;
		}

		// Converts a JSON scalar into a typed intervention value without coercion.
		inline bool
		InterventionValue(const json &value, world::DataFactoryInterventionValue &out, std::string &failure) {
			if (value.is_null())
				out.Type = world::DataFactoryInterventionValue::Kind::Missing;
			else if (value.is_boolean()) {
				out.Type = world::DataFactoryInterventionValue::Kind::Boolean;
				out.Boolean = value.get<bool>();
			} else if (value.is_number_integer()) {
				out.Type = world::DataFactoryInterventionValue::Kind::Integer;
				out.Integer = value.get<int64_t>();
			} else if (value.is_number_float() && std::isfinite(value.get<double>())) {
				out.Type = world::DataFactoryInterventionValue::Kind::Number;
				out.Number = value.get<double>();
			} else if (value.is_string()) {
				out.Type = world::DataFactoryInterventionValue::Kind::String;
				if (!Text(value, "intervention value", out.String, failure)) return false;
			} else {
				failure = Error(
					"validation_failed", "intervention values must be null, bool, integer, number, or string"
				);
				return false;
			}
			return true;
		}

		// Verifies the live session still matches the request's tick, epoch, and version fence.
		inline bool
		Preconditions(world::DataFactorySession &session, const Request &request, std::string &failure) {
			const world::DataFactoryReply current = session.Inspect(request.InstanceId);
			if (current.Status != world::DataFactoryStatus::Ok) {
				failure = Error(world::Describe(current.Status), current.Detail);
				return false;
			}
			if (current.Clock.Tick != request.Tick) {
				failure = Error("version_conflict", "expected_tick does not match the completed tick");
				return false;
			}
			if (current.WorldEpoch != request.Epoch) {
				failure = Error("version_conflict", "expected_world_epoch does not match");
				return false;
			}
			if (current.WorldVersion != request.Version) {
				failure = Error("version_conflict", "expected_world_version does not match");
				return false;
			}
			return true;
		}

		// Adds a retirement tombstone only to world lifecycle replies that retired an instance.
		inline json WorldReply(const world::DataFactoryReply &reply, bool retired) {
			json result = Reply(reply);
			if (retired) result["tombstone"] = reply.Tombstone;
			return result;
		}

		// Builds a closed JSON schema from the selected optional and required request fields.
		inline json Schema(
			std::initializer_list<std::string_view> optional, std::initializer_list<std::string_view> required
		) {
			json properties{
				{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
				{"target_tick", {{"type", "integer"}, {"minimum", 0}}},
				{"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
				{"expected_world_version", {{"type", "integer"}, {"minimum", 0}}},
				{"operation_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"seed", {{"type", "integer"}, {"minimum", 0}}},
				{"tick_rate", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000}}},
				{"checkpoint_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"branch_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"temporal_history", {{"type", "string"}, {"enum", {"preserve"}}}},
				{"base_snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"changed_causes",
				 {{"type", "array"},
				  {"minItems", 1},
				  {"maxItems", MAXIMUM_ACTIONS},
				  {"items",
				   {{"type", "object"},
					{"additionalProperties", false},
					{"properties",
					 {{"target_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"expected", {{"type", {"null", "boolean", "integer", "number", "string"}}}},
					  {"value", {{"type", {"null", "boolean", "integer", "number", "string"}}}}}},
					{"required", {"target_id", "path", "expected", "value"}}}}}},
				{"scope", {{"type", "string"}, {"enum", {"all_systems", "physics_only"}}}},
				{"dt_ns",
				 {{"type", "object"},
				  {"additionalProperties", false},
				  {"properties",
				   {{"numerator", {{"type", "integer"}, {"minimum", 1}}},
					{"denominator", {{"type", "integer"}, {"minimum", 1}, {"maximum", UINT32_MAX}}}}},
				  {"required", {"numerator", "denominator"}}}},
				{"actions",
				 {{"type", "array"},
				  {"maxItems", world::MAXIMUM_DATA_FACTORY_ACTIONS},
				  {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}}}},
			};
			json selected = json::object();
			for (const std::string_view name : optional)
				selected[std::string(name)] = properties.at(std::string(name));
			json needs = json::array();
			for (const std::string_view name : required)
				needs.push_back(name);
			return {
				{"type", "object"},
				{"additionalProperties", false},
				{"properties", std::move(selected)},
				{"required", std::move(needs)}
			};
		}
	}

}
