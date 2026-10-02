#include "NodeHarness.hpp"
#include "nodes/Processor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_bevel")
using namespace engine::imagegraph;
namespace {
	Document BevelGraph(std::string type = "pc.bevel") {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{3}}, {"height", int64_t{3}}, {"colour", Colour{255, 255, 255, 255}}}},
			{"bevel", type, "", {}, {}}
		};
		const std::string input = type == "pc.bevel" ? "surface_in" : "surface";
		doc.Links = {{"source", "image", "bevel", input}};
		doc.Outputs = {{"image", "bevel", "surface_out"}};
		if (type == "pc.bevel")
			doc.Nodes.back().Values = {
				{"height", int64_t{2}}, {"oversample", EnumValue{1}}, {"attribute_color_depth", EnumValue{5}}
			};
		else
			doc.Outputs.push_back({"inner", "bevel", "inner_area"});
		return doc;
	}
	Image BevelEvaluate(Document &doc, std::string id = "image") {
		Plan plan;
		Diagnostic diagnostic;
		const auto compile = Compile(doc, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(compile == Status::Ok);
		Image output;
		const auto status = Evaluate(doc, plan, id, output, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output;
	}
}
TEST_CASE(
	"Source bevel round distance and high resolution remain distinct through persistence", "[source_bevel]"
) {
	auto doc = BevelGraph();
	const auto image = BevelEvaluate(doc);
	REQUIRE(detail::ReadPixel(image, 0, 0)[0] == Catch::Approx(.5).margin(.00001));
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(1));
	doc.Nodes.back().Values.push_back({"highres", true});
	const auto high = BevelEvaluate(doc);
	REQUIRE(high.Pixels != image.Pixels);
	REQUIRE(detail::ReadPixel(high, 0, 0)[0] == Catch::Approx(.3125));
	REQUIRE(detail::ReadPixel(high, 1, 1)[0] == Catch::Approx(.8125));
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(BevelEvaluate(restored).Pixels == high.Pixels);
}
TEST_CASE("Source bevel inactive and mask mix retain original colour", "[source_bevel]") {
	auto doc = BevelGraph();
	doc.Nodes.front().Values.back().Data = Colour{255, 0, 0, 255};
	doc.Outputs.push_back({"source", "source", "image"});
	const auto source = BevelEvaluate(doc, "source");
	REQUIRE(source.Format == SurfaceFormat::RGBA8Unorm);
	doc.Nodes.back().Values.push_back({"mix", 0.0});
	const auto original = BevelEvaluate(doc);
	REQUIRE(original.Format == SurfaceFormat::RGBA32Float);
	for (uint32_t y = 0; y < original.Height; y++)
		for (uint32_t x = 0; x < original.Width; x++)
			REQUIRE(detail::ReadPixel(original, x, y) == detail::Rgba{1, 0, 0, 1});
	doc.Nodes.back().Values.back() = {"active", false};
	const auto inactive = BevelEvaluate(doc);
	REQUIRE(inactive.Format == source.Format);
	REQUIRE(inactive.Width == source.Width);
	REQUIRE(inactive.Height == source.Height);
	REQUIRE(inactive.Pixels == source.Pixels);
}
TEST_CASE(
	"Source Pixel Bevel retains fully occupied image and publishes separate inner area", "[source_bevel]"
) {
	auto doc = BevelGraph("pc.pb_fx_bevel");
	auto image = BevelEvaluate(doc);
	auto inner = BevelEvaluate(doc, "inner");
	REQUIRE(image.Pixels == inner.Pixels);
	REQUIRE(detail::ReadPixel(inner, 1, 1) == detail::Rgba{1, 1, 1, 1});
	doc.Nodes.front().Values.back().Data = Colour{0, 0, 0, 255};
	image = BevelEvaluate(doc);
	inner = BevelEvaluate(doc, "inner");
	REQUIRE(detail::ReadPixel(image, 1, 1) == detail::Rgba{0, 0, 0, 1});
	REQUIRE(detail::ReadPixel(inner, 1, 1) == detail::Rgba{});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(BevelEvaluate(restored, "inner").Pixels == inner.Pixels);
	doc.Nodes.front().Values.back().Data = Colour{255, 255, 255, 128};
	image = BevelEvaluate(doc);
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == Catch::Approx(128 / 255.));
	REQUIRE(detail::ReadPixel(image, 1, 1)[3] == Catch::Approx(64 / 255.));
}
TEST_CASE(
	"Source bevel oversized sweeps and singular scale refuse without replacing caller result",
	"[source_bevel]"
) {
	auto doc = BevelGraph();
	doc.Nodes.back().Values.push_back({"scale", Vector2{0, 1}});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	Image sentinel{1, 1, {3, 7, 11, 13}}, output = sentinel;
	REQUIRE(Evaluate(doc, plan, "image", output, diagnostic) == Status::UnsupportedExecution);
	REQUIRE(output.Pixels == sentinel.Pixels);
	REQUIRE(diagnostic.Port == "scale");
	doc.Nodes.back().Values.back() = {"height", int64_t{2'000'000}};
	doc.Nodes.back().Values.erase(doc.Nodes.back().Values.begin());
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(doc, plan, "image", output, diagnostic) == Status::LimitExceeded);
	REQUIRE(output.Pixels == sentinel.Pixels);
}

TEST_CASE("Source Pixel Bevel gradients and all-side highlight follow staged topology", "[source_bevel]") {
	Image image{5, 5, std::vector<uint8_t>(100, 0)};
	for (uint32_t y = 0; y < 5; y++)
		for (uint32_t x = 0; x < 5; x++)
			detail::WritePixel(
				image,
				x,
				y,
				{x >= 1 && x <= 3 && y >= 1 && y <= 3 ? 1. : 0.,
				 x >= 1 && x <= 3 && y >= 1 && y <= 3 ? 1. : 0.,
				 x >= 1 && x <= 3 && y >= 1 && y <= 3 ? 1. : 0.,
				 1}
			);
	Gradient height;
	height.Keys.push_back({0, {128, 255, 64, 255}});
	Gradient angle;
	angle.Keys.push_back({0, {255, 255, 255, 255}});
	const auto result = imagegraph_test::RunNode(
		"pc.pb_fx_bevel",
		{{"surface", &image}},
		{{"height", int64_t{1}},
		 {"color_over_height", height},
		 {"color_over_angle", angle},
		 {"highlight", true},
		 {"highlight_all", true},
		 {"highlight_color", Colour{255, 0, 0, 255}}}
	);
	INFO(result.Port << ":" << result.Message);
	REQUIRE(result.Ok);
	REQUIRE(detail::ReadPixel(result.Output(), 2, 2) == detail::Rgba{1, 0, 0, 1});
	REQUIRE(detail::ReadPixel(result.Output(), 1, 2)[0] == Catch::Approx(128 / 255.));
	REQUIRE(detail::ReadPixel(result.Output(), 1, 2)[1] == 1);
	REQUIRE(detail::ReadPixel(result.Output("inner_area"), 2, 2) == detail::Rgba{1, 1, 1, 1});
	REQUIRE(detail::ReadPixel(result.Output("inner_area"), 1, 2) == detail::Rgba{});
}
TEST_CASE(
	"Source mapped integer bevel rounds endpoints and samples map RGB without alpha", "[source_bevel]"
) {
	auto doc = BevelGraph();
	doc.Nodes.push_back(
		{"map",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 0}}}}
	);
	doc.Nodes[1].Values.push_back({"height_mapped", true});
	doc.Nodes[1].Values.push_back({"height_map_range", Vector2{.5, 2.5}});
	doc.Links.push_back({"map", "image", "bevel", "height_map"});
	const auto image = BevelEvaluate(doc);
	REQUIRE(detail::ReadPixel(image, 0, 0)[0] == Catch::Approx(.5));
	REQUIRE(detail::ReadPixel(image, 1, 1)[0] == 1);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(BevelEvaluate(restored).Pixels == image.Pixels);
}
TEST_CASE(
	"Source mapped bevel selects nested rows without rounding the source depth-two input", "[source_bevel]"
) {
	auto doc = BevelGraph();
	doc.Nodes.push_back(
		{"map",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 0}}}}
	);
	doc.Nodes[1].Values.push_back({"height_mapped", true});
	doc.Nodes[1].Values.push_back({"height_map_range", Vector2{0, 1000}});
	ArrayValue ranges;
	ranges.ElementType = ValueType::Scalar;
	ranges.Nested = {{.5, 2.5}, {.5, 4.5}};
	doc.Junctions.push_back({"ranges", "", ValueType::Array, ranges});
	doc.Links.push_back({"ranges", "value", "bevel", "height"});
	doc.Links.push_back({"map", "image", "bevel", "height_map"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(doc, plan, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	ImageArray images;
	const auto evaluated = EvaluateArray(doc, plan, "image", EvaluationRequest{}, images, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	REQUIRE(detail::ReadPixel(images.Images[0], 0, 0)[0] == Catch::Approx(.4).margin(.00001));
	REQUIRE(detail::ReadPixel(images.Images[0], 1, 1)[0] == Catch::Approx(.8).margin(.00001));
	REQUIRE(detail::ReadPixel(images.Images[1], 0, 0)[0] == Catch::Approx(1 / 4.5).margin(.00001));
	Document restored;
	REQUIRE(Read(Write(doc), restored, diagnostic) == Status::Ok);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	ImageArray replay;
	REQUIRE(EvaluateArray(restored, plan, "image", EvaluationRequest{}, replay, diagnostic) == Status::Ok);
	REQUIRE(replay.Images.size() == images.Images.size());
	for (size_t i = 0; i < images.Images.size(); i++)
		REQUIRE(replay.Images[i].Pixels == images.Images[i].Pixels);
}
TEST_CASE(
	"Source bevel admits work across all selected processor rows before caller publication", "[source_bevel]"
) {
	auto doc = BevelGraph();
	doc.Junctions.push_back(
		{"heights", "", ValueType::Array, ArrayValue{ValueType::Integer, {int64_t{14000}, int64_t{14000}}}}
	);
	doc.Links.push_back({"heights", "value", "bevel", "height"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	ImageArray output;
	output.Images = {{1, 1, {1, 2, 3, 4}}};
	const auto status = EvaluateArray(doc, plan, "image", EvaluationRequest{}, output, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	REQUIRE(diagnostic.Port == "height");
	REQUIRE(output.Images.size() == 1);
	REQUIRE(output.Images[0].Pixels == std::vector<uint8_t>{1, 2, 3, 4});
}
