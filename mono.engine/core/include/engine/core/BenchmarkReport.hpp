#pragma once

// Constant-space CPU frame totals for a complete benchmark run.
// Reported worker and device spans are separate resources, so they are excluded.
// @tier L0 · shared

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string_view>

namespace engine::core {
	// An additional bounded numeric observation supplied by a product.
	struct BenchmarkMetric {
		// Stable JSON key containing only lowercase letters, digits and underscores.
		std::string_view Name;
		// Finite nonnegative measurement.
		double Value = 0.0;
	};

	// Accumulates every completed frame, independently of retained graph history.
	class BenchmarkReport {
	  public:
		// Starts the allocation delta without resetting process-wide profiler state.
		explicit BenchmarkReport(const HeapTotals &initial);
		// Adds one published frame. Ancestors bearing an imagegraph name include
		// child self time once, even when the child has a different category.
		void AddFrame(
			std::span<const FrameSpan> spans,
			double milliseconds,
			size_t dropped,
			const HeapTotals &heap,
			size_t offThreadDropped = 0
		);
		// Number of completed measured update frames.
		uint64_t FrameCount() const;
		// Writes schema 1; rejects incomplete, unavailable or nonfinite measurements.
		bool Write(
			const std::filesystem::path &path,
			double durationSeconds,
			double requiredSeconds,
			const HeapTotals &final,
			bool heapCompiled,
			std::span<const BenchmarkMetric> additional = {}
		) const;

	  private:
		HeapTotals Initial;
		uint64_t Frames = 0;
		uint64_t Dropped = 0;
		uint64_t OffThreadDropped = 0;
		int64_t PeakBytes = 0;
		// Render, physics, network, script, imagegraph, complete frame wall time.
		std::array<double, 6> Milliseconds{};
		// Inclusive outer live scopes for render, physics, network and imagegraph.
		std::array<double, 4> WallMilliseconds{};
		bool Valid = true;
	};
}
