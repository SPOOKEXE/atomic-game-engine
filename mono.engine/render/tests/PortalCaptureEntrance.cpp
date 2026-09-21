#include "PortalCaptureEntrance.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.portalcaptureentrance")

TEST_CASE("incoming captures skip only one reciprocal aperture", "[render][portal-entrance]") {
	using namespace engine;
	const render::PortalImageEntrance entrance{"source", {3, 4, 5}, {2, 0, 0}, {0, 3, 0}};
	scene::PortalSeam seam;
	seam.Crosses = true;
	seam.DestinationWorld = core::Name("source");
	seam.Centre = {3, 4, 5};
	seam.First = {-2, 0, 0};
	seam.Second = {0, 3, 0};
	seam.Surface = 2;
	CHECK(render::ResolvePortalEntrance(entrance, std::span(&seam, 1)) == 2);
	std::swap(seam.First, seam.Second);
	CHECK(render::ResolvePortalEntrance(entrance, std::span(&seam, 1)) == 2);
	const auto matching = seam;
	seam.Centre.Y += .00014f;
	seam.Centre.Z += .00023f;
	CHECK(render::ResolvePortalEntrance(entrance, std::span(&seam, 1)) == 2);
	const std::array nearby{seam, matching};
	CHECK_FALSE(render::ResolvePortalEntrance(entrance, nearby));
	for (int difference = 0; difference < 5; ++difference) {
		seam = matching;
		if (difference == 0) seam.Centre.Z += 1;
		if (difference == 1) seam.First = seam.First * 2;
		if (difference == 2) seam.DestinationWorld = core::Name("elsewhere");
		if (difference == 3) seam.Crosses = false;
		if (difference == 4) seam.Surface = -1;
		CHECK_FALSE(render::ResolvePortalEntrance(entrance, std::span(&seam, 1)));
		const std::array seams{seam, matching};
		CHECK(render::ResolvePortalEntrance(entrance, seams) == 2);
	}
	const std::array ambiguous{matching, matching};
	CHECK_FALSE(render::ResolvePortalEntrance(entrance, ambiguous));
}
