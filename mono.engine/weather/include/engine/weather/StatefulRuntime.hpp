#pragma once

// ECS owns authoritative physical grids. Replica stores retain copied checkpoints, not CPU grid mirrors.
// Storm-column history retains bounded actual solver samples, never complete past grids.
// @tier L8 shared

#include <engine/ecs/Entity.hpp>
#include <engine/weather/Checkpoint.hpp>
#include <engine/weather/Consumers.hpp>

#include <memory>

namespace engine::ecs {
	class Store;
	class Scheduler;
}

namespace engine::weather {
	// One completed canonical storm-column sample.
	struct StatefulTickSample {
		// Completed solver tick, canonical world column and its copied consumer responses.
		//@{
		uint64_t Tick = 0;
		core::Vector3 PositionMetres;
		ConsumerSample Sample;
		//@}
	};
	// Local authoritative storage with a complete save codec; excluded from replication defaults.
	struct StatefulWeatherState {
		// Authoritative physical history, named seed key, accepted revision and local update diagnostics.
		//@{
		SimulationState Simulation;
		std::vector<StatefulTickSample> TickSamples;
		uint64_t LastWorldTick = 0;
		uint32_t Seed = 1;
		uint32_t LayerKey = 0;
		uint32_t AcceptedRevision = 0;
		SimulationStatus LastStatus = SimulationStatus::Success;
		// Process-local solver scratch, omitted from saves and never copied across a world boundary.
		std::shared_ptr<SimulationWorkspace> Workspace{};
		//@}
	};
	// Persisted local diagnostic for a refused initial atmosphere, never a physical state or checkpoint.
	struct StatefulWeatherInitializationFailure {
		// The accepted authored revision whose initialization was refused.
		uint32_t DefinitionRevision = 1;
		// Derived retry watermarks are omitted from saves and reset when restored.
		//@{
		uint64_t AcceptedWrites = UINT64_MAX;
		uint64_t CheckpointWrites = UINT64_MAX;
		//@}
	};
	// Published at five-second weather intervals or explicit requests, suitable for copied late joins.
	struct StatefulWeatherCheckpoint {
		// Copied timeline metadata and complete versioned solver bytes.
		//@{
		uint64_t Tick = 0;
		uint64_t Epoch = 1;
		uint32_t Revision = 1;
		std::vector<std::byte> Bytes;
		//@}
		// Derived scalar-query index, populated only by checkpoint validation and QueryStatefulWeather.
		// It retains metadata and an offset into Bytes, never a CPU physical grid or serialized authority.
		struct QueryIndex {
			// Compatible definition, integrity word and first physical cell byte.
			//@{
			SimulationDefinition Definition;
			uint64_t Digest = 0;
			uint64_t Tick = 0;
			uint64_t SourceWriteVersion = 0;
			uint32_t CellOffset = 0;
			uint32_t RainOffset = 0;
			std::vector<uint8_t> SolidMask{};
			std::vector<float> GroundHeightMetres{};
			//@}
		};
		// Checkpoint bytes remain authoritative; integrity and observed source writes invalidate this cache.
		mutable std::optional<QueryIndex> Queries{};
	};
	// Registers authoritative state, checkpoint and initialization-refusal component codecs.
	void RegisterStatefulWeatherComponents();
	// Fixed-tick authority. Accepted structural/seed/purpose edits start a new physics epoch at current
	// phase; wind and visual edits preserve the atmosphere. Invalid edits retain the previous state.
	// Retains catchup backlog and skips AdoptOnly stores entirely.
	void UpdateStatefulWeather(ecs::Store &store);
	// Registers after the weather clock and before consumers.
	void RegisterStatefulWeatherSystem(ecs::Scheduler &scheduler);
	// Forces a checkpoint publication without advancing physics. Invalid states leave the old copy intact.
	[[nodiscard]] bool PublishStatefulCheckpoint(ecs::Store &store, ecs::Entity provider);
	// Restores a compatible copied checkpoint on an authoritative host, transactionally.
	[[nodiscard]] bool RestoreStatefulCheckpoint(
		ecs::Store &store, ecs::Entity provider, const StatefulWeatherCheckpoint &checkpoint
	);
	// Current solver tick; absence means the requested domain has not been accepted.
	[[nodiscard]] std::optional<uint64_t> StatefulWeatherTick(const ecs::Store &store, ecs::Entity provider);
	// Current queries sample physical cells. Historical queries require the retained deterministic storm
	// column.
	[[nodiscard]] std::optional<ConsumerSample> QueryStatefulWeather(
		ecs::Store &store,
		ecs::Entity provider,
		core::Vector3 positionMetres,
		float groundHeightMetres = 0,
		uint64_t tick = UINT64_MAX
	);
	// The canonical event-column key includes seed, named layer, epoch and completed solver tick.
	[[nodiscard]] core::Vector3 StatefulEventColumn(const StatefulWeatherState &state, uint64_t tick);
	// Certifies that accepted terrain excludes every physical cell in a canonical storm column.
	// The caller separately checks that the requested phase tick has completed.
	[[nodiscard]] bool StatefulEventColumnBlocked(const StatefulWeatherState &state, core::Vector3 column);
}
