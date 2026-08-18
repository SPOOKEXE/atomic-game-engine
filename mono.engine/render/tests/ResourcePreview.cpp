#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ResourcePreview.hpp>

TEST_SUITE_ID("engine.render.resourcepreview")

using engine::render::ImageMode;
using engine::render::ResourcePreviewRoute;
using engine::render::ResourcePreviewSlots;

TEST_CASE("a preview route includes its pipeline resource and viewport", "[render][preview]") {
	const ResourcePreviewRoute wanted{
		engine::core::Name("Default PBR#7"), engine::core::Name("tonemapped"), 2
	};
	CHECK(
		wanted ==
		ResourcePreviewRoute{engine::core::Name("Default PBR#7"), engine::core::Name("tonemapped"), 2}
	);
	CHECK_FALSE(
		wanted == ResourcePreviewRoute{engine::core::Name("Thermal#7"), engine::core::Name("tonemapped"), 2}
	);
	CHECK_FALSE(
		wanted == ResourcePreviewRoute{engine::core::Name("Default PBR#7"), engine::core::Name("depth"), 2}
	);
	CHECK_FALSE(
		wanted ==
		ResourcePreviewRoute{engine::core::Name("Default PBR#7"), engine::core::Name("tonemapped"), 1}
	);
}

TEST_CASE("a retained preview is never rewritten while the interface reads it", "[render][preview]") {
	ResourcePreviewSlots slots;
	CHECK_FALSE(slots.Ready);
	CHECK(slots.Writable() == 0);

	slots.Publish(slots.Writable());
	REQUIRE(slots.Ready);
	CHECK(slots.Visible == 0);
	CHECK(slots.Writable() == 1);

	slots.Publish(slots.Writable());
	CHECK(slots.Visible == 1);
	CHECK(slots.Writable() == 0);

	slots.Reset();
	CHECK_FALSE(slots.Ready);
	CHECK(slots.Writable() == 0);
}

TEST_CASE("image preview uniforms distinguish channels from spectrum reversal", "[render][preview]") {
	const auto ordinary = ImageMode(false, false);
	CHECK(ordinary.SingleChannel == 0.0f);
	CHECK(ordinary.ReverseSpectrum == 0.0f);

	const auto reversedDepth = ImageMode(true, true);
	CHECK(reversedDepth.SingleChannel == 1.0f);
	CHECK(reversedDepth.ReverseSpectrum == 1.0f);
	CHECK(reversedDepth.Reserved[0] == 0.0f);
	CHECK(reversedDepth.Reserved[1] == 0.0f);
}
