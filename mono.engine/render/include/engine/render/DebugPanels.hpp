#pragma once

// The two in-game debug panels.
//
//   F3 — the statistics counter: FPS now, and the shape of the last twenty
//        seconds. A number that only shows the current frame hides exactly the
//        thing worth seeing, which is the occasional 40 ms one.
//
//   F5 — the frame graph: last frame's scope tree as a flamegraph, plus
//        per-category totals, per-system costs and the metrics counters.
//
// Both draw into an OverlayImage and know nothing about the GPU.
//
// @tier L12 · client

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/render/Overlay.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::render {

	// Everything the statistics panel shows, from one walk of the window.
	//
	// The panel used to ask for these one at a time, and each question walked
	// the whole window to answer it. At a few hundred frames a second a twenty
	// second window is tens of thousands of samples, so four questions was four
	// passes over a quarter of a megabyte, every frame, to draw seven lines of
	// text — and it cost more than the scene did.
	//
	// @client
	struct FrameSummary {
		// Frames per second, from the most recent sample.
		float Current = 0.0f;

		// Duration of the most recent frame, in milliseconds.
		float CurrentMilliseconds = 0.0f;

		// Frames per second of the *worst* frame in the window.
		float Minimum = 0.0f;

		// Frames per second from the mean frame duration over the window.
		float Average = 0.0f;

		// Frames per second of the best frame in the window.
		float Maximum = 0.0f;

		// Mean absolute change between consecutive frames, in milliseconds.
		float Jitter = 0.0f;
	};

	// A rolling window of frame times. Twenty seconds, because a hitch every
	// ten is a thing you have to be able to sit and watch for.
	// Queries return zero while the window is empty.
	//
	// @client
	class FrameStatistics {
	  public:
		// Length of the rolling sample window, in seconds.
		static constexpr double WINDOW_SECONDS = 20.0;

		// Records one positive frame duration and removes samples older than the window.
		//
		// @param now          Current monotonic time, in seconds.
		// @param deltaSeconds Duration of the frame, in seconds. Non-positive values are ignored.
		void Record(double now, float deltaSeconds);

		// Removes every recorded sample.
		void Clear();

		// Reports whether the window contains at least one sample.
		bool HasSamples() const;

		// Every figure below, from a single pass over the window.
		//
		// What anything drawing the panel should call. The individual accessors
		// are each a walk of the whole window and are kept for callers that want
		// exactly one number; asking for several of them in a row is asking for
		// the same walk several times.
		FrameSummary Summarise() const;

		// Frames per second, from the most recent sample.
		float Current() const;

		// Over the window. Minimum FPS is the worst frame, which is the one
		// that matters — an average of 144 with a floor of 12 is not a smooth
		// game.
		float Minimum() const;

		// Frames per second from the mean frame duration over the window.
		float Average() const;

		// Maximum frames per second over the window.
		float Maximum() const;

		// Duration of the most recent frame, in milliseconds.
		float CurrentMilliseconds() const;

		// Mean absolute change between consecutive frames. A high average FPS
		// with high jitter reads as stutter, and neither number alone says so.
		// Returns milliseconds, or zero when fewer than two samples exist.
		float Jitter() const;

		// Number of frame samples currently inside the rolling window.
		size_t SampleCount() const {
			return Count;
		}

	  private:
		struct Sample {
			double Time = 0.0;
			float Delta = 0.0f;
		};

		// A ring over a flat vector, not a deque.
		//
		// The access pattern is one push and a few pops from the front per
		// frame, which is what a deque is for — but the *reads* are full walks
		// of tens of thousands of samples, and a deque walks them through a
		// table of chunk pointers. A ring over contiguous storage is the same
		// O(1) at both ends and a straight line to read, which is the operation
		// that was actually costing something.
		//
		// It also stops allocating once the window has filled, where the deque
		// released and reacquired a chunk every time the window slid across a
		// boundary.
		std::vector<Sample> Ring;

		// Index of the oldest sample. Ring is empty until the first Record.
		size_t Head = 0;

		// Live samples, which is not Ring.size() — that is the capacity.
		size_t Count = 0;

		// Running totals, maintained as samples arrive and leave.
		//
		// A walk of the window was the obvious way to answer these, and it had
		// the wrong shape: the number of samples in twenty seconds is the frame
		// *rate* times twenty, so every frame the game got faster this got
		// slower. Something that costs more the better the thing it measures
		// performs is not a measurement anybody can leave switched on.
		// Mutable for the same reason the extremes are: Rescan below rebuilds
		// them, and it runs from a const query.
		mutable double DeltaSum = 0.0;

		// Sum of the absolute change between consecutive samples, which is what
		// Jitter is a mean of. A sample leaving takes exactly one pair with it.
		mutable double ChangeSum = 0.0;

		// The extremes, cached. Mutable because they are rebuilt lazily by a
		// const query rather than eagerly by every Record.
		mutable float Worst = 0.0f;
		mutable float Best = 0.0f;

		// Set when the sample that *was* an extreme leaves the window, because
		// nothing cheaper than another look can say what the next one is.
		//
		// The worst frame in twenty seconds ages out about once every twenty
		// seconds, so this is rare — and in the pathological case where it is
		// every frame, the cost is the walk this replaced. Never worse.
		mutable bool ExtremesStale = false;

		// Rebuilds the extremes, and the sums with them.
		//
		// The sums are recomputed here rather than only accumulated because a
		// running total that is only ever added to and subtracted from drifts,
		// and this pass is already touching every sample.
		void Rescan() const;

		// Ring index of the nth-oldest sample.
		size_t IndexOf(size_t offset) const {
			return (Head + offset) % Ring.size();
		}
	};

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

		// Number of selectable tabs; not itself a view.
		Count,
	};

	// Returns the lowercase display name of a profiler tab, or `?` for a sentinel or invalid value.
	//
	// @param tab The tab to name.
	// @return A string literal with static lifetime.
	// @client
	std::string_view GetProfilerTabName(ProfilerTab tab);

	// One scheduler system's elapsed time for the current frame.
	//
	// The name is borrowed and must remain valid until DrawDebugPanels returns.
	//
	// @client
	struct SystemTiming {
		// Borrowed system name.
		std::string_view Name;

		// Accumulated system time for the frame, in milliseconds.
		float Milliseconds = 0.0f;
	};

	// Everything the panels draw, gathered by the caller. Passed by reference
	// and never stored: the spans point into the frame graph's published frame,
	// which is only stable until the next EndFrame.
	//
	// All pointers, spans, and string views are borrowed. Their storage must
	// remain valid until DrawDebugPanels returns.
	//
	// @client
	struct DebugPanelData {
		// Whether to draw the F3 statistics panel.
		bool ShowStatistics = false;

		// Whether to draw the F5 profiler panel.
		bool ShowFrameGraph = false;

		// Profiler view to draw.
		ProfilerTab Tab = ProfilerTab::Frame;

		// Number of visible body rows to skip from the top.
		int Scroll = 0;

		// Whether an external Tracy client is currently attached.
		bool TracyAttached = false;

		// How deep the flamegraph goes. A span below it is not drawn and its
		// nearest visible ancestor is marked with a "+" instead — a hidden
		// subtree that leaves no trace makes a parent look like a leaf, and
		// "collect-instances 8 ms" means two different things depending on
		// whether that is all of it.
		//
		// Defaults to everything. Past core::FrameGraph::MAXIMUM_DEPTH nothing
		// was recorded, so there is nothing deeper to reveal.
		uint32_t DepthLimit = core::FrameGraph::MAXIMUM_DEPTH;

		// Borrowed frame-rate statistics, or null while no statistics source is available.
		const FrameStatistics *Statistics = nullptr;

		// Borrowed spans from the frame graph's published previous frame.
		std::span<const core::FrameSpan> Spans;

		// Total duration of the published frame, in milliseconds.
		float FrameMilliseconds = 0.0f;

		// How much of that frame ran inside no span. Drawn as a row of its own
		// and as a bar of its own, so the rows a person reads add up to the
		// heading above them — see core::FrameGraph::UnmarkedMilliseconds.
		float UnmarkedMilliseconds = 0.0f;

		// How much of that frame was spent waiting rather than working.
		//
		// Subtracted from the frame to get the figure the SHARE column is a
		// share of. With vertical sync on, fifteen of a sixteen millisecond
		// frame are a sleep, and a share of the whole frame would report every
		// span that did something as one per cent of it — a panel on which
		// nothing is ever worth optimising.
		float IdleMilliseconds = 0.0f;

		// The frame less the waiting: what there is to make faster.
		float BusyMilliseconds() const {
			return FrameMilliseconds > IdleMilliseconds ? FrameMilliseconds - IdleMilliseconds
														: FrameMilliseconds;
		}

		// Number of spans omitted while the published frame was collected.
		size_t DroppedSpans = 0;

		// How much history the RMAX column is a maximum over. Shown because a
		// worst-case over a fifth of a second and one over five seconds are
		// different claims, and the column looks identical either way.
		double HistorySeconds = 0.0;

		// Borrowed scheduler timings for the current frame.
		std::span<const SystemTiming> Systems;

		// Borrowed metrics drained for the current frame.
		std::span<const core::Counter> Counters;

		// Number of drawable entities in the world.
		uint64_t Entities = 0;

		// Draw calls submitted by the previous rendered frame.
		uint64_t DrawCalls = 0;

		// Triangles submitted by the previous rendered frame.
		uint64_t Triangles = 0;

		// Borrowed GPU backend name, or an empty view when unknown.
		std::string_view Backend;

		// Configured simulation tick rate, in hertz. This is not the frame rate,
		// which is the point of showing
		// both. A render rate on its own cannot distinguish "the frame is
		// slow" from "the simulation is behind", and those need different
		// fixes. Zero means the caller does not tick separately.
		double TickRate = 0.0;

		// Simulation ticks measured over the caller's recent window, in ticks per second.
		float TicksPerSecond = 0.0f;

		// Total simulation ticks discarded by the fixed-timestep catch-up limit.
		uint64_t DroppedTicks = 0;

		// Positive integer pixel scale, raised on high-DPI displays to keep the panels legible.
		int Scale = 2;
	};

	// Clears the image and draws whatever is switched on. Draws nothing, and
	// leaves the image clean, when both panels are off — which is what lets the
	// renderer skip the upload.
	//
	// An empty image, such as one for a minimised window, is cleared safely and
	// remains clean. No data from `data` is retained after the call.
	//
	// @param image CPU image to clear and draw into.
	// @param data  Borrowed panel state and profiler data.
	// @client
	void DrawDebugPanels(OverlayImage &image, const DebugPanelData &data);
}
