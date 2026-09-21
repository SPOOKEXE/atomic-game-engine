#pragma once

// MCP handoff for completed, value-only replication exchange records.
// The host owns one queue per world. Factory reads use the session fence;
// listening servers expose the same metadata without a factory session.
// Wire payloads and store rows remain private.
// @tier L13 · shared

#include <engine/control/DataFactoryReadFence.hpp>
#include <engine/control/Surface.hpp>
#include <engine/replication/Observation.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace engine::control {
	using nlohmann::json;

	namespace replication_observation_detail {
		inline json Hooks() {
			json hooks = json::array();
			for (const auto hook : std::array{
					 replication::ReplicationHook::AuthorityPublished,
					 replication::ReplicationHook::AuthorityReceived,
					 replication::ReplicationHook::AuthorityApplied,
					 replication::ReplicationHook::AuthorityRejected,
					 replication::ReplicationHook::AuthorityRepaired,
					 replication::ReplicationHook::AuthorityDropped,
					 replication::ReplicationHook::ReplicaApplied,
					 replication::ReplicationHook::ReplicaRejected
				 })
				hooks.push_back(replication::Describe(hook));
			return hooks;
		}

		inline json Record(const replication::ReplicationObservationRecord &record) {
			json result{{"hook", replication::Describe(record.Hook)}};
			std::visit(
				[&](const auto &context) {
					using Context = std::decay_t<decltype(context)>;
					const auto &identity = context.Identity;
					result["world"] = std::string(identity.World.Text());
					result["authority"] = std::string(identity.Authority.Text());
					result["client"] =
						identity.Client.IsValid()
							? json{{"slot", identity.Client.Index}, {"generation", identity.Client.Generation}}
							: json(nullptr);
					result["baseline"] = identity.BaselineAvailable ? json(identity.Baseline) : json(nullptr);
					result["tick"] = identity.TickAvailable ? json(identity.Tick) : json(nullptr);
					result["exchange_round"] = identity.Round;
					if constexpr (std::is_same_v<Context, replication::AuthorityPublishedObservation>) {
						result["messages"] = context.MessageCount;
						result["bytes"] = context.ByteCount;
						result["snapshot"] = context.Snapshot;
					} else if constexpr (std::is_same_v<Context, replication::AuthorityRepairedObservation>) {
						result["groups"] = context.GroupCount;
						result["entities"] = context.EntityCount;
					} else {
						if constexpr (std::is_same_v<Context, replication::AuthorityRejectedObservation> ||
									  std::is_same_v<Context, replication::AuthorityDroppedObservation> ||
									  std::is_same_v<Context, replication::ReplicaRejectedObservation>)
							result["message"] = context.MessageAvailable
													? json(replication::Describe(context.Message))
													: json(nullptr);
						else
							result["message"] = replication::Describe(context.Message);
						result["bytes"] = context.ByteCount;
						if constexpr (!std::is_same_v<Context, replication::AuthorityDroppedObservation> &&
									  !std::is_same_v<Context, replication::AuthorityReceivedObservation>)
							result["status"] = replication::Describe(context.Status);
						if constexpr (std::is_same_v<Context, replication::AuthorityAppliedObservation>) {
							result["values"] = context.ValueCount;
							result["refused_values"] = context.RefusedCount;
						}
					}
				},
				record.Context
			);
			return result;
		}
	}

	inline void AddReplicationObservationTools(
		Surface &surface,
		world::DataFactorySession *session,
		replication::ReplicationObservations &observations,
		std::string worldName
	) {
		surface.Add(
			Tool{
				"replication_observation_hooks",
				"Lists stable replication exchange observation hook names and the completed-record capacity.",
				[] { return json{{"type", "object"}, {"additionalProperties", false}}; },
				[](const json &arguments, std::string &failure) -> json {
					if (!arguments.is_object() || !arguments.empty()) {
						failure = "validation_failed: replication_observation_hooks takes no arguments";
						return nullptr;
					}
					return json{
						{"hooks", replication_observation_detail::Hooks()},
						{"capacity", replication::ReplicationObservations::MAXIMUM_RECORDS}
					};
				}
			}
		);
		surface.Add(
			Tool{
				"replication_observation_poll",
				"Polls up to 256 completed replication exchange metadata records for one world. "
				"Each record identifies its hook, world, authority, client generation, baseline, tick and "
				"exchange round. Unavailable fields are null. Wire payloads and private component rows are "
				"never "
				"returned.",
				[session] {
					json required = {"instance_id"};
					if (session != nullptr) {
						required.push_back("expected_tick");
						required.push_back("expected_world_epoch");
						required.push_back("expected_world_version");
					}
					return json{
						{"type", "object"},
						{"properties",
						 {{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
						  {"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"expected_world_version", {{"type", "integer"}, {"minimum", 0}}},
						  {"limit", {{"type", "integer"}, {"minimum", 0}, {"maximum", 256}}}}},
						{"required", std::move(required)},
						{"additionalProperties", false}
					};
				},
				[session,
				 &observations,
				 worldName = std::move(worldName)](const json &arguments, std::string &failure) -> json {
					if (!arguments.is_object()) {
						failure = "validation_failed: arguments must be an object";
						return nullptr;
					}
					for (auto field = arguments.begin(); field != arguments.end(); ++field)
						if (field.key() != "instance_id" && field.key() != "expected_tick" &&
							field.key() != "expected_world_epoch" &&
							field.key() != "expected_world_version" && field.key() != "limit") {
							failure = "validation_failed: unknown argument: " + field.key();
							return nullptr;
						}
					if (session == nullptr &&
						(arguments.contains("expected_tick") || arguments.contains("expected_world_epoch") ||
						 arguments.contains("expected_world_version"))) {
						failure = "validation_failed: no data-factory revision exists for this host";
						return nullptr;
					}
					std::string instance;
					if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
					json fence;
					if (!data_factory_read_fence::Validate(session, instance, arguments, fence, failure))
						return fence;
					if (instance != worldName) {
						failure = "validation_failed: this observation queue belongs to another world";
						return nullptr;
					}
					size_t limit = replication::ReplicationObservations::MAXIMUM_RECORDS;
					if (arguments.contains("limit")) {
						uint64_t requested = 0;
						if (!data_factory_read_fence::UInt(arguments["limit"], "limit", requested, failure) ||
							requested > limit) {
							if (failure.empty()) failure = "validation_failed: limit exceeds 256 records";
							return nullptr;
						}
						limit = static_cast<size_t>(requested);
					}
					json records = json::array();
					for (size_t index = 0; index < limit; ++index) {
						const auto record = observations.Poll();
						if (!record) break;
						const json encoded = replication_observation_detail::Record(*record);
						if (encoded["world"] != worldName) {
							failure = "invalid_data: observation world does not match its host queue";
							return nullptr;
						}
						records.push_back(encoded);
					}
					return json{
						{"status", "ok"},
						{"instance_id", instance},
						{"hooks", replication_observation_detail::Hooks()},
						{"records", std::move(records)},
						{"dropped", observations.Dropped()},
						{"capacity", replication::ReplicationObservations::MAXIMUM_RECORDS}
					};
				}
			}
		);
	}

	namespace features {
		inline Feature ReplicationObservation(
			world::DataFactorySession *session,
			replication::ReplicationObservations &observations,
			std::string worldName
		) {
			return Feature{
				"replication_observation",
				[session, &observations, worldName = std::move(worldName)](Surface &surface) {
					AddReplicationObservationTools(surface, session, observations, worldName);
				}
			};
		}
	}
}
