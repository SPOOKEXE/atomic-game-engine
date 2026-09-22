#include <engine/render/InterfacePass.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.render.interfacepass")

namespace {
	using namespace engine;
	using namespace engine::render;
}

TEST_CASE("canvas group target bounds are clipped before device sizing", "[render][interfacepass]") {
	const auto target = InterfaceGroupTargetFor(
		{{20.0f, -10.0f}, {180.0f, 70.0f}},
		{{40.0f, 10.0f}, {300.0f, 90.0f}},
		{100.0f, 80.0f},
		{200.0f, 160.0f}
	);
	REQUIRE(target.has_value());
	CHECK(target->Bounds.Min.X == 40.0f);
	CHECK(target->Bounds.Min.Y == 10.0f);
	CHECK(target->Bounds.Max.X == 100.0f);
	CHECK(target->Bounds.Max.Y == 70.0f);
	CHECK(target->Width == 120);
	CHECK(target->Height == 120);
}

TEST_CASE("canvas group target sizing refuses hostile coordinates", "[render][interfacepass]") {
	const core::Rect canvas{{0.0f, 0.0f}, {100.0f, 80.0f}};
	const core::Vector2 targetPixels{200.0f, 160.0f};
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{std::numeric_limits<float>::quiet_NaN(), 0.0f}, {10.0f, 10.0f}}, canvas, canvas.Max, targetPixels
	));
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{0.0f, 0.0f}, {std::numeric_limits<float>::infinity(), 10.0f}}, canvas, canvas.Max, targetPixels
	));
	CHECK_FALSE(
		InterfaceGroupTargetFor({{-20.0f, -20.0f}, {-1.0f, -1.0f}}, canvas, canvas.Max, targetPixels)
	);
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{0.0f, 0.0f}, {10.0f, 10.0f}}, canvas, canvas.Max, {std::numeric_limits<float>::max(), 160.0f}
	));
	CHECK_FALSE(InterfaceGroupTargetFor(
		{{0.0f, 0.0f}, {100.0f, 80.0f}},
		canvas,
		canvas.Max,
		{static_cast<float>(MAXIMUM_INTERFACE_GROUP_TARGET_EDGE + 1),
		 static_cast<float>(MAXIMUM_INTERFACE_GROUP_TARGET_EDGE + 1)}
	));

	const auto clamped =
		InterfaceGroupTargetFor({{-1.0e30f, -1.0e30f}, {1.0e30f, 1.0e30f}}, canvas, canvas.Max, targetPixels);
	REQUIRE(clamped.has_value());
	CHECK(clamped->Bounds == canvas);
	CHECK(clamped->Width == 200);
	CHECK(clamped->Height == 160);
}
