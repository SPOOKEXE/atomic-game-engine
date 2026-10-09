#pragma once

#include "../tests/fixtures/BusRoutingParity.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace bus_routing_bench {
	using engine::core::FrameGraph;
	using engine::core::HeapNodeView;
	using engine::core::HeapProfile;
	constexpr std::array<std::string_view, 6> PHASES = {
		"world bus collect",
		"world bus order",
		"world bus apply",
		"world bus delivery",
		"world bus route",
		"tick other"
	};
	inline bool Flag(const char *name) {
		const char *value = std::getenv(name);
		return value != nullptr && std::string_view(value) == "1";
	}
	inline bool ProfileEnabled() {
		static const bool value = Flag("ATOMIC_WORLD_BUS_PROFILE");
		return value;
	}
	inline bool CanonicalEnabled() {
		static const bool value = Flag("ATOMIC_WORLD_BUS_CANONICAL");
		return value;
	}
	inline void Add(HeapNodeView &total, const HeapNodeView &node) {
		total.TotalBytes += node.TotalBytes;
		total.TotalBlocks += node.TotalBlocks;
		total.LiveBytes += node.LiveBytes;
		total.LiveBlocks += node.LiveBlocks;
		total.PeakBytes += node.PeakBytes;
	}
	// Heap totals attributed to routing phases and the enclosing tick.
	struct HeapReading {
		// Exclusive totals for each entry in PHASES, including the residual.
		std::array<HeapNodeView, 6> Exclusive{};
		// Aggregate totals for nodes beneath Universe::Tick.
		HeapNodeView Tick;
	};
	inline HeapReading ReadHeap() {
		const uint32_t count = HeapProfile::NodeCount();
		bus_routing_fixture::Require(
			count <= HeapProfile::MAXIMUM_NODES, "bus heap diagnostic node bound exceeded"
		);
		HeapReading result;
		// Counter reads allocate no node snapshots inside the measured boundary.
		for (uint32_t index = 1; index < count; ++index) {
			const auto node = HeapProfile::Node(index);
			uint32_t ancestor = index;
			size_t phase = 5;
			bool classified = false, underTick = false;
			for (uint32_t depth = 0; ancestor != 0; ++depth) {
				bus_routing_fixture::Require(
					depth < count && ancestor < count, "bus heap parent chain malformed"
				);
				const auto parent = HeapProfile::Node(ancestor);
				if (!classified) {
					for (size_t candidate = 0; candidate < 5; ++candidate) {
						if (parent.Name == PHASES[candidate]) {
							phase = candidate;
							classified = true;
							break;
						}
					}
				}
				if (parent.Name == "Universe::Tick" && parent.Parent == HeapProfile::ROOT) underTick = true;
				ancestor = parent.Parent;
			}
			if (underTick) {
				Add(result.Exclusive[phase], node);
				Add(result.Tick, node);
			}
		}
		return result;
	}
	// Restores frame-graph collection to its prior state on scope exit.
	struct RestoreProfile {
		// Collector setting captured when the diagnostic scope begins.
		bool Previous = FrameGraph::IsEnabled();
		~RestoreProfile() {
			FrameGraph::SetEnabled(Previous);
		}
	};
	// Stateful benchmark hook retaining its parity oracle and call count.
	struct Diagnostic {
		// Canonical routing oracle advanced alongside each measured batch.
		bus_routing_fixture::ChattyOracle Oracle;
		// Number of batches used to enforce warmup and sample limits.
		unsigned Calls = 0;
		// Starts the oracle against the universe being profiled.
		explicit Diagnostic(engine::world::Universe &universe) : Oracle(universe) {}
		void Batch(engine::world::Universe &universe) {
			const bool capture = ProfileEnabled(), canonical = CanonicalEnabled();
			bus_routing_fixture::Require(
				!(capture && canonical) && !Flag("ATOMIC_WORLD_LANE_HEAP"),
				"bus capture modes must run separately from each other and lane capture"
			);
			bus_routing_fixture::Require(
				Calls < 13, "bus diagnostics permit eight warmups and at most five samples"
			);
			bus_routing_fixture::Require(
				!capture || HeapProfile::IsCompiledIn(), "bus capture requires compiled heap hooks"
			);
			const RestoreProfile restore;
			if (capture) FrameGraph::SetEnabled(true);
			++Calls;
			if (Calls == 1)
				std::printf(
					"# world-bus-diagnostic mode=%s broader_bench_includes_reporting=1 "
					"canonical_bench_timings_invalid=%u input_hash=%016" PRIx64 "\n",
					capture ? "profile" : "canonical",
					static_cast<unsigned>(canonical),
					Oracle.InputHash
				);
			for (unsigned pass = 0; pass < 50; ++pass) {
				HeapReading before, after;
				engine::core::HeapTotals processBefore, processAfter;
				if (capture) {
					FrameGraph::BeginFrame();
					before = ReadHeap();
					processBefore = HeapProfile::Totals();
				}
				try {
					universe.Tick(1.0f / 60.0f);
				} catch (...) {
					// Publish an exceptional owner frame before restoring collector state.
					if (capture) FrameGraph::EndFrame();
					throw;
				}
				if (capture) {
					after = ReadHeap();
					processAfter = HeapProfile::Totals();
					FrameGraph::EndFrame();
				}
				// Verification, canonical serialization and printing follow the owner
				// and its heap boundary. Their cost remains in the broader BENCH row.
				const uint64_t sequence = 2 + (Calls - 1) * 50 + pass;
				const auto words = Oracle.Verify(universe, sequence, canonical);
				words.Print(Calls, pass, "chatty");
				if (!capture) continue;
				const auto &spans = FrameGraph::Spans();
				bus_routing_fixture::Require(
					FrameGraph::Dropped() == 0 && !spans.empty(), "bus capture dropped or omitted owner spans"
				);
				std::array<unsigned, 5> required{};
				std::array<float, 5> inclusive{}, self{};
				unsigned tickCount = 0, barrierCount = 0;
				float tickInclusive = 0, tickSelf = 0, barrierInclusive = 0, barrierSelf = 0, selfTotal = 0;
				for (size_t index = 0; index < spans.size(); ++index) {
					const auto &span = spans[index];
					bus_routing_fixture::Require(
						!span.Reported && std::isfinite(span.Milliseconds) &&
							std::isfinite(span.SelfMilliseconds) && span.Milliseconds >= 0 &&
							span.SelfMilliseconds >= 0 && span.SelfMilliseconds <= span.Milliseconds + .01f,
						"bus capture invalid serial span timing"
					);
					if (span.Parent == FrameGraph::NO_PARENT) {
						bus_routing_fixture::Require(
							span.Depth == 0 && span.Name == "Universe::Tick", "bus capture unexpected root"
						);
					} else {
						bus_routing_fixture::Require(
							span.Parent < index && span.Depth == spans[span.Parent].Depth + 1 &&
								span.StartMilliseconds >= spans[span.Parent].StartMilliseconds - .01f &&
								span.StartMilliseconds + span.Milliseconds <=
									spans[span.Parent].StartMilliseconds + spans[span.Parent].Milliseconds +
										.02f,
							"bus capture incomplete child containment"
						);
					}
					selfTotal += span.SelfMilliseconds;
					if (span.Name == "Universe::Tick") {
						++tickCount;
						tickInclusive += span.Milliseconds;
						tickSelf += span.SelfMilliseconds;
					}
					if (span.Name == "barrier") {
						++barrierCount;
						barrierInclusive += span.Milliseconds;
						barrierSelf += span.SelfMilliseconds;
					}
					for (size_t phase = 0; phase < 5; ++phase) {
						if (span.Name != PHASES[phase]) continue;
						++required[phase];
						inclusive[phase] += span.Milliseconds;
						self[phase] += span.SelfMilliseconds;
						const auto parent = spans[span.Parent].Name;
						bus_routing_fixture::Require(
							parent == (phase == 4 ? "barrier" : "world bus route"),
							"bus capture required phase has wrong parent"
						);
					}
				}
				bus_routing_fixture::Require(
					tickCount == 1 && barrierCount == 1 && required == std::array<unsigned, 5>{2, 1, 1, 1, 1},
					"bus capture required routing phases incomplete"
				);
				bus_routing_fixture::Require(
					std::abs(
						selfTotal + FrameGraph::UnmarkedMilliseconds() - FrameGraph::FrameMilliseconds()
					) <= .05f,
					"bus capture self/unmarked frame reconciliation failed"
				);
				bus_routing_fixture::Require(
					processAfter.DroppedScopes == processBefore.DroppedScopes &&
						after.Tick.TotalBytes - before.Tick.TotalBytes ==
							processAfter.TotalBytes - processBefore.TotalBytes &&
						after.Tick.TotalBlocks - before.Tick.TotalBlocks ==
							processAfter.TotalBlocks - processBefore.TotalBlocks,
					"bus capture heap drops or exclusive/process allocation mismatch"
				);
				std::printf(
					"# world-bus-profile call=%u pass=%u warmup=%u sequence=%" PRIu64
					" worlds=50 operations=50 deliveries=2450 payload_bytes=%" PRIu64
					" input_hash=%016" PRIx64 " output_bytes=%" PRIu64 " output_hash=%016" PRIx64
					" spans=%zu frame_drops=%zu heap_drops=%" PRIu64
					" frame_ms=%.9g unmarked_ms=%.9g idle_self_ms=%.9g tick_inclusive_ms=%.9g "
					"tick_self_ms=%.9g barrier_inclusive_ms=%.9g barrier_self_ms=%.9g"
					" allocated_bytes=%" PRIu64 " allocated_blocks=%" PRIu64
					" process_live_bytes_before=%" PRId64 " process_live_bytes_after=%" PRId64
					" process_live_blocks_before=%" PRId64 " process_live_blocks_after=%" PRId64
					" profiler_overhead_bytes=%" PRId64 "\n",
					Calls,
					pass,
					static_cast<unsigned>(Calls <= 8),
					sequence,
					words.PayloadBytes,
					Oracle.InputHash,
					words.Bytes,
					words.Hash,
					spans.size(),
					FrameGraph::Dropped(),
					processAfter.DroppedScopes - processBefore.DroppedScopes,
					FrameGraph::FrameMilliseconds(),
					FrameGraph::UnmarkedMilliseconds(),
					FrameGraph::CategoryMilliseconds(engine::core::ProfileCategory::Idle),
					tickInclusive,
					tickSelf,
					barrierInclusive,
					barrierSelf,
					after.Tick.TotalBytes - before.Tick.TotalBytes,
					after.Tick.TotalBlocks - before.Tick.TotalBlocks,
					processBefore.LiveBytes,
					processAfter.LiveBytes,
					processBefore.LiveBlocks,
					processAfter.LiveBlocks,
					processAfter.OverheadBytes
				);
				for (size_t phase = 0; phase < PHASES.size(); ++phase) {
					const auto &old = before.Exclusive[phase], &now = after.Exclusive[phase];
					std::printf(
						"# world-bus-phase call=%u pass=%u phase=%zu name=\"%.*s\" inclusive_ms=%.9g "
						"self_ms=%.9g allocated_bytes=%" PRIu64 " allocated_blocks=%" PRIu64
						" live_bytes_before=%" PRId64 " live_bytes_after=%" PRId64
						" live_blocks_before=%" PRId64 " live_blocks_after=%" PRId64
						" historical_sum_tag_peak_bytes=%" PRId64 "\n",
						Calls,
						pass,
						phase,
						static_cast<int>(PHASES[phase].size()),
						PHASES[phase].data(),
						phase < 5 ? inclusive[phase] : 0,
						phase < 5 ? self[phase] : 0,
						now.TotalBytes - old.TotalBytes,
						now.TotalBlocks - old.TotalBlocks,
						old.LiveBytes,
						now.LiveBytes,
						old.LiveBlocks,
						now.LiveBlocks,
						now.PeakBytes
					);
				}
				// Emit every completed span before beginning another frame. This does
				// not rely on history capacity, and carries actual ordered hierarchy.
				for (size_t index = 0; index < spans.size(); ++index) {
					const auto &span = spans[index];
					std::printf(
						"# world-bus-span call=%u pass=%u index=%zu parent=%u depth=%u name=\"%.*s\" "
						"start_ms=%.9g inclusive_ms=%.9g self_ms=%.9g idle_ms=%.9g category=%u reported=%u\n",
						Calls,
						pass,
						index,
						span.Parent,
						span.Depth,
						static_cast<int>(span.Name.size()),
						span.Name.data(),
						span.StartMilliseconds,
						span.Milliseconds,
						span.SelfMilliseconds,
						span.IdleMilliseconds,
						static_cast<unsigned>(span.Category),
						static_cast<unsigned>(span.Reported)
					);
				}
			}
		}
	};
	inline void PreflightOnce() {
		static const bool passed = [] {
			bus_routing_fixture::Preflight();
			return true;
		}();
		(void)passed;
	}
	inline void DiagnosticBatch(engine::world::Universe &universe) {
		static Diagnostic diagnostic(universe);
		diagnostic.Batch(universe);
	}
}
