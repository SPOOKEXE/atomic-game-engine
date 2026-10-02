// Cell-size costs on deterministic 4000-body scenes and adjacent floor-only baselines. The retained
// per-tick records include warmup calls; the last --samples records are measured.
#include "../tests/fixtures/SteppingScene.hpp"
#include "PipelineInternals.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.cell-sizes")

namespace {
	using namespace engine;
	using physics::testing::BuildScene;
	using physics::testing::Layout;
	using physics::testing::Tick;

	constexpr std::array<std::string_view, 4> PHASES{
		"physics.sync-broadphase", "physics.broadphase", "physics.narrowphase", "physics.solve"
	};

	struct RestoreSettings {
		bool Serial = parallel::ForceSerialCompute();
		bool Profile = core::FrameGraph::IsEnabled();

		~RestoreSettings() {
			core::FrameGraph::SetEnabled(Profile);
			parallel::SetForceSerialCompute(Serial);
		}
	};

	struct Reading {
		size_t Call = 0;
		size_t Pairs = 0;
		size_t Manifolds = 0;
		size_t Events = 0;
		size_t Dropped = 0;
		size_t Spans = 0;
		bool Tree = false;
		float OwnerElapsedMilliseconds = 0.0f;
		std::array<float, PHASES.size()> PhaseMilliseconds{};
	};

	struct Fixture {
		Layout Arrangement;
		float CellSize;
		size_t Bodies;
		std::unique_ptr<ecs::Store> Store;
		std::array<Reading, 13> Readings{};
		size_t ReadingCount = 0;

		Fixture(Layout layout, float cellSize, size_t bodies)
			: Arrangement(layout), CellSize(cellSize), Bodies(bodies),
			  Store(BuildScene(layout, bodies, cellSize)) {
			const RestoreSettings restore;
			parallel::SetForceSerialCompute(true);
			for (size_t tick = 0; tick < 10; tick++)
				Tick(*Store);
		}

		~Fixture() {
			for (size_t index = 0; index < ReadingCount; index++) {
				const Reading &reading = Readings[index];
				std::printf(
					"# cell-profile layout=%d cell=%.0f bodies=%zu call=%zu pairs=%zu manifolds=%zu "
					"events=%zu tree=%d "
					"dropped=%zu spans=%zu owner_elapsed_ms=%.6f sync_ms=%.6f broad_ms=%.6f narrow_ms=%.6f "
					"solve_ms=%.6f\n",
					static_cast<int>(Arrangement),
					CellSize,
					Bodies,
					reading.Call,
					reading.Pairs,
					reading.Manifolds,
					reading.Events,
					reading.Tree,
					reading.Dropped,
					reading.Spans,
					reading.OwnerElapsedMilliseconds,
					reading.PhaseMilliseconds[0],
					reading.PhaseMilliseconds[1],
					reading.PhaseMilliseconds[2],
					reading.PhaseMilliseconds[3]
				);
			}
		}

		void Measure() {
			if (ReadingCount == Readings.size())
				throw std::runtime_error(
					"cell-size sweep supports at most five measured samples after eight warmups"
				);
			const RestoreSettings restore;
			parallel::SetForceSerialCompute(true);
			core::FrameGraph::SetEnabled(true);
			core::FrameGraph::BeginFrame();
			Tick(*Store);
			core::FrameGraph::EndFrame();
			Reading reading;
			reading.Call = ReadingCount + 1;
			const physics::PhysicsWorld &world = *Store->Resource<physics::PhysicsWorld>();
			reading.Pairs = world.Pairs().size();
			reading.Manifolds = world.Manifolds().size();
			reading.Events = world.Events().size();
			reading.Tree = physics::PipelineInternals::DynamicTreeActive(world);
			reading.Dropped = core::FrameGraph::Dropped();
			reading.Spans = core::FrameGraph::Spans().size();
			reading.OwnerElapsedMilliseconds = core::FrameGraph::FrameMilliseconds();
			std::array<bool, PHASES.size()> found{};
			for (const core::FrameSpan &span : core::FrameGraph::Spans()) {
				for (size_t phase = 0; phase < PHASES.size(); phase++) {
					if (span.Name == PHASES[phase]) {
						reading.PhaseMilliseconds[phase] += span.Milliseconds;
						found[phase] = true;
					}
				}
			}
			Readings[ReadingCount++] = reading;
			if (reading.Dropped != 0 || std::find(found.begin(), found.end(), false) != found.end()) {
				throw std::runtime_error("cell-size profile dropped scopes or missed a required phase");
			}
			engine::testing::Consume(reading.Pairs);
		}
	};

	void Measure(Layout layout, float cellSize, size_t bodies) {
		static std::vector<std::unique_ptr<Fixture>> fixtures;
		for (const auto &fixture : fixtures) {
			if (fixture->Arrangement == layout && fixture->CellSize == cellSize &&
				fixture->Bodies == bodies) {
				fixture->Measure();
				return;
			}
		}
		fixtures.push_back(std::make_unique<Fixture>(layout, cellSize, bodies));
		fixtures.back()->Measure();
	}
}

BENCH("Serial floor-only baseline · pile control · 4m cells", 1) {
	Measure(Layout::Pile, 4.0f, 0);
}

BENCH("Serial tick profile · 4000 pile bodies · 4m cells", 1) {
	Measure(Layout::Pile, 4.0f, 4000);
}

BENCH("Serial floor-only baseline · pile control · 2m cells", 1) {
	Measure(Layout::Pile, 2.0f, 0);
}

BENCH("Serial tick profile · 4000 pile bodies · 2m cells", 1) {
	Measure(Layout::Pile, 2.0f, 4000);
}

BENCH("Serial floor-only baseline · pile control · 1m cells", 1) {
	Measure(Layout::Pile, 1.0f, 0);
}

BENCH("Serial tick profile · 4000 pile bodies · 1m cells", 1) {
	Measure(Layout::Pile, 1.0f, 4000);
}

BENCH("Serial floor-only baseline · stacks control · 4m cells", 1) {
	Measure(Layout::Stacks, 4.0f, 0);
}

BENCH("Serial tick profile · 4000 stacks bodies · 4m cells", 1) {
	Measure(Layout::Stacks, 4.0f, 4000);
}

BENCH("Serial floor-only baseline · stacks control · 2m cells", 1) {
	Measure(Layout::Stacks, 2.0f, 0);
}

BENCH("Serial tick profile · 4000 stacks bodies · 2m cells", 1) {
	Measure(Layout::Stacks, 2.0f, 4000);
}

BENCH("Serial floor-only baseline · stacks control · 1m cells", 1) {
	Measure(Layout::Stacks, 1.0f, 0);
}

BENCH("Serial tick profile · 4000 stacks bodies · 1m cells", 1) {
	Measure(Layout::Stacks, 1.0f, 4000);
}

BENCH("Serial floor-only baseline · scattered control · 4m cells", 1) {
	Measure(Layout::Scattered, 4.0f, 0);
}

BENCH("Serial tick profile · 4000 scattered bodies · 4m cells", 1) {
	Measure(Layout::Scattered, 4.0f, 4000);
}

BENCH("Serial floor-only baseline · scattered control · 2m cells", 1) {
	Measure(Layout::Scattered, 2.0f, 0);
}

BENCH("Serial tick profile · 4000 scattered bodies · 2m cells", 1) {
	Measure(Layout::Scattered, 2.0f, 4000);
}

BENCH("Serial floor-only baseline · scattered control · 1m cells", 1) {
	Measure(Layout::Scattered, 1.0f, 0);
}

BENCH("Serial tick profile · 4000 scattered bodies · 1m cells", 1) {
	Measure(Layout::Scattered, 1.0f, 4000);
}
