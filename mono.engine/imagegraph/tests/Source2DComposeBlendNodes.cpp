#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.source_compose_blend")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Image Solid(uint32_t width, uint32_t height, std::array<uint8_t, 4> rgba) {
		Image image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(size_t(width) * height * 4);
		for (size_t at = 0; at < image.Pixels.size(); at += 4)
			for (size_t c = 0; c < 4; ++c)
				image.Pixels[at + c] = rgba[c];
		return image;
	}
	Document Graph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"back",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{2, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{64, 128, 192, 192}}}},
			{"fore",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{200, 100, 50, 128}}}},
			{"blend", "pc.blend", "", {}, {{"fill_mode", EnumValue{1}}, {"mask_feather", 0.}}}
		};
		doc.Links = {
			{"back", "surface_out", "blend", "background"}, {"fore", "surface_out", "blend", "foreground"}
		};
		doc.Outputs = {{"out", "blend", "surface_out"}};
		return doc;
	}
}
TEST_CASE(
	"Source Blend twenty-five shader modes retain recorded arithmetic goldens", "[imagegraph][source_2d]"
) {
	const auto back = Solid(1, 1, {64, 128, 192, 192}), fore = Solid(1, 1, {200, 100, 50, 128}),
			   mask = Solid(1, 1, {255, 255, 255, 128});
	struct Golden {
		int64_t Mode;
		std::array<uint8_t, 4> Pixel;
	};
	constexpr std::array cases{
		Golden{0, {96, 121, 158, 204}},	  Golden{1, {115, 117, 139, 168}},	Golden{3, {67, 113, 153, 156}},
		Golden{8, {108, 144, 193, 240}},  Golden{9, {105, 157, 222, 204}},	Golden{11, {75, 128, 192, 192}},
		Golden{22, {25, 103, 172, 156}},  Golden{4, {43, 22, 58, 40}},		Golden{5, {41, 51, 96, 76}},
		Golden{6, {64, 38, 19, 48}},	  Golden{10, {223, 211, 247, 194}}, Golden{13, {114, 134, 131, 102}},
		Golden{14, {93, 150, 199, 158}},  Golden{15, {106, 141, 158, 125}}, Golden{16, {0, 255, 255, 255}},
		Golden{17, {182, 121, 131, 103}}, Golden{18, {80, 160, 175, 138}},	Golden{20, {91, 90, 173, 216}},
		Golden{21, {150, 160, 222, 178}}, Golden{23, {92, 255, 255, 255}},	Golden{25, {101, 141, 191, 192}},
		Golden{26, {70, 144, 219, 192}},  Golden{27, {72, 145, 219, 192}},	Golden{29, {0, 0, 0, 0}},
		Golden{30, {96, 96, 96, 96}}
	};
	for (const auto &expected : cases) {
		const auto run = RunNode(
			"pc.blend",
			{{"background", &back}, {"foreground", &fore}, {"mask", &mask}},
			{{"blend_mode", EnumValue{expected.Mode}}, {"opacity", .75}, {"mask_feather", 0.}}
		);
		INFO(expected.Mode);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels == std::vector<uint8_t>(expected.Pixel.begin(), expected.Pixel.end()));
	}
}
TEST_CASE(
	"Source Blend all output dimension policies and half-even constant bounds", "[imagegraph][source_2d]"
) {
	const auto back = Solid(2, 1, {64, 64, 64, 255}), fore = Solid(1, 3, {128, 128, 128, 255}),
			   mask = Solid(4, 2, {255, 255, 255, 255});
	constexpr std::array expected{Vector2{2, 1}, Vector2{1, 3}, Vector2{4, 2}, Vector2{4, 3}, Vector2{2, 4}};
	for (int64_t dim = 0; dim < 5; ++dim) {
		auto run = RunNode(
			"pc.blend",
			{{"background", &back}, {"foreground", &fore}, {"mask", &mask}},
			{{"output_dimension", EnumValue{dim}},
			 {"constant_dimension", Vector2{2.5, 3.5}},
			 {"mask_feather", 0.},
			 {"fill_mode", EnumValue{1}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Width == expected[dim].X);
		CHECK(run.Output().Height == expected[dim].Y);
	}
}
TEST_CASE(
	"Source Blend None clips centered foreground while Stretch and Tile fill", "[imagegraph][source_2d]"
) {
	const auto back = Solid(3, 1, {0, 0, 0, 255}), fore = Solid(1, 1, {255, 0, 0, 255});
	for (int64_t fill = 0; fill < 3; ++fill) {
		auto run = RunNode(
			"pc.blend", {{"background", &back}, {"foreground", &fore}}, {{"fill_mode", EnumValue{fill}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Pixels[4] == 255);
		CHECK(run.Output().Pixels[0] == (fill ? 255 : 0));
		CHECK(run.Output().Pixels[8] == (fill ? 255 : 0));
	}
}
TEST_CASE(
	"Source Blend masked amount is applied once and preserve-alpha keeps background",
	"[imagegraph][source_2d]"
) {
	const auto back = Solid(1, 1, {0, 0, 0, 64}), fore = Solid(1, 1, {255, 0, 0, 255}),
			   mask = Solid(1, 1, {255, 255, 255, 128});
	auto run = RunNode(
		"pc.blend",
		{{"background", &back}, {"foreground", &fore}, {"mask", &mask}},
		{{"blend_mode", EnumValue{1}}, {"preserve_alpha", true}, {"mask_feather", 0.}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 0, 0, 64});
}
TEST_CASE(
	"Source Blend alpha-only and inverted mask retain source temporary conversion", "[imagegraph][source_2d]"
) {
	const auto back = Solid(1, 1, {0, 0, 0, 255}), fore = Solid(1, 1, {255, 0, 0, 255}),
			   mask = Solid(1, 1, {0, 0, 0, 64});
	auto run = RunNode(
		"pc.blend",
		{{"background", &back}, {"foreground", &fore}, {"mask", &mask}},
		{{"blend_mode", EnumValue{1}}, {"mask_alpha_only", true}, {"invert_mask", true}, {"mask_feather", 0.}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels[0] == 191);
}
TEST_CASE(
	"Source Blend safe single-channel background draw replaces blend shader", "[imagegraph][source_2d]"
) {
	Image back{1, 1, {64}, 0, SurfaceFormat::R8Unorm};
	const auto fore = Solid(1, 1, {255, 0, 0, 255});
	auto run = RunNode(
		"pc.blend", {{"background", &back}, {"foreground", &fore}}, {{"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{64, 64, 64, 255});
}
TEST_CASE(
	"Source Blend stretch single-channel foreground replaces sampling shader", "[imagegraph][source_2d]"
) {
	const auto back = Solid(2, 1, {0, 0, 0, 255});
	Image fore{1, 1, {64}, 0, SurfaceFormat::R8Unorm};
	auto run =
		RunNode("pc.blend", {{"background", &back}, {"foreground", &fore}}, {{"fill_mode", EnumValue{1}}});
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == Solid(2, 1, {64, 64, 64, 255}).Pixels);
}
TEST_CASE(
	"Source Blend missing foreground copies and Swap with missing background clears",
	"[imagegraph][source_2d]"
) {
	const auto back = Solid(2, 1, {64, 128, 192, 128});
	auto copy = RunNode("pc.blend", {{"background", &back}});
	REQUIRE(copy.Ok);
	CHECK(copy.Output().Pixels == back.Pixels);
	auto swap = RunNode("pc.blend", {{"background", &back}}, {{"swap", true}});
	REQUIRE(swap.Ok);
	CHECK(swap.Output().Pixels == std::vector<uint8_t>(4));
}
TEST_CASE(
	"Source Blend inactive ignores blend controls and returns original background", "[imagegraph][source_2d]"
) {
	const auto back = Solid(2, 1, {64, 128, 192, 128}), fore = Solid(1, 1, {255, 0, 0, 255});
	auto run = RunNode(
		"pc.blend",
		{{"background", &back}, {"foreground", &fore}},
		{{"active", false}, {"swap", true}, {"blend_mode", EnumValue{2}}}
	);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == back.Pixels);
}
TEST_CASE(
	"Source Blend undefined shader divisions and separators diagnose selected equation",
	"[imagegraph][source_2d]"
) {
	const auto clear = Solid(1, 1, {0, 0, 0, 0});
	auto divide =
		RunNode("pc.blend", {{"background", &clear}, {"foreground", &clear}}, {{"blend_mode", EnumValue{3}}});
	CHECK_FALSE(divide.Ok);
	CHECK(divide.Code == Status::UnsupportedExecution);
	auto separator =
		RunNode("pc.blend", {{"background", &clear}, {"foreground", &clear}}, {{"blend_mode", EnumValue{2}}});
	CHECK_FALSE(separator.Ok);
}
TEST_CASE(
	"Source Blend compiled graph repeats deterministically and byte refusal is atomic",
	"[imagegraph][source_2d]"
) {
	auto doc = Graph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image result;
	REQUIRE(Evaluate(doc, plan, "out", {}, result, diagnostic) == Status::Ok);
	const auto previous = result;
	REQUIRE(Evaluate(doc, plan, "out", {}, result, diagnostic) == Status::Ok);
	CHECK(result == previous);
	CHECK(Evaluate(doc, plan, "out", {}, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result == previous);
}
TEST_CASE("Source Blend actual processor opacity rows retain distinct results", "[imagegraph][source_2d]") {
	auto doc = Graph();
	Node rows{"rows", "pc.array", "", {}, {}, {}};
	rows.DynamicInputs = {{"input_0", ValueType::Scalar, 0.}, {"input_1", ValueType::Scalar, 1.}};
	doc.Nodes.push_back(rows);
	doc.Links.push_back({"rows", "array", "blend", "opacity"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray result;
	const auto status = EvaluateArray(doc, plan, "out", {}, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Images.size() == 2);
	CHECK(result.Images[0].Pixels == Solid(2, 1, {64, 128, 192, 192}).Pixels);
	CHECK(result.Images[1].Pixels[0] == 142);
}
TEST_CASE(
	"Source Blend complete canvas work refusal preserves previous graph output", "[imagegraph][source_2d]"
) {
	auto doc = Graph();
	Plan plan;
	Diagnostic diagnostic;
	Image output;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "out", {}, output, diagnostic) == Status::Ok);
	const auto previous = output;
	doc.Nodes[2].Values.push_back({"output_dimension", EnumValue{4}});
	doc.Nodes[2].Values.push_back({"constant_dimension", Vector2{4096, 4096}});
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(doc, plan, "out", {}, output, diagnostic) == Status::LimitExceeded);
	CHECK(output == previous);
}
TEST_CASE(
	"Source Blend SurfaceAtlas keeps its identity and source original surface metadata",
	"[imagegraph][source_2d]"
) {
	const auto back = Solid(2, 1, {0, 0, 0, 255});
	AtlasValue atlas;
	atlas.Data.emplace();
	atlas.Data->Kind = AtlasKind::SurfaceAtlas;
	atlas.Data->Surface.Data = Solid(1, 1, {255, 0, 0, 255});
	atlas.Data->Dimension = {1, 1};
	atlas.Data->Position = {1, 0};
	atlas.Data->Scale = {2, 3};
	atlas.Data->RotationDegrees = 30;
	auto run = RunNode("pc.blend", {{"background", &back}}, {{"foreground", atlas}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto *value = run.OutputValue("surface_out");
	REQUIRE(value);
	const auto *result = std::get_if<AtlasValue>(value);
	REQUIRE(result);
	REQUIRE(result->Data);
	CHECK(result->Data->Kind == AtlasKind::SurfaceAtlas);
	CHECK(result->Data->Position == Vector2{});
	CHECK(result->Data->Scale == Vector2{2, 3});
	CHECK(result->Data->RotationDegrees == 30);
	CHECK(result->Data->Surface.Data.Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 0, 0, 255});
	CHECK(result->Data->Dimension == Vector2{2, 1});
}
TEST_CASE("Source Blend floating replacement retains HDR source values", "[imagegraph][source_2d]") {
	Image back{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float}, fore = back;
	REQUIRE(StoreSurfacePixel(back, 0, 0, {-1, 2, 3, .5}));
	REQUIRE(StoreSurfacePixel(fore, 0, 0, {3, 4, -1, 1}));
	auto run = RunNode(
		"pc.blend",
		{{"background", &back}, {"foreground", &fore}},
		{{"blend_mode", EnumValue{1}}, {"opacity", .5}, {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, pixel));
	CHECK(pixel == SurfacePixel{1, 3, 1, .75});
}
TEST_CASE(
	"Source Blend Gaussian mask fractional unobserved uniform is diagnosed", "[imagegraph][source_2d]"
) {
	const auto back = Solid(1, 1, {0, 0, 0, 255}), fore = Solid(1, 1, {255, 0, 0, 255}),
			   mask = Solid(1, 1, {255, 255, 255, 255});
	auto run = RunNode(
		"pc.blend", {{"background", &back}, {"foreground", &fore}, {"mask", &mask}}, {{"mask_feather", 2.25}}
	);
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == Status::UnsupportedExecution);
	CHECK(run.Port == "mask_feather");
	auto exact = RunNode(
		"pc.blend", {{"background", &back}, {"foreground", &fore}, {"mask", &mask}}, {{"mask_feather", 1.}}
	);
	REQUIRE(exact.Ok);
	CHECK(exact.Output().Pixels == fore.Pixels);
}
TEST_CASE(
	"Source Blend selected SurfaceAtlas image and downstream Get share owned pixels",
	"[imagegraph][source_2d]"
) {
	for (const std::string_view route : {"junction", "producer", "instance"}) {
		INFO(route);
		auto doc = Graph();
		AtlasValue atlas;
		atlas.Data.emplace();
		atlas.Data->Kind = AtlasKind::SurfaceAtlas;
		atlas.Data->Surface.Data = Solid(1, 1, {255, 0, 0, 255});
		atlas.Data->Dimension = {1, 1};
		atlas.Data->Position = {1, 0};
		doc.Junctions.push_back({"atlas", "", ValueType::Atlas, atlas});
		doc.Links[1] = {"atlas", "value", "blend", "foreground"};
		doc.Nodes[2].Values[0].Data = EnumValue{0};
		std::string selected = "blend";
		if (route == "producer") {
			doc.Nodes.push_back({"carrier", "pc.atlas_set", "", {}, {}});
			doc.Links[1] = {"carrier", "atlas", "blend", "foreground"};
			doc.Links.push_back({"atlas", "value", "carrier", "input_0"});
		} else if (route == "instance") {
			Node instance{"instance", "pc.blend", "", {}, {}};
			instance.InstanceBase = "blend";
			doc.Nodes.push_back(std::move(instance));
			selected = "instance";
			doc.Outputs[0].NodeId = selected;
		}
		Plan plan;
		Diagnostic diagnostic;
		auto status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Image preview;
		status = Evaluate(doc, plan, "out", {}, preview, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const std::vector<uint8_t> expected{64, 128, 192, 192, 255, 0, 0, 255};
		CHECK(preview.Pixels == expected);
		EvaluatedValue typed;
		status = EvaluateValue(doc, plan, "out", {}, typed, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto *typedAtlas = std::get_if<AtlasValue>(&typed.Data);
		REQUIRE(typedAtlas);
		REQUIRE(typedAtlas->Data);
		CHECK(typedAtlas->Data->Kind == AtlasKind::SurfaceAtlas);
		CHECK(typedAtlas->Data->Surface.Data.Pixels == expected);
		CHECK(typedAtlas->Data->Position == Vector2{0, 0});
		const auto retainedTyped = typed;
		Image repeated;
		status = Evaluate(doc, plan, "out", {}, repeated, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(repeated == preview);
		CHECK(typed == retainedTyped);
		CHECK(std::get<AtlasValue>(*doc.Junctions[0].Default) == atlas);
		for (const auto &resolved : plan.ResolvedInputs)
			if (const auto *stored = std::get_if<AtlasValue>(&resolved.Data)) CHECK(*stored == atlas);
		doc.Nodes.push_back({"get", "pc.atlas_get", "", {}, {}});
		doc.Links.push_back({selected, "surface_out", "get", "input_0"});
		doc.Outputs.push_back({"get", "get", "surface"});
		status = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		Image downstream;
		status = Evaluate(doc, plan, "get", {}, downstream, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(downstream.Pixels == expected);
		const auto previous = preview;
		CHECK(Evaluate(doc, plan, "out", {}, preview, diagnostic, 1) == Status::LimitExceeded);
		CHECK(preview == previous);
		Document restored;
		REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		status = Evaluate(restored, plan, "get", {}, downstream, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ": " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(downstream.Pixels == expected);
	}
}
