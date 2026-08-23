#pragma once

// Data shaping for Studio's frame-graph panel.
//
// The panel itself needs imgui, but averaging a recorded tree and assigning
// non-overlapping display rows do not. Keeping those operations here makes the
// silent failures testable without a window or a graphics device.
//
// @tier L13 · client

#include <engine/core/FrameGraph.hpp>

#include <span>
#include <string>
#include <vector>

namespace studio {

	// One frame-graph span retained past the frame that produced it.
	//
	// `core::FrameSpan` borrows its name. Studio snapshots own the text because
	// an interval can retain it for several seconds.
	struct DiagnosticSpan {
		std::string Name;
		uint32_t Depth = 0;
		uint32_t Parent = engine::core::FrameGraph::NO_PARENT;

		float StartMilliseconds = 0.0f;
		float Milliseconds = 0.0f;
		float SelfMilliseconds = 0.0f;
		float IdleMilliseconds = 0.0f;

		engine::core::ProfileCategory Category = engine::core::ProfileCategory::Engine;
		bool Reported = false;

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

	// Fits reported worker work into the measured timeline used for drawing.
	//
	// Reported durations are CPU work totals and may exceed wall time when
	// workers overlap. Direct reported children are scaled into their measured
	// parent's largest uncovered interval, then their descendants are fitted
	// proportionally inside that logical bar. Call this on a display copy: the
	// table and tooltip must retain the producer's actual milliseconds.
	void FitReportedDiagnosticTimeline(std::vector<DiagnosticSpan> &spans, float frameMilliseconds);

	// Adds display-only children for time inside a span that none of its direct
	// children covers. Reported worker spans are logical work rather than wall
	// intervals, so they neither cover measured gaps nor receive synthetic gaps.
	// The input tree stays intact and new spans are appended, so recorded parent
	// indices remain valid.
	void AppendUnaccountedDiagnosticSpans(std::vector<DiagnosticSpan> &spans);

	// Assigns a non-overlapping display row to every span.
	//
	// Rows remain grouped by tree depth. Overlapping siblings take separate
	// lanes; spans under different parents reuse the same lanes because their
	// parent branches already explain the overlap.
	// `minimumMilliseconds` is the graph's one-pixel minimum expressed in time.
	//
	// @return How many rows the graph needs.
	uint32_t LayoutDiagnosticRows(
		std::span<const DiagnosticSpan> spans,
		float frameMilliseconds,
		float minimumMilliseconds,
		std::vector<uint32_t> &rows
	);
}
