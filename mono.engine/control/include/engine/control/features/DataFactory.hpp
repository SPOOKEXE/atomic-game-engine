#pragma once

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
		inline constexpr size_t MAXIMUM_ID = 128;
		inline constexpr size_t MAXIMUM_ACTIONS = 32;

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

		inline bool FiniteVector(const json &value, core::Vector3 &out) {
			if (!value.is_array() || value.size() != 3) return false;
			for (const json &part : value)
				if (!part.is_number() || !std::isfinite(part.get<double>())) return false;
			out = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
			return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
		}

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

		struct Request {
			std::string InstanceId;
			std::string OperationId;
			std::string CheckpointId;
			std::string SnapshotId;
			std::string TemporalHistory;
			uint64_t Tick = 0;
			uint64_t Epoch = 0;
			uint64_t Version = 0;
			world::DataFactoryInterval Interval;
			world::DataFactoryPauseScope Scope = world::DataFactoryPauseScope::AllSystems;
			std::string BaseSnapshotId;
			std::vector<world::DataFactoryIntervention> Changes;
			std::vector<world::DataFactoryAction> Actions;
		};

		inline std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

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

		inline bool Field(const json &values, std::string_view name, const json *&out, std::string &failure) {
			const auto found = values.find(std::string(name));
			if (found == values.end()) {
				failure = Error("validation_failed", std::string(name) + " is required");
				return false;
			}
			out = &*found;
			return true;
		}

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

		inline json WorldReply(const world::DataFactoryReply &reply, bool retired) {
			json result = Reply(reply);
			if (retired) result["tombstone"] = reply.Tombstone;
			return result;
		}

		inline json Schema(
			std::initializer_list<std::string_view> optional, std::initializer_list<std::string_view> required
		) {
			json properties{
				{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
				{"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
				{"expected_world_version", {{"type", "integer"}, {"minimum", 0}}},
				{"operation_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
				{"seed", {{"type", "integer"}, {"minimum", 0}}},
				{"tick_rate", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1000}}},
				{"checkpoint_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
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

	inline void Surface::AddDataFactoryTools(world::DataFactorySession &session) {
		using namespace data_factory_detail;
		auto ledger = DataFactoryOperations();
		auto invoke = [&session, ledger](
						  std::string_view tool,
						  const json &values,
						  std::string &failure,
						  bool requiredOperation,
						  const auto &parse,
						  const auto &call
					  ) -> json {
			Request request;
			json normalized;
			if (!Base(values, requiredOperation, request, normalized, failure) ||
				!parse(values, request, normalized, failure))
				return nullptr;
			normalized["tool"] = tool;
			if (!request.OperationId.empty()) {
				json replay;
				const auto prior =
					ledger->Replay(tool, request.OperationId, normalized.dump(), replay, failure);
				if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
				if (prior == DataFactoryOperationReplay::Replay) return replay;
			}
			if (!Preconditions(session, request, failure)) return nullptr;
			const world::DataFactoryReply reply = call(request);
			json result = Reply(reply);
			if (reply.Status != world::DataFactoryStatus::Ok)
				failure = Error(
					world::Describe(reply.Status), reply.Detail.empty() ? "operation refused" : reply.Detail
				);
			if (!request.OperationId.empty())
				ledger->Store(std::string(tool), request.OperationId, normalized.dump(), result, failure);
			return result;
		};
		auto worldTool = [&session, ledger](
							 std::string name,
							 std::string description,
							 bool revisionRequired,
							 bool settingsRequired,
							 bool retired,
							 auto operation
						 ) {
			const std::string toolName = name;
			return Tool{
				std::move(name),
				std::move(description),
				[revisionRequired, settingsRequired] {
					if (!settingsRequired)
						return Schema(
							{"instance_id",
							 "expected_tick",
							 "expected_world_epoch",
							 "expected_world_version",
							 "operation_id"},
							{"instance_id",
							 "expected_tick",
							 "expected_world_epoch",
							 "expected_world_version",
							 "operation_id"}
						);
					return revisionRequired ? Schema(
												  {"instance_id",
												   "seed",
												   "tick_rate",
												   "expected_tick",
												   "expected_world_epoch",
												   "expected_world_version",
												   "operation_id"},
												  {"instance_id",
												   "seed",
												   "tick_rate",
												   "expected_tick",
												   "expected_world_epoch",
												   "expected_world_version",
												   "operation_id"}
											  )
											: Schema(
												  {"instance_id", "seed", "tick_rate", "operation_id"},
												  {"instance_id", "seed", "tick_rate", "operation_id"}
											  );
				},
				[&session, ledger, toolName, revisionRequired, settingsRequired, retired, operation](
					const json &values, std::string &failure
				) -> json {
					if (!values.is_object()) {
						failure = Error("validation_failed", "arguments must be an object");
						return nullptr;
					}
					const auto allowed =
						settingsRequired
							? std::initializer_list<
								  std::
									  string_view>{"instance_id", "seed", "tick_rate", "expected_tick", "expected_world_epoch", "expected_world_version", "operation_id"}
							: std::initializer_list<std::string_view>{
								  "instance_id",
								  "expected_tick",
								  "expected_world_epoch",
								  "expected_world_version",
								  "operation_id"
							  };
					if (!Only(values, allowed, failure)) return nullptr;
					world::DataFactoryWorldRequest request;
					const json *field = nullptr;
					if (!Field(values, "instance_id", field, failure) ||
						!Text(*field, "instance_id", request.InstanceId, failure) ||
						(settingsRequired &&
						 (!Field(values, "seed", field, failure) ||
						  !UInt(*field, "seed", request.Seed, failure) ||
						  !Field(values, "tick_rate", field, failure) || !field->is_number() ||
						  !std::isfinite(request.TickRate = field->get<double>()) ||
						  request.TickRate <= 0.0 || request.TickRate > 1000.0)) ||
						!Field(values, "operation_id", field, failure) ||
						!Text(*field, "operation_id", request.OperationId, failure)) {
						if (failure.empty())
							failure =
								Error("validation_failed", "tick_rate must be finite and between 0 and 1000");
						return nullptr;
					}
					if (revisionRequired &&
						(!Field(values, "expected_world_epoch", field, failure) ||
						 !UInt(*field, "expected_world_epoch", request.ExpectedWorldEpoch, failure) ||
						 !Field(values, "expected_tick", field, failure) ||
						 !UInt(*field, "expected_tick", request.ExpectedTick, failure) ||
						 !Field(values, "expected_world_version", field, failure) ||
						 !UInt(*field, "expected_world_version", request.ExpectedWorldVersion, failure)))
						return nullptr;
					json normalized{
						{"tool", toolName},
						{"instance_id", request.InstanceId},
						{"operation_id", request.OperationId}
					};
					if (settingsRequired) {
						normalized["seed"] = request.Seed;
						normalized["tick_rate"] = request.TickRate;
					}
					if (revisionRequired) {
						normalized["expected_tick"] = request.ExpectedTick;
						normalized["expected_world_epoch"] = request.ExpectedWorldEpoch;
						normalized["expected_world_version"] = request.ExpectedWorldVersion;
					}
					json replay;
					const auto prior =
						ledger->Replay(toolName, request.OperationId, normalized.dump(), replay, failure);
					if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
					if (prior == DataFactoryOperationReplay::Replay) return replay;
					const auto reply = (session.*operation)(request);
					json result = WorldReply(reply, retired);
					if (reply.Status != world::DataFactoryStatus::Ok)
						failure = Error(world::Describe(reply.Status), reply.Detail);
					ledger->Store(toolName, request.OperationId, normalized.dump(), result, failure);
					return result;
				}
			};
		};
		Add(worldTool(
			"world_create",
			"Creates one factory-owned local world in this Universe.",
			false,
			true,
			false,
			&world::DataFactorySession::CreateWorld
		));
		Add(worldTool(
			"world_reset",
			"Resets one factory-owned local world after an all-systems pause.",
			true,
			true,
			false,
			&world::DataFactorySession::ResetWorld
		));
		Add(worldTool(
			"world_retire",
			"Retires one factory-owned local world after an all-systems pause and returns a tombstone.",
			true,
			false,
			true,
			&world::DataFactorySession::RetireWorld
		));

		Add(Tool{
			"lifecycle_inspect",
			"Reads one data-factory world's completed tick and lifecycle revision without changing it.",
			[] { return Schema({"instance_id"}, {"instance_id"}); },
			[&session](const json &values, std::string &failure) -> json {
				if (!values.is_object() || !Only(values, {"instance_id"}, failure)) {
					if (failure.empty()) failure = Error("validation_failed", "arguments must be an object");
					return nullptr;
				}
				const json *instance = nullptr;
				std::string instanceId;
				if (!Field(values, "instance_id", instance, failure) ||
					!Text(*instance, "instance_id", instanceId, failure))
					return nullptr;
				const world::DataFactoryReply reply = session.Inspect(instanceId);
				if (reply.Status != world::DataFactoryStatus::Ok) {
					failure = Error(world::Describe(reply.Status), reply.Detail);
					return nullptr;
				}
				return Reply(reply);
			}
		});
		Add(Tool{
			"data_factory_operation_audit",
			"Reads the bounded, redacted replay ledger shared by lifecycle and capture mutation tools. "
			"Rows are newest first and contain no raw scene arguments.",
			[] {
				return json{
					{"type", "object"},
					{"additionalProperties", false},
					{"properties", {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}}}
				};
			},
			[ledger](const json &values, std::string &failure) -> json {
				if (!values.is_object() || !Only(values, {"limit"}, failure)) {
					if (failure.empty()) failure = Error("validation_failed", "arguments must be an object");
					return nullptr;
				}
				size_t limit = 32;
				if (const auto found = values.find("limit"); found != values.end()) {
					uint64_t requested = 0;
					if (!UInt(*found, "limit", requested, failure) || requested == 0 ||
						requested > DataFactoryOperationLedger::MAXIMUM_ENTRIES) {
						if (failure.empty())
							failure = Error("validation_failed", "limit must be between 1 and 256");
						return nullptr;
					}
					limit = static_cast<size_t>(requested);
				}
				json rows = json::array();
				for (const DataFactoryOperationAudit &row : ledger->Recent(limit)) {
					rows.push_back(
						{{"tool", row.Tool},
						 {"operation_id", row.OperationId},
						 {"status", row.Status},
						 {"refused", row.Refused}}
					);
				}
				return {{"maximum_entries", DataFactoryOperationLedger::MAXIMUM_ENTRIES}, {"entries", rows}};
			}
		});

		Add(Tool{
			"pause",
			"Pauses one data-factory world at a completed tick boundary.",
			[] {
				return Schema(
					{"instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id",
					 "scope"},
					{"instance_id", "expected_tick", "expected_world_epoch", "expected_world_version"}
				);
			},
			[invoke, &session](const json &v, std::string &f) {
				return invoke(
					"pause",
					v,
					f,
					false,
					[](const json &v, Request &r, json &n, std::string &f) {
						if (!Only(
								v,
								{"instance_id",
								 "expected_tick",
								 "expected_world_epoch",
								 "expected_world_version",
								 "operation_id",
								 "scope"},
								f
							))
							return false;
						if (const auto it = v.find("scope"); it != v.end()) {
							std::string scope;
							if (!Text(*it, "scope", scope, f)) return false;
							if (scope == "physics_only")
								r.Scope = world::DataFactoryPauseScope::PhysicsOnly;
							else if (scope != "all_systems") {
								f = Error("validation_failed", "scope must be all_systems or physics_only");
								return false;
							}
						}
						n["scope"] = r.Scope == world::DataFactoryPauseScope::PhysicsOnly ? "physics_only"
																						  : "all_systems";
						return true;
					},
					[&session](const Request &r) { return session.Pause(r.InstanceId, r.Scope, r.Tick); }
				);
			}
		});
		Add(Tool{
			"resume",
			"Resumes one data-factory world at a completed tick boundary.",
			[] {
				return Schema(
					{"instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id"},
					{"instance_id", "expected_tick", "expected_world_epoch", "expected_world_version"}
				);
			},
			[invoke, &session](const json &v, std::string &f) {
				return invoke(
					"resume",
					v,
					f,
					false,
					[](const json &v, Request &, json &, std::string &f) {
						return Only(
							v,
							{"instance_id",
							 "expected_tick",
							 "expected_world_epoch",
							 "expected_world_version",
							 "operation_id"},
							f
						);
					},
					[&session](const Request &r) { return session.Resume(r.InstanceId, r.Tick); }
				);
			}
		});
		Add(Tool{
			"step",
			"Advances one all-systems-paused world through its canonical dt_ns interval. Nonempty actions "
			"are unsupported until the host installs an executor.",
			[] {
				return Schema(
					{"instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id",
					 "dt_ns",
					 "actions"},
					{"instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id",
					 "dt_ns"}
				);
			},
			[invoke, &session](const json &v, std::string &f) {
				return invoke(
					"step",
					v,
					f,
					true,
					[](const json &v, Request &r, json &n, std::string &f) {
						return Only(
								   v,
								   {"instance_id",
									"expected_tick",
									"expected_world_epoch",
									"expected_world_version",
									"operation_id",
									"dt_ns",
									"actions"},
								   f
							   ) &&
							   Interval(v, r, n, f) && Actions(v, r, n, f);
					},
					[&session](const Request &r) {
						return session.Step(r.InstanceId, r.Interval, r.Tick, r.Version, r.Actions);
					}
				);
			}
		});

		auto capture = [&session, ledger](const char *name, const char *description, bool checkpoint) {
			return Tool{
				name,
				description,
				[] {
					return Schema(
						{"instance_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version",
						 "operation_id"},
						{"instance_id", "expected_tick", "expected_world_epoch", "expected_world_version"}
					);
				},
				[&session, ledger, checkpoint](const json &v, std::string &f) -> json {
					Request request;
					json normalized;
					if (!Base(v, false, request, normalized, f) || !Only(
																	   v,
																	   {"instance_id",
																		"expected_tick",
																		"expected_world_epoch",
																		"expected_world_version",
																		"operation_id"},
																	   f
																   ))
						return nullptr;
					normalized["tool"] = checkpoint ? "checkpoint" : "snapshot";
					if (!request.OperationId.empty()) {
						json replay;
						const auto prior = ledger->Replay(
							normalized["tool"].get<std::string>(),
							request.OperationId,
							normalized.dump(),
							replay,
							f
						);
						if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
						if (prior == DataFactoryOperationReplay::Replay) return replay;
					}
					if (!Preconditions(session, request, f)) return nullptr;
					std::string id;
					const world::DataFactoryReply reply = checkpoint
															  ? session.Checkpoint(request.InstanceId, id)
															  : session.Snapshot(request.InstanceId, id);
					json result = Reply(reply);
					if (reply.Status == world::DataFactoryStatus::Ok)
						result[checkpoint ? "checkpoint_id" : "snapshot_id"] = id;
					else
						f = Error(
							world::Describe(reply.Status),
							reply.Detail.empty() ? "operation refused" : reply.Detail
						);
					if (!request.OperationId.empty())
						ledger->Store(
							normalized["tool"].get<std::string>(),
							request.OperationId,
							normalized.dump(),
							result,
							f
						);
					return result;
				}
			};
		};
		Add(capture("snapshot", "Retains one immutable world snapshot at a completed boundary.", false));
		Add(
			capture("checkpoint", "Retains a restorable checkpoint when the host provides rehydration.", true)
		);
		Add(Tool{
			"render_only",
			"Queues one static frame from a paused snapshot. Submitted means the frame command was accepted, "
			"not that GPU readback is ready.",
			[] {
				return Schema(
					{"instance_id",
					 "snapshot_id",
					 "temporal_history",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id"},
					{"instance_id",
					 "snapshot_id",
					 "temporal_history",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id"}
				);
			},
			[&session, ledger](const json &v, std::string &f) -> json {
				Request request;
				json normalized;
				if (!Base(v, true, request, normalized, f) || !Only(
																  v,
																  {"instance_id",
																   "snapshot_id",
																   "temporal_history",
																   "expected_tick",
																   "expected_world_epoch",
																   "expected_world_version",
																   "operation_id"},
																  f
															  ))
					return nullptr;
				const json *field = nullptr;
				if (!Field(v, "snapshot_id", field, f) ||
					!Text(*field, "snapshot_id", request.SnapshotId, f) ||
					!Field(v, "temporal_history", field, f) ||
					!Text(*field, "temporal_history", request.TemporalHistory, f))
					return nullptr;
				if (request.TemporalHistory != "preserve") {
					f = Error("capability_unsupported", "this host supports only preserve temporal history");
					return nullptr;
				}
				normalized["tool"] = "render_only";
				normalized["snapshot_id"] = request.SnapshotId;
				normalized["temporal_history"] = request.TemporalHistory;
				json replay;
				const auto prior =
					ledger->Replay("render_only", request.OperationId, normalized.dump(), replay, f);
				if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
				if (prior == DataFactoryOperationReplay::Replay) {
					if (replay.value("status", "") == "pending" && replay.contains("operation_id") &&
						replay["operation_id"].is_number_unsigned()) {
						RenderOnlyReply(
							session.PollRenderOnly(
								request.InstanceId, replay["operation_id"].get<uint64_t>()
							),
							replay,
							f
						);
						ledger->Update(request.OperationId, replay, f);
					}
					return replay;
				}
				if (!Preconditions(session, request, f)) return nullptr;
				const world::DataFactoryRenderOnlyReply reply = session.RenderOnly({
					.InstanceId = request.InstanceId,
					.SnapshotId = request.SnapshotId,
					.ExpectedWorldEpoch = request.Epoch,
					.ExpectedWorldVersion = request.Version,
					.ExpectedTick = request.Tick,
					.TemporalHistory = world::DataFactoryTemporalHistory::Preserve,
				});
				json result;
				RenderOnlyReply(reply, result, f);
				ledger->Store("render_only", request.OperationId, normalized.dump(), result, f);
				return result;
			}
		});
		Add(Tool{
			"poll_render_only",
			"Reads pending, submitted, or failed render-only command status. Submitted does not claim GPU "
			"readback readiness.",
			[] {
				return json{
					{"type", "object"},
					{"additionalProperties", false},
					{"properties",
					 {{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"operation_id", {{"type", "integer"}, {"minimum", 1}}}}},
					{"required", {"instance_id", "operation_id"}}
				};
			},
			[&session](const json &v, std::string &f) -> json {
				if (!Only(v, {"instance_id", "operation_id"}, f)) return nullptr;
				const json *field = nullptr;
				std::string instance;
				uint64_t operationId = 0;
				if (!Field(v, "instance_id", field, f) || !Text(*field, "instance_id", instance, f) ||
					!Field(v, "operation_id", field, f) || !UInt(*field, "operation_id", operationId, f) ||
					operationId == 0) {
					if (f.empty()) f = Error("validation_failed", "operation_id must be positive");
					return nullptr;
				}
				const world::DataFactoryRenderOnlyReply reply = session.PollRenderOnly(instance, operationId);
				json result;
				RenderOnlyReply(reply, result, f);
				return result;
			}
		});
		Add(Tool{
			"restore",
			"Restores a compatible checkpoint through a scratch universe and starts a fresh epoch.",
			[] {
				return Schema(
					{"instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id",
					 "checkpoint_id"},
					{"instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "checkpoint_id"}
				);
			},
			[invoke, &session](const json &v, std::string &f) {
				return invoke(
					"restore",
					v,
					f,
					false,
					[](const json &v, Request &r, json &n, std::string &f) {
						if (!Only(
								v,
								{"instance_id",
								 "expected_tick",
								 "expected_world_epoch",
								 "expected_world_version",
								 "operation_id",
								 "checkpoint_id"},
								f
							))
							return false;
						const json *field = nullptr;
						if (!Field(v, "checkpoint_id", field, f) ||
							!Text(*field, "checkpoint_id", r.CheckpointId, f))
							return false;
						n["checkpoint_id"] = r.CheckpointId;
						return true;
					},
					[&session](const Request &r) { return session.Restore(r.InstanceId, r.CheckpointId); }
				);
			}
		});
		if (session.SupportsIntervention())
			Add(Tool{
				"apply_intervention",
				"Atomically applies host-owned stable-id property edits against one current paused snapshot.",
				[] {
					return Schema(
						{"instance_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version",
						 "operation_id",
						 "base_snapshot_id",
						 "changed_causes"},
						{"instance_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version",
						 "operation_id",
						 "base_snapshot_id",
						 "changed_causes"}
					);
				},
				[invoke, &session](const json &v, std::string &f) {
					return invoke(
						"apply_intervention",
						v,
						f,
						true,
						[](const json &v, Request &r, json &n, std::string &f) {
							if (!Only(
									v,
									{"instance_id",
									 "expected_tick",
									 "expected_world_epoch",
									 "expected_world_version",
									 "operation_id",
									 "base_snapshot_id",
									 "changed_causes"},
									f
								))
								return false;
							const json *field = nullptr;
							if (!Field(v, "base_snapshot_id", field, f) ||
								!Text(*field, "base_snapshot_id", r.BaseSnapshotId, f))
								return false;
							if (!Field(v, "changed_causes", field, f) || !field->is_array() ||
								field->empty() || field->size() > MAXIMUM_ACTIONS) {
								f = Error("validation_failed", "changed_causes must contain 1 to 32 edits");
								return false;
							}
							for (const json &row : *field) {
								if (!Only(row, {"target_id", "path", "expected", "value"}, f)) return false;
								const json *target = nullptr, *path = nullptr, *expected = nullptr,
										   *value = nullptr;
								world::DataFactoryIntervention edit;
								if (!Field(row, "target_id", target, f) ||
									!Text(*target, "target_id", edit.TargetId, f) ||
									!Field(row, "path", path, f) || !Text(*path, "path", edit.Path, f) ||
									!Field(row, "expected", expected, f) ||
									!InterventionValue(*expected, edit.Expected, f) ||
									!Field(row, "value", value, f) ||
									!InterventionValue(*value, edit.Value, f))
									return false;
								r.Changes.push_back(std::move(edit));
							}
							n["base_snapshot_id"] = r.BaseSnapshotId;
							n["changed_causes"] = *field;
							return true;
						},
						[&session](const Request &r) {
							return session.ApplyIntervention(
								r.InstanceId, r.BaseSnapshotId, r.Changes, r.Tick, r.Version
							);
						}
					);
				}
			});

		Add(Tool{
			"get_collider_bev",
			"Returns a snapshot-bound bird's-eye collider-contact grid. Rows are z-major from minimum Z to "
			"maximum Z; columns then run from minimum X to maximum X. Each cell is occupied, empty, or "
			"explicitly unknown when physics cannot prove a negative answer.",
			[] {
				const json xz{
					{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}
				};
				const json lifecycle{
					{"type", "object"},
					{"additionalProperties", false},
					{"properties",
					 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
					  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
					  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
					{"required", {"tick", "world_epoch", "world_version"}}
				};
				return json{
					{"type", "object"},
					{"additionalProperties", false},
					{"properties",
					 {{"schema_version", {{"const", "collider-bev/v1"}}},
					  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"lifecycle", lifecycle},
					  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"xz_bounds_metres",
					   {{"type", "object"},
						{"additionalProperties", false},
						{"properties", {{"minimum", xz}, {"maximum", xz}}},
						{"required", {"minimum", "maximum"}}}},
					  {"y_minimum_metres", {{"type", "number"}}},
					  {"y_maximum_metres", {{"type", "number"}}},
					  {"rows", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}},
					  {"columns", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}}}},
					{"required",
					 {"schema_version",
					  "world_id",
					  "lifecycle",
					  "snapshot_id",
					  "xz_bounds_metres",
					  "y_minimum_metres",
					  "y_maximum_metres",
					  "rows",
					  "columns"}}
				};
			},
			[&session](const json &arguments, std::string &failure) -> json {
				using namespace data_factory_detail;
				if (!arguments.is_object() || !Only(
												  arguments,
												  {"schema_version",
												   "world_id",
												   "lifecycle",
												   "snapshot_id",
												   "xz_bounds_metres",
												   "y_minimum_metres",
												   "y_maximum_metres",
												   "rows",
												   "columns"},
												  failure
											  ))
					return nullptr;
				Request request;
				const json *field = nullptr;
				if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
					field->get<std::string>() != "collider-bev/v1" ||
					!Field(arguments, "world_id", field, failure) ||
					!Text(*field, "world_id", request.InstanceId, failure) ||
					!Field(arguments, "snapshot_id", field, failure) ||
					!Text(*field, "snapshot_id", request.SnapshotId, failure)) {
					if (failure.empty())
						failure = Error("validation_failed", "schema_version must be collider-bev/v1");
					return nullptr;
				}
				const auto lifecycle = arguments.find("lifecycle");
				if (lifecycle == arguments.end() || !lifecycle->is_object() ||
					!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
					!Field(*lifecycle, "tick", field, failure) ||
					!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
					!Field(*lifecycle, "world_epoch", field, failure) ||
					!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
					!Field(*lifecycle, "world_version", field, failure) ||
					!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
					if (failure.empty())
						failure = Error(
							"validation_failed", "lifecycle requires tick, world_epoch and world_version"
						);
					return nullptr;
				}
				const auto finite = [](const json &value, float &out) {
					if (!value.is_number() || !std::isfinite(value.get<double>()) ||
						value.get<double>() < -std::numeric_limits<float>::max() ||
						value.get<double>() > std::numeric_limits<float>::max())
						return false;
					out = value.get<float>();
					return std::isfinite(out);
				};
				const json *bounds = nullptr;
				if (!Field(arguments, "xz_bounds_metres", bounds, failure) || !bounds->is_object() ||
					!Only(*bounds, {"minimum", "maximum"}, failure) || !bounds->contains("minimum") ||
					!bounds->contains("maximum") || !bounds->at("minimum").is_array() ||
					!bounds->at("maximum").is_array() || bounds->at("minimum").size() != 2 ||
					bounds->at("maximum").size() != 2) {
					if (failure.empty())
						failure = Error(
							"validation_failed", "xz_bounds_metres needs finite [x,z] minimum and maximum"
						);
					return nullptr;
				}
				script::DataSceneColliderBevRequest bev;
				if (!finite(bounds->at("minimum")[0], bev.MinimumXMetres) ||
					!finite(bounds->at("minimum")[1], bev.MinimumZMetres) ||
					!finite(bounds->at("maximum")[0], bev.MaximumXMetres) ||
					!finite(bounds->at("maximum")[1], bev.MaximumZMetres) ||
					!Field(arguments, "y_minimum_metres", field, failure) ||
					!finite(*field, bev.MinimumYMetres) ||
					!Field(arguments, "y_maximum_metres", field, failure) ||
					!finite(*field, bev.MaximumYMetres) || !Field(arguments, "rows", field, failure) ||
					!field->is_number_unsigned() || field->get<uint64_t>() > 8 ||
					!Field(arguments, "columns", field, failure) || !field->is_number_unsigned() ||
					field->get<uint64_t>() > 8) {
					if (failure.empty())
						failure = Error(
							"validation_failed",
							"BEV bounds must be finite and dimensions must be 1 through 8"
						);
					return nullptr;
				}
				bev.Rows = arguments.at("rows").get<uint8_t>();
				bev.Columns = arguments.at("columns").get<uint8_t>();
				if (bev.Rows == 0 || bev.Columns == 0 || bev.MinimumXMetres >= bev.MaximumXMetres ||
					bev.MinimumZMetres >= bev.MaximumZMetres || bev.MinimumYMetres >= bev.MaximumYMetres) {
					failure = Error("validation_failed", "BEV bounds and Y slab must be strictly increasing");
					return nullptr;
				}
				const auto hasExtent = [](float minimum, float maximum) {
					return static_cast<float>((static_cast<double>(maximum) - minimum) * 0.5) > 0.0f;
				};
				const auto boundary = [](float minimum, float maximum, uint8_t index, uint8_t count) {
					if (index == 0) return minimum;
					if (index == count) return maximum;
					return static_cast<float>(
						static_cast<double>(minimum) +
						(static_cast<double>(maximum) - minimum) * static_cast<double>(index) / count
					);
				};
				if (!hasExtent(bev.MinimumYMetres, bev.MaximumYMetres)) {
					failure = Error("validation_failed", "Y slab half extent rounds to zero");
					return nullptr;
				}
				for (uint8_t row = 0; row < bev.Rows; ++row) {
					const float minimumZ = boundary(bev.MinimumZMetres, bev.MaximumZMetres, row, bev.Rows);
					const float maximumZ =
						boundary(bev.MinimumZMetres, bev.MaximumZMetres, row + 1, bev.Rows);
					for (uint8_t column = 0; column < bev.Columns; ++column) {
						const float minimumX =
							boundary(bev.MinimumXMetres, bev.MaximumXMetres, column, bev.Columns);
						const float maximumX =
							boundary(bev.MinimumXMetres, bev.MaximumXMetres, column + 1, bev.Columns);
						if (!hasExtent(minimumX, maximumX) || !hasExtent(minimumZ, maximumZ)) {
							failure = Error("validation_failed", "a BEV cell half extent rounds to zero");
							return nullptr;
						}
					}
				}
				if (!session.OwnsWorld(request.InstanceId)) {
					failure =
						Error("validation_failed", "world_id is not owned by this data-factory session");
					return nullptr;
				}
				if (!Preconditions(session, request, failure)) return nullptr;
				const world::DataFactoryReply barrier =
					session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
				if (barrier.Status != world::DataFactoryStatus::Ok) {
					failure = Error(world::Describe(barrier.Status), barrier.Detail);
					return nullptr;
				}
				json result;
				const world::WorldStatus entered = session.UniverseOf().Enter(
					session.UniverseOf().Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
						result = data_scene_detail::Result(script::ColliderBev(store, bev), failure);
					}
				);
				if (entered != world::WorldStatus::Ok && failure.empty())
					failure = Error("validation_failed", "scene is unavailable");
				if (!failure.empty()) return nullptr;
				result["world_id"] = barrier.InstanceId;
				result["lifecycle"] = {
					{"tick", barrier.Clock.Tick},
					{"world_epoch", barrier.WorldEpoch},
					{"world_version", barrier.WorldVersion}
				};
				result["snapshot_id"] = request.SnapshotId;
				return result;
			}
		});

		Add(Tool{
			"get_collider_occupancy",
			"Tests up to 32 named finite world-space AABBs against the exact completed, retained "
			"all-systems-paused snapshot. Boundary contact counts as collider contact. This is not a "
			"filled-volume test: unavailable physics, broad-phase overflow, and mesh or hull candidates "
			"remain explicitly incomplete.",
			[] {
				const json vector{
					{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}
				};
				const json lifecycle{
					{"type", "object"},
					{"additionalProperties", false},
					{"properties",
					 {{"tick", {{"type", "integer"}, {"minimum", 0}}},
					  {"world_epoch", {{"type", "integer"}, {"minimum", 0}}},
					  {"world_version", {{"type", "integer"}, {"minimum", 0}}}}},
					{"required", {"tick", "world_epoch", "world_version"}}
				};
				return json{
					{"type", "object"},
					{"properties",
					 {{"schema_version", {{"const", "collider-occupancy/v1"}}},
					  {"world_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"lifecycle", lifecycle},
					  {"snapshot_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
					  {"probes",
					   {{"type", "array"},
						{"minItems", 1},
						{"maxItems", 32},
						{"items",
						 {{"type", "object"},
						  {"additionalProperties", false},
						  {"properties",
						   {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
							{"minimum_metres", vector},
							{"maximum_metres", vector}}},
						  {"required", {"name", "minimum_metres", "maximum_metres"}}}}}}}},
					{"required", {"schema_version", "world_id", "lifecycle", "snapshot_id", "probes"}},
					{"additionalProperties", false}
				};
			},
			[&session](const json &arguments, std::string &failure) -> json {
				using namespace data_factory_detail;
				if (!arguments.is_object() ||
					!Only(
						arguments,
						{"schema_version", "world_id", "lifecycle", "snapshot_id", "probes"},
						failure
					)) {
					if (failure.empty()) failure = Error("validation_failed", "arguments must be an object");
					return nullptr;
				}
				Request request;
				const json *field = nullptr;
				if (!Field(arguments, "schema_version", field, failure) || !field->is_string() ||
					field->get<std::string>() != "collider-occupancy/v1" ||
					!Field(arguments, "world_id", field, failure) ||
					!Text(*field, "world_id", request.InstanceId, failure)) {
					if (failure.empty())
						failure = Error("validation_failed", "schema_version must be collider-occupancy/v1");
					return nullptr;
				}
				const auto lifecycle = arguments.find("lifecycle");
				if (lifecycle == arguments.end() || !lifecycle->is_object() ||
					!Only(*lifecycle, {"tick", "world_epoch", "world_version"}, failure) ||
					!Field(*lifecycle, "tick", field, failure) ||
					!UInt(*field, "lifecycle.tick", request.Tick, failure) ||
					!Field(*lifecycle, "world_epoch", field, failure) ||
					!UInt(*field, "lifecycle.world_epoch", request.Epoch, failure) ||
					!Field(*lifecycle, "world_version", field, failure) ||
					!UInt(*field, "lifecycle.world_version", request.Version, failure)) {
					if (failure.empty())
						failure = Error(
							"validation_failed", "lifecycle requires tick, world_epoch and world_version"
						);
					return nullptr;
				}
				const json *snapshot = nullptr;
				if (!Field(arguments, "snapshot_id", snapshot, failure) ||
					!Text(*snapshot, "snapshot_id", request.SnapshotId, failure))
					return nullptr;
				const auto probesField = arguments.find("probes");
				if (probesField == arguments.end() || !probesField->is_array() || probesField->empty() ||
					probesField->size() > 32) {
					failure = Error("validation_failed", "probes must contain 1 through 32 named AABBs");
					return nullptr;
				}
				std::array<core::AABB, 32> probes;
				std::array<std::string, 32> names;
				std::unordered_set<std::string> uniqueNames;
				for (size_t index = 0; index < probesField->size(); ++index) {
					const json &probe = (*probesField)[index];
					if (!probe.is_object() ||
						!Only(probe, {"name", "minimum_metres", "maximum_metres"}, failure) ||
						!probe.contains("name") ||
						!Text(probe.at("name"), "probe name", names[index], failure) ||
						!uniqueNames.emplace(names[index]).second) {
						if (failure.empty())
							failure = Error("validation_failed", "probe names must be unique");
						return nullptr;
					}
					core::Vector3 minimum, maximum;
					if (!probe.contains("minimum_metres") || !probe.contains("maximum_metres") ||
						!FiniteVector(probe.at("minimum_metres"), minimum) ||
						!FiniteVector(probe.at("maximum_metres"), maximum) ||
						!StrictFiniteBox(minimum, maximum)) {
						failure = Error(
							"validation_failed", "each probe needs finite strict minimum and maximum vectors"
						);
						return nullptr;
					}
					probes[index] = {minimum, maximum};
				}
				if (!Preconditions(session, request, failure)) return nullptr;
				const world::DataFactoryReply barrier =
					session.RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
				if (barrier.Status != world::DataFactoryStatus::Ok) {
					failure = Error(world::Describe(barrier.Status), barrier.Detail);
					return nullptr;
				}
				std::array<physics::ColliderOccupancy, 32> occupancy;
				json answers = json::array();
				world::Universe &universe = session.UniverseOf();
				const world::WorldStatus entered =
					universe.Enter(universe.Find(core::Name(barrier.InstanceId)), [&](ecs::Store &store) {
						physics::ColliderOccupancyBatch(
							store,
							std::span{probes}.first(probesField->size()),
							std::span{occupancy}.first(probesField->size())
						);
						for (size_t index = 0; index < probesField->size(); ++index) {
							const physics::ColliderOccupancy &answer = occupancy[index];
							bool witnessIdentityAvailable = false;
							json witnessId = nullptr;
							bool ambiguousIdentity = false;
							if (answer.WitnessAvailable) {
								ecs::AttributeValue value;
								if (ecs::GetAttribute(
										store,
										answer.Witness,
										core::Name(script::DATA_SCENE_ID_ATTRIBUTE),
										value
									) &&
									value.Type == ecs::PropertyType::String && !value.String.empty() &&
									value.String.size() <= script::MAX_DATA_SCENE_ID_BYTES &&
									value.String.find('\0') == std::string::npos &&
									OccupancyUtf8(value.String)) {
									size_t matches = 0;
									store.Each<const ecs::InstanceName>([&](ecs::Entity entity,
																			const ecs::InstanceName &) {
										ecs::AttributeValue candidate;
										if (ecs::GetAttribute(
												store,
												entity,
												core::Name(script::DATA_SCENE_ID_ATTRIBUTE),
												candidate
											) &&
											candidate.Type == ecs::PropertyType::String &&
											candidate.String == value.String)
											matches++;
									});
									ambiguousIdentity = matches != 1;
									if (!ambiguousIdentity) {
										witnessIdentityAvailable = true;
										witnessId = value.String;
									}
								}
							}
							const bool rowAvailable =
								answer.Available && (answer.OverlapFound || answer.Complete);
							const json overlap = rowAvailable ? json(answer.OverlapFound) : json(nullptr);
							const json reason =
								rowAvailable || answer.Why == physics::ColliderOccupancy::Reason::None
									? json(nullptr)
									: json(OccupancyReason(answer.Why));
							answers.push_back(
								{{"name", names[index]},
								 {"minimum_metres", (*probesField)[index].at("minimum_metres")},
								 {"maximum_metres", (*probesField)[index].at("maximum_metres")},
								 {"available", rowAvailable},
								 {"overlap_found", overlap},
								 {"witness_id", std::move(witnessId)},
								 {"witness_identity_available", witnessIdentityAvailable},
								 {"complete", answer.Complete},
								 {"reason", reason}}
							);
						}
					});
				if (entered != world::WorldStatus::Ok) {
					failure = Error("validation_failed", "scene is unavailable");
					return nullptr;
				}
				json result{
					{"schema_version", "collider-occupancy/v1"},
					{"world_id", barrier.InstanceId},
					{"lifecycle",
					 {{"tick", barrier.Clock.Tick},
					  {"world_epoch", barrier.WorldEpoch},
					  {"world_version", barrier.WorldVersion}}},
					{"snapshot_id", request.SnapshotId},
					{"probes", std::move(answers)}
				};
				if (result.dump().size() > 64u * 1024u) {
					failure =
						Error("resource_limit", "collider occupancy exceeds the 65536-byte response limit");
					return nullptr;
				}
				return result;
			}
		});
	}
}

namespace engine::control::features {

	// A product explicitly opts into the lifecycle vocabulary for the session it
	// owns. The feature retains no state beyond the Surface's tool closures.
	inline Feature DataFactory(world::DataFactorySession &session) {
		return Feature{"data_factory", [&session](Surface &surface) {
						   surface.AddDataFactoryTools(session);
					   }};
	}
}
