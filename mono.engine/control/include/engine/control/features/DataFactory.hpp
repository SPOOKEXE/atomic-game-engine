#pragma once

// The data-factory lifecycle tools. The host owns the session; this adapter
// validates MCP input before inspecting or mutating that session.

#include <engine/control/Surface.hpp>
#include <engine/world/DataFactory.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::control {

	using nlohmann::json;

	namespace data_factory_detail {
		inline constexpr size_t MAXIMUM_ID = 128;
		inline constexpr size_t MAXIMUM_ACTIONS = 32;
		inline constexpr size_t MAXIMUM_LEDGER_ENTRIES = 256;

		struct Request {
			std::string InstanceId;
			std::string OperationId;
			std::string CheckpointId;
			uint64_t Tick = 0;
			uint64_t Epoch = 0;
			uint64_t Version = 0;
			world::DataFactoryInterval Interval;
			world::DataFactoryPauseScope Scope = world::DataFactoryPauseScope::AllSystems;
			std::string BaseSnapshotId;
			std::vector<world::DataFactoryIntervention> Changes;
		};

		struct LedgerEntry {
			std::string Arguments;
			json Result;
			std::string Failure;
		};
		struct Ledger {
			std::unordered_map<std::string, LedgerEntry> Entries;
			std::deque<std::string> Order;
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

		inline bool Actions(const json &values, json &normalized, std::string &failure) {
			const auto found = values.find("actions");
			if (found == values.end()) {
				normalized["actions"] = json::array();
				return true;
			}
			if (!found->is_array() || found->size() > MAXIMUM_ACTIONS) {
				failure = Error("validation_failed", "actions must be an array with at most 32 names");
				return false;
			}
			for (const json &action : *found) {
				std::string name;
				if (!Text(action, "actions entry", name, failure)) return false;
			}
			normalized["actions"] = *found;
			if (!found->empty()) {
				failure = Error("capability_unsupported", "actions have no installed executor");
				return false;
			}
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

		inline void Store(
			Ledger &ledger, const std::string &id, std::string arguments, json result, std::string failure
		) {
			ledger.Order.push_back(id);
			ledger.Entries.emplace(
				id, LedgerEntry{std::move(arguments), std::move(result), std::move(failure)}
			);
			while (ledger.Order.size() > MAXIMUM_LEDGER_ENTRIES) {
				ledger.Entries.erase(ledger.Order.front());
				ledger.Order.pop_front();
			}
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
				{"checkpoint_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}}},
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
				  {"maxItems", MAXIMUM_ACTIONS},
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
		auto ledger = std::make_shared<Ledger>();
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
				const auto prior = ledger->Entries.find(request.OperationId);
				if (prior != ledger->Entries.end()) {
					if (prior->second.Arguments != normalized.dump()) {
						failure = Error(
							"operation_id_conflict", "operation_id was already used with different arguments"
						);
						return nullptr;
					}
					failure = prior->second.Failure;
					return prior->second.Result;
				}
			}
			if (!Preconditions(session, request, failure)) return nullptr;
			const world::DataFactoryReply reply = call(request);
			json result = Reply(reply);
			if (reply.Status != world::DataFactoryStatus::Ok)
				failure = Error(
					world::Describe(reply.Status), reply.Detail.empty() ? "operation refused" : reply.Detail
				);
			if (!request.OperationId.empty())
				data_factory_detail::Store(*ledger, request.OperationId, normalized.dump(), result, failure);
			return result;
		};

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
							   Interval(v, r, n, f) && Actions(v, n, f);
					},
					[&session](const Request &r) {
						return session.Step(r.InstanceId, r.Interval, r.Tick, r.Version);
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
						if (const auto prior = ledger->Entries.find(request.OperationId);
							prior != ledger->Entries.end()) {
							if (prior->second.Arguments != normalized.dump()) {
								f = Error(
									"operation_id_conflict",
									"operation_id was already used with different arguments"
								);
								return nullptr;
							}
							f = prior->second.Failure;
							return prior->second.Result;
						}
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
						data_factory_detail::Store(
							*ledger, request.OperationId, normalized.dump(), result, f
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
