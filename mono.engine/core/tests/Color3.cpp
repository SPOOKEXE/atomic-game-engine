#include <engine/core/types/Color3.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.color3")

using Catch::Approx;
using engine::core::Color3;

TEST_CASE("FromRGB converts sRGB to linear", "[color3]") {
	// Mid grey is around 0.216 linear, not 0.5. Getting this wrong is why
	// lighting looks washed out, and it looks plausible enough to ship.
	REQUIRE(Color3::FromRGB(128, 128, 128).R == Approx(0.2158f).margin(0.001f));

	REQUIRE(Color3::FromRGB(0, 0, 0).R == Approx(0.0f));
	REQUIRE(Color3::FromRGB(255, 255, 255).R == Approx(1.0f));
}

TEST_CASE("the conversion is the piecewise curve, not a 2.2 power", "[color3]") {
	// Below the knee sRGB is linear, at channel/12.92. A pure power
	// approximation is visibly wrong exactly here, which is where ambient
	// terms live.
	REQUIRE(Color3::FromRGB(10, 10, 10).R == Approx((10.0f / 255.0f) / 12.92f).margin(1e-5));

	// And above it, the exponential branch.
	REQUIRE(Color3::FromRGB(200, 200, 200).R == Approx(0.5776f).margin(0.001f));
}

TEST_CASE("FromLinear does not convert", "[color3]") {
	// The escape hatch for a value already in linear space. Running it through
	// the transfer function twice is the other half of the washed-out bug.
	const Color3 raw = Color3::FromLinear(0.5f, 0.25f, 0.125f);

	REQUIRE(raw.R == Approx(0.5f));
	REQUIRE(raw.G == Approx(0.25f));
	REQUIRE(raw.B == Approx(0.125f));
}

TEST_CASE("channels convert independently", "[color3]") {
	const Color3 mixed = Color3::FromRGB(0, 128, 255);

	REQUIRE(mixed.R == Approx(0.0f));
	REQUIRE(mixed.G == Approx(0.2158f).margin(0.001f));
	REQUIRE(mixed.B == Approx(1.0f));
}

TEST_CASE("arithmetic behaves", "[color3]") {
	const Color3 a = Color3::FromLinear(0.2f, 0.4f, 0.6f);
	const Color3 b = Color3::FromLinear(0.1f, 0.1f, 0.1f);

	REQUIRE((a + b).R == Approx(0.3f));
	REQUIRE((a * 2.0f).G == Approx(0.8f));
	// Component-wise, which is what tinting is.
	REQUIRE((a * b).B == Approx(0.06f));
}

TEST_CASE("lerp hits both ends and the middle", "[color3]") {
	const Color3 black = Color3::FromLinear(0.0f, 0.0f, 0.0f);
	const Color3 white = Color3::FromLinear(1.0f, 1.0f, 1.0f);

	REQUIRE(black.Lerp(white, 0.0f) == black);
	REQUIRE(black.Lerp(white, 1.0f) == white);
	REQUIRE(black.Lerp(white, 0.5f).R == Approx(0.5f));
}
