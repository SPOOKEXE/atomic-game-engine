#include <engine/physics/PortalIsland.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

TEST_SUITE_ID("engine.physics.portalisland")

namespace {
	engine::physics::PortalIslandBody Body(uint64_t key, float velocity, bool available = true) {
		using namespace engine;
		return {
			{0, key, 1, 0}, {velocity, 0, 0}, {velocity, 0, 0}, {}, {0, 0, 0}, 1.0f, .5f, 0.0f, available
		};
	}
}

TEST_CASE("portal island solves opposing cross-world bodies once", "[physics][portal][island]") {
	using namespace engine;
	std::vector<physics::PortalIslandBody> bodies{Body(2, -2), Body(1, 2)};
	std::vector<physics::PortalIslandResult> result;
	const physics::PortalIslandContact contact{{0, 1, 1, 0}, {0, 2, 1, 0}, {0, 0, 0}, {1, 0, 0}};
	REQUIRE(physics::SolvePortalIsland(bodies, {contact}, result) == physics::PortalIslandStatus::Complete);
	REQUIRE(result.size() == 2);
	CHECK(result[0].Id.KeyLow == 1);
	CHECK(result[0].LinearVelocity.X == Catch::Approx(0));
	CHECK(result[1].LinearVelocity.X == Catch::Approx(0));
}

TEST_CASE(
	"portal island refuses an unavailable participant before a delayed impulse", "[physics][portal][island]"
) {
	using namespace engine;
	std::vector<physics::PortalIslandBody> bodies{Body(1, 2), Body(2, -2, false)};
	std::vector<physics::PortalIslandResult> result;
	const physics::PortalIslandContact contact{{0, 1, 1, 0}, {0, 2, 1, 0}, {}, {1, 0, 0}};
	CHECK(physics::SolvePortalIsland(bodies, {contact}, result) == physics::PortalIslandStatus::Unavailable);
	CHECK(result.empty());
}

TEST_CASE(
	"portal island maps uniform-scale inertia and impulse through the transpose", "[physics][portal][island]"
) {
	using namespace engine;
	CHECK(physics::MapPortalInverseInertia({4, 8, 12}, 2) == core::Vector3{1, 2, 3});
	CHECK(physics::MapPortalImpulseBack({1, 2, 3}, 2) == core::Vector3{2, 4, 6});
}

TEST_CASE(
	"portal island keeps copied shape inertia in its rotated principal axes", "[physics][portal][island]"
) {
	using namespace engine;
	physics::CopiedDynamicContact copied;
	copied.Identity.Key.Low = 7;
	copied.Identity.Generation = 1;
	copied.Frame = core::CFrame::Angles(0, 0, std::numbers::pi_v<float> * .5f);
	copied.Extent = {1, 2, 3};
	copied.Mass = 12;
	const physics::PortalIslandBody body = physics::MakePortalIslandBody(copied);
	CHECK(body.InverseInertia.X == Catch::Approx(1.0f / 52.0f));
	CHECK(body.InverseInertia.Y == Catch::Approx(1.0f / 40.0f));
	CHECK(body.InverseInertia.Z == Catch::Approx(1.0f / 20.0f));
	CHECK(body.PrincipalAxes[0].Y == Catch::Approx(1));
	CHECK(body.PrincipalAxes[1].X == Catch::Approx(-1));
}
