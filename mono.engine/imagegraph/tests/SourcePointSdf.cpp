#include "../src/ProcessorBatch.hpp"
#include "../src/nodes/Families.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.source_point_sdf")
using namespace engine::imagegraph;
namespace {
	ArrayValue Points() {
		return {ValueType::Vector2, {Vector2{1.5, 1.5}}};
	}
	SurfacePixel Pixel(const Image &image, uint32_t x, uint32_t y) {
		SurfacePixel p;
		REQUIRE(LoadSurfacePixel(image, x, y, p));
		return p;
	}
	imagegraph_test::NodeRun
	Run(Value points = Points(), double distance = 2, bool inverted = false, int64_t depth = 5) {
		return imagegraph_test::RunNode(
			"pc.point_sdf",
			{},
			{{"dimension", Vector2{3, 3}},
			 {"dimension_unit", EnumValue{0}},
			 {"points", std::move(points)},
			 {"max_distance", distance},
			 {"inverted", inverted},
			 {"attribute_color_depth", depth}}
		);
	}
}
TEST_CASE(
	"Point SDF uses pixel centres and minimum Euclidean float distance", "[imagegraph][source-point-sdf]"
) {
	auto result = Run();
	REQUIRE(result.Ok);
	for (uint32_t y = 0; y < 3; ++y)
		for (uint32_t x = 0; x < 3; ++x) {
			float dx = (float(x) + .5f) / 3.f * 3.f - 1.5f, dy = (float(y) + .5f) / 3.f * 3.f - 1.5f;
			auto p = Pixel(result.Output(), x, y);
			CHECK(p[0] == Catch::Approx(std::sqrt(dx * dx + dy * dy) / 2.f));
			CHECK(p[1] == p[0]);
			CHECK(p[2] == p[0]);
			CHECK(p[3] == 1);
		}
	auto inverted = Run(Points(), -2, true);
	REQUIRE(inverted.Ok);
	CHECK(Pixel(inverted.Output(), 0, 0)[0] > 1);
	ArrayValue two{ValueType::Vector2, {Vector2{1.5, 1.5}, Vector2{.5, .5}}};
	auto nearest = Run(two);
	REQUIRE(nearest.Ok);
	CHECK(Pixel(nearest.Output(), 0, 0)[0] == 0);
	auto capped = Run(ArrayValue{ValueType::Vector2, {Vector2{20000, 20000}}}, 1);
	REQUIRE(capped.Ok);
	CHECK(Pixel(capped.Output(), 0, 0)[0] == 9999);
}
TEST_CASE("Point SDF stores all source surface depths", "[imagegraph][source-point-sdf]") {
	const std::array formats{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (size_t i = 0; i < formats.size(); ++i) {
		auto run = Run(Points(), 2, false, int64_t(i + 2));
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == formats[i]);
		CHECK(Pixel(run.Output(), 1, 1)[0] == 0);
	}
	auto input = Run(Points(), 2, false, 0);
	REQUIRE(input.Ok);
	CHECK(input.Output().Format == SurfaceFormat::RGBA8Unorm);
}
TEST_CASE(
	"Point SDF source rows pad missing coordinates and ignore extras", "[imagegraph][source-point-sdf]"
) {
	ArrayValue rows;
	rows.ElementType = ValueType::Scalar;
	rows.Nested = {{.5, .5, 100.}, {1.5}};
	auto run = Run(rows);
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output(), 0, 0)[0] == 0);
	ArrayValue items;
	items.ElementType = ValueType::Any;
	items.Items = {SourceArrayItem{
		std::vector<SourceArrayItem>{SourceArrayItem{ElementValue{.5}}, SourceArrayItem{ElementValue{.5}}}
	}};
	auto general = Run(items);
	REQUIRE(general.Ok);
	CHECK(Pixel(general.Output(), 0, 0)[0] == 0);
}
TEST_CASE(
	"Point SDF preflight refuses unsupported and nonfinite inputs before output allocation",
	"[imagegraph][source-point-sdf]"
) {
	for (Value points : std::vector<Value>{
			 ArrayValue{ValueType::Scalar, {1., 2.}},
			 ArrayValue{ValueType::Vector2, {Vector2{std::numeric_limits<double>::infinity(), 0}}},
			 ArrayValue{ValueType::Vector2, {Vector2{1e30, 0}}}
		 }) {
		auto run = Run(points);
		CHECK_FALSE(run.Ok);
		CHECK(run.Images.empty());
	}
	ArrayValue large;
	large.ElementType = ValueType::Vector2;
	large.Elements.resize(257, Vector2{});
	auto run = Run(large);
	CHECK(run.Code == Status::LimitExceeded);
	CHECK(run.Images.empty());
	auto zero = Run(Points(), 0);
	CHECK(zero.Code == Status::UnsupportedExecution);
	CHECK(zero.Images.empty());
	auto empty = Run(ArrayValue{ValueType::Vector2, {}});
	CHECK(empty.Code == Status::UnsupportedExecution);
	CHECK(empty.Images.empty());
}
TEST_CASE(
	"Point SDF nearest mapped distance uses raw RGB mean and ignores alpha", "[imagegraph][source-point-sdf]"
) {
	Image map{2, 1, std::vector<uint8_t>(32), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(map, 0, 0, {.25, .5, .75, 0}));
	REQUIRE(StoreSurfacePixel(map, 1, 0, {1, 1, 1, 0}));
	auto run = imagegraph_test::RunNode(
		"pc.point_sdf",
		{{"max_distance_map", &map}},
		{{"dimension", Vector2{3, 3}},
		 {"dimension_unit", EnumValue{0}},
		 {"points", Points()},
		 {"max_distance_mapped", true},
		 {"max_distance_map_range", Vector2{2, 4}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(run.Ok);
	CHECK(Pixel(run.Output(), 0, 1)[0] == Catch::Approx(1.f / 3.f));
	CHECK(Pixel(run.Output(), 2, 1)[0] == Catch::Approx(.25f));
	// An explicitly authored scalar still duplicates itself, overriding the synthetic mapped range.
	auto scalar = imagegraph_test::RunNode(
		"pc.point_sdf",
		{{"max_distance_map", &map}},
		{{"dimension", Vector2{3, 3}},
		 {"dimension_unit", EnumValue{0}},
		 {"points", Points()},
		 {"max_distance_mapped", true},
		 {"max_distance", 2.},
		 {"max_distance_map_range", Vector2{2, 4}},
		 {"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(scalar.Ok);
	CHECK(Pixel(scalar.Output(), 2, 1)[0] == Catch::Approx(.5f));
	REQUIRE(StoreSurfacePixel(map, 1, 0, {0, 0, 0, 1}));
	auto invalid = imagegraph_test::RunNode(
		"pc.point_sdf",
		{{"max_distance_map", &map}},
		{{"dimension", Vector2{3, 3}},
		 {"dimension_unit", EnumValue{0}},
		 {"points", Points()},
		 {"max_distance_mapped", true},
		 {"max_distance_map_range", Vector2{0, 4}}}
	);
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Images.empty());
}
TEST_CASE("Point SDF empty points clone captured matching output only", "[imagegraph][source-point-sdf]") {
	auto initial = Run();
	REQUIRE(initial.Ok);
	GroupRenderSession session;
	session.Outputs.Nodes.push_back(
		{"node",
		 "pc.point_sdf",
		 "",
		 true,
		 {{"surface_out", Value{SurfaceValue{initial.Output()}}, std::nullopt, std::nullopt, false}}}
	);
	Node node{"node", "pc.point_sdf", "", {}, {}};
	EvaluationRequest request;
	request.GroupRender = &session;
	auto entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"dimension", Vector2{3, 3}},
		{"dimension_unit", EnumValue{0}},
		{"points", ArrayValue{ValueType::Vector2, {}}},
		{"attribute_color_depth", EnumValue{5}},
		{"max_distance", 0.}
	};
	REQUIRE(detail::SourcePointSdf(context));
	REQUIRE(context.OutputImages.size() == 1);
	CHECK(context.OutputImages[0].second.Pixels == initial.Output().Pixels);
	detail::NodeContext resized(node, *entry, request);
	resized.ByteBudget = Limits::MaximumEvaluationBytes;
	resized.Values = context.Values;
	resized.Values[0].second = Vector2{4, 3};
	CHECK_FALSE(detail::SourcePointSdf(resized));
	CHECK(resized.FailureCode == Status::UnsupportedExecution);
	CHECK(resized.OutputImages.empty());
}
TEST_CASE(
	"Point SDF processor modes preserve point lists as one input and stage all rows",
	"[imagegraph][source-point-sdf]"
) {
	for (int64_t mode = 0; mode < 4; ++mode) {
		Document d;
		d.FormatVersion = 11;
		d.Nodes = {
			{"sdf",
			 "pc.point_sdf",
			 "",
			 {},
			 {{"dimension", Vector2{3, 3}},
			  {"dimension_unit", EnumValue{0}},
			  {"points", Points()},
			  {"max_distance", ArrayValue{ValueType::Scalar, {2., 4.}}},
			  {"inverted", ArrayValue{ValueType::Boolean, {false, true}}},
			  {"attribute_color_depth", EnumValue{5}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		d.Outputs = {{"out", "sdf", "surface_out"}};
		Plan plan;
		Diagnostic diag;
		const auto compileStatus = Compile(d, plan, diag);
		INFO(diag.Message << " port=" << diag.Port << " mode=" << mode);
		REQUIRE(compileStatus == Status::Ok);
		ImageArray images;
		auto status = EvaluateArray(d, plan, "out", {}, images, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(images.Images.size() == (mode < 2 ? 2 : 4));
		for (size_t row = 0; row < images.Images.size(); ++row) {
			// Source inverse mirrors the suffix stride of all five physical slots, including scalar
			// dimension, points and map slots. The inverted slot therefore uses stride four.
			size_t distanceIndex = mode < 2 ? row : row / 2;
			size_t invertedIndex = mode < 2 ? row : mode == 2 ? row % 2 : row / 4;
			float expected = 1.f / (distanceIndex ? 4.f : 2.f);
			if (invertedIndex) expected = 1.f - expected;
			CHECK(Pixel(images.Images[row], 0, 1)[0] == Catch::Approx(expected));
		}
		d.Nodes[0].Values[3].Data = ArrayValue{ValueType::Scalar, {2., 0.}};
		REQUIRE(Compile(d, plan, diag) == Status::Ok);
		const auto before = images;
		CHECK(EvaluateArray(d, plan, "out", {}, images, diag) == Status::UnsupportedExecution);
		CHECK(images.Images == before.Images);
	}
}
TEST_CASE(
	"Point SDF aggregate admission catches final row before any observer", "[imagegraph][source-point-sdf]"
) {
	Node node{"node", "pc.point_sdf", "", {}, {}};
	EvaluationRequest request;
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	c.Values = {
		{"dimension", Vector2{3, 3}},
		{"dimension_unit", EnumValue{0}},
		{"points", Points()},
		{"max_distance", ArrayValue{ValueType::Scalar, {2., 0.}}},
		{"inverted", false},
		{"attribute_color_depth", EnumValue{5}},
		{"attribute_array_process", EnumValue{0}}
	};
	size_t observed = 0;
	auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	CHECK_FALSE(detail::RunProcessorBatch(c, detail::SourcePointSdf, observer, &observed));
	CHECK(observed == 0);
	CHECK(c.OutputImages.empty());
	detail::NodeContext limited(node, *entry, request);
	limited.Values = c.Values;
	limited.Values[3].second = ArrayValue{ValueType::Scalar, {2., 4.}};
	limited.ByteBudget = 200;
	CHECK_FALSE(detail::RunProcessorBatch(limited, detail::SourcePointSdf, observer, &observed));
	CHECK(limited.FailureCode == Status::LimitExceeded);
	CHECK(observed == 0);
}
TEST_CASE("Point SDF boolean getter uses source numeric half threshold", "[imagegraph][source-point-sdf]") {
	for (double flag : std::array{-1., 0., .5, .50001, 5.}) {
		auto run = imagegraph_test::RunNode(
			"pc.point_sdf",
			{},
			{{"dimension", Vector2{3, 3}},
			 {"dimension_unit", EnumValue{0}},
			 {"points", Points()},
			 {"max_distance", 4.},
			 {"inverted", flag},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		REQUIRE(run.Ok);
		CHECK(Pixel(run.Output(), 0, 1)[0] == Catch::Approx(flag > .5 ? .75 : .25));
	}
}
TEST_CASE(
	"Point SDF later half storage overflow refuses before any row publishes", "[imagegraph][source-point-sdf]"
) {
	Node node{"node", "pc.point_sdf", "", {}, {}};
	EvaluationRequest request;
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	c.Values = {
		{"dimension", Vector2{3, 3}},
		{"dimension_unit", EnumValue{0}},
		{"points", Points()},
		{"max_distance", ArrayValue{ValueType::Scalar, {2., 1e-8}}},
		{"inverted", false},
		{"attribute_color_depth", int64_t{4}},
		{"attribute_array_process", EnumValue{0}}
	};
	size_t observed = 0;
	auto observer = [](detail::NodeContext &, void *state) {
		++*static_cast<size_t *>(state);
		return true;
	};
	CHECK_FALSE(detail::RunProcessorBatch(c, detail::SourcePointSdf, observer, &observed));
	CHECK(c.FailureCode == Status::InvalidValue);
	CHECK(observed == 0);
	CHECK(c.OutputImages.empty());
}
TEST_CASE(
	"Point SDF dimension units retain raw shader dimensions and inherited format",
	"[imagegraph][source-point-sdf]"
) {
	Node node{"node", "pc.point_sdf", "", {}, {}};
	EvaluationRequest request;
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	detail::NodeContext c(node, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	c.Project.SurfaceWidth = 4;
	c.Project.SurfaceHeight = 6;
	c.InheritedSurfaceFormat = SurfaceFormat::RGBA32Float;
	c.Values = {
		{"dimension", Vector2{.625, .5}},
		{"dimension_unit", int64_t{1}},
		{"points", ArrayValue{ValueType::Vector2, {Vector2{0, 0}}}},
		{"max_distance", 2.},
		{"attribute_color_depth", int64_t{1}}
	};
	REQUIRE(detail::SourcePointSdf(c));
	auto &out = c.OutputImages[0].second;
	CHECK(out.Width == 2);
	CHECK(out.Height == 3);
	CHECK(out.Format == SurfaceFormat::RGBA32Float);
	float dx = .5f / 2.f * 2.5f, dy = .5f / 3.f * 3.f;
	CHECK(Pixel(out, 0, 0)[0] == Catch::Approx(std::sqrt(dx * dx + dy * dy) / 2.f));
	detail::NodeContext mask(node, *entry, request);
	mask.ByteBudget = Limits::MaximumEvaluationBytes;
	mask.Values = c.Values;
	mask.Values[1].second = int64_t{2};
	CHECK_FALSE(detail::SourcePointSdf(mask));
	CHECK(mask.FailureCode == Status::UnsupportedExecution);
	CHECK(mask.OutputImages.empty());
	EvaluationRequest bounded;
	bounded.MaximumImageDimension = 1;
	detail::NodeContext limit(node, *entry, bounded);
	limit.ByteBudget = Limits::MaximumEvaluationBytes;
	limit.Values = {
		{"dimension", Vector2{3, 3}},
		{"dimension_unit", EnumValue{0}},
		{"points", Points()},
		{"attribute_color_depth", EnumValue{5}}
	};
	CHECK_FALSE(detail::SourcePointSdf(limit));
	CHECK(limit.FailureCode == Status::LimitExceeded);
	CHECK(limit.OutputImages.empty());
}
TEST_CASE(
	"Point SDF linked mapped toggle uses source condition numeric threshold", "[imagegraph][source-point-sdf]"
) {
	Image map{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(map, 0, 0, {1, 1, 1, 0}));
	for (double toggle : std::array{-1., .5, .50001}) {
		Document d;
		d.FormatVersion = 11;
		d.Nodes = {
			{"toggle", "pc.number", "", {}, {{"value", toggle}}},
			{"map", "image.captured", "", {}, {{"source_id", std::string{"map"}}}},
			{"distance", "pc.vector2", "", {}, {{"x", 2.}, {"y", 4.}}},
			{"sdf",
			 "pc.point_sdf",
			 "",
			 {},
			 {{"dimension", Vector2{3, 3}},
			  {"dimension_unit", EnumValue{0}},
			  {"points", Points()},
			  {"attribute_color_depth", EnumValue{5}}}}
		};
		d.Links = {
			{"toggle", "number", "sdf", "max_distance_mapped"},
			{"map", "image", "sdf", "max_distance_map"},
			{"distance", "vector", "sdf", "max_distance"}
		};
		d.Outputs = {{"out", "sdf", "surface_out"}};
		Plan plan;
		Diagnostic diag;
		const auto compiled = Compile(d, plan, diag);
		INFO(diag.Message << " port=" << diag.Port);
		REQUIRE(compiled == Status::Ok);
		std::array sources{RequestImageSource{"map", map}};
		EvaluationRequest request;
		request.ImageSources = sources;
		Image output;
		const auto evaluated = Evaluate(d, plan, "out", request, output, diag);
		INFO(diag.Message << " port=" << diag.Port);
		REQUIRE(evaluated == Status::Ok);
		CHECK(Pixel(output, 0, 1)[0] == Catch::Approx(toggle > .5 ? .25 : .5));
	}
}
