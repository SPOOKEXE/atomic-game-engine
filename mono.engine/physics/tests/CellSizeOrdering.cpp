#include "fixtures/CellSizeJobs.hpp"
#include "fixtures/CellSizeParity.hpp"
#include "fixtures/SteppingScene.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using engine::physics::testing::JobPoolFixture;

TEST_SUITE_ID("engine.physics.cell-size-ordering")
TEST_DEPENDS("engine.physics.broadphase")
TEST_DEPENDS("engine.physics.narrowphase")
TEST_DEPENDS("engine.physics.solver")
TEST_DEPENDS("engine.parallel.jobs")

TEST_CASE(
	"representative cell-size scenes preserve exact output in both orders", "[slow][physics][cell-sizes]"
) {
	JobPoolFixture jobPool;
	using namespace engine;
	using namespace engine::physics::testing;
	for (const Layout layout : {Layout::Pile, Layout::Stacks, Layout::Scattered, Layout::MixedScale}) {
		for (const bool reverse : {false, true}) {
			CAPTURE(static_cast<int>(layout), reverse);
			auto floor4 = BuildScene(layout, 0, 4.0f);
			auto scene4 = BuildScene(layout, 4000, 4.0f);
			auto floor2 = BuildScene(layout, 0, 2.0f);
			auto scene2 = BuildScene(layout, 4000, 2.0f);
			std::array<ecs::Store *, 4> worlds{floor4.get(), scene4.get(), floor2.get(), scene2.get()};
			const std::array<size_t, 4> order =
				reverse ? std::array<size_t, 4>{2, 3, 0, 1} : std::array<size_t, 4>{0, 1, 2, 3};
			for (size_t tick = 0; tick < 23; tick++) {
				CAPTURE(tick);
				for (const size_t index : order)
					Tick(*worlds[index]);
				REQUIRE(SameSnapshot(Capture(*floor4), Capture(*floor2)));
				REQUIRE(SameSnapshot(Capture(*scene4), Capture(*scene2)));
			}
		}
	}
}
