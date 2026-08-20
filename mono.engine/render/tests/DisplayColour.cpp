// The display-to-working conversion used before HDR fog blending.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <DisplayColour.hpp>

TEST_SUITE_ID("engine.render.displaycolour")

using Catch::Approx;
using engine::render::WorkingFromDisplay;

TEST_CASE("display endpoints and the default fog colour survive tone mapping", "[render][colour]") {
	CHECK(WorkingFromDisplay(-1.0f) == Approx(0.0f));
	CHECK(WorkingFromDisplay(0.0f) == Approx(0.0f));
	CHECK(WorkingFromDisplay(0.5f) == Approx(0.151313f));
	CHECK(WorkingFromDisplay(0.753f) == Approx(0.394253f));
	CHECK(WorkingFromDisplay(1.0f) == Approx(7.241657f));
	CHECK(WorkingFromDisplay(2.0f) == Approx(7.241657f));
}
