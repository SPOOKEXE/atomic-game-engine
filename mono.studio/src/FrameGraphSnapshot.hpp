#pragma once

#include <engine/render/FrameStatistics.hpp>

#include <cstddef>
#include <utility>

namespace studio::frame_graph_detail {
	constexpr bool ShouldRecordFrameGraph(bool visible, bool paused, bool otherConsumer) {
		return (visible && !paused) || otherConsumer;
	}

	template <class ReadDroppedMarks>
	void UpdateFrameGraphCounters(
		engine::render::FrameSummary &presentation,
		bool &hasPresentation,
		size_t &droppedGpuMarks,
		bool paused,
		const engine::render::FrameStatistics &statistics,
		ReadDroppedMarks &&readDroppedMarks
	) {
		if (paused) {
			return;
		}

		hasPresentation = statistics.HasSamples();
		presentation = hasPresentation ? statistics.Summarise() : engine::render::FrameSummary{};
		droppedGpuMarks = std::forward<ReadDroppedMarks>(readDroppedMarks)();
	}
}
