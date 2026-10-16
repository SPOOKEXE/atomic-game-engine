// Typed and dependency-only graph compilation, including the allocation-free
// order path used by realtime consumers. Fixtures are checked before timing.

#include <engine/core/HeapProfile.hpp>
#include <engine/graphcore/Compile.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

TEST_SUITE_ID("engine.graphcore.bench.compile")

namespace {
	using namespace engine::graphcore;
	using engine::testing::Consume;

	constexpr Limits LIMITS{
		.MaximumNodes = 1024,
		.MaximumConnections = 4096,
		.MaximumSchemas = 4,
		.MaximumPorts = 8,
		.MaximumIdentityBytes = 128 * 1024
	};

	struct Fixture {
		std::vector<Node> Nodes;
		std::vector<Connection> Connections;
		std::vector<Dependency> Dependencies;
		std::vector<NodeSchema> Schemas;
	};

	Fixture Make(size_t count) {
		Fixture fixture;
		fixture.Schemas.push_back(
			{.Id = "unit",
			 .Ports = {
				 {.Id = "in", .ValueType = "value", .Optional = true},
				 {.Id = "out", .ValueType = "value", .Direction = PortDirection::Output}
			 }}
		);
		for (size_t index = 0; index < count; ++index) {
			fixture.Nodes.push_back({"node" + std::to_string(index), "unit"});
			if (index > 0) {
				fixture.Dependencies.push_back(
					{static_cast<NodeIndex>(index - 1), static_cast<NodeIndex>(index)}
				);
				fixture.Connections.push_back(
					{fixture.Nodes[index - 1].Id, "out", fixture.Nodes[index].Id, "in"}
				);
			}
		}
		Diagnostic diagnostic;
		CompiledGraph compiled;
		if (ValidateSchemas(fixture.Schemas, LIMITS, diagnostic) != CompileStatus::Ok ||
			CompileGraph(fixture.Nodes, fixture.Connections, fixture.Schemas, LIMITS, compiled, diagnostic) !=
				CompileStatus::Ok ||
			compiled.Dependencies.Order.size() != count ||
			compiled.Connections.size() != fixture.Connections.size())
			throw std::runtime_error("graphcore benchmark fixture refused");
		for (size_t index = 0; index < count; ++index) {
			if (compiled.Dependencies.Order[index] != index ||
				compiled.Dependencies.Inputs[index] !=
					(index == 0 ? std::vector<NodeIndex>{}
								: std::vector<NodeIndex>{static_cast<NodeIndex>(index - 1)}))
				throw std::runtime_error("graphcore benchmark order or adjacency changed");
		}
		return fixture;
	}

	const Fixture EMPTY = Make(0);
	const Fixture SIMPLE = Make(3);
	const Fixture BUSY = Make(64);
	const Fixture COMPLEX = Make(512);

	void Typed(const Fixture &fixture) {
		CompiledGraph compiled;
		Diagnostic diagnostic;
		if (CompileGraph(fixture.Nodes, fixture.Connections, fixture.Schemas, LIMITS, compiled, diagnostic) !=
			CompileStatus::Ok)
			throw std::runtime_error("graphcore typed compile refused");
		Consume(compiled.Dependencies.Order.size());
		Consume(compiled.Connections.size());
	}

	void Dependencies(const Fixture &fixture, OrderPolicy policy) {
		Topology topology;
		Diagnostic diagnostic;
		if (CompileDependencies(
				fixture.Nodes.size(), fixture.Dependencies, LIMITS, topology, diagnostic, policy
			) != CompileStatus::Ok)
			throw std::runtime_error("graphcore dependency compile refused");
		Consume(topology.Order.size());
		Consume(topology.Waves.size());
	}

	void Prepared(
		const Fixture &fixture, OrderPolicy policy, OrderWorkspace &workspace, std::vector<NodeIndex> &order
	) {
		const OrderResult result =
			CompileOrder(fixture.Nodes.size(), fixture.Dependencies, LIMITS, workspace, order, policy);
		if (result.Status != CompileStatus::Ok || result.Count != fixture.Nodes.size())
			throw std::runtime_error("graphcore prepared compile refused");
		Consume(order.data());
	}

	OrderWorkspace BUSY_WORKSPACE(64, 63);
	std::vector<NodeIndex> BUSY_ORDER(64);

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("graphcore heap probe requires MONO_HEAP_PROFILE");
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
				  << '\t' << after.LiveBytes << '\t' << peak << '\n';
	}

	void ProbeMemoryOnce() {
		static const bool probed = [] {
			if (const char *setting = std::getenv("MONO_BENCH_MEMORY");
				setting != nullptr && setting[0] == '1' && setting[1] == '\0') {
				std::cout
					<< "# b0 heap "
					   "v1\tname\tallocated_bytes\tallocated_blocks\tretained_bytes\tlive_bytes\tscope_peak_"
					   "bytes\n";
				ProbeHeap("b0 graphcore simple", [] { Typed(SIMPLE); });
				ProbeHeap("b0 graphcore busy", [] { Typed(BUSY); });
				ProbeHeap("b0 graphcore complex", [] { Typed(COMPLEX); });
				ProbeHeap("b0 graphcore prepared", [] {
					Prepared(BUSY, OrderPolicy::AuthoredSweep, BUSY_WORKSPACE, BUSY_ORDER);
				});
			}
			return true;
		}();
		(void)probed;
	}
}

BENCH("CompileGraph empty accepted", 1) {
	ProbeMemoryOnce();
	Typed(EMPTY);
}
BENCH("CompileGraph simple 3", 1) {
	Typed(SIMPLE);
}
BENCH("CompileGraph busy 64", 1) {
	Typed(BUSY);
}
BENCH("CompileGraph complex 512", 1) {
	Typed(COMPLEX);
}
BENCH("CompileDependencies 64 authored sweep", 1) {
	Dependencies(BUSY, OrderPolicy::AuthoredSweep);
}
BENCH("CompileDependencies 64 lowest ready", 1) {
	Dependencies(BUSY, OrderPolicy::LowestReady);
}
BENCH("CompileDependencies 64 FIFO", 1) {
	Dependencies(BUSY, OrderPolicy::FifoReady);
}
BENCH("CompileOrder prepared 64 authored sweep", 1) {
	Prepared(BUSY, OrderPolicy::AuthoredSweep, BUSY_WORKSPACE, BUSY_ORDER);
}
