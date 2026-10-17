#include <engine/testing/Bench.hpp>
#include <engine/weather/Assemblies.hpp>

TEST_SUITE_ID("engine.weather.bench.assemblies")

namespace {
	using namespace engine::weather;
	constexpr size_t ITERATIONS = 1000;

	const AssemblySummary &Fixture() {
		static const AssemblySummary summary = [] {
			WeatherDefinition definition;
			definition.ThicknessCells = 100;
			const auto time = *ResolveTime(InitialAnchor(definition), 100);
			const AssemblyRegion region{{0, 16, 0}, 1, 0};
			return *Summarize(region, time, *EvaluateChildren(definition, time, region));
		}();
		return summary;
	}
}

BENCH("eight children: bounded balanced reconstruction", ITERATIONS) {
	const auto &summary = Fixture();
	for (size_t iteration = 0; iteration < ITERATIONS; ++iteration)
		engine::testing::Consume(
			engine::weather::SplitBalanced(summary, static_cast<uint32_t>(iteration), 7, 4)
		);
}

BENCH("eight children: scalar/front and pairwise wind merge predicates", ITERATIONS) {
	const auto &summary = Fixture();
	for (size_t iteration = 0; iteration < ITERATIONS; ++iteration)
		engine::testing::Consume(engine::weather::CanMerge(summary, {}));
}

BENCH("eight children: canonical reconstruction and exact summary", ITERATIONS) {
	engine::weather::WeatherDefinition definition;
	const auto time = *engine::weather::ResolveTime(engine::weather::InitialAnchor(definition), 100);
	const engine::weather::AssemblyRegion region{{0, 16, 0}, 1, 0};
	for (size_t iteration = 0; iteration < ITERATIONS; ++iteration) {
		const auto children = engine::weather::EvaluateChildren(definition, time, region);
		engine::testing::Consume(engine::weather::Summarize(region, time, *children));
	}
}
