#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.ecs.bench.scheduler")

namespace scheduler_timing_bench {
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;

	void Require(bool valid) {
		if (!valid) throw std::runtime_error("Scheduler diagnostic oracle mismatch");
	}
	struct Fixture {
		Store World{"scheduler-bench"};
		Scheduler Systems;
		std::vector<std::string> Names;
		std::vector<uint64_t> Outputs;
		size_t Phases;
		bool Parallel;
		Fixture(size_t count, bool longNames, size_t phases, bool parallel)
			: Names(count), Outputs(count), Phases(phases), Parallel(parallel) {
			for (size_t index = 0; index < count; ++index)
				Names[index] = std::string(longNames ? 40 : 1, 's') + std::to_string(index);
		}
		void Register() {
			for (size_t index = 0; index < Names.size(); ++index) {
				const auto phase = static_cast<Phase>(index % Phases);
				if (Parallel)
					Systems.AddParallel(Names[index], phase, [this, index](const Store &) {
						Outputs[index] += index + 1;
					});
				else
					Systems.Add(Names[index], phase, [this, index](Store &) { Outputs[index] += index + 1; });
			}
		}
		void Execute(bool split) {
			Systems.ClearTimings();
			if (split) {
				for (size_t repeat = 0; repeat < 2; ++repeat)
					for (size_t phase = Phases; phase-- > 0;)
						Systems.RunPhases(World, static_cast<Phase>(phase), static_cast<Phase>(phase));
			} else {
				Systems.RunPhases(World, Phase::Input, static_cast<Phase>(Phases - 1));
			}
		}
		uint64_t Canonical() const {
			uint64_t hash = 14695981039346656037ull;
			const auto byte = [&](uint8_t value) { hash = (hash ^ value) * 1099511628211ull; };
			for (uint64_t output : Outputs)
				for (size_t shift = 0; shift < 64; shift += 8)
					byte(static_cast<uint8_t>(output >> shift));
			for (const auto &row : Systems.Timings()) {
				for (unsigned char character : row.Name)
					byte(character);
				byte(0);
				byte(static_cast<uint8_t>(row.RunPhase));
			}
			return hash;
		}
		void Verify(bool split, uint64_t priorRuns) const {
			const auto &rows = Systems.Timings();
			Require(rows.size() == Names.size());
			size_t position = 0;
			for (size_t phaseIndex = 0; phaseIndex < Phases; ++phaseIndex) {
				const size_t phase = split ? Phases - 1 - phaseIndex : phaseIndex;
				for (size_t index = phase; index < Names.size(); index += Phases) {
					const auto &row = rows[position++];
					Require(row.Name == Names[index] && row.RunPhase == static_cast<Phase>(phase));
					Require(std::isfinite(row.Milliseconds) && row.Milliseconds >= 0);
					Require(Outputs[index] == (priorRuns + (split ? 2 : 1)) * (index + 1));
				}
			}
			Require(World.Time().Tick == 0);
		}
	};

	engine::core::HeapNodeView OwnerHeap() {
		engine::core::HeapNodeView total;
		for (uint32_t id = 1; id < HeapProfile::NodeCount(); ++id) {
			bool owner = false;
			for (uint32_t ancestor = id; ancestor != 0;) {
				const auto node = HeapProfile::Node(ancestor);
				if (node.Name == "Scheduler timing diagnostic") {
					owner = true;
					break;
				}
				Require(node.Parent < ancestor);
				ancestor = node.Parent;
			}
			if (!owner) continue;
			const auto node = HeapProfile::Node(id);
			total.TotalBytes += node.TotalBytes;
			total.TotalBlocks += node.TotalBlocks;
			total.LiveBytes += node.LiveBytes;
			total.LiveBlocks += node.LiveBlocks;
			total.PeakBytes += node.PeakBytes;
		}
		return total;
	}
	template <class Work, class Oracle>
	void Capture(size_t id, size_t sample, const char *operation, Work work, Oracle oracle) {
		const auto ownerBefore = OwnerHeap();
		const auto before = HeapProfile::Totals();
		FrameGraph::BeginFrame();
		try {
			ENGINE_PROFILE("Scheduler timing diagnostic");
			work();
		} catch (...) {
			FrameGraph::EndFrame();
			throw;
		}
		FrameGraph::EndFrame();
		const auto after = HeapProfile::Totals();
		const auto ownerAfter = OwnerHeap();
		oracle();
		Require(FrameGraph::Dropped() == 0 && before.DroppedScopes == after.DroppedScopes);
		const auto spans = FrameGraph::Spans();
		Require(!spans.empty() && spans.front().Name == "Scheduler timing diagnostic");
		std::printf(
			"# scheduler-timing case=%zu phase=%zu warmup=%d operation=%s oracle=outputs-and-rows "
			"frame_ms=%.9f unmarked_ms=%.9f inclusive_ms=%.9f self_ms=%.9f idle_ms=%.9f "
			"heap_available=1 process_allocated_bytes=%" PRIu64 " process_allocated_blocks=%" PRIu64
			" process_live_before=%" PRId64 " process_live_after=%" PRId64 " process_blocks_before=%" PRId64
			" process_blocks_after=%" PRId64 " process_peak_bytes=%" PRId64
			" profiler_overhead_before=%" PRId64 " profiler_overhead_after=%" PRId64 " heap_dropped=%" PRIu64
			" frame_dropped=%zu\n",
			id,
			sample,
			sample <= 8,
			operation,
			FrameGraph::FrameMilliseconds(),
			FrameGraph::UnmarkedMilliseconds(),
			spans.front().Milliseconds,
			spans.front().SelfMilliseconds,
			spans.front().IdleMilliseconds,
			after.TotalBytes - before.TotalBytes,
			after.TotalBlocks - before.TotalBlocks,
			before.LiveBytes,
			after.LiveBytes,
			before.LiveBlocks,
			after.LiveBlocks,
			after.PeakBytes,
			before.OverheadBytes,
			after.OverheadBytes,
			after.DroppedScopes,
			static_cast<size_t>(FrameGraph::Dropped())
		);
		std::printf(
			"# scheduler-timing-owner-heap case=%zu phase=%zu operation=%s allocated_bytes=%" PRIu64
			" allocated_blocks=%" PRIu64 " live_before=%" PRId64 " live_after=%" PRId64
			" blocks_before=%" PRId64 " blocks_after=%" PRId64 " sum_node_lifetime_peak_bytes=%" PRId64 "\n",
			id,
			sample,
			operation,
			ownerAfter.TotalBytes - ownerBefore.TotalBytes,
			ownerAfter.TotalBlocks - ownerBefore.TotalBlocks,
			ownerBefore.LiveBytes,
			ownerAfter.LiveBytes,
			ownerBefore.LiveBlocks,
			ownerAfter.LiveBlocks,
			ownerAfter.PeakBytes
		);
		for (size_t index = 0; index < spans.size(); ++index) {
			const auto &span = spans[index];
			Require(std::isfinite(span.Milliseconds) && span.Milliseconds >= 0);
			Require(std::isfinite(span.SelfMilliseconds) && span.SelfMilliseconds >= 0);
			Require(std::isfinite(span.IdleMilliseconds) && span.IdleMilliseconds >= 0);
			Require(std::isfinite(span.StartMilliseconds) && span.StartMilliseconds >= 0);
			Require(span.Parent == FrameGraph::NO_PARENT || span.Parent < index);
			std::printf(
				"# scheduler-timing-span case=%zu phase=%zu operation=%s index=%zu name=%.*s "
				"category=%u parent=%u depth=%u start_ms=%.9f inclusive_ms=%.9f self_ms=%.9f "
				"idle_ms=%.9f reported=%d owner=%u\n",
				id,
				sample,
				operation,
				index,
				static_cast<int>(span.Name.size()),
				span.Name.data(),
				static_cast<unsigned>(span.Category),
				span.Parent,
				span.Depth,
				span.StartMilliseconds,
				span.Milliseconds,
				span.SelfMilliseconds,
				span.IdleMilliseconds,
				span.Reported,
				static_cast<unsigned>(span.Owner)
			);
		}
	}
	void Preflight() {
		Store world("scheduler-preflight");
		Scheduler scheduler;
		int completed = 0;
		const std::string shared(96, 's');
		const std::string added(96, 'a');
		// Long strings retain their allocation when registration relocates Systems.
		scheduler.Add(shared, Phase::Input, [&](Store &) { ++completed; });
		scheduler.Add(shared, Phase::Render, [&](Store &) { ++completed; });
		scheduler.RunPhases(world, Phase::Render, Phase::Render);
		scheduler.AddParallel(added, Phase::Simulation, [](const Store &) {});
		scheduler.RunPhases(world, Phase::Input, Phase::Simulation);
		Require(completed == 2 && scheduler.Timings().size() == 2);
		Require(scheduler.Timings()[0].Name == shared && scheduler.Timings()[0].RunPhase == Phase::Render);
		Require(scheduler.Timings()[1].Name == added);
		Require(!scheduler.Replace(shared, 2, [](Store &) {}));
		Require(scheduler.SystemRevision(shared) == 0);
		Require(scheduler.ReplaceParallel(added, 2, [&](const Store &) { completed += 10; }));
		Require(!scheduler.ReplaceParallel(added, 2, [](const Store &) {}));
		Require(scheduler.SystemRevision(added) == 2);
		scheduler.ClearTimings();
		scheduler.RunPhases(world, Phase::Input, Phase::Simulation);
		Require(completed == 13 && scheduler.Timings()[0].RunPhase == Phase::Input);
		Scheduler nested;
		nested.Add("outer", Phase::Input, [&](Store &store) {
			nested.RunPhases(store, Phase::Render, Phase::Render);
		});
		nested.Add("inner", Phase::Render, [](Store &) {});
		nested.Add("throw", Phase::Simulation, [](Store &) { throw std::runtime_error("preflight"); });
		bool threw = false;
		try {
			nested.RunPhases(world, Phase::Input, Phase::Simulation);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		Require(threw && nested.Timings().size() == 2);
		Require(nested.Timings()[0].Name == "inner" && nested.Timings()[1].Name == "outer");
	}
	void Run() {
		const char *enabled = std::getenv("ATOMIC_SCHEDULER_TIMING_PROFILE");
		if (!enabled || std::string_view(enabled) != "1") {
			Fixture fixture(64, false, 7, false);
			fixture.Register();
			fixture.Execute(false);
			fixture.Verify(false, 0);
			return;
		}
		Require(HeapProfile::IsCompiledIn());
		const char *selection = std::getenv("ATOMIC_SCHEDULER_TIMING_CASE");
		Require(selection && *selection);
		for (const char *digit = selection; *digit; ++digit)
			Require(*digit >= '0' && *digit <= '9');
		char *end = nullptr;
		const auto selectedCase = std::strtoul(selection, &end, 10);
		Require(end && *end == '\0' && selectedCase < 80);
		Preflight();
		static size_t sample = 0;
		Require(++sample <= 13);
		const bool collecting = FrameGraph::IsEnabled();
		const bool startedPool = engine::parallel::Jobs::WorkerCount() == 0;
		if (startedPool) engine::parallel::Jobs::Start(3);
		FrameGraph::SetEnabled(true);
		try {
			size_t id = 0;
			for (size_t count : {0u, 1u, 8u, 64u, 256u})
				for (bool longNames : {false, true})
					for (size_t phases : {1u, 7u})
						for (bool parallel : {false, true})
							for (bool split : {false, true}) {
								if (id != selectedCase) {
									++id;
									continue;
								}
								Fixture fixture(count, longNames, phases, parallel);
								std::printf(
									"# scheduler-timing-case case=%zu phase=%zu systems=%zu long_names=%d "
									"phases=%zu parallel=%d split=%d\n",
									id,
									sample,
									count,
									longNames,
									phases,
									parallel,
									split
								);
								Capture(
									id,
									sample,
									"register",
									[&] { fixture.Register(); },
									[&] {
										Require(fixture.Systems.Timings().empty());
										for (size_t index = 0; index < count; ++index)
											Require(
												fixture.Outputs[index] == 0 &&
												fixture.Systems.SystemRevision(fixture.Names[index]) == 1
											);
									}
								);
								Capture(
									id,
									sample,
									"rebuild",
									[&] { fixture.Execute(split); },
									[&] { fixture.Verify(split, 0); }
								);
								for (size_t warmup = 0; warmup < 8; ++warmup) {
									fixture.Execute(split);
									fixture.Verify(split, (warmup + 1) * (split ? 2 : 1));
								}
								Capture(
									id,
									sample,
									"run",
									[&] { fixture.Execute(split); },
									[&] { fixture.Verify(split, 9 * (split ? 2 : 1)); }
								);
								std::printf(
									"# scheduler-timing-canonical case=%zu phase=%zu fixture_warmups=8 "
									"hash=%" PRIu64 "\n",
									id,
									sample,
									fixture.Canonical()
								);
								++id;
							}
		} catch (...) {
			FrameGraph::SetEnabled(collecting);
			if (startedPool) engine::parallel::Jobs::Stop();
			throw;
		}
		FrameGraph::SetEnabled(collecting);
		if (startedPool) engine::parallel::Jobs::Stop();
	}
}

BENCH("Scheduler timing diagnostic", 1) {
	scheduler_timing_bench::Run();
}
