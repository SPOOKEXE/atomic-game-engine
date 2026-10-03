#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraph.source_stripe")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	Document StripeGraph(Vector2 dimension = {8, 4}) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"stripe",
			 "pc.stripe",
			 "",
			 {},
			 {{"dimension", dimension},
			  {"dimension_unit", EnumValue{0}},
			  {"size", 3.0},
			  {"size_unit", EnumValue{0}},
			  {"seed", 17.0},
			  {"attribute_color_depth", EnumValue{3}}}}
		};
		d.Outputs = {{"out", "stripe", "surface_out"}};
		return d;
	}
	void Set(Document &d, std::string port, Value value) {
		for (auto &item : d.Nodes[0].Values)
			if (item.Port == port) {
				item.Data = std::move(value);
				return;
			}
		d.Nodes[0].Values.push_back({std::move(port), std::move(value)});
	}
	Image Draw(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto compiled = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(compiled == Status::Ok);
		Image image;
		const auto status = Evaluate(d, p, "out", {}, image, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void Row(const Image &image, std::initializer_list<int> values, size_t y = 0) {
		REQUIRE(image.Width == values.size());
		size_t x = 0;
		for (auto value : values) {
			for (size_t c = 0; c < 3; ++c)
				CHECK(image.Pixels[(y * image.Width + x) * 4 + c] == value);
			CHECK(image.Pixels[(y * image.Width + x) * 4 + 3] == 255);
			++x;
		}
	}
	Node Range(std::string id, Value a, Value b, ValueType type = ValueType::Scalar) {
		Node n{std::move(id), "pc.array", "", {}, {}, {}};
		n.DynamicInputs = {{"input_0", type, std::move(a)}, {"input_1", type, std::move(b)}};
		return n;
	}
}
TEST_CASE("Stripe Solid preserves aspect ratio and the ratio epsilon", "[source_stripe]") {
	auto d = StripeGraph();
	const auto image = Draw(d);
	for (size_t y = 0; y < 4; ++y)
		Row(image, {0, 255, 0, 0, 255, 0, 0, 255}, y);
	Set(d, "strip_ratio", .499);
	Row(Draw(d), {0, 255, 255, 0, 255, 255, 0, 255});
}
TEST_CASE("Stripe Smooth retains shader sine phase", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "type", EnumValue{1});
	Row(Draw(d), {17, 238, 128, 17, 238, 128, 17, 238});
}
TEST_CASE("Stripe AA uses canvas maximum dimension for smoothstep", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "type", EnumValue{2});
	Row(Draw(d), {0, 255, 128, 0, 255, 128, 0, 255});
}
TEST_CASE("Stripe rotation samples vertical phase with the source sign", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "angle", 90.0);
	const auto image = Draw(d);
	Row(image, {0, 0, 0, 0, 0, 0, 0, 0}, 0);
	Row(image, {255, 255, 255, 255, 255, 255, 255, 255}, 1);
	Row(image, {0, 0, 0, 0, 0, 0, 0, 0}, 2);
	Row(image, {0, 0, 0, 0, 0, 0, 0, 0}, 3);
}
TEST_CASE("Stripe Palette Smooth retains unnormalized interpolation and negative slots", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "coloring", EnumValue{1});
	Row(Draw(d), {0, 0, 0, 0, 0, 0, 0, 0});
	Set(d, "type", EnumValue{2});
	Row(Draw(d), {0, 0, 0, 0, 0, 0, 0, 0});
	Set(d, "type", EnumValue{1});
	Row(Draw(d), {85, 85, 0, 85, 85, 0, 85, 85});
}
TEST_CASE("Stripe Random boundaries use the pinned stateless shader hash", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "random", .8);
	Row(Draw(d), {0, 255, 255, 255, 255, 0, 0, 255});
}
TEST_CASE("Stripe Random gradient hashes selected bands and wraps shift", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "coloring", EnumValue{2});
	Set(d, "colors", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}});
	Set(d, "shift", .25);
	Row(Draw(d), {133, 133, 64, 64, 64, 250, 250, 250});
	Set(d, "shift", 1.25);
	Row(Draw(d), {133, 133, 64, 64, 64, 250, 250, 250});
}
TEST_CASE("Stripe Tiled and Amount are source inert shader uniforms", "[source_stripe]") {
	auto d = StripeGraph();
	const auto prior = Draw(d);
	Set(d, "tiled", true);
	Set(d, "amount", 123.0);
	CHECK(Draw(d) == prior);
}
TEST_CASE("Stripe Reference scalar size and position equal resolved physical controls", "[source_stripe]") {
	auto d = StripeGraph();
	const auto physical = Draw(d);
	Set(d, "size", 3.0 / 8);
	Set(d, "size_unit", EnumValue{1});
	CHECK(Draw(d) == physical);
	Set(d, "position", Vector2{4, 2});
	Set(d, "position_unit", EnumValue{0});
	CHECK(Draw(d) == physical);
}
TEST_CASE("Stripe linked mapped pairs retain each exact source numeric socket", "[source_stripe]") {
	for (const auto &[port, a, b] : std::array{
			 std::tuple{"size", 3.0, 4.0},
			 std::tuple{"angle", 0.0, 90.0},
			 std::tuple{"strip_ratio", .5, .25},
			 std::tuple{"random", 0.0, .8}
		 }) {
		auto d = StripeGraph();
		d.Nodes.push_back(Range("range", a, b));
		d.Nodes.push_back(
			{"map",
			 "pc.solid",
			 "",
			 {},
			 {{"dimension", Vector2{1, 1}},
			  {"dimension_unit", EnumValue{0}},
			  {"color", Colour{255, 255, 255, 0}}}}
		);
		Set(d, std::string(port) + "_mapped", true);
		d.Links = {
			{"range", "array", "stripe", port}, {"map", "surface_out", "stripe", std::string(port) + "_map"}
		};
		auto expected = StripeGraph();
		Set(expected, port, b);
		CHECK(Draw(d) == Draw(expected));
		d.Nodes[2].Values[2].Data = Colour{0, 0, 0, 255};
		Set(expected, port, a);
		CHECK(Draw(d) == Draw(expected));
	}
}
TEST_CASE("Stripe mapped endpoints without a sampler use the first source endpoint", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "size_mapped", true);
	d.Nodes.push_back(Range("range", 3.0, 6.0));
	d.Links = {{"range", "array", "stripe", "size"}};
	CHECK(Draw(d) == Draw(StripeGraph()));
	Set(d, "size_unit", EnumValue{1});
	CHECK(Draw(d) == Draw(StripeGraph()));
}
TEST_CASE("Stripe catalogue fallback mapped size uses explicit synthetic range", "[source_stripe]") {
	auto d = StripeGraph();
	std::erase_if(d.Nodes[0].Values, [](const AuthoredValue &v) { return v.Port == "size"; });
	Set(d, "size_mapped", true);
	Set(d, "size_map_range", Vector2{3, 6});
	Set(d, "size_unit", EnumValue{1});
	CHECK(Draw(d) == Draw(StripeGraph()));
}
TEST_CASE("Stripe mapped gradient samples a filtered range independently of its keys", "[source_stripe]") {
	Image map{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}, 0};
	auto run = RunNode(
		"pc.stripe",
		{{"colors_map", &map}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"position", Vector2{0, 0}},
		 {"position_unit", EnumValue{0}},
		 {"progress", .25},
		 {"coloring", EnumValue{2}},
		 {"colors_mapped", true},
		 {"colors_map_range", Vector4{.5, .5, .5, .5}},
		 {"colors", Gradient{}},
		 {"seed", 17.0},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{128, 0, 128, 255});
}
TEST_CASE("Stripe UV alpha and Mask Empty retain separate alpha multipliers", "[source_stripe]") {
	const Image uv{1, 1, {128, 128, 0, 128}, 0}, mask{1, 1, {128, 128, 128, 128}, 0};
	auto run = RunNode(
		"pc.stripe",
		{{"uv_map", &uv}, {"mask", &mask}},
		{{"dimension", Vector2{1, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"strip_ratio", -1.0},
		 {"uv_mix", 0.0},
		 {"mask_alpha_only", true},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Pixels == std::vector<uint8_t>{255, 255, 255, 32});
}
TEST_CASE("Stripe Mask dimension scales physical canvas before half-even rounding", "[source_stripe]") {
	const Image mask{3, 2, std::vector<uint8_t>(24, 255), 0};
	auto run = RunNode(
		"pc.stripe",
		{{"mask", &mask}},
		{{"dimension", Vector2{1.5, 1.25}},
		 {"dimension_unit", EnumValue{2}},
		 {"size", 1.0},
		 {"size_unit", EnumValue{0}},
		 {"attribute_color_depth", EnumValue{3}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(run.Output().Width == 4);
	CHECK(run.Output().Height == 2);
}
TEST_CASE("Stripe float output and masked single-red drawing preserve source profiles", "[source_stripe]") {
	const Image black{1, 1, {0, 0, 0, 255}, 0};
	for (int64_t depth : {5, 7, 8}) {
		auto run = RunNode(
			"pc.stripe",
			{{"mask", &black}},
			{{"dimension", Vector2{1, 1}},
			 {"dimension_unit", EnumValue{0}},
			 {"size", 1.0},
			 {"size_unit", EnumValue{0}},
			 {"strip_ratio", -1.0},
			 {"color_1", Colour{64, 0, 0, 128}},
			 {"attribute_color_depth", EnumValue{depth}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		SurfacePixel p{};
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, p));
		CHECK(p[0] == Catch::Approx(64 / 255.).margin(.0002));
		CHECK(p[3] == (depth == 5 ? 0 : 1));
	}
}
TEST_CASE("Stripe source divisors refuse atomically with producer attribution", "[source_stripe]") {
	for (const auto field : {"size", "dimension"}) {
		auto d = StripeGraph();
		if (std::string_view(field) == "dimension")
			Set(d, field, Vector2{0, 4});
		else
			Set(d, field, 0.0);
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		Image image{1, 1, {9, 8, 7, 6}, 0};
		const auto prior = image;
		CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::UnsupportedExecution);
		CHECK(diag.NodeId == "stripe");
		CHECK(diag.Port == field);
		CHECK(image == prior);
	}
}
TEST_CASE("Stripe unresolved source seed refuses only stochastic branches", "[source_stripe]") {
	auto d = StripeGraph();
	std::erase_if(d.Nodes[0].Values, [](const AuthoredValue &v) { return v.Port == "seed"; });
	CHECK(Draw(d) == Draw(StripeGraph()));
	for (bool gradient : {false, true}) {
		Set(d, gradient ? "coloring" : "random", gradient ? Value{EnumValue{2}} : Value{.8});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		Image image{1, 1, {9, 8, 7, 6}, 0};
		const auto prior = image;
		CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::UnsupportedExecution);
		CHECK(diag.Port == "seed");
		CHECK(image == prior);
	}
}
TEST_CASE(
	"Stripe empty palette and authored gradient preserve distinct admission boundaries", "[source_stripe]"
) {
	auto d = StripeGraph();
	Set(d, "coloring", EnumValue{1});
	Set(d, "colors_2", ArrayValue{ValueType::Colour, {}, {}});
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "colors_2");
	CHECK(image == prior);
	const auto valid = d;
	Set(d, "coloring", EnumValue{2});
	Set(d, "colors", Gradient{});
	CHECK(Compile(d, p, diag) == Status::InvalidValue);
	CHECK(diag.Port == "colors");
	CHECK(Evaluate(valid, p, "out", {}, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "colors_2");
	CHECK(image == prior);
}
TEST_CASE("Stripe persistence and source instance controls regenerate identical pixels", "[source_stripe]") {
	auto d = StripeGraph();
	Set(d, "angle", 30.0);
	Set(d, "random", .5);
	Set(d, "type", EnumValue{1});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Draw(restored) == Draw(d));
	Node instance{"copy", "pc.stripe", "", {}, {}};
	instance.InstanceBase = "stripe";
	d.Nodes.push_back(std::move(instance));
	d.Outputs[0].NodeId = "copy";
	CHECK(Draw(d) == Draw(restored));
}
TEST_CASE("Stripe heterogeneous compiled dimension rows match scalar shader oracles", "[source_stripe]") {
	auto d = StripeGraph();
	d.Nodes.push_back(Range("sizes", Vector2{8, 4}, Vector2{4, 4}, ValueType::Vector2));
	d.Links = {{"sizes", "array", "stripe", "dimension"}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(d, p, "out", {}, images, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	CHECK(images.Images[0] == Draw(StripeGraph({8, 4})));
	auto later = StripeGraph({4, 4});
	Set(later, "position", Vector2{4, 2});
	Set(later, "position_unit", EnumValue{0});
	CHECK(images.Images[1] == Draw(later));
}
TEST_CASE(
	"Stripe later expensive dimension row work refuses before small first allocation", "[source_stripe]"
) {
	auto d = StripeGraph();
	d.Nodes.push_back(Range("sizes", Vector2{1, 1}, Vector2{3000, 3000}, ValueType::Vector2));
	d.Links = {{"sizes", "array", "stripe", "dimension"}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	CHECK(Evaluate(d, p, "out", {}, image, diag, 100000) == Status::LimitExceeded);
	CHECK(diag.NodeId == "stripe");
	CHECK(diag.Message.find("whole-array work") != std::string::npos);
	CHECK(image == prior);
}
TEST_CASE("Stripe output byte admission preserves prior public image", "[source_stripe]") {
	auto d = StripeGraph();
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	CHECK(Evaluate(d, p, "out", {}, image, diag, 1) == Status::LimitExceeded);
	CHECK(image == prior);
}

TEST_CASE("Stripe actual linked filtered gradient map survives native persistence", "[source_stripe]") {
	auto d = StripeGraph({1, 1});
	Set(d, "coloring", EnumValue{2});
	Set(d, "colors_mapped", true);
	Set(d, "colors_map_range", Vector4{.5, .5, .5, .5});
	d.Nodes.push_back(
		{"map",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{11, 22, 33, 128}}}}
	);
	d.Links = {{"map", "surface_out", "stripe", "colors_map"}};
	CHECK(Draw(d).Pixels == std::vector<uint8_t>{11, 22, 33, 128});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Draw(restored) == Draw(d));
}
TEST_CASE("Stripe scalar disabled processing preserves the same shader result", "[source_stripe]") {
	auto d = StripeGraph();
	const auto expected = Draw(d);
	Set(d, "attribute_process", false);
	CHECK(Draw(d) == expected);
}

TEST_CASE(
	"Stripe static source units and mapped attributes preserve every processor schedule", "[source_stripe]"
) {
	auto d = StripeGraph();
	d.Nodes.push_back(Range("sizes", Vector2{8, 4}, Vector2{4, 4}, ValueType::Vector2));
	d.Links = {{"sizes", "array", "stripe", "dimension"}};
	for (int64_t mode = 0; mode <= 3; ++mode) {
		Set(d, "attribute_array_process", EnumValue{mode});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(d, p, "out", {}, images, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		CHECK(images.Images[0] == Draw(StripeGraph({8, 4})));
		auto later = StripeGraph({4, 4});
		Set(later, "position", Vector2{4, 2});
		Set(later, "position_unit", EnumValue{0});
		CHECK(images.Images[1] == Draw(later));
	}
}

TEST_CASE(
	"Stripe Position applies Reference units to numeric links and bypasses surface dimensions",
	"[source_stripe]"
) {
	auto d = StripeGraph();
	d.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", .5}, {"y", .5}}});
	d.Links = {{"position", "vector", "stripe", "position"}};
	CHECK(Draw(d) == Draw(StripeGraph()));
	d.Nodes[1] = {
		"position",
		"pc.solid",
		"",
		{},
		{{"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}},
		 {"color", Colour{255, 255, 255, 255}}}
	};
	d.Links[0].FromPort = "surface_out";
	auto expected = StripeGraph();
	Set(expected, "position", Vector2{2, 1});
	Set(expected, "position_unit", EnumValue{0});
	CHECK(Draw(d) == Draw(expected));
}

TEST_CASE(
	"Stripe batched Reference controls resolve against the first prepared dimension", "[source_stripe]"
) {
	auto d = StripeGraph();
	Set(d, "size", .375);
	Set(d, "size_unit", EnumValue{1});
	d.Nodes.push_back(Range("sizes", Vector2{8, 4}, Vector2{4, 4}, ValueType::Vector2));
	d.Links = {{"sizes", "array", "stripe", "dimension"}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(d, p, "out", {}, images, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	auto a = StripeGraph({8, 4}), b = StripeGraph({4, 4});
	Set(a, "position", Vector2{4, 2});
	Set(b, "position", Vector2{4, 2});
	Set(a, "position_unit", EnumValue{0});
	Set(b, "position_unit", EnumValue{0});
	CHECK(images.Images[0] == Draw(a));
	CHECK(images.Images[1] == Draw(b));
}
TEST_CASE(
	"Stripe selected palette exceeds GLSL uniform capacity without silently truncating", "[source_stripe]"
) {
	auto d = StripeGraph();
	Set(d, "coloring", EnumValue{1});
	ArrayValue palette{ValueType::Colour, {}};
	palette.Elements.assign(257, ElementValue{Colour{1, 2, 3, 255}});
	Set(d, "colors_2", std::move(palette));
	Plan p;
	Diagnostic diag;
	const auto compiled = Compile(d, p, diag);
	INFO(diag.Message);
	REQUIRE(compiled == Status::Ok);
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "colors_2");
	CHECK(image == prior);
	Set(d, "coloring", EnumValue{0});
	CHECK(Draw(d) == Draw(StripeGraph()));
}
TEST_CASE(
	"Stripe defined larger native gradient refuses the selected GLSL key capacity atomically",
	"[source_stripe]"
) {
	auto d = StripeGraph();
	Set(d, "coloring", EnumValue{2});
	Gradient gradient;
	for (size_t i = 0; i <= 64; ++i)
		gradient.Keys.push_back({double(i) / 64, Colour{11, 22, 33, 255}});
	Set(d, "colors", std::move(gradient));
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image image{1, 1, {9, 8, 7, 6}, 0};
	const auto prior = image;
	CHECK(Evaluate(d, p, "out", {}, image, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "colors");
	CHECK(image == prior);
	Set(d, "coloring", EnumValue{0});
	CHECK(Draw(d) == Draw(StripeGraph()));
}

TEST_CASE(
	"Stripe surface Position preserves getter identity through snapshots and instances", "[source_stripe]"
) {
	auto d = StripeGraph();
	d.Nodes.push_back(
		{"position",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}, {"color", Colour{31, 63, 95, 127}}}}
	);
	d.Links = {{"position", "surface_out", "stripe", "position"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	const auto status = EvaluateNodeInputs(d, plan, "stripe", {}, snapshot, diagnostic);
	INFO(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
	REQUIRE(status == Status::Ok);
	bool foundValue = false, foundImage = false;
	for (const auto &value : snapshot.Values())
		if (value.Port == "position") {
			REQUIRE(std::holds_alternative<Vector2>(value.Data));
			CHECK(std::get<Vector2>(value.Data) == Vector2{2, 1});
			CHECK(value.Linked);
			foundValue = true;
		}
	for (const auto &image : snapshot.Images())
		if (image.Port == "position") {
			CHECK(image.Data.Width == 2);
			CHECK(image.Data.Height == 1);
			CHECK(image.Data.Pixels == std::vector<uint8_t>{31, 63, 95, 127, 31, 63, 95, 127});
			foundImage = true;
		}
	REQUIRE(foundValue);
	REQUIRE(foundImage);
	const auto retained = snapshot.RetainedBytes();
	const auto positionImage =
		std::find_if(snapshot.Images().begin(), snapshot.Images().end(), [](const auto &input) {
			return input.Port == "position";
		});
	REQUIRE(positionImage != snapshot.Images().end());
	const auto prior = positionImage->Data;
	CHECK(EvaluateNodeInputs(d, plan, "stripe", {}, snapshot, diagnostic, 1) == Status::LimitExceeded);
	CHECK(snapshot.RetainedBytes() == retained);
	const auto preserved =
		std::find_if(snapshot.Images().begin(), snapshot.Images().end(), [](const auto &input) {
			return input.Port == "position";
		});
	REQUIRE(preserved != snapshot.Images().end());
	CHECK(preserved->Data == prior);
	auto expected = StripeGraph();
	Set(expected, "position", Vector2{2, 1});
	Set(expected, "position_unit", EnumValue{0});
	const auto oracle = Draw(expected);
	CHECK(Draw(d) == oracle);
	Document restored;
	REQUIRE(Read(Write(d), restored, diagnostic) == Status::Ok);
	CHECK(Draw(restored) == oracle);
	Node instance{"copy", "pc.stripe", "", {}, {}};
	instance.InstanceBase = "stripe";
	restored.Nodes.push_back(std::move(instance));
	restored.Outputs[0].NodeId = "copy";
	CHECK(Draw(restored) == oracle);
}
