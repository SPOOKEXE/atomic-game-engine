#include <engine/physics/PortalMotion.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.physics.portalmotion")

TEST_CASE("moving portal mouths map body velocity relative to each mouth", "[physics][portal]") {
	using namespace engine;
	scene::SeamTransform through;
	through.Frame = core::CFrame({10, 0, 0});
	through.Origin = {0, 0, 0};
	through.Scale = 2;
	const physics::PortalMouthMotion source{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
	const physics::PortalMouthMotion destination{{10, 0, 0}, {0, 2, 0}, {0, 1, 0}};
	const scene::Motion body{{4, 3, 0}, {0, 0, 3}};
	const auto mapped = physics::MapPortalMotion(through, {0, 1, 0}, body, source, destination);
	// Source mouth velocity is (0, 0, 0) at this point. Destination field
	// includes its angular contribution at the mapped point.
	CHECK(mapped.Linear.X == Catch::Approx(8));
	CHECK(mapped.Linear.Y == Catch::Approx(8));
	CHECK(mapped.Linear.Z == Catch::Approx(0));
	CHECK(mapped.Angular.X == Catch::Approx(0));
	CHECK(mapped.Angular.Y == Catch::Approx(1));
	CHECK(mapped.Angular.Z == Catch::Approx(2));
}
