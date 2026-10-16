// Device-free orchestration compilation and synchronous schedule traversal.
// Execute rows measure scheduler and adapter dispatch, not game or GPU work.

#include <engine/core/HeapProfile.hpp>
#include <engine/graph/EngineGraph.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

TEST_SUITE_ID("engine.graph.bench.enginegraph")

namespace {
	using namespace engine::graph;
	using engine::core::Name;
	using engine::testing::Consume;

	struct Fixture {
		EngineGraph Graph;
		std::vector<Name> Order;
		size_t Barriers = 0;
	};

	class OracleRunner final : public EngineNodeRunner {
	  public:
		explicit OracleRunner(const Fixture &fixture) : Expected(fixture.Order) {}

		bool BeginWave(uint32_t wave, bool, std::span<const EngineBarrier> barriers) override {
			if (wave != Waves++) return false;
			BarrierCount += barriers.size();
			return true;
		}
		bool Run(const EngineRunContext &context) override {
			if (Calls >= Expected.size() || context.Work.Name != Expected[Calls]) return false;
			++Calls;
			return true;
		}
		bool EndWave(uint32_t wave) override {
			return wave + 1 == Waves;
		}

		size_t Calls = 0;
		size_t Waves = 0;
		size_t BarrierCount = 0;

	  private:
		const std::vector<Name> &Expected;
	};

	Fixture Make(size_t count, bool chain) {
		Fixture fixture;
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
			fixture.Order.push_back(node.Name);
			if (!fixture.Graph.AddNode(std::move(node)).IsValid())
				throw std::runtime_error("engine graph node refused");
		}
		EngineSchedule schedule;
		Name offender;
		if (CompileEngineGraph(fixture.Graph, schedule, offender) != EngineGraphStatus::Ok)
			throw std::runtime_error("engine graph fixture compile refused");
		fixture.Barriers = schedule.Barriers.size();
		if (fixture.Barriers != (chain && count > 0 ? count - 1 : 0) ||
			schedule.Waves.size() != (chain ? count : static_cast<size_t>(count != 0)))
			throw std::runtime_error("engine graph fixture wave/barrier count changed");
		for (size_t index = 0; index < fixture.Barriers; ++index) {
			const EngineBarrier &barrier = schedule.Barriers[index];
			if (barrier.Resource != Name("bench.resource." + std::to_string(index)) ||
				barrier.ProducerWave != index || barrier.ConsumerWave != index + 1 ||
				barrier.From != (index % 2 == 0 ? ExecutionQueue::Cpu : ExecutionQueue::Compute) ||
				barrier.To != (index % 2 == 0 ? ExecutionQueue::Compute : ExecutionQueue::Cpu))
				throw std::runtime_error("engine graph handover changed");
		}
		OracleRunner runner(fixture);
		if (!ExecuteEngineGraph(fixture.Graph, schedule, runner) || runner.Calls != count ||
			runner.BarrierCount != fixture.Barriers)
			throw std::runtime_error("engine graph fixture execution changed");
		return fixture;
	}

	const Fixture EMPTY = Make(0, false);
	const Fixture SIMPLE = Make(3, true);
	const Fixture BUSY = Make(64, false);
	const Fixture COMPLEX = Make(64, true);
	[[maybe_unused]] const bool DETERMINISTIC_VERIFIED = [] {
		EngineSchedule schedule;
		Name offender;
		if (CompileEngineGraph(BUSY.Graph, schedule, offender, EngineExecutionMode::Deterministic) !=
				EngineGraphStatus::Ok ||
			schedule.Waves.size() != BUSY.Order.size())
			throw std::runtime_error("engine graph deterministic fixture refused");
		for (size_t index = 0; index < schedule.Waves.size(); ++index)
			if (schedule.Waves[index].Nodes.size() != 1 ||
				schedule.Waves[index].Nodes.front().Node.Value != index + 1 ||
				schedule.Waves[index].Concurrent)
				throw std::runtime_error("engine graph deterministic order changed");
		return true;
	}();

	void Compile(const Fixture &fixture, EngineExecutionMode mode) {
		EngineSchedule schedule;
		Name offender;
		if (CompileEngineGraph(fixture.Graph, schedule, offender, mode) != EngineGraphStatus::Ok)
			throw std::runtime_error("engine graph compile refused");
		Consume(schedule.Waves.size());
		Consume(schedule.Barriers.size());
	}

	void Execute(const Fixture &fixture, EngineExecutionMode mode) {
		EngineSchedule schedule;
		Name offender;
		if (CompileEngineGraph(fixture.Graph, schedule, offender, mode) != EngineGraphStatus::Ok)
			throw std::runtime_error("engine graph compile refused");
		OracleRunner runner(fixture);
		if (!ExecuteEngineGraph(fixture.Graph, schedule, runner) || runner.Calls != fixture.Order.size() ||
			runner.BarrierCount != fixture.Barriers)
			throw std::runtime_error("engine graph execution changed");
		Consume(runner.Calls);
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
				ProbeHeap("b0 enginegraph execute busy", [] {
					Execute(BUSY, EngineExecutionMode::Concurrent);
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
	Execute(BUSY, EngineExecutionMode::Concurrent);
}
BENCH("Compile plus Execute scheduler-only 64 chained handovers", 1) {
	Execute(COMPLEX, EngineExecutionMode::Concurrent);
}
