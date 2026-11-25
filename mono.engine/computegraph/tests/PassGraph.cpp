#include "Fixture.hpp"

#include <engine/computegraph/Stock.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.computegraph.pass_graph")
TEST_DEPENDS("engine.computegraph.compile")
TEST_DEPENDS("engine.computegraph.stock")

using namespace engine::computegraph;

namespace {
	// FNV-1a over every planned field a rendering host reads.
	struct Fnv {
		uint64_t Value = 0xcbf29ce484222325ull;
		void Number(uint64_t value) {
			for (size_t index = 0; index < sizeof(value); ++index) {
				Value = (Value ^ ((value >> (index * 8)) & 0xff)) * 0x100000001b3ull;
			}
		}
	};

	uint64_t HashPlan(const CompiledGraph &plan) {
		Fnv hash;
		hash.Number(plan.Bytes);
		for (const auto &allocation : plan.Allocations) {
			hash.Number(allocation.Bytes);
			hash.Number(allocation.CountBuffer);
		}
		for (const auto &pass : plan.Passes) {
			hash.Number(pass.Kernel);
			hash.Number(pass.DispatchBuffer);
			for (const auto &binding : pass.Bindings) {
				hash.Number(binding.Port);
				hash.Number(binding.Buffer);
			}
			for (const uint32_t groups : pass.Work.Groups) {
				hash.Number(groups);
			}
			hash.Number(pass.Work.Elements);
			hash.Number(pass.Work.DeviceCount);
		}
		const auto lists = [&](const std::vector<std::vector<uint32_t>> &rows) {
			for (const auto &row : rows) {
				hash.Number(row.size());
				for (const uint32_t value : row) {
					hash.Number(value);
				}
			}
		};
		for (const uint32_t index : plan.Topology.Order) {
			hash.Number(index);
		}
		lists(plan.Topology.Inputs);
		lists(plan.Topology.Dependents);
		lists(plan.Topology.Waves);
		for (const auto &barrier : plan.Barriers) {
			hash.Number(barrier.Producer);
			hash.Number(barrier.Consumer);
			hash.Number(barrier.Buffer);
		}
		return hash.Value;
	}
}

// The stock compaction program compiles through the ang pass graph and budget
// run to the plan recorded from the graphcore path it replaced.
TEST_CASE("stock compaction program compiles to the recorded plan", "[computegraph]") {
	Program program;
	Diagnostic diagnostic;
	REQUIRE(StockStableCompactU32(65537, program, diagnostic) == Status::Ok);
	CompiledGraph plan;
	REQUIRE(
		Compile(program.Definition, tests::Reflections(program.Definition), {}, {}, plan, diagnostic) ==
		Status::Ok
	);
	CHECK(plan.Passes.size() == 7);
	CHECK(HashPlan(plan) == 0x6da88c886bff8c76ull);
}
