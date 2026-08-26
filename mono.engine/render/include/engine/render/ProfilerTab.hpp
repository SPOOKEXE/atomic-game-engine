#pragma once

// Which view the F5 profiler panel is showing.
//
// **Its own header for one reason: everything that only needs to *name* a tab
// was paying for the panels.** `--profiler-tab` is a command-line option, so
// `client/Options.hpp` holds one of these - and reaching it through
// `render/DebugPanels.hpp` costs 92,484 preprocessed lines, almost all of it
// `core::FrameGraph`, `core::HeapProfile` and `core::Metrics`, none of which an
// options struct has any business seeing. `docs/ARCH_REVIEW.md` E1 is the same
// finding one layer down.
//
// @tier L12 · client

#include <cstdint>
#include <string_view>

namespace engine::render {

	// A view available in the F5 profiler panel.
	//
	// @client
	enum class ProfilerTab : uint8_t {
		// The flamegraph.
		Frame,
		// Time per category, as bars.
		Categories,
		// Per-system cost from the scheduler.
		Systems,
		// Whatever was written to core::Metrics this frame.
		Counters,

		// Where the live bytes are, and whether they are climbing.
		//
		// **The one tab that is not about this frame.** Everything above
		// reports what the last frame cost, and a frame that is fast and forty
		// megabytes heavier than the one before it reads as healthy on every
		// one of them. So this view leads with a graph of live bytes over
		// minutes and puts the tag tree under it, which is the opposite
		// arrangement to the flamegraph and is the right way round for a
		// quantity whose shape over time is the finding.
		//
		// @since v0.18
		Heap,

		// Number of selectable tabs; not itself a view.
		Count,
	};

	// Returns the lowercase display name of a profiler tab, or `?` for a sentinel or invalid value.
	//
	// @param tab The tab to name.
	// @return A string literal with static lifetime.
	// @client
	std::string_view GetProfilerTabName(ProfilerTab tab);
}
