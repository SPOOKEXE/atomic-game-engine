#include "../src/ValueOps.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.imagegraph.value_ops")

namespace ops = engine::imagegraph::detail;
using engine::imagegraph::Colour;
using engine::imagegraph::Vector2;

TEST_CASE("Math operations follow source mode order and guard undefined values", "[imagegraph]") {
	const auto evaluate = [](int64_t mode, double a = 4.0, double b = 2.0, double amount = 0.5) {
		return ops::Math(a, b, amount, {0.0, 1.0}, {10.0, 20.0}, mode, false);
	};
	const std::array<double, 22> expected{
		6.0,
		2.0,
		8.0,
		2.0,
		16.0,
		2.0,
		std::sin(4.0) * 2.0,
		std::cos(4.0) * 2.0,
		std::tan(4.0) * 2.0,
		0.0,
		4.0,
		4.0,
		4.0,
		3.0,
		4.0,
		2.0,
		4.0,
		0.0,
		50.0,
		2.0,
		4.0,
		2.0
	};
	for (int64_t mode = 0; mode < static_cast<int64_t>(expected.size()); mode++) {
		const auto result = evaluate(mode);
		REQUIRE(result.has_value());
		CHECK(*result == Catch::Approx(expected[static_cast<size_t>(mode)]));
	}
	CHECK(evaluate(3, 4.0, 0.0) == 0.0);
	CHECK(evaluate(5, 4.0, 0.0) == 0.0);
	CHECK(evaluate(9, -5.0, 2.0) == -1.0);
	CHECK(evaluate(17, -1.25, 0.0) == 0.75);
	CHECK(ops::Math(90.0, 1.0, 0.0, {}, {}, 6, true) == Catch::Approx(1.0));
	CHECK_FALSE(evaluate(22).has_value());
	CHECK_FALSE(ops::Math(1.0, 2.0, 0.0, {1.0, 1.0}, {}, 18, false).has_value());
	CHECK_FALSE(evaluate(19, -1.0, 2.0).has_value());
}

TEST_CASE("Math scalar arithmetic and vector range edges follow pinned source", "[imagegraph]") {
	CHECK(ops::Math(30.0, 90.0, 0.0, {}, {}, 1, true) == -60.0);
	CHECK(ops::Math(30.0, 90.0, 0.0, {}, {}, 2, true) == 2700.0);
	CHECK(ops::Math(30.0, 0.0, 0.0, {}, {}, 3, true) == 0.0);
	CHECK(ops::Math(30.0, 90.0, 0.25, {}, {}, 13, true) == 45.0);
	CHECK(ops::Math(30.0, 0.0, 0.0, {10.0, 50.0}, {-2.0, 2.0}, 18, true) == 0.0);
	CHECK(ops::Math(30.0, 0.0, 0.0, {10.0, 20.0}, {0.0, 1.0}, 18, true) == 2.0);
	CHECK_FALSE(ops::Math(30.0, 0.0, 0.0, {1.0, 1.0}, {0.0, 1.0}, 18, true));
}

TEST_CASE("Compare and logic modes match the source enums", "[imagegraph]") {
	CHECK(ops::Compare(2.0, 2.0, 0) == true);
	CHECK(ops::Compare(2.0, 3.0, 1) == true);
	CHECK(ops::Compare(3.0, 2.0, 2) == true);
	CHECK(ops::Compare(3.0, 3.0, 3) == true);
	CHECK(ops::Compare(2.0, 3.0, 4) == true);
	CHECK(ops::Compare(3.0, 3.0, 5) == true);
	CHECK_FALSE(ops::Compare(1.0, 2.0, 6).has_value());
	CHECK(ops::Logic(true, false, 0) == false);
	CHECK(ops::Logic(true, false, 1) == true);
	CHECK(ops::Logic(true, false, 2) == false);
	CHECK(ops::Logic(true, false, 3) == true);
	CHECK(ops::Logic(true, false, 4) == false);
	CHECK(ops::Logic(true, false, 5) == true);
	CHECK_FALSE(ops::Logic(true, false, 6).has_value());
}

TEST_CASE("RGB creation, data extraction and mixing preserve alpha", "[imagegraph]") {
	CHECK(ops::MakeRgbColour(1.0, 0.5, 0.0, 0.25, true) == Colour{255, 128, 0, 64});
	CHECK(ops::MakeRgbColour(260.0, -3.0, 42.0, 255.0, false) == Colour{255, 0, 42, 255});
	CHECK(ops::MixRgbColour({0, 20, 40, 128}, {100, 40, 0, 255}, 0.5) == Colour{50, 30, 20, 192});
	const auto red = ops::ColorData({255, 0, 0, 128}, true);
	CHECK(red[0] == 1.0);
	CHECK(red[3] == 0.0);
	CHECK(red[4] == 1.0);
	CHECK(red[5] == 1.0);
	CHECK(red[6] == Catch::Approx(std::sqrt(0.241)));
	CHECK(red[7] == Catch::Approx(128.0 / 255.0));
	const auto raw = ops::ColorData({0, 255, 0, 255}, false);
	CHECK(raw[1] == 255.0);
	CHECK(raw[3] == Catch::Approx(255.0 / 3.0));
	CHECK(raw[5] == 255.0);
	CHECK(ops::MakeHsvColour(0.0, 255.0, 255.0, 128.0) == Colour{255, 0, 0, 128});
	CHECK(ops::MakeHsvColour(85.0, 255.0, 255.0, 255.0) == Colour{0, 255, 0, 255});
	CHECK(ops::MixHsvColour({255, 0, 0, 0}, {0, 255, 0, 255}, 0.5).Alpha == 128);
}

TEST_CASE("Vector operations use Y-down direction and zero-safe normalization", "[imagegraph]") {
	CHECK(ops::VectorMagnitude({3.0, 4.0}) == 5.0);
	const Vector2 unit = ops::Normalize({3.0, 4.0});
	CHECK(unit.X == Catch::Approx(0.6));
	CHECK(unit.Y == Catch::Approx(0.8));
	CHECK(ops::Normalize({}) == Vector2{});
	CHECK(ops::Dot({2.0, 3.0}, {4.0, 5.0}) == 23.0);
	CHECK(ops::Cross({1.0, 0.0}, {0.0, 1.0}) == 1.0);
	CHECK(ops::VectorDirection({0.0, -1.0}, false) == Catch::Approx(90.0));
	CHECK(ops::VectorDirection({0.0, 1.0}, true) == Catch::Approx(3.0 * std::acos(-1.0) / 2.0));
}
