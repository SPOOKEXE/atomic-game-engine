#pragma once

// The three in-game debug panels.
//
//   F3 - the statistics counter: FPS now, and the shape of the last twenty
//        seconds. A number that only shows the current frame hides exactly the
//        thing worth seeing, which is the occasional 40 ms one.
//
//   F4 - the network panel: what is crossing the link, what it costs per
//        second, and how far behind the world being drawn is. Only when there
//        is a link - see `NetworkStatistics::Connected`.
//
//   F5 - the frame graph: last frame's scope tree as a flamegraph, plus
//        per-category totals, per-system costs, the metrics counters, and the
//        heap.
//
// All three draw into an OverlayImage and know nothing about the GPU.
//
// @tier L12 · client

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/render/FrameStatistics.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/render/ProfilerTab.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::render {

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

	// What is crossing a replication link, and what it costs.
	//
	// **Rates are per second and are the caller's to measure, not this
	// module's.** Everything `net` counts is cumulative over a connection's
	// life, and a panel that divided a lifetime total by a lifetime would show
	// a number that stops moving after a minute - the figure worth reading is
	// over the last second or so, and only the caller knows when it last
	// sampled. `DebugPanels` reads no clock, which is what keeps it drawable
	// from a test.
	//
	// @since v0.5
	// @client
	struct NetworkStatistics {
		// Whether there is a link at all.
		//
		// **False switches the panel off rather than drawing zeroes.** A client
		// with no `--connect` has no network to show, and a panel of zeroes
		// reads as a link that is up and idle - which is a different and much
		// more alarming thing than a client that was never asked to connect.
		bool Connected = false;

		// Whether the joining snapshot has arrived and been applied.
		//
		// Connected and not joined is the state worth seeing: the handshake
		// finished and the world has not landed.
		bool Joined = false;

		// Payload bytes per second arriving, measured by the caller over its own
		// recent window.
		double ReceivedBytesPerSecond = 0.0;

		// Payload bytes per second going out, over the same window.
		double SentBytesPerSecond = 0.0;

		// Packets per second arriving, over the same window.
		double ReceivedPacketsPerSecond = 0.0;

		// Packets per second going out, over the same window.
		double SentPacketsPerSecond = 0.0;

		// Payload bytes accepted over the connection's life.
		uint64_t ReceivedBytes = 0;

		// Payload bytes handed to the transport over the connection's life.
		uint64_t SentBytes = 0;

		// Round trip in milliseconds, smoothed by `net::Link`.
		float RoundTripMilliseconds = 0.0f;

		// Gaps in the far side's sequence numbers.
		//
		// An estimate: a late packet and a lost one are the same thing until
		// one of them turns up. `net::ConnectionStats::PacketsLost`.
		uint64_t PacketsLost = 0;

		// Unreliable packets discarded because a newer one had already
		// arrived. Near zero on a real network is the surprising reading.
		uint64_t PacketsStale = 0;

		// Sends refused because the byte budget for the tick was spent.
		//
		// **Not congestion.** This is the budget being enforced, and `D00007`
		// was found by this number coming off zero.
		uint64_t SendsOverBudget = 0;

		// Ticks per second the replica is actually receiving.
		//
		// Measured from the stream rather than configured - the server's rate
		// is not on the wire and the two programs do not share a default, so
		// this is the only figure that is about the authority rather than about
		// what this process was told. `replication::SnapshotBuffer`.
		double TickRate = 0.0;

		// The last tick applied in full.
		uint64_t AppliedTick = 0;

		// Snapshots applied. More than one means the server decided this client
		// had fallen too far behind to catch up with deltas.
		uint64_t Snapshots = 0;

		// Deltas applied.
		uint64_t Deltas = 0;

		// Structural messages applied - creations, destroys and forgets.
		uint64_t Structures = 0;

		// Messages refused as malformed.
		uint64_t Malformed = 0;

		// Messages about a tick already passed. Ordinary on an unreliable
		// transport, which reorders; a figure that climbs is a link delivering
		// more late than useful.
		uint64_t Stale = 0;

		// How far behind the newest received tick the world is being drawn, in
		// ticks. The snapshot buffer's jitter budget, as it stands right now.
		double BehindTicks = 0.0;

		// Frames the render clock could not advance because it had run out of
		// received state.
		//
		// **The number that says the delay is too small for this link.**
		uint64_t Stalls = 0;

		// Poses answered by interpolating between two received ticks. Against
		// `Held` below, this is the ratio that says whether the world is
		// actually being smoothed.
		uint64_t Interpolated = 0;

		// Poses answered by holding a single tick. All held and none
		// interpolated is a buffer doing nothing.
		uint64_t Held = 0;

		// Entities the replica holds.
		uint64_t Entities = 0;

		// Rows the draw pass produced from them. Below `Entities` is rows that
		// arrived without something to draw them with.
		uint64_t Drawn = 0;
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

		// Whether to draw the F4 network panel.
		//
		// **Asking for it is not enough.** It is drawn only when
		// `Network.Connected` is also true, so a build with no replication in
		// it cannot be made to show an empty one. `DrawDebugPanels` enforces
		// that rather than trusting the caller - the whole reason the panel
		// exists is to answer "is anything crossing", and a panel of zeroes
		// answers it wrongly.
		bool ShowNetwork = false;

		// Whether to draw the F5 profiler panel.
		bool ShowFrameGraph = false;

		// Profiler view to draw.
		ProfilerTab Tab = ProfilerTab::Frame;

		// Number of visible body rows to skip from the top.
		int Scroll = 0;

		// Whether an external Tracy client is currently attached.
		bool TracyAttached = false;

		// How deep the flamegraph goes. A span below it is not drawn and its
		// nearest visible ancestor is marked with a "+" instead - a hidden
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
		// heading above them - see core::FrameGraph::UnmarkedMilliseconds.
		float UnmarkedMilliseconds = 0.0f;

		// How much of that frame was spent waiting rather than working.
		//
		// Subtracted from the frame to get the figure the SHARE column is a
		// share of. With vertical sync on, fifteen of a sixteen millisecond
		// frame are a sleep, and a share of the whole frame would report every
		// span that did something as one per cent of it - a panel on which
		// nothing is ever worth optimising.
		float IdleMilliseconds = 0.0f;

		// The frame less the waiting: what there is to make faster.
		float BusyMilliseconds() const {
			return FrameMilliseconds > IdleMilliseconds ? FrameMilliseconds - IdleMilliseconds
														: FrameMilliseconds;
		}

		// Number of spans omitted while the published frame was collected.
		size_t DroppedSpans = 0;

		// GPU timestamp writes omitted because the per-frame query budget filled.
		size_t DroppedGpuMarks = 0;

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

		// What is crossing the replication link, when there is one.
		NetworkStatistics Network;

		// The process heap totals, as `core::HeapProfile::Totals` reports them.
		core::HeapTotals Heap;

		// Whether the allocator hooks were compiled into this program.
		//
		// **Drawn as a sentence rather than as an empty tree.** A `release`
		// build has no hooks, so every figure here is zero - and a heap panel
		// reading zero bytes is a far more alarming thing than one saying it was
		// not compiled in.
		bool HeapCompiledIn = false;

		// Borrowed tag tree rows in draw order, from `HeapProfile::TreeRows`.
		std::span<const core::HeapTreeRow> HeapRows;

		// Borrowed live-byte readings, oldest first, from `HeapProfile::History`.
		//
		// This is the graph. Its cadence is the caller's - every program here
		// samples once a second - so the width of the plot is a span of minutes
		// rather than of frames.
		std::span<const core::HeapSample> HeapHistory;

		// Borrowed growth report, steepest first, from `HeapProfile::Growth`.
		//
		// **Computed by the caller when it samples rather than when the panel
		// draws.** Fitting a slope to every tracked node is a pass over the
		// whole retained window, and doing it at the panel's repaint rate would
		// make the profiler the most expensive thing in the frame.
		std::span<const core::HeapGrowth> HeapGrowth;

		// Seconds the growth figures are fitted over.
		double HeapHistorySeconds = 0.0;

		// Logical GPU resource payload sampled with the process heap. SDL has no
		// portable driver-heap query, so this covers buffers, transfer buffers,
		// textures, mip levels, and samples rather than backend allocation slack.
		//@{
		uint64_t GpuHeapLiveBytes = 0;
		uint64_t GpuHeapPeakBytes = 0;
		uint64_t GpuAllocatedBytes = 0;
		uint64_t GpuReleasedBytes = 0;
		uint64_t GpuBufferAllocations = 0;
		uint64_t GpuTransferBufferAllocations = 0;
		uint64_t GpuTextureAllocations = 0;
		uint64_t GpuBufferBytes = 0;
		uint64_t GpuTransferBufferBytes = 0;
		uint64_t GpuTextureBytes = 0;
		std::span<const uint64_t> GpuHeapHistory;
		//@}

		// Positive integer pixel scale, raised on high-DPI displays to keep the panels legible.
		int Scale = 2;
	};

	// The share arithmetic these panels are drawn from - `BusyShares`,
	// `BusyMillisecondsOf`, `CategoryShares` - is in `src/PanelShares.hpp`.
	//
	// It was here, published so that the suite could reach it, which is the
	// one thing the root `AGENTS.md` names: do not widen a public header to
	// make a test easier. Nothing outside this module ever called any of it.

	// Clears the image and draws whatever is switched on. Draws nothing, and
	// leaves the image clean, when both panels are off - which is what lets the
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
