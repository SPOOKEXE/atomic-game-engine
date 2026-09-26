#include "ObservationSink.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Observation.hpp>
#include <engine/script/PortalObservation.hpp>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <system_error>
#include <tuple>
#include <vector>

namespace server {

	namespace {
		using nlohmann::json;

		json Vector(const engine::core::Vector3 &value) {
			return json::array({value.X, value.Y, value.Z});
		}

		json
		Row(std::string_view process,
			std::string_view world,
			std::string_view hook,
			const engine::script::PortalObservationStamp &stamp) {
			return {
				{"process", process},
				{"world", world},
				{"hook", hook},
				{"tick", stamp.Tick},
				{"sequence", stamp.Sequence},
				{"trace", stamp.Trace},
				{"subject", stamp.Subject},
			};
		}
	}

	bool ObservationSink::Open(const std::filesystem::path &directory, std::string process, uint64_t trace) {
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error) {
			ENGINE_ERROR("--observe-dir '{}' cannot be created: {}", directory.string(), error.message());
			return false;
		}
		Process = std::move(process);
		Trace = trace;
		Out.open(directory / (Process + ".jsonl"), std::ios::app);
		if (!Out) {
			ENGINE_ERROR("--observe-dir '{}' is not writable", directory.string());
			return false;
		}
		return true;
	}

	void ObservationSink::Collect(engine::world::Universe &worlds) {
		if (!Out) return;
		using namespace engine::script;
		for (const auto id : worlds.Worlds()) {
			if (worlds.IsRemote(id)) continue;
			const std::string world(worlds.NameOf(id).Text());
			Cursor &cursor = Cursors[world];
			worlds.Enter(id, [&](engine::ecs::Store &store) {
				if (!cursor.Traced && Trace != 0) {
					SetPortalObservationTrace(store, Trace);
					cursor.Traced = true;
				}
				const auto portal = CopyPortalObservations(store);
				const auto fresh = [&](const PortalObservationStamp &stamp) {
					return stamp.Sequence > cursor.Portal;
				};
				// Each hook is its own ring; merging by sequence restores one ordered trail.
				std::vector<std::pair<uint64_t, json>> rows;
				for (const auto &record : portal.Crossings) {
					if (!fresh(record.Stamp)) continue;
					json row = Row(Process, world, PORTAL_CROSSING_OBSERVATION, record.Stamp);
					row["begun"] = record.Begun;
					row["direct"] = record.Direct;
					row["transfer"] = record.Transfer;
					row["prior_offset"] = record.PriorOffset;
					row["current_offset"] = record.CurrentOffset;
					row["prior"] = Vector(record.Prior);
					row["current"] = Vector(record.Current);
					row["destination"] = record.Destination.View();
					row["reason"] = record.Reason.View();
					rows.emplace_back(record.Stamp.Sequence, std::move(row));
				}
				for (const auto &record : portal.Arrivals) {
					if (!fresh(record.Stamp)) continue;
					json row = Row(Process, world, PORTAL_ARRIVAL_OBSERVATION, record.Stamp);
					row["transfer"] = record.Transfer;
					row["position"] = Vector(record.Position);
					row["baseline_input_tick"] = record.BaselineInputTick;
					row["source"] = record.Source.View();
					rows.emplace_back(record.Stamp.Sequence, std::move(row));
				}
				for (const auto &record : portal.Inputs) {
					if (!fresh(record.Stamp)) continue;
					json row = Row(Process, world, PORTAL_INPUT_OBSERVATION, record.Stamp);
					row["route"] = Describe(record.Route);
					row["input_tick"] = record.InputTick;
					row["direction"] = Vector(record.Direction);
					rows.emplace_back(record.Stamp.Sequence, std::move(row));
				}
				for (const auto &record : portal.Handoffs) {
					if (!fresh(record.Stamp)) continue;
					json row = Row(Process, world, PORTAL_HANDOFF_OBSERVATION, record.Stamp);
					row["event"] = Describe(record.Event);
					row["transfer"] = record.Transfer;
					row["input_tick"] = record.InputTick;
					row["peer"] = record.Peer.View();
					rows.emplace_back(record.Stamp.Sequence, std::move(row));
				}
				std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
					return left.first < right.first;
				});
				for (const auto &[sequence, row] : rows) {
					Out << row.dump() << '\n';
					cursor.Portal = std::max(cursor.Portal, sequence);
				}

				for (const auto &record : engine::physics::CopyPhysicsObservationRecords(store)) {
					const bool stepped = engine::physics::HasObservationValue(
						record.Available, engine::physics::PhysicsObservationAvailability::PhysicsStep
					);
					// Every boundary of a step shares its step number, so compare
					// (tick, step, boundary) as one position in the pipeline.
					const auto position = std::make_tuple(
						record.Tick, stepped ? record.PhysicsStep : 0, static_cast<int>(record.Boundary)
					);
					if (position <=
						std::make_tuple(cursor.PhysicsTick, cursor.PhysicsStep, cursor.PhysicsBoundary))
						continue;
					cursor.PhysicsTick = std::get<0>(position);
					cursor.PhysicsStep = std::get<1>(position);
					cursor.PhysicsBoundary = std::get<2>(position);
					json row = {
						{"process", Process},
						{"world", world},
						{"hook", record.Hook()},
						{"tick", record.Tick},
						{"trace", Trace},
						{"step", record.PhysicsStep},
						{"step_seconds", record.StepSeconds},
						{"moving_bodies", record.MovingBodies},
						{"candidate_pairs", record.CandidatePairs},
						{"manifolds", record.Manifolds},
						{"solver_bodies", record.SolverBodies},
						{"solver_rows", record.SolverRows},
					};
					Out << row.dump() << '\n';
				}
			});
		}
		Out.flush();
	}
}
