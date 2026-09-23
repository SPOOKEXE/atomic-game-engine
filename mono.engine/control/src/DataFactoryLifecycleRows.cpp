#include "DataFactoryMcpAdapter.hpp"

namespace engine::control {
	using nlohmann::json;

	void AddDataFactorySceneObservationRows(Surface &surface, world::DataFactorySession &session);

	void Surface::AddDataFactoryTools(world::DataFactorySession &session, DataFactoryToolSet tools) {
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
					if (settingsRequired) {
						if (!Only(
								values,
								{"instance_id",
								 "seed",
								 "tick_rate",
								 "expected_tick",
								 "expected_world_epoch",
								 "expected_world_version",
								 "operation_id"},
								failure
							))
							return nullptr;
					} else if (!Only(
								   values,
								   {"instance_id",
									"expected_tick",
									"expected_world_epoch",
									"expected_world_version",
									"operation_id"},
								   failure
							   )) {
						return nullptr;
					}
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
			"fork_world",
			"Forks one retained checkpoint into an isolated, non-presented branch runtime. The parent "
			"lifecycle revision and checkpoint identity fence the operation.",
			[] {
				return Schema(
					{"instance_id",
					 "checkpoint_id",
					 "branch_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id"},
					{"instance_id",
					 "checkpoint_id",
					 "branch_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id"}
				);
			},
			[&session, ledger](const json &values, std::string &failure) -> json {
				Request request;
				json normalized;
				if (!Base(values, true, request, normalized, failure) || !Only(
																			 values,
																			 {"instance_id",
																			  "checkpoint_id",
																			  "branch_id",
																			  "expected_tick",
																			  "expected_world_epoch",
																			  "expected_world_version",
																			  "operation_id"},
																			 failure
																		 ))
					return nullptr;
				const json *field = nullptr;
				std::string branchId;
				if (!Field(values, "checkpoint_id", field, failure) ||
					!Text(*field, "checkpoint_id", request.CheckpointId, failure) ||
					!Field(values, "branch_id", field, failure) ||
					!Text(*field, "branch_id", branchId, failure))
					return nullptr;
				normalized["checkpoint_id"] = request.CheckpointId;
				normalized["branch_id"] = branchId;
				normalized["tool"] = "fork_world";
				json replay;
				const auto prior =
					ledger->Replay("fork_world", request.OperationId, normalized.dump(), replay, failure);
				if (prior == DataFactoryOperationReplay::Conflict) return nullptr;
				if (prior == DataFactoryOperationReplay::Replay) return replay;
				if (!Preconditions(session, request, failure)) return nullptr;
				const world::DataFactoryReply reply = session.Fork({
					.InstanceId = request.InstanceId,
					.CheckpointId = request.CheckpointId,
					.BranchId = std::move(branchId),
					.ExpectedWorldEpoch = request.Epoch,
					.ExpectedWorldVersion = request.Version,
					.ExpectedTick = request.Tick,
				});
				json result = Reply(reply);
				if (reply.Status != world::DataFactoryStatus::Ok)
					failure = Error(
						world::Describe(reply.Status), reply.Detail.empty() ? "fork refused" : reply.Detail
					);
				ledger->Store("fork_world", request.OperationId, normalized.dump(), result, failure);
				return result;
			},
		});
		if (tools.RenderOnly) {
			Add(Tool{
				"render_only",
				"Queues one static frame from a paused snapshot. Submitted means the frame command was "
				"accepted, "
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
						f = Error(
							"capability_unsupported", "this host supports only preserve temporal history"
						);
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
				"Reads pending, submitted, or failed render-only command status. Submitted does not claim "
				"GPU "
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
						!Field(v, "operation_id", field, f) ||
						!UInt(*field, "operation_id", operationId, f) || operationId == 0) {
						if (f.empty()) f = Error("validation_failed", "operation_id must be positive");
						return nullptr;
					}
					const world::DataFactoryRenderOnlyReply reply =
						session.PollRenderOnly(instance, operationId);
					json result;
					RenderOnlyReply(reply, result, f);
					return result;
				}
			});
		}
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
		if (session.SupportsBackwardSeek())
			Add(Tool{
				"seek_backward",
				"Restores the newest retained paused checkpoint and replays contiguous action-free fixed "
				"steps "
				"to an earlier completed tick.",
				[] {
					return Schema(
						{"instance_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version",
						 "operation_id",
						 "target_tick"},
						{"instance_id",
						 "expected_tick",
						 "expected_world_epoch",
						 "expected_world_version",
						 "operation_id",
						 "target_tick"}
					);
				},
				[invoke, &session](const json &v, std::string &f) {
					return invoke(
						"seek_backward",
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
									 "target_tick"},
									f
								))
								return false;
							const json *field = nullptr;
							if (!Field(v, "target_tick", field, f) ||
								!UInt(*field, "target_tick", r.TargetTick, f))
								return false;
							n["target_tick"] = r.TargetTick;
							return true;
						},
						[&session](const Request &r) {
							return session.SeekBackward(r.InstanceId, r.TargetTick, r.Tick, r.Version);
						}
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

		AddDataFactorySceneObservationRows(*this, session);
	}
}
