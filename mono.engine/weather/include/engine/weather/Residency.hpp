#pragma once

// Portable planning and publication metadata for disposable renderer-owned pages.
// No resident cells, device handles, or duplicate authored weather definitions live here.
//
// @tier L8 · shared

#include <engine/weather/Weather.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::weather {

	inline constexpr uint32_t BRICK_SIDE = 8;
	inline constexpr uint32_t BRICK_CELLS = BRICK_SIDE * BRICK_SIDE * BRICK_SIDE;
	inline constexpr uint32_t BRICK_PAYLOAD_BYTES = BRICK_CELLS * ENCODED_CELL_BYTES;
	inline constexpr uint32_t MAXIMUM_RESIDENCY_PAGES = 8192;
	inline constexpr uint32_t MAXIMUM_RESIDENCY_DEMANDS = 65536;
	inline constexpr uint32_t MAXIMUM_RESIDENCY_VIEWS = 32;

	struct BrickKey {
		CellCoordinate Origin;
		uint32_t Lod = 0;
		bool operator==(const BrickKey &) const = default;
	};

	// Repeated keys from independent views are combined. Coarse coverage precedes refinement.
	struct BrickDemand {
		BrickKey Key;
		float ProjectedPixels = 0.0f;
		float MaximumOpticalError = 0.0f;
		bool RequiredCoarse = false;
	};

	struct ResidencySelection {
		std::vector<BrickKey> Pages;
		uint32_t RefusedRefinements = 0;
		uint32_t CoarseFallbackRegions = 0;
		// Missing guaranteed parents use direct canonical evaluation, never empty samples.
		bool RequiresProceduralFallback = false;
	};

	struct LodPolicy {
		uint32_t MaximumLod = 8;
		float RefineAbovePixels = 64.0f;
		float CoarsenBelowPixels = 32.0f;
		float MaximumOpticalError = 0.04f;
		uint64_t MinimumResidenceTicks = 20;
	};

	struct MotionEnvelope {
		float MaximumSpeedMetresPerSecond = 0.0f;
		float MaximumAccelerationMetresPerSecondSquared = 0.0f;
		float CameraSpeedMetresPerSecond = 0.0f;
		float GrowthMetresPerSecond = 0.0f;
		float ReconstructionMarginMetres = 0.0f;
	};

	struct ResidencyView {
		uint64_t Key = 0;
		core::Vector3 CameraPositionMetres;
		uint64_t PortalRevision = 0;
	};

	struct MaintenanceStamp {
		uint64_t Tick = 0;
		uint32_t WeatherRevision = 0;
		uint64_t TimelineRevision = 0;
		std::vector<ResidencyView> Views;
		bool Valid = false;
	};

	struct MaintenancePolicy {
		uint32_t TickRate = 20;
		float IntervalSeconds = 5.0f;
		float MaximumCameraTravelMetres = 256.0f;
	};

	enum class PageLifetime : uint8_t { Vacant, Generating, Published, Retired };

	struct ResidencyPage {
		BrickKey Key;
		uint64_t Generation = 0;
		uint64_t LastReferenceSubmission = 0;
		PageLifetime Lifetime = PageLifetime::Vacant;
	};

	// Caller-owned disposable cache. The renderer allocates a fixed slot pool once.
	struct ResidencyState {
		std::vector<ResidencyPage> Slots;
		uint64_t NextGeneration = 1;
		uint64_t PublishedGeneration = 0;
		uint64_t PublishedTick = 0;
		uint32_t PublishedRevision = 0;
		uint64_t PendingGeneration = 0;
		uint64_t PendingTick = 0;
		uint32_t PendingRevision = 0;
		uint64_t GenerationSubmission = 0;
	};

	struct PageGeneration {
		uint64_t Generation = 0;
		std::vector<uint32_t> Slots;
	};

	struct ResidencyMemory {
		uint64_t PayloadBytes = 0;
		uint64_t MetadataBytes = 0;
		uint64_t PublishedBytes = 0;
		uint64_t GeneratingBytes = 0;
		uint64_t RetiredBytes = 0;
	};

	[[nodiscard]] bool ValidBrickKey(const BrickKey &key);
	// Stable key order breaks priority ties; seeds never contain physical page slots.
	[[nodiscard]] std::optional<ResidencySelection>
	SelectResidency(std::span<const BrickDemand> demands, uint32_t pageBudget);
	// Optical error is a conservative finest-cell bound and doubles per coarsening level.
	[[nodiscard]] std::optional<uint32_t> ChooseLod(
		float finestProjectedPixels,
		float finestOpticalError,
		uint32_t previousLod,
		uint64_t residenceTicks,
		const LodPolicy &policy = {}
	);
	// Includes actual scheduling overrun, growth and camera motion, not just nominal cadence.
	[[nodiscard]] std::optional<float> MotionMargin(const MotionEnvelope &envelope, float lifetimeSeconds);
	[[nodiscard]] bool MaintenanceRequired(
		const MaintenanceStamp &stamp,
		std::span<const ResidencyView> views,
		uint64_t tick,
		uint32_t weatherRevision,
		uint64_t timelineRevision,
		const MaintenancePolicy &policy = {}
	);
	[[nodiscard]] std::optional<MaintenanceStamp> RecordMaintenance(
		std::span<const ResidencyView> views,
		uint64_t tick,
		uint32_t weatherRevision,
		uint64_t timelineRevision
	);
	// Reservation is all-or-nothing. Published/in-flight slots cannot be overwritten.
	[[nodiscard]] std::optional<PageGeneration> ReserveGeneration(
		ResidencyState &state,
		std::span<const BrickKey> pages,
		uint64_t tick,
		uint32_t revision,
		uint64_t completedSubmission
	);
	[[nodiscard]] bool
	MarkGenerationSubmitted(ResidencyState &state, uint64_t generation, uint64_t submission);
	// Only a completed GPU submission publishes the entire table and its exact tick/revision.
	[[nodiscard]] bool
	PublishGeneration(ResidencyState &state, uint64_t generation, uint64_t completedSubmission);
	[[nodiscard]] bool ReferencePublished(ResidencyState &state, uint64_t submission);
	void RetireCompleted(ResidencyState &state, uint64_t completedSubmission);
	[[nodiscard]] bool CancelGeneration(ResidencyState &state, uint64_t generation);
	[[nodiscard]] ResidencyMemory MemoryOf(const ResidencyState &state);
}
