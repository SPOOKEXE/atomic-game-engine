#include "../src/AtlasPayload.hpp"
#include "../src/nodes/Processor.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_atlas_nodes")
using namespace engine::imagegraph;
namespace {
	AtlasValue AtlasFixture(AtlasKind kind = AtlasKind::SurfaceAtlas) {
		AtlasValue a;
		auto &d = a.Data.emplace();
		d.Kind = kind;
		d.Surface.Data = {2, 1, {255, 0, 0, 255, 0, 255, 0, 255}, 701};
		d.Position = {3, 4};
		d.Scale = {1, 1};
		d.Dimension = {2, 1};
		d.Alpha = .75;
		d.OriginalSurface = SurfaceValue{Image{8, 8, std::vector<uint8_t>(256), 702}};
		d.OriginalDimension = {8, 8};
		return a;
	}
	Document AtlasGraph(std::string type, std::string input = "input_0") {
		Document d;
		d.FormatVersion = 9;
		d.Junctions.push_back({"atlas", "", ValueType::Atlas, AtlasFixture()});
		d.Nodes = {{"node", type, "", {}, {}}};
		d.Links = {{"atlas", "value", "node", input}};
		return d;
	}
	void AtlasControl(Node &node, std::string port, Value value) {
		for (auto &v : node.Values)
			if (v.Port == port) {
				v.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::move(port), std::move(value)});
	}
	Value AtlasValueOutput(Document &d, std::string port) {
		d.Outputs = {{"out", "node", port}};
		Plan plan;
		Diagnostic error;
		auto status = Compile(d, plan, error);
		INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
		REQUIRE(status == Status::Ok);
		EvaluatedValue v;
		status = EvaluateValue(d, plan, "out", {}, v, error);
		INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
		REQUIRE(status == Status::Ok);
		return v.Data;
	}
	Image AtlasImageOutput(Document &d, std::string port) {
		d.Outputs = {{"out", "node", port}};
		Plan plan;
		Diagnostic error;
		auto status = Compile(d, plan, error);
		INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
		REQUIRE(status == Status::Ok);
		Image image;
		status = Evaluate(d, plan, "out", image, error);
		INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
}
TEST_CASE(
	"Atlas Set keeps ordered source transform controls old dimensions and copy ownership",
	"[source_atlas_nodes]"
) {
	auto document = AtlasGraph("pc.atlas_set");
	auto &node = document.Nodes[0];
	node.Values = {
		{"set_position", true},
		{"mode", EnumValue{1}},
		{"position", Vector2{2, 3}},
		{"set_rotation", true},
		{"rotation", 90.0},
		{"set_scale", true},
		{"scale", Vector2{2, 3}},
		{"set_blending", true},
		{"mode_4", EnumValue{1}},
		{"blend", Colour{17, 33, 65, 129}},
		{"set_alpha", true},
		{"mode_5", EnumValue{2}},
		{"alpha", .5}
	};
	auto output = AtlasValueOutput(document, "atlas");
	const auto &atlas = std::get<AtlasValue>(output);
	REQUIRE(atlas.Data);
	CHECK(atlas.Data->Position.X == Catch::Approx(4.5));
	CHECK(atlas.Data->Position.Y == Catch::Approx(7.5));
	CHECK((atlas.Data->Scale == Vector2{2, 3}));
	CHECK(atlas.Data->RotationDegrees == 90);
	CHECK((atlas.Data->Blend == Colour{17, 33, 65, 129}));
	CHECK(atlas.Data->Alpha == .375);
	auto preserved = AtlasFixture();
	CHECK(std::get<AtlasValue>(*document.Junctions[0].Default).Data->Position == preserved.Data->Position);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(AtlasValueOutput(restored, "atlas") == output);
	document.Junctions[0].Default = AtlasFixture(AtlasKind::Atlas);
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue retained;
	retained.Data = output;
	CHECK(EvaluateValue(document, plan, "out", {}, retained, diagnostic) == Status::UnsupportedExecution);
	CHECK(retained.Data == output);
	document.Junctions[0].Default = AtlasFixture();
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	document.Nodes.push_back({"get", "pc.atlas_get", "", {}, {}});
	document.Links.push_back({"node", "atlas", "get", "input_0"});
	document.Outputs = {{"image", "get", "surface"}};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image pixels{1, 1, {1, 2, 3, 4}, 0};
	const auto original = pixels;
	CHECK(Evaluate(document, plan, "image", {}, pixels, diagnostic, 128) == Status::LimitExceeded);
	CHECK(pixels == original);
}
TEST_CASE(
	"Atlas Get and Struct expose unflattened draw fields and original surface", "[source_atlas_nodes]"
) {
	auto document = AtlasGraph("pc.atlas_get");
	CHECK(std::get<Vector2>(AtlasValueOutput(document, "position")) == Vector2{3, 4});
	CHECK(std::get<double>(AtlasValueOutput(document, "alpha")) == .75);
	auto expectedImage = AtlasFixture().Data->Surface.Data;
	// Evaluated image outputs carry the canonical surface hash, not an authored provenance hash.
	expectedImage.Hash = SurfaceHash(expectedImage);
	CHECK(AtlasImageOutput(document, "surface") == expectedImage);
	document.Nodes[0].Type = "pc.atlas_struct";
	const auto value = AtlasValueOutput(document, "struct");
	const auto &fields = std::get<StructValue>(value).Data->Fields;
	REQUIRE(fields.size() == 7);
	CHECK(fields[0].first == "surface");
	CHECK(std::get<SurfaceValue>(fields[0].second) == AtlasFixture().Data->Surface);
	CHECK(std::get<Vector2>(fields[1].second) == Vector2{2, 1});
	CHECK(std::get<Vector2>(fields[2].second) == Vector2{3, 4});
}
TEST_CASE(
	"Draw Atlas preserves ordered source SurfaceAtlas branches padding and combine arrays",
	"[source_atlas_nodes]"
) {
	auto document = AtlasGraph("pc.atlas_draw", "input_1");
	auto a = AtlasFixture();
	a.Data->Position = {0, 0};
	a.Data->Alpha = 1;
	document.Junctions[0].Default = a;
	document.Nodes[0].Values = {
		{"use_base_dimension", false},
		{"dimension", Vector2{3, 1}},
		{"dimension_unit", EnumValue{0}},
		{"padding", Vector4{1, 0, 1, 0}},
		{"interpolate", EnumValue{1}}
	};
	auto image = AtlasImageOutput(document, "surface");
	REQUIRE(image.Width == 5);
	CHECK(detail::ReadPixel(image, 1, 0)[0] == 1);
	CHECK(detail::ReadPixel(image, 2, 0)[1] == 1);
	auto b = a;
	b.Data->Position = {1, 0};
	b.Data->Blend = {0, 0, 255, 255};
	b.Data->Surface.Data = {1, 1, {255, 255, 255, 255}, 0};
	document.Junctions[0].Type = ValueType::Array;
	document.Junctions[0].Default = ArrayValue{ValueType::Atlas, {a, b}};
	image = AtlasImageOutput(document, "surface");
	CHECK(detail::ReadPixel(image, 2, 0)[2] == 1);
	CHECK(detail::ReadPixel(image, 2, 0)[1] == 0);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(AtlasImageOutput(restored, "surface") == image);
	document.Nodes[0].Values.push_back({"combine", false});
	document.Outputs = {{"out", "node", "surface"}};
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	ImageArray individual;
	REQUIRE(EvaluateArray(document, plan, "out", {}, individual, diagnostic) == Status::Ok);
	REQUIRE(individual.Images.size() == 2);
	CHECK(detail::ReadPixel(individual.Images[0], 2, 0)[1] == 1);
	CHECK(detail::ReadPixel(individual.Images[1], 2, 0)[2] == 1);
}
TEST_CASE(
	"Atlas Affector applies uniform position scale alpha and typed interpolation without pixel mutation",
	"[source_atlas_nodes]"
) {
	auto document = AtlasGraph("pc.atlas_affector", "atlas_in");
	document.Junctions[0].Type = ValueType::Array;
	document.Junctions[0].Default = ArrayValue{ValueType::Atlas, {AtlasFixture()}};
	document.Nodes[0].Values = {
		{"seed", 0.0},
		{"influence_shape", EnumValue{4}},
		{"influence", .5},
		{"effect_position", true},
		{"mode", EnumValue{1}},
		{"position", Vector2{8, 4}},
		{"axis", int64_t{1}},
		{"set_scale", true},
		{"scale", Vector2{3, 5}},
		{"axis_2", int64_t{2}},
		{"set_alpha", true},
		{"alpha", .25}
	};
	auto value = AtlasValueOutput(document, "atlas_out");
	const auto &array = std::get<ArrayValue>(value);
	REQUIRE(array.Items.size() == 1);
	const auto &a = std::get<AtlasValue>(std::get<ElementValue>(array.Items[0].Data));
	CHECK((a.Data->Position == Vector2{7, 3}));
	CHECK((a.Data->Scale == Vector2{1, 3}));
	CHECK(a.Data->Alpha == .5);
	CHECK(a.Data->Surface == AtlasFixture().Data->Surface);
	auto target = AtlasFixture();
	target.Data->Position = {11, 13};
	target.Data->Scale = {5, 5};
	target.Data->RotationDegrees = 120;
	document.Junctions.push_back({"target", "", ValueType::Array, ArrayValue{ValueType::Atlas, {target}}});
	document.Links.push_back({"target", "value", "node", "target_atlas"});
	AtlasControl(document.Nodes[0], "interpolate", true);
	value = AtlasValueOutput(document, "atlas_out");
	const auto &b = std::get<AtlasValue>(std::get<ElementValue>(std::get<ArrayValue>(value).Items[0].Data));
	CHECK((b.Data->Position == Vector2{9, 8}));
	CHECK((b.Data->Scale == Vector2{3, 4}));
	CHECK(b.Data->RotationDegrees == 60);
}
TEST_CASE(
	"Pixel Expand preserves radial opaque selection scan priority and directional controls",
	"[source_atlas_nodes]"
) {
	const auto input =
		imagegraph_test::MakeImage(4, 1, {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 255, 128, 0, 255, 0, 255});
	const auto scan = imagegraph_test::RunNode(
		"pc.atlas",
		{{"surface_in", &input}},
		{{"method", EnumValue{1}}, {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(scan.Message);
	REQUIRE(scan.Ok);
	CHECK(detail::ReadPixel(scan.Output(), 1, 0)[2] == 1);
	CHECK(detail::ReadPixel(scan.Output(), 1, 0)[3] == Catch::Approx(128.0 / 255));
	const auto radial = imagegraph_test::RunNode(
		"pc.atlas", {{"surface_in", &input}}, {{"method", EnumValue{0}}, {"resolution", int64_t{2}}}
	);
	INFO(radial.Message);
	REQUIRE(radial.Ok);
	CHECK(detail::ReadPixel(radial.Output(), 1, 0)[0] == 1);
	CHECK(detail::ReadPixel(radial.Output(), 2, 0)[1] == 1);
	const auto linear = imagegraph_test::RunNode(
		"pc.atlas",
		{{"surface_in", &input}},
		{{"method", EnumValue{2}}, {"direction", 180.0}, {"both_side", false}}
	);
	INFO(linear.Message);
	REQUIRE(linear.Ok);
	CHECK(detail::ReadPixel(linear.Output(), 1, 0)[0] == 1);
	CHECK(linear.Output().Pixels[0] == 255);
	CHECK(input.Pixels[4] == 0);
	const auto undefined = imagegraph_test::RunNode(
		"pc.atlas", {{"surface_in", &input}}, {{"method", EnumValue{0}}, {"resolution", int64_t{0}}}
	);
	CHECK_FALSE(undefined.Ok);
	CHECK(undefined.Code == Status::UnsupportedExecution);
	const auto bounded = imagegraph_test::RunNode(
		"pc.atlas", {{"surface_in", &input}}, {{"method", EnumValue{0}}, {"resolution", int64_t{1000000}}}
	);
	CHECK_FALSE(bounded.Ok);
	CHECK(bounded.Code == Status::LimitExceeded);
	// A single row admits about 32.2 million worst-case samples; two exceed the batch cap.
	Document batch;
	batch.FormatVersion = 9;
	batch.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{4}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 0}}}},
		{"rows", "pc.array", "", {}, {{"type", EnumValue{1}}}},
		{"expand", "pc.atlas", "", {}, {{"method", EnumValue{0}}, {"resolution", int64_t{20000}}}}
	};
	batch.Nodes[1].DynamicInputs = {
		{"input_0", ValueType::Image, std::nullopt}, {"input_1", ValueType::Image, std::nullopt}
	};
	batch.Links = {
		{"source", "image", "rows", "input_0"},
		{"source", "image", "rows", "input_1"},
		{"rows", "array", "expand", "surface_in"}
	};
	batch.Outputs = {{"out", "expand", "surface_out"}};
	Plan batchPlan;
	Diagnostic error;
	const auto compiled = Compile(batch, batchPlan, error);
	INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray preserved;
	preserved.Images.push_back(input);
	preserved.Items.push_back({size_t{0}});
	const auto before = preserved;
	const auto rejected = EvaluateArray(batch, batchPlan, "out", {}, preserved, error);
	INFO(error.NodeId << ":" << error.Port << ": " << error.Message);
	CHECK(rejected == Status::LimitExceeded);
	CHECK(error.Port == "resolution");
	CHECK(preserved.Images == before.Images);
	REQUIRE(preserved.Items.size() == before.Items.size());
	CHECK(std::get<size_t>(preserved.Items.front().Data) == size_t{0});
}
TEST_CASE(
	"Atlas Affector influence shapes preserve source geometric and map quirks", "[source_atlas_nodes]"
) {
	auto document = AtlasGraph("pc.atlas_affector", "atlas_in");
	auto input = AtlasFixture();
	input.Data->Position = {0, 0};
	input.Data->Dimension = {2, 1};
	document.Junctions[0].Type = ValueType::Array;
	document.Junctions[0].Default = ArrayValue{ValueType::Atlas, {input}};
	document.Nodes[0].Values = {
		{"seed", 0.0},
		{"use_base_dimension", false},
		{"dimension", Vector2{8, 8}},
		{"dimension_unit", EnumValue{0}},
		{"set_alpha", true},
		{"alpha", 1.0},
		{"area_unit", EnumValue{0}},
		{"area", Area{1, .5, 2, 2, 1, 0}},
		{"falloff", 1.0},
		{"influence_shape", EnumValue{0}}
	};
	const auto alpha = [&]() {
		const auto v = AtlasValueOutput(document, "atlas_out");
		return std::get<AtlasValue>(std::get<ElementValue>(std::get<ArrayValue>(v).Items[0].Data))
			.Data->Alpha;
	};
	CHECK(alpha() == 1);
	AtlasControl(document.Nodes[0], "area", Area{1, .5, 2, 2, 0, 0});
	CHECK(alpha() == 1);
	AtlasControl(document.Nodes[0], "influence_shape", EnumValue{1});
	AtlasControl(document.Nodes[0], "wipe_origin", Vector2{1, .5});
	AtlasControl(document.Nodes[0], "wipe_origin_unit", EnumValue{0});
	CHECK(alpha() == .875);
	AtlasControl(document.Nodes[0], "influence_shape", EnumValue{3});
	AtlasControl(document.Nodes[0], "order_index", 1.0);
	CHECK(alpha() == .875);
	document.Nodes.push_back(
		{"map",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	document.Links.push_back({"map", "image", "node", "influence_map"});
	AtlasControl(document.Nodes[0], "influence_shape", EnumValue{2});
	CHECK(alpha() == Catch::Approx(1.0275));
	// Source colorBrightness weights blue by .224, so white influence sums to 1.11.
	AtlasControl(document.Nodes[0], "influence_shape", EnumValue{4});
	AtlasControl(document.Nodes[0], "influence", .5);
	AtlasControl(document.Nodes[0], "inf_noise", .8);
	const auto first = alpha();
	CHECK(alpha() == first);
	CHECK(first >= .75);
	CHECK(first <= 1);
	CHECK(first == Catch::Approx(.9161189206965488).margin(1e-14));
	document.Junctions[0].Default = ArrayValue{ValueType::Atlas, {input, input, input}};
	const auto noiseRows = AtlasValueOutput(document, "atlas_out");
	const std::array<double, 3> html5Influences{.6644756827861954, .4365527119638606, .29732551013645886};
	for (size_t i = 0; i < 3; i++) {
		const auto &atlas =
			std::get<AtlasValue>(std::get<ElementValue>(std::get<ArrayValue>(noiseRows).Items[i].Data));
		CHECK(atlas.Data->Alpha == Catch::Approx(.75 + .25 * html5Influences[i]).margin(1e-14));
	}

	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(AtlasValueOutput(restored, "atlas_out") == AtlasValueOutput(document, "atlas_out"));
}
