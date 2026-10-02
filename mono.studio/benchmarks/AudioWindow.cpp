// Actual panel Update owners exclude verification and capture reporting.
#include "../../mono.engine/imagegraph/tests/fixtures/AudioWindowObservation.hpp"
#include "AudioWindowPanel.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
TEST_SUITE_ID("studio.bench.audio-window")
TEST_DEPENDS("studio.audio_window_panel")
namespace audio_window_bench {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr size_t UPDATES = 16;
	constexpr size_t POINTS = 320;
	constexpr std::string_view OWNER = "studio audio window benchmark";
	constexpr std::string_view OBSERVATION = "image composer audio window observation";
	constexpr std::string_view CORE = "imagegraph.audio_window.observe";
	constexpr std::string_view GEOMETRY = "imagegraph.audio_window.geometry";
	enum class Mode { Unchanged, Cursor, SourceRevision, FailureRecovery };
	void Require(bool ok, const char *message) {
		if (!ok) throw std::runtime_error(message);
	}
	bool CaptureEnabled() {
		const char *v = std::getenv("ATOMIC_STUDIO_AUDIO_WINDOW_PROFILE");
		return v && std::string_view(v) == "1";
	}
	struct Fixture {
		testing::AudioWindowObservationFixture Graph;
		studio::AudioWindowPanel Panel;
		FrameTime Clock{64, .5, false};
		uint64_t Revision = 1;
		size_t Source = 0, Operation = 0, Calls = 0;
		Diagnostic Error;
		uint64_t OriginalInputHash;
		uint64_t InputPayloadBytes = 0;
		explicit Fixture(size_t packets) : Graph(packets), OriginalInputHash(Graph.InputHash) {
			for (const auto &s : Graph.Sources)
				for (const auto &plane : s.Data.Channels)
					InputPayloadBytes += plane.capacity() * sizeof(double);
			Update(true);
			Verify();
		}
		int Row(Mode mode) const {
			return int(mode) + (Graph.Packets == 65536 ? 4 : 0);
		}
		void VerifyInputs() const {
			Graph.VerifyInputs();
		}
		void Prepare(Mode mode) {
			++Operation;
			if (mode == Mode::Cursor) {
				constexpr std::array<FrameTime, 4> clocks{
					{{64, .5, false}, {64, .5, true}, {0, .5, false}, {0, .5, true}}
				};
				Clock = clocks[Operation % clocks.size()];
			}
			if (mode == Mode::SourceRevision) {
				Source = Operation & 1;
				++Revision;
			}
			Graph.Select(Source, Clock);
			Graph.Request.MaximumImageDimension = mode == Mode::FailureRecovery && (Operation & 1) ? 0 : 128;
		}
		void Update(bool success) {
			const bool actual = Panel.Update(Graph.Doc, "window", Graph.Request, 1, Revision, Error);
			Require(actual == success, "Audio Window panel expected refresh status");
			if (!success)
				Require(
					!Panel.Current() && Error.Code == Status::InvalidValue,
					"Audio Window failed refresh exposed stale artifact"
				);
		}
		uint64_t Verify() const {
			Require(Panel.Current() != nullptr, "Audio Window final artifact absent");
			return Graph.Verify(*Panel.Current(), Source, Clock);
		}
		void Batch(Mode mode) {
			const Vector2 *points = Panel.Current()->Points.data();
			for (size_t i = 0; i < UPDATES; ++i) {
				Prepare(mode);
				const bool success = mode != Mode::FailureRecovery || !(Operation & 1);
				Update(success);
				if (success && mode != Mode::SourceRevision)
					Require(
						Panel.Current()->Points.data() == points,
						"Audio Window unchanged waveform replaced capacity"
					);
				if (success) points = Panel.Current()->Points.data();
			}
			engine::testing::Consume(Panel.Current()->Cursor);
		}
	};
	void Preflight() {
		static const bool done = [] {
			for (size_t packets : {4096, 65536})
				for (Mode mode :
					 {Mode::Unchanged, Mode::Cursor, Mode::SourceRevision, Mode::FailureRecovery}) {
					Fixture f(packets);
					for (size_t i = 0; i < UPDATES; ++i) {
						f.Prepare(mode);
						bool success = mode != Mode::FailureRecovery || !(f.Operation & 1);
						f.Update(success);
						if (success) f.Verify();
					}
					f.VerifyInputs();
					// Find the exact retained-old + snapshot + replacement admission threshold outside
					// capture.
					uint64_t low = 0, high = 4 * 1024 * 1024;
					const auto tryCap = [&](uint64_t cap) {
						AudioWindowPresentation out = *f.Panel.Current();
						Diagnostic e;
						f.Graph.Select(1 - f.Source, f.Clock);
						return ResolveAudioWindowPresentation(
							f.Graph.Doc, f.Graph.Compiled, "window", f.Graph.Request, cap, out, e
						);
					};
					Require(tryCap(high) == Status::Ok, "Audio Window cap upper bound");
					while (low < high) {
						uint64_t mid = low + (high - low) / 2;
						if (tryCap(mid) == Status::Ok)
							high = mid;
						else
							low = mid + 1;
					}
					Require(
						low && tryCap(low) == Status::Ok && tryCap(low - 1) == Status::LimitExceeded,
						"Audio Window exact/minus1 cap"
					);
					f.Graph.Select(1 - f.Source, f.Clock);
					AudioWindowPresentation out = *f.Panel.Current();
					const auto before = out;
					Diagnostic e;
					Require(
						ResolveAudioWindowPresentation(
							f.Graph.Doc, f.Graph.Compiled, "window", f.Graph.Request, low - 1, out, e
						) == Status::LimitExceeded &&
							out.Points == before.Points && out.Cursor == before.Cursor &&
							out.Start == before.Start && out.End == before.End,
						"Audio Window cap refusal changed old data"
					);
				}
			return true;
		}();
		(void)done;
	}
	struct HeapReading {
		engine::core::HeapNodeView All;
		engine::core::HeapNodeView Owner;
		engine::core::HeapNodeView Observation;
		engine::core::HeapNodeView Geometry;
	};
	void Add(engine::core::HeapNodeView &sum, const engine::core::HeapNodeView &node) {
		sum.TotalBytes += node.TotalBytes;
		sum.TotalBlocks += node.TotalBlocks;
		sum.LiveBytes += node.LiveBytes;
		sum.LiveBlocks += node.LiveBlocks;
		sum.PeakBytes += node.PeakBytes;
	}
	HeapReading ReadHeap() {
		const uint32_t count = HeapProfile::NodeCount();
		Require(
			count <= HeapProfile::MAXIMUM_NODES, "Audio Window panel heap tree exceeds bounded diagnostic"
		);
		HeapReading result;
		for (uint32_t index = 0; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			Add(result.All, node);
			bool observation = false, geometry = false, owner = false;
			for (uint32_t ancestor = index, depth = 0; ancestor != 0; ++depth) {
				Require(depth < count && ancestor < count, "Audio Window panel heap hierarchy invalid");
				const auto parent = HeapProfile::Node(ancestor);
				observation |= parent.Name == OBSERVATION;
				geometry |= parent.Name == GEOMETRY;
				owner |= parent.Name == OWNER;
				ancestor = parent.Parent;
			}
			if (!owner) continue;
			if (node.Name == OWNER) Add(result.Owner, node);
			if (observation) Add(result.Observation, node);
			if (geometry) Add(result.Geometry, node);
		}
		return result;
	}
	double CounterValue(const std::vector<engine::core::Counter> &counters, std::string_view name) {
		for (const auto &counter : counters)
			if (counter.Name.Text() == name) return counter.Value;
		return 0;
	}
	struct RestoreProfile {
		bool Previous = FrameGraph::IsEnabled();
		~RestoreProfile() {
			FrameGraph::SetEnabled(Previous);
		}
	};
	void Measure(Fixture &fixture, Mode mode) {
		// Integrity serialization/hashing is outside both owner timing and heap deltas.
		fixture.VerifyInputs();
		Require(
			fixture.Calls < 13, "Audio Window panel benchmark supports only eight warmups plus five samples"
		);
		const size_t call = ++fixture.Calls;
		if (!CaptureEnabled()) {
			{
				ENGINE_PROFILE("studio audio window benchmark");
				fixture.Batch(mode);
			}
			fixture.VerifyInputs();
			return;
		}
		Require(HeapProfile::IsCompiledIn(), "Audio Window panel capture requires compiled heap hooks");
		const RestoreProfile restore;
		FrameGraph::SetEnabled(true);
		Metrics::Drain();
		const auto before = ReadHeap();
		const auto totalBefore = HeapProfile::Totals();
		const size_t capacityBefore = fixture.Panel.Current()->Points.capacity();
		FrameGraph::BeginFrame();
		try {
			ENGINE_PROFILE("studio audio window benchmark");
			fixture.Batch(mode);
		} catch (...) {
			// Close an exceptional owner before restoring the frame collector.
			FrameGraph::EndFrame();
			throw;
		}
		FrameGraph::EndFrame();
		const auto totalAfter = HeapProfile::Totals();
		const auto after = ReadHeap();
		Require(
			FrameGraph::Dropped() == 0 && totalAfter.DroppedScopes == totalBefore.DroppedScopes,
			"Audio Window panel capture dropped frame/heap scopes"
		);
		const auto &spans = FrameGraph::Spans();
		Require(
			!spans.empty() && spans.size() <= 8192 && spans.front().Name == OWNER &&
				spans.front().Parent == FrameGraph::NO_PARENT && spans.front().Depth == 0,
			"Audio Window panel capture missed bounded owner hierarchy"
		);
		size_t observations = 0, geometries = 0, coreOwners = 0;
		float observationMilliseconds = 0, observationSelf = 0, geometryMilliseconds = 0;
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			Require(
				!span.Reported && span.IdleMilliseconds == 0,
				"Audio Window headless synchronous capture has unexpected idle/producer spans"
			);
			if (index != 0) {
				Require(
					span.Parent < index && spans[span.Parent].Depth + 1 == span.Depth,
					"Audio Window panel capture invalid nested hierarchy"
				);
				const auto &parent = spans[span.Parent];
				Require(
					span.StartMilliseconds + .01f >= parent.StartMilliseconds &&
						span.StartMilliseconds + span.Milliseconds <=
							parent.StartMilliseconds + parent.Milliseconds + .01f,
					"Audio Window panel child timing exceeds actual parent interval"
				);
			}
			if (span.Name == OBSERVATION) {
				Require(
					span.Parent == 0 && span.Depth == 1, "Audio Window observation not inside batch owner"
				);
				++observations;
				observationMilliseconds += span.Milliseconds;
				observationSelf += span.SelfMilliseconds;
			}
			if (span.Name == CORE) {
				Require(
					span.Parent < index && spans[span.Parent].Name == OBSERVATION,
					"Audio Window core outside panel"
				);
				++coreOwners;
			}
			if (span.Name == GEOMETRY) {
				Require(
					span.Parent < index && spans[span.Parent].Name == CORE,
					"Audio Window geometry not inside actual panel observation"
				);
				++geometries;
				geometryMilliseconds += span.Milliseconds;
			}
		}
		Require(
			observations == UPDATES && coreOwners == (mode == Mode::Unchanged ? 0 : UPDATES) &&
				geometries == (mode == Mode::SourceRevision ? UPDATES : 0),
			"Audio Window panel capture missing observation/geometry operations"
		);
		const auto counters = Metrics::Drain();
		const double payload = CounterValue(counters, "imagegraph.audio_window.allocated_payload_bytes");
		const double allocations = CounterValue(counters, "imagegraph.audio_window.allocations");
		const double cacheHits = CounterValue(counters, "imagegraph.audio_window.geometry_cache_hits");
		const double snapshotRetained =
			CounterValue(counters, "imagegraph.audio_window.snapshot_retained_payload_bytes");
		Require(
			(mode == Mode::Unchanged) == (snapshotRetained == 0), "Audio Window snapshot retained counter"
		);
		const size_t capacityAfter = fixture.Panel.Current()->Points.capacity();
		Require(
			allocations == (mode == Mode::SourceRevision ? UPDATES : 0) &&
				payload ==
					(mode == Mode::SourceRevision ? double(UPDATES * capacityAfter * sizeof(Vector2)) : 0) &&
				cacheHits == (mode == Mode::Cursor			  ? UPDATES
							  : mode == Mode::FailureRecovery ? UPDATES / 2
															  : 0),
			"Audio Window panel actual geometry/cache counters mismatch analytical operations"
		);
		fixture.VerifyInputs();
		const uint64_t outputHash = fixture.Verify();
		const auto bytes = [](const auto &a, const auto &b) { return a.TotalBytes - b.TotalBytes; };
		const auto blocks = [](const auto &a, const auto &b) { return a.TotalBlocks - b.TotalBlocks; };
		const uint64_t processBytes = bytes(totalAfter, totalBefore);
		const uint64_t processBlocks = blocks(totalAfter, totalBefore);
		Require(
			bytes(after.All, before.All) == processBytes && blocks(after.All, before.All) == processBlocks,
			"Audio Window panel exclusive all-tag deltas differ from process allocation counters"
		);
		const uint64_t ownerBytes = bytes(after.Owner, before.Owner);
		const uint64_t ownerBlocks = blocks(after.Owner, before.Owner);
		const uint64_t observationBytes = bytes(after.Observation, before.Observation);
		const uint64_t observationBlocks = blocks(after.Observation, before.Observation);
		Require(
			ownerBytes + observationBytes <= processBytes &&
				ownerBlocks + observationBlocks <= processBlocks &&
				bytes(after.Geometry, before.Geometry) <= observationBytes &&
				blocks(after.Geometry, before.Geometry) <= observationBlocks,
			"Audio Window panel exclusive/inclusive heap attribution does not reconcile"
		);
		std::printf(
			"# studio-audio-window-profile row=%d call=%zu warmup=%d updates=%zu samples_per_channel=%zu "
			"channels=2 points=%zu fps=128 sample_rate=32768 input_payload_bytes=%" PRIu64
			" input_fnv=%" PRIu64 " output_fnv=%" PRIu64 " clock_tick=%" PRIu64
			" clock_subframe=%.17g clock_negative=%d source_index=%zu input_revision=%" PRIu64
			" frames=1 spans=%zu "
			"frame_drops=0 heap_drop_delta=0 owner_inclusive_ms=%.6f owner_self_ms=%.6f "
			"observation_inclusive_ms=%.6f observation_self_ms=%.6f geometry_inclusive_ms=%.6f "
			"frame_ms=%.6f unmarked_ms=%.6f idle_ms=0 reported_spans=0 geometry_payload_bytes=%.0f "
			"geometry_allocations=%.0f geometry_cache_hits=%.0f snapshot_retained_payload_bytes=%.0f "
			"capacity_bytes_before=%zu "
			"capacity_bytes_after=%zu "
			"heap_coverage=cxx_new_delete owner_exclusive_bytes=%" PRIu64 " owner_exclusive_blocks=%" PRIu64
			" observation_inclusive_bytes=%" PRIu64 " observation_inclusive_blocks=%" PRIu64
			" geometry_inclusive_bytes=%" PRIu64 " geometry_inclusive_blocks=%" PRIu64
			" all_tag_allocated_bytes=%" PRIu64 " all_tag_allocated_blocks=%" PRIu64
			" process_allocated_bytes=%" PRIu64 " process_allocated_blocks=%" PRIu64
			" process_peak_bytes=%" PRId64 " process_live_before=%" PRId64 " process_live_after=%" PRId64
			" process_live_blocks_before=%" PRId64 " process_live_blocks_after=%" PRId64
			" geometry_live_before=%" PRId64 " geometry_live_after=%" PRId64
			" geometry_live_blocks_before=%" PRId64 " geometry_live_blocks_after=%" PRId64
			" geometry_sum_tag_peak_bytes=%" PRId64 " profiler_overhead_bytes=%" PRId64 "\n",
			fixture.Row(mode),
			call,
			call <= 8,
			UPDATES,
			fixture.Graph.Packets,
			POINTS,
			fixture.InputPayloadBytes,
			fixture.OriginalInputHash,
			outputHash,
			fixture.Clock.Tick,
			fixture.Clock.Subframe,
			fixture.Clock.NegativeFrame,
			fixture.Source,
			fixture.Revision,
			spans.size(),
			spans.front().Milliseconds,
			spans.front().SelfMilliseconds,
			observationMilliseconds,
			observationSelf,
			geometryMilliseconds,
			FrameGraph::FrameMilliseconds(),
			FrameGraph::UnmarkedMilliseconds(),
			payload,
			allocations,
			cacheHits,
			snapshotRetained,
			capacityBefore * sizeof(Vector2),
			capacityAfter * sizeof(Vector2),
			ownerBytes,
			ownerBlocks,
			observationBytes,
			observationBlocks,
			bytes(after.Geometry, before.Geometry),
			blocks(after.Geometry, before.Geometry),
			bytes(after.All, before.All),
			blocks(after.All, before.All),
			processBytes,
			processBlocks,
			totalAfter.PeakBytes,
			totalBefore.LiveBytes,
			totalAfter.LiveBytes,
			totalBefore.LiveBlocks,
			totalAfter.LiveBlocks,
			before.Geometry.LiveBytes,
			after.Geometry.LiveBytes,
			before.Geometry.LiveBlocks,
			after.Geometry.LiveBlocks,
			after.Geometry.PeakBytes,
			totalAfter.OverheadBytes
		);
		// Consume every completed frame now; history overwrite cannot imply completeness.
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			std::printf(
				"# studio-audio-window-span row=%d call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
				"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=0 name=%.*s\n",
				fixture.Row(mode),
				call,
				index,
				span.Parent,
				span.Depth,
				span.StartMilliseconds,
				span.Milliseconds,
				span.SelfMilliseconds,
				span.IdleMilliseconds,
				int(span.Name.size()),
				span.Name.data()
			);
		}
	}
}
BENCH_PER_ITEM("Audio Window 4096-packet stereo Unchanged, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(4096);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::Unchanged);
}

BENCH_PER_ITEM("Audio Window 4096-packet stereo Cursor, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(4096);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::Cursor);
}

BENCH_PER_ITEM("Audio Window 4096-packet stereo SourceRevision, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(4096);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::SourceRevision);
}

BENCH_PER_ITEM("Audio Window 4096-packet stereo FailureRecovery, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(4096);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::FailureRecovery);
}

BENCH_PER_ITEM("Audio Window 65536-packet stereo Unchanged, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(65536);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::Unchanged);
}

BENCH_PER_ITEM("Audio Window 65536-packet stereo Cursor, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(65536);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::Cursor);
}

BENCH_PER_ITEM("Audio Window 65536-packet stereo SourceRevision, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(65536);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::SourceRevision);
}

BENCH_PER_ITEM("Audio Window 65536-packet stereo FailureRecovery, per Update", audio_window_bench::UPDATES) {
	audio_window_bench::Preflight();
	static audio_window_bench::Fixture fixture(65536);
	audio_window_bench::Measure(fixture, audio_window_bench::Mode::FailureRecovery);
}
