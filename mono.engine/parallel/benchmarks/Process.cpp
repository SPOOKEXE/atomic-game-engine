// The fixed setup cost of a processed job context.

#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Bench.hpp>

#include <filesystem>

TEST_SUITE_ID("engine.parallel.bench.process")

BENCH("Processed context · spawn and reap one child", 20) {
	const std::filesystem::path self =
		engine::core::Paths::Base() / engine::core::Paths::Program("bench_parallel");
	for (int pass = 0; pass < 20; pass++) {
		engine::parallel::Process child;
		if (child.Start(self, {"--mono-suites"})) {
			engine::testing::Consume(child.Wait().Code);
		}
	}
}
