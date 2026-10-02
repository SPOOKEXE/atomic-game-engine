#include "../src/nodes/PixelBuilderEffects.hpp"
#include "../src/nodes/PixelBuilderPrimitives.hpp"
#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_2d_nodes")

using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE("Cartesian and area UV maps preserve texel centers", "[imagegraph][source_2d]") {
	for (const auto type : {"pc.uv_cartesian", "pc.uv_area"}) {
		const auto run = RunNode(type, {}, {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}});
		REQUIRE(run.Ok);
		CHECK(
			run.Output().Pixels ==
			std::vector<uint8_t>{64, 191, 0, 255, 191, 191, 0, 255, 64, 64, 0, 255, 191, 64, 0, 255}
		);
	}
}

TEST_CASE(
	"Cartesian UV repeat modes retain empty tile clamp and ping pong behavior", "[imagegraph][source_2d]"
) {
	const std::array<std::vector<uint8_t>, 4> expected{
		std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0},
		std::vector<uint8_t>{64, 128, 0, 255, 191, 128, 0, 255},
		std::vector<uint8_t>{0, 128, 0, 255, 0, 128, 0, 255},
		std::vector<uint8_t>{191, 128, 0, 255, 64, 128, 0, 255},
	};
	for (int64_t mode = 0; mode < 4; ++mode) {
		const auto run = RunNode(
			"pc.uv_cartesian",
			{},
			{{"dimension", Vector2{2, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{1, 0}},
			 {"repeat", EnumValue{mode}}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == expected[size_t(mode)]);
	}
}

TEST_CASE("UV map mix reverses green coordinates and retains map alpha", "[imagegraph][source_2d]") {
	const Image map{1, 1, {204, 51, 0, 64}, 0};
	const auto run = RunNode(
		"pc.uv_cartesian",
		{{"uv_map", &map}},
		{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"uv_mix", 0.5}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{166, 89, 0, 64});
}

TEST_CASE(
	"Polar UV evaluates angular and radial coordinates with channel inversion", "[imagegraph][source_2d]"
) {
	const auto run =
		RunNode("pc.uv_polar", {}, {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}});
	REQUIRE(run.Ok);
	CHECK(
		run.Output().Pixels ==
		std::vector<uint8_t>{96, 128, 0, 255, 32, 128, 0, 255, 159, 128, 0, 255, 223, 128, 0, 255}
	);
	const auto inverted = RunNode(
		"pc.uv_polar", {}, {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}, {"invert", true}}
	);
	REQUIRE(inverted.Ok);
	for (size_t pixel = 0; pixel < 4; ++pixel) {
		CHECK(inverted.Output().Pixels[pixel * 4] == run.Output().Pixels[pixel * 4 + 1]);
		CHECK(inverted.Output().Pixels[pixel * 4 + 1] == run.Output().Pixels[pixel * 4]);
	}
}

TEST_CASE("Isometric axes distinguish top left and right", "[imagegraph][source_2d]") {
	const std::array<std::vector<uint8_t>, 3> expected{
		std::vector<uint8_t>{128, 128, 0, 255},
		std::vector<uint8_t>{128, 128, 0, 255},
		std::vector<uint8_t>{128, 128, 0, 255},
	};
	for (int64_t side = 0; side < 3; ++side) {
		const auto run = RunNode(
			"pc.uv_isometric",
			{},
			{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"direction", EnumValue{side}}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == expected[size_t(side)]);
		const auto roundtrip = RunNode(
			"pc.uv_isometric",
			{},
			{{"dimension", Vector2{2, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"direction", EnumValue{side}},
			 {"inverted", true}}
		);
		REQUIRE(roundtrip.Ok);
		CHECK(
			roundtrip.Output().Pixels !=
			RunNode("pc.uv_cartesian", {}, {{"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}})
				.Output()
				.Pixels
		);
	}
}

TEST_CASE("Perspective UV direction selects depth axis", "[imagegraph][source_2d]") {
	for (int64_t side = 0; side < 4; ++side) {
		const auto run = RunNode(
			"pc.uv_perspective",
			{},
			{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}, {"direction", EnumValue{side}}}
		);
		REQUIRE(run.Ok);
		CHECK(
			run.Output().Pixels == (side == 0 || side == 2 ? std::vector<uint8_t>{128, 85, 0, 255}
														   : std::vector<uint8_t>{170, 128, 0, 255})
		);
	}
}

TEST_CASE("Height UV uses wrapped red-channel central differences", "[imagegraph][source_2d]") {
	const Image height{3, 1, {0, 0, 0, 255, 128, 0, 0, 64, 255, 0, 0, 255}, 0};
	const auto run = RunNode(
		"pc.uv_height",
		{{"height_map", &height}},
		{{"dimension", Vector2{3, 1}}, {"dimension_unit", EnumValue{0}}, {"strength", 0.25}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{11, 128, 0, 255, 191, 128, 0, 64, 181, 128, 0, 255});
	CHECK_FALSE(RunNode("pc.uv_height", {}).Ok);
}

TEST_CASE("UV generator mask uses mean RGB times alpha and mask dimensions", "[imagegraph][source_2d]") {
	const Image mask{1, 1, {255, 0, 0, 128}, 0};
	const auto run = RunNode("pc.uv_cartesian", {{"mask", &mask}}, {{"dimension_unit", EnumValue{2}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 128, 0, 43});
	CHECK_FALSE(RunNode("pc.uv_cartesian", {}, {{"dimension_unit", EnumValue{2}}}).Ok);
}

TEST_CASE("UV blend reads red mask without affecting opaque output alpha", "[imagegraph][source_2d]") {
	const Image background{1, 1, {0, 255, 0, 0}, 0}, foreground{1, 1, {255, 0, 255, 0}, 0},
		mask{1, 1, {128, 0, 0, 0}, 0};
	const auto run = RunNode(
		"pc.uv_blend", {{"uv_bg", &background}, {"uv_fg", &foreground}, {"mask", &mask}}, {{"amount", 1.0}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 127, 0, 255});
	CHECK_FALSE(RunNode("pc.uv_blend", {{"uv_bg", &background}}).Ok);
	CHECK_FALSE(RunNode("pc.uv_blend", {{"uv_fg", &foreground}}).Ok);
}

TEST_CASE(
	"UV generators reject invalid choices singular projections and excess dimensions",
	"[imagegraph][source_2d]"
) {
	CHECK_FALSE(RunNode("pc.uv_cartesian", {}, {{"repeat", 0.5}}).Ok);
	CHECK_FALSE(RunNode("pc.uv_isometric", {}, {{"direction", 0.5}}).Ok);
	CHECK_FALSE(RunNode("pc.uv_perspective", {}, {{"direction", 0.5}}).Ok);
	CHECK_FALSE(RunNode("pc.uv_area", {}, {{"area", Area{0.5, 0.5, 0, 0}}}).Ok);
	CHECK_FALSE(RunNode(
					"pc.uv_perspective",
					{},
					{{"distance", -2.0}, {"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}
	).Ok);
	CHECK(
		RunNode(
			"pc.uv_cartesian",
			{},
			{{"dimension", Vector2{double(Limits::MaximumDimension + 1), 1}},
			 {"dimension_unit", EnumValue{0}}}
		).Code == Status::LimitExceeded
	);
}

TEST_CASE(
	"Color Remove matches RGB and LAB palette distances with inverted selection", "[imagegraph][source_2d]"
) {
	const Image image{2, 1, {0, 0, 0, 128, 255, 0, 0, 64}, 0};
	for (int64_t space = 0; space < 2; ++space) {
		const auto remove = RunNode(
			"pc.color_remove",
			{{"surface_in", &image}},
			{{"color_space", EnumValue{space}}, {"threshold", 0.01}}
		);
		REQUIRE(remove.Ok);
		CHECK(remove.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0, 255, 0, 0, 64});
		const auto inverted = RunNode(
			"pc.color_remove",
			{{"surface_in", &image}},
			{{"color_space", EnumValue{space}}, {"threshold", 0.01}, {"invert", true}}
		);
		REQUIRE(inverted.Ok);
		CHECK(inverted.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 128, 0, 0, 0, 0});
	}
	CHECK(
		RunNode("pc.color_remove", {{"surface_in", &image}}, {{"color_space", EnumValue{2}}})
			.Output()
			.Pixels ==
		RunNode("pc.color_remove", {{"surface_in", &image}}, {{"color_space", EnumValue{1}}}).Output().Pixels
	);
}

TEST_CASE(
	"Normal Adjust rotates around half red and green while preserving alpha", "[imagegraph][source_2d]"
) {
	const Image image{1, 1, {255, 128, 128, 64}, 0};
	const auto run =
		RunNode("pc.normal_adjust", {{"normal", &image}}, {{"rotate", 90.0}, {"intensity", 0.5}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{127, 191, 64, 64});
	const Image mask{1, 1, {0, 0, 0, 255}, 0};
	const auto masked =
		RunNode("pc.normal_adjust", {{"normal", &image}, {"mask", &mask}}, {{"rotate", 90.0}});
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Pixels == image.Pixels);
	const auto normalized = RunNode("pc.normal_adjust", {{"normal", &image}}, {{"normalize", true}});
	REQUIRE(normalized.Ok);
	const auto sample = engine::imagegraph::detail::ReadPixel(normalized.Output(), 0, 0);
	CHECK(std::hypot(sample[0], sample[1], sample[2]) == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE("High Pass retains source alpha while applying weighted edge response", "[imagegraph][source_2d]") {
	const Image image{3, 1, {0, 0, 0, 128, 128, 128, 128, 64, 255, 255, 255, 32}, 0};
	const auto run = RunNode(
		"pc.high_pass", {{"surface_in", &image}}, {{"blend_original", false}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 128, 0, 0, 0, 64, 32, 32, 32, 32});
	const auto inactive = RunNode("pc.high_pass", {{"surface_in", &image}}, {{"active", false}});
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Pixels == image.Pixels);
	CHECK(
		RunNode("pc.high_pass", {{"surface_in", &image}}, {{"radius", int64_t{1000000}}}).Code ==
		Status::LimitExceeded
	);
}

TEST_CASE(
	"Scale handles nearest duplication and aspect fit without changing source alpha",
	"[imagegraph][source_2d]"
) {
	const Image image{2, 1, {255, 0, 0, 128, 0, 0, 255, 64}, 0};
	const auto scaled = RunNode("pc.scale", {{"surface_in", &image}}, {{"scale", 2.0}});
	REQUIRE(scaled.Ok);
	CHECK(scaled.Output().Width == 4);
	CHECK(scaled.Output().Height == 2);
	CHECK(scaled.Output().Pixels == std::vector<uint8_t>{255, 0,   0, 128, 255, 0,	 0, 128, 0,	  0,   255,
														 64,  0,   0, 255, 64,	255, 0, 0,	 128, 255, 0,
														 0,	  128, 0, 0,   255, 64,	 0, 0,	 255, 64});
	for (int64_t fit = 0; fit < 3; ++fit) {
		const auto sized = RunNode(
			"pc.scale",
			{{"surface_in", &image}},
			{{"mode", EnumValue{1}}, {"target_dimension", Vector2{4, 4}}, {"fit_mode", EnumValue{fit}}}
		);
		REQUIRE(sized.Ok);
		CHECK(sized.Output().Width == (fit == 2 ? 8 : 4));
		CHECK(sized.Output().Height == (fit == 1 ? 2 : 4));
	}
	CHECK(
		RunNode("pc.scale", {{"surface_in", &image}}, {{"scale", double(Limits::MaximumDimension)}}).Code ==
		Status::LimitExceeded
	);
	CHECK(RunNode("pc.scale", {{"surface_in", &image}}, {{"mode", EnumValue{2}}}).Output().Width == 32);
}

TEST_CASE(
	"Crop uses left offset at source index two and transparent exterior pixels", "[imagegraph][source_2d]"
) {
	const Image image{3, 1, {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120}, 0};
	const auto crop = RunNode(
		"pc.crop", {{"surface_in", &image}}, {{"crop", Vector4{0, 0, 1, 0}}, {"crop_unit", EnumValue{0}}}
	);
	REQUIRE(crop.Ok);
	CHECK(crop.Output().Pixels == std::vector<uint8_t>{50, 60, 70, 80, 90, 100, 110, 120});
	const auto extended = RunNode(
		"pc.crop", {{"surface_in", &image}}, {{"crop", Vector4{0, 0, -1, 0}}, {"crop_unit", EnumValue{0}}}
	);
	REQUIRE(extended.Ok);
	CHECK(
		extended.Output().Pixels ==
		std::vector<uint8_t>{0, 0, 0, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120}
	);
	const auto aspect =
		RunNode("pc.crop", {{"surface_in", &image}}, {{"aspect_ratio", EnumValue{3}}, {"width", 3.0}});
	REQUIRE(aspect.Ok);
	CHECK(aspect.Output().Width == 3);
	CHECK(aspect.Output().Height == 2);
	CHECK_FALSE(
		RunNode(
			"pc.crop", {{"surface_in", &image}}, {{"aspect_ratio", EnumValue{1}}, {"ratio", Vector2{1, 0}}}
		).Ok
	);
	CHECK(
		RunNode("pc.crop", {{"surface_in", &image}}, {{"fit_mode", EnumValue{4}}}).Output().Pixels ==
		image.Pixels
	);
}

TEST_CASE("Multiply Alpha composites attenuation and clears inclusive threshold", "[imagegraph][source_2d]") {
	const Image image{2, 1, {255, 100, 0, 128, 255, 100, 0, 0}, 0};
	const auto run =
		RunNode("pc.multiply_alpha", {{"surface_in", &image}}, {{"bg_color", Colour{0, 255, 255, 0}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 100, 0, 255, 0, 0, 0, 0});
	const auto threshold =
		RunNode("pc.multiply_alpha", {{"surface_in", &image}}, {{"threshold", 128.0 / 255.0}});
	REQUIRE(threshold.Ok);
	CHECK(threshold.Output().Pixels == std::vector<uint8_t>(8, 0));
}

TEST_CASE("Color Blind modes retain pinned shader matrix column order and alpha", "[imagegraph][source_2d]") {
	const Image image{1, 1, {255, 0, 0, 128}, 0};
	const std::array<std::vector<uint8_t>, 9> expected{
		std::vector<uint8_t>{255, 0, 0, 128},
		{39, 255, 0, 128},
		{208, 85, 0, 128},
		{94, 220, 0, 128},
		{204, 51, 0, 128},
		{255, 0, 0, 128},
		{247, 8, 0, 128},
		{76, 76, 76, 128},
		{158, 82, 16, 128}
	};
	for (int64_t mode = 0; mode < 9; ++mode) {
		const auto run = RunNode("pc.color_blind", {{"surface_in", &image}}, {{"type", EnumValue{mode}}});
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == expected[size_t(mode)]);
	}
	CHECK(
		RunNode("pc.color_blind", {{"surface_in", &image}}, {{"type", EnumValue{9}}}).Output().Pixels ==
		expected[8]
	);
}

TEST_CASE("RGBA extract keeps source channel alpha rules for both display modes", "[imagegraph][source_2d]") {
	const Image image{1, 1, {20, 40, 60, 80}, 0};
	const auto plain = RunNode("pc.rgb_channel", {{"surface_in", &image}});
	REQUIRE(plain.Ok);
	CHECK(plain.Output("red").Pixels == std::vector<uint8_t>{20, 0, 0, 255});
	CHECK(plain.Output("green").Pixels == std::vector<uint8_t>{0, 40, 0, 255});
	CHECK(plain.Output("blue").Pixels == std::vector<uint8_t>{0, 0, 60, 255});
	CHECK(plain.Output("alpha").Pixels == std::vector<uint8_t>{255, 255, 255, 80});
	const auto grey = RunNode(
		"pc.rgb_channel", {{"surface_in", &image}}, {{"output_type", EnumValue{1}}, {"keep_alpha", true}}
	);
	REQUIRE(grey.Ok);
	CHECK(grey.Output("red").Pixels == std::vector<uint8_t>{20, 20, 20, 80});
	CHECK(grey.Output("alpha").Pixels == std::vector<uint8_t>{80, 80, 80, 255});
}

TEST_CASE("HSV extraction changes only its third channel for HSL mode", "[imagegraph][source_2d]") {
	const Image image{1, 1, {255, 0, 0, 64}, 0};
	const auto hsv = RunNode("pc.hsv_channel", {{"surface_in", &image}});
	REQUIRE(hsv.Ok);
	CHECK(hsv.Output("hue").Pixels == std::vector<uint8_t>{0, 0, 0, 64});
	CHECK(hsv.Output("saturation").Pixels == std::vector<uint8_t>{255, 255, 255, 64});
	CHECK(hsv.Output("value").Pixels == std::vector<uint8_t>{255, 255, 255, 64});
	CHECK(hsv.Output("alpha").Pixels == std::vector<uint8_t>{255, 255, 255, 64});
	const auto hsl = RunNode("pc.hsv_channel", {{"surface_in", &image}}, {{"color_space", EnumValue{1}}});
	REQUIRE(hsl.Ok);
	CHECK(hsl.Output("value").Pixels == std::vector<uint8_t>{128, 128, 128, 64});
}

TEST_CASE(
	"Channel extraction array mode publishes four ordered complete surfaces", "[imagegraph][source_2d]"
) {
	for (const auto type : {"pc.rgb_channel", "pc.hsv_channel"}) {
		const CatalogueEntry *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		const auto executor = engine::imagegraph::detail::FindExecutor(type);
		REQUIRE(executor);
		Node node{"node", type, "", {}, {}};
		EvaluationRequest request;
		engine::imagegraph::detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		const Image source{1, 1, {255, 0, 0, 128}, 0};
		context.Images.emplace_back("surface_in", &source);
		context.Values.emplace_back("output_array", true);
		REQUIRE(executor(context));
		REQUIRE(context.OutputImages.empty());
		REQUIRE(context.OutputImageArrays.size() == 1);
		const auto &channels = context.OutputImageArrays[0].second;
		REQUIRE(channels.Images.size() == 4);
		REQUIRE(channels.Items.size() == 4);
		for (size_t index = 0; index < 4; ++index)
			CHECK(std::get<size_t>(channels.Items[index].Data) == index);
		CHECK(channels.Images[3].Pixels == std::vector<uint8_t>{255, 255, 255, 128});
	}
}

TEST_CASE("Curvature evaluates axial and diagonal second differences", "[imagegraph][source_2d]") {
	const Image flat{1, 1, {128, 128, 128, 64}, 0};
	const auto plane = RunNode("pc.curvature", {{"surface_in", &flat}});
	REQUIRE(plane.Ok);
	CHECK(plane.Output().Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	const Image spike{3, 1, {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255}, 0};
	const auto convex = RunNode(
		"pc.curvature",
		{{"surface_in", &spike}},
		{{"radius", 1.0}, {"intensity", 1.0}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(convex.Ok);
	CHECK(
		convex.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255}
	);
	const auto absolute = RunNode(
		"pc.curvature",
		{{"surface_in", &spike}},
		{{"radius", 1.0}, {"intensity", 1.0}, {"absolute", true}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(absolute.Ok);
	CHECK(absolute.Output().Pixels == std::vector<uint8_t>(12, 255));
	CHECK_FALSE(RunNode("pc.curvature", {{"surface_in", &flat}}, {{"radius", 0.0}}).Ok);
}

TEST_CASE("Emboss samples directional height while retaining original alpha", "[imagegraph][source_2d]") {
	const Image flat{1, 1, {128, 128, 128, 64}, 0};
	const auto unchanged = RunNode("pc.emboss", {{"surface_in", &flat}}, {{"high_res", true}});
	REQUIRE(unchanged.Ok);
	CHECK(unchanged.Output().Pixels == flat.Pixels);
	const Image edge{2, 1, {128, 128, 128, 255, 0, 0, 0, 255}, 0};
	const auto bright =
		RunNode("pc.emboss", {{"surface_in", &edge}}, {{"direction", 0.0}, {"oversample", EnumValue{3}}});
	REQUIRE(bright.Ok);
	CHECK(bright.Output().Pixels == std::vector<uint8_t>{192, 192, 192, 255, 0, 0, 0, 255});
	const auto dark =
		RunNode("pc.emboss", {{"surface_in", &edge}}, {{"direction", 180.0}, {"oversample", EnumValue{3}}});
	REQUIRE(dark.Ok);
	CHECK(dark.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255, 0, 0, 0, 255});
	CHECK_FALSE(RunNode("pc.emboss", {{"surface_in", &edge}}, {{"height", int64_t{0}}}).Ok);
	CHECK(
		RunNode(
			"pc.emboss", {{"surface_in", &edge}}, {{"height", int64_t{1000000}}, {"high_res", true}}
		).Code == Status::LimitExceeded
	);
}

TEST_CASE(
	"Quantize Colors retains all four source spaces and per-channel step counts", "[imagegraph][source_2d]"
) {
	const Image black{1, 1, {0, 0, 0, 128}, 0};
	for (int64_t space = 0; space < 4; ++space) {
		const auto run =
			RunNode("pc.bit_reduce", {{"surface_in", &black}}, {{"color_space", EnumValue{space}}});
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == black.Pixels);
	}
	const Image colour{1, 1, {64, 128, 192, 128}, 0};
	const auto reduced = RunNode("pc.bit_reduce", {{"surface_in", &colour}});
	REQUIRE(reduced.Ok);
	CHECK(reduced.Output().Pixels == std::vector<uint8_t>{85, 170, 255, 128});
	const auto binary = RunNode("pc.bit_reduce", {{"surface_in", &colour}}, {{"steps", Vector3{1, 1, 1}}});
	REQUIRE(binary.Ok);
	CHECK(binary.Output().Pixels == std::vector<uint8_t>{0, 255, 255, 128});
	CHECK_FALSE(RunNode("pc.bit_reduce", {{"surface_in", &colour}}, {{"alpha_steps", 1.0}}).Ok);
	CHECK_FALSE(RunNode("pc.bit_reduce", {{"surface_in", &colour}}, {{"color_space", 0.5}}).Ok);
}

TEST_CASE(
	"Quantize Colors uses all pinned Bayer patterns and preserves floating overshoot",
	"[imagegraph][source_2d]"
) {
	const Image grey{
		2, 2, {128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255}, 0
	};
	const auto bayer = RunNode("pc.bit_reduce", {{"surface_in", &grey}}, {{"dithering", true}});
	REQUIRE(bayer.Ok);
	CHECK(
		bayer.Output().Pixels ==
		std::vector<uint8_t>{85, 85, 85, 255, 170, 170, 170, 255, 170, 170, 170, 255, 85, 85, 85, 255}
	);
	for (int64_t pattern = 1; pattern < 3; ++pattern) {
		const auto run = RunNode(
			"pc.bit_reduce", {{"surface_in", &grey}}, {{"dithering", true}, {"pattern", EnumValue{pattern}}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == bayer.Output().Pixels);
	}
	const Image white{1, 1, {255, 255, 255, 255}, 0};
	const auto floating =
		RunNode("pc.bit_reduce", {{"surface_in", &white}}, {{"attribute_color_depth", EnumValue{5}}});
	REQUIRE(floating.Ok);
	CHECK(floating.Output().Format == SurfaceFormat::RGBA32Float);
	const auto pixel = engine::imagegraph::detail::ReadPixel(floating.Output(), 0, 0);
	CHECK(pixel[0] == Catch::Approx(4.0 / 3.0));
	CHECK(pixel[3] == Catch::Approx(256.0 / 255.0));
}

TEST_CASE("Checker runs solid smooth and antialiased source shader modes", "[imagegraph][source_2d]") {
	const std::array<uint8_t, 3> dark{0, 64, 40}, light{255, 191, 215};
	for (int64_t mode = 0; mode < 3; ++mode) {
		const auto run = RunNode(
			"pc.checker",
			{},
			{{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}, {"type", EnumValue{mode}}}
		);
		REQUIRE(run.Ok);
		for (uint32_t y = 0; y < 4; ++y)
			for (uint32_t x = 0; x < 4; ++x) {
				const auto &pixels = run.Output().Pixels;
				const bool alternate = (x / 2 + y / 2) % 2 != 0;
				CHECK(pixels[(y * 4 + x) * 4] == (alternate ? light[size_t(mode)] : dark[size_t(mode)]));
				CHECK(pixels[(y * 4 + x) * 4 + 3] == 255);
			}
	}
	const Image mask{1, 1, {255, 255, 255, 128}, 0};
	const auto masked = RunNode("pc.checker", {{"mask", &mask}}, {{"dimension_unit", EnumValue{2}}});
	REQUIRE(masked.Ok);
	CHECK(masked.Output().Width == 1);
	CHECK(masked.Output().Pixels[3] == 128);
	CHECK_FALSE(RunNode("pc.checker", {}, {{"size", 0.0}}).Ok);
}

TEST_CASE(
	"Source vignette preserves alpha and applies darken lighten and its authored curve",
	"[imagegraph][source_2d]"
) {
	const Image source{1, 1, {64, 32, 16, 80}, 0};
	const auto dark = RunNode("pc.vignette", {{"surface_in", &source}}, {{"exposure", 1.0}});
	REQUIRE(dark.Ok);
	CHECK(dark.Output().Pixels == std::vector<uint8_t>{32, 16, 8, 80});
	const auto light =
		RunNode("pc.vignette", {{"surface_in", &source}}, {{"exposure", 1.0}, {"lighten", 1.0}});
	REQUIRE(light.Ok);
	CHECK(light.Output().Pixels == std::vector<uint8_t>{255, 128, 64, 80});
	const auto full =
		RunNode("pc.vignette", {{"surface_in", &source}}, {{"exposure", 16.0}, {"exponent", 0.0}});
	REQUIRE(full.Ok);
	CHECK(full.Output().Pixels == source.Pixels);
	Curve flat;
	flat.Header = {0, 1, 0, 0, 1, 0};
	flat.Anchors = {{0, 0, 0, 1, 0, 0}, {0, 0, 1, 1, 0, 0}};
	const auto curved = RunNode(
		"pc.vignette",
		{{"surface_in", &source}},
		{{"exposure", 1.0}, {"strength_curved", true}, {"strength_curve", flat}}
	);
	REQUIRE(curved.Ok);
	CHECK(curved.Output().Pixels == source.Pixels);
}

TEST_CASE(
	"Source symmetric nearest chooses each RGB channel and averages half the symmetric neighbourhood",
	"[imagegraph][source_2d]"
) {
	const Image source{3, 1, {0, 0, 0, 64, 255, 255, 255, 64, 0, 0, 0, 64}, 0};
	const auto run = RunNode(
		"pc.symmetric_nn", {{"surface_in", &source}}, {{"radius", 1.0}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 85, 85, 85, 255, 0, 0, 0, 255});
	const auto zero =
		RunNode("pc.symmetric_nn", {{"surface_in", &source}}, {{"radius", 0.0}, {"intensity", 0.0}});
	REQUIRE(zero.Ok);
	CHECK(zero.Output().Pixels == source.Pixels);
	const auto limited = RunNode("pc.symmetric_nn", {{"surface_in", &source}}, {{"radius", 100000.0}});
	CHECK_FALSE(limited.Ok);
	CHECK(limited.Code == Status::LimitExceeded);
}

TEST_CASE("Source quasicrystal retains extrapolated color mixing and UV alpha", "[imagegraph][source_2d]") {
	const auto center =
		RunNode("pc.quasicrystal", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}});
	REQUIRE(center.Ok);
	CHECK(center.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	const Image map{1, 1, {128, 128, 0, 64}, 0};
	const auto mapped = RunNode(
		"pc.quasicrystal",
		{{"uv_map", &map}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"color_1", Colour{25, 50, 100, 255}},
		 {"color_2", Colour{25, 50, 100, 255}}}
	);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels == std::vector<uint8_t>{25, 50, 100, 64});
	CHECK_FALSE(RunNode("pc.quasicrystal", {}, {{"scale", 0.0}}).Ok);
}

TEST_CASE(
	"Source surface morph searches matched opaque samples with four channel distance",
	"[imagegraph][source_2d]"
) {
	const Image first{1, 1, {100, 50, 0, 255}, 0}, second{1, 1, {200, 50, 0, 255}, 0};
	for (double amount : {0.0, 0.25, 0.75, 1.0}) {
		const auto run = RunNode(
			"pc.morph_surface",
			{{"surface_from", &first}, {"surface_to", &second}},
			{{"morph_amount", amount}, {"threshold", 1.0}}
		);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == std::vector<uint8_t>{uint8_t(100 + 100 * amount), 50, 0, 255});
	}
	const auto different = RunNode(
		"pc.morph_surface", {{"surface_from", &first}, {"surface_to", &second}}, {{"threshold", 0.0}}
	);
	REQUIRE(different.Ok);
	CHECK(different.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0});
	const Image transparent{1, 1, {100, 50, 0, 0}, 0};
	const auto empty = RunNode(
		"pc.morph_surface", {{"surface_from", &transparent}, {"surface_to", &second}}, {{"threshold", 1.0}}
	);
	REQUIRE(empty.Ok);
	CHECK(empty.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0});
}

TEST_CASE(
	"Source slope blur follows the height gradient and weighted iteration curve", "[imagegraph][source_2d]"
) {
	const Image source{3, 1, {0, 0, 0, 128, 128, 128, 128, 128, 255, 255, 255, 128}, 0};
	const auto run = RunNode(
		"pc.blur_slope",
		{{"surface_in", &source}, {"slope_map", &source}},
		{{"strength", 2.0}, {"step", 0.25}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 128, 170, 170, 170, 128, 255, 255, 255, 128});
	const auto corrected = RunNode(
		"pc.blur_slope",
		{{"surface_in", &source}, {"slope_map", &source}},
		{{"strength", 2.0}, {"step", 0.25}, {"oversample", EnumValue{3}}, {"gamma_correction", true}}
	);
	REQUIRE(corrected.Ok);
	CHECK(corrected.Output().Pixels[4] > run.Output().Pixels[4]);
	const auto first =
		RunNode("pc.blur_slope", {{"surface_in", &source}, {"slope_map", &source}}, {{"strength", 1.0}});
	REQUIRE(first.Ok);
	CHECK(first.Output().Pixels == source.Pixels);
	CHECK_FALSE(
		RunNode("pc.blur_slope", {{"surface_in", &source}, {"slope_map", &source}}, {{"strength", 0.0}}).Ok
	);
}

TEST_CASE("Source wave interference applies all wave post and blending modes", "[imagegraph][source_2d]") {
	for (int64_t wave = 0; wave < 4; ++wave) {
		const auto run = RunNode(
			"pc.wave_interfere",
			{},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"scale", Vector2{0, 0}},
			 {"phases", Vector2{0.25, 0.25}},
			 {"wave", EnumValue{wave}},
			 {"blend_mode", EnumValue{1}}}
		);
		REQUIRE(run.Ok);
		const std::array<uint8_t, 4> expected{64, 0, 64, 4};
		CHECK(
			run.Output().Pixels ==
			std::vector<uint8_t>{expected[size_t(wave)], expected[size_t(wave)], expected[size_t(wave)], 255}
		);
	}
	for (int64_t blend = 0; blend < 3; ++blend) {
		const auto run = RunNode(
			"pc.wave_interfere",
			{},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"scale", Vector2{0, 0}},
			 {"phases", Vector2{0.25, 0.25}},
			 {"blend_mode", EnumValue{blend}}}
		);
		REQUIRE(run.Ok);
		const std::array<uint8_t, 3> expected{0, 64, 128};
		CHECK(run.Output().Pixels[0] == expected[size_t(blend)]);
	}
	const auto absolute = RunNode(
		"pc.wave_interfere",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"scale", Vector2{0, 0}},
		 {"phases", Vector2{0.75, 0.0}},
		 {"post_process", EnumValue{1}}}
	);
	REQUIRE(absolute.Ok);
	CHECK(absolute.Output().Pixels[0] == 128);
	const auto normalized = RunNode(
		"pc.wave_interfere",
		{},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"scale", Vector2{0, 0}},
		 {"post_process", EnumValue{2}},
		 {"blend_mode", EnumValue{1}},
		 {"pattern", EnumValue{1}}}
	);
	REQUIRE(normalized.Ok);
	CHECK(normalized.Output().Pixels[0] == 64);
}

TEST_CASE(
	"Source height blend retains each union and intersection smoothing formula", "[imagegraph][source_2d]"
) {
	const Image background{1, 1, {128, 128, 128, 255}, 0}, foreground{1, 1, {64, 64, 64, 255}, 0};
	const std::array<std::array<uint8_t, 6>, 2> expected{
		{{165, 132, 129, 164, 165, 163}, {27, 51, 54, 28, 27, 29}}
	};
	for (int64_t mode = 0; mode < 2; ++mode)
		for (int64_t type = 0; type < 6; ++type) {
			const auto run = RunNode(
				"pc.blend_height",
				{{"background", &background}, {"foreground", &foreground}},
				{{"mode", EnumValue{mode}}, {"type", EnumValue{type}}, {"factor", 0.25}}
			);
			REQUIRE(run.Ok);
			const uint8_t level = expected[size_t(mode)][size_t(type)];
			CHECK(run.Output().Pixels == std::vector<uint8_t>{level, level, level, 255});
		}
	CHECK_FALSE(RunNode(
					"pc.blend_height",
					{{"background", &background}, {"foreground", &background}},
					{{"type", EnumValue{2}}}
	).Ok);
}

TEST_CASE(
	"Source deblur retains the vertical unsharp pass and distinct enhancement kernels",
	"[imagegraph][source_2d]"
) {
	const Image source{3, 1, {64, 64, 64, 80, 128, 128, 128, 80, 64, 64, 64, 80}, 0};
	const auto unsharp = RunNode(
		"pc.deblur",
		{{"surface_in", &source}},
		{{"method", EnumValue{0}}, {"radius", 1.0}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(unsharp.Ok);
	CHECK(unsharp.Output().Pixels == source.Pixels);
	const auto edge = RunNode(
		"pc.deblur",
		{{"surface_in", &source}},
		{{"method", EnumValue{1}}, {"radius", 1.0}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(edge.Ok);
	CHECK(edge.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 80, 255, 255, 255, 80, 0, 0, 0, 80});
	const auto wiener = RunNode(
		"pc.deblur",
		{{"surface_in", &source}},
		{{"method", EnumValue{2}}, {"radius", 1.0}, {"denoise", 100.0}, {"oversample", EnumValue{3}}}
	);
	REQUIRE(wiener.Ok);
	CHECK(wiener.Output().Pixels == std::vector<uint8_t>{45, 45, 45, 80, 166, 166, 166, 80, 45, 45, 45, 80});
}

TEST_CASE("Source linear brush normalizes RGB while retaining accumulated alpha", "[imagegraph][source_2d]") {
	const Image source{1, 1, {50, 100, 200, 64}, 0};
	const auto run = RunNode(
		"pc.brush_linear", {{"surface_in", &source}}, {{"iteration", int64_t{3}}, {"attenuation", 0.5}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{50, 100, 200, 112});
	const auto seeded = RunNode(
		"pc.brush_linear",
		{{"surface_in", &source}},
		{{"iteration", int64_t{3}}, {"attenuation", 0.5}, {"seed", int64_t{99}}}
	);
	REQUIRE(seeded.Ok);
	CHECK(seeded.Output().Pixels == run.Output().Pixels);
	CHECK_FALSE(RunNode("pc.brush_linear", {{"surface_in", &source}}, {{"length", 0.0}}).Ok);
	CHECK(
		RunNode("pc.brush_linear", {{"surface_in", &source}}, {{"iteration", int64_t{100000000}}}).Code ==
		Status::LimitExceeded
	);
}

TEST_CASE("Source shape blur retains blur max control and zero-strength bypass", "[imagegraph][source_2d]") {
	const Image source{1, 1, {64, 128, 192, 80}, 0},
		shape{2, 2, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, 0},
		zero{1, 1, {0, 0, 0, 255}, 0};
	const auto blur = RunNode("pc.blur_shape", {{"surface_in", &source}, {"blur_shape", &shape}});
	REQUIRE(blur.Ok);
	CHECK(blur.Output().Pixels == source.Pixels);
	const auto maximum =
		RunNode("pc.blur_shape", {{"surface_in", &source}, {"blur_shape", &shape}}, {{"mode", EnumValue{1}}});
	REQUIRE(maximum.Ok);
	CHECK(maximum.Output().Pixels == std::vector<uint8_t>{64, 128, 192, 255});
	const auto authoredGamma = RunNode(
		"pc.blur_shape", {{"surface_in", &source}, {"blur_shape", &shape}}, {{"gamma_correction", true}}
	);
	REQUIRE(authoredGamma.Ok);
	CHECK(authoredGamma.Output().Pixels == source.Pixels);
	const auto bypass =
		RunNode("pc.blur_shape", {{"surface_in", &source}, {"blur_shape", &zero}, {"blur_mask", &zero}});
	REQUIRE(bypass.Ok);
	CHECK(bypass.Output().Pixels == source.Pixels);
	CHECK_FALSE(RunNode("pc.blur_shape", {{"surface_in", &source}, {"blur_shape", &zero}}).Ok);
}

TEST_CASE(
	"Source de-corner evaluates all four corner orientations in both modes", "[imagegraph][source_2d]"
) {
	for (int64_t type = 0; type < 2; ++type)
		for (int rotation = 0; rotation < 4; ++rotation) {
			Image source{3, 3, std::vector<uint8_t>(36), 0};
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 3; ++x) {
					int rotatedX = x, rotatedY = y;
					for (int i = 0; i < rotation; ++i) {
						const int previousX = rotatedX;
						rotatedX = 2 - rotatedY;
						rotatedY = previousX;
					}
					const uint8_t level = x <= 1 && y <= 1 ? 255 : 0;
					const size_t offset = size_t(rotatedY * 3 + rotatedX) * 4;
					source.Pixels[offset] = source.Pixels[offset + 1] = source.Pixels[offset + 2] = level;
					source.Pixels[offset + 3] = 255;
				}
			const auto run = RunNode(
				"pc.de_corner",
				{{"surface_in", &source}},
				{{"iteration", int64_t{1}}, {"type", EnumValue{type}}, {"oversample", EnumValue{3}}}
			);
			REQUIRE(run.Ok);
			CHECK(run.Output().Pixels[16] == 0);
			CHECK(run.Output().Pixels[19] == 255);
			const auto untouched =
				RunNode("pc.de_corner", {{"surface_in", &source}}, {{"iteration", int64_t{0}}});
			REQUIRE(untouched.Ok);
			CHECK(untouched.Output().Pixels == source.Pixels);
		}
}

TEST_CASE("Source fold noise retains greyscale map and unclamped alpha output", "[imagegraph][source_2d]") {
	for (int64_t mode = 0; mode < 2; ++mode) {
		const auto run = RunNode(
			"pc.fold_noise",
			{},
			{{"dimension", Vector2{2, 2}},
			 {"dimension_unit", EnumValue{0}},
			 {"iteration", int64_t{0}},
			 {"mode", EnumValue{mode}}}
		);
		REQUIRE(run.Ok);
		for (size_t pixel = 0; pixel < 4; ++pixel) {
			CHECK(run.Output().Pixels[pixel * 4] == (mode == 0 ? 180 : 128));
			CHECK(run.Output().Pixels[pixel * 4 + 1] == (mode == 0 ? 180 : 128));
			CHECK(run.Output().Pixels[pixel * 4 + 2] == (mode == 0 ? 180 : 0));
		}
	}
	const auto floating = RunNode(
		"pc.fold_noise",
		{},
		{{"dimension", Vector2{2, 2}},
		 {"dimension_unit", EnumValue{0}},
		 {"iteration", int64_t{0}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(floating.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(floating.Output(), 0, 0)[3] == Catch::Approx(1 + std::sqrt(0.5))
	);
	CHECK(RunNode("pc.fold_noise", {}, {{"iteration", int64_t{100000000}}}).Code == Status::LimitExceeded);
}

TEST_CASE(
	"Source Gaussian conversion uses red channels for Box Muller and level remapping",
	"[imagegraph][source_2d]"
) {
	const Image first{1, 1, {128, 255, 0, 0}, 0}, second{1, 1, {0, 255, 0, 0}, 0};
	const auto run = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &first}, {"conv_surf_2", &second}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"varience", 0.25}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{202, 202, 202, 255});
	const auto shifted = RunNode(
		"pc.noise_gaussian",
		{{"conv_surf_1", &first}, {"conv_surf_2", &second}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"use_conversion", true},
		 {"varience", 0.0},
		 {"level_out", Vector2{0.25, 0.75}}}
	);
	REQUIRE(shifted.Ok);
	CHECK(shifted.Output().Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	const Image zero{1, 1, {0, 0, 0, 255}, 0};
	CHECK_FALSE(RunNode(
					"pc.noise_gaussian",
					{{"conv_surf_1", &zero}, {"conv_surf_2", &second}},
					{{"use_conversion", true}}
	).Ok);
	const auto seeded = RunNode(
		"pc.noise_gaussian",
		{},
		{{"seed", 0.0}, {"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(seeded.Ok);
	const auto replay = RunNode(
		"pc.noise_gaussian",
		{},
		{{"seed", 0.0}, {"dimension", Vector2{2, 2}}, {"dimension_unit", EnumValue{0}}}
	);
	REQUIRE(replay.Ok);
	CHECK(seeded.Output().Pixels == replay.Output().Pixels);
}

TEST_CASE(
	"Source zigzag uses snapped pixel coordinates in all four render modes", "[imagegraph][source_2d]"
) {
	const std::array<std::array<uint8_t, 4>, 4> expected{
		{{255, 0, 0, 0}, {255, 128, 0, 128}, {255, 191, 128, 191}, {255, 128, 0, 128}}
	};
	for (int64_t type = 0; type < 4; ++type) {
		const auto run = RunNode(
			"pc.zigzag",
			{},
			{{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}, {"type", EnumValue{type}}}
		);
		REQUIRE(run.Ok);
		for (size_t pixel = 0; pixel < 4; ++pixel)
			CHECK(run.Output().Pixels[pixel * 4] == expected[size_t(type)][pixel]);
	}
	CHECK_FALSE(RunNode("pc.zigzag", {}, {{"size", 0.0}}).Ok);
}

TEST_CASE("Source box pattern preserves cross modulo and multiscale XOR", "[imagegraph][source_2d]") {
	for (int64_t render = 0; render < 3; ++render) {
		const auto run = RunNode(
			"pc.box_pattern",
			{},
			{{"dimension", Vector2{4, 4}},
			 {"dimension_unit", EnumValue{0}},
			 {"render_type", EnumValue{render}}}
		);
		REQUIRE(run.Ok);
		const std::array<uint8_t, 3> expected{255, 191, 0};
		for (size_t pixel = 0; pixel < 16; ++pixel)
			CHECK(run.Output().Pixels[pixel * 4] == expected[size_t(render)]);
	}
	const auto xored = RunNode(
		"pc.box_pattern",
		{},
		{{"dimension", Vector2{4, 4}}, {"dimension_unit", EnumValue{0}}, {"pattern", EnumValue{1}}}
	);
	REQUIRE(xored.Ok);
	CHECK(xored.Output().Pixels[0] == 255);
	CHECK(xored.Output().Pixels[8] == 16);
	CHECK_FALSE(RunNode("pc.box_pattern", {}, {{"scale", 0.0}}).Ok);
}

TEST_CASE(
	"Source matrix interpretation tiles offsets and supports palette and gradient mapping",
	"[imagegraph][source_2d]"
) {
	const MatrixValue matrix{2, 1, {0.0, 0.5}};
	const auto grey = RunNode(
		"pc.interpret_matrix",
		{},
		{{"matrix", matrix},
		 {"dimension", Vector2{4, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"offset", Vector2{1, 0}}}
	);
	REQUIRE(grey.Ok);
	CHECK(
		grey.Output().Pixels ==
		std::vector<uint8_t>{128, 128, 128, 255, 0, 0, 0, 255, 128, 128, 128, 255, 0, 0, 0, 255}
	);
	const auto palette = RunNode(
		"pc.interpret_matrix",
		{},
		{{"matrix", MatrixValue{2, 1, {0, 1}}},
		 {"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"mode", EnumValue{1}}}
	);
	REQUIRE(palette.Ok);
	CHECK(palette.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 255, 0, 0, 0, 255});
	Gradient ramp{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}};
	const auto gradient = RunNode(
		"pc.interpret_matrix",
		{},
		{{"matrix", matrix},
		 {"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"mode", EnumValue{2}},
		 {"gradient", ramp}}
	);
	REQUIRE(gradient.Ok);
	CHECK(gradient.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 255, 128, 128, 128, 255});
	CHECK_FALSE(RunNode("pc.interpret_matrix", {}, {{"range", Vector2{0, 0}}}).Ok);
}

TEST_CASE(
	"Pixel Builder box conversion preserves anchor units and outward rounding", "[imagegraph][source_2d]"
) {
	PixelBoxValue box;
	auto &data = box.Data.emplace();
	data.BaseBounds = {10, 20, 30, 60};
	data.Anchors = {0.125, 0.25, 0, 0, 0.5, 0.25};
	data.Fractional = {true, true, false, false, true, true};
	const auto run = RunNode("pc.pb_box_bbox", {}, {{"pbbox", box}});
	REQUIRE(run.Ok);
	CHECK(std::get<Vector4>(*run.OutputValue("bbox")) == Vector4{12, 30, 23, 40});
	CHECK(std::get<Vector2>(*run.OutputValue("dimension")) == Vector2{11, 10});
	const auto &area = std::get<ArrayValue>(*run.OutputValue("area"));
	REQUIRE(area.Elements.size() == 5);
	CHECK(std::get<double>(area.Elements[0]) == 17.5);
	CHECK(std::get<double>(area.Elements[2]) == 5.5);
}

TEST_CASE("Pixel Builder box split keeps unbounded ratios and reversed anchor", "[imagegraph][source_2d]") {
	const auto split = RunNode("pc.pb_box_split", {}, {{"ratio", 1.25}});
	REQUIRE(split.Ok);
	const auto &first = std::get<PixelBoxValue>(*split.OutputValue("pbbox"));
	const auto &second = std::get<PixelBoxValue>(*split.OutputValue("pbbox_2"));
	const auto firstBounds = RunNode("pc.pb_box_bbox", {}, {{"pbbox", first}});
	const auto secondBounds = RunNode("pc.pb_box_bbox", {}, {{"pbbox", second}});
	REQUIRE(firstBounds.Ok);
	REQUIRE(secondBounds.Ok);
	CHECK(std::get<Vector4>(*firstBounds.OutputValue("bbox")) == Vector4{0, 0, 40, 32});
	CHECK(std::get<Vector4>(*secondBounds.OutputValue("bbox")) == Vector4{40, 0, 32, 32});
	const auto reversed = RunNode(
		"pc.pb_box_split",
		{},
		{{"unit", EnumValue{1}}, {"size", int64_t{7}}, {"anchor", EnumValue{1}}, {"axis", EnumValue{1}}}
	);
	REQUIRE(reversed.Ok);
	const auto &bottom = std::get<PixelBoxValue>(*reversed.OutputValue("pbbox_2"));
	const auto bottomBounds = RunNode("pc.pb_box_bbox", {}, {{"pbbox", bottom}});
	REQUIRE(bottomBounds.Ok);
	CHECK(std::get<Vector4>(*bottomBounds.OutputValue("bbox")) == Vector4{0, 25, 32, 32});
}

TEST_CASE("Pixel Builder box ignores unlinked scalar overrides", "[imagegraph][source_2d]") {
	const auto box = RunNode("pc.pb_box", {}, {{"pbbox_width", 3.0}, {"pbbox_left", 9.0}});
	REQUIRE(box.Ok);
	const auto converted = RunNode("pc.pb_box_bbox", {}, {{"pbbox", *box.OutputValue("pbbox")}});
	REQUIRE(converted.Ok);
	CHECK(std::get<Vector4>(*converted.OutputValue("bbox")) == Vector4{0, 0, 32, 32});
	const auto point = RunNode("pc.pb_box_point", {}, {{"position", Vector2{0.25, -0.5}}});
	REQUIRE(point.Ok);
	CHECK(std::get<Vector2>(*point.OutputValue("point")) == Vector2{24, 0});
}

TEST_CASE("PB crop clips source pixels into the resolved box", "[imagegraph][source_2d]") {
	const Image surface =
		imagegraph_test::MakeImage(3, 1, {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120});
	PixelBoxValue box;
	box.Data.emplace().FixedBounds = std::array<double, 4>{1, 0, 4, 1};
	const auto cropped = RunNode("pc.pb_crop_pbbox", {{"surface", &surface}}, {{"pbbox", box}});
	REQUIRE(cropped.Ok);
	CHECK(
		cropped.Output("surface").Pixels ==
		std::vector<uint8_t>{50, 60, 70, 80, 90, 100, 110, 120, 0, 0, 0, 0}
	);
}

TEST_CASE("PB surface mirror preserves source additive alpha blending", "[imagegraph][source_2d]") {
	const Image surface = imagegraph_test::MakeImage(2, 1, {64, 0, 0, 128, 0, 64, 0, 128});
	PixelBoxValue box;
	box.Data.emplace().FixedBounds = std::array<double, 4>{0, 0, 2, 1};
	const auto mirrored =
		RunNode("pc.pb_filter_mirror", {{"surface", &surface}}, {{"pbbox", box}, {"axis", int64_t{1}}});
	REQUIRE(mirrored.Ok);
	CHECK(mirrored.Output().Pixels == std::vector<uint8_t>{32, 64, 0, 255, 64, 32, 0, 255});
}

TEST_CASE("PB highlight uses source padding order and first nearest side", "[imagegraph][source_2d]") {
	const Image surface =
		imagegraph_test::MakeImage(3, 3, {0,  0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 20,
										  30, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	 0});
	const auto highlighted = RunNode(
		"pc.pb_fx_highlight",
		{{"surface", &surface}},
		{{"width", Vector4{1, 1, 1, 1}},
		 {"color_left", Colour{255, 0, 0, 255}},
		 {"color_right", Colour{0, 255, 0, 255}}}
	);
	REQUIRE(highlighted.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(highlighted.Output(), 1, 1) ==
		engine::imagegraph::detail::Rgba{1, 0, 0, 1}
	);
	const auto rightOnly = RunNode(
		"pc.pb_fx_highlight",
		{{"surface", &surface}},
		{{"width", Vector4{1, 0, 0, 0}}, {"color_right", Colour{0, 255, 0, 255}}}
	);
	REQUIRE(rightOnly.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(rightOnly.Output(), 1, 1) ==
		engine::imagegraph::detail::Rgba{0, 1, 0, 1}
	);
}

TEST_CASE(
	"PB extrusion keeps foreground and clones colour along source direction", "[imagegraph][source_2d]"
) {
	const Image surface = imagegraph_test::MakeImage(3, 1, {0, 0, 0, 0, 64, 128, 192, 128, 0, 0, 0, 0});
	const auto extruded = RunNode(
		"pc.pb_fx_extrude",
		{{"surface", &surface}},
		{{"use_pbbox", false},
		 {"distance", int64_t{1}},
		 {"clone_color", true},
		 {"color", Colour{255, 128, 255, 255}}}
	);
	REQUIRE(extruded.Ok);
	CHECK(extruded.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0, 64, 128, 192, 128, 64, 64, 192, 128});
	const auto highlighted = RunNode(
		"pc.pb_fx_extrude",
		{{"surface", &surface}},
		{{"use_pbbox", false}, {"highlight", true}, {"highlight_color", Colour{0, 255, 0, 255}}}
	);
	REQUIRE(highlighted.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(highlighted.Output(), 1, 0) ==
		engine::imagegraph::detail::Rgba{0, 1, 0, 1}
	);
}

TEST_CASE(
	"Pixel Builder owns its resize recipe and preserves source layer alpha", "[imagegraph][source_2d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes.push_back(
		Node{
			"builder",
			"pc.pixel_builder",
			"",
			{},
			{{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}
		}
	);
	const auto *entry = FindCatalogueEntry("pc.pixel_builder");
	REQUIRE(entry);
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.pixel_builder");
	REQUIRE(executor);
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(document.Nodes.front(), *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.EvaluationDocument = &document;
	context.PixelBuilderCanvas = Vector2{1, 1};
	const Image source = imagegraph_test::MakeImage(1, 1, {128, 0, 0, 128});
	const std::array<engine::imagegraph::detail::PixelBuilderLayer, 1> layers{
		engine::imagegraph::detail::PixelBuilderLayer{"layer", &source, 1, 0, true}
	};
	context.PixelBuilderLayers = layers;
	REQUIRE(executor(context));
	REQUIRE(context.OutputImages.size() == 1);
	CHECK(context.OutputImages.front().second.Pixels == std::vector<uint8_t>{64, 0, 0, 64});
	REQUIRE(context.OutputValues.size() == 1);
	const auto &recipe = std::get<DynamicSurfaceValue>(context.OutputValues.front().Data);
	REQUIRE(recipe.Data);
	CHECK(recipe.Data->BaseDimension == Vector2{1, 1});
	document.Nodes.front().Values.front().Data = Vector2{7, 9};
	CHECK(std::get<Vector2>(recipe.Data->Authored.Nodes.front().Values.front().Data) == Vector2{1, 1});
}

TEST_CASE("Pixel Builder zero thickness outline includes orthogonal neighbours", "[imagegraph][source_2d]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes.push_back(
		Node{"builder", "pc.pixel_builder", "", {}, {{"outline", true}, {"color", Colour{255, 0, 0, 255}}}}
	);
	const auto *entry = FindCatalogueEntry("pc.pixel_builder");
	REQUIRE(entry);
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(document.Nodes.front(), *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.EvaluationDocument = &document;
	context.PixelBuilderCanvas = Vector2{3, 3};
	context.Values = {{"outline", true}, {"color", Colour{255, 0, 0, 255}}};
	const Image source =
		imagegraph_test::MakeImage(3, 3, {0,   0,	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255,
										  255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   0});
	const std::array<engine::imagegraph::detail::PixelBuilderLayer, 1> layers{
		engine::imagegraph::detail::PixelBuilderLayer{"layer", &source, 1, 0, true}
	};
	context.PixelBuilderLayers = layers;
	REQUIRE(engine::imagegraph::detail::FindExecutor("pc.pixel_builder")(context));
	const auto &output = context.OutputImages.front().second;
	CHECK(
		engine::imagegraph::detail::ReadPixel(output, 1, 0) == engine::imagegraph::detail::Rgba{1, 0, 0, 1}
	);
	CHECK(
		engine::imagegraph::detail::ReadPixel(output, 0, 0) == engine::imagegraph::detail::Rgba{0, 0, 0, 0}
	);
}

TEST_CASE("PB Draw Surface uses source size and fixed box scissor", "[imagegraph][source_2d]") {
	const Image surface =
		imagegraph_test::MakeImage(3, 1, {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120});
	PixelBoxValue box;
	box.Data.emplace().FixedBounds = std::array<double, 4>{0, 0, 1, 1};
	const auto full =
		RunNode("pc.pb_draw_surface", {{"surface", &surface}}, {{"pbbox", box}, {"pbbox_width", 99.0}});
	REQUIRE(full.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(full.Output("surface"), 2, 0) ==
		engine::imagegraph::detail::Rgba{90 / 255.0, 100 / 255.0, 110 / 255.0, 120 / 255.0}
	);
	const auto clipped =
		RunNode("pc.pb_draw_surface", {{"surface", &surface}}, {{"pbbox", box}, {"crop", true}});
	REQUIRE(clipped.Ok);
	CHECK(
		engine::imagegraph::detail::ReadPixel(clipped.Output("surface"), 1, 0) ==
		engine::imagegraph::detail::Rgba{0, 0, 0, 0}
	);
}

TEST_CASE("PB rectangle resolves grouped checker pattern in pixel units", "[imagegraph][source_2d]") {
	Node node{"rectangle", "pc.pb_draw_rectangle", "", {}, {}};
	node.DynamicInputs.push_back({"effect_type_0", ValueType::Enum, EnumValue{0}});
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Project.SurfaceWidth = 4;
	context.Project.SurfaceHeight = 1;
	context.Values = {
		{"color_0", Colour{255, 0, 0, 255}},
		{"pattern_0", EnumValue{7}},
		{"pattern_color_0", Colour{0, 0, 255, 255}},
		{"pattern_scale_0", Vector2{1, 1}},
		{"pattern_scale_unit_0", EnumValue{0}}
	};
	REQUIRE(engine::imagegraph::detail::FindExecutor(node.Type)(context));
	REQUIRE(context.OutputImages.size() == 1);
	CHECK(
		context.OutputImages.front().second.Pixels ==
		std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255}
	);
	REQUIRE(context.OutputValues.size() == 1);
	const auto &box = std::get<PixelBoxValue>(context.OutputValues.front().Data);
	REQUIRE(box.Data);
	CHECK(box.Data->BaseBounds == std::array<double, 4>{0, 0, 4, 1});
}

TEST_CASE("PB inherited effects retain first-pass clearing and source tint", "[imagegraph][source_2d]") {
	using namespace engine::imagegraph::detail;
	Node node{"rectangle", "pc.pb_draw_rectangle", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	const Image shape = imagegraph_test::MakeImage(3, 1, {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0});
	for (int64_t type = 0; type < 6; ++type) {
		NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		PixelBuilderEffect effect;
		effect.Type = type;
		effect.Color = {1, 0, 0, 1};
		effect.HighlightWidths.Z = 1;
		effect.HighlightColors[0] = {1, 0, 0, 1};
		effect.Direction = 0;
		effect.Shines = {3};
		effect.Slope = 0;
		effect.Progress = .25;
		Image output = imagegraph_test::MakeImage(3, 1, std::vector<uint8_t>(12));
		REQUIRE(
			ApplyPixelBuilderEffects(context, shape, {1, 0, 2, 1}, std::span(&effect, 1), output, {3, 1})
		);
		if (type == 4) {
			CHECK(ReadPixel(output, 1, 0) == Rgba{0, 0, 0, 0});
			CHECK(ReadPixel(output, 2, 0) == Rgba{1, 0, 0, 1});
		} else {
			CHECK(ReadPixel(output, 1, 0) == Rgba{1, 0, 0, 1});
			CHECK(ReadPixel(output, 0, 0) == Rgba{0, 0, 0, 0});
		}
	}
}

TEST_CASE("PB diamond shader preserves scale and minimum corner masks", "[imagegraph][source_2d]") {
	for (int64_t corner = 0; corner < 2; ++corner) {
		Node node{"diamond", "pc.pb_draw_diamond", "", {}, {}};
		node.DynamicInputs.push_back({"effect_type_0", ValueType::Enum, EnumValue{0}});
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		EvaluationRequest request;
		engine::imagegraph::detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Project.SurfaceWidth = 4;
		context.Project.SurfaceHeight = 4;
		context.Values = {{"corner", EnumValue{corner}}};
		REQUIRE(engine::imagegraph::detail::FindExecutor(node.Type)(context));
		const auto &output = context.OutputImages.front().second;
		CHECK(
			engine::imagegraph::detail::ReadPixel(output, 0, 0) ==
			engine::imagegraph::detail::Rgba{0, 0, 0, 0}
		);
		CHECK(
			engine::imagegraph::detail::ReadPixel(output, 1, 0) ==
			engine::imagegraph::detail::Rgba{1, 1, 1, 1}
		);
		CHECK(
			engine::imagegraph::detail::ReadPixel(output, 1, 1) ==
			engine::imagegraph::detail::Rgba{1, 1, 1, 1}
		);
		CHECK(
			engine::imagegraph::detail::ReadPixel(output, 3, 3) ==
			engine::imagegraph::detail::Rgba{0, 0, 0, 0}
		);
	}
}

TEST_CASE("Standalone Shine preserves source palette order and mask output", "[imagegraph][source_2d]") {
	const Image source = imagegraph_test::MakeImage(3, 1, {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255});
	const ArrayValue shines{ValueType::Scalar, {3.0}};
	const ArrayValue palette{
		ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}, Colour{0, 0, 255, 255}}
	};
	const auto run = RunNode(
		"pc.pb_fx_shine",
		{{"surface", &source}},
		{{"shines", shines}, {"colors", palette}, {"slope", 0.0}, {"progress", .25}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255});
	CHECK(run.Output("mask").Pixels == std::vector<uint8_t>{0, 255, 0, 255, 0, 255, 0, 255, 0, 0, 0, 0});
}

TEST_CASE("Matrix Color Apply uses raw coefficients and preserves alpha", "[imagegraph][source_2d]") {
	const Image source = imagegraph_test::MakeImage(1, 1, {20, 80, 160, 120});
	const MatrixValue permutation{3, 3, {0, 0, 1, 1, 0, 0, 0, 1, 0}};
	const auto run = RunNode("pc.matrix_color_apply", {{"surface_in", &source}}, {{"matrix", permutation}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{160, 20, 80, 120});
	const MatrixValue shortMatrix{1, 1, {2}};
	const auto padded =
		RunNode("pc.matrix_color_apply", {{"surface_in", &source}}, {{"matrix", shortMatrix}});
	REQUIRE(padded.Ok);
	CHECK(padded.Output().Pixels == std::vector<uint8_t>{40, 0, 0, 120});
}

TEST_CASE("PB Polar rotates around box center and adds source alpha", "[imagegraph][source_2d]") {
	const Image source = imagegraph_test::MakeImage(1, 1, {64, 0, 0, 128});
	PixelBoxValue box;
	box.Data.emplace().FixedBounds = std::array<double, 4>{0, 0, 1, 1};
	const auto run =
		RunNode("pc.pb_filter_polar", {{"surface", &source}}, {{"pbbox", box}, {"copies", int64_t{2}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{96, 0, 0, 255});
	const auto empty = RunNode("pc.pb_filter_polar", {{"surface", &source}}, {{"copies", int64_t{0}}});
	REQUIRE(empty.Ok);
	CHECK(empty.Output().Pixels == std::vector<uint8_t>{0, 0, 0, 0});
}

TEST_CASE("PB source primitive vertices retain deterministic triangle coverage", "[imagegraph][source_2d]") {
	using namespace engine::imagegraph::detail;
	for (const auto &[type, filled] : std::array<std::pair<std::string_view, size_t>, 3>{
			 {{"pc.pb_draw_quadrilateral", 9}, {"pc.pb_draw_trapezoid", 12}, {"pc.pb_draw_triangle", 7}}
		 }) {
		Node node{"shape", std::string(type), "", {}, {}};
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		EvaluationRequest request;
		NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Image shape = imagegraph_test::MakeImage(4, 4, std::vector<uint8_t>(64));
		REQUIRE(RasterPixelBuilderPrimitive(context, {0, 0, 4, 4}, shape));
		size_t count = 0;
		for (uint32_t y = 0; y < 4; ++y)
			for (uint32_t x = 0; x < 4; ++x)
				count += ReadPixel(shape, x, y)[3] != 0;
		CHECK(count == filled);
	}
}

TEST_CASE("PB curve round caps update subsequent ellipse precision", "[imagegraph][source_2d]") {
	using namespace engine::imagegraph::detail;
	Node curve{"curve", "pc.pb_draw_curve", "", {}, {}};
	Node ellipse{"ellipse", "pc.pb_draw_ellipse", "", {}, {}};
	EvaluationRequest request;
	const auto *curveEntry = FindCatalogueEntry(curve.Type), *ellipseEntry = FindCatalogueEntry(ellipse.Type);
	REQUIRE(curveEntry);
	REQUIRE(ellipseEntry);
	PixelBuilderDrawState state{24};
	NodeContext curveContext(curve, *curveEntry, request);
	curveContext.ByteBudget = Limits::MaximumEvaluationBytes;
	curveContext.PixelBuilderDrawing = &state;
	curveContext.Values = {{"thickness", int64_t{2}}};
	Image curveImage = imagegraph_test::MakeImage(12, 12, std::vector<uint8_t>(576));
	REQUIRE(RasterPixelBuilderPrimitive(curveContext, {0, 0, 12, 12}, curveImage));
	CHECK(state.CirclePrecision == 8);
	NodeContext ellipseContext(ellipse, *ellipseEntry, request);
	ellipseContext.ByteBudget = Limits::MaximumEvaluationBytes;
	ellipseContext.PixelBuilderDrawing = &state;
	Image inherited = imagegraph_test::MakeImage(12, 12, std::vector<uint8_t>(576));
	REQUIRE(RasterPixelBuilderPrimitive(ellipseContext, {0, 0, 12, 12}, inherited));
	PixelBuilderDrawState referenceState{8};
	ellipseContext.PixelBuilderDrawing = &referenceState;
	Image reference = imagegraph_test::MakeImage(12, 12, std::vector<uint8_t>(576));
	REQUIRE(RasterPixelBuilderPrimitive(ellipseContext, {0, 0, 12, 12}, reference));
	CHECK(inherited.Pixels == reference.Pixels);
	referenceState.CirclePrecision = 24;
	Image fine = imagegraph_test::MakeImage(12, 12, std::vector<uint8_t>(576));
	REQUIRE(RasterPixelBuilderPrimitive(ellipseContext, {0, 0, 12, 12}, fine));
	CHECK(inherited.Pixels != fine.Pixels);
}

TEST_CASE("PB effect controls bound 64 complete groups including units", "[imagegraph][source_2d]") {
	using namespace engine::imagegraph::detail;
	Node node{"rectangle", "pc.pb_draw_rectangle", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	REQUIRE(entry->DynamicTemplate.size() == 28);
	for (size_t group = 0; group < 64; ++group)
		for (const auto &input : entry->DynamicTemplate)
			node.DynamicInputs.push_back(
				{std::string(input.Id) + "_" + std::to_string(group), input.Type, CatalogueDefault(input)}
			);
	REQUIRE(node.DynamicInputs.size() == 1792);
	EvaluationRequest request;
	NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"pattern_scale_unit_63", EnumValue{0}},
		{"pattern_position_unit_63", EnumValue{0}},
		{"pattern_scale_63", Vector2{2, 3}},
		{"pattern_position_63", Vector2{4, 5}}
	};
	std::vector<PixelBuilderEffect> effects;
	REQUIRE(ReadPixelBuilderEffects(context, {10, 20}, effects));
	REQUIRE(effects.size() == 64);
	CHECK(effects.back().PatternScale == Vector2{2, 3});
	CHECK(effects.back().PatternPosition == Vector2{4, 5});
	node.DynamicInputs.push_back({"effect_type_64", ValueType::Enum, EnumValue{0}});
	effects.clear();
	CHECK_FALSE(ReadPixelBuilderEffects(context, {10, 20}, effects));
	CHECK(context.FailureCode == Status::LimitExceeded);
	node.DynamicInputs.clear();
	for (size_t group = 0; group < 65; ++group)
		node.DynamicInputs.push_back({"effect_type_" + std::to_string(group), ValueType::Enum, EnumValue{0}});
	NodeContext oversized(node, *entry, request);
	oversized.ByteBudget = Limits::MaximumEvaluationBytes;
	effects.clear();
	CHECK_FALSE(ReadPixelBuilderEffects(oversized, {10, 20}, effects));
	CHECK(oversized.FailureCode == Status::LimitExceeded);
}

TEST_CASE(
	"Dynamic Builder group replay snapshots own declarations and preserve failed copies",
	"[imagegraph][source_2d]"
) {
	using namespace engine::imagegraph::detail;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"input",
		 "pc.group_input",
		 "group",
		 {},
		 {{"input_type", EnumValue{2}},
		  {"subtype", EnumValue{0}},
		  {"vector_size", EnumValue{0}},
		  {"parent_value", .5}}},
		{"output", "pc.group_output", "group", {}, {}}
	};
	Group group{"group", "Group"};
	group.Ports = {
		{"input", "input/parent", PortDirection::Input, "input"},
		{"output", "output/parent", PortDirection::Output, "output"}
	};
	document.Groups.push_back(std::move(group));
	document.Junctions = {
		{"input/parent", "group", ValueType::Any, .5},
		{"output/parent", "group", ValueType::Any, std::nullopt}
	};
	document.Links = {
		{"input/parent", "value", "input", "parent_value"},
		{"input", "value", "output", "value"},
		{"output", "value", "output/parent", "value"}
	};
	document.Outputs = {{"result", "output", "value"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	GroupReplayState empty, loaded;
	GroupRefreshEvent event;
	event.NodeId = "input";
	REQUIRE(
		ReplayGroupRefresh(document, plan, std::span(&event, 1), empty, 1, loaded, diagnostic) == Status::Ok
	);
	REQUIRE(loaded.Find("input"));
	PixelBuilderGroupState snapshot;
	REQUIRE(
		FreezePixelBuilderGroups(loaded, snapshot, diagnostic, Limits::MaximumEvaluationBytes) == Status::Ok
	);
	REQUIRE(snapshot.Replay.Find("input"));
	CHECK(snapshot.Replay.Find("input") != loaded.Find("input"));
	PixelBuilderGroupState copied(snapshot);
	CHECK(copied == snapshot);
	CHECK(copied.Replay.Find("input") != snapshot.Replay.Find("input"));
	loaded = GroupReplayState{};
	CHECK(snapshot.Replay.Find("input") != nullptr);
	CHECK(FreezePixelBuilderGroups(empty, snapshot, diagnostic, 1) == Status::LimitExceeded);
	CHECK(snapshot == copied);
}

TEST_CASE(
	"Pixel Builder freezes a live host surface before resizing without its provider",
	"[imagegraph][source_2d]"
) {
	class Provider final : public HostNodeProvider {
	  public:
		uint32_t Calls = 0;
		bool Capture(const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &) override {
			++Calls;
			output.Authored = invocation.Authored;
			output.Tick = invocation.Request.Tick;
			output.Subframe = invocation.Request.Subframe;
			output.NegativeFrame = invocation.Request.NegativeFrame;
			output.Inputs.assign(invocation.Inputs.begin(), invocation.Inputs.end());
			output.Images = {{"surface", imagegraph_test::MakeImage(1, 1, {91, 32, 17, 255})}};
			return true;
		}
	} provider;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"builder",
		 "pc.pixel_builder",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{0}}}},
		{"layer", "pc.pb_output", "builder", {}, {}},
		{"live", "pc.spout_receive", "builder", {}, {}}
	};
	Group group{"builder", "builder"};
	group.OwnerNodeId = "builder";
	document.Groups = {group};
	document.Links = {{"live", "surface", "layer", "surface"}};
	document.Outputs = {{"recipe", "builder", "dynamic_builder"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = &provider;
	EvaluatedValue result;
	const auto status = EvaluateValue(document, plan, "recipe", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(provider.Calls == 1);
	const auto *recipe = std::get_if<DynamicSurfaceValue>(&result.Data);
	REQUIRE(recipe);
	REQUIRE(recipe->Data);
	REQUIRE(recipe->Data->HostCaptures.size() == 1);
	Image resized;
	const auto raster =
		engine::imagegraph::detail::RasterizePixelBuilder(*recipe, {2, 2}, resized, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(raster == Status::Ok);
	CHECK(provider.Calls == 1);
	REQUIRE(resized.Width == 2);
	REQUIRE(resized.Height == 2);
	CHECK(
		engine::imagegraph::detail::ReadPixel(resized, 0, 0) ==
		engine::imagegraph::detail::ReadPixel(recipe->Data->HostCaptures.front().Images.front().Data, 0, 0)
	);
}

TEST_CASE("Blend Edge preserves mapped axis mixing and channel selection", "[imagegraph][source_2d]") {
	const Image source =
		imagegraph_test::MakeImage(2, 2, {0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 255, 255, 0, 255});
	const auto horizontal =
		RunNode("pc.blend_edge", {{"surface_in", &source}}, {{"types", EnumValue{1}}, {"width", 1.0}});
	REQUIRE(horizontal.Ok);
	CHECK(
		horizontal.Output().Pixels ==
		std::vector<uint8_t>{128, 0, 0, 255, 128, 0, 0, 255, 128, 255, 0, 255, 128, 255, 0, 255}
	);
	const auto both = RunNode("pc.blend_edge", {{"surface_in", &source}}, {{"width", 1.0}});
	REQUIRE(both.Ok);
	CHECK(
		both.Output().Pixels ==
		std::vector<uint8_t>{128, 128, 0, 255, 128, 128, 0, 255, 128, 128, 0, 255, 128, 128, 0, 255}
	);
	const Image widthMap = imagegraph_test::MakeImage(1, 1, {255, 255, 255, 0});
	const auto mapped = RunNode(
		"pc.blend_edge",
		{{"surface_in", &source}, {"width_map", &widthMap}},
		{{"types", EnumValue{1}},
		 {"width_mapped", true},
		 {"width_map_range", Vector2{.1, 1}},
		 {"channel", int64_t{2}}}
	);
	REQUIRE(mapped.Ok);
	CHECK(mapped.Output().Pixels == source.Pixels);
}
