#include <engine/core/types/CFrame.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

TEST_SUITE_ID("engine.core.types.obb")
TEST_DEPENDS("engine.core.types.vector3")
TEST_DEPENDS("engine.core.types.aabb")
TEST_DEPENDS("engine.core.types.cframe")

using Catch::Approx;
using engine::core::AABB;
using engine::core::CFrame;
using engine::core::OrientedBoxBounds;
using engine::core::Vector3;

TEST_CASE("an oriented box rotates its half extents into a world bound", "[obb]") {
	constexpr float quarter = std::numbers::pi_v<float> / 4.0f;
	const Vector3 halfExtent{1.0f, 2.0f, 3.0f};
	const CFrame frame = CFrame(Vector3{4.0f, -2.0f, 7.0f}) * CFrame::Angles(0.0f, quarter, 0.0f);

	const AABB bound = OrientedBoxBounds(frame, halfExtent);
	const float root2 = std::numbers::sqrt2_v<float>;
	REQUIRE(bound.Centre() == frame.Position);
	CHECK(bound.Size().X == Approx(4.0f * root2).margin(1e-5));
	CHECK(bound.Size().Y == Approx(4.0f).margin(1e-5));
	CHECK(bound.Size().Z == Approx(4.0f * root2).margin(1e-5));
}

TEST_CASE("an oriented bound contains every transformed box corner", "[obb]") {
	const CFrame frame = CFrame(Vector3{2.0f, -1.0f, 4.0f}) * CFrame::Angles(0.4f, 1.2f, -0.3f);
	const Vector3 halfExtent{0.5f, 1.5f, 0.25f};
	const AABB bound = OrientedBoxBounds(frame, halfExtent);
	const AABB withSlack =
		AABB::FromCentre(bound.Centre(), bound.Size() * 0.5f + Vector3{1e-4f, 1e-4f, 1e-4f});

	for (int corner = 0; corner < 8; corner++) {
		const Vector3 local{
			(corner & 1) ? halfExtent.X : -halfExtent.X,
			(corner & 2) ? halfExtent.Y : -halfExtent.Y,
			(corner & 4) ? halfExtent.Z : -halfExtent.Z,
		};
		REQUIRE(withSlack.Contains(frame.PointToWorldSpace(local)));
	}
}
