#pragma once

// The profiler panel's arithmetic, split from its drawing.
//
// **Private, and a header only so that a test can look at it.** Nothing
// outside this module draws these bars or these columns, so this is not public
// surface — the module's own suite reaches `src/` and that is what the private
// include directory is for. `BusyShares` and `BusyMillisecondsOf` lived in
// `DebugPanels.hpp` for exactly this reason and were published to get it,
// which is the one thing the root `AGENTS.md` says not to do to make a test
// easier.
//
// The split exists at all because the SHARE column once read **2634%**: a
// span's `Milliseconds` is inclusive, the denominator was the frame less its
// idle time, and `Renderer::Render` encloses the swapchain wait, so the column
// divided two real numbers that were never the same measurement. What fixed
// that for good was not correcting the arithmetic, it was moving the
// arithmetic somewhere a test could see it.
//
// **Every function here takes the whole panel state rather than a
// denominator.** Handed one as an argument, the arithmetic is right and the
// caller picks the wrong number — which is precisely what happened to the
// categories tab, whose bars were a share of the whole frame while its own
// comment and its own test both said busy. Choosing the denominator has to be
// something these functions do, or it is something nothing checks.
//
// @tier L12 · client

#include <engine/core/FrameGraph.hpp>
#include <engine/render/DebugPanels.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace engine::render {

	// A span's inclusive time less the waiting inside it.
	//
	// **The number the overlay leads with, and the reason the BUSY and IDLE
	// columns are a pair.** `Milliseconds` alone made `Renderer::Render` read
	// 16 ms on a vsynced frame when 15.9 of it was one child blocking on the
	// display, and a reader going after the biggest number went after the
	// renderer — twice, for work it was not doing. This is the half somebody
	// can act on; `FrameSpan::IdleMilliseconds` is the half they cannot.
	//
	// Clamped at zero: the two figures come from separate accumulators and
	// float error must not produce a negative cost.
	//
	// @param span The span.
	// @return Its busy milliseconds.
	inline float BusyMillisecondsOf(const core::FrameSpan &span) {
		return std::max(span.Milliseconds - span.IdleMilliseconds, 0.0f);
	}

	// The share of the frame's busy time each span accounts for.
	//
	// Both sides on one basis, by taking the waiting out of the numerator as
	// well as the denominator. Every result is bounded by 100%, a parent still
	// reads as its whole subtree, and a scope whose entire job is to block
	// reads zero.
	//
	// @param data Borrowed panel state. Reads the spans and the frame's busy
	//             time; retains nothing.
	// @return One share per span, parallel to `data.Spans`.
	std::vector<float> BusyShares(const DebugPanelData &data);

	// Slots in a category-share array: one per category, then unmarked time.
	//
	// Unmarked time rides along rather than being a second return value
	// because it is drawn as one more bar on the same axis — the tab's whole
	// claim is that its bars sum to the busy frame, and a figure computed
	// somewhere else is a figure that can stop agreeing.
	inline constexpr size_t CATEGORY_BAR_COUNT = static_cast<size_t>(core::ProfileCategory::Count) + 1;

	// Index of the unmarked-time bar.
	inline constexpr size_t UNMARKED_BAR = static_cast<size_t>(core::ProfileCategory::Count);

	// Each category's self time as a share of the frame's busy time, with the
	// frame's unmarked time in the last slot.
	//
	// **Busy, not the whole frame.** Category totals are self time and idle is
	// a category of its own, so the non-idle totals plus the unmarked
	// remainder are exactly the busy frame — the bars are a partition of it
	// and read as one. Against the whole frame they are a partition of
	// something they do not cover: with vertical sync on, fifteen of a sixteen
	// millisecond frame are a sleep, and every bar renders at four per cent of
	// its track whatever the engine is doing.
	//
	// `Idle`'s own slot is zero rather than its share. It is not part of the
	// denominator, so a share of it is not a quantity; the tab draws the
	// figure in its header instead, where it means something.
	//
	// Clamped to 1.0, because a `Reported` span carries time measured on
	// another thread and is not subtracted from its parent — eight workers can
	// honestly contribute more self time than the frame has wall clock, and a
	// bar wider than its track draws across the game rather than off the end
	// of the panel.
	//
	// @param data Borrowed panel state. Reads the spans, the frame and its
	//             idle time, and the unmarked remainder; retains nothing.
	// @return One share per slot, each in [0, 1].
	std::array<float, CATEGORY_BAR_COUNT> CategoryShares(const DebugPanelData &data);
}
