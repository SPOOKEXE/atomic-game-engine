#include "../src/nodes/Families.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_points_nodes")
using namespace engine::imagegraph;
namespace engine::imagegraph::detail {
	std::span<const ExecutorEntry> SourcePointsExecutors();
}
TEST_CASE(
	"Point UV mapping uses rounded clamped source pixels and source amount interpolation",
	"[imagegraph][source_points]"
) {
	Image image{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}, 0};
	ArrayValue input{ValueType::Vector2, {Vector2{.5, 0}, Vector2{1.5, 0}, Vector2{-1, 0}}};
	Node node{"points", "pc.points_remap", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"uv_map", &image}};
	context.Values = {{"points", input}, {"amount", .5}};
	REQUIRE(engine::imagegraph::detail::SourcePointsExecutors()[0].Run(context));
	REQUIRE(context.FailureCode == Status::Ok);
	REQUIRE(context.OutputValues.size() == 1);
	const auto &array = std::get<ArrayValue>(context.OutputValues[0].Data);
	REQUIRE(array.Elements.size() == 3);
	CHECK(std::get<Vector2>(array.Elements[0]).X == Catch::Approx(1.25));
	CHECK(std::get<Vector2>(array.Elements[0]).Y == 0);
	CHECK(std::get<Vector2>(array.Elements[1]).X == Catch::Approx(.75));
	CHECK(std::get<Vector2>(array.Elements[1]).Y == Catch::Approx(.5));
	CHECK(std::get<Vector2>(array.Elements[2]).X == Catch::Approx(.5));
}
TEST_CASE(
	"Point UV mapping reads floating RG channels without normalized clamping", "[imagegraph][source_points]"
) {
	Image image;
	image.Width = 1;
	image.Height = 1;
	image.Format = SurfaceFormat::RGBA32Float;
	image.Pixels.resize(16);
	REQUIRE(StoreSurfacePixel(image, 0, 0, {2, -1, 0, 1}));
	ArrayValue input;
	input.ElementType = ValueType::Scalar;
	input.Nested = {{0.0, 0.0}};
	Node node{"points", "pc.points_remap", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"uv_map", &image}};
	context.Values = {{"points", input}};
	REQUIRE(engine::imagegraph::detail::SourcePointsExecutors()[0].Run(context));
	const auto &array = std::get<ArrayValue>(context.OutputValues[0].Data);
	CHECK(std::get<Vector2>(array.Elements[0]) == Vector2{2, -1});
}
TEST_CASE(
	"Single channel UV surfaces use source packed color getters and malformed points refuse",
	"[imagegraph][source_points]"
) {
	Image image{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::R32Float};
	REQUIRE(StoreSurfacePixel(image, 0, 0, {513, 0, 0, 1}));
	Node node{"points", "pc.points_remap", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images = {{"uv_map", &image}};
	context.Values = {{"points", Vector2{0, 0}}};
	REQUIRE(engine::imagegraph::detail::SourcePointsExecutors()[0].Run(context));
	const auto &array = std::get<ArrayValue>(context.OutputValues[0].Data);
	CHECK(std::get<Vector2>(array.Elements[0]).X == Catch::Approx(1.0 / 255));
	CHECK(std::get<Vector2>(array.Elements[0]).Y == Catch::Approx(2.0 / 255));
	ArrayValue invalid{ValueType::Scalar, {1.0}};
	context.ClearOutputs();
	context.Values = {{"points", invalid}};
	CHECK_FALSE(engine::imagegraph::detail::SourcePointsExecutors()[0].Run(context));
	CHECK(context.FailureCode == Status::TypeMismatch);
}

TEST_CASE(
	"Point UV mapping compiles and evaluates authored source coordinate arrays", "[imagegraph][source_points]"
) {
	RequestImageSource capture{"uv", Image{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}, 0}};
	ArrayValue points;
	SECTION("vector row carrier") {
		points = {ValueType::Vector2, {Vector2{.5, 0}, Vector2{1.5, 0}, Vector2{-1, 0}}};
	}
	SECTION("numeric nested row carrier") {
		points.ElementType = ValueType::Scalar;
		points.Nested = {{.5, 0.0}, {1.5, 0.0}, {-1.0, 0.0}};
	}
	SECTION("general source row carrier") {
		points.ElementType = ValueType::Any;
		points.Items = {
			{std::vector<SourceArrayItem>{{ElementValue{.5}}, {ElementValue{0.0}}}},
			{ElementValue{Vector2{1.5, 0}}},
			{std::vector<SourceArrayItem>{{ElementValue{-1.0}}, {ElementValue{int64_t{0}}}}}
		};
	}
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"capture", "image.captured", "", {}, {{"source_id", std::string{"uv"}}}},
		{"remap", "pc.points_remap", "", {}, {{"points", points}, {"amount", .5}}}
	};
	document.Links = {{"capture", "image", "remap", "uv_map"}};
	document.Outputs = {{"mapped", "remap", "points"}};
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Plan plan;
	const auto compiled = Compile(restored, plan, diagnostic);
	INFO(diagnostic.Message << " port=" << diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request;
	request.ImageSources = std::span<const RequestImageSource>{&capture, 1};
	EvaluatedValue result;
	const auto evaluated = EvaluateValue(restored, plan, "mapped", request, result, diagnostic);
	INFO(diagnostic.Message << " port=" << diagnostic.Port);
	REQUIRE(evaluated == Status::Ok);
	const auto &mapped = std::get<ArrayValue>(result.Data);
	REQUIRE(mapped.Elements.size() == 3);
	CHECK(std::get<Vector2>(mapped.Elements[0]) == Vector2{1.25, 0});
	CHECK(std::get<Vector2>(mapped.Elements[1]) == Vector2{.75, .5});
	CHECK(std::get<Vector2>(mapped.Elements[2]) == Vector2{.5, 0});
	CHECK(capture.Data.Pixels == std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255});
}

TEST_CASE("Fresh point remapping evaluates the source origin-row default", "[imagegraph][source_points]") {
	const auto *entry = FindCatalogueEntry("pc.points_remap");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "points");
	REQUIRE(input);
	const auto value = CatalogueDefault(*input);
	REQUIRE(value);
	const auto &points = std::get<ArrayValue>(*value);
	REQUIRE(points.Elements.size() == 1);
	CHECK(std::get<Vector2>(points.Elements.front()) == Vector2{});
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"capture", "image.captured", "", {}, {{"source_id", std::string{"uv"}}}},
		{"remap", "pc.points_remap", "", {}, {}}
	};
	document.Links = {{"capture", "image", "remap", "uv_map"}};
	document.Outputs = {{"mapped", "remap", "points"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	RequestImageSource capture{"uv", Image{1, 1, {255, 0, 0, 255}, 0}};
	EvaluationRequest request;
	request.ImageSources = std::span<const RequestImageSource>{&capture, 1};
	EvaluatedValue result;
	const auto status = EvaluateValue(document, plan, "mapped", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &mapped = std::get<ArrayValue>(result.Data);
	REQUIRE(mapped.Elements.size() == 1);
	CHECK(std::get<Vector2>(mapped.Elements.front()) == Vector2{1, 0});
}
