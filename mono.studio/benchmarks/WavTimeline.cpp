// Actual headless panel observation, including its retained geometry cache.
// Captured owners exclude verification/reporting; BENCH includes those diagnostics.
#include "WavTimelinePanel.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("studio.bench.wav-timeline")
TEST_DEPENDS("studio.wav_timeline_panel")

namespace wav_timeline_bench {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr size_t UPDATES = 64;
	constexpr size_t SAMPLES = 65536;
	constexpr size_t POINTS = 257;
	constexpr double FPS = 128;
	constexpr FrameTime INITIAL_CLOCK{32, .5, false};
	constexpr std::array<FrameTime, 6> CURSOR_CLOCKS{
		{{64, .5, false}, {64, .5, true}, {512, .5, false}, {512, .5, true}, {0, .5, false}, {0, .5, true}}
	};
	constexpr std::string_view OWNER = "studio wav timeline benchmark";
	constexpr std::string_view OBSERVATION = "image composer wav timeline observation";
	constexpr std::string_view GEOMETRY = "imagegraph.wav_timeline.geometry";
	enum class Mode { Unchanged, Cursor, SourceRevision };
	void Require(bool condition, const char *message) {
		if (!condition) throw std::runtime_error(message);
	}
	bool CaptureEnabled() {
		static const bool enabled = [] {
			const char *value = std::getenv("ATOMIC_STUDIO_WAV_TIMELINE_PROFILE");
			return value && std::string_view(value) == "1";
		}();
		return enabled;
	}
	double Amplitude(size_t index, size_t source) {
		return (double(index % 1024) - 512) / 512 + double(source) / 8;
	}
	struct Fixture {
		Document Doc;
		EvaluationRequest Request;
		std::array<AudioClipSource, 2> Sources;
		studio::WavTimelinePanel Panel;
		Diagnostic Error;
		FrameTime Clock = INITIAL_CLOCK;
		uint64_t Revision = 1;
		size_t Source = 0;
		size_t Operation = 0;
		size_t Calls = 0;
		uint64_t OriginalInputHash = 0;
		uint64_t InputPayloadBytes = 0;
		explicit Fixture() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"file", "pc.wav_file_read", "", {}, {{"path", std::string("clip")}, {"mono", true}}}
			};
			Doc.Outputs = {{"audio", "file", "data"}};
			for (size_t source = 0; source < Sources.size(); ++source) {
				Sources[source].SourceId = "clip";
				Sources[source].Data.SampleRate = 32768;
				Sources[source].Data.Channels.resize(2);
				for (auto &channel : Sources[source].Data.Channels) {
					channel.resize(SAMPLES);
					InputPayloadBytes += channel.capacity() * sizeof(double);
				}
				for (size_t index = 0; index < SAMPLES; ++index) {
					Sources[source].Data.Channels[0][index] = Amplitude(index, source);
					Sources[source].Data.Channels[1][index] = -Amplitude(index, source);
				}
			}
			OriginalInputHash = HashInputs();
			Request.AudioClips = std::span<const AudioClipSource>(&Sources[Source], 1);
			Require(SetFrameTime(Request, Clock), "WAV benchmark initial clock invalid");
			Update();
			Verify();
			VerifyInputs();
		}
		static void HashWord(uint64_t &hash, uint64_t word) {
			for (size_t byte = 0; byte < sizeof(word); ++byte) {
				hash ^= (word >> (byte * 8)) & 255;
				hash *= 1099511628211ULL;
			}
		}

		static void HashText(uint64_t &hash, std::string_view text) {
			HashWord(hash, text.size());
			for (unsigned char byte : text) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
		}
		uint64_t HashInputs() const {
			uint64_t hash = 14695981039346656037ULL;
			HashText(hash, Write(Doc));
			HashWord(hash, std::bit_cast<uint64_t>(FPS));
			HashWord(hash, UPDATES);
			HashWord(hash, 1); // Initial document revision.
			HashWord(hash, 1); // Initial input revision.
			HashWord(hash, 0); // Initial source index.
			HashText(hash, "source=operation&1;input_revision=1+operation");
			HashWord(hash, static_cast<uint64_t>(Mode::Unchanged));
			HashWord(hash, static_cast<uint64_t>(Mode::Cursor));
			HashWord(hash, static_cast<uint64_t>(Mode::SourceRevision));
			const auto clock = [&](FrameTime value) {
				HashWord(hash, value.Tick);
				HashWord(hash, std::bit_cast<uint64_t>(value.Subframe));
				HashWord(hash, value.NegativeFrame);
			};
			clock(INITIAL_CLOCK);
			HashWord(hash, CURSOR_CLOCKS.size());
			for (const auto &value : CURSOR_CLOCKS)
				clock(value);
			HashWord(hash, Sources.size());
			for (size_t source = 0; source < Sources.size(); ++source) {
				HashWord(hash, source);
				HashText(hash, Sources[source].SourceId);
				const auto &audio = Sources[source].Data;
				HashWord(hash, std::bit_cast<uint64_t>(audio.SampleRate));
				HashWord(hash, audio.Samples.size());
				for (double sample : audio.Samples)
					HashWord(hash, std::bit_cast<uint64_t>(sample));
				HashWord(hash, audio.Channels.size());
				for (size_t channel = 0; channel < audio.Channels.size(); ++channel) {
					HashWord(hash, channel);
					HashWord(hash, audio.Channels[channel].size());
					for (double sample : audio.Channels[channel])
						HashWord(hash, std::bit_cast<uint64_t>(sample));
				}
			}
			return hash;
		}
		void VerifyInputs() const {
			Require(HashInputs() == OriginalInputHash, "WAV immutable document/source/configuration changed");
			uint64_t bytes = 0;
			for (const auto &source : Sources) {
				bytes += source.Data.Samples.capacity() * sizeof(double);
				for (const auto &channel : source.Data.Channels)
					bytes += channel.capacity() * sizeof(double);
			}
			Require(bytes == InputPayloadBytes && bytes != 0, "WAV actual input capacity ledger changed");
		}

		void Prepare(Mode mode) {
			++Operation;
			if (mode == Mode::Cursor) {
				Clock = CURSOR_CLOCKS[(Operation - 1) % CURSOR_CLOCKS.size()];
				Require(SetFrameTime(Request, Clock), "WAV benchmark signed cursor invalid");
			} else if (mode == Mode::SourceRevision) {
				Source = Operation & 1;
				Request.AudioClips = std::span<const AudioClipSource>(&Sources[Source], 1);
				++Revision;
			}
		}
		void Update() {
			Require(
				Panel.Update(Doc, "file", Request, FPS, 1, Revision, Error),
				"WAV benchmark actual Panel.Update failed"
			);
		}
		uint64_t Verify() const {
			const auto *geometry = Panel.Current();
			Require(
				geometry && geometry->Points.size() == POINTS && geometry->Channels == 2 &&
					geometry->Duration == 2,
				"WAV panel output shape/duration incorrect"
			);
			// 32768/128 is exactly 256. The last of 257 points repeats the final
			// original channel-zero sample, independent of the authored Mono flag.
			uint64_t hash = 14695981039346656037ULL;
			for (size_t index = 0; index < POINTS; ++index) {
				const auto &point = geometry->Points[index];
				Require(
					point.X == double(index) &&
						point.Y == Amplitude(std::min(index * 256, SAMPLES - 1), Source),
					"WAV panel sampled coordinate/amplitude incorrect"
				);
				HashWord(hash, std::bit_cast<uint64_t>(point.X));
				HashWord(hash, std::bit_cast<uint64_t>(point.Y));
			}
			const double expectedProgress =
				Clock.NegativeFrame ? 0 : std::clamp((double(Clock.Tick) + Clock.Subframe) / 256, 0., 1.);
			Require(geometry->Progress == expectedProgress, "WAV panel signed progress incorrect");
			HashWord(hash, std::bit_cast<uint64_t>(geometry->Progress));
			HashWord(hash, std::bit_cast<uint64_t>(geometry->Duration));
			HashWord(hash, geometry->Channels);
			return hash;
		}
		void Batch(Mode mode) {
			for (size_t operation = 0; operation < UPDATES; ++operation) {
				Prepare(mode);
				const auto *points = Panel.Current()->Points.data();
				Update();
				if (mode != Mode::SourceRevision)
					Require(
						Panel.Current()->Points.data() == points,
						"WAV cache replaced point buffer on unchanged/cursor update"
					);
			}
			engine::testing::Consume(Panel.Current()->Progress);
		}
	};
	void Preflight() {
		// Independent complete point checks cover every transition before warmup.
		static const bool completed = [] {
			Fixture fixture;
			for (Mode mode : {Mode::Unchanged, Mode::Cursor, Mode::SourceRevision})
				for (size_t operation = 0; operation < UPDATES; ++operation) {
					const auto *points = fixture.Panel.Current()->Points.data();
					fixture.Prepare(mode);
					fixture.Update();
					fixture.Verify();
					if (mode != Mode::SourceRevision)
						Require(
							fixture.Panel.Current()->Points.data() == points,
							"WAV preflight cache failed to retain geometry"
						);
				}
			fixture.VerifyInputs();
			return true;
		}();
		(void)completed;
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
		Require(count <= HeapProfile::MAXIMUM_NODES, "WAV panel heap tree exceeds bounded diagnostic");
		HeapReading result;
		for (uint32_t index = 0; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			Add(result.All, node);
			bool observation = false, geometry = false, owner = false;
			for (uint32_t ancestor = index, depth = 0; ancestor != 0; ++depth) {
				Require(depth < count && ancestor < count, "WAV panel heap hierarchy invalid");
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
		Require(fixture.Calls < 13, "WAV panel benchmark supports only eight warmups plus five samples");
		const size_t call = ++fixture.Calls;
		if (!CaptureEnabled()) {
			{
				ENGINE_PROFILE("studio wav timeline benchmark");
				fixture.Batch(mode);
			}
			fixture.VerifyInputs();
			return;
		}
		Require(HeapProfile::IsCompiledIn(), "WAV panel capture requires compiled heap hooks");
		const RestoreProfile restore;
		FrameGraph::SetEnabled(true);
		Metrics::Drain();
		const auto before = ReadHeap();
		const auto totalBefore = HeapProfile::Totals();
		const size_t capacityBefore = fixture.Panel.Current()->Points.capacity();
		FrameGraph::BeginFrame();
		try {
			ENGINE_PROFILE("studio wav timeline benchmark");
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
			"WAV panel capture dropped frame/heap scopes"
		);
		const auto &spans = FrameGraph::Spans();
		Require(
			!spans.empty() && spans.size() <= 8192 && spans.front().Name == OWNER &&
				spans.front().Parent == FrameGraph::NO_PARENT && spans.front().Depth == 0,
			"WAV panel capture missed bounded owner hierarchy"
		);
		size_t observations = 0, geometries = 0;
		float observationMilliseconds = 0, observationSelf = 0, geometryMilliseconds = 0;
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			Require(
				!span.Reported && span.IdleMilliseconds == 0,
				"WAV headless synchronous capture has unexpected idle/producer spans"
			);
			if (index != 0) {
				Require(
					span.Parent < index && spans[span.Parent].Depth + 1 == span.Depth,
					"WAV panel capture invalid nested hierarchy"
				);
				const auto &parent = spans[span.Parent];
				Require(
					span.StartMilliseconds + .01f >= parent.StartMilliseconds &&
						span.StartMilliseconds + span.Milliseconds <=
							parent.StartMilliseconds + parent.Milliseconds + .01f,
					"WAV panel child timing exceeds actual parent interval"
				);
			}
			if (span.Name == OBSERVATION) {
				Require(span.Parent == 0 && span.Depth == 1, "WAV observation not inside batch owner");
				++observations;
				observationMilliseconds += span.Milliseconds;
				observationSelf += span.SelfMilliseconds;
			}
			if (span.Name == GEOMETRY) {
				Require(
					span.Parent < index && spans[span.Parent].Name == OBSERVATION,
					"WAV geometry not inside actual panel observation"
				);
				++geometries;
				geometryMilliseconds += span.Milliseconds;
			}
		}
		Require(
			observations == UPDATES && geometries == (mode == Mode::SourceRevision ? UPDATES : 0),
			"WAV panel capture missing observation/geometry operations"
		);
		const auto counters = Metrics::Drain();
		const double payload = CounterValue(counters, "imagegraph.wav_timeline.allocated_payload_bytes");
		const double allocations = CounterValue(counters, "imagegraph.wav_timeline.allocations");
		const double cacheHits = CounterValue(counters, "studio.wav_timeline.geometry_cache_hits");
		const size_t capacityAfter = fixture.Panel.Current()->Points.capacity();
		Require(
			allocations == (mode == Mode::SourceRevision ? UPDATES : 0) &&
				payload ==
					(mode == Mode::SourceRevision ? double(UPDATES * capacityAfter * sizeof(Vector2)) : 0) &&
				cacheHits == (mode == Mode::Cursor ? UPDATES : 0),
			"WAV panel actual geometry/cache counters mismatch analytical operations"
		);
		fixture.VerifyInputs();
		const uint64_t outputHash = fixture.Verify();
		const auto bytes = [](const auto &a, const auto &b) { return a.TotalBytes - b.TotalBytes; };
		const auto blocks = [](const auto &a, const auto &b) { return a.TotalBlocks - b.TotalBlocks; };
		const uint64_t processBytes = bytes(totalAfter, totalBefore);
		const uint64_t processBlocks = blocks(totalAfter, totalBefore);
		Require(
			bytes(after.All, before.All) == processBytes && blocks(after.All, before.All) == processBlocks,
			"WAV panel exclusive all-tag deltas differ from process allocation counters"
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
			"WAV panel exclusive/inclusive heap attribution does not reconcile"
		);
		std::printf(
			"# studio-wav-profile row=%d call=%zu warmup=%d updates=%zu samples_per_channel=%zu "
			"channels=2 points=%zu fps=128 sample_rate=32768 input_payload_bytes=%" PRIu64
			" input_fnv=%" PRIu64 " output_fnv=%" PRIu64 " clock_tick=%" PRIu64
			" clock_subframe=%.17g clock_negative=%d source_index=%zu input_revision=%" PRIu64
			" frames=1 spans=%zu "
			"frame_drops=0 heap_drop_delta=0 owner_inclusive_ms=%.6f owner_self_ms=%.6f "
			"observation_inclusive_ms=%.6f observation_self_ms=%.6f geometry_inclusive_ms=%.6f "
			"frame_ms=%.6f unmarked_ms=%.6f idle_ms=0 reported_spans=0 geometry_payload_bytes=%.0f "
			"geometry_allocations=%.0f geometry_cache_hits=%.0f capacity_bytes_before=%zu "
			"capacity_bytes_after=%zu "
			"heap_coverage=cxx_new_delete owner_exclusive_bytes=%" PRIu64 " owner_exclusive_blocks=%" PRIu64
			" observation_inclusive_bytes=%" PRIu64 " observation_inclusive_blocks=%" PRIu64
			" geometry_inclusive_bytes=%" PRIu64 " geometry_inclusive_blocks=%" PRIu64
			" process_allocated_bytes=%" PRIu64 " process_allocated_blocks=%" PRIu64
			" process_peak_bytes=%" PRId64 " process_live_before=%" PRId64 " process_live_after=%" PRId64
			" process_live_blocks_before=%" PRId64 " process_live_blocks_after=%" PRId64
			" geometry_live_before=%" PRId64 " geometry_live_after=%" PRId64
			" geometry_live_blocks_before=%" PRId64 " geometry_live_blocks_after=%" PRId64
			" geometry_sum_tag_peak_bytes=%" PRId64 " profiler_overhead_bytes=%" PRId64 "\n",
			int(mode),
			call,
			call <= 8,
			UPDATES,
			SAMPLES,
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
			capacityBefore * sizeof(Vector2),
			capacityAfter * sizeof(Vector2),
			ownerBytes,
			ownerBlocks,
			observationBytes,
			observationBlocks,
			bytes(after.Geometry, before.Geometry),
			blocks(after.Geometry, before.Geometry),
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
				"# studio-wav-span row=%d call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
				"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=0 name=%.*s\n",
				int(mode),
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
BENCH_PER_ITEM(
	"WAV inspector unchanged request, 65536-sample stereo, per Update", wav_timeline_bench::UPDATES
) {
	wav_timeline_bench::Preflight();
	static wav_timeline_bench::Fixture fixture;
	wav_timeline_bench::Measure(fixture, wav_timeline_bench::Mode::Unchanged);
}
BENCH_PER_ITEM("WAV inspector signed cursor, retained points, per Update", wav_timeline_bench::UPDATES) {
	wav_timeline_bench::Preflight();
	static wav_timeline_bench::Fixture fixture;
	wav_timeline_bench::Measure(fixture, wav_timeline_bench::Mode::Cursor);
}
BENCH_PER_ITEM("WAV inspector source/input revision replacement, per Update", wav_timeline_bench::UPDATES) {
	wav_timeline_bench::Preflight();
	static wav_timeline_bench::Fixture fixture;
	wav_timeline_bench::Measure(fixture, wav_timeline_bench::Mode::SourceRevision);
}
