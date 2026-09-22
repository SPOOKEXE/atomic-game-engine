#pragma once

// Data-factory read of completed, value-only fixed-step physics observations.
// The world is entered only after the lifecycle revision has been checked.
// @tier L13 · shared

#include <engine/control/DataFactoryReadFence.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Observation.hpp>
#include <engine/world/Universe.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace engine::control {
	using nlohmann::json;

	namespace physics_observation_detail {
		// Encodes one physics observation for a control response.
		inline json Record(const physics::PhysicsObservationRecord &record) {
			using Available = physics::PhysicsObservationAvailability;
			const auto value = [&](Available field, auto number) -> json {
				return physics::HasObservationValue(record.Available, field) ? json(number) : json(nullptr);
			};
			return json{
				{"hook", record.Hook()},
				{"world",
				 physics::HasObservationValue(record.Available, Available::World) ? json(record.WorldName())
																				  : json(nullptr)},
				{"tick", value(Available::Tick, record.Tick)},
				{"physics_step", value(Available::PhysicsStep, record.PhysicsStep)},
				{"step_in_tick", value(Available::StepInTick, record.StepInTick)},
				{"step_seconds", value(Available::StepSeconds, record.StepSeconds)},
				{"time_unit", "seconds"},
				{"moving_bodies", value(Available::MovingBodies, record.MovingBodies)},
				{"candidate_pairs", value(Available::CandidatePairs, record.CandidatePairs)},
				{"manifolds", value(Available::Manifolds, record.Manifolds)},
				{"solver_bodies", value(Available::SolverBodies, record.SolverBodies)},
				{"solver_rows", value(Available::SolverRows, record.SolverRows)}
			};
		}
	}

	// Installs physics-observation rows for a data-factory session.
	inline void AddPhysicsObservationTools(Surface &surface, world::DataFactorySession &session) {
		surface.Add(
			Tool{
				"physics_observation_hooks",
				"Lists the stable fixed-step physics observation hook names and the completed-record "
				"capacity.",
				[] { return json{{"type", "object"}, {"additionalProperties", false}}; },
				[](const json &arguments, std::string &failure) -> json {
					if (!arguments.is_object() || !arguments.empty()) {
						failure = "validation_failed: physics_observation_hooks takes no arguments";
						return nullptr;
					}
					json hooks = json::array();
					for (const std::string_view hook : physics::PhysicsObservationHooks())
						hooks.push_back(hook);
					return json{
						{"hooks", std::move(hooks)}, {"capacity", physics::PhysicsObservationLog::CAPACITY}
					};
				}
			}
		);
		surface.Add(
			Tool{
				"physics_observation_records",
				"Returns up to 96 completed fixed-step physics records for one factory-owned world. The hook "
				"name declares post-integration, pre-solve, or completed-solver state. Values retain exact "
				"tick, physics step, step seconds and availability; unavailable fields are null. "
				"The read cannot change physics state.",
				[] {
					return json{
						{"type", "object"},
						{"properties",
						 {{"instance_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
						  {"expected_tick", {{"type", "integer"}, {"minimum", 0}}},
						  {"expected_world_epoch", {{"type", "integer"}, {"minimum", 0}}},
						  {"expected_world_version", {{"type", "integer"}, {"minimum", 0}}}}},
						{"required",
						 {"instance_id", "expected_tick", "expected_world_epoch", "expected_world_version"}},
						{"additionalProperties", false}
					};
				},
				[&session](const json &arguments, std::string &failure) -> json {
					if (!arguments.is_object()) {
						failure = "validation_failed: arguments must be an object";
						return nullptr;
					}
					for (auto field = arguments.begin(); field != arguments.end(); ++field)
						if (field.key() != "instance_id" && field.key() != "expected_tick" &&
							field.key() != "expected_world_epoch" &&
							field.key() != "expected_world_version") {
							failure = "validation_failed: unknown argument: " + field.key();
							return nullptr;
						}
					std::string instance;
					if (!data_factory_read_fence::InstanceId(arguments, instance, failure)) return nullptr;
					json fence;
					if (!data_factory_read_fence::Validate(&session, instance, arguments, fence, failure))
						return fence;
					world::Universe &universe = session.UniverseOf();
					const world::WorldId id = universe.Find(core::Name(instance));
					if (!id.IsValid()) {
						failure = "validation_failed: world is unavailable";
						return nullptr;
					}
					std::vector<physics::PhysicsObservationRecord> copied;
					uint64_t overwritten = 0;
					bool available = false;
					if (universe.Enter(id, [&](ecs::Store &store) {
							available = store.Resource<physics::PhysicsObservationLog>() != nullptr;
							copied = physics::CopyPhysicsObservationRecords(store);
							overwritten = physics::OverwrittenPhysicsObservationRecords(store);
						}) != world::WorldStatus::Ok) {
						failure = "unavailable: world cannot be entered";
						return nullptr;
					}
					json records = json::array();
					for (const auto &record : copied)
						records.push_back(physics_observation_detail::Record(record));
					json hooks = json::array();
					for (const std::string_view hook : physics::PhysicsObservationHooks())
						hooks.push_back(hook);
					return json{
						{"status", "ok"},
						{"instance_id", instance},
						{"available", available},
						{"hooks", std::move(hooks)},
						{"records", std::move(records)},
						{"capacity", physics::PhysicsObservationLog::CAPACITY},
						{"overwritten", overwritten}
					};
				}
			}
		);
	}

	namespace features {
		// Returns the physics-observation feature for this session.
		inline Feature PhysicsObservation(world::DataFactorySession &session) {
			return Feature{"physics_observation", [&session](Surface &surface) {
							   AddPhysicsObservationTools(surface, session);
						   }};
		}
	}
}
