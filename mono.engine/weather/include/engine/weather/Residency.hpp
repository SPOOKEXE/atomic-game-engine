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

	// Edge length of a cubic brick in canonical cells.
	inline constexpr uint32_t BRICK_SIDE = 8;
	// Number of canonical cell records in one cubic brick.
	inline constexpr uint32_t BRICK_CELLS = BRICK_SIDE * BRICK_SIDE * BRICK_SIDE;
	// Packed bytes uploaded for one brick's encoded physical cells.
	inline constexpr uint32_t BRICK_PAYLOAD_BYTES = BRICK_CELLS * ENCODED_CELL_BYTES;
	// Fixed renderer page-pool capacity used by portable residency planning.
	inline constexpr uint32_t MAXIMUM_RESIDENCY_PAGES = 8192;
	// Bound on merged view demands accepted by one planning pass.
	inline constexpr uint32_t MAXIMUM_RESIDENCY_DEMANDS = 65536;
	// Bound on views included in one maintenance snapshot.
	inline constexpr uint32_t MAXIMUM_RESIDENCY_VIEWS = 32;

	// Stable canonical key for a cubic brick at one level of detail.
	struct BrickKey {
		// Aligned canonical origin and coarsening level.
		//@{
		CellCoordinate Origin;
		uint32_t Lod = 0;
		//@}
		// Equality compares the full key, never a renderer slot.
		bool operator==(const BrickKey &) const = default;
	};

	// Repeated keys from independent views are combined. Coarse coverage precedes refinement.
	struct BrickDemand {
		// Requested key, screen-space priority, error bound and mandatory coarse coverage.
		//@{
		BrickKey Key;
		float ProjectedPixels = 0.0f;
		float MaximumOpticalError = 0.0f;
		bool RequiredCoarse = false;
		//@}
	};

	// Deterministic page plan and explicit fallbacks when the budget cannot satisfy it.
	struct ResidencySelection {
		// Selected keys and counts of denied refinements and coarse fallback regions.
		//@{
		std::vector<BrickKey> Pages;
		uint32_t RefusedRefinements = 0;
		uint32_t CoarseFallbackRegions = 0;
		//@}
		// Missing guaranteed parents use direct canonical evaluation, never empty samples.
		bool RequiresProceduralFallback = false;
	};

	// Screen-size and optical-error limits used with prior key residence.
	struct LodPolicy {
		// Coarsening cap, pixel thresholds, error cap and anti-churn hold time.
		//@{
		uint32_t MaximumLod = 8;
		float RefineAbovePixels = 64.0f;
		float CoarsenBelowPixels = 32.0f;
		float MaximumOpticalError = 0.04f;
		uint64_t MinimumResidenceTicks = 20;
		//@}
	};

	// Conservative motion and growth bounds over a page's retention lifetime.
	struct MotionEnvelope {
		// Weather, camera and reconstruction expansion rates in metres-based units.
		//@{
		float MaximumSpeedMetresPerSecond = 0.0f;
		float MaximumAccelerationMetresPerSecondSquared = 0.0f;
		float CameraSpeedMetresPerSecond = 0.0f;
		float GrowthMetresPerSecond = 0.0f;
		float ReconstructionMarginMetres = 0.0f;
		//@}
	};

	// One camera/portal identity and canonical position for maintenance decisions.
	struct ResidencyView {
		// Stable view key, camera position and portal topology revision.
		//@{
		uint64_t Key = 0;
		core::Vector3 CameraPositionMetres;
		uint64_t PortalRevision = 0;
		//@}
	};

	// Inputs that justified the last page-demand maintenance pass.
	struct MaintenanceStamp {
		// Accepted simulation identity, sampled views and initialization state.
		//@{
		uint64_t Tick = 0;
		uint32_t WeatherRevision = 0;
		uint64_t TimelineRevision = 0;
		std::vector<ResidencyView> Views;
		bool Valid = false;
		//@}
	};

	// Time and travel limits that force a fresh page-demand pass.
	struct MaintenancePolicy {
		// Simulation cadence, periodic interval and maximum camera displacement.
		//@{
		uint32_t TickRate = 20;
		float IntervalSeconds = 5.0f;
		float MaximumCameraTravelMetres = 256.0f;
		//@}
	};

	// A slot's publication state; retired slots wait for their last GPU reference.
	enum class PageLifetime : uint8_t { Vacant, Generating, Published, Retired };

	// Identity and in-flight reference watermark for one renderer-owned page slot.
	struct ResidencyPage {
		// Brick key, generation identity, last use and lifetime state.
		//@{
		BrickKey Key;
		uint64_t Generation = 0;
		uint64_t LastReferenceSubmission = 0;
		PageLifetime Lifetime = PageLifetime::Vacant;
		//@}
	};

	// Caller-owned disposable cache. The renderer allocates a fixed slot pool once.
	struct ResidencyState {
		// Fixed slots and monotonically assigned generation identity.
		//@{
		std::vector<ResidencyPage> Slots;
		uint64_t NextGeneration = 1;
		//@}
		// Published table identity and exact accepted source sample.
		//@{
		uint64_t PublishedGeneration = 0;
		uint64_t PublishedTick = 0;
		uint32_t PublishedRevision = 0;
		//@}
		// Pending table identity and submission owning its uploads.
		//@{
		uint64_t PendingGeneration = 0;
		uint64_t PendingTick = 0;
		uint32_t PendingRevision = 0;
		uint64_t GenerationSubmission = 0;
		//@}
	};

	// Reserved slots for one not-yet-published page-table generation.
	struct PageGeneration {
		// Generation number and its all-or-nothing slot reservation.
		//@{
		uint64_t Generation = 0;
		std::vector<uint32_t> Slots;
		//@}
	};

	// Logical payload and metadata accounting split by page lifetime.
	struct ResidencyMemory {
		// Byte totals; payload includes fixed brick cell storage.
		//@{
		uint64_t PayloadBytes = 0;
		uint64_t MetadataBytes = 0;
		uint64_t PublishedBytes = 0;
		uint64_t GeneratingBytes = 0;
		uint64_t RetiredBytes = 0;
		//@}
	};

	// Checks alignment, level bounds and coordinate range for a canonical brick.
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
	// Detects changes to cadence, accepted weather, timeline or views since the prior pass.
	[[nodiscard]] bool MaintenanceRequired(
		const MaintenanceStamp &stamp,
		std::span<const ResidencyView> views,
		uint64_t tick,
		uint32_t weatherRevision,
		uint64_t timelineRevision,
		const MaintenancePolicy &policy = {}
	);
	// Captures canonical view inputs and accepted versions as a valid maintenance stamp.
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
	// Associates upload work with a generation so publication waits for completion.
	[[nodiscard]] bool
	MarkGenerationSubmitted(ResidencyState &state, uint64_t generation, uint64_t submission);
	// Only a completed GPU submission publishes the entire table and its exact tick/revision.
	[[nodiscard]] bool
	PublishGeneration(ResidencyState &state, uint64_t generation, uint64_t completedSubmission);
	// Extends the published table's GPU reference watermark for this submission.
	[[nodiscard]] bool ReferencePublished(ResidencyState &state, uint64_t submission);
	// Releases retired slots only after their last referencing submission completes.
	void RetireCompleted(ResidencyState &state, uint64_t completedSubmission);
	// Cancels only an unsubmitted generation and returns its reservations to the pool.
	[[nodiscard]] bool CancelGeneration(ResidencyState &state, uint64_t generation);
	// Computes logical live bytes from fixed slots and their current lifetimes.
	[[nodiscard]] ResidencyMemory MemoryOf(const ResidencyState &state);
}
