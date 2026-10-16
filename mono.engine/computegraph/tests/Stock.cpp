#include <engine/computegraph/Stock.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.computegraph.stock")

TEST_CASE(
	"stock compaction keeps reusable kernels and explicit device count across hierarchy depths",
	"[computegraph]"
) {
	using namespace engine::computegraph;
	for (const auto capacity : {0u, 1u, 65u, 4097u, 65537u, 1048576u, 65'535u * 64u}) {
		Program program;
		Diagnostic diagnostic;
		REQUIRE(StockStableCompactU32(capacity, program, diagnostic) == Status::Ok);
		CHECK(program.Definition.Kernels.size() == 4);
		CHECK(program.Kernels.size() == 4);
		const auto packed = std::find_if(
			program.Definition.Buffers.begin(), program.Definition.Buffers.end(), [](const auto &b) {
				return b.Name == "packed";
			}
		);
		REQUIRE(packed != program.Definition.Buffers.end());
		CHECK(packed->Capacity == capacity);
		CHECK(packed->CountBuffer == "count");
		CHECK(program.Definition.Passes.front().DispatchBuffer == "flags");
		CHECK(program.Definition.Passes.back().Kernel == "scatter");
		REQUIRE_FALSE(program.ErrorBuffers.empty());
		for (const auto &code : program.Kernels) {
			CHECK(code.Source.empty());
			CHECK_FALSE(code.BuiltIn.empty());
		}
		CHECK(
			std::none_of(
				program.Definition.Buffers.begin(), program.Definition.Buffers.end(), [](const auto &b) {
					return b.Name == "doubled";
				}
			)
		);
	}
}
TEST_CASE("stock compaction refuses oversize without replacing an accepted plan", "[computegraph]") {
	using namespace engine::computegraph;
	Program program;
	Diagnostic diagnostic;
	REQUIRE(StockStableCompactU32(65, program, diagnostic) == Status::Ok);
	const auto passes = program.Definition.Passes.size();
	CHECK(StockStableCompactU32(UINT32_MAX, program, diagnostic) == Status::LimitExceeded);
	CHECK(program.Definition.Passes.size() == passes);
	CHECK(program.Definition.Buffers.front().Capacity == 65);

	constexpr uint32_t maximumCapacity = 65'535u * 64u;
	REQUIRE(StockStableCompactU32(maximumCapacity, program, diagnostic) == Status::Ok);
	const auto maximumPasses = program.Definition.Passes.size();
	CHECK(StockStableCompactU32(maximumCapacity + 1, program, diagnostic) == Status::LimitExceeded);
	CHECK(program.Definition.Passes.size() == maximumPasses);
	CHECK(program.Definition.Buffers.front().Capacity == maximumCapacity);
}
