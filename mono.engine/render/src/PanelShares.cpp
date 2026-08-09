#include "PanelShares.hpp"

#include <algorithm>
#include <array>

namespace engine::render {

	// The panels are drawn by rasterising glyphs into a CPU image, one pixel at
	// a time, every frame. That is the right trade for a tool that has to work
	// with no second process attached — but it is not free, and until the
	// profiling scopes existed the whole cost landed as self time on whatever
	// the caller had opened around it. A panel reporting a frame it is a large
	// part of, and not saying so, is the one measurement error a profiler
	// cannot afford.
	//
	// The reading is of the *previous* frame, so the panel does show its own
	// cost. That is not a flaw to be corrected away: the observer is part of the
	// frame while it is open, and hiding that would make closing the panel look
	// like it fixed something.
	std::vector<float> BusyShares(const DebugPanelData &data) {
		const float denominator = std::max(data.BusyMilliseconds(), 0.0001f);

		// The walk that used to be here now happens once in `FrameGraph`, where
		// `RecordHistory` needs the same answer — a share taken from busy time
		// beside an RMAX taken from wall time is two numbers on one row that
		// contradict each other.
		std::vector<float> shares(data.Spans.size(), 0.0f);
		for (size_t index = 0; index < data.Spans.size(); index++) {
			shares[index] = BusyMillisecondsOf(data.Spans[index]) / denominator;
		}
		return shares;
	}

	std::array<float, CATEGORY_BAR_COUNT> CategoryShares(const DebugPanelData &data) {
		// **Busy, and this line is the whole fix.** The frame is the wrong
		// denominator for a set of bars that idle is not one of, and the same
		// floor the flamegraph uses keeps a frame that measured zero — the
		// first one, or one taken across a coarse clock — from dividing by it.
		const float denominator = std::max(data.BusyMilliseconds(), 0.0001f);

		std::array<float, CATEGORY_BAR_COUNT> shares{};

		// One pass over the frame for every bar. Asked a category at a time
		// this walked the whole span list once per category, which is a cost
		// that grew every time somebody added one.
		for (const auto &span : data.Spans) {
			const auto index = static_cast<size_t>(span.Category);
			// A category outside the enum can only arrive from a caller
			// filling `FrameSpan` by hand, and dropping it is better than
			// writing past the array — the panels are the thing that says
			// whether the frame is trustworthy, so they do not get to be the
			// thing that corrupts it.
			if (index < static_cast<size_t>(core::ProfileCategory::Count)) {
				shares[index] += span.SelfMilliseconds;
			}
		}
		shares[UNMARKED_BAR] = data.UnmarkedMilliseconds;

		// Idle last, and to zero. It is not in the denominator, so its share
		// is not a quantity; leaving its accumulated milliseconds in the slot
		// would let a caller divide them by a total they are not part of.
		shares[static_cast<size_t>(core::ProfileCategory::Idle)] = 0.0f;

		for (float &share : shares) {
			share = std::clamp(share / denominator, 0.0f, 1.0f);
		}
		return shares;
	}
}
