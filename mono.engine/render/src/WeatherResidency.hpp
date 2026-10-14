#pragma once

// Sparse weather topology derived from current ECS inputs and the union of views.
// The graph owns device generations; this cache retains only disposable coordinate metadata.

#include <engine/scene/WeatherCompute.hpp>
#include <engine/weather/Assemblies.hpp>
#include <engine/weather/Residency.hpp>

#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::render {

	inline constexpr uint32_t WEATHER_RESIDENCY_PAGES = 512;
	inline constexpr uint32_t WEATHER_COARSE_PAGES = 128;
	inline constexpr uint32_t WEATHER_MAINTENANCE_CHUNK = 128;
	inline constexpr uint32_t WEATHER_CLASSIFICATION_CHUNK = 8;
	inline constexpr float WEATHER_LOD_TRANSITION_SECONDS = 0.5f;

	struct WeatherResidencyCandidate {
		weather::BrickKey Key;
		float DistanceMetres = 0.0f;
		float ProjectedPixels = 0.0f;
	};

	struct WeatherResidencyView {
		weather::ResidencyView View;
		float ProjectionPixelsPerRadian = 960.0f;
		float MaximumDistanceMetres = 32768.0f;
	};

	struct WeatherResidencyCache {
		weather::MaintenanceStamp Maintenance;
		std::vector<weather::BrickKey> Selected;
		// Canonical origin X/Y/Z and LOD; upload adapters add storage kinds and word offsets.
		std::vector<glm::ivec4> PageKeys;
		uint64_t InputSignature = 0;
		uint64_t TopologySignature = 0;
		uint64_t GeometrySignature = 0;
		uint32_t CoarsePages = 0;
		uint32_t RefusedRefinements = 0;
		bool RequiresProceduralFallback = false;
		// A bounded candidate pass spans calls, using one canonical sample time throughout.
		std::vector<weather::BrickDemand> WorkDemands;
		std::vector<WeatherResidencyCandidate> WorkCandidates;
		std::array<int64_t, 3> WorkMinimum{};
		std::array<int64_t, 3> WorkMaximum{};
		weather::WeatherTime WorkTime;
		uint32_t WorkEvaluations = 0;
		uint32_t WorkCoarseCount = 0;
		uint64_t LastAdvanceSequence = 0;
		uint64_t NextMaintenanceTick = 0;
		bool WorkActive = false;
		std::vector<glm::ivec4> PreviousPageKeys;
		uint64_t TransitionStartTick = 0;
		uint32_t TransitionDurationTicks = 0;
		double TransitionStartSeconds = 0.0;
		bool TransitionUsesPresentationClock = false;
		// 0: dense, 1: compact scalar totals with child winds, 2: certified all-time empty.
		std::vector<uint8_t> PageKinds;
		uint32_t ClassificationIndex = 0;
		uint32_t FieldWords = 0;
		bool ClassificationActive = false;
	};

	// Spans remain valid until this cache's next topology rebuild; upload while recording.
	struct WeatherResidencyPlan {
		std::span<const glm::ivec4> Pages;
		uint64_t Signature = 0;
		uint64_t MaintenanceAgeTicks = 0;
		uint32_t CoarsePages = 0;
		uint32_t RefusedRefinements = 0;
		float MotionMarginMetres = 0.0f;
		bool Rebuilt = false;
		bool RequiresProceduralFallback = false;
		std::span<const glm::ivec4> PreviousPages;
		float TransitionAlpha = 1.0f;
		uint32_t CandidateEvaluations = 0;
		bool MaintenancePending = false;
		uint64_t MetadataBytes = 0;
		std::span<const uint8_t> PageKinds;
		uint32_t FieldWords = 0;
		uint32_t ClassifiedPages = 0;
	};

	[[nodiscard]] std::optional<WeatherResidencyPlan> BuildWeatherResidency(
		WeatherResidencyCache &cache,
		const scene::WeatherCompute &controls,
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		uint64_t timelineRevision,
		std::span<const WeatherResidencyView> views,
		uint64_t frameSequence = 0,
		double presentationSeconds = -1.0
	);
	// Reference helpers for tests and graph adapters. No CPU copy of the resident field is retained.
	[[nodiscard]] uint64_t WeatherResidencyMetadataBytes(const WeatherResidencyCache &cache);
	[[nodiscard]] std::optional<weather::AssemblySummary> WeatherPageAssembly(
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		const weather::BrickKey &page
	);
	// Explicit completion IDs are available for adapters whose device backend exposes fences.
	// SDL's graph instead owns cycling resources and commits its history after successful submission.
	[[nodiscard]] std::optional<weather::PageGeneration> ReserveWeatherPages(
		weather::ResidencyState &state,
		const WeatherResidencyCache &cache,
		uint64_t tick,
		uint32_t revision,
		uint64_t completedSubmission
	);
}
