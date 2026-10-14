#pragma once

// ECS owns accepted weather consumer state and bounded authoritative storm history.
// Cached compilation is derived arithmetic; WeatherCompute remains the only mutable definition.
// @tier L8 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/weather/Consumers.hpp>
#include <engine/weather/Events.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace engine::ecs {
	class Store;
	class Scheduler;
}

namespace engine::weather {
	// Bound on simultaneously prepared authored weather layers.
	inline constexpr uint32_t MAX_RUNTIME_WEATHER_LAYERS = 32;
	// Persistent event integrators. Saving this record prevents replaying damage after load.
	struct WeatherRuntimeState {
		// Accepted epoch, most recently completed tick and next unique event sequence.
		//@{
		uint64_t Epoch = 1;
		uint64_t LastTick = 0;
		uint64_t NextSequence = 1;
		//@}
		// Canonical source seed and fixed-point Q16 event-growth accumulators.
		//@{
		uint32_t Seed = 1;
		uint32_t ChargeQ16 = 0;
		uint32_t HailGrowthQ16 = 0;
		//@}
		// False until the initial accepted clock has been captured.
		bool Ready = false;
		// Compares semantic scheduling fields without object padding.
		bool operator==(const WeatherRuntimeState &) const = default;
	};
	// One derived compilation. No authored control is duplicated here.
	struct PreparedWeatherLayer {
		// ECS identity is local cache metadata, never an event seed.
		ecs::Entity Provider;
		// Prepared reference arithmetic for the current definition revision.
		std::optional<CompiledWeather> Compiled;
		// Stable authored provider path identifies the copied event stream.
		std::string Layer;
		// All colliding named identities are refused, independently of entity allocation order.
		bool AmbiguousIdentity = false;
		// Desired controls or provider-path edits wait until accepted ticks have completed.
		bool PendingDefinition = false;
	};
	// Process-local ECS cache reset on loading; source epochs recover missed cleared changes.
	struct WeatherRuntimeCache {
		// Bounded derived records for the authored providers.
		std::vector<PreparedWeatherLayer> Layers;
		// Source write/membership epochs, with a separate missed-change watermark.
		//@{
		uint64_t DefinitionWrites = 0;
		uint64_t DefinitionMembership = 0;
		uint64_t NameWrites = 0;
		uint64_t HierarchyWrites = 0;
		uint64_t AcceptedWrites = 0;
		uint64_t AcceptedMembership = 0;
		//@}
		// True after the first complete membership pass.
		bool Ready = false;
		// Number of definitions actually compiled, for profiling cache work.
		uint64_t Compilations = 0;
	};
	// One listener's copied local position and thunder receipt state.
	struct WeatherListener {
		// World listener position in metres, supplied by the presentation/audio adapter.
		core::Vector3 PositionMetres;
		// Air temperature used by the declared dry-air sound model.
		float TemperatureCelsius = 15.0f;
		// Listener-local scheduling state, independent of authoritative storm generation.
		ThunderQueue Thunder;
	};
	// Intraworld host binding to its accepted physics index. The callback never enters weather payloads or
	// saves. The save reader clears it; each host reinstalls its own function before running weather.runtime.
	struct WeatherGroundQuery {
		// Downward first-hit height from the cloud origin, within the supplied distance, in metres.
		// No captured context is permitted: the owning store supplies all query state.
		std::optional<float> (*Height)(const ecs::Store &, core::Vector3, float) = nullptr;
		// Derived terrain preparation watermarks reset on load, alongside the local callback.
		//@{
		uint64_t PreparedDefinitionWrites = UINT64_MAX;
		uint64_t PreparedDefinitionMembership = UINT64_MAX;
		uint64_t PreparedAcceptedWrites = UINT64_MAX;
		uint64_t PreparedAcceptedMembership = UINT64_MAX;
		uint64_t PreparedPhysicalMembership = UINT64_MAX;
		uint64_t PreparedCheckpointMembership = UINT64_MAX;
		//@}
	};
	// Accepted portable provider path, bounded to the storm payload identity limit.
	// Pending renames retain their canonical path until old ticks have completed.
	[[nodiscard]] std::string WeatherLayerName(const ecs::Store &store, ecs::Entity provider);
	// Registers named persistent runtime/event codecs and clears derived/local caches on load.
	void RegisterWeatherRuntimeComponents();
	// Prepares derived queries. Only the clock's acceptance boundary may publish desired edits,
	// after all old accepted ticks have completed; queries cannot reanchor a clock implicitly.
	void PrepareWeatherRuntime(ecs::Store &store, bool acceptDesiredEdits = false);
	// Queries accepted integral clock time through the shared canonical reference.
	[[nodiscard]] std::optional<ConsumerSample> QueryWeather(
		ecs::Store &store, ecs::Entity provider, core::Vector3 positionMetres, float groundHeightMetres = 0.0f
	);
	// Runs after weather.clock and the host's completed physics index. Replicas never create damage.
	// Missing ground bindings or absent hits refuse impacts without advancing event identity.
	void UpdateWeatherRuntime(ecs::Store &store);
	// Registers in Animation, after the physics phases and the optional weather.ground host binding.
	void RegisterWeatherRuntimeSystem(ecs::Scheduler &scheduler);
	// Applies one accepted event to scene humanoids in its world radius; refuses replicas and repeats.
	[[nodiscard]] bool
	ApplyStormDamage(ecs::Store &store, WeatherEventCursor &cursor, const StormEvent &event);
}
