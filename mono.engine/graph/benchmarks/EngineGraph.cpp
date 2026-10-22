// Device-free orchestration compilation and synchronous schedule traversal.
// Execute rows measure scheduler and adapter dispatch, not game or GPU work.

#include <engine/core/HeapProfile.hpp>
#include <engine/graph/EngineGraph.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.bench.enginegraph")

namespace {
	using namespace engine::graph;
	using engine::core::Name;
	using engine::testing::Consume;

	struct ExpectedNode {
		Name Identity;
		EngineNodeId Id;
		ExecutionQueue Queue = ExecutionQueue::Cpu;
	};

	struct ExpectedWave {
		size_t FirstNode = 0;
		size_t NodeCount = 0;
		bool Concurrent = false;
		std::vector<EngineBarrier> Barriers{};
	};

	struct Fixture {
		EngineGraph Graph;
		EngineSchedule Prepared;
		EngineExecutionMode Mode = EngineExecutionMode::Concurrent;
		std::vector<ExpectedNode> Order;
		std::vector<ExpectedWave> Waves;
		size_t Barriers = 0;
	};

	bool SameBarriers(std::span<const EngineBarrier> actual, std::span<const EngineBarrier> expected) {
		if (actual.size() != expected.size()) return false;
		for (size_t index = 0; index < expected.size(); ++index) {
			const auto &held = actual[index];
			const auto &golden = expected[index];
			if (held.Resource != golden.Resource || held.From != golden.From || held.To != golden.To ||
				held.ProducerWave != golden.ProducerWave || held.ConsumerWave != golden.ConsumerWave)
				return false;
		}
		return true;
	}

	class OracleRunner final : public EngineNodeRunner {
	  public:
		explicit OracleRunner(const Fixture &fixture) : Expected(fixture) {}

		bool BeginWave(uint32_t wave, bool concurrent, std::span<const EngineBarrier> barriers) override {
			if (Open || wave != Waves || Waves >= Expected.Waves.size()) return false;
			const auto &golden = Expected.Waves[Waves];
			if (Calls != golden.FirstNode || concurrent != golden.Concurrent ||
				!SameBarriers(barriers, golden.Barriers))
				return false;
			Open = true;
			WaveCalls = 0;
			BarrierCount += barriers.size();
			return true;
		}
		bool Run(const EngineRunContext &context) override {
			if (!Open || context.Wave != Waves || Calls >= Expected.Order.size()) return false;
			const auto &wave = Expected.Waves[Waves];
			const auto &node = Expected.Order[Calls];
			if (WaveCalls >= wave.NodeCount || context.Work.Name != node.Identity ||
				context.Node != node.Id || context.Work.Queue != node.Queue ||
				!SameBarriers(context.Barriers, wave.Barriers))
				return false;
			++Calls;
			++WaveCalls;
			return true;
		}
		bool EndWave(uint32_t wave) override {
			if (!Open || wave != Waves || WaveCalls != Expected.Waves[Waves].NodeCount) return false;
			Open = false;
			++Waves;
			return true;
		}
		bool Complete() const {
			return !Open && Calls == Expected.Order.size() && Waves == Expected.Waves.size() &&
				   BarrierCount == Expected.Barriers;
		}

		size_t Calls = 0;
		size_t Waves = 0;
		size_t BarrierCount = 0;

	  private:
		const Fixture &Expected;
		size_t WaveCalls = 0;
		bool Open = false;
	};

	Fixture Make(size_t count, bool chain, EngineExecutionMode mode = EngineExecutionMode::Concurrent) {
		Fixture fixture;
		fixture.Mode = mode;
		const bool serialWaves = chain || mode == EngineExecutionMode::Deterministic;
		for (size_t index = 0; index < count; ++index) {
			EngineNode node;
			node.Name = Name("bench.node." + std::to_string(index));
			node.Stage = EngineStage::World;
			node.Queue = chain ? (index % 2 == 0 ? ExecutionQueue::Cpu : ExecutionQueue::Compute)
							   : ExecutionQueue::Cpu;
			node.AsyncEligible = true;
			if (chain) {
				const Name resource("bench.resource." + std::to_string(index));
				if (!fixture.Graph.AddResource({resource, EngineResourceLifetime::Tick}))
					throw std::runtime_error("engine graph resource refused");
				node.Writes.push_back(resource);
				if (index > 0) node.Reads.push_back(Name("bench.resource." + std::to_string(index - 1)));
			}
			fixture.Order.push_back({node.Name, EngineNodeId{static_cast<uint32_t>(index + 1)}, node.Queue});
			if (serialWaves) {
				ExpectedWave wave{.FirstNode = index, .NodeCount = 1};
				if (chain && index > 0) {
					wave.Barriers.push_back({
						.Resource = Name("bench.resource." + std::to_string(index - 1)),
						.From = index % 2 == 0 ? ExecutionQueue::Compute : ExecutionQueue::Cpu,
						.To = node.Queue,
						.ProducerWave = static_cast<uint32_t>(index - 1),
						.ConsumerWave = static_cast<uint32_t>(index),
					});
				}
				fixture.Waves.push_back(std::move(wave));
			}
			if (!fixture.Graph.AddNode(std::move(node)).IsValid())
				throw std::runtime_error("engine graph node refused");
		}
		if (!serialWaves && count > 0) fixture.Waves.push_back({.NodeCount = count, .Concurrent = count > 1});
		fixture.Barriers = chain && count > 0 ? count - 1 : 0;
		Name offender;
		if (CompileEngineGraph(fixture.Graph, fixture.Prepared, offender, fixture.Mode) !=
			EngineGraphStatus::Ok)
			throw std::runtime_error("engine graph fixture compile refused");
		if (fixture.Prepared.Barriers.size() != fixture.Barriers ||
			fixture.Prepared.Waves.size() != fixture.Waves.size())
			throw std::runtime_error("engine graph fixture wave/barrier count changed");
		for (size_t index = 0; index < fixture.Barriers; ++index) {
			const EngineBarrier &barrier = fixture.Prepared.Barriers[index];
			if (barrier.Resource != Name("bench.resource." + std::to_string(index)) ||
				barrier.ProducerWave != index || barrier.ConsumerWave != index + 1 ||
				barrier.From != (index % 2 == 0 ? ExecutionQueue::Cpu : ExecutionQueue::Compute) ||
				barrier.To != (index % 2 == 0 ? ExecutionQueue::Compute : ExecutionQueue::Cpu))
				throw std::runtime_error("engine graph handover changed");
		}
		OracleRunner runner(fixture);
		if (!ExecuteEngineGraph(fixture.Graph, fixture.Prepared, runner) || !runner.Complete())
			throw std::runtime_error("engine graph fixture execution changed");
		return fixture;
	}

	const Fixture EMPTY = Make(0, false);
	const Fixture SIMPLE = Make(3, true);
	const Fixture BUSY = Make(64, false);
	const Fixture COMPLEX = Make(64, true);
	const Fixture BUSY_DETERMINISTIC = Make(64, false, EngineExecutionMode::Deterministic);

	void Compile(const Fixture &fixture, EngineExecutionMode mode) {
		EngineSchedule schedule;
		Name offender;
		if (CompileEngineGraph(fixture.Graph, schedule, offender, mode) != EngineGraphStatus::Ok)
			throw std::runtime_error("engine graph compile refused");
		Consume(schedule.Waves.size());
		Consume(schedule.Barriers.size());
	}

	void ExecutePrepared(const Fixture &fixture, const EngineSchedule &schedule) {
		OracleRunner runner(fixture);
		if (!ExecuteEngineGraph(fixture.Graph, schedule, runner) || !runner.Complete())
			throw std::runtime_error("engine graph execution changed");
		Consume(runner.Calls);
	}

	void Execute(const Fixture &fixture) {
		EngineSchedule schedule;
		Name offender;
		if (CompileEngineGraph(fixture.Graph, schedule, offender, fixture.Mode) != EngineGraphStatus::Ok)
			throw std::runtime_error("engine graph compile refused");
		ExecutePrepared(fixture, schedule);
	}

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("engine graph heap probe requires MONO_HEAP_PROFILE");
		const auto before = HeapProfile::Totals();
		{
			ENGINE_HEAP_SCOPE(name);
			work();
		}
		const auto after = HeapProfile::Totals();
		int64_t peak = 0;
		bool found = false;
		for (uint32_t index = 1; index < HeapProfile::NodeCount(); ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Parent == HeapProfile::ROOT && node.Name == name) {
				found = true;
				peak = node.PeakBytes;
			}
		}
		if (!found) throw std::runtime_error("benchmark heap scope was not recorded");
		std::cout << "# b0 heap\t" << name << '\t' << (after.TotalBytes - before.TotalBytes) << '\t'
				  << (after.TotalBlocks - before.TotalBlocks) << '\t' << (after.LiveBytes - before.LiveBytes)
				  << '\t' << after.LiveBytes << '\t' << peak << '\t' << after.LiveBlocks << '\t'
				  << after.PeakBytes << '\t' << after.OverheadBytes << '\t' << HeapProfile::BlockOverhead()
				  << '\t' << (after.DroppedScopes - before.DroppedScopes) << '\t'
				  << (after.ForeignFrees - before.ForeignFrees) << '\n';
	}

	void ProbeMemoryOnce() {
		static const bool probed = [] {
			if (const char *setting = std::getenv("MONO_BENCH_MEMORY");
				setting != nullptr && setting[0] == '1' && setting[1] == '\0') {
				std::cout << "# b0 heap basis\ttracked_cpp_requested_payload=1\t"
						  << "malloc_gpu_excluded=1\twork_invocations=1\tfixtures_prepared=1\t"
						  << "scope_peak=exclusive\tprocess_peak=lifetime\t"
						  << "external_contention_invalidates_timing=1\ttiming_valid=0\t"
						  << "counters=process_local\n";
				std::cout
					<< "# b0 heap "
					   "v2\tname\tprocess_allocated_bytes\tprocess_allocated_blocks\tprocess_retained_bytes\t"
					   "process_live_bytes\tscope_self_peak_bytes\tprocess_live_blocks\tprocess_peak_"
					   "lifetime_bytes\t"
					   "process_header_overhead_model_bytes\tblock_overhead_bytes\tdropped_scopes_"
					   "delta\tforeign_"
					   "frees_delta\n";
				ProbeHeap("b0 enginegraph empty", [] { Compile(EMPTY, EngineExecutionMode::Concurrent); });
				ProbeHeap("b0 enginegraph simple", [] { Compile(SIMPLE, EngineExecutionMode::Concurrent); });
				ProbeHeap("b0 enginegraph busy", [] { Compile(BUSY, EngineExecutionMode::Concurrent); });
				ProbeHeap("b0 enginegraph complex", [] {
					Compile(COMPLEX, EngineExecutionMode::Concurrent);
				});
				ProbeHeap("b0 enginegraph execute busy", [] { Execute(BUSY); });
				ProbeHeap("b0 enginegraph prepared execute empty", [] {
					ExecutePrepared(EMPTY, EMPTY.Prepared);
				});
				ProbeHeap("b0 enginegraph prepared execute simple", [] {
					ExecutePrepared(SIMPLE, SIMPLE.Prepared);
				});
				ProbeHeap("b0 enginegraph prepared execute busy", [] {
					ExecutePrepared(BUSY, BUSY.Prepared);
				});
				ProbeHeap("b0 enginegraph prepared execute complex", [] {
					ExecutePrepared(COMPLEX, COMPLEX.Prepared);
				});
				ProbeHeap("b0 enginegraph prepared execute busy deterministic", [] {
					ExecutePrepared(BUSY_DETERMINISTIC, BUSY_DETERMINISTIC.Prepared);
				});
			}
			return true;
		}();
		(void)probed;
	}
}

BENCH("Compile empty graph accepted", 1) {
	ProbeMemoryOnce();
	Compile(EMPTY, EngineExecutionMode::Concurrent);
}
BENCH("Compile simple 3-node chain", 1) {
	Compile(SIMPLE, EngineExecutionMode::Concurrent);
}
BENCH("Compile busy 64 independent", 1) {
	Compile(BUSY, EngineExecutionMode::Concurrent);
}
BENCH("Compile busy 64 deterministic", 1) {
	Compile(BUSY, EngineExecutionMode::Deterministic);
}
BENCH("Compile complex 64 chained handovers", 1) {
	Compile(COMPLEX, EngineExecutionMode::Concurrent);
}
BENCH("Compile plus Execute scheduler-only 64 independent", 1) {
	Execute(BUSY);
}
BENCH("Compile plus Execute scheduler-only 64 chained handovers", 1) {
	Execute(COMPLEX);
}
BENCH("Execute only prepared scheduler empty graph", 1) {
	ProbeMemoryOnce();
	ExecutePrepared(EMPTY, EMPTY.Prepared);
}
BENCH("Execute only prepared scheduler simple 3-node chain", 1) {
	ProbeMemoryOnce();
	ExecutePrepared(SIMPLE, SIMPLE.Prepared);
}
BENCH("Execute only prepared scheduler busy 64 independent", 1) {
	ProbeMemoryOnce();
	ExecutePrepared(BUSY, BUSY.Prepared);
}
BENCH("Execute only prepared scheduler complex 64 chained handovers", 1) {
	ProbeMemoryOnce();
	ExecutePrepared(COMPLEX, COMPLEX.Prepared);
}
BENCH("Execute only prepared scheduler busy 64 independent deterministic", 1) {
	ProbeMemoryOnce();
	ExecutePrepared(BUSY_DETERMINISTIC, BUSY_DETERMINISTIC.Prepared);
}
