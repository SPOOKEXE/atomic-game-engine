#include <engine/testing/Bench.hpp>
#include <engine/weather/Residency.hpp>

#include <array>

TEST_SUITE_ID("engine.weather.bench.residency")

namespace {
	using namespace engine::weather;
	constexpr size_t ITERATIONS = 100;

	const std::array<BrickDemand, 4096> &Demands() {
		static const std::array<BrickDemand, 4096> demands = [] {
			std::array<BrickDemand, 4096> rows{};
			for (size_t index = 0; index < rows.size(); ++index)
				rows[index] = {
					{{static_cast<int32_t>(index * 8), 0, 0}, 0},
					static_cast<float>((index * 137) % 256),
					static_cast<float>(index % 64) / 256.0f,
					index % 16 == 0
				};
			return rows;
		}();
		return demands;
	}
}

BENCH("4096 sparse demands: coarse coverage and budgeted stable selection", ITERATIONS) {
	const auto &demands = Demands();
	for (size_t iteration = 0; iteration < ITERATIONS; ++iteration)
		engine::testing::Consume(engine::weather::SelectResidency(demands, 1024));
}

BENCH("four views: lazy maintenance cache hit", 10000) {
	const std::array<engine::weather::ResidencyView, 4> views{
		{{1, {}, 0}, {2, {}, 0}, {3, {}, 0}, {4, {}, 0}}
	};
	const auto stamp = *engine::weather::RecordMaintenance(views, 0, 1, 1);
	for (size_t iteration = 0; iteration < 10000; ++iteration)
		engine::testing::Consume(engine::weather::MaintenanceRequired(stamp, views, 99, 1, 1));
}
