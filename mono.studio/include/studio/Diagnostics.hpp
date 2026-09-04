#pragma once

// arch-waiver public-header: forward studio API. Editor integrations retain
// this complete diagnostics contract.

// Data shaping for Studio's frame-graph panel.
//
// The panel itself needs imgui, but averaging a recorded tree and assigning
// stable hierarchy rows do not. Keeping those operations here makes the silent
// failures testable without a window or a graphics device.
//
// @tier L13 · client

#include <engine/core/FrameGraph.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studio {

	// One frame-graph span retained past the frame that produced it.
	//
	// `core::FrameSpan` borrows its name. Studio snapshots own the text because
	// an interval can retain it for several seconds.
	struct DiagnosticSpan {
		// Owned identity and retained hierarchy coordinates.
		//@{
		std::string Name;
		uint32_t Depth = 0;
		uint32_t Parent = engine::core::FrameGraph::NO_PARENT;
		//@}

		// Start, inclusive, self, and idle durations in milliseconds.
		//@{
		float StartMilliseconds = 0.0f;
		float Milliseconds = 0.0f;
		float SelfMilliseconds = 0.0f;
		float IdleMilliseconds = 0.0f;
		//@}

		// Work category, submitting product, and whether producer work reported
		// the span.
		//@{
		engine::core::ProfileCategory Category = engine::core::ProfileCategory::Engine;
		engine::core::ProfileOwner Owner = engine::core::ProfileOwner::Engine;
		bool Reported = false;
		//@}

		// How many frames contributed to this structural occurrence. Repeated
		// siblings get separate rows, so this is at most the interval's frame
		// count rather than the number of times a name appeared anywhere.
		uint32_t Occurrences = 0;
	};

	// Adds one recorded frame to a structural average.
	//
	// Spans are matched by parent, name and same-name sibling ordinal. That
	// preserves separate world and phase trees when the same scheduler names
	// occur more than once in a frame.
	void AccumulateDiagnosticSpans(
		std::span<const engine::core::FrameSpan> frame, std::vector<DiagnosticSpan> &totals
	);

	// Converts accumulated totals to one average frame.
	//
	// Durations divide by all frames so an intermittent span contributes only
	// when it ran. Starts divide by occurrences because an absent span has no
	// meaningful start position.
	void FinishDiagnosticAverage(std::vector<DiagnosticSpan> &spans, uint32_t frames);

	// Copies one owner view while retaining a valid tree. A matching span whose
	// recorded parent belongs to another owner is reparented to its nearest
	// matching ancestor, or becomes a root. `All` copies the complete tree.
	//
	// @since v0.22
	void FilterDiagnosticSpans(
		std::span<const DiagnosticSpan> spans,
		engine::core::ProfileOwner owner,
		std::vector<DiagnosticSpan> &filtered
	);

	// Copies one span and every descendant into a standalone display tree.
	// `sourceIndices` maps each copied span to the source index so a focused
	// graph can navigate back to its parent in the full display tree.
	void FocusDiagnosticSpans(
		std::span<const DiagnosticSpan> spans,
		uint32_t root,
		std::vector<DiagnosticSpan> &focused,
		std::vector<uint32_t> &sourceIndices
	);

	// Fits reported worker work into the measured timeline used for drawing.
	//
	// Reported durations are CPU work totals and may exceed wall time when
	// workers overlap. Direct reported children are scaled into their measured
	// parent's largest uncovered interval, then their descendants are fitted
	// proportionally inside that logical bar. Descendants are finally clipped to
	// their parent, so a projected subtree cannot overlap the next measured
	// sibling. Call this on a display copy: the table and tooltip must retain the
	// producer's actual milliseconds.
	void FitReportedDiagnosticTimeline(std::vector<DiagnosticSpan> &spans, float frameMilliseconds);

	// Adds display-only children for time inside a span that none of its direct
	// children covers. Reported worker spans are logical work rather than wall
	// intervals, so they neither cover measured gaps nor receive synthetic gaps.
	// The input tree stays intact and new spans are appended, so recorded parent
	// indices remain valid.
	void AppendUnaccountedDiagnosticSpans(std::vector<DiagnosticSpan> &spans);

	// Assigns one stable display row to each hierarchy level.
	//
	// A valid child is always exactly one row below its parent. Timing overlap
	// does not create another row: reported worker summaries intentionally share
	// wall time with the measured wait that contains them, and pixel-sized bars
	// may overlap after scaling. Turning either into a lane makes every deeper
	// span jump vertically when the frame timing changes.
	//
	// @return How many rows the graph needs.
	uint32_t LayoutDiagnosticRows(std::span<const DiagnosticSpan> spans, std::vector<uint32_t> &rows);

	// Human explanation shown for every flame-graph bar. Known hot paths get a
	// specific contract; dynamic names fall back to their category, so no bar
	// has a tooltip that merely repeats its label.
	std::string_view
	DescribeDiagnosticSpan(std::string_view name, engine::core::ProfileCategory category, bool reported);
}
