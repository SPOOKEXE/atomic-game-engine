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

TEST_CASE("representative cell-size scenes preserve exact output in both orders", "[physics][cell-sizes]") {
	JobPoolFixture jobPool;
	using namespace engine;
	using namespace engine::physics::testing;
	// Fixed visit orders isolate cross-world state on small scenes; CellSizes covers full stress and workers.
	constexpr size_t BODY_COUNT = 128;
	for (const Layout layout : {Layout::Pile, Layout::Stacks, Layout::Scattered, Layout::MixedScale}) {
		for (const bool reverse : {false, true}) {
			CAPTURE(static_cast<int>(layout), reverse);
			auto floor4 = BuildScene(layout, 0, 4.0f);
			auto scene4 = BuildScene(layout, BODY_COUNT, 4.0f);
			auto floor2 = BuildScene(layout, 0, 2.0f);
			auto scene2 = BuildScene(layout, BODY_COUNT, 2.0f);
			std::array<ecs::Store *, 4> worlds{floor4.get(), scene4.get(), floor2.get(), scene2.get()};
			const std::array<size_t, 4> order =
				reverse ? std::array<size_t, 4>{2, 3, 0, 1} : std::array<size_t, 4>{0, 1, 2, 3};
			for (size_t tick = 0; tick < 23; tick++) {
				CAPTURE(tick);
				for (const size_t index : order)
					Tick(*worlds[index]);
				const CellSizeSnapshot expectedFloor = Capture(*floor4);
				const CellSizeSnapshot actualFloor = Capture(*floor2);
				REQUIRE(actualFloor.Pairs == expectedFloor.Pairs);
				REQUIRE(actualFloor.Manifolds == expectedFloor.Manifolds);
				REQUIRE(actualFloor.Events == expectedFloor.Events);
				REQUIRE(actualFloor.Bodies == expectedFloor.Bodies);
				const CellSizeSnapshot expected = Capture(*scene4);
				const CellSizeSnapshot actual = Capture(*scene2);
				REQUIRE(actual.Pairs == expected.Pairs);
				REQUIRE(actual.Manifolds == expected.Manifolds);
				REQUIRE(actual.Events == expected.Events);
				REQUIRE(actual.Bodies == expected.Bodies);
			}
		}
	}
}
