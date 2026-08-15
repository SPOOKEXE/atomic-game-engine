#include <engine/core/types/UDim.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.core.types.udim")

using Catch::Approx;
using engine::core::UDim;
using engine::core::UDim2;
using engine::core::Vector2;

TEST_CASE("resolving mixes the proportion and the offset", "[udim]") {
	// "Half the parent, minus eight" - the case the type exists for, and the
	// one neither number alone can express.
	const UDim length{0.5f, -8.0f};

	REQUIRE(length.Resolve(200.0f) == Approx(92.0f));
	REQUIRE(length.Resolve(100.0f) == Approx(42.0f));

	// The offset survives a parent of zero, which is the half a pure scale
	// would have lost.
	REQUIRE(length.Resolve(0.0f) == Approx(-8.0f));
}

TEST_CASE("a pure offset ignores the parent", "[udim]") {
	const UDim fixed{0.0f, 24.0f};

	REQUIRE(fixed.Resolve(0.0f) == Approx(24.0f));
	REQUIRE(fixed.Resolve(1000.0f) == Approx(24.0f));
}

TEST_CASE("arithmetic adds both parts", "[udim]") {
	const UDim a{0.25f, 10.0f};
	const UDim b{0.5f, -4.0f};

	REQUIRE((a + b) == UDim{0.75f, 6.0f});
	REQUIRE((b - a) == UDim{0.25f, -14.0f});
	REQUIRE(-a == UDim{-0.25f, -10.0f});
}

TEST_CASE("a UDim2 resolves each axis against its own parent extent", "[udim]") {
	const UDim2 size{0.5f, -8.0f, 1.0f, 0.0f};
	const Vector2 resolved = size.Resolve(Vector2{200.0f, 60.0f});

	REQUIRE(resolved.X == Approx(92.0f));
	REQUIRE(resolved.Y == Approx(60.0f));
}

TEST_CASE("the four-number constructor uses Roblox's argument order", "[udim]") {
	// xScale, xOffset, yScale, yOffset - not xScale, yScale, xOffset, yOffset.
	// Getting this wrong produces a layout that is plausible and mirrored.
	const UDim2 size{0.5f, -8.0f, 0.25f, 4.0f};

	REQUIRE(size.X == UDim{0.5f, -8.0f});
	REQUIRE(size.Y == UDim{0.25f, 4.0f});
}

TEST_CASE("UDim2 arithmetic works on both axes", "[udim]") {
	const UDim2 a{0.5f, 10.0f, 0.25f, 4.0f};
	const UDim2 b{0.5f, -10.0f, 0.75f, 6.0f};

	REQUIRE((a + b) == UDim2{1.0f, 0.0f, 1.0f, 10.0f});
	REQUIRE((a - b) == UDim2{0.0f, 20.0f, -0.5f, -2.0f});
	REQUIRE(-a == UDim2{-0.5f, -10.0f, -0.25f, -4.0f});
}

TEST_CASE("UDim2 lerp interpolates scale and offset independently", "[udim]") {
	const UDim2 from{0.0f, 0.0f, 0.0f, 0.0f};
	const UDim2 to{1.0f, 100.0f, 0.5f, -20.0f};

	REQUIRE(from.Lerp(to, 0.0f) == from);
	REQUIRE(from.Lerp(to, 1.0f) == to);
	REQUIRE(from.Lerp(to, 0.5f) == UDim2{0.5f, 50.0f, 0.25f, -10.0f});
}

TEST_CASE("a default UDim2 is zero everywhere", "[udim]") {
	REQUIRE(UDim2{}.Resolve(Vector2{500.0f, 500.0f}) == Vector2::Zero);
}
