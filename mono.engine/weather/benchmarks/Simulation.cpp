#include <engine/testing/Bench.hpp>
#include <engine/weather/Checkpoint.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.weather.bench.simulation")

namespace {
	engine::weather::SimulationState &Solver() {
		static auto state = [] {
			engine::weather::SimulationDefinition definition;
			definition.Grid.Cells = {16, 16, 16};
			definition.Grid.CellSizeMetres = 64;
			definition.Grid.Boundaries.fill(engine::weather::SolverBoundary::Periodic);
			engine::weather::SolverCell initial;
			initial.VelocityMetresPerSecond = {6, 0, 0};
			const auto created = engine::weather::CreateSimulation(definition, initial);
			if (!created) throw std::runtime_error("weather benchmark grid refused");
			return *created;
		}();
		return state;
	}
}

BENCH("weather constant-density 4096 cells cold workspace tick", 1) {
	const auto status = engine::weather::StepSimulation(Solver());
	if (status != engine::weather::SimulationStatus::Success)
		throw std::runtime_error("weather benchmark step refused");
	engine::testing::Consume(status);
}

BENCH("weather constant-density 4096 cells warm workspace tick", 1) {
	static engine::weather::SimulationWorkspace workspace;
	const auto status = engine::weather::StepSimulation(Solver(), workspace);
	if (status != engine::weather::SimulationStatus::Success)
		throw std::runtime_error("weather warm benchmark step refused");
	engine::testing::Consume(status);
}

BENCH("weather checkpoint 4096 full-precision cells encode", 1) {
	const auto encoded = engine::weather::EncodeCheckpoint(Solver());
	if (!encoded || encoded->size() < 4096 * sizeof(engine::weather::SolverCell))
		throw std::runtime_error("weather benchmark checkpoint refused or incomplete");
	engine::testing::Consume(*encoded);
}

BENCH("weather presentation 4096 eight-byte cells pack", 1) {
	const auto packed = engine::weather::PackSimulation(Solver(), engine::weather::EncodingProfile{});
	if (!packed || packed->size() != 4096)
		throw std::runtime_error("weather benchmark packing refused or incomplete");
	engine::testing::Consume(*packed);
}
